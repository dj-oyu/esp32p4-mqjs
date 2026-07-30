#!/usr/bin/env python3
"""Pull the LP SRAM black box off the device with a signed MQTT request.

    python3 tools/bb_pull.py HOST BASE_TOPIC [what] [options]

      what          lastboot (default) | live | stats
      --port N      broker port (default 1883)
      --from N      start at record N (resume a pull)
      --ctr N       use this replay counter instead of the wall clock
      --timeout S   seconds to wait for a reply chunk (default 10)
      --json        print the raw reply chunks instead of the log text
      --raw         publish the request UNSIGNED (expect a rejection)
      --tamper      sign, then flip one message byte (expect a rejection)

    python3 tools/bb_pull.py 192.168.1.2 esp32p4-mqjs/task/u7q3x9f2
    python3 tools/bb_pull.py 192.168.1.2 esp32p4-mqjs/task/u7q3x9f2 stats

Wire format (components/term_core/term_bb_pull.h is the contract):

  request  -> <base>/bb        Ed25519 signature(64) || message
              message = "bbpull1\\nctr=<n>\\ntop=<base>\\nwhat=<w>\\nfrom=<n>\\n"
  reply    -> <base>/bb/reply  one JSON object per chunk:
              {"bb":1,"ctr":..,"what":..,"seq":..,"from":..,
               "body":{...term_lp_dump_json...},"last":0|1}
  refusal  -> <base>/status    "bb: rejected (<reason>)"

The signature is the same Ed25519 envelope tools/mqjs_push.py publishes for
app pushes, verified against the same embedded key: one trust root.

`ctr` is wall-clock milliseconds and the device only accepts a value strictly
greater than the last one it accepted (the high-water mark lives in NVS, so a
power cut does not reopen the window). That is the replay defence: a captured
request cannot be re-published later to pull a future boot's log.
"""
import json
import os
import select
import socket
import sys
import time

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY = os.path.join(ROOT, "tools", "task_signing_key.pem")


def vlq(n: int) -> bytes:
    out = b""
    while True:
        b = n % 128
        n //= 128
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out


class Mqtt:
    """Minimal MQTT 3.1.1 QoS0 client (same approach as ndl_bridge.py)."""

    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.setblocking(False)
        self.buf = b""
        cid = f"bb-pull-{os.getpid()}".encode()
        var = (b"\x00\x04MQTT\x04\x02\x00\x3c" +
               len(cid).to_bytes(2, "big") + cid)
        self.sock.sendall(b"\x10" + vlq(len(var)) + var)
        typ, body = self._packet(10)
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
            chunk = self.sock.recv(65536)
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

    def subscribe(self, *topics: str) -> None:
        body = b"\x00\x01"
        for t in topics:
            e = t.encode()
            body += len(e).to_bytes(2, "big") + e + b"\x00"
        self.sock.sendall(b"\x82" + vlq(len(body)) + body)
        typ, _ = self._packet(10)
        if typ != 0x90:
            raise ConnectionError("SUBACK missing")

    def publish(self, topic: str, payload: bytes) -> None:
        t = topic.encode()
        body = len(t).to_bytes(2, "big") + t + payload
        self.sock.sendall(b"\x30" + vlq(len(body)) + body)

    def poll(self, timeout: float):
        """(topic, payload) for the next QoS0 PUBLISH, or None on timeout."""
        if not self.buf:
            r, _, _ = select.select([self.sock], [], [], timeout)
            if not r:
                return None
        typ, body = self._packet(5)
        if typ != 0x30:
            return None
        tlen = int.from_bytes(body[:2], "big")
        return body[2:2 + tlen].decode(), body[2 + tlen:]


def sign(msg: bytes) -> bytes:
    with open(KEY, "rb") as f:
        sk = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(sk, Ed25519PrivateKey):
        raise SystemExit("key is not Ed25519")
    return sk.sign(msg) + msg


def request(base: str, what: str, frm: int, ctr: int) -> bytes:
    return (f"bbpull1\nctr={ctr}\ntop={base}\nwhat={what}\nfrom={frm}\n"
            .encode())


