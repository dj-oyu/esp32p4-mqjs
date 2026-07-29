/*
 * skk_dict.c — the order-defining half of skk_core.
 *
 * Everything here exists to make one sentence true: THE PREP TOOL THAT
 * SORTS AND THE ENGINE THAT SEARCHES USE THE SAME COMPARATOR. SKK-JISYO
 * ships sorted in EUC-JP byte order and transcoding it to UTF-8 silently
 * reorders it (U+30FC "ー" and U+FF1A "：" move: 2 violations in M, 285
 * in L). A binary search over a mis-sorted array does not crash — it
 * answers wrong for some words on some probe paths, which is the worst
 * kind of bug to have in an input method. So the collation table, the
 * packed key, the comparator, the index builder and the search all live
 * in this one file, and skk_index_verify() exists to prove at runtime
 * that an image really is in that order.
 *
 * Contents:
 *   - CRC-32/ISO-HDLC (shared with the prep tool and the image header)
 *   - the 182-character collation table and skk_char_code()/skk_pack()
 *   - skk_entry_cmp(), the definition of dictionary order
 *   - skk_dict_open(): validate an image produced by tools/skk_prep.py
 *   - binary search + zero-copy candidate extraction
 *   - index construction and verification (prep tool / host tests)
 *   - the personal dictionary's MRU ordering, with pure serialisation
 *     (no file I/O — that is the caller's business, see §S7)
 *
 * No allocation, no clock, no ESP-IDF headers. The uint64 key arrays are
 * read in place out of the image, which assumes a little-endian host —
 * the same assumption the on-disk format already makes. Both targets
 * (riscv32, x86-64 host tests) are little-endian; a big-endian port would
 * have to byte-swap in skk_dict_open() and is deliberately not attempted.
 */

#include "skk_core.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Three functions this file exports that skk_core.h does not declare
 * yet. They are not new policy — each is the machinery the design
 * already asks for, just named:
 *
 *   skk_lookup_stats  skk_lookup() plus the counters, so skk_core.c can
 *                     fill skk_stats_t.probes/fullcmp/cands/dropped
 *                     without a second search. skk_lookup() is this with
 *                     a NULL `st`.
 *   skk_text_split    find the ";; okuri-ari/nasi entries." markers, the
 *                     first thing the prep tool must do (design §6.1);
 *                     skk_index_build() takes ONE block.
 *   skk_index_check   the monotonicity check on the arrays as built,
 *                     before an image exists. skk_index_verify() is the
 *                     same check on an opened image.
 *
 * NOTE FOR THE HEADER OWNER: move these declarations (and the MRU block
 * near the bottom of this file) into skk_core.h and define the two
 * guards there; that makes the move a no-op here. */
#ifndef SKK_DICT_EXTRAS_DECLARED
#define SKK_DICT_EXTRAS_DECLARED
int skk_lookup_stats(const skk_dict_t *d, skk_blk_t blk,
                     const char *reading, size_t len,
                     skk_cand_t *out, size_t cap, size_t *out_n,
                     skk_stats_t *st);
int skk_text_split(skk_blob_t text, skk_blob_t *out_ari, skk_blob_t *out_nasi);
int skk_index_check(skk_blob_t text, const uint64_t *keys, const uint32_t *offs,
                    size_t n, uint32_t *out_at);
#endif

/* ================================================================== */
/* CRC-32/ISO-HDLC (the zlib polynomial, reflected 0xEDB88320).
 *
 * A 16-entry nibble table rather than the usual 256-entry one: 64 bytes
 * of rodata instead of 1 KB, and the only caller that moves real data is
 * SKK_OPEN_VERIFY, which runs once at open. Table-driven and scalar on
 * purpose — PIE has no carry-less multiply and a gather is exactly the
 * shape it loses at (design §4.6). */

static const uint32_t s_crc_nib[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

uint32_t skk_crc32(uint32_t seed, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~seed;
    size_t i;

    if (!p) {
        return seed;
    }
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ s_crc_nib[crc & 0x0Fu];
        crc = (crc >> 4) ^ s_crc_nib[crc & 0x0Fu];
    }
    return ~crc;
}

/* ================================================================== */
/* The collation table.
 *
 * MEASURED, not guessed (2026-07-29, over the shipped dictionaries):
 * the set of characters that occur in a reading is 95 in S, 127 in M,
 * 129 in ML and 182 in L, and each is a strict subset of L's — so their
 * union is exactly L's 182. That union is this table, used at every
 * stage, so plum (M) and bamboo (ML) are built with the pine alphabet
 * and skk_alphabet_crc32() never changes when the dictionary grows.
 *
 * The array is sorted ascending by code point, which for UTF-8 is the
 * same as sorted by encoded byte order (UTF-8 is order-preserving and
 * prefix-free). A character's code is its index + 1, so:
 *
 *   - comparing two packed keys as unsigned integers, most significant
 *     byte first, compares the readings character by character in
 *     UTF-8 order — which is what memcmp of the readings does;
 *   - code 0 (SKK_CODE_END) is owned by no character, so a reading that
 *     is a prefix of another packs strictly lower, exactly as strcmp;
 *   - 0xFF (SKK_CODE_ESC) is above every real code, so anything off the
 *     table sorts last and is resolved by the full byte compare.
 *
 * Contents: ASCII '!'..'~' except ';' (93 — ';' cannot appear because it
 * opens an annotation), 、。「」, hiragana ぁ..ん except ゔ, ゛, ー, ：.
 * 182 <= 254, so the two reserved codes still fit and eight characters
 * still pack into a uint64.
 *
 * DO NOT reorder or "tidy" this array. It is hashed into every image;
 * changing it invalidates them all (loudly — SKK_ERR_ALPHABET — which is
 * the point). tools/skk_prep.py must reproduce it byte for byte; the
 * expected stamp is documented at skk_alphabet_crc32(). */

#define SKK_ALPHA_N 182

