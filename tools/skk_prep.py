#!/usr/bin/env python3
"""Prepare a SKK-JISYO for skk_core: transcode, RE-SORT, index, verify.

    python tools/skk_prep.py build SKK-JISYO.M -o skk_dict.bin
    python tools/skk_prep.py build SKK-JISYO.L --strip-annotations \
        -o skk_dict.bin --emit-dict SKK-JISYO.L.sorted.utf8
    python tools/skk_prep.py alphabet -o skk_alphabet.inc
    python tools/skk_prep.py inspect skk_dict.bin

WHY A RE-SORT IS NOT OPTIONAL (docs/skk-ime-design.md 6.1).  SKK-JISYO
ships sorted in EUC-JP byte order.  Transcoding to UTF-8 silently
reorders it, because U+30FC "-" lives in the symbol row 0xA1BC in EUC-JP
(before every kana) but encodes as E3 83 BC in UTF-8 (after every kana);
U+FF1A ":" moves the same way.  Measured: 2 order violations in M, 285 in
L.  A binary search over a mis-sorted array does not crash -- it answers
wrong for some words on some probe paths.  So this tool re-sorts into
UTF-8 byte order and then VERIFIES the result it wrote, re-reading the
image through an independent parser and binary-searching every entry
back.  Nothing that fails verification is ever written out.

ORDER IS DEFINED IN EXACTLY ONE PLACE: skk_entry_cmp() in
components/skk_core/include/skk_core.h, mirrored here by entry_sort_key().
Primary is the packed key (the first 8 characters folded into a uint64,
each character coded by its rank in UTF-8 byte order); the tie-break is a
memcmp of the whole reading, then shorter-first.  Python's tuple/bytes
comparison is exactly that, byte for byte.  The generator and the engine
must not be able to disagree, which is the whole point of the packed key
-- speed is a side effect.

THE COLLATION TABLE IS FIXED (see ALPHABET).  It is the union of the
reading characters of S/M/ML/L, measured 2026-07-29: 95/127/129/182
distinct characters, each set a strict subset of L's, union 182.  Plum
ships M but is built with the pine alphabet, so moving to ML or L never
changes skk_alphabet_crc32() and never invalidates an image.  Do not
"improve" this table: the CRC is stamped into every image and checked at
open, so adding one character rejects every image built before it.
"""

import argparse
import json
import math
import struct
import sys
import zlib
from pathlib import Path

# --------------------------------------------------------------------
# The collation table.  Union over SKK-JISYO S/M/ML/L reading characters,
# in ascending code-point order (== UTF-8 byte order).  The code of a
# character is its 1-based rank here; 0x00 is END (padding, owned by no
# character, so a prefix packs strictly lower) and 0xFF is ESC (not in
# the table, sorts above every real code).
ALPHABET = (
    "!\"#$%&'()*+,-./0123456789:<=>?@"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
    "abcdefghijklmnopqrstuvwxyz{|}~"
    "、。「」"
    "ぁあぃいぅうぇえぉおかがきぎくぐけげこごさざしじすずせぜそぞ"
    "ただちぢっつづてでとどなにぬねのはばぱひびぴふぶぷへべぺほぼぽ"
    "まみむめもゃやゅゆょよらりるれろわゐゑをん"
    "゛ー："
)

PACK_CHARS = 8       # SKK_PACK_CHARS
CODE_END = 0x00      # SKK_CODE_END
CODE_ESC = 0xFF      # SKK_CODE_ESC

IMAGE_MAGIC = 0x314B4B53  # "SKK1"
IMAGE_VERSION = 2         # 2 added the sampled search tree
IMAGE_ALIGN = 8
HDR_SIZE = 64
HDR_FMT = "<IHHIIIIIIIIIIIII4x"

# The sampled search tree above each key array (skk_core.h, design 6.5).
# Level 1 is every FANOUT-th key, level 2 every FANOUT-th of those, and
# so on while a level still holds FANOUT.  Eight uint64 is one 64-byte
# cache line, which is the whole point: the search touches log8(n) lines
# instead of log2(n).
FANOUT = 8               # SKK_FANOUT
FAN_LEVELS_MAX = 12      # SKK_FAN_LEVELS_MAX

BLK_NASI = 0
BLK_ARI = 1
BLK_NAME = ("okuri-nasi", "okuri-ari")

ARI_MARK = ";; okuri-ari entries."
NASI_MARK = ";; okuri-nasi entries."

# Capacities from skk_core.h, checked against the data so a dictionary
# that would not fit the engine is reported rather than silently truncated
# at runtime.
READING_MAX = 96     # SKK_READING_MAX
COMMIT_MAX = 256     # SKK_COMMIT_MAX

CODE = {ch: i + 1 for i, ch in enumerate(ALPHABET)}
assert len(CODE) == len(ALPHABET), "duplicate character in ALPHABET"
assert len(ALPHABET) <= 254, "alphabet does not leave room for END and ESC"
assert list(ALPHABET) == sorted(ALPHABET), "ALPHABET must be in code-point order"

# CRC-32/ISO-HDLC (seed 0) over the UTF-8 bytes of the table in code
# order.  This is what skk_alphabet_crc32() must return and what
# skk_image_hdr_t.alphabet_crc32 carries.
ALPHABET_CRC32 = zlib.crc32(ALPHABET.encode("utf-8")) & 0xFFFFFFFF


def char_code(ch):
    """skk_char_code(): the character's rank, or ESC."""
    return CODE.get(ch, CODE_ESC)


def pack(reading):
    """skk_pack(): fold a reading into one uint64, first char highest."""
    key = 0
    for i in range(PACK_CHARS):
        code = char_code(reading[i]) if i < len(reading) else CODE_END
        key |= code << (8 * (PACK_CHARS - 1 - i))
    return key


def entry_sort_key(reading):
    """skk_entry_cmp() as a Python sort key.

    Primary the packed key as an unsigned integer, tie-break a memcmp of
    the full UTF-8 reading and then shorter-first -- which is precisely
    what comparing Python bytes does.
    """
    return (pack(reading), reading.encode("utf-8"))


def has_escape(reading):
    return any(ch not in CODE for ch in reading)


# --------------------------------------------------------------------
# Reading the source dictionary


class Entry:
    __slots__ = ("reading", "cands", "line")

    def __init__(self, reading, cands):
        self.reading = reading
        self.cands = cands            # list[str], annotations included
        self.line = None              # bytes, filled in by render()

    def render(self):
        self.line = ("%s /%s/\n" % (self.reading, "/".join(self.cands))).encode("utf-8")
        return self.line