def collect(m: Mqtt, base: str, ctr: int, timeout: float, want_json: bool):
    """Gather reply chunks for `ctr` until "last". Returns (lines, tail)."""
    lines: list[str] = []
    expect = 0
    tail = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        got = m.poll(max(0.1, deadline - time.time()))
        if not got:
            continue
        topic, payload = got
        text = payload.decode("utf-8", "replace")
        if topic == f"{base}/status":
            if text.startswith("bb:"):
                print(f"[status] {text}", file=sys.stderr)
                if "rejected" in text:
                    raise SystemExit(1)
            continue
        if topic != f"{base}/bb/reply":
            continue
        try:
            rep = json.loads(text)
        except ValueError:
            print(f"[warn] unparseable chunk ({len(payload)}B)",
                  file=sys.stderr)
            continue
        if rep.get("ctr") != ctr:
            continue                      # an older pull still in flight
        if want_json:
            print(json.dumps(rep, ensure_ascii=False))
        seq = rep.get("seq", 0)
        if seq != expect:
            print(f"[warn] chunk {expect} missing (got {seq}); "
                  f"QoS0 loss — re-run with --from", file=sys.stderr)
        expect = seq + 1
        body = rep.get("body") or {}
        lines.extend(body.get("lines", []))
        deadline = time.time() + timeout   # progress: extend the window
        if rep.get("last"):
            tail = rep
            break
    return lines, tail


def main() -> None:
    argv = sys.argv[1:]
    if len(argv) < 2:
        raise SystemExit(__doc__)

    def opt(name, default=None, cast=str):
        if name in argv:
            return cast(argv[argv.index(name) + 1])
        return default

    flags = {"--json", "--raw", "--tamper"}
    valued = {"--port", "--from", "--ctr", "--timeout"}
    pos = []
    skip = False
    for i, a in enumerate(argv):
        if skip:
            skip = False
            continue
        if a in valued:
            skip = True
            continue
        if a in flags:
            continue
        if a.startswith("--"):
            raise SystemExit(f"unknown option: {a}")
        pos.append(a)

    host, base = pos[0], pos[1].rstrip("/")
    what = pos[2] if len(pos) > 2 else "lastboot"
    if what not in ("lastboot", "live", "stats"):
        raise SystemExit("what must be lastboot, live or stats")
    port = opt("--port", 1883, int)
    frm = opt("--from", 0, int)
    timeout = opt("--timeout", 10.0, float)
    ctr = opt("--ctr", None, int)
    want_json = "--json" in argv

    m = Mqtt(host, port)
    m.subscribe(f"{base}/bb/reply", f"{base}/status")

    total: list[str] = []
    rounds = 0
    while True:
        rounds += 1
        use = ctr if ctr is not None else time.time_ns() // 1_000_000
        ctr = None                        # only the first round is pinned
        msg = request(base, what, frm, use)
        if "--raw" in argv:
            payload = msg               # unsigned: expect bad-signature
        else:
            payload = sign(msg)
            if "--tamper" in argv:
                b = bytearray(payload)
                b[-1] ^= 0x01           # flip a byte the signature covers
                payload = bytes(b)
        print(f"[req] ctr={use} what={what} from={frm} "
              f"({len(payload)}B{' unsigned' if '--raw' in argv else ''}"
              f"{' tampered' if '--tamper' in argv else ''})",
              file=sys.stderr)
        m.publish(f"{base}/bb", payload)

        lines, tail = collect(m, base, use, timeout, want_json)
        total.extend(lines)
        if tail is None:
            print(f"[warn] no final chunk within {timeout}s "
                  f"({len(lines)} lines so far)", file=sys.stderr)
            break
        body = tail.get("body") or {}
        if tail.get("stall"):
            print("[warn] device reported stall (chunk buffer too small)",
                  file=sys.stderr)
            break
        if tail.get("cut") and rounds < 8:
            frm = body.get("next", frm)
            print(f"[info] device cut the reply at its chunk bound, "
                  f"resuming from {frm}", file=sys.stderr)
            time.sleep(0.2)             # the next ctr must be strictly larger
            continue
        break

    if what == "stats":
        if not want_json and tail:
            print(json.dumps(tail.get("body"), ensure_ascii=False, indent=1))
    elif not want_json:
        for line in total:
            print(line)
    print(f"[done] {len(total)} records", file=sys.stderr)


if __name__ == "__main__":
    main()