static const uint16_t s_alpha_cp[SKK_ALPHA_N] = {
    0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029,
    0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032,
    0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003A, 0x003C,
    0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045,
    0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E,
    0x004F, 0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F, 0x0060,
    0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069,
    0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072,
    0x0073, 0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B,
    0x007C, 0x007D, 0x007E, 0x3001, 0x3002, 0x300C, 0x300D, 0x3041, 0x3042,
    0x3043, 0x3044, 0x3045, 0x3046, 0x3047, 0x3048, 0x3049, 0x304A, 0x304B,
    0x304C, 0x304D, 0x304E, 0x304F, 0x3050, 0x3051, 0x3052, 0x3053, 0x3054,
    0x3055, 0x3056, 0x3057, 0x3058, 0x3059, 0x305A, 0x305B, 0x305C, 0x305D,
    0x305E, 0x305F, 0x3060, 0x3061, 0x3062, 0x3063, 0x3064, 0x3065, 0x3066,
    0x3067, 0x3068, 0x3069, 0x306A, 0x306B, 0x306C, 0x306D, 0x306E, 0x306F,
    0x3070, 0x3071, 0x3072, 0x3073, 0x3074, 0x3075, 0x3076, 0x3077, 0x3078,
    0x3079, 0x307A, 0x307B, 0x307C, 0x307D, 0x307E, 0x307F, 0x3080, 0x3081,
    0x3082, 0x3083, 0x3084, 0x3085, 0x3086, 0x3087, 0x3088, 0x3089, 0x308A,
    0x308B, 0x308C, 0x308D, 0x308F, 0x3090, 0x3091, 0x3092, 0x3093, 0x309B,
    0x30FC, 0xFF1A
};

uint8_t skk_char_code(uint32_t cp)
{
    uint32_t lo = 0, hi = SKK_ALPHA_N;

    /* Every entry fits in 16 bits, so anything larger is off the table
       without touching it. */
    if (cp > 0xFFFFu) {
        return SKK_CODE_ESC;
    }
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (s_alpha_cp[mid] < (uint16_t)cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < SKK_ALPHA_N && s_alpha_cp[lo] == (uint16_t)cp) {
        return (uint8_t)(lo + 1);          /* 1..182; never SKK_CODE_END */
    }
    return SKK_CODE_ESC;
}

/* CRC of the table, serialised as the UTF-8 encoding of its characters in
 * code order — i.e. of the table read as one string. Serialising
 * explicitly rather than hashing the array's storage keeps the stamp
 * independent of this file's uint16 packing and of host endianness, and
 * UTF-8 (rather than, say, 4-byte code points) is chosen because it is
 * what tools/skk_prep.py reproduces in one expression:
 *
 *     zlib.crc32(ALPHABET.encode("utf-8"))
 *
 * EXPECTED VALUE FOR THE 182-CHARACTER TABLE ABOVE: 0xE4EF637F, verified
 * against tools/skk_prep.py by tools/test_skk_dict.c, which opens the
 * images that tool actually writes.
 * If the prep tool computes anything else, the two disagree about
 * ordering and every image it writes would be silently wrong — which is
 * exactly what skk_dict_open() refuses with SKK_ERR_ALPHABET. */