def parse_dict(text, path, warn):
    """Split a decoded SKK-JISYO into its two blocks.

    Returns [nasi_entries, ari_entries].  Comment lines are dropped; the
    ";; okuri-*" markers are what defines the blocks, because guessing
    from the reading is exactly the kind of quiet mistake this tool
    exists to prevent.
    """
    blocks = [[], []]
    cur = None
    malformed = 0
    # split("\n"), NOT splitlines(): the latter also breaks on U+000B,
    # U+000C, U+001C-001E, U+0085, U+2028 and U+2029, while the C engine
    # only ever breaks on '\n'.  An entry containing any of those would be
    # torn into fragments here and dropped, while skk_dict.c would still
    # read it as one line -- the index and the text would then disagree
    # about where entries start, which is the silent-wrong-answer class
    # this tool exists to prevent.
    for lineno, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip("\r")
        if line.startswith(";"):
            if line.startswith(ARI_MARK):
                cur = BLK_ARI
            elif line.startswith(NASI_MARK):
                cur = BLK_NASI
            continue
        if not line.strip():
            continue
        if cur is None:
            raise ValueError(
                "%s:%d: entry before any ';; okuri-ari/nasi entries.' marker"
                % (path, lineno))
        sp = line.find(" ")
        if sp <= 0:
            malformed += 1
            warn("%s:%d: no space between reading and candidates, skipped" % (path, lineno))
            continue
        reading, rest = line[:sp], line[sp + 1:]
        if not (rest.startswith("/") and rest.endswith("/")) or len(rest) < 2:
            malformed += 1
            warn("%s:%d: candidate list is not /.../ , skipped" % (path, lineno))
            continue
        blocks[cur].append(Entry(reading, rest[1:-1].split("/")))
    if malformed:
        warn("%s: %d malformed line(s) skipped" % (path, malformed))
    return blocks


def strip_annotations(entries):
    """Drop ';annotation' from every candidate.

    A bare ';' always starts an annotation: SKK escapes it as \\073 inside
    a (concat "...") form, so no unescaping is needed to find it.  Saves
    538 KB on L.  Candidates that become empty, and entries that lose all
    their candidates, are dropped and counted.
    """
    kept = []
    lost_cands = 0
    lost_entries = 0
    for e in entries:
        out = []
        for c in e.cands:
            i = c.find(";")
            if i >= 0:
                c = c[:i]
            if c:
                out.append(c)
            else:
                lost_cands += 1
        if out:
            e.cands = out
            kept.append(e)
        else:
            lost_entries += 1
    return kept, lost_cands, lost_entries


def merge_duplicates(entries, warn, merge):
    """Fold entries that share a reading (L has one: 'がいひ').

    Two entries with the same reading compare equal, so the binary search
    can land on either and the loser's candidates become unreachable.
    Merging keeps both, in order, without duplicates.
    """
    by_reading = {}
    order = []
    dups = 0
    for e in entries:
        prev = by_reading.get(e.reading)
        if prev is None:
            by_reading[e.reading] = e
            order.append(e)
            continue
        dups += 1
        if not merge:
            raise ValueError("duplicate reading %r (use --merge-duplicates)" % e.reading)
        seen = set(prev.cands)
        prev.cands.extend(c for c in e.cands if not (c in seen or seen.add(c)))
    if dups:
        warn("merged %d duplicate reading(s)" % dups)
    return order, dups


def count_order_violations(entries, ascending):
    """How many adjacent pairs of the SOURCE are out of UTF-8 order.

    Measured in the direction SKK-JISYO itself claims for that block --
    okuri-nasi ascending, okuri-ari descending -- so the count is the
    number of places where the EUC-JP sort the file shipped with is NOT
    a UTF-8 sort.  This is the measurement that justifies the whole tool
    (2 in M, 285 in L, both okuri-nasi), and reporting it makes a silent
    regression in the input or in the comparator visible.

    Returns (out_of_order, equal): a duplicate reading is not a UTF-8
    reordering but it is still fatal for a strictly-ascending index, so
    the two are counted apart and reported apart.
    """
    bad = equal = 0
    for a, b in zip(entries, entries[1:]):
        ka, kb = entry_sort_key(a.reading), entry_sort_key(b.reading)
        if ka == kb:
            equal += 1
        elif (ka > kb) if ascending else (ka < kb):
            bad += 1
    return bad, equal


# --------------------------------------------------------------------
# Image writer


def align_up(n, a):
    return (n + a - 1) // a * a


def fan_level_sizes(count):
    """skk_fan_levels(): [count, n1, n2, ...], derived from count alone.

    Mirrors the C exactly, cap included.  Only the SIZES need to agree --
    if they did not, the reader would slice the tree at the wrong places
    and every lookup past that level would answer wrong without failing.
    So neither side stores them: both compute them.
    """
    sizes = [count]
    n = count
    while n >= FANOUT and len(sizes) < FAN_LEVELS_MAX:
        n //= FANOUT
        sizes.append(n)
    return sizes


def fan_total(count):
    return sum(fan_level_sizes(count)[1:])


def fan_build(keys):
    """skk_fan_build(): the levels, flattened, bottom level first."""
    sizes = fan_level_sizes(len(keys))
    out = []
    cur = keys
    for n in sizes[1:]:
        nxt = [cur[(j + 1) * FANOUT - 1] for j in range(n)]
        out.extend(nxt)
        cur = nxt
    return out


def build_image(blocks, sort=True):
    """Serialise both blocks into the skk_core image.

    Layout (skk_core.h): 64-byte header, then per block a uint64 key
    array and a uint32 offset array, then the entry text.  Every offset
    in the file is relative to the image base -- one blob, one base.
    BOTH BLOCKS ASCEND in the index, including okuri-ari, which
    SKK-JISYO itself stores descending: the index is ours to define and
    one comparator with one search direction is one less way to be
    subtly wrong.

    sort=False keeps the source order.  It exists only so `selftest` can
    build the broken image the re-sort prevents and show the verifier
    rejecting it; no command line reaches it.
    """
    sorted_blocks = [sorted(b, key=lambda e: entry_sort_key(e.reading)) if sort else list(b)
                     for b in blocks]

    text = bytearray()
    text_offs = []
    for entries in sorted_blocks:
        offs = []
        for e in entries:
            offs.append(len(text))
            text += e.render()
        text_offs.append(offs)

    counts = [len(b) for b in sorted_blocks]
    fans = [fan_total(c) for c in counts]
    off = HDR_SIZE
    sec = {}
    for blk in (BLK_NASI, BLK_ARI):
        off = align_up(off, 8)
        sec["keys%d" % blk] = off
        off += 8 * counts[blk]
        # The tree sits next to the keys it samples, ahead of offs[]:
        # the search reads it on every lookup and offs[] once.
        off = align_up(off, 8)
        sec["fan%d" % blk] = off if fans[blk] else 0
        off += 8 * fans[blk]
        off = align_up(off, 4)
        sec["offs%d" % blk] = off
        off += 4 * counts[blk]
    off = align_up(off, 8)
    text_off = off
    image_len = text_off + len(text)

    body = bytearray(image_len)
    body[text_off:image_len] = text
    for blk in (BLK_NASI, BLK_ARI):
        key_list = [pack(e.reading) for e in sorted_blocks[blk]]
        keys = struct.pack("<%dQ" % counts[blk], *key_list)
        offs = struct.pack("<%dI" % counts[blk],
                           *(text_off + o for o in text_offs[blk]))
        p = sec["keys%d" % blk]
        body[p:p + len(keys)] = keys
        if fans[blk]:
            lvls = fan_build(key_list)
            assert len(lvls) == fans[blk], (len(lvls), fans[blk])
            fan = struct.pack("<%dQ" % fans[blk], *lvls)
            p = sec["fan%d" % blk]
            body[p:p + len(fan)] = fan
        p = sec["offs%d" % blk]
        body[p:p + len(offs)] = offs

    crc = zlib.crc32(bytes(body[HDR_SIZE:image_len])) & 0xFFFFFFFF
    hdr = struct.pack(
        HDR_FMT,
        IMAGE_MAGIC, IMAGE_VERSION, 0, image_len, crc, ALPHABET_CRC32,
        text_off, len(text),
        counts[BLK_NASI], sec["keys0"], sec["offs0"],
        counts[BLK_ARI], sec["keys1"], sec["offs1"],
        sec["fan0"], sec["fan1"])
    assert len(hdr) == HDR_SIZE, len(hdr)
    body[0:HDR_SIZE] = hdr
    return bytes(body), sorted_blocks, sec, text_off, len(text)


