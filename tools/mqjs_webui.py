#!/usr/bin/env python3
"""Local web UI for pushing JS tasks to mqjs devices.

    python3 tools/mqjs_webui.py [--port 8765] [--broker 192.168.1.2] \
                                [--topic esp32p4-mqjs/task/u7q3x9f2]

Open http://localhost:8765 . Binds to 127.0.0.1 only: this process holds
the Ed25519 signing key, do not expose it beyond localhost.

Needs (in WSL): python3-cryptography, gcc (for the PC test runner and
the bytecode compiler, built on demand from components/mqjs).
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY_PATH = os.path.join(ROOT, "tools", "task_signing_key.pem")
EXAMPLES = os.path.join(ROOT, "examples")
MQJS_DIR = os.path.join(ROOT, "components", "mqjs")
TOOL_DIR = "/tmp/mqjs_webui_tools"

ENGINE_SRCS = ["mqjs_runtime.c", "mquickjs/mquickjs.c", "mquickjs/cutils.c",
               "mquickjs/dtoa.c", "mquickjs/libm.c"]

status_log = deque(maxlen=200)
ARGS = None


def log_status(source, text):
    status_log.append({"t": time.strftime("%H:%M:%S"), "src": source, "msg": text})


# ---------------------------------------------------------------- tools

def build_tools():
    """Build run_pc and compile_task once (PC engine with device stdlib)."""
    os.makedirs(TOOL_DIR, exist_ok=True)
    srcs = [os.path.join(MQJS_DIR, s) for s in ENGINE_SRCS]
    for out, main_src in (("run_pc", "tools/run_pc.c"),
                          ("compile_task", "tools/compile_task.c")):
        out_path = os.path.join(TOOL_DIR, out)
        if os.path.exists(out_path):
            continue
        cmd = ["gcc", "-O2", f"-I{MQJS_DIR}", f"-I{MQJS_DIR}/gen_pc",
               f"-I{MQJS_DIR}/mquickjs", "-o", out_path,
               os.path.join(MQJS_DIR, main_src)] + srcs + ["-lm"]
        print(f"[tools] building {out} ...")
        subprocess.run(cmd, check=True)
    return TOOL_DIR


# ---------------------------------------------------------------- mqtt

def vlq(n):
    out = b""
    while True:
        b = n % 128
        n //= 128
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out


def mqtt_connect(host, port, client_id):
    cid = client_id.encode()
    var = b"\x00\x04MQTT\x04\x02\x00\x3c" + len(cid).to_bytes(2, "big") + cid
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(b"\x10" + vlq(len(var)) + var)
    ack = s.recv(4)
    if len(ack) < 4 or ack[0] != 0x20 or ack[3] != 0:
        s.close()
        raise RuntimeError(f"CONNACK failed: {ack.hex()}")
    return s


def mqtt_publish(host, port, topic, payload):
    s = mqtt_connect(host, port, "mqjs-webui-pub")
    body = len(topic).to_bytes(2, "big") + topic.encode() + payload
    s.sendall(b"\x30" + vlq(len(body)) + body)
    time.sleep(0.3)
    s.sendall(b"\xe0\x00")
    s.close()


def read_packet(s):
    """Read one MQTT packet (type byte + body) or None on timeout."""
    try:
        h = s.recv(1)
    except socket.timeout:
        return None, None
    if not h:
        raise ConnectionError("closed")
    mult, length = 1, 0
    while True:
        b = s.recv(1)
        if not b:
            raise ConnectionError("closed")
        length += (b[0] & 0x7F) * mult
        if not (b[0] & 0x80):
            break
        mult *= 128
    body = b""
    while len(body) < length:
        chunk = s.recv(length - len(body))
        if not chunk:
            raise ConnectionError("closed")
        body += chunk
    return h[0], body


def status_listener(host, port, topic):
    """Subscribe to <topic>/status and feed the status log. Reconnects."""
    sub_topic = (topic + "/status").encode()
    while True:
        try:
            s = mqtt_connect(host, port, "mqjs-webui-sub")
            body = b"\x00\x01" + len(sub_topic).to_bytes(2, "big") + sub_topic + b"\x00"
            s.sendall(b"\x82" + vlq(len(body)) + body)
            s.settimeout(30)
            log_status("ui", f"status listener connected to {host}")
            last_ping = time.time()
            while True:
                ptype, body = read_packet(s)
                if ptype is not None and (ptype & 0xF0) == 0x30 and body:
                    tlen = struct.unpack(">H", body[:2])[0]
                    msg = body[2 + tlen:].decode(errors="replace")
                    log_status("device", msg)
                if time.time() - last_ping > 25:
                    s.sendall(b"\xc0\x00")  # PINGREQ
                    last_ping = time.time()
        except Exception as e:
            log_status("ui", f"status listener reconnecting ({e})")
            time.sleep(5)


# ---------------------------------------------------------------- actions

def sign(payload: bytes) -> bytes:
    with open(KEY_PATH, "rb") as f:
        sk = serialization.load_pem_private_key(f.read(), password=None)
    assert isinstance(sk, Ed25519PrivateKey)
    return sk.sign(payload) + payload


def action_test(source: str):
    """Run the script with the PC engine (stub gpio/i2c/mqtt), 5s cap."""
    build_tools()
    path = os.path.join(TOOL_DIR, "test_input.js")
    with open(path, "w") as f:
        f.write(source)
    r = subprocess.run(["timeout", "5", os.path.join(TOOL_DIR, "run_pc"), path],
                       capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    note = "" if r.returncode != 124 else "\n(5 秒でタイムアウト打ち切り: 常駐スクリプトなら正常)"
    return f"exit={r.returncode}\n{out}{note}"


def action_push(source: str, broker: str, topic: str, as_bytecode: bool):
    payload = source.encode()
    log = []
    if as_bytecode:
        build_tools()
        src = os.path.join(TOOL_DIR, "push_input.js")
        out = os.path.join(TOOL_DIR, "push_output.bin")
        with open(src, "w") as f:
            f.write(source)
        r = subprocess.run([os.path.join(TOOL_DIR, "compile_task"), src, out],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return f"コンパイル失敗:\n{r.stdout}{r.stderr}"
        with open(out, "rb") as f:
            payload = f.read()
        log.append(f"compiled to {len(payload)} bytes of 32-bit bytecode")

    signed = sign(payload)
    host, _, port = broker.partition(":")
    mqtt_publish(host, int(port or 1883), topic, signed)
    log.append(f"published {len(signed)} bytes (sig 64 + payload {len(payload)}) "
               f"to {topic} @ {host}")
    log_status("ui", log[-1])
    return "\n".join(log)


# ---------------------------------------------------------------- http

PAGE = """<!doctype html><html lang=ja><meta charset=utf-8>
<title>mqjs task push</title>
<style>
 body{font-family:system-ui,sans-serif;margin:1.2rem;background:#14161a;color:#dfe3ea}
 h1{font-size:1.1rem} a{color:#7ab7ff}
 textarea{width:100%;height:21rem;background:#0d0f12;color:#cde3c8;border:1px solid #333;
          font-family:Consolas,monospace;font-size:.85rem;padding:.5rem;box-sizing:border-box}
 input[type=text]{background:#0d0f12;color:#dfe3ea;border:1px solid #333;padding:.3rem .4rem}
 button{background:#2563eb;color:#fff;border:0;padding:.45rem 1rem;border-radius:4px;cursor:pointer;margin-right:.5rem}
 button.alt{background:#374151}
 #out,#status{background:#0d0f12;border:1px solid #333;padding:.5rem;white-space:pre-wrap;
      font-family:Consolas,monospace;font-size:.8rem;min-height:2.5rem;max-height:14rem;overflow:auto}
 .row{display:flex;gap:1rem;align-items:center;margin:.6rem 0;flex-wrap:wrap}
 label{font-size:.85rem}
 .cols{display:grid;grid-template-columns:1fr 22rem;gap:1rem}
 @media(max-width:900px){.cols{grid-template-columns:1fr}}
</style>
<h1>mqjs task push <small style="color:#888">(Ed25519 署名つき MQTT 配信)</small></h1>
<div class=cols>
<div>
 <div class=row>
  <label>example: <select id=ex onchange=loadExample()><option value="">--</option></select></label>
 </div>
 <textarea id=src spellcheck=false>print("hello from web ui");</textarea>
 <div class=row>
  <label>broker <input type=text id=broker size=16></label>
  <label>topic <input type=text id=topic size=28></label>
 </div>
 <div class=row>
  <label><input type=checkbox id=bc> バイトコードにコンパイルして送信</label>
 </div>
 <div class=row>
  <button class=alt onclick=test()>PC テスト実行</button>
  <button onclick=push()>署名して Push</button>
 </div>
 <div id=out></div>
</div>
<div>
 <h1>device status <small style="color:#888">(topic/status)</small></h1>
 <div id=status></div>
</div>
</div>
<script>
const $=id=>document.getElementById(id);
fetch('/api/config').then(r=>r.json()).then(c=>{$('broker').value=c.broker;$('topic').value=c.topic;
  for(const n of c.examples){const o=document.createElement('option');o.value=o.textContent=n;$('ex').appendChild(o);}});
async function loadExample(){const n=$('ex').value;if(!n)return;
  $('src').value=await (await fetch('/api/example?name='+encodeURIComponent(n))).text();}
async function call(url,body){$('out').textContent='...';
  const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  $('out').textContent=await r.text();}
function test(){call('/api/test',{source:$('src').value});}
function push(){call('/api/push',{source:$('src').value,broker:$('broker').value,
  topic:$('topic').value,bytecode:$('bc').checked});}
setInterval(async()=>{const s=await (await fetch('/api/status')).json();
  $('status').textContent=s.map(e=>`${e.t} [${e.src}] ${e.msg}`).join('\\n');
  $('status').scrollTop=$('status').scrollHeight;},2000);
</script>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="text/plain; charset=utf-8"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/":
            self._send(200, PAGE, "text/html; charset=utf-8")
        elif self.path == "/api/config":
            examples = sorted(f for f in os.listdir(EXAMPLES) if f.endswith(".js"))
            self._send(200, json.dumps({"broker": ARGS.broker, "topic": ARGS.topic,
                                        "examples": examples}), "application/json")
        elif self.path == "/api/status":
            self._send(200, json.dumps(list(status_log)), "application/json")
        elif self.path.startswith("/api/example?name="):
            name = os.path.basename(self.path.split("=", 1)[1])
            p = os.path.join(EXAMPLES, name)
            if not os.path.isfile(p):
                self._send(404, "not found")
                return
            with open(p, "r", encoding="utf-8") as f:
                self._send(200, f.read())
        else:
            self._send(404, "not found")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try:
            req = json.loads(self.rfile.read(n))
            if self.path == "/api/test":
                self._send(200, action_test(req["source"]))
            elif self.path == "/api/push":
                self._send(200, action_push(req["source"], req["broker"],
                                            req["topic"], bool(req.get("bytecode"))))
            else:
                self._send(404, "not found")
        except Exception as e:
            self._send(500, f"error: {e}")


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--broker", default="192.168.1.2")
    ap.add_argument("--topic", default="esp32p4-mqjs/task/u7q3x9f2")
    ARGS = ap.parse_args()

    if not os.path.exists(KEY_PATH):
        sys.exit(f"signing key not found: {KEY_PATH} (run tools/mqjs_keygen.py)")
    build_tools()

    host, _, port = ARGS.broker.partition(":")
    threading.Thread(target=status_listener,
                     args=(host, int(port or 1883), ARGS.topic), daemon=True).start()

    srv = ThreadingHTTPServer(("127.0.0.1", ARGS.port), Handler)
    print(f"mqjs web ui: http://localhost:{ARGS.port}  "
          f"(broker={ARGS.broker}, topic={ARGS.topic})")
    srv.serve_forever()


if __name__ == "__main__":
    main()
