#!/usr/bin/env python3
"""Sign a JS app into a file a Tab5 can read straight off a microSD card.

    python3 tools/mqjs_pack.py OUTDIR FILE [FILE ...]

Writes OUTDIR/<name>.mjsa for each input, where <name> comes from the
file's "// @app <name>" first line. Point OUTDIR at the card's apps
folder:

    python3 tools/mqjs_pack.py E:/apps examples/files.js

The bytes written are EXACTLY what mqjs_push.py --shelf publishes to
<base>/apps/<name>: a 64-byte Ed25519 signature followed by the script.
The device verifies it with the same crypto_sign_open() and the same
embedded public key, so a card is not a second trust path -- only the
holder of tools/task_signing_key.pem can put a runnable app on one.
See docs/filer-storage-design.md section 13.

The card is a distribution channel, not a home: the device installs a
verified app into its own littlefs, so pulling the card afterwards does
not take the app with it.
"""
import os
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY = os.path.join(ROOT, "tools", "task_signing_key.pem")


def load_key() -> Ed25519PrivateKey:
    with open(KEY, "rb") as f:
        sk = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(sk, Ed25519PrivateKey):
        raise SystemExit("key is not Ed25519")
    return sk


def app_name(script: bytes, path: str) -> str:
    first = script.split(b"\n", 1)[0]
    if not first.startswith(b"// @app "):
        raise SystemExit(f"{path}: needs a '// @app <name>' first line")
    name = first[8:].strip().decode()
    # The device only accepts these characters in a card filename (it has
    # to become both a path and an app name); reject here so a bad name
    # fails on the desk instead of silently vanishing from the store page.
    if not name or not all(c.isalnum() or c in "_-" for c in name):
        raise SystemExit(f"{path}: '{name}' is not a usable app name")
    return name


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    sk = load_key()
    for path in sys.argv[2:]:
        with open(path, "rb") as f:
            script = f.read()
        name = app_name(script, path)
        body = sk.sign(script) + script
        out = os.path.join(outdir, name + ".mjsa")
        with open(out, "wb") as f:
            f.write(body)
        print(f"packed {name}: {len(body)}B -> {out}")


if __name__ == "__main__":
    main()