def render_dict_text(sorted_blocks, header_comment):
    """The re-sorted dictionary as a standalone SKK-JISYO in UTF-8.

    Here the SKK file convention is kept -- okuri-ari DESCENDING,
    okuri-nasi ascending -- so the output is a dictionary other SKK
    implementations can read.  The image's index is unaffected: it
    ascends for both blocks and reaches the text only through offs[].
    """
    out = bytearray()
    out += header_comment.encode("utf-8")
    out += (ARI_MARK + "\n").encode("utf-8")
    for e in reversed(sorted_blocks[BLK_ARI]):
        out += e.line if e.line else e.render()
    out += (NASI_MARK + "\n").encode("utf-8")
    for e in sorted_blocks[BLK_NASI]:
        out += e.line if e.line else e.render()
    return bytes(out)


# --------------------------------------------------------------------
# Image reader / verifier.  Deliberately independent of the writer: it
# parses the bytes back from scratch, so a bug that is symmetric in both
# halves of this file is the only kind it can miss.


class Image:
    def __init__(self, blob):
        self.blob = blob
        if len(blob) < HDR_SIZE:
            raise ValueError("shorter than a header")
        f = struct.unpack(HDR_FMT, blob[:HDR_SIZE])
        (self.magic, self.version, self.flags, self.image_len, self.payload_crc32,
         self.alphabet_crc32, self.text_off, self.text_len,
         self.nasi_count, self.nasi_keys_off, self.nasi_offs_off,
         self.ari_count, self.ari_keys_off, self.ari_offs_off,
         self.nasi_fan_off, self.ari_fan_off) = f
        if self.magic != IMAGE_MAGIC:
            raise ValueError("bad magic 0x%08x (SKK_ERR_MAGIC)" % self.magic)
        if self.version != IMAGE_VERSION:
            raise ValueError("image version %d, expected %d (SKK_ERR_VERSION)"
                             % (self.version, IMAGE_VERSION))
        if self.image_len != len(blob):
            raise ValueError("image_len %d but blob is %d bytes (SKK_ERR_TRUNCATED)"
                             % (self.image_len, len(blob)))
        self.count = (self.nasi_count, self.ari_count)
        self.keys_off = (self.nasi_keys_off, self.ari_keys_off)
        self.offs_off = (self.nasi_offs_off, self.ari_offs_off)
        self.fan_off = (self.nasi_fan_off, self.ari_fan_off)

    def section_checks(self):
        for blk in (BLK_NASI, BLK_ARI):
            if self.keys_off[blk] % 8:
                raise ValueError("%s keys are not 8-byte aligned (SKK_ERR_ALIGN)"
                                 % BLK_NAME[blk])
            if self.offs_off[blk] % 4:
                raise ValueError("%s offsets are not 4-byte aligned" % BLK_NAME[blk])
            if self.keys_off[blk] + 8 * self.count[blk] > self.image_len:
                raise ValueError("%s keys run past the image" % BLK_NAME[blk])
            if self.offs_off[blk] + 4 * self.count[blk] > self.image_len:
                raise ValueError("%s offsets run past the image" % BLK_NAME[blk])
            n = fan_total(self.count[blk])
            if not self.fan_off[blk]:
                if n:
                    raise ValueError("%s has %d entries but no search tree"
                                     % (BLK_NAME[blk], self.count[blk]))
                continue
            if self.fan_off[blk] % 8:
                raise ValueError("%s search tree is not 8-byte aligned "
                                 "(SKK_ERR_ALIGN)" % BLK_NAME[blk])
            if self.fan_off[blk] + 8 * n > self.image_len:
                raise ValueError("%s search tree runs past the image" % BLK_NAME[blk])
        if self.text_off + self.text_len > self.image_len:
            raise ValueError("text runs past the image (SKK_ERR_TRUNCATED)")

    def crc_check(self):
        crc = zlib.crc32(self.blob[HDR_SIZE:self.image_len]) & 0xFFFFFFFF
        if crc != self.payload_crc32:
            raise ValueError("payload CRC 0x%08x, header says 0x%08x (SKK_ERR_CRC)"
                             % (crc, self.payload_crc32))
        if self.alphabet_crc32 != ALPHABET_CRC32:
            raise ValueError("alphabet CRC 0x%08x, this tool's table is 0x%08x "
                             "(SKK_ERR_ALPHABET)"
                             % (self.alphabet_crc32, ALPHABET_CRC32))

    def keys(self, blk):
        p = self.keys_off[blk]
        return struct.unpack_from("<%dQ" % self.count[blk], self.blob, p)

    def offs(self, blk):
        p = self.offs_off[blk]
        return struct.unpack_from("<%dI" % self.count[blk], self.blob, p)

    def levels(self, blk, keys):
        """[keys, L1, L2, ...] as the engine slices the tree at open."""
        sizes = fan_level_sizes(self.count[blk])
        out = [keys]
        p = self.fan_off[blk]
        for n in sizes[1:]:
            out.append(struct.unpack_from("<%dQ" % n, self.blob, p))
            p += 8 * n
        return out

    def line(self, off):
        end = self.blob.index(b"\n", off)
        return self.blob[off:end]

    def reading_at(self, off):
        line = self.line(off)
        sp = line.index(b" ")
        return line[:sp]


