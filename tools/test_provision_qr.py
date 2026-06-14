#!/usr/bin/env python3
"""Host tests for the MQJSP1 provisioning wire format."""

import importlib.util
from pathlib import Path


MODULE = Path(__file__).with_name("mqjs_provision_qr.py")
spec = importlib.util.spec_from_file_location("mqjs_provision_qr", MODULE)
assert spec and spec.loader
qr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qr)


def main() -> int:
    payload = {
        "v": 1,
        "id": "test-1",
        "wifi": {"ssid": "home", "password": "secret"},
        "tailscale": {"authKey": "tskey-auth-test"},
    }
    text = qr.encode_payload(payload)
    assert text.startswith(qr.PREFIX)
    assert qr.decode_text(text) == payload
    summary = qr.redacted_summary(payload)
    assert summary["wifi"]["password"] == "<present>"
    assert summary["tailscale"]["authKey"] == "<present>"
    assert "secret" not in str(summary)
    try:
        qr.decode_text("MQJSP1:e30")
    except ValueError:
        pass
    else:
        raise AssertionError("invalid payload was accepted")
    print("PASS: MQJSP1 round trip and redaction")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