uint32_t skk_alphabet_crc32(void)
{
    uint32_t crc = 0;
    uint32_t i;

    for (i = 0; i < SKK_ALPHA_N; i++) {
        uint32_t cp = s_alpha_cp[i];
        uint8_t  u8[3];
        size_t   n;

        /* Every entry is <= U+FFFF and none is a surrogate, so one to
           three bytes and no special cases. */
        if (cp < 0x80u) {
            u8[0] = (uint8_t)cp;
            n = 1;
        } else if (cp < 0x800u) {
            u8[0] = (uint8_t)(0xC0u | (cp >> 6));
            u8[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
            n = 2;
        } else {
            u8[0] = (uint8_t)(0xE0u | (cp >> 12));
            u8[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
            u8[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
            n = 3;
        }
        crc = skk_crc32(crc, u8, n);
    }
    return crc;
}

/* ================================================================== */
/* Packed key */

uint64_t skk_pack(const char *reading, size_t len, bool *out_escaped)
{
    uint64_t key = 0;
    bool escaped = false;
    size_t i = 0;
    int n = 0;

    if (out_escaped) {
        *out_escaped = false;
    }
    if (!reading) {
        return 0;
    }
    while (i < len && n < SKK_PACK_CHARS) {
        size_t adv = 0;
        uint32_t cp = skk_utf8_decode(reading, len, i, &adv);
        uint8_t code = skk_char_code(cp);

        if (adv == 0) {
            adv = 1;                      /* never stall on a bad decoder */
        }
        if (code == SKK_CODE_ESC) {
            escaped = true;
        }
        key |= (uint64_t)code << (56 - 8 * n);
        i += adv;
        n++;
    }
    if (out_escaped) {
        *out_escaped = escaped;
    }
    return key;
}

int skk_entry_cmp(uint64_t ka, const char *a, size_t alen,
                  uint64_t kb, const char *b, size_t blen)
{
    size_t n;
    int r;

    if (ka != kb) {
        return (ka < kb) ? -1 : 1;
    }
    /* Same first eight characters (0.25% of M, 3.19% of L): fall back to
       the bytes. This is the only place the dictionary text is touched
       during a search, and it happens once or twice per lookup. */
    n = (alen < blen) ? alen : blen;
    if (n > 0 && a && b) {
        r = memcmp(a, b, n);
        if (r != 0) {
            return (r < 0) ? -1 : 1;
        }
    }
    if (alen != blen) {
        return (alen < blen) ? -1 : 1;
    }
    return 0;
}

/* ================================================================== */
/* Entry-line parsing.
 *
 * A line is  "<reading> /<cand>/<cand>/.../"  and a candidate may carry
 * a ";annotation". v1 scope (skk_core.h): the annotation is stripped, a
 * "(concat ...)" form is skipped, an "[okuri/c1/c2/]" group is skipped
 * whole, empties are skipped. Measured over M and L: no candidate token
 * is ever empty in the middle and no annotation contains a raw '/', so
 * splitting on '/' is safe; L has 9 concat forms and no okuri groups,
 * but personal dictionaries do use groups, so both are handled. */

typedef struct {
    const char *reading;
    size_t      reading_len;
    const char *body;        /* from the first '/' to the end of line */
    size_t      body_len;
} skk_line_t;

/* Split one line (no trailing '\n') into reading and candidate body.
   Returns 0 when the line is a comment, blank, or has no reading/body
   separator — callers treat that as "not an entry". */
static int line_split(const char *ls, size_t llen, skk_line_t *out)
{
    const char *sp;

    if (llen > 0 && ls[llen - 1] == '\r') {
        llen--;                        /* a CRLF file must not poison the
                                          last candidate of every line */
    }
    if (llen == 0 || ls[0] == ';') {
        return 0;
    }
    sp = (const char *)memchr(ls, ' ', llen);
    if (!sp) {
        return 0;
    }
    out->reading     = ls;
    out->reading_len = (size_t)(sp - ls);
    out->body        = sp + 1;
    out->body_len    = llen - out->reading_len - 1;
    return (out->reading_len > 0);
}

/* Locate the line starting at image offset `off`, which must lie inside
   the dictionary's text section. */
static int line_at(const skk_dict_t *d, uint32_t off, skk_line_t *out)
{
    size_t lo = (size_t)((const uint8_t *)d->text - d->image.base);
    size_t hi = lo + d->text_len;
    const char *ls;
    const char *nl;
    size_t llen;

    if ((size_t)off < lo || (size_t)off >= hi) {
        return SKK_ERR_FORMAT;
    }
    ls = (const char *)d->image.base + off;
    nl = (const char *)memchr(ls, '\n', hi - (size_t)off);
    llen = nl ? (size_t)(nl - ls) : (hi - (size_t)off);
    return line_split(ls, llen, out) ? SKK_OK : SKK_ERR_FORMAT;
}

typedef struct {
    const char *p;
    const char *end;
    int         in_group;
} cand_iter_t;

static void cand_iter_init(cand_iter_t *it, const skk_line_t *ln)
{
    it->p        = ln->body;
    it->end      = ln->body + ln->body_len;
    it->in_group = 0;
}

/* Next usable candidate. Returns 1 and fills txt/len, or 0 at the end.
   Everything skipped bumps *dropped, so skk_stats_t.dropped reports what
   v1 could not represent rather than pretending the entry was shorter. */
static int cand_next(cand_iter_t *it, const char **txt, size_t *len,
                     uint32_t *dropped)
{
    while (it->p < it->end) {
        const char *tok;
        const char *slash;
        const char *semi;
        size_t tlen;

        if (*it->p != '/') {
            return 0;                    /* malformed body; stop quietly */
        }
        it->p++;
        tok = it->p;
        slash = (const char *)memchr(tok, '/', (size_t)(it->end - tok));
        tlen = slash ? (size_t)(slash - tok) : (size_t)(it->end - tok);
        it->p = slash ? slash : it->end;

        if (tlen == 0) {
            continue;                    /* the trailing '/' of the line */
        }
        if (it->in_group) {
            if (tok[0] == ']') {
                it->in_group = 0;
            }
            if (dropped) { (*dropped)++; }
            continue;
        }
        if (tok[0] == '[') {             /* "[okuri/c1/c2/]" — S7 work */
            it->in_group = 1;
            if (dropped) { (*dropped)++; }
            continue;
        }
        if (tok[0] == '(') {             /* "(concat ...)" needs unescaping */
            if (dropped) { (*dropped)++; }
            continue;
        }
        semi = (const char *)memchr(tok, ';', tlen);
        if (semi) {
            tlen = (size_t)(semi - tok); /* annotation: not displayed in v1 */
        }
        if (tlen == 0 || tlen > 0xFFFFu) {
            if (dropped) { (*dropped)++; }
            continue;
        }
        *txt = tok;
        *len = tlen;
        return 1;
    }
    return 0;
}

/* ================================================================== */
/* Opening an image */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* section [off, off + n*elem) inside the image, with `align` honoured */
static int sect_ok(uint32_t off, uint32_t count, uint32_t elem,
                   uint32_t align, uint32_t image_len)
{
    uint64_t end = (uint64_t)off + (uint64_t)count * (uint64_t)elem;

    /* An empty section occupies nothing, but its OFFSET is still stored
       and still gets turned into a pointer by the caller. Returning early
       on count == 0 let an arbitrary uint32 through untouched — for the
       text section (which is sized by bytes, so count==0 means an empty
       dictionary) that produced d->text = base + <whatever the header
       said>. Check the offset itself in every case; only the extent is
       conditional. */
    if (off < sizeof(skk_image_hdr_t) || off > image_len ||
        (off & (align - 1u)) != 0) {
        return 0;
    }
    if (count == 0) {
        return 1;                        /* in range, and needs no extent */
    }
    return end <= (uint64_t)image_len;
}

int skk_dict_open(skk_dict_t *d, skk_blob_t image, uint32_t flags)
{
    skk_image_hdr_t h;
    const uint8_t *b = image.base;

    if (!d || !b || image.len < sizeof(skk_image_hdr_t)) {
        return SKK_ERR_ARG;
    }
    /* The uint64 key arrays are read in place, so an unaligned base is a
       trap on riscv32, not a slowdown. Refuse rather than fault: if
       EMBED_FILES ever gives only 4 bytes, ship the image through a
       generated .c holding a static const uint64_t[] (skk_core.h). */
    if (((uintptr_t)b & (SKK_IMAGE_ALIGN - 1u)) != 0) {
        return SKK_ERR_ALIGN;
    }

    h.magic          = rd_u32(b + 0);
    h.version        = rd_u16(b + 4);
    h.flags          = rd_u16(b + 6);
    h.image_len      = rd_u32(b + 8);
    h.payload_crc32  = rd_u32(b + 12);
    h.alphabet_crc32 = rd_u32(b + 16);
    h.text_off       = rd_u32(b + 20);
    h.text_len       = rd_u32(b + 24);
    h.nasi_count     = rd_u32(b + 28);
    h.nasi_keys_off  = rd_u32(b + 32);
    h.nasi_offs_off  = rd_u32(b + 36);
    h.ari_count      = rd_u32(b + 40);
    h.ari_keys_off   = rd_u32(b + 44);
    h.ari_offs_off   = rd_u32(b + 48);

    if (h.magic != SKK_IMAGE_MAGIC) {
        return SKK_ERR_MAGIC;
    }
    if (h.version != SKK_IMAGE_VERSION) {
        return SKK_ERR_VERSION;
    }
    /* image_len is authoritative and may be shorter than the blob: an
       esp_partition_mmap() window is page-rounded, and a partition is
       almost never exactly the size of what was flashed into it. */
    if (h.image_len < sizeof(skk_image_hdr_t) || h.image_len > image.len) {
        return SKK_ERR_TRUNCATED;
    }
    if (h.alphabet_crc32 != skk_alphabet_crc32()) {
        return SKK_ERR_ALPHABET;
    }
    if (!sect_ok(h.text_off, h.text_len, 1, 1, h.image_len) ||
        !sect_ok(h.nasi_keys_off, h.nasi_count, 8, 8, h.image_len) ||
        !sect_ok(h.nasi_offs_off, h.nasi_count, 4, 4, h.image_len) ||
        !sect_ok(h.ari_keys_off,  h.ari_count,  8, 8, h.image_len) ||
        !sect_ok(h.ari_offs_off,  h.ari_count,  4, 4, h.image_len)) {
        return SKK_ERR_TRUNCATED;
    }
    if (h.text_len == 0 && (h.nasi_count || h.ari_count)) {
        return SKK_ERR_TRUNCATED;
    }
    if ((flags & SKK_OPEN_VERIFY) != 0) {
        uint32_t crc = skk_crc32(0, b + sizeof(skk_image_hdr_t),
                                 h.image_len - sizeof(skk_image_hdr_t));
        if (crc != h.payload_crc32) {
            return SKK_ERR_CRC;
        }
    }

    d->image.base = b;
    d->image.len  = h.image_len;
    d->text       = b + h.text_off;
    d->text_len   = h.text_len;

    d->blk[SKK_BLK_NASI].keys  = (const uint64_t *)(const void *)(b + h.nasi_keys_off);
    d->blk[SKK_BLK_NASI].offs  = (const uint32_t *)(const void *)(b + h.nasi_offs_off);
    d->blk[SKK_BLK_NASI].count = h.nasi_count;
    d->blk[SKK_BLK_ARI].keys   = (const uint64_t *)(const void *)(b + h.ari_keys_off);
    d->blk[SKK_BLK_ARI].offs   = (const uint32_t *)(const void *)(b + h.ari_offs_off);
    d->blk[SKK_BLK_ARI].count  = h.ari_count;

    if (h.nasi_count == 0) {
        d->blk[SKK_BLK_NASI].keys = NULL;
        d->blk[SKK_BLK_NASI].offs = NULL;
    }
    if (h.ari_count == 0) {
        d->blk[SKK_BLK_ARI].keys = NULL;
        d->blk[SKK_BLK_ARI].offs = NULL;
    }
    return SKK_OK;
}

/* ================================================================== */
/* Search.
 *
 * Both blocks ascend — SKK-JISYO stores okuri-ari descending but the
 * index is ours, so the prep tool re-sorts it (skk_core.h). One
 * comparator, one direction, one less way to be subtly wrong.
 *
 * The binary search touches only the uint64 key array: no dependent load
 * of an offset and then of a line, which is what halves the cache misses
 * per probe. The dictionary text is read only when a packed key matches,
 * i.e. once or twice per lookup. */

static uint32_t key_lower_bound(const skk_index_t *ix, uint64_t key,
                                uint32_t *probes)
{
    uint32_t lo = 0, hi = ix->count;

    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (probes) { (*probes)++; }
        if (ix->keys[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static int find_entry(const skk_dict_t *d, skk_blk_t blk,
                      const char *reading, size_t len,
                      skk_line_t *out, skk_stats_t *st)
{
    const skk_index_t *ix = &d->blk[blk];
    uint64_t key;
    uint32_t i;

    if (!ix->keys || !ix->offs || ix->count == 0) {
        return 0;
    }
    key = skk_pack(reading, len, NULL);
    i = key_lower_bound(ix, key, st ? &st->probes : NULL);

    /* Walk the run of identical packed keys (at most 22 entries even in
       L) doing the full byte compare. Sorted, so a positive compare means
       we are past where the reading would have been. */
    for (; i < ix->count && ix->keys[i] == key; i++) {
        skk_line_t ln;
        int c;

        if (line_at(d, ix->offs[i], &ln) != SKK_OK) {
            continue;                     /* corrupt offset: skip, do not fault */
        }
        if (st) { st->fullcmp++; }
        c = skk_entry_cmp(key, reading, len, key, ln.reading, ln.reading_len);
        if (c == 0) {
            *out = ln;
            return 1;
        }
        if (c < 0) {
            break;
        }
    }
    return 0;
}

/* TAB completion: the nth reading starting with `prefix`.
 *
 * The matches are contiguous, because the index is sorted by the same
 * comparator that orders "prefix" before "prefix + anything". So this is
 * one binary search plus a forward walk, and the walk stops at nth
 * rather than enumerating the run.
 *
 * Two wrinkles the obvious loop gets wrong:
 *
 *  - lower_bound() can land slightly BEFORE the run when the prefix is
 *    longer than the 8 characters a packed key holds: entries sharing
 *    those 8 characters compare equal on the key and may still sort
 *    below the prefix. Breaking on the first non-match would then miss
 *    everything. Hence in_run: before the first match, keep walking
 *    while the packed key is unchanged (a collision group is at most 22
 *    entries even in L); after it, the first non-match ends the run.
 *  - an entry EQUAL to the prefix is not a completion of it, so it is
 *    skipped rather than offered back to the user unchanged.
 */
int skk_complete(const skk_dict_t *d, skk_blk_t blk,
                 const char *prefix, size_t plen, size_t nth,
                 const char **out, size_t *out_len, uint32_t *probes)
{
    const skk_index_t *ix;
    uint64_t key;
    uint32_t i;
    size_t seen = 0;
    int in_run = 0;

    if (!d || !prefix || plen == 0 || !out || !out_len ||
        (unsigned)blk >= SKK_BLK_COUNT) {
        return SKK_ERR_ARG;
    }
    ix = &d->blk[blk];
    if (!ix->keys || !ix->offs || ix->count == 0) {
        return 0;
    }

    key = skk_pack(prefix, plen, NULL);
    i = key_lower_bound(ix, key, probes);

    for (; i < ix->count; i++) {
        skk_line_t ln;
        int match;

        if (line_at(d, ix->offs[i], &ln) != SKK_OK) {
            continue;                 /* corrupt offset: skip, do not fault */
        }
        match = (ln.reading_len >= plen &&
                 memcmp(ln.reading, prefix, plen) == 0);
        if (!match) {
            if (in_run || ix->keys[i] != key) {
                break;
            }
            continue;                 /* still inside the collision group */
        }
        in_run = 1;
        if (ln.reading_len == plen) {
            continue;                 /* the prefix itself is not a completion */
        }
        if (seen++ == nth) {
            *out     = ln.reading;
            *out_len = ln.reading_len;
            return 1;
        }
    }
    return 0;
}

/* skk_lookup() with the counters the state machine wants. skk_core.c
   calls this so skk_stats_t.probes/fullcmp/cands/dropped are filled in
   without duplicating the search; `st` may be NULL. */
int skk_lookup_stats(const skk_dict_t *d, skk_blk_t blk,
                     const char *reading, size_t len,
                     skk_cand_t *out, size_t cap, size_t *out_n,
                     skk_stats_t *st)
{
    skk_line_t ln;
    cand_iter_t it;
    const char *txt;
    size_t tlen;
    size_t n = 0;

    if (out_n) {
        *out_n = 0;
    }
    if (!d || !d->image.base || !reading || (cap > 0 && !out)) {
        return SKK_ERR_ARG;
    }
    if ((unsigned)blk >= (unsigned)SKK_BLK_COUNT) {
        return SKK_ERR_ARG;
    }
    if (st) { st->lookups++; }
    if (len == 0 || !find_entry(d, blk, reading, len, &ln, st)) {
        return SKK_OK;                    /* no match is not an error */
    }

    cand_iter_init(&it, &ln);
    while (cand_next(&it, &txt, &tlen, st ? &st->dropped : NULL)) {
        if (n >= cap) {
            /* The tail is dropped rather than failing the call: a caller
               with a small array still gets the first candidates, which
               are the ones a user ever sees. */
            if (st) { st->dropped++; }
            continue;
        }
        out[n].off = (uint32_t)((const uint8_t *)txt - d->image.base);
        out[n].len = (uint16_t)tlen;
        out[n].src = (uint16_t)SKK_SRC_DICT;
        n++;
        if (st) { st->cands++; }
    }
    if (out_n) {
        *out_n = n;
    }
    return SKK_OK;
}

int skk_lookup(const skk_dict_t *d, skk_blk_t blk,
               const char *reading, size_t len,
               skk_cand_t *out, size_t cap, size_t *out_n)
{
    return skk_lookup_stats(d, blk, reading, len, out, cap, out_n, NULL);
}

const char *skk_cand_text(const skk_dict_t *d, const skk_cand_t *c, size_t *len)
{
    if (len) {
        *len = 0;
    }
    if (!d || !c || !d->image.base || c->src != (uint16_t)SKK_SRC_DICT) {
        return NULL;
    }
    if ((uint64_t)c->off + (uint64_t)c->len > (uint64_t)d->image.len) {
        return NULL;
    }
    if (len) {
        *len = c->len;
    }
    return (const char *)d->image.base + c->off;
}

/* ================================================================== */
/* Index construction and verification */

int skk_index_build(skk_blob_t text, uint64_t *keys, uint32_t *offs,
                    size_t cap, size_t *out_count)
{
    size_t i = 0;
    size_t n = 0;

    if (out_count) {
        *out_count = 0;
    }
    if (!text.base && text.len > 0) {
        return SKK_ERR_ARG;
    }
    if (cap > 0 && (!keys || !offs)) {
        return SKK_ERR_ARG;
    }
    while (i < text.len) {
        const char *ls = (const char *)text.base + i;
        const char *nl = (const char *)memchr(ls, '\n', text.len - i);
        size_t llen = nl ? (size_t)(nl - ls) : (text.len - i);
        skk_line_t ln;

        if (line_split(ls, llen, &ln)) {
            if (n < cap) {
                keys[n] = skk_pack(ln.reading, ln.reading_len, NULL);
                offs[n] = (uint32_t)i;
            }
            n++;
        }
        i += llen + (nl ? 1u : 0u);
    }
    if (out_count) {
        *out_count = n;
    }
    /* The true count is always reported, so a caller can size an array in
       one pass and fill it in a second. */
    return (n > cap) ? SKK_ERR_NOSPACE : SKK_OK;
}

/* Reading of the line at `off`, where [lo, hi) is the valid text window
   inside `base`. */
static int reading_at(const uint8_t *base, size_t lo, size_t hi, uint32_t off,
                      const char **rd, size_t *rdlen)
{
    const char *ls;
    const char *nl;
    size_t llen;
    skk_line_t ln;

    if ((size_t)off < lo || (size_t)off >= hi) {
        return SKK_ERR_FORMAT;
    }
    ls = (const char *)base + off;
    nl = (const char *)memchr(ls, '\n', hi - (size_t)off);
    llen = nl ? (size_t)(nl - ls) : (hi - (size_t)off);
    if (!line_split(ls, llen, &ln)) {
        return SKK_ERR_FORMAT;
    }
    *rd = ln.reading;
    *rdlen = ln.reading_len;
    return SKK_OK;
}

/* The check that catches the EUC-JP -> UTF-8 reordering.
 *
 * Three things are verified per entry, because all three are ways for a
 * generator and this engine to disagree without crashing:
 *   1. the offset points at a parsable entry line,
 *   2. keys[i] really is skk_pack() of that line's reading (a stale or
 *      differently-tabled generator fails here),
 *   3. the entry is not less than its predecessor under skk_entry_cmp().
 * Equal neighbours are tolerated; duplicate readings are the prep tool's
 * problem, not a search hazard. */
static int index_check(const uint8_t *base, size_t lo, size_t hi,
                       const uint64_t *keys, const uint32_t *offs,
                       size_t n, uint32_t *out_at)
{
    const char *prev = NULL;
    size_t prev_len = 0;
    uint64_t prev_key = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        const char *rd;
        size_t rdlen;

        if (reading_at(base, lo, hi, offs[i], &rd, &rdlen) != SKK_OK) {
            if (out_at) { *out_at = (uint32_t)i; }
            return SKK_ERR_FORMAT;
        }
        if (keys[i] != skk_pack(rd, rdlen, NULL)) {
            if (out_at) { *out_at = (uint32_t)i; }
            return SKK_ERR_FORMAT;
        }
        if (i > 0 &&
            skk_entry_cmp(prev_key, prev, prev_len, keys[i], rd, rdlen) > 0) {
            if (out_at) { *out_at = (uint32_t)i; }
            return SKK_ERR_FORMAT;
        }
        prev     = rd;
        prev_len = rdlen;
        prev_key = keys[i];
    }
    return SKK_OK;
}

int skk_index_verify(const skk_dict_t *d, skk_blk_t blk, uint32_t *out_at)
{
    const skk_index_t *ix;
    size_t lo;

    if (out_at) {
        *out_at = 0;
    }
    if (!d || !d->image.base || (unsigned)blk >= (unsigned)SKK_BLK_COUNT) {
        return SKK_ERR_ARG;
    }
    ix = &d->blk[blk];
    if (ix->count == 0) {
        return SKK_OK;
    }
    if (!ix->keys || !ix->offs) {
        return SKK_ERR_ARG;
    }
    lo = (size_t)((const uint8_t *)d->text - d->image.base);
    return index_check(d->image.base, lo, lo + d->text_len,
                       ix->keys, ix->offs, ix->count, out_at);
}

/* Same check before an image exists, i.e. straight on the arrays that
   skk_index_build() filled and the prep tool has just sorted. Offsets are
   relative to text.base. This is what a host test points at a raw
   SKK-JISYO.*.utf8 to prove the EUC-JP order is broken (M: fails at the
   entry after ぺーじ; L: 285 such places). */
int skk_index_check(skk_blob_t text, const uint64_t *keys, const uint32_t *offs,
                    size_t n, uint32_t *out_at)
{
    if (out_at) {
        *out_at = 0;
    }
    if (n == 0) {
        return SKK_OK;
    }
    if (!text.base || !keys || !offs) {
        return SKK_ERR_ARG;
    }
    return index_check(text.base, 0, text.len, keys, offs, n, out_at);
}

/* Locate the two blocks in a raw SKK-JISYO by its marker lines. The
   caller (tools/skk_prep.py's C counterpart, and the host test) needs
   this before it can call skk_index_build() per block; comment lines
   left inside a span are ignored by the builder. Either span may be
   empty, but both markers must be present — a file without them is not a
   SKK dictionary and guessing would be worse than refusing. */
int skk_text_split(skk_blob_t text, skk_blob_t *out_ari, skk_blob_t *out_nasi)
{
    static const char k_ari[]  = ";; okuri-ari entries.";
    static const char k_nasi[] = ";; okuri-nasi entries.";
    size_t i = 0;
    size_t ari_start = 0, nasi_start = 0;
    size_t ari_marker = 0, nasi_marker = 0;
    int have_ari = 0, have_nasi = 0;

    if (!text.base || !out_ari || !out_nasi) {
        return SKK_ERR_ARG;
    }
    while (i < text.len) {
        const char *ls = (const char *)text.base + i;
        const char *nl = (const char *)memchr(ls, '\n', text.len - i);
        size_t llen = nl ? (size_t)(nl - ls) : (text.len - i);
        size_t next = i + llen + (nl ? 1u : 0u);

        if (!have_ari && llen >= sizeof(k_ari) - 1 &&
            memcmp(ls, k_ari, sizeof(k_ari) - 1) == 0) {
            have_ari = 1;
            ari_marker = i;
            ari_start = next;
        } else if (!have_nasi && llen >= sizeof(k_nasi) - 1 &&
                   memcmp(ls, k_nasi, sizeof(k_nasi) - 1) == 0) {
            have_nasi = 1;
            nasi_marker = i;
            nasi_start = next;
        }
        if (have_ari && have_nasi) {
            break;
        }
        i = next;
    }
    if (!have_ari || !have_nasi) {
        return SKK_ERR_FORMAT;
    }
    /* Each block runs from just after its own marker to whichever marker
       comes next, or to the end of the text. Standard SKK-JISYO puts
       okuri-ari first and nothing in this project writes it any other
       way, but the format does not require that order — assuming it made
       a reversed file slice into one empty block and one that swallowed
       the other, silently, with no parse error.

       Compare the two MARKER positions, not a marker against the other
       block's start: when a block is empty its start coincides with the
       next marker, so "nasi_marker > ari_start" is false both when nasi
       comes first AND when the ari block is merely empty — and the empty
       case would then take the whole rest of the text. */
    out_ari->base  = text.base + ari_start;
    out_ari->len   = (nasi_marker > ari_marker) ? (nasi_marker - ari_start)
                                                : (text.len - ari_start);
    out_nasi->base = text.base + nasi_start;
    out_nasi->len  = (ari_marker > nasi_marker) ? (ari_marker - nasi_start)
                                                : (text.len - nasi_start);
    return SKK_OK;
}

/* ================================================================== */
/* Personal dictionary — MRU ordering.
 *
 * Pure logic, as the rest of the file: learning reorders an array, and
 * persistence is two functions that render to and parse from a byte
 * buffer. Reading and writing the littlefs file is the caller's job (S7),
 * which keeps skk_core free of I/O and lets the host test exercise the
 * whole round trip.
 *
 * Records are fixed size and stored newest first, so "move to front" is
 * one memmove of at most 4.7 KB and nothing allocates or fragments. A
 * reading or candidate too long to fit is simply not learned — better
 * than an arena that can fill up and needs compaction.
 *
 * NOTE FOR THE HEADER OWNER: the declarations below belong in
 * skk_core.h. They are guarded so that moving them there is a no-op for
 * this file — define SKK_MRU_DECLARED alongside them. */

#ifndef SKK_MRU_DECLARED
#define SKK_MRU_DECLARED

#define SKK_MRU_ENTRIES     48
#define SKK_MRU_READING_MAX 48
#define SKK_MRU_CAND_MAX    48
/* Worst case of skk_mru_save(): header + per record "N " + reading +
   " /" + candidate + "/\n". */
#define SKK_MRU_SAVE_MAX    (32 + SKK_MRU_ENTRIES * \
                             (SKK_MRU_READING_MAX + SKK_MRU_CAND_MAX + 6))

typedef struct {
    uint8_t blk;             /* skk_blk_t */
    uint8_t reading_len;
    uint8_t cand_len;
    uint8_t pad;
    char    reading[SKK_MRU_READING_MAX];
    char    cand[SKK_MRU_CAND_MAX];
} skk_mru_rec_t;

typedef struct {
    uint32_t      n;
    skk_mru_rec_t rec[SKK_MRU_ENTRIES];   /* newest first */
} skk_mru_t;

void skk_mru_init(skk_mru_t *m);
int  skk_mru_note(skk_mru_t *m, skk_blk_t blk,
                  const char *reading, size_t rlen,
                  const char *cand, size_t clen);
int  skk_mru_apply(const skk_mru_t *m, const skk_dict_t *d, skk_blk_t blk,
                   const char *reading, size_t rlen,
                   skk_cand_t *cands, size_t n, size_t cap, size_t *out_n);
const char *skk_mru_text(const skk_mru_t *m, const skk_cand_t *c, size_t *len);
int  skk_mru_save(const skk_mru_t *m, char *buf, size_t cap, size_t *out_len);
int  skk_mru_load(skk_mru_t *m, const char *buf, size_t len);

#endif /* SKK_MRU_DECLARED */

void skk_mru_init(skk_mru_t *m)
{
    if (m) {
        memset(m, 0, sizeof(*m));
    }
}

static int mru_same(const skk_mru_rec_t *r, skk_blk_t blk,
                    const char *reading, size_t rlen,
                    const char *cand, size_t clen)
{
    return r->blk == (uint8_t)blk &&
           r->reading_len == rlen && r->cand_len == clen &&
           memcmp(r->reading, reading, rlen) == 0 &&
           memcmp(r->cand, cand, clen) == 0;
}

static void mru_to_front(skk_mru_t *m, uint32_t i)
{
    skk_mru_rec_t tmp;

    if (i == 0) {
        return;
    }
    tmp = m->rec[i];
    memmove(&m->rec[1], &m->rec[0], (size_t)i * sizeof(m->rec[0]));
    m->rec[0] = tmp;
}

int skk_mru_note(skk_mru_t *m, skk_blk_t blk,
                 const char *reading, size_t rlen,
                 const char *cand, size_t clen)
{
    uint32_t i;

    if (!m || !reading || !cand || rlen == 0 || clen == 0) {
        return SKK_ERR_ARG;
    }
    if ((unsigned)blk >= (unsigned)SKK_BLK_COUNT) {
        return SKK_ERR_ARG;
    }
    if (rlen > SKK_MRU_READING_MAX || clen > SKK_MRU_CAND_MAX) {
        return SKK_ERR_NOSPACE;          /* too long to learn; not fatal */
    }
    for (i = 0; i < m->n; i++) {
        if (mru_same(&m->rec[i], blk, reading, rlen, cand, clen)) {
            mru_to_front(m, i);
            return SKK_OK;
        }
    }
    if (m->n < SKK_MRU_ENTRIES) {
        m->n++;
    }
    memmove(&m->rec[1], &m->rec[0], (size_t)(m->n - 1) * sizeof(m->rec[0]));
    memset(&m->rec[0], 0, sizeof(m->rec[0]));
    m->rec[0].blk         = (uint8_t)blk;
    m->rec[0].reading_len = (uint8_t)rlen;
    m->rec[0].cand_len    = (uint8_t)clen;
    memcpy(m->rec[0].reading, reading, rlen);
    memcpy(m->rec[0].cand, cand, clen);
    return SKK_OK;
}

const char *skk_mru_text(const skk_mru_t *m, const skk_cand_t *c, size_t *len)
{
    if (len) {
        *len = 0;
    }
    if (!m || !c || c->src != (uint16_t)SKK_SRC_USER) {
        return NULL;
    }
    if ((size_t)c->off + (size_t)c->len > sizeof(*m)) {
        return NULL;
    }
    if (len) {
        *len = c->len;
    }
    return (const char *)m + c->off;
}

static uint32_t mru_cand_off(const skk_mru_t *m, uint32_t i)
{
    (void)m;
    return (uint32_t)(offsetof(skk_mru_t, rec) +
                      (size_t)i * sizeof(skk_mru_rec_t) +
                      offsetof(skk_mru_rec_t, cand));
}

/* Resolve any candidate this module can, for comparison purposes.
   SKK_SRC_SCRATCH lives in a skk_t we cannot see, so it never matches —
   which is correct: a katakana rendering is not a learned dictionary
   candidate. */
static const char *cand_resolve(const skk_dict_t *d, const skk_mru_t *m,
                                const skk_cand_t *c, size_t *len)
{
    if (c->src == (uint16_t)SKK_SRC_DICT) {
        return skk_cand_text(d, c, len);
    }
    if (c->src == (uint16_t)SKK_SRC_USER) {
        return skk_mru_text(m, c, len);
    }
    if (len) {
        *len = 0;
    }
    return NULL;
}

int skk_mru_apply(const skk_mru_t *m, const skk_dict_t *d, skk_blk_t blk,
                  const char *reading, size_t rlen,
                  skk_cand_t *cands, size_t n, size_t cap, size_t *out_n)
{
    uint32_t i;

    if (out_n) {
        *out_n = n;
    }
    if (!m || !reading || (n > 0 && !cands) || n > cap) {
        return SKK_ERR_ARG;
    }
    if ((unsigned)blk >= (unsigned)SKK_BLK_COUNT) {
        return SKK_ERR_ARG;
    }
    if (rlen == 0 || m->n == 0) {
        return SKK_OK;
    }
    /* Oldest match first, each moved to the front: the newest ends up at
       index 0 and the rest keep their relative recency. */
    for (i = m->n; i-- > 0; ) {
        const skk_mru_rec_t *r = &m->rec[i];
        size_t j;
        int found = 0;

        if (r->blk != (uint8_t)blk || r->reading_len != rlen ||
            memcmp(r->reading, reading, rlen) != 0) {
            continue;
        }
        for (j = 0; j < n; j++) {
            size_t clen = 0;
            const char *ct = cand_resolve(d, m, &cands[j], &clen);

            if (ct && clen == r->cand_len &&
                memcmp(ct, r->cand, clen) == 0) {
                skk_cand_t tmp = cands[j];
                memmove(&cands[1], &cands[0], j * sizeof(cands[0]));
                cands[0] = tmp;
                found = 1;
                break;
            }
        }
        if (!found && n < cap) {
            /* Learned but no longer in the dictionary (or never was):
               keep offering it, sourced from the MRU store itself. */
            memmove(&cands[1], &cands[0], n * sizeof(cands[0]));
            cands[0].off = mru_cand_off(m, i);
            cands[0].len = r->cand_len;
            cands[0].src = (uint16_t)SKK_SRC_USER;
            n++;
        }
    }
    if (out_n) {
        *out_n = n;
    }
    return SKK_OK;
}

/* Serialisation. The text form is deliberately the SKK personal
   dictionary shape, one record per line, newest first:
 *
 *     ;; skk_core mru 1
 *     N かんじ /漢字/
 *     A おくr /送/
 *
 * so a user can read and edit it, and so a future "merge my desktop
 * ~/.skk-jisyo" is parsing, not reverse engineering. Neither a reading
 * nor a candidate can contain a space, '/' or newline (the dictionary
 * format splits on exactly those), so no escaping is needed. */

static void mru_put(char *buf, size_t cap, size_t *pos, const char *s, size_t n)
{
    if (buf && *pos + n <= cap) {
        memcpy(buf + *pos, s, n);
    }
    *pos += n;
}

int skk_mru_save(const skk_mru_t *m, char *buf, size_t cap, size_t *out_len)
{
    static const char k_hdr[] = ";; skk_core mru 1\n";
    size_t pos = 0;
    uint32_t i;

    if (out_len) {
        *out_len = 0;
    }
    if (!m || (cap > 0 && !buf)) {
        return SKK_ERR_ARG;
    }
    mru_put(buf, cap, &pos, k_hdr, sizeof(k_hdr) - 1);
    for (i = 0; i < m->n && i < SKK_MRU_ENTRIES; i++) {
        const skk_mru_rec_t *r = &m->rec[i];

        if (r->reading_len == 0 || r->cand_len == 0) {
            continue;
        }
        mru_put(buf, cap, &pos, (r->blk == (uint8_t)SKK_BLK_ARI) ? "A " : "N ", 2);
        mru_put(buf, cap, &pos, r->reading, r->reading_len);
        mru_put(buf, cap, &pos, " /", 2);
        mru_put(buf, cap, &pos, r->cand, r->cand_len);
        mru_put(buf, cap, &pos, "/\n", 2);
    }
    if (out_len) {
        *out_len = pos;               /* always the needed size */
    }
    return (pos > cap) ? SKK_ERR_NOSPACE : SKK_OK;
}

int skk_mru_load(skk_mru_t *m, const char *buf, size_t len)
{
    size_t i = 0;

    if (!m || (!buf && len > 0)) {
        return SKK_ERR_ARG;
    }
    skk_mru_init(m);
    while (i < len && m->n < SKK_MRU_ENTRIES) {
        const char *ls = buf + i;
        const char *nl = (const char *)memchr(ls, '\n', len - i);
        size_t llen = nl ? (size_t)(nl - ls) : (len - i);
        size_t next = i + llen + (nl ? 1u : 0u);
        skk_line_t ln;
        skk_blk_t blk;
        cand_iter_t it;
        const char *ct;
        size_t clen;

        i = next;
        if (llen < 4 || (ls[0] != 'A' && ls[0] != 'N') || ls[1] != ' ') {
            continue;                  /* comment, blank, or junk: ignore */
        }
        blk = (ls[0] == 'A') ? SKK_BLK_ARI : SKK_BLK_NASI;
        if (!line_split(ls + 2, llen - 2, &ln)) {
            continue;
        }
        cand_iter_init(&it, &ln);
        if (!cand_next(&it, &ct, &clen, NULL)) {
            continue;
        }
        if (ln.reading_len > SKK_MRU_READING_MAX || clen > SKK_MRU_CAND_MAX) {
            continue;
        }
        /* Appended in file order, which is newest first — not through
           skk_mru_note(), whose move-to-front would reverse the file. */
        {
            skk_mru_rec_t *r = &m->rec[m->n];
            memset(r, 0, sizeof(*r));
            r->blk         = (uint8_t)blk;
            r->reading_len = (uint8_t)ln.reading_len;
            r->cand_len    = (uint8_t)clen;
            memcpy(r->reading, ln.reading, ln.reading_len);
            memcpy(r->cand, ct, clen);
            m->n++;
        }
    }
    return SKK_OK;
}
