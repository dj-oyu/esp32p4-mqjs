#!/usr/bin/env python3
"""Dependency-free MQTT 3.1.1 publisher (QoS 0).

Usage: mqtt_pub.py HOST TOPIC FILE|-m MESSAGE [PORT]

Used to push JS tasks to the Stamp-P4 task-source topic:
    python3 tools/mqtt_pub.py test.mosquitto.org esp32p4-mqjs/task/XXXX examples/bench.js
"""
import socket
import sys
import time


def vlq(n: int) -> bytes:
    out = b""
    while True:
        b = n % 128
        n //= 128
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out


def main() -> None:
    host = sys.argv[1]
    topic = sys.argv[2].encode()
    if sys.argv[3] == "-m":
        payload = sys.argv[4].encode()
        port = int(sys.argv[5]) if len(sys.argv) > 5 else 1883
    else:
        with open(sys.argv[3], "rb") as f:
            payload = f.read()
        port = int(sys.argv[4]) if len(sys.argv) > 4 else 1883

    client_id = b"mqtt-pub-py"
    var = b"\x00\x04MQTT\x04\x02\x00\x3c" + len(client_id).to_bytes(2, "big") + client_id
    connect = b"\x10" + vlq(len(var)) + var

    s = socket.create_connection((host, port), timeout=10)
    s.sendall(connect)
    ack = s.recv(4)
    if len(ack) < 4 or ack[0] != 0x20 or ack[3] != 0:
        raise SystemExit(f"CONNACK failed: {ack.hex()}")

    body = len(topic).to_bytes(2, "big") + topic + payload
    s.sendall(b"\x30" + vlq(len(body)) + body)
    time.sleep(0.3)          # let the broker take the QoS0 packet
    s.sendall(b"\xe0\x00")   # DISCONNECT
    s.close()
    print(f"published {len(payload)} bytes to {sys.argv[2]}")


if __name__ == "__main__":
    main()
