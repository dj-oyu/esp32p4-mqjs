#!/usr/bin/env python3
"""Regenerate components/ui_tab5/fonts/font_term_mono.c (design S4).

    uv run --with fonttools python tools/font_term_mono.py [--out PATH]

Why this exists: the first two generations were run by hand with the TTF
sitting in a temp directory, so the recipe survived only as the "Opts:" line
in the generated .c and the font itself was gone. Regenerating meant
re-deriving the character set from scratch.

What it produces: HackGen Console NF v2.10.0, 17px, 4bpp, uncompressed —
  - the original ranges (ASCII, box drawing, Nerd Font BMP icons), and
  - JIS X 0208 ku 1-7 and 16-47, enumerated through the euc_jp codec rather
    than hand-listed: symbols, fullwidth alnum, kana, Greek, Cyrillic and the
    2,965 level-1 kanji. ku 8 is skipped (box drawing is already covered).

Two things it refuses to let you get wrong:
  - glyph_bitmap >= 1 MB. bitmap_index is a :20 bitfield when
    CONFIG_LV_FONT_FMT_TXT_LARGE is off, so going over does not fail the
    build — it wraps and draws garbage (design 7.5).
  - a CJK glyph wider than two 9px cells, which ui.cells would clip.

The _Static_assert in the .c is NOT emitted by lv_font_conv; this script
re-inserts it. Run components/ui_tab5/fonts/decode_test.py afterwards: the
raw-bitmap blit in ui_tab5.cpp depends on the packing staying a continuous
4bpp bitstream, and lv_font_conv is free to change that between versions.
"""
import argparse
import io
import os
import re
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

FONT_URL = ("https://github.com/yuru7/HackGen/releases/download/"
            "v2.10.0/HackGen_NF_v2.10.0.zip")
TTF = "HackGenConsoleNF-Regular.ttf"
SIZE, BPP = 17, 4
CELL_W = 9
CAP = 1 << 20
KEEP_RANGES = ["0x20-0x7E", "0x2500-0x259F", "0x23FB-0x23FE", "0x2B58",
               "0xE000-0xF8FF"]
KU = list(range(1, 8)) + list(range(16, 48))
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(ROOT, "components", "ui_tab5", "fonts",
                           "font_term_mono.c")

ASSERT = """
/* HAND-ADDED, NOT FROM lv_font_conv — tools/font_term_mono.py re-inserts it
   after every regeneration (docs/skk-ime-design.md 7.5 / 11).
   CONFIG_LV_FONT_FMT_TXT_LARGE is off, so lv_font_fmt_txt.h declares
   `uint32_t bitmap_index : 20` = one font's glyph_bitmap may not exceed
   1 MB. Crossing it does NOT fail the build: the index wraps at 0x100000
   and glyphs past that point silently draw garbage. %d B here (%.1f%%). */
_Static_assert(sizeof(glyph_bitmap) < (1u << 20),
               "font_term_mono glyph_bitmap >= 1MB: bitmap_index:20 wraps "
               "and rendering breaks with no build error");
"""


def jis_symbols():
    """JIS X 0208 ku 1-7 + 16-47, minus what KEEP_RANGES already covers."""
    have = set()
    for r in KEEP_RANGES:
        lo, _, hi = r.partition("-")
        have |= set(range(int(lo, 16), int(hi or lo, 16) + 1))
    out = []
    for ku in KU:
        for ten in range(1, 95):
            try:
                c = bytes([0xA0 + ku, 0xA0 + ten]).decode("euc_jp")
            except UnicodeDecodeError:
                continue                      # unassigned slot
            if ord(c) not in have:
                out.append(c)
    return "".join(sorted(set(out), key=ord))


def fetch_ttf(cache):
    path = os.path.join(cache, TTF)
    if os.path.exists(path):
        return path
    zpath = os.path.join(cache, os.path.basename(FONT_URL))
    if not os.path.exists(zpath):
        print("downloading %s" % FONT_URL)
        urllib.request.urlretrieve(FONT_URL, zpath)
    with zipfile.ZipFile(zpath) as z:
        member = next(n for n in z.namelist() if n.endswith("/" + TTF)
                      or n == TTF)
        with z.open(member) as src, io.open(path, "wb") as dst:
            dst.write(src.read())
    return path


