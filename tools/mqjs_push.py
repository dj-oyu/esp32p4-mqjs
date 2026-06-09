#!/usr/bin/env python3
"""Push a JS script (or a control command) to an esp32p4-mqjs device.

Examples:
    # distribute a script (retained, so the device also gets it on reboot)
    ./mqjs_push.py -b 192.168.1.10 -d aabbccddeeff blink.js

    # control commands
    ./mqjs_push.py -b 192.168.1.10 -d aabbccddeeff --cmd restart
    ./mqjs_push.py -b 192.168.1.10 -d aabbccddeeff --cmd stop
    ./mqjs_push.py -b 192.168.1.10 -d aabbccddeeff --cmd clear

    # watch the device status
    mosquitto_sub -h 192.168.1.10 -t 'mqjs/+/status' -v

The device id is the efuse MAC as 12 hex digits; it is printed on the
serial console at boot ("device id: ...").

Requires: pip install paho-mqtt
"""
import argparse
import sys

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is required: pip install paho-mqtt")

MAX_SCRIPT = 64 * 1024  # MQJS_SCRIPT_MAX on the device


def make_client(args):
    try:  # paho-mqtt >= 2.0
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:  # paho-mqtt 1.x
        client = mqtt.Client()
    if args.username:
        client.username_pw_set(args.username, args.password)
    client.connect(args.broker, args.port, keepalive=10)
    return client


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog="\n".join(__doc__.splitlines()[1:]))
    p.add_argument("-b", "--broker", required=True, help="MQTT broker host")
    p.add_argument("-p", "--port", type=int, default=1883)
    p.add_argument("-d", "--device", required=True,
                   help="device id (12 hex digits, printed at boot)")
    p.add_argument("-t", "--prefix", default="mqjs", help="topic prefix")
    p.add_argument("-u", "--username", default=None)
    p.add_argument("-P", "--password", default=None)
    p.add_argument("--no-retain", action="store_true",
                   help="do not retain the script on the broker")
    p.add_argument("--cmd", choices=["restart", "stop", "clear"],
                   help="send a control command instead of a script")
    p.add_argument("script", nargs="?", help="JS file to distribute")
    args = p.parse_args()

    if bool(args.cmd) == bool(args.script):
        p.error("give either a script file or --cmd")

    if args.cmd:
        topic = f"{args.prefix}/{args.device}/cmd"
        payload = args.cmd
        retain = False
    else:
        with open(args.script, "rb") as f:
            payload = f.read()
        if len(payload) > MAX_SCRIPT:
            sys.exit(f"script too large: {len(payload)} > {MAX_SCRIPT} bytes")
        topic = f"{args.prefix}/{args.device}/script"
        retain = not args.no_retain

    client = make_client(args)
    client.loop_start()
    info = client.publish(topic, payload, qos=1, retain=retain)
    info.wait_for_publish(timeout=10)
    client.loop_stop()
    client.disconnect()

    size = len(payload) if isinstance(payload, (bytes, bytearray)) else len(payload.encode())
    print(f"published {size} bytes to {topic} (retain={retain})")


if __name__ == "__main__":
    main()
