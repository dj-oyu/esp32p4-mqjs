#!/usr/bin/env python3
"""Sign a JS task with the mqjs Ed25519 key and publish it.

    python3 tools/mqjs_push.py HOST TOPIC FILE [PORT]
    python3 tools/mqjs_push.py HOST TOPIC FILE [PORT] --retain
    python3 tools/mqjs_push.py HOST TOPIC --delete [PORT]

Wire format published to TOPIC: signature(64 bytes) || script bytes,
which is exactly what the device's TweetNaCl crypto_sign_open() verifies
against the embedded public key.

P4c app registry: publishing with --retain to <task topic>/apps/<name>
puts the app on the broker's shelf (devices sync it on every connect,
install-only). --delete publishes an empty retained payload = the MQTT
tombstone that clears the shelf entry and uninstalls on devices.

Pass --raw to publish the file unsigned (for testing rejection).
"""
import os
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


def mqtt_publish(host: str, port: int, topic: bytes, payload: bytes,
                 retain: bool = False) -> None:
    cid = b"mqjs-push-py"
    var = b"\x00\x04MQTT\x04\x02\x00\x3c" + len(cid).to_bytes(2, "big") + cid
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(b"\x10" + vlq(len(var)) + var)
    ack = s.recv(4)
    if len(ack) < 4 or ack[0] != 0x20 or ack[3] != 0:
        raise SystemExit(f"CONNACK failed: {ack.hex()}")
    body = len(topic).to_bytes(2, "big") + topic + payload
    head = b"\x31" if retain else b"\x30"  # retain = fixed-header bit 0
    s.sendall(head + vlq(len(body)) + body)
    time.sleep(0.3)
    s.sendall(b"\xe0\x00")
    s.close()


def main() -> None:
    flags = {"--raw", "--retain", "--delete"}
    args = [a for a in sys.argv[1:] if a not in flags]
    raw = "--raw" in sys.argv
    retain = "--retain" in sys.argv
    delete = "--delete" in sys.argv

    host, topic = args[0], args[1].encode()
    if delete:
        # tombstone: empty retained payload clears the broker's shelf
        # entry and uninstalls on devices (cannot carry a signature)
        port = int(args[2]) if len(args) > 2 else 1883
        mqtt_publish(host, port, topic, b"", retain=True)
        print(f"published tombstone (empty retained) to {args[1]}")
        return

    path = args[2]
    port = int(args[3]) if len(args) > 3 else 1883

    with open(path, "rb") as f:
        script = f.read()

    if raw:
        payload = script
    else:
        with open(KEY, "rb") as f:
            sk = serialization.load_pem_private_key(f.read(), password=None)
        if not isinstance(sk, Ed25519PrivateKey):
            raise SystemExit("key is not Ed25519")
        payload = sk.sign(script) + script   # 64-byte sig prepended

    mqtt_publish(host, port, topic, payload, retain=retain)
    print(f"published {len(payload)} bytes "
          f"({'unsigned' if raw else 'signed'}"
          f"{', retained' if retain else ''}, script {len(script)}) "
          f"to {args[1]}")


if __name__ == "__main__":
    main()