def verify_image(blob, search="full", progress=None):
    """Re-read the image and prove every invariant the engine relies on.

    Checks, in order: header sanity, section bounds and alignment, the
    payload and alphabet CRCs, then per block -- every offset lands on a
    line start inside the text, every stored key equals pack() of the
    reading found there, the block is STRICTLY ascending under
    skk_entry_cmp, and (search='full') a real binary search for every
    reading lands back on its own entry.  The last one is the check that
    would have caught the EUC-JP/UTF-8 reordering: a mis-sorted array
    does not fail the others.
    """
    img = Image(blob)
    img.section_checks()
    img.crc_check()
    text_lo, text_hi = img.text_off, img.text_off + img.text_len
    report = {"checked": 0, "searched": 0}

    for blk in (BLK_NASI, BLK_ARI):
        keys = img.keys(blk)
        offs = img.offs(blk)
        prev = None
        readings = []
        for i, (k, o) in enumerate(zip(keys, offs)):
            if not (text_lo <= o < text_hi):
                raise ValueError("%s[%d]: offset %d outside the text section"
                                 % (BLK_NAME[blk], i, o))
            if o != text_lo and blob[o - 1] != 0x0A:
                raise ValueError("%s[%d]: offset %d is not a line start"
                                 % (BLK_NAME[blk], i, o))
            r = img.reading_at(o)
            want = pack(r.decode("utf-8"))
            if want != k:
                raise ValueError("%s[%d]: key 0x%016x but %r packs to 0x%016x"
                                 % (BLK_NAME[blk], i, k, r, want))
            cur = (k, r)
            if prev is not None and cur <= prev:
                raise ValueError(
                    "%s[%d]: NOT ASCENDING -- %r (key 0x%016x) does not follow "
                    "%r (key 0x%016x). The block would binary-search wrong."
                    % (BLK_NAME[blk], i, r, k, prev[1], prev[0]))
            prev = cur
            readings.append(r)
            report["checked"] += 1

        # The sampled tree.  It is derived data, and a tree that does not
        # match its keys does not fail any check above -- it just steers
        # the descent into the wrong eight keys and the lookup comes back
        # empty.  Same silent-wrong-answer family as the sort order, so
        # check it the same way: structurally, then by searching.
        lvls = None
        if img.fan_off[blk]:
            lvls = img.levels(blk, keys)
            for k in range(1, len(lvls)):
                up, dn = lvls[k], lvls[k - 1]
                for j in range(len(up)):
                    if up[j] != dn[(j + 1) * FANOUT - 1]:
                        raise ValueError(
                            "%s: search tree level %d entry %d is 0x%016x but "
                            "samples key 0x%016x. Lookups would silently miss."
                            % (BLK_NAME[blk], k, j, up[j], dn[(j + 1) * FANOUT - 1]))
                report["checked"] += len(up)

        if search == "none" or not readings:
            continue
        probe = readings if search == "full" else readings[::max(1, len(readings) // 512)]
        lines = 0
        for r in probe:
            i = bsearch(img, blk, keys, offs, r)
            if i < 0 or img.reading_at(offs[i]) != r:
                raise ValueError("%s: binary search lost %r" % (BLK_NAME[blk], r))
            if lvls is not None:
                # Run the descent the firmware actually runs and require
                # it to land on the same entry.  This is the check that
                # makes the tree trustworthy; the structural pass above
                # cannot see a wrong LEVEL COUNT, which lands the search
                # in an unrelated part of the array.
                t, ln = tsearch(img, blk, lvls, offs, r)
                lines += ln
                if t != i:
                    raise ValueError("%s: tree search put %r at %d, binary "
                                     "search at %d" % (BLK_NAME[blk], r, t, i))
        report["searched"] += len(probe)
        if progress:
            note = ""
            if lvls is not None:
                note = (", tree agrees (%.1f lines/lookup vs %.1f bisecting)"
                        % (lines / len(probe), math.log2(max(2, len(readings)))))
            progress("  verified %s: %d entries ascending, %d binary searches%s"
                     % (BLK_NAME[blk], len(readings), len(probe), note))
    return img, report


def bsearch(img, blk, keys, offs, reading):
    """The engine's search, in Python: uint64 first, text only on a hit."""
    target_key = pack(reading.decode("utf-8"))
    lo, hi = 0, img.count[blk]
    while lo < hi:
        mid = (lo + hi) // 2
        k = keys[mid]
        if k < target_key:
            lo = mid + 1
        elif k > target_key:
            hi = mid
        else:
            r = img.reading_at(offs[mid])
            if r < reading:
                lo = mid + 1
            elif r > reading:
                hi = mid
            else:
                return mid
    return -1


def fan_lower_bound(lvls, target_key):
    """key_lower_bound() through the sampled tree, exactly as the C does.

    At every level the window is eight consecutive, 8-aligned elements,
    because each separator IS one of the keys below it.  Returns the
    lower bound in lvls[0] and the number of levels touched (= 64-byte
    lines, which is what the search costs on mmap'd flash).
    """
    i, lines = 0, 0
    for k in range(len(lvls) - 1, -1, -1):
        a = lvls[k]
        top = (k == len(lvls) - 1)
        lo = 0 if top else i * FANOUT
        hi = len(a) if top else min(lo + FANOUT, len(a))
        lines += 1
        while lo < hi and a[lo] < target_key:
            lo += 1
        i = lo
    return i, lines


def tsearch(img, blk, lvls, offs, reading):
    """find_entry(): descend the tree, then walk the collision group."""
    target_key = pack(reading.decode("utf-8"))
    i, lines = fan_lower_bound(lvls, target_key)
    keys = lvls[0]
    while i < len(keys) and keys[i] == target_key:
        r = img.reading_at(offs[i])
        if r == reading:
            return i, lines
        if r > reading:
            break
        i += 1
    return -1, lines


# --------------------------------------------------------------------
# Statistics


def collision_stats(entries):
    groups = {}
    for e in entries:
        k = pack(e.reading)
        groups[k] = groups.get(k, 0) + 1
    colliding = sum(n for n in groups.values() if n > 1)
    return {
        "entries": len(entries),
        "distinct_keys": len(groups),
        "colliding": colliding,
        "colliding_pct": round(100.0 * colliding / len(entries), 4) if entries else 0.0,
        "max_group": max(groups.values()) if groups else 0,
    }


def percentile(values, q):
    if not values:
        return 0
    s = sorted(values)
    return s[min(len(s) - 1, int(q * (len(s) - 1) + 0.5))]


def dict_stats(blocks, violations, source_bytes):
    chars = set()
    escaped_entries = 0
    read_bytes = []
    lengths = {}
    cands = 0
    max_cand = 0
    over_reading = 0
    over_commit = 0
    for blk, entries in enumerate(blocks):
        per_block = []
        for e in entries:
            chars.update(e.reading)
            if has_escape(e.reading):
                escaped_entries += 1
            per_block.append(len(e.reading))
            nb = len(e.reading.encode("utf-8"))
            read_bytes.append(nb)
            if nb > READING_MAX:
                over_reading += 1
            cands += len(e.cands)
            for c in e.cands:
                cb = len(c.encode("utf-8"))
                max_cand = max(max_cand, cb)
                if cb > COMMIT_MAX:
                    over_commit += 1
        lengths[BLK_NAME[blk]] = {
            "median_chars": percentile(per_block, 0.5),
            "p90_chars": percentile(per_block, 0.9),
            "max_chars": max(per_block) if per_block else 0,
        }
    n = len(chars)
    bits = max(1, math.ceil(math.log2(n))) if n else 0
    out = {
        "source_bytes": source_bytes,
        "entries": sum(len(b) for b in blocks),
        "okuri_nasi": len(blocks[BLK_NASI]),
        "okuri_ari": len(blocks[BLK_ARI]),
        "candidates": cands,
        "order_violations_utf8": violations,
        "distinct_reading_chars": n,
        "bits_per_char": bits,
        "chars_per_uint64": 64 // bits if bits else 0,
        "chars_outside_table": sorted(chars - set(ALPHABET)),
        "entries_with_escape": escaped_entries,
        "reading_length": lengths,
        "reading_bytes_max": max(read_bytes) if read_bytes else 0,
        "candidate_bytes_max": max_cand,
        "readings_over_SKK_READING_MAX": over_reading,
        "candidates_over_SKK_COMMIT_MAX": over_commit,
        "collisions": {BLK_NAME[b]: collision_stats(blocks[b]) for b in (BLK_NASI, BLK_ARI)},
        # With no escaped characters the packed-key order is provably the
        # UTF-8 memcmp order (codes are ranks in that order, and a tie in
        # the first 8 characters is broken by memcmp anyway). Checking it
        # against the sorted output is a free audit of the whole scheme.
        "utf8_memcmp_agrees": all(
            all(a.reading.encode("utf-8") < b.reading.encode("utf-8")
                for a, b in zip(entries, entries[1:]))
            for entries in blocks),
    }
    return out


def print_stats(st, out=sys.stdout):
    def kb(n):
        return "%d B (%.1f KB)" % (n, n / 1024.0)

    p = lambda s: print(s, file=out)
    p("  source                 %s" % kb(st["source_bytes"]))
    p("  entries                %d  (okuri-nasi %d / okuri-ari %d)"
      % (st["entries"], st["okuri_nasi"], st["okuri_ari"]))
    p("  candidates             %d" % st["candidates"])
    for name, v in st["order_violations_utf8"].items():
        p("  UTF-8 order violations %-11s %d out of order%s  <- in the source, "
          "fixed by the re-sort"
          % (name, v["out_of_order"],
             (", %d duplicate reading(s)" % v["equal"]) if v["equal"] else ""))
    p("  reading chars          %d distinct, %d bit/char, %d chars per uint64"
      % (st["distinct_reading_chars"], st["bits_per_char"], st["chars_per_uint64"]))
    for name, L in st["reading_length"].items():
        p("  reading length %-11s median %d chars, p90 %d chars, max %d chars"
          % (name, L["median_chars"], L["p90_chars"], L["max_chars"]))
    p("  longest reading        %d bytes (SKK_READING_MAX is %d)"
      % (st["reading_bytes_max"], READING_MAX))
    if st["chars_outside_table"]:
        p("  NOT IN THE TABLE       %s  (%d entries pack with SKK_CODE_ESC)"
          % (" ".join(st["chars_outside_table"]), st["entries_with_escape"]))
    else:
        p("  outside the table      none")
    for name, c in st["collisions"].items():
        p("  packed-key %-11s %d entries, %d distinct keys, %.2f%% colliding, "
          "largest group %d" % (name, c["entries"], c["distinct_keys"],
                                c["colliding_pct"], c["max_group"]))
    p("  packed order == memcmp %s" % ("yes" if st["utf8_memcmp_agrees"]
                                       else "no (escaped characters present)"))
    if "image" in st:
        im = st["image"]
        p("  image                  %s" % kb(im["image_bytes"]))
        p("    text                 %s" % kb(im["text_bytes"]))
        p("    packed keys          %s" % kb(im["key_bytes"]))
        if "fan_bytes" in im:
            p("    search tree          %s  (%s)"
              % (kb(im["fan_bytes"]),
                 ", ".join("%s %d levels -> %d lines/lookup" % (n, v, v + 1)
                           for n, v in im["fan_levels"].items())))
        p("    offsets              %s" % kb(im["offs_bytes"]))
        p("    header+padding       %d B" % im["overhead_bytes"])
        p("  alphabet crc32         0x%08x" % ALPHABET_CRC32)
        p("  payload crc32          0x%08x" % im["payload_crc32"])
    if st.get("warnings"):
        for w in st["warnings"]:
            p("  ! %s" % w)


# --------------------------------------------------------------------
# The C collation table


def alphabet_runs():
    runs = []
    for i, ch in enumerate(ALPHABET):
        cp = ord(ch)
        if runs and cp == runs[-1][1] + 1:
            runs[-1][1] = cp
        else:
            runs.append([cp, cp, i + 1])
    return runs


def emit_alphabet_inc():
    runs = alphabet_runs()
    ascii_codes = [CODE.get(chr(c), 0) for c in range(128)]
    lines = []
    a = lines.append
    a("/* skk_alphabet.inc -- GENERATED by tools/skk_prep.py. Do not edit.")
    a(" *")
    a(" * The collation table behind skk_char_code() / skk_pack(): the union of")
    a(" * the reading characters of SKK-JISYO S/M/ML/L (95/127/129/182 distinct,")
    a(" * each a strict subset of L's), so every stage -- plum on M, bamboo on ML,")
    a(" * pine on L -- shares one table and one SKK_ALPHABET_CRC32, and no image")
    a(" * is ever invalidated by moving up a dictionary.")
    a(" *")
    a(" * A character's code is its 1-based rank in UTF-8 byte order, which is what")
    a(" * makes one uint64 compare agree with a memcmp of the readings. 0x00 is")
    a(" * END (padding; owned by no character, so a prefix packs strictly lower)")
    a(" * and 0xFF is ESC (absent from the table; sorts above every real code).")
    a(" *")
    a(" * SKK_ALPHABET_CRC32 is CRC-32/ISO-HDLC (zlib polynomial, seed 0) over the")
    a(" * UTF-8 bytes of the table's characters concatenated in code order. It is")
    a(" * stamped into every image header and checked by skk_dict_open(), so a")
    a(" * stale generator is rejected loudly instead of answering wrong quietly.")
    a(" */")
    a("")
    a("#define SKK_ALPHABET_N     %d" % len(ALPHABET))
    a("#define SKK_ALPHABET_CRC32 0x%08xu" % ALPHABET_CRC32)
    a("")
    a("/* Direct lookup for U+0000..U+007F. 0 means \"not in the table\", which is")
    a("   unambiguous because no character owns SKK_CODE_END. */")
    a("static const uint8_t skk_alphabet_ascii[128] = {")
    for i in range(0, 128, 16):
        a("    " + " ".join("%3d," % c for c in ascii_codes[i:i + 16]))
    a("};")
    a("")
    hi_runs = [r for r in runs if r[1] >= 0x80]
    a("/* The rest as contiguous code-point runs; the code of `lo` is `code` and it")
    a("   increments to `hi`. %d runs, so skk_char_code() is a short linear scan. */"
      % len(hi_runs))
    a("#define SKK_ALPHABET_RUNS %d" % len(hi_runs))
    a("static const struct { uint32_t lo, hi; uint8_t code; }")
    a("skk_alphabet_run[SKK_ALPHABET_RUNS] = {")
    for lo, hi, code in hi_runs:
        a("    { 0x%04X, 0x%04X, %3d },  /* %s%s */"
          % (lo, hi, code, chr(lo), "-" + chr(hi) if hi != lo else ""))
    a("};")
    a("")
    a("/* Every code point in code order, for a generator or a round-trip test. */")
    a("static const uint32_t skk_alphabet_cp[SKK_ALPHABET_N] = {")
    for i in range(0, len(ALPHABET), 8):
        a("    " + " ".join("0x%04X," % ord(c) for c in ALPHABET[i:i + 8]))
    a("};")
    a("")
    return "\n".join(lines)


# --------------------------------------------------------------------
# Commands


def command_build(args):
    warnings = []
    verbose = (not args.quiet)

    def warn(msg):
        warnings.append(msg)
        if verbose:
            print("warning: %s" % msg, file=sys.stderr)

    src = Path(args.input)
    raw = src.read_bytes()
    try:
        text = raw.decode(args.encoding)
    except UnicodeDecodeError as exc:
        raise ValueError("%s: not %s (%s). Try --encoding euc_jp / euc_jisx0213 / utf-8."
                         % (src, args.encoding, exc))

    blocks = parse_dict(text, src.name, warn)
    if not blocks[BLK_NASI] and not blocks[BLK_ARI]:
        raise ValueError("%s: no entries" % src)

    violations = {}
    for blk, ascending in ((BLK_NASI, True), (BLK_ARI, False)):
        bad, equal = count_order_violations(blocks[blk], ascending)
        violations[BLK_NAME[blk]] = {"out_of_order": bad, "equal": equal}

    dropped_cands = dropped_entries = 0
    if args.strip_annotations:
        for blk in (BLK_NASI, BLK_ARI):
            blocks[blk], dc, de = strip_annotations(blocks[blk])
            dropped_cands += dc
            dropped_entries += de
        if dropped_cands or dropped_entries:
            warn("annotation stripping dropped %d empty candidate(s) and %d entry(ies)"
                 % (dropped_cands, dropped_entries))

    for blk in (BLK_NASI, BLK_ARI):
        blocks[blk], _ = merge_duplicates(blocks[blk], warn, args.merge_duplicates)

    blob, sorted_blocks, sec, text_off, text_len = build_image(blocks)

    if verbose:
        print("%s -> %d B image, verifying (search=%s)..."
              % (src.name, len(blob), args.verify))
    img, rep = verify_image(blob, search=args.verify,
                            progress=(print if verbose else None))

    st = dict_stats(sorted_blocks, violations, len(raw))
    key_bytes = 8 * (img.nasi_count + img.ari_count)
    offs_bytes = 4 * (img.nasi_count + img.ari_count)
    fan_bytes = 8 * (fan_total(img.nasi_count) + fan_total(img.ari_count))
    st["image"] = {
        "image_bytes": len(blob),
        "text_bytes": text_len,
        "key_bytes": key_bytes,
        "fan_bytes": fan_bytes,
        "fan_levels": {BLK_NAME[b]: len(fan_level_sizes(img.count[b])) - 1
                       for b in (BLK_NASI, BLK_ARI)},
        "offs_bytes": offs_bytes,
        "overhead_bytes": len(blob) - text_len - key_bytes - offs_bytes - fan_bytes,
        "payload_crc32": img.payload_crc32,
        "alphabet_crc32": ALPHABET_CRC32,
        "verified_entries": rep["checked"],
        "verified_searches": rep["searched"],
    }
    if st["readings_over_SKK_READING_MAX"]:
        warn("%d reading(s) exceed SKK_READING_MAX (%d B) and are unreachable "
             "from the engine" % (st["readings_over_SKK_READING_MAX"], READING_MAX))
    if st["candidates_over_SKK_COMMIT_MAX"]:
        warn("%d candidate(s) exceed SKK_COMMIT_MAX (%d B) and will be truncated "
             "on commit" % (st["candidates_over_SKK_COMMIT_MAX"], COMMIT_MAX))
    if st["chars_outside_table"]:
        warn("%d reading character(s) are not in the collation table and pack as "
             "SKK_CODE_ESC: %s" % (len(st["chars_outside_table"]),
                                   " ".join(st["chars_outside_table"])))
    st["warnings"] = warnings

    # Nothing is written before verification has passed.
    if args.output:
        if args.max_size and len(blob) > args.max_size:
            raise ValueError(
                "image is %d B but the target holds %d B. Flashing it would be "
                "truncated at the partition boundary, and a truncated image is "
                "not obviously broken -- it opens, searches, and answers wrong "
                "past the cut. Use a smaller dictionary, --strip-annotations, "
                "or grow `jisyo` in partitions.csv (it is the last partition, "
                "so growing it moves nothing)." % (len(blob), args.max_size))
        Path(args.output).write_bytes(blob)
    if args.emit_dict:
        header = ";; SKK-JISYO re-sorted into UTF-8 byte order by tools/skk_prep.py\n" \
                 ";; source: %s%s\n" % (src.name,
                                        ", annotations stripped" if args.strip_annotations else "")
        data = render_dict_text(sorted_blocks, header)
        # The emitted dictionary keeps the SKK file convention (okuri-ari
        # descending); prove that too, with the same comparator.
        check_dict_text(data)
        Path(args.emit_dict).write_bytes(data)
    if args.emit_alphabet:
        Path(args.emit_alphabet).write_text(emit_alphabet_inc(), encoding="utf-8")

    if args.json:
        print(json.dumps(st, ensure_ascii=False, indent=2))
    else:
        print("%s" % src.name)
        print_stats(st)
        print("  verified               %d index entries, %d binary searches, "
              "strictly ascending" % (rep["checked"], rep["searched"]))
        for label, path in (("image", args.output), ("dictionary", args.emit_dict),
                            ("alphabet", args.emit_alphabet)):
            if path:
                print("  wrote %-16s %s" % (label, path))
        if not (args.output or args.emit_dict or args.emit_alphabet):
            print("  (no output requested: pass -o / --emit-dict / --emit-alphabet)")


def check_dict_text(data):
    """Verify the emitted SKK-JISYO: nasi ascending, ari descending."""
    cur = None
    prev = {BLK_NASI: None, BLK_ARI: None}
    for lineno, line in enumerate(data.split(b"\n"), 1):
        if line.startswith(b";"):
            if line.startswith(ARI_MARK.encode()):
                cur = BLK_ARI
            elif line.startswith(NASI_MARK.encode()):
                cur = BLK_NASI
            continue
        if not line:
            continue
        r = line[:line.index(b" ")]
        k = (pack(r.decode("utf-8")), r)
        if prev[cur] is not None:
            ok = k > prev[cur] if cur == BLK_NASI else k < prev[cur]
            if not ok:
                raise ValueError("emitted dictionary line %d: %s block is not %s at %r"
                                 % (lineno, BLK_NAME[cur],
                                    "ascending" if cur == BLK_NASI else "descending",
                                    r))
        prev[cur] = k


def command_inspect(args):
    blob = Path(args.image).read_bytes()
    img, rep = verify_image(blob, search=args.verify,
                            progress=(None if args.quiet else print))
    st = {
        "source_bytes": len(blob),
        "entries": img.nasi_count + img.ari_count,
        "okuri_nasi": img.nasi_count,
        "okuri_ari": img.ari_count,
        "text_bytes": img.text_len,
        "payload_crc32": img.payload_crc32,
        "alphabet_crc32": img.alphabet_crc32,
        "verified_entries": rep["checked"],
        "verified_searches": rep["searched"],
    }
    if args.json:
        print(json.dumps(st, ensure_ascii=False, indent=2))
        return
    print("%s" % args.image)
    print("  magic/version          SKK1 / %d" % img.version)
    print("  image                  %d B, text %d B at 0x%x"
          % (img.image_len, img.text_len, img.text_off))
    print("  entries                %d (okuri-nasi %d / okuri-ari %d)"
          % (st["entries"], img.nasi_count, img.ari_count))
    for blk in (BLK_NASI, BLK_ARI):
        sizes = fan_level_sizes(img.count[blk])[1:]
        print("  search tree %-11s %s  ->  %d lines/lookup (bisecting: %.1f)"
              % (BLK_NAME[blk],
                 (" ".join(str(n) for n in sizes) if sizes else "(none)"),
                 len(sizes) + 1, math.log2(max(2, img.count[blk]))))
    print("  payload crc32          0x%08x  OK" % img.payload_crc32)
    print("  alphabet crc32         0x%08x  OK" % img.alphabet_crc32)
    print("  verified               %d index entries, %d binary searches, "
          "strictly ascending" % (rep["checked"], rep["searched"]))
    if args.lookup:
        for reading in args.lookup:
            hit = None
            for blk in (BLK_NASI, BLK_ARI):
                i = bsearch(img, blk, img.keys(blk), img.offs(blk),
                            reading.encode("utf-8"))
                if i >= 0:
                    hit = (blk, i)
                    break
            if hit is None:
                print("  %-12s not found" % reading)
            else:
                blk, i = hit
                line = img.line(img.offs(blk)[i]).decode("utf-8")
                print("  %-12s [%s] %s" % (reading, BLK_NAME[blk], line))


def command_alphabet(args):
    inc = emit_alphabet_inc()
    if args.output:
        Path(args.output).write_text(inc, encoding="utf-8")
        print("wrote %s (%d characters, crc32 0x%08x)"
              % (args.output, len(ALPHABET), ALPHABET_CRC32), file=sys.stderr)
    else:
        sys.stdout.write(inc)
    if args.check:
        missing = {}
        for path in args.check:
            text = Path(path).read_bytes().decode(args.encoding)
            chars = set()
            for line in text.split("\n"):  # see parse_dict(): only '\n'
                if line.startswith(";") or " " not in line:
                    continue
                chars.update(line.split(" ", 1)[0])
            extra = sorted(chars - set(ALPHABET))
            print("check %s: %d distinct reading characters, %d outside the table%s"
                  % (path, len(chars), len(extra),
                     (": " + " ".join(extra)) if extra else ""), file=sys.stderr)
            if extra:
                missing[path] = extra
        if missing:
            raise ValueError(
                "readings use characters absent from the collation table. They "
                "still work (they pack as SKK_CODE_ESC and fall to the full "
                "compare), but if you extend ALPHABET every previously built "
                "image is invalidated -- SKK_ALPHABET_CRC32 changes.")


def command_selftest(args):
    """Prove the guarantees this tool claims, on a synthetic dictionary.

    The one that matters is t3/t4: a mis-sorted index does not crash and
    does not fail a CRC -- it answers wrong for some words on some probe
    paths.  So the tool has to demonstrate, not assert, that its verifier
    rejects exactly that.
    """
    import random

    failures = []

    def check(name, cond, detail=""):
        print("  %-46s %s%s" % (name, "ok" if cond else "FAIL",
                                (" -- " + detail) if detail and not cond else ""))
        if not cond:
            failures.append(name)

    def expect_error(name, fn, needle):
        try:
            fn()
        except ValueError as exc:
            check(name, needle in str(exc), "raised %r" % str(exc)[:70])
            return
        check(name, False, "no error raised")

    # t0 -- the table itself
    check("alphabet is 182 chars, ordered, unique",
          len(ALPHABET) == 182 and list(ALPHABET) == sorted(set(ALPHABET)))
    check("alphabet crc32 == 0x%08x" % ALPHABET_CRC32, ALPHABET_CRC32 != 0)
    check("no character owns SKK_CODE_END", CODE_END not in CODE.values())
    check("SKK_CODE_ESC is above every real code", max(CODE.values()) < CODE_ESC)

    # t1 -- with no escapes, the comparator IS memcmp of the UTF-8
    rng = random.Random(20260729)
    words = ["".join(rng.choice(ALPHABET) for _ in range(rng.randint(1, 12)))
             for _ in range(4000)]
    by_cmp = sorted(words, key=entry_sort_key)
    by_bytes = sorted(words, key=lambda w: w.encode("utf-8"))
    check("packed-key order == UTF-8 memcmp order (4000 words)", by_cmp == by_bytes)
    check("a prefix sorts below its extension",
          entry_sort_key("かん") < entry_sort_key("かんじ"))
    check("the 9th character never changes the packed key",
          pack("あいうえおかきくけ") == pack("あいうえおかきくこ"))

    # t2 -- escaped characters stay a total order
    esc = ["ゔ", "ゎ", "一", "ん", "あ"]
    check("characters outside the table pack as ESC",
          char_code("ゔ") == CODE_ESC and char_code("あ") != CODE_ESC)
    check("escaped words still sort deterministically",
          sorted(esc, key=entry_sort_key) == sorted(esc, key=entry_sort_key))

    # t3 -- end to end over the actual trap: EUC-JP puts "ー" before every
    # kana, UTF-8 puts it after, so this source is sorted for EUC and
    # broken for UTF-8.
    src = ("%s\n" % ARI_MARK +
           "うごk /動/\n"
           "あるk /歩/\n" +
           "%s\n" % NASI_MARK +
           "ぺーじ /頁/\n"      # <- out of UTF-8 order ...
           "ぺい /兵/\n"        # <- ... with this one
           "ぺんき /ペンキ/\n")
    blocks = parse_dict(src, "<selftest>", lambda m: None)
    bad, _ = count_order_violations(blocks[BLK_NASI], True)
    check("synthetic source reproduces the EUC-JP/UTF-8 trap", bad == 1)

    good, sorted_blocks, _, _, _ = build_image(blocks)
    verify_image(good, search="full")
    check("re-sorted image verifies", True)
    check("re-sort moved 'ぺーじ' after 'ぺい'",
          [e.reading for e in sorted_blocks[BLK_NASI]] == ["ぺい", "ぺんき", "ぺーじ"])

    broken, _, _, _, _ = build_image(blocks, sort=False)
    expect_error("un-sorted image is REJECTED",
                 lambda: verify_image(broken, search="full"), "NOT ASCENDING")

    # t4 -- and show what that rejection is protecting: the same array,
    # searched, silently loses a word instead of failing.
    bimg = Image(broken)
    lost = [r for r in ("ぺーじ", "ぺい", "ぺんき")
            if bsearch(bimg, BLK_NASI, bimg.keys(BLK_NASI), bimg.offs(BLK_NASI),
                       r.encode("utf-8")) < 0]
    check("un-sorted array silently loses a word (this is the bug)",
          lost == ["ぺーじ"], "lost %r" % lost)
    gimg = Image(good)
    found = [r for r in ("ぺーじ", "ぺい", "ぺんき", "うごk")
             if any(bsearch(gimg, b, gimg.keys(b), gimg.offs(b), r.encode("utf-8")) >= 0
                    for b in (BLK_NASI, BLK_ARI))]
    check("sorted image finds all four readings", len(found) == 4, "found %r" % found)

    # t5 -- integrity
    flipped = bytearray(good)
    flipped[len(flipped) - 3] ^= 0x20
    expect_error("a flipped payload byte is caught",
                 lambda: verify_image(bytes(flipped), search="none"), "SKK_ERR_CRC")
    stale = bytearray(good)
    stale[16:20] = struct.pack("<I", ALPHABET_CRC32 ^ 1)  # hdr.alphabet_crc32
    expect_error("a stale alphabet stamp is caught",
                 lambda: verify_image(bytes(stale), search="none"), "SKK_ERR_ALPHABET")
    short = good[:-1]
    expect_error("a truncated image is caught",
                 lambda: verify_image(short, search="none"), "SKK_ERR_TRUNCATED")

    # t6 -- the sampled search tree.  The synthetic dictionary above has
    # three entries, which is below SKK_FANOUT and gets no tree at all, so
    # this needs its own corpus large enough for several levels.
    #
    # A wrong tree is the same class of bug as a wrong sort order: nothing
    # crashes, nothing fails a CRC, the descent simply lands in the wrong
    # eight keys and the word is "not in the dictionary".  So sabotage it
    # and RESEAL the CRC first -- otherwise the integrity check fires and
    # proves nothing about the tree.
    kana = "あいうえおかきくけこさしすせそたちつてとなにぬねの"
    big = set()
    while len(big) < 5000:
        big.add("".join(rng.choice(kana) for _ in range(rng.randint(2, 10))))
    big = sorted(big)
    bsrc = ("%s\n" % ARI_MARK + "%s\n" % NASI_MARK +
            "".join("%s /%s/\n" % (r, r) for r in big))
    bblocks = parse_dict(bsrc, "<selftest-tree>", lambda m: None)
    timg, tsorted, tsec, _, _ = build_image(bblocks)

    sizes = fan_level_sizes(len(big))
    check("tree levels are %s" % (sizes[1:],),
          sizes == [5000, 625, 78, 9, 1] and fan_total(5000) == 713)
    verify_image(timg, search="full")
    check("5000-entry tree verifies and agrees with bisection on every entry",
          True)

    def reseal(buf):
        buf[12:16] = struct.pack("<I", zlib.crc32(bytes(buf[HDR_SIZE:])) & 0xFFFFFFFF)
        return bytes(buf)

    # Separator 3 of level 1 is given separator 4's value.  Bumping it by
    # one would be undetectable and mean nothing: the keys are sparse
    # 64-bit values, so widening a separator by 1 changes the answer only
    # for a key that is literally sep+1.  Overshooting it into the NEXT
    # group is the realistic shape of the bug -- an off-by-one in the
    # sampling stride -- and it swallows that group whole.
    bent = bytearray(timg)
    p = tsec["fan0"] + 8 * 3          # level 1, separator 3
    bent[p:p + 8] = bent[p + 8:p + 16]
    expect_error("one bent separator is REJECTED",
                 lambda: verify_image(reseal(bent), search="full"), "samples key")

    # ... and show what that rejection buys, the way t4 does for the sort
    # order: with the separator bent, the descent steps into the wrong
    # eight keys and those readings are simply gone.  Bisection over the
    # same (still perfectly sorted) array finds every one of them, so no
    # other check in this file can see the damage.
    bimg2 = Image(reseal(bent))
    bkeys, boffs = bimg2.keys(BLK_NASI), bimg2.offs(BLK_NASI)
    blvls = bimg2.levels(BLK_NASI, bkeys)
    gone = [r for r in big
            if tsearch(bimg2, BLK_NASI, blvls, boffs, r.encode("utf-8"))[0] < 0]
    still = [r for r in gone
             if bsearch(bimg2, BLK_NASI, bkeys, boffs, r.encode("utf-8")) >= 0]
    check("a bent tree silently loses words (this is the bug)",
          len(gone) > 0 and len(still) == len(gone),
          "lost %d, of which %d are still there by bisection" % (len(gone), len(still)))

    # A tree that is internally consistent but built one level short: the
    # reader slices it by count, so every level lands on the wrong data.
    # The structural check must catch this too -- it is what a fanout
    # mismatch between the writer and the engine would look like.
    shifted = bytearray(timg)
    lv = fan_build([pack(e.reading) for e in tsorted[BLK_NASI]])
    lv = lv[sizes[1]:] + lv[:sizes[1]]        # rotate the levels
    shifted[tsec["fan0"]:tsec["fan0"] + 8 * len(lv)] = struct.pack("<%dQ" % len(lv), *lv)
    expect_error("a tree whose levels are shifted is REJECTED",
                 lambda: verify_image(reseal(shifted), search="full"), "samples key")

    # t7 -- the emitted dictionary keeps the SKK file convention
    check_dict_text(render_dict_text(sorted_blocks, ";; selftest\n"))
    check("emitted dictionary is nasi-ascending / ari-descending", True)

    # t8 -- annotations
    e = parse_dict("%s\n%s\nかんじ /漢字;kanji/感じ/\n" % (ARI_MARK, NASI_MARK),
                   "<selftest>", lambda m: None)
    kept, dc, de = strip_annotations(e[BLK_NASI])
    check("annotation stripping keeps the candidates",
          kept[0].cands == ["漢字", "感じ"] and (dc, de) == (0, 0))

    print("%d check(s) failed" % len(failures) if failures else "all checks passed")
    if failures:
        raise SystemExit(1)


def parser():
    root = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = root.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build", help="transcode, re-sort, index, verify")
    build.add_argument("input", help="SKK-JISYO.{S,M,ML,L}")
    build.add_argument("-o", "--output", help="skk_core image (.bin) output path")
    build.add_argument("--emit-dict", metavar="PATH",
                       help="also write the re-sorted dictionary as UTF-8 text")
    build.add_argument("--emit-alphabet", metavar="PATH",
                       help="also write the C collation table (.inc). NOT into "
                            "components/skk_core/ from here -- pick the path")
    build.add_argument("--encoding", default="euc_jp",
                       help="source encoding (default euc_jp; utf-8 also works)")
    build.add_argument("--strip-annotations", action="store_true",
                       help="drop ';annotation' from every candidate (-538 KB on L)")
    build.add_argument("--merge-duplicates", action=argparse.BooleanOptionalAction,
                       default=True,
                       help="fold entries sharing a reading (L has one); "
                            "--no-merge-duplicates makes it an error")
    build.add_argument("--verify", choices=("full", "sample", "none"), default="full",
                       help="binary-search every reading back (default), a sample, "
                            "or skip. Monotonicity is always checked")
    build.add_argument("--max-size", type=lambda s: int(s, 0), metavar="BYTES",
                       help="refuse to write an image larger than this. Pass the "
                            "`jisyo` partition size (0x880000) when building for "
                            "the partition")
    build.add_argument("--json", action="store_true", help="machine-readable stats")
    build.add_argument("-q", "--quiet", action="store_true")
    build.set_defaults(func=command_build)

    inspect = sub.add_parser("inspect", help="verify and summarise a built image")
    inspect.add_argument("image")
    inspect.add_argument("--lookup", nargs="+", metavar="READING",
                         help="binary-search these readings and print the entries")
    inspect.add_argument("--verify", choices=("full", "sample", "none"), default="full")
    inspect.add_argument("--json", action="store_true")
    inspect.add_argument("-q", "--quiet", action="store_true")
    inspect.set_defaults(func=command_inspect)

    alpha = sub.add_parser("alphabet", help="emit the C collation table")
    alpha.add_argument("-o", "--output", help="default: stdout")
    alpha.add_argument("--check", nargs="+", metavar="DICT",
                       help="report reading characters absent from the table")
    alpha.add_argument("--encoding", default="euc_jp")
    alpha.set_defaults(func=command_alphabet)

    st = sub.add_parser("selftest", help="prove the comparator and the verifier")
    st.set_defaults(func=command_selftest)
    return root


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, OSError):
        pass
    args = parser().parse_args()
    try:
        args.func(args)
    except (OSError, ValueError, struct.error) as exc:
        raise SystemExit("error: %s" % exc)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
