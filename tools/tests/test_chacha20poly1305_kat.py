#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["cryptography>=42"]
# ///
"""Host KAT for the microlink ChaCha20-Poly1305 PSA port (Phase 3, layer 1).

The PSA migration rewrote `chacha20poly1305_encrypt/decrypt` in
components/microlink/src/ml_noise.c from mbedtls_chachapoly_* to PSA
psa_aead_*. This test locks down the *contract* that port depends on, using
Python `cryptography` (RFC 8439 IETF ChaCha20-Poly1305, 12-byte nonce) as a
trusted oracle:

  1. oracle sanity vs the official RFC 8439 / RFC 7539 §2.8.2 test vector
  2. the Tailscale nonce construction (4 zero bytes + 8-byte BIG-endian counter)
     exactly as ml_noise.c builds it
  3. ciphertext||tag contiguous output layout (PSA buffer-sizing assumption)
  4. encrypt/decrypt round-trip and tamper -> auth failure
  5. emit golden vectors for the on-device test (layer 2) to compare microlink's
     actual PSA output against

PSA call equivalence itself needs the IDF runtime and is a device test; this
validates everything the port relies on that is checkable on the host.
"""
from __future__ import annotations

import sys

from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.exceptions import InvalidTag

passed = 0
failed = 0


def check(name: str, cond: bool) -> None:
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"FAIL: {name}")


def ts_nonce(counter: int) -> bytes:
    """Tailscale ts2021 nonce, matching ml_noise.c:
    4 zero bytes + 8-byte big-endian counter."""
    return b"\x00\x00\x00\x00" + counter.to_bytes(8, "big")


# --- 1. Oracle sanity: official RFC 8439 / RFC 7539 sec 2.8.2 vector ----------
rfc_key = bytes(range(0x80, 0xA0))                      # 80..9f (32 bytes)
rfc_nonce = bytes.fromhex("070000004041424344454647")  # 12-byte IETF nonce
rfc_aad = bytes.fromhex("50515253c0c1c2c3c4c5c6c7")
rfc_pt = (
    b"Ladies and Gentlemen of the class of '99: If I could offer you "
    b"only one tip for the future, sunscreen would be it."
)
rfc_expected_ct = bytes.fromhex(
    "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
    "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
    "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
    "3ff4def08e4b7a9de576d26586cec64b6116"
)
rfc_expected_tag = bytes.fromhex("1ae10b594f09e26a7e902ecbd0600691")

out = ChaCha20Poly1305(rfc_key).encrypt(rfc_nonce, rfc_pt, rfc_aad)
check("RFC8439 ciphertext", out[:-16] == rfc_expected_ct)
check("RFC8439 tag", out[-16:] == rfc_expected_tag)
check("RFC8439 output is ct||tag contiguous", len(out) == len(rfc_pt) + 16)

# --- 2. Tailscale nonce construction (matches ml_noise.c byte order) ----------
check("nonce counter=0", ts_nonce(0) == bytes.fromhex("000000000000000000000000"))
check("nonce counter=1", ts_nonce(1) == bytes.fromhex("000000000000000000000001"))
check("nonce counter=255", ts_nonce(255) == bytes.fromhex("0000000000000000000000ff"))
check(
    "nonce counter=0x0102030405060708 big-endian",
    ts_nonce(0x0102030405060708) == bytes.fromhex("000000000102030405060708"),
)

# --- 3/4. Round-trip + tamper detection with the Tailscale nonce --------------
key = bytes(range(32))                 # 00..1f
aad = b"ts2021-handshake-aad"
pt = b"the quick brown fox jumps over the lazy dog" * 3
aead = ChaCha20Poly1305(key)

for counter in (0, 1, 42, 0xDEADBEEF, 0xFFFFFFFFFFFFFFFF):
    n = ts_nonce(counter)
    ct = aead.encrypt(n, pt, aad)
    check(f"roundtrip counter={counter}", aead.decrypt(n, ct, aad) == pt)
    check(f"ct||tag length counter={counter}", len(ct) == len(pt) + 16)

# tamper: flip a tag byte -> auth failure
n = ts_nonce(7)
ct = bytearray(aead.encrypt(n, pt, aad))
ct[-1] ^= 0x01
try:
    aead.decrypt(n, bytes(ct), aad)
    check("tampered tag rejected", False)
except InvalidTag:
    check("tampered tag rejected", True)

# tamper: wrong AAD -> auth failure
ct = aead.encrypt(n, pt, aad)
try:
    aead.decrypt(n, ct, b"wrong-aad")
    check("wrong AAD rejected", False)
except InvalidTag:
    check("wrong AAD rejected", True)

# --- 5. Golden vectors for the on-device test (layer 2) -----------------------
print(f"\n{passed} passed, {failed} failed")
if failed == 0:
    print("\n--- golden vectors (feed identical inputs to microlink on device,")
    print("    compare chacha20poly1305_encrypt output to expected_ct_tag) ---")
    g_key = bytes(range(0x10, 0x30))   # 10..2f
    g_aad = bytes.fromhex("0011223344556677")
    g_pt = b"microlink ts2021 noise payload"
    print(f"key      = {g_key.hex()}")
    print(f"aad      = {g_aad.hex()}")
    print(f"pt       = {g_pt.hex()}  ({g_pt!r})")
    for counter in (0, 1, 0x0102030405060708):
        ctt = ChaCha20Poly1305(g_key).encrypt(ts_nonce(counter), g_pt, g_aad)
        print(f"counter={counter:#x}  nonce={ts_nonce(counter).hex()}  "
              f"expected_ct_tag={ctt.hex()}")

sys.exit(1 if failed else 0)
