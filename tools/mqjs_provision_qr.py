#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = [
#   "qrcode[pil]>=8.0",
# ]
# ///
"""Create and inspect MQJSP1 device-provisioning QR payloads.

Examples:
  uv run tools/mqjs_provision_qr.py create --ssid home --output setup.png
  uv run tools/mqjs_provision_qr.py inspect --text-file setup.txt

Passwords and auth keys are prompted when their option is passed without a
value. Inspection only prints redacted metadata.
"""

from __future__ import annotations

import argparse
import base64
import getpass
import json
import time
import uuid
from pathlib import Path
from typing import Any


PREFIX = "MQJSP1:"
MAX_TEXT_BYTES = 2048


def canonical_json(payload: dict[str, Any]) -> bytes:
    return json.dumps(
        payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")


def encode_payload(payload: dict[str, Any]) -> str:
    validate_payload(payload)
    body = base64.urlsafe_b64encode(canonical_json(payload)).rstrip(b"=")
    text = PREFIX + body.decode("ascii")
    if len(text.encode("ascii")) > MAX_TEXT_BYTES:
        raise ValueError(f"provisioning text exceeds {MAX_TEXT_BYTES} bytes")
    return text


def decode_text(text: str) -> dict[str, Any]:
    text = text.strip()
    if not text.startswith(PREFIX):
        raise ValueError("not an MQJSP1 provisioning payload")
    encoded = text[len(PREFIX) :]
    if not encoded:
        raise ValueError("empty MQJSP1 payload")
    padding = "=" * (-len(encoded) % 4)
    try:
        raw = base64.b64decode(encoded + padding, altchars=b"-_", validate=True)
        payload = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid MQJSP1 payload") from exc
    if not isinstance(payload, dict):
        raise ValueError("payload must be a JSON object")
    validate_payload(payload)
    return payload


def validate_payload(payload: dict[str, Any]) -> None:
    allowed = {"v", "id", "exp", "device", "wifi", "tailscale"}
    if set(payload) - allowed:
        raise ValueError(f"unknown fields: {sorted(set(payload) - allowed)}")
    if payload.get("v") != 1:
        raise ValueError("v must be 1")
    if "id" in payload and (
        not isinstance(payload["id"], str) or not 1 <= len(payload["id"]) <= 64
    ):
        raise ValueError("id must be a 1-64 character string")
    if "exp" in payload and (
        not isinstance(payload["exp"], int) or payload["exp"] <= 0
    ):
        raise ValueError("exp must be a positive Unix timestamp")
    if "device" in payload and (
        not isinstance(payload["device"], str)
        or not 1 <= len(payload["device"]) <= 63
    ):
        raise ValueError("device must be a 1-63 character string")

    wifi = payload.get("wifi")
    tailscale = payload.get("tailscale")
    if wifi is None and tailscale is None:
        raise ValueError("wifi or tailscale settings are required")
    if wifi is not None:
        if not isinstance(wifi, dict) or set(wifi) != {"ssid", "password"}:
            raise ValueError("wifi must contain exactly ssid and password")
        if not isinstance(wifi["ssid"], str) or not 1 <= len(
            wifi["ssid"].encode("utf-8")
        ) <= 32:
            raise ValueError("wifi.ssid must be 1-32 UTF-8 bytes")
        if not isinstance(wifi["password"], str) or len(
            wifi["password"].encode("utf-8")
        ) > 64:
            raise ValueError("wifi.password must be at most 64 UTF-8 bytes")
    if tailscale is not None:
        if not isinstance(tailscale, dict) or set(tailscale) != {"authKey"}:
            raise ValueError("tailscale must contain exactly authKey")
        key = tailscale["authKey"]
        if not isinstance(key, str) or not 1 <= len(key.encode("utf-8")) <= 255:
            raise ValueError("tailscale.authKey must be 1-255 UTF-8 bytes")


def redacted_summary(payload: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {
        "v": payload["v"],
        "id": payload.get("id"),
        "exp": payload.get("exp"),
        "device": payload.get("device"),
    }
    if "wifi" in payload:
        out["wifi"] = {
            "ssid": payload["wifi"]["ssid"],
            "password": "<present>",
        }
    if "tailscale" in payload:
        out["tailscale"] = {"authKey": "<present>"}
    return {key: value for key, value in out.items() if value is not None}


def prompted(value: str | None, prompt: str) -> str:
    return getpass.getpass(prompt) if value == "" else (value or "")


def command_create(args: argparse.Namespace) -> None:
    payload: dict[str, Any] = {"v": 1, "id": args.id or str(uuid.uuid4())}
    if args.expires_in:
        payload["exp"] = int(time.time()) + args.expires_in
    if args.device:
        payload["device"] = args.device
    if args.ssid:
        if args.open_wifi and args.wifi_password is not None:
            raise ValueError("--open-wifi and --wifi-password are exclusive")
        password = (
            ""
            if args.open_wifi
            else getpass.getpass("Wi-Fi password: ")
            if args.wifi_password in (None, "")
            else args.wifi_password
        )
        payload["wifi"] = {
            "ssid": args.ssid,
            "password": password,
        }
    if args.tailscale_auth_key is not None:
        payload["tailscale"] = {
            "authKey": prompted(args.tailscale_auth_key, "Tailscale auth key: ")
        }

    text = encode_payload(payload)
    if decode_text(text) != payload:
        raise RuntimeError("internal round-trip verification failed")

    if args.text_out:
        Path(args.text_out).write_text(text + "\n", encoding="ascii")
    if args.output:
        import qrcode

        qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M)
        qr.add_data(text)
        qr.make(fit=True)
        qr.make_image(
            fill_color="white" if args.dark else "black",
            back_color="black" if args.dark else "white",
        ).save(args.output)
    if not args.text_out and not args.output:
        raise ValueError("specify --output and/or --text-out")
    print(json.dumps(redacted_summary(payload), ensure_ascii=False, indent=2))
    print(f"encoded bytes: {len(text.encode('ascii'))}")


def command_inspect(args: argparse.Namespace) -> None:
    if bool(args.text) == bool(args.text_file):
        raise ValueError("specify exactly one of --text or --text-file")
    text = args.text or Path(args.text_file).read_text(encoding="ascii")
    payload = decode_text(text)
    print(json.dumps(redacted_summary(payload), ensure_ascii=False, indent=2))
    if "exp" in payload:
        print("expired:", payload["exp"] < int(time.time()))


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    sub = root.add_subparsers(dest="command", required=True)

    create = sub.add_parser("create", help="create provisioning text/QR")
    create.add_argument("--ssid")
    create.add_argument(
        "--wifi-password",
        nargs="?",
        const="",
        help="value, or omit value to prompt without shell history",
    )
    create.add_argument("--open-wifi", action="store_true")
    create.add_argument(
        "--tailscale-auth-key",
        nargs="?",
        const="",
        help="value, or omit value to prompt without shell history",
    )
    create.add_argument("--device")
    create.add_argument("--id")
    create.add_argument("--expires-in", type=int, metavar="SECONDS")
    create.add_argument("--output", help="PNG output path")
    create.add_argument(
        "--dark",
        action="store_true",
        help="white modules on black background for display-camera scanning",
    )
    create.add_argument("--text-out", help="wire text output path")
    create.set_defaults(func=command_create)

    inspect = sub.add_parser("inspect", help="validate and summarize wire text")
    inspect.add_argument("--text")
    inspect.add_argument("--text-file")
    inspect.set_defaults(func=command_inspect)
    return root


def main() -> int:
    args = parser().parse_args()
    try:
        args.func(args)
    except (OSError, ValueError) as exc:
        raise SystemExit(f"error: {exc}") from exc
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
