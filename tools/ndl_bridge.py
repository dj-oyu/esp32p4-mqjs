#!/usr/bin/env python3
"""NDL search bridge for the reading.js app (stdlib only, no paho).

The Tab5 runtime has no HTTP client, so book-metadata lookup is delegated
to the PC, local-first style: the device publishes a bare ISBN to
<ndl>/req, this bridge queries the NDL Search OpenSearch API and publishes
{"isbn", "ok", "title", "author", "pages"} to <ndl>/res.

    python3 tools/ndl_bridge.py [HOST] [PORT]     # run the bridge
    python3 tools/ndl_bridge.py --lookup ISBN     # one-shot API test, no broker

Defaults: broker 192.168.1.2:1883, topics esp32p4-mqjs/ndl/{req,res}.
"""
import json
import re
import select
import socket
import sys
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

REQ_TOPIC = "esp32p4-mqjs/ndl/req"
RES_TOPIC = "esp32p4-mqjs/ndl/res"
API = "https://ndlsearch.ndl.go.jp/api/opensearch?isbn="
DC = "{http://purl.org/dc/elements/1.1/}"


def ndl_lookup(isbn: str) -> dict:
    isbn = re.sub(r"[^0-9Xx]", "", isbn)
    out = {"isbn": isbn, "ok": False}
    if len(isbn) not in (10, 13):
        out["err"] = "bad isbn"
        return out
    req = urllib.request.Request(
        API + urllib.parse.quote(isbn),
        headers={"User-Agent": "esp32p4-mqjs-ndl-bridge/1.0"})
    try:
        data = urllib.request.urlopen(req, timeout=15).read()
        root = ET.fromstring(data)
    except Exception as e:
        out["err"] = f"api: {e}"
        return out
    best = None
    for item in root.iter("item"):
        title = (item.findtext("title") or "").strip()
        if not title:
            continue
        author = (item.findtext(DC + "creator") or "").strip()
        # NDL authority form "夏目, 漱石, 1867-1916" → "夏目漱石";
        # plain form "夏目漱石 著" → "夏目漱石"
        author = re.sub(r",?\s*\d{4}-(\d{4})?$", "", author)
        author = re.sub(r"\s*(著|作|訳|編著|編|監修)$", "", author)
        author = re.sub(r"([^\x00-\x7f]),\s*([^\x00-\x7f])", r"\1\2", author)
        pages = 0
        for ext in item.findall(DC + "extent"):
            m = re.search(r"(\d+)\s*[pｐ頁]", ext.text or "")
            if m:
                pages = int(m.group(1))
                break
        cand = {"isbn": isbn, "ok": True, "title": title,
                "author": author, "pages": pages}
        # prefer the record that knows the page count
        if best is None or (pages and not best["pages"]):
            best = cand
        if best["pages"]:
            break
    if best is None:
        out["err"] = "not found"
        return out
    return best


# ---- minimal MQTT 3.1.1 client (QoS0), same wire approach as mqjs_push.py


def vlq(n: int) -> bytes:
    out = b""
    while True:
        b = n % 128
        n //= 128
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out


class Mqtt:
    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.setblocking(False)
        self.buf = b""
        self.last_io = time.time()
        cid = b"ndl-bridge"
        var = (b"\x00\x04MQTT\x04\x02\x00\x3c" +
               len(cid).to_bytes(2, "big") + cid)
        self.sock.sendall(b"\x10" + vlq(len(var)) + var)
        typ, body = self._packet(timeout=10)
        if typ != 0x20 or body[1] != 0:
            raise ConnectionError(f"CONNACK failed: {body.hex()}")

    def _recv(self, n: int, timeout: float) -> bytes:
        end = time.time() + timeout
        while len(self.buf) < n:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError("mqtt read timeout")
            r, _, _ = select.select([self.sock], [], [], left)
            if not r:
                continue
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("broker closed connection")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _packet(self, timeout: float):
        head = self._recv(1, timeout)[0]
        length = 0
        for shift in range(0, 28, 7):
            b = self._recv(1, 5)[0]
            length |= (b & 0x7F) << shift
            if not (b & 0x80):
                break
        return head & 0xF0, self._recv(length, 5) if length else b""

    def subscribe(self, topic: str) -> None:
        t = topic.encode()
        body = b"\x00\x01" + len(t).to_bytes(2, "big") + t + b"\x00"
        self.sock.sendall(b"\x82" + vlq(len(body)) + body)
        typ, _ = self._packet(timeout=10)
        if typ != 0x90:
            raise ConnectionError("SUBACK missing")

    def publish(self, topic: str, payload: bytes) -> None:
        t = topic.encode()
        body = len(t).to_bytes(2, "big") + t + payload
        self.sock.sendall(b"\x30" + vlq(len(body)) + body)
        self.last_io = time.time()

    def poll(self, timeout: float):
        """Yield (topic, payload) for QoS0 PUBLISHes; ping on idle."""
        if time.time() - self.last_io > 30:
            self.sock.sendall(b"\xc0\x00")  # PINGREQ
            self.last_io = time.time()
        r, _, _ = select.select([self.sock], [], [], timeout)
        if not r and not self.buf:
            return None
        typ, body = self._packet(timeout=5)
        if typ != 0x30:  # PINGRESP etc.
            return None
        tlen = int.from_bytes(body[:2], "big")
        return body[2:2 + tlen].decode(), body[2 + tlen:]


def main() -> None:
    if len(sys.argv) >= 3 and sys.argv[1] == "--lookup":
        print(json.dumps(ndl_lookup(sys.argv[2]), ensure_ascii=False))
        return
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.2"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
    while True:
        try:
            m = Mqtt(host, port)
            m.subscribe(REQ_TOPIC)
            print(f"ndl_bridge: connected to {host}:{port}, "
                  f"waiting on {REQ_TOPIC}")
            while True:
                msg = m.poll(1.0)
                if msg is None:
                    continue
                isbn = msg[1].decode(errors="replace").strip()
                print(f"req: {isbn}")
                res = ndl_lookup(isbn)
                print(f"res: {json.dumps(res, ensure_ascii=False)}")
                m.publish(RES_TOPIC,
                          json.dumps(res, ensure_ascii=False).encode())
        except KeyboardInterrupt:
            return
        except Exception as e:
            print(f"ndl_bridge: {e} - reconnecting in 5s")
            time.sleep(5)


if __name__ == "__main__":
    main()
