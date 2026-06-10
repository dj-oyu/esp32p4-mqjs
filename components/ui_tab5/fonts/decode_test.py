#!/usr/bin/env python3
"""Host validation of the 4bpp glyph bit-packing for font_term_mono.c.

Parses the lv_font_conv output (glyph_dsc + glyph_bitmap) and decodes a
few glyphs to ASCII art using the SAME bit-unpacking the C blit uses, so
we confirm the packing assumption (continuous bitstream, MSB nibble
first) BEFORE running it on the device. Run: python3 decode_test.py
"""
import re, sys

src = open("font_term_mono.c", encoding="utf-8").read()

# glyph_bitmap[] bytes
m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\};", src, re.S)
bitmap = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", m.group(1))]

# glyph_dsc[] entries
m = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\};", src, re.S)
dsc = []
for line in re.findall(r"\{[^}]*\}", m.group(1)):
    g = {}
    for k in ("bitmap_index", "adv_w", "box_w", "box_h", "ofs_x", "ofs_y"):
        mm = re.search(k + r"\s*=\s*(-?\d+)", line)
        g[k] = int(mm.group(1)) if mm else 0
    dsc.append(g)

print("glyphs=%d bitmap_bytes=%d" % (len(dsc), len(bitmap)))

# packing check: delta between consecutive bitmap_index vs continuous vs row-padded
def cont_bytes(g):
    return (g["box_w"] * g["box_h"] * 4 + 7) // 8
def padded_bytes(g):
    return g["box_h"] * ((g["box_w"] * 4 + 7) // 8)

cont_ok = padded_ok = True
for i in range(1, len(dsc) - 1):
    delta = dsc[i + 1]["bitmap_index"] - dsc[i]["bitmap_index"]
    if dsc[i]["box_w"] == 0:
        continue
    if delta != cont_bytes(dsc[i]):
        cont_ok = False
    if delta != padded_bytes(dsc[i]):
        padded_ok = False
print("packing: continuous=%s row_padded=%s" % (cont_ok, padded_ok))

def decode(gi, continuous):
    g = dsc[gi]
    start = g["bitmap_index"]
    bw, bh = g["box_w"], g["box_h"]
    print("glyph[%d] box=%dx%d adv=%.1f ofs=(%d,%d)" %
          (gi, bw, bh, g["adv_w"] / 16, g["ofs_x"], g["ofs_y"]))
    for py in range(bh):
        row = ""
        rowbit = py * bw * 4 if continuous else py * ((bw * 4 + 7) // 8) * 8
        for px in range(bw):
            bit = rowbit + px * 4
            byte = bitmap[start + (bit >> 3)]
            a = (byte & 0x0F) if (bit & 4) else (byte >> 4)
            row += " .:-=+*#@"[min(8, a // 2)]
        print("  |" + row + "|")

# ASCII glyph index = codepoint - 0x20 + 1 (index 0 = reserved). 'A'=0x41, 'M'=0x4D
mode = cont_ok if cont_ok != padded_ok else True
for ch in "AM#":
    gi = ord(ch) - 0x20 + 1
    print("=== '%s' (continuous packing) ===" % ch)
    decode(gi, True)