def bitmap_bytes(src):
    m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    # single-digit literals ("0x0,") are emitted too; a {2}-digit pattern
    # undercounts by ~44% and would wave an over-cap font through
    return len(re.findall(r"0x[0-9a-fA-F]{1,2}\b", m.group(1)))


def check_widths(src):
    """No glyph may reach past the two cells ui.cells will clip it to."""
    m = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    entries = re.findall(r"\{[^}]*\}", m.group(1))[1:]   # [0] is the dummy
    cps = [int(h, 16) for h in re.findall(r"/\* U\+([0-9A-Fa-f]{4,6})", src)]
    bad = []
    for i in range(min(len(entries), len(cps))):
        c = cps[i]
        if not ((0x2E80 <= c <= 0xA4CF) or (0xFF00 <= c <= 0xFF60) or
                (0xFFE0 <= c <= 0xFFE6) or (0x3040 <= c <= 0x30FF)):
            continue
        g = {k: int(re.search(k + r"\s*=\s*(-?\d+)", entries[i]).group(1))
             for k in ("box_w", "ofs_x")}
        if g["ofs_x"] + g["box_w"] > 2 * CELL_W:
            bad.append((c, g["ofs_x"] + g["box_w"]))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--cache", default=os.path.join(tempfile.gettempdir(),
                                                    "hackgen_nf"))
    args = ap.parse_args()

    os.makedirs(args.cache, exist_ok=True)
    ttf = fetch_ttf(args.cache)
    syms = jis_symbols()
    print("added codepoints: %d" % len(syms))

    # lv_font_conv derives the lv_font_t symbol name from the output FILE
    # NAME, so it has to be generated as font_term_mono.c — writing to
    # "<out>.new" would silently define font_term_mono_new and the link
    # fails with "undefined reference to font_term_mono".
    stage = tempfile.mkdtemp(prefix="lvfont")
    tmp = os.path.join(stage, os.path.basename(args.out))
    cmd = ["npx", "--yes", "lv_font_conv", "--font", ttf,
           "--size", str(SIZE), "--bpp", str(BPP), "--no-compress",
           "--format", "lvgl", "--lv-include", "lvgl.h"]
    for r in KEEP_RANGES:
        cmd += ["-r", r]
    cmd += ["--symbols", syms, "-o", tmp]
    subprocess.run(cmd, check=True, shell=(os.name == "nt"))

    src = io.open(tmp, encoding="utf-8", errors="replace").read()
    n = bitmap_bytes(src)
    print("glyph_bitmap: %d B (%.1f%% of the 1 MB cap)" % (n, 100.0 * n / CAP))
    if n >= CAP:
        print("OVER CAP — refusing to install (rendering would silently break)")
        sys.exit(3)
    bad = check_widths(src)
    if bad:
        print("glyphs reaching past 2 cells: %d, e.g. U+%04X -> %dpx"
              % (len(bad), bad[0][0], bad[0][1]))
        sys.exit(3)

    src = src.replace("\n};\n\n\n/*---------------------\n *  GLYPH DESCRIPTION",
                      "\n};\n" + (ASSERT % (n, 100.0 * n / CAP)) +
                      "\n\n/*---------------------\n *  GLYPH DESCRIPTION", 1)
    if "_Static_assert" not in src:
        print("could not splice the _Static_assert — add it by hand")
        sys.exit(3)
    if "lv_font_t %s " % os.path.splitext(os.path.basename(args.out))[0] \
            not in src:
        print("generated file does not define the expected lv_font_t symbol")
        sys.exit(3)
    io.open(args.out, "w", encoding="utf-8", newline="\n").write(src)
    os.remove(tmp)
    os.rmdir(stage)
    print("wrote %s" % args.out)
    print("now run: cd components/ui_tab5/fonts && python3 decode_test.py")


if __name__ == "__main__":
    main()
