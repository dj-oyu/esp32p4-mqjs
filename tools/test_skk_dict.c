/*
 * tools/test_skk_dict.c — host unit test for the dictionary half of
 * skk_core (components/skk_core/skk_dict.c), S2 in docs/skk-ime-design.md.
 *
 * The one bug this file exists to prevent: SKK-JISYO ships sorted in
 * EUC-JP byte order, transcoding it to UTF-8 silently reorders it, and a
 * binary search over a mis-sorted array DOES NOT CRASH — it answers wrong
 * for some words on some probe paths (design §6.1, §11). So the tests are
 * built around order, not around "does a lookup work":
 *
 *   1. ORDER PRESERVATION. For every entry of every shipped dictionary,
 *      the packed-key order agrees with the raw UTF-8 byte order. Checked
 *      two ways: monotonicity of the key along a byte-sorted array (which
 *      is equivalent to the pairwise statement, and covers all entries),
 *      and an explicit O(n^2) pairwise sweep on the smaller blocks plus a
 *      large random sample on the big ones.
 *   2. THE MONOTONICITY CHECK REJECTS AN UNSORTED DICTIONARY. The raw
 *      *.utf8 files are fed in as-is; M must be refused (2 violations),
 *      ML (22), L (285), and the descending okuri-ari block of all four.
 *      S has 0 violations in okuri-nasi and must be accepted — that is
 *      the control that proves the check is not simply always failing.
 *   3. ROUND TRIP. Every heading of every dictionary is looked up and
 *      must find itself, with the candidate list the line actually holds.
 *
 * Everything runs through skk_blob_t on malloc+fread memory, which is the
 * point of §6.7: host, PSRAM and mmap are the same code path.
 *
 * skk_dict.c is #included rather than linked so the test can drive the
 * file-static helpers (find_entry, cand_next, reading_at) and so the
 * personal-dictionary types are the real ones rather than a copy that
 * could drift.
 *
 * Build (see README):
 *   gcc -O2 -std=c99 -Wall -Wextra -Icomponents/skk_core/include \
 *       -Icomponents/skk_core tools/test_skk_dict.c \
 *       components/skk_core/skk_kana.c -o /tmp/test_skk_dict
 *   /tmp/test_skk_dict <dir with SKK-JISYO.*.utf8> [dir with skk_dict_*.bin]
 */

#define _POSIX_C_SOURCE 199309L   /* clock_gettime under -std=c99 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "skk_core.h"
#include "skk_dict.c"          /* unit under test, statics and all */

/* ================================================================== */
/* Harness */

static int g_pass, g_fail;
static const char *g_ctx = "";

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (cond) {                                                        \
            g_pass++;                                                      \
        } else {                                                           \
            g_fail++;                                                      \
            printf("  FAIL [%s] %s:%d: ", g_ctx, __FILE__, __LINE__);       \
            printf(__VA_ARGS__);                                           \
            putchar('\n');                                                 \
        }                                                                  \
    } while (0)

/* A failure that makes everything after it meaningless: give up on this
   test and let the rest of the suite run. */
#define REQUIRE(cond, ...)                                                 \
    do {                                                                   \
        CHECK(cond, __VA_ARGS__);                                          \
        if (!(cond)) { return; }                                           \
    } while (0)

static void section(const char *name)
{
    printf("\n== %s\n", name);
    g_ctx = name;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t g_rng = 88172645463325252ull;
static uint64_t rnd64(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

/* ================================================================== */
/* Files */

static uint8_t *load_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *p;

    *out_len = 0;
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    /* +1 so a text file is always safely NUL terminated for printf-style
       debugging; skk_core itself never relies on it. */
    p = (uint8_t *)malloc((size_t)sz + 1);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); fclose(f); return NULL; }
    fclose(f);
    p[sz] = 0;
    *out_len = (size_t)sz;
    return p;
}

/* 8-byte aligned buffer, because skk_dict_open() refuses anything else
   (the uint64 key arrays are read in place — an unaligned base is a trap
   on riscv32, not a slowdown). */
static uint8_t *alloc_aligned(size_t n, void **out_free)
{
    uint8_t *raw = (uint8_t *)malloc(n + 16);
    uintptr_t a;

    *out_free = raw;
    if (!raw) {
        return NULL;
    }
    a = ((uintptr_t)raw + 7u) & ~(uintptr_t)7u;
    return (uint8_t *)a;
}

/* ================================================================== */
/* Entry arrays built straight out of a raw SKK-JISYO */

typedef struct {
    uint64_t    key;
    uint32_t    off;      /* relative to the block blob's base */
    const char *rd;
    uint32_t    rdlen;
} ent_t;

static int cmp_ent(const void *pa, const void *pb)
{
    const ent_t *a = (const ent_t *)pa;
    const ent_t *b = (const ent_t *)pb;
    return skk_entry_cmp(a->key, a->rd, a->rdlen, b->key, b->rd, b->rdlen);
}

/* Raw UTF-8 byte order: memcmp, then shorter first. This is the order the
   packed key claims to reproduce, and it is computed here WITHOUT any of
   skk_dict.c's machinery on purpose — an independent oracle. */
static int cmp_raw_bytes(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t n = (alen < blen) ? alen : blen;
    int r = n ? memcmp(a, b, n) : 0;

    if (r != 0) {
        return (r < 0) ? -1 : 1;
    }
    if (alen != blen) {
        return (alen < blen) ? -1 : 1;
    }
    return 0;
}

static int cmp_ent_bytes(const void *pa, const void *pb)
{
    const ent_t *a = (const ent_t *)pa;
    const ent_t *b = (const ent_t *)pb;
    return cmp_raw_bytes(a->rd, a->rdlen, b->rd, b->rdlen);
}

/* Byte length of the first up-to-SKK_PACK_CHARS characters. */
static size_t prefix8_len(const char *s, size_t len)
{
    size_t i = 0;
    int n = 0;

    while (i < len && n < SKK_PACK_CHARS) {
        i = skk_utf8_next(s, len, i);
        n++;
    }
    return i;
}

typedef struct {
    uint8_t   *raw;
    size_t     raw_len;
    skk_blob_t span[SKK_BLK_COUNT];   /* the two block spans inside raw */
    ent_t     *ent[SKK_BLK_COUNT];
    size_t     n[SKK_BLK_COUNT];
} rawdict_t;

static int rawdict_load(rawdict_t *rd, const char *path)
{
    skk_blob_t whole;
    skk_blob_t ari, nasi;
    int b;

    memset(rd, 0, sizeof(*rd));
    rd->raw = load_file(path, &rd->raw_len);
    if (!rd->raw) {
        return -1;
    }
    whole.base = rd->raw;
    whole.len  = rd->raw_len;
    if (skk_text_split(whole, &ari, &nasi) != SKK_OK) {
        return -1;
    }
    rd->span[SKK_BLK_NASI] = nasi;
    rd->span[SKK_BLK_ARI]  = ari;

    for (b = 0; b < SKK_BLK_COUNT; b++) {
        size_t cnt = 0, cnt2 = 0, i;
        uint64_t *keys;
        uint32_t *offs;
        int rc;

        rc = skk_index_build(rd->span[b], NULL, NULL, 0, &cnt);
        if (rc != SKK_OK && rc != SKK_ERR_NOSPACE) {
            return -1;
        }
        keys = (uint64_t *)malloc((cnt + 1) * sizeof(uint64_t));
        offs = (uint32_t *)malloc((cnt + 1) * sizeof(uint32_t));
        if (!keys || !offs) {
            return -1;
        }
        if (skk_index_build(rd->span[b], keys, offs, cnt, &cnt2) != SKK_OK ||
            cnt2 != cnt) {
            return -1;
        }
        rd->ent[b] = (ent_t *)malloc((cnt + 1) * sizeof(ent_t));
        if (!rd->ent[b]) {
            return -1;
        }
        for (i = 0; i < cnt; i++) {
            const char *r = NULL;
            size_t rlen = 0;

            if (reading_at(rd->span[b].base, 0, rd->span[b].len, offs[i],
                           &r, &rlen) != SKK_OK) {
                return -1;
            }
            rd->ent[b][i].key   = keys[i];
            rd->ent[b][i].off   = offs[i];
            rd->ent[b][i].rd    = r;
            rd->ent[b][i].rdlen = (uint32_t)rlen;
        }
        rd->n[b] = cnt;
        free(keys);
        free(offs);
    }
    return 0;
}

static void rawdict_free(rawdict_t *rd)
{
    int b;
    for (b = 0; b < SKK_BLK_COUNT; b++) {
        free(rd->ent[b]);
    }
    free(rd->raw);
    memset(rd, 0, sizeof(*rd));
}

/* Is `s` really not one of the headings? An independent linear scan, so a
   "must miss" assertion can never be testing a reading the dictionary
   actually contains. */
static int absent_in(const ent_t *e, size_t n, const char *s, size_t len)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (e[i].rdlen == len && memcmp(e[i].rd, s, len) == 0) {
            return 0;
        }
    }
    return 1;
}

/* Order violations under skk_entry_cmp() in the order the entries appear
   in the file — i.e. exactly what §6.1 measured. */
static size_t count_violations(const ent_t *e, size_t n)
{
    size_t i, v = 0;

    for (i = 1; i < n; i++) {
        if (skk_entry_cmp(e[i - 1].key, e[i - 1].rd, e[i - 1].rdlen,
                          e[i].key, e[i].rd, e[i].rdlen) > 0) {
            v++;
        }
    }
    return v;
}

/* ================================================================== */
/* Image assembly.
 *
 * Deliberately an independent writer (the prep tool is Python and lives
 * outside this test): the layout in skk_core.h is implemented here from
 * the documentation, so a disagreement between skk_dict_open() and the
 * documented format shows up as a test failure rather than as two files
 * agreeing on a mistake. The entry text is the whole source file, in its
 * ORIGINAL order — only offs[] defines what the index means (skk_core.h),
 * and testing that explicitly is worth more than re-emitting sorted text. */

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)(v & 0xFFFFFFFFu));
    wr32(p + 4, (uint32_t)(v >> 32));
}

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

/* Build an image from `rd`'s (already sorted) entry arrays. */
static uint8_t *image_build(const rawdict_t *rd, size_t *out_len, void **out_free)
{
    size_t n0 = rd->n[SKK_BLK_NASI], n1 = rd->n[SKK_BLK_ARI];
    size_t f0 = skk_fan_total((uint32_t)n0), f1 = skk_fan_total((uint32_t)n1);
    size_t off = 64;
    size_t nasi_keys_off, nasi_offs_off, ari_keys_off, ari_offs_off, text_off;
    size_t nasi_fan_off, ari_fan_off;
    size_t total, i;
    uint8_t *img;

    nasi_keys_off = off;                 off += n0 * 8;
    nasi_fan_off  = align_up(off, 8);    off = nasi_fan_off + f0 * 8;
    nasi_offs_off = align_up(off, 4);    off = nasi_offs_off + n0 * 4;
    ari_keys_off  = align_up(off, 8);    off = ari_keys_off + n1 * 8;
    ari_fan_off   = align_up(off, 8);    off = ari_fan_off + f1 * 8;
    ari_offs_off  = align_up(off, 4);    off = ari_offs_off + n1 * 4;
    text_off      = off;                 off += rd->raw_len;
    total         = off;

    img = alloc_aligned(total, out_free);
    if (!img) {
        return NULL;
    }
    memset(img, 0, total);
    memcpy(img + text_off, rd->raw, rd->raw_len);

    for (i = 0; i < n0; i++) {
        size_t base = (size_t)(rd->span[SKK_BLK_NASI].base - rd->raw);
        wr64(img + nasi_keys_off + i * 8, rd->ent[SKK_BLK_NASI][i].key);
        wr32(img + nasi_offs_off + i * 4,
             (uint32_t)(text_off + base + rd->ent[SKK_BLK_NASI][i].off));
    }
    for (i = 0; i < n1; i++) {
        size_t base = (size_t)(rd->span[SKK_BLK_ARI].base - rd->raw);
        wr64(img + ari_keys_off + i * 8, rd->ent[SKK_BLK_ARI][i].key);
        wr32(img + ari_offs_off + i * 4,
             (uint32_t)(text_off + base + rd->ent[SKK_BLK_ARI][i].off));
    }

    /* The sampled search tree, from the keys just written. Read back in
       place rather than from rd->ent so this exercises the same bytes the
       engine will: little-endian host, which skk_dict.c already assumes. */
    skk_fan_build((const uint64_t *)(void *)(img + nasi_keys_off), (uint32_t)n0,
                  (uint64_t *)(void *)(img + nasi_fan_off));
    skk_fan_build((const uint64_t *)(void *)(img + ari_keys_off), (uint32_t)n1,
                  (uint64_t *)(void *)(img + ari_fan_off));

    wr32(img + 0,  SKK_IMAGE_MAGIC);
    wr16(img + 4,  (uint16_t)SKK_IMAGE_VERSION);
    wr16(img + 6,  0);
    wr32(img + 8,  (uint32_t)total);
    wr32(img + 12, 0);                       /* payload crc, below */
    wr32(img + 16, skk_alphabet_crc32());
    wr32(img + 20, (uint32_t)text_off);
    wr32(img + 24, (uint32_t)rd->raw_len);
    wr32(img + 28, (uint32_t)n0);
    wr32(img + 32, (uint32_t)nasi_keys_off);
    wr32(img + 36, (uint32_t)nasi_offs_off);
    wr32(img + 40, (uint32_t)n1);
    wr32(img + 44, (uint32_t)ari_keys_off);
    wr32(img + 48, (uint32_t)ari_offs_off);
    wr32(img + 52, f0 ? (uint32_t)nasi_fan_off : 0u);
    wr32(img + 56, f1 ? (uint32_t)ari_fan_off : 0u);
    wr32(img + 12, skk_crc32(0, img + 64, total - 64));

    *out_len = total;
    return img;
}

/* ================================================================== */
/* 1. Primitives */

static void test_crc32(void)
{
    static const char kat[] = "123456789";
    uint32_t c;

    section("crc32 / alphabet stamp");
    /* CRC-32/ISO-HDLC check value, the one every implementation agrees on. */
    CHECK(skk_crc32(0, kat, 9) == 0xCBF43926u,
          "CRC32(\"123456789\") = 0x%08X, want 0xCBF43926",
          skk_crc32(0, kat, 9));
    CHECK(skk_crc32(0, "", 0) == 0u, "CRC32 of nothing must be 0");
    /* Streamable: seeding with the previous result must equal one pass. */
    c = skk_crc32(skk_crc32(0, kat, 4), kat + 4, 5);
    CHECK(c == 0xCBF43926u, "chained CRC32 = 0x%08X", c);
    CHECK(skk_crc32(0, NULL, 10) == 0u, "NULL data returns the seed");

    printf("  alphabet crc32 = 0x%08X\n", skk_alphabet_crc32());
    /* Pinned, because the stamp is what stops a stale generator from
       writing an image that this engine would search in a different
       order. tools/skk_prep.py computes it as
       zlib.crc32(ALPHABET.encode("utf-8")); if either side changes, the
       real images opened further down stop opening. */
    CHECK(skk_alphabet_crc32() == 0xE4EF637Fu,
          "alphabet stamp is 0x%08X, want 0xE4EF637F (== tools/skk_prep.py's "
          "ALPHABET_CRC32)", skk_alphabet_crc32());
}

static void test_alphabet(void)
{
    uint32_t cp;
    int prev = -1;
    int n = 0;

    section("collation table");
    /* The code of a character must be its rank in code-point order, which
       for UTF-8 is byte order — that identity is what makes one uint64
       compare equal a memcmp of the readings. Sweep the whole BMP. */
    for (cp = 1; cp <= 0xFFFFu; cp++) {
        uint8_t code = skk_char_code(cp);

        if (code == SKK_CODE_ESC) {
            continue;
        }
        n++;
        CHECK(code != SKK_CODE_END, "U+%04X mapped to SKK_CODE_END", cp);
        CHECK((int)code > prev, "U+%04X code %u not above previous %d",
              cp, code, prev);
        prev = (int)code;
    }
    CHECK(n == 182, "table has %d characters, design says 182", n);
    CHECK(skk_char_code(0x1F600) == SKK_CODE_ESC, "astral must escape");
    CHECK(skk_char_code(0x3B) == SKK_CODE_ESC, "';' opens an annotation, must escape");
    CHECK(skk_char_code(0x30A2) == SKK_CODE_ESC, "katakana is not a reading char");
    CHECK(skk_char_code(0x4E00) == SKK_CODE_ESC, "kanji is not a reading char");
    CHECK(skk_char_code(0x20) == SKK_CODE_ESC, "space separates reading from body");
    CHECK(skk_char_code(0x2F) != SKK_CODE_ESC, "'/' is in the table");
    CHECK(skk_char_code(0x30FC) != SKK_CODE_ESC, "U+30FC must be in the table");
    CHECK(skk_char_code(0xFF1A) != SKK_CODE_ESC, "U+FF1A must be in the table");
}

static uint64_t packs(const char *s)
{
    return skk_pack(s, strlen(s), NULL);
}

static void test_pack(void)
{
    bool esc = true;
    uint64_t k;

    section("packed key");
    CHECK(packs("") == 0, "the empty reading packs to 0");
    /* A prefix must pack strictly lower — SKK_CODE_END is owned by no
       character, which is what makes this true and what makes the index
       agree with strcmp. */
    CHECK(packs("か") < packs("かん"), "prefix must pack lower");
    CHECK(packs("かん") < packs("かんじ"), "prefix must pack lower");
    CHECK(packs("あ") < packs("い"), "kana order");
    CHECK(packs("!") < packs("あ"), "ASCII sorts before kana in UTF-8");
    /* The bug the whole design is built around. */
    CHECK(packs("ぺい") < packs("ぺーじ"),
          "U+30FC must sort AFTER kana (this is the EUC-JP trap)");
    CHECK(packs("ん") < packs("ー"), "U+30FC after every hiragana");
    /* Only the first 8 characters participate. */
    CHECK(packs("あいうえおかきく") == packs("あいうえおかきくけこ"),
          "characters past the 8th must not change the key");
    CHECK(packs("あいうえおかきく") != packs("あいうえおかきけ"),
          "the 8th character must participate");
    /* First character in the most significant byte. */
    k = packs("あ");
    CHECK((k >> 56) == skk_char_code(0x3042), "first char goes in the MSB");
    CHECK((k & 0xFFFFFFFFFFFFFFull) == 0, "the rest is END padding");

    (void)skk_pack("漢", 3, &esc);
    CHECK(esc, "an off-table character must report escaped");
    esc = true;
    (void)skk_pack("かんじ", 9, &esc);
    CHECK(!esc, "an on-table reading must not report escaped");
    /* Invalid UTF-8 must not stall or desynchronise the comparator. */
    esc = false;
    k = skk_pack("\xFF\xFE", 2, &esc);
    CHECK(esc && k != 0, "invalid UTF-8 packs as escaped, not as nothing");
    CHECK(skk_pack(NULL, 5, NULL) == 0, "NULL reading packs to 0");
}

static void test_entry_cmp(void)
{
    section("skk_entry_cmp");
    CHECK(skk_entry_cmp(packs("あ"), "あ", 3, packs("あ"), "あ", 3) == 0, "reflexive");
    CHECK(skk_entry_cmp(packs("あ"), "あ", 3, packs("い"), "い", 3) < 0, "ascending");
    CHECK(skk_entry_cmp(packs("い"), "い", 3, packs("あ"), "あ", 3) > 0, "antisymmetric");
    /* Same first 8 characters: the tie-break must be the full bytes, then
       shorter-first. This is the only path that touches dictionary text. */
    {
        const char *a = "あいうえおかきくけ";
        const char *b = "あいうえおかきくこ";
        const char *p = "あいうえおかきく";
        CHECK(packs(a) == packs(b), "these must collide in the packed key");
        CHECK(skk_entry_cmp(packs(a), a, strlen(a), packs(b), b, strlen(b)) < 0,
              "colliding keys fall back to memcmp");
        CHECK(skk_entry_cmp(packs(p), p, strlen(p), packs(a), a, strlen(a)) < 0,
              "shorter first when one is a prefix of the other");
    }
}

static void test_utf8(void)
{
    const char *s = "aあ\xE3\x81";   /* 1 byte, 3 bytes, then a truncated seq */
    size_t adv = 0;

    section("utf8 helpers");
    CHECK(skk_utf8_next(s, 6, 0) == 1, "ASCII advances by 1");
    CHECK(skk_utf8_next(s, 6, 1) == 4, "3-byte kana advances by 3");
    CHECK(skk_utf8_prev(s, 6, 4) == 1, "step back over kana");
    CHECK(skk_utf8_prev(s, 6, 0) == 0, "clamped at 0");
    CHECK(skk_utf8_next(s, 6, 6) == 6, "clamped at len");
    CHECK(skk_utf8_decode(s, 6, 1, &adv) == 0x3042 && adv == 3, "decode kana");
    /* A truncated sequence must not run off the end. */
    CHECK(skk_utf8_next(s, 6, 4) <= 6, "truncated sequence must stay in bounds");
}

/* ================================================================== */
/* 2. Synthetic dictionaries: candidate parsing, boundaries, error paths */

/* Assemble an image from literal dictionary text (already in the right
   order for each block). Returns the image; *out_free must be freed. */
static uint8_t *synth_image(const char *text, size_t *out_len, void **out_free,
                            rawdict_t *rd)
{
    memset(rd, 0, sizeof(*rd));
    rd->raw_len = strlen(text);
    rd->raw = (uint8_t *)malloc(rd->raw_len + 1);
    memcpy(rd->raw, text, rd->raw_len + 1);
    {
        skk_blob_t whole, ari, nasi;
        int b;
        whole.base = rd->raw;
        whole.len  = rd->raw_len;
        if (skk_text_split(whole, &ari, &nasi) != SKK_OK) {
            return NULL;
        }
        rd->span[SKK_BLK_NASI] = nasi;
        rd->span[SKK_BLK_ARI]  = ari;
        for (b = 0; b < SKK_BLK_COUNT; b++) {
            size_t cnt = 0, i;
            uint64_t keys[64];
            uint32_t offs[64];
            if (skk_index_build(rd->span[b], keys, offs, 64, &cnt) != SKK_OK) {
                return NULL;
            }
            rd->ent[b] = (ent_t *)malloc((cnt + 1) * sizeof(ent_t));
            for (i = 0; i < cnt; i++) {
                const char *r = NULL;
                size_t rlen = 0;
                if (reading_at(rd->span[b].base, 0, rd->span[b].len, offs[i],
                               &r, &rlen) != SKK_OK) {
                    return NULL;
                }
                rd->ent[b][i].key   = keys[i];
                rd->ent[b][i].off   = offs[i];
                rd->ent[b][i].rd    = r;
                rd->ent[b][i].rdlen = (uint32_t)rlen;
            }
            rd->n[b] = cnt;
            qsort(rd->ent[b], cnt, sizeof(ent_t), cmp_ent);
        }
    }
    return image_build(rd, out_len, out_free);
}

/* Look up `reading` and compare the candidate list against `want`, a
   '|'-separated expectation ("" = no candidates, NULL = no such entry). */
static void expect_cands(const skk_dict_t *d, skk_blk_t blk,
                         const char *reading, const char *want)
{
    skk_cand_t c[SKK_CAND_MAX];
    size_t n = 0, i;
    char got[512];
    size_t gl = 0;
    int rc;

    rc = skk_lookup(d, blk, reading, strlen(reading), c, SKK_CAND_MAX, &n);
    CHECK(rc == SKK_OK, "%s: lookup rc %d", reading, rc);
    for (i = 0; i < n; i++) {
        size_t tl = 0;
        const char *t = skk_cand_text(d, &c[i], &tl);
        if (!t) { CHECK(0, "%s: candidate %zu did not resolve", reading, i); return; }
        if (i) { got[gl++] = '|'; }
        memcpy(got + gl, t, tl);
        gl += tl;
    }
    got[gl] = 0;
    if (!want) {
        CHECK(n == 0, "%s: expected no entry, got %zu candidates (%s)",
              reading, n, got);
        return;
    }
    CHECK(strcmp(got, want) == 0, "%s: got \"%s\", want \"%s\"", reading, got, want);
}

static void test_candidate_parsing(void)
{
    /* Every v1 rule from skk_core.h, one line each. */
    static const char text[] =
        ";; okuri-ari entries.\n"
        "おくr /送/[り/送/]/贈/\n"
        ";; okuri-nasi entries.\n"
        "かんじ /漢字/感じ/幹事/監事/\n"
        "きゃく /客;guest/脚;leg/\n"
        "くうはく ///\n"
        "こんかっと /(concat \"a\\057b\")/正/\n"
        "ちゅうい /注意;-> ちゅうい/\n"
        "ぜんぶちゅうしゃく /;only an annotation/\n";
    rawdict_t rd;
    skk_dict_t d;
    void *fr = NULL;
    size_t ilen = 0;
    uint8_t *img;
    skk_blob_t blob;

    section("candidate parsing");
    img = synth_image(text, &ilen, &fr, &rd);
    REQUIRE(img != NULL, "synthetic image build failed");
    blob.base = img;
    blob.len  = ilen;
    REQUIRE(skk_dict_open(&d, blob, SKK_OPEN_VERIFY) == SKK_OK, "open failed");

    expect_cands(&d, SKK_BLK_NASI, "かんじ", "漢字|感じ|幹事|監事");
    /* ';' opens an annotation and is stripped; the candidate survives. */
    expect_cands(&d, SKK_BLK_NASI, "きゃく", "客|脚");
    expect_cands(&d, SKK_BLK_NASI, "ちゅうい", "注意");
    /* Every token empty -> found, but zero usable candidates. */
    expect_cands(&d, SKK_BLK_NASI, "くうはく", "");
    /* "(concat ...)" is skipped in v1, the plain candidate is not. */
    expect_cands(&d, SKK_BLK_NASI, "こんかっと", "正");
    /* A candidate that is nothing but an annotation is dropped. */
    expect_cands(&d, SKK_BLK_NASI, "ぜんぶちゅうしゃく", "");
    /* "[okuri/.../]" is skipped whole; the candidates around it are kept. */
    expect_cands(&d, SKK_BLK_ARI, "おくr", "送|贈");
    /* Blocks are separate spaces: an okuri-nasi reading is not in ari. */
    expect_cands(&d, SKK_BLK_ARI, "かんじ", NULL);
    expect_cands(&d, SKK_BLK_NASI, "おくr", NULL);

    /* dropped counting: the skipped forms must be reported, not hidden. */
    {
        skk_stats_t st;
        skk_cand_t c[SKK_CAND_MAX];
        size_t n = 0;
        memset(&st, 0, sizeof(st));
        skk_lookup_stats(&d, SKK_BLK_NASI, "こんかっと", strlen("こんかっと"),
                         c, SKK_CAND_MAX, &n, &st);
        CHECK(n == 1 && st.cands == 1 && st.dropped == 1,
              "concat: n=%zu cands=%u dropped=%u", n, st.cands, st.dropped);
        memset(&st, 0, sizeof(st));
        skk_lookup_stats(&d, SKK_BLK_ARI, "おくr", strlen("おくr"),
                         c, SKK_CAND_MAX, &n, &st);
        CHECK(n == 2 && st.dropped == 3,
              "okuri group: n=%zu dropped=%u (want 2 / 3)", n, st.dropped);
    }
    /* A small `cap` must still return the first candidates, not an error. */
    {
        skk_cand_t c[2];
        size_t n = 0;
        skk_stats_t st;
        memset(&st, 0, sizeof(st));
        CHECK(skk_lookup_stats(&d, SKK_BLK_NASI, "かんじ", strlen("かんじ"),
                               c, 2, &n, &st) == SKK_OK, "small cap must not fail");
        CHECK(n == 2, "small cap returned %zu", n);
        CHECK(st.dropped == 2, "small cap dropped %u, want 2", st.dropped);
    }
    /* Argument validation. */
    {
        size_t n = 123;
        CHECK(skk_lookup(NULL, SKK_BLK_NASI, "あ", 3, NULL, 0, &n) == SKK_ERR_ARG,
              "NULL dict");
        CHECK(n == 0, "out_n must be zeroed even on error");
        CHECK(skk_lookup(&d, (skk_blk_t)7, "あ", 3, NULL, 0, &n) == SKK_ERR_ARG,
              "bad block");
        CHECK(skk_lookup(&d, SKK_BLK_NASI, NULL, 3, NULL, 0, &n) == SKK_ERR_ARG,
              "NULL reading");
        CHECK(skk_lookup(&d, SKK_BLK_NASI, "", 0, NULL, 0, &n) == SKK_OK && n == 0,
              "empty reading is a miss, not an error");
    }
    free(fr);
    rawdict_free(&rd);
}

static void test_boundaries(void)
{
    static const char one[] =
        ";; okuri-ari entries.\n"
        ";; okuri-nasi entries.\n"
        "かんじ /漢字/\n";
    static const char empty[] =
        ";; okuri-ari entries.\n"
        ";; okuri-nasi entries.\n";
    rawdict_t rd;
    skk_dict_t d;
    void *fr = NULL;
    size_t ilen = 0;
    uint8_t *img;
    skk_blob_t blob;

    section("boundaries: single-entry and empty dictionaries");

    img = synth_image(one, &ilen, &fr, &rd);
    REQUIRE(img != NULL, "single-entry image build failed");
    blob.base = img; blob.len = ilen;
    REQUIRE(skk_dict_open(&d, blob, SKK_OPEN_VERIFY) == SKK_OK, "open failed");
    CHECK(d.blk[SKK_BLK_NASI].count == 1, "one nasi entry");
    CHECK(d.blk[SKK_BLK_ARI].count == 0, "no ari entries");
    CHECK(d.blk[SKK_BLK_ARI].keys == NULL, "empty block must have NULL keys");
    CHECK(skk_index_verify(&d, SKK_BLK_NASI, NULL) == SKK_OK, "verify nasi");
    CHECK(skk_index_verify(&d, SKK_BLK_ARI, NULL) == SKK_OK, "verify empty ari");
    expect_cands(&d, SKK_BLK_NASI, "かんじ", "漢字");
    expect_cands(&d, SKK_BLK_NASI, "あ", NULL);      /* before the only key */
    expect_cands(&d, SKK_BLK_NASI, "ん", NULL);      /* after it */
    expect_cands(&d, SKK_BLK_NASI, "かん", NULL);    /* a proper prefix */
    expect_cands(&d, SKK_BLK_NASI, "かんじじ", NULL);/* an extension */
    expect_cands(&d, SKK_BLK_ARI, "かんじ", NULL);   /* searching an empty block */
    free(fr);
    rawdict_free(&rd);

    img = synth_image(empty, &ilen, &fr, &rd);
    REQUIRE(img != NULL, "empty image build failed");
    blob.base = img; blob.len = ilen;
    REQUIRE(skk_dict_open(&d, blob, SKK_OPEN_VERIFY) == SKK_OK,
            "an empty dictionary must still open");
    CHECK(d.blk[SKK_BLK_NASI].count == 0 && d.blk[SKK_BLK_ARI].count == 0,
          "both blocks empty");
    CHECK(skk_index_verify(&d, SKK_BLK_NASI, NULL) == SKK_OK, "verify empty");
    expect_cands(&d, SKK_BLK_NASI, "かんじ", NULL);
    free(fr);
    rawdict_free(&rd);

    /* A file without the two marker lines is not a SKK dictionary. */
    {
        skk_blob_t t, a, n;
        static const char junk[] = "hello\nworld\n";
        t.base = (const uint8_t *)junk;
        t.len  = sizeof(junk) - 1;
        CHECK(skk_text_split(t, &a, &n) == SKK_ERR_FORMAT,
              "a file without markers must be refused");
    }
}

static void test_open_errors(const rawdict_t *rd)
{
    size_t ilen = 0;
    void *fr = NULL;
    uint8_t *img = image_build(rd, &ilen, &fr);
    uint8_t *copy;
    void *cfr = NULL;
    skk_dict_t d;
    skk_blob_t blob;

    section("skk_dict_open error paths");
    if (!img) { CHECK(0, "image build failed"); return; }
    copy = alloc_aligned(ilen, &cfr);
    if (!copy) { CHECK(0, "alloc failed"); free(fr); return; }

#define RESET() memcpy(copy, img, ilen); blob.base = copy; blob.len = ilen

    RESET();
    CHECK(skk_dict_open(&d, blob, SKK_OPEN_VERIFY) == SKK_OK, "the pristine copy opens");
    CHECK(skk_dict_open(NULL, blob, 0) == SKK_ERR_ARG, "NULL dict");
    { skk_blob_t z; z.base = NULL; z.len = 100;
      CHECK(skk_dict_open(&d, z, 0) == SKK_ERR_ARG, "NULL base"); }
    { skk_blob_t z; z.base = copy; z.len = 10;
      CHECK(skk_dict_open(&d, z, 0) == SKK_ERR_ARG, "blob shorter than a header"); }

    RESET(); wr32(copy + 0, 0xDEADBEEFu);
    CHECK(skk_dict_open(&d, blob, 0) == SKK_ERR_MAGIC, "bad magic");

    RESET(); wr16(copy + 4, 99);
    CHECK(skk_dict_open(&d, blob, 0) == SKK_ERR_VERSION, "bad version");

    RESET(); wr32(copy + 16, skk_alphabet_crc32() ^ 1u);
    CHECK(skk_dict_open(&d, blob, 0) == SKK_ERR_ALPHABET,
          "an image built with another collation table must be refused");

    RESET(); wr32(copy + 8, (uint32_t)ilen + 8u);
    CHECK(skk_dict_open(&d, blob, 0) == SKK_ERR_TRUNCATED, "image_len past the blob");

    RESET(); wr32(copy + 32, (uint32_t)(ilen - 8));   /* nasi_keys_off */
    CHECK(skk_dict_open(&d, blob, 0) == SKK_ERR_TRUNCATED, "key array runs off the end");

    RESET(); wr32(copy + 32, 68);                     /* 4-aligned, not 8 */
    CHECK(skk_dict_open(&d, blob, 0) == SKK_ERR_TRUNCATED, "unaligned key array");

    RESET(); wr32(copy + 32, 4);                      /* inside the header */
    CHECK(skk_dict_open(&d, blob, 0) == SKK_ERR_TRUNCATED, "section overlapping the header");

    RESET(); copy[ilen - 1] ^= 0xFFu;
    CHECK(skk_dict_open(&d, blob, 0) == SKK_OK, "a corrupt payload opens without VERIFY");
    CHECK(skk_dict_open(&d, blob, SKK_OPEN_VERIFY) == SKK_ERR_CRC,
          "SKK_OPEN_VERIFY must catch a flipped byte");

    /* Unaligned base: refused, not faulted. skk_core.h makes this a hard
       contract because EMBED_FILES does not promise 8 bytes. */
    RESET();
    {
        uint8_t *un = (uint8_t *)malloc(ilen + 8);
        skk_blob_t ub;
        memcpy(un + 1, img, ilen);
        ub.base = un + 1;
        ub.len  = ilen;
        if ((((uintptr_t)un + 1) & 7u) != 0) {
            CHECK(skk_dict_open(&d, ub, 0) == SKK_ERR_ALIGN, "unaligned base");
        }
        free(un);
    }
#undef RESET
    free(cfr);
    free(fr);
}

/* ================================================================== */
/* 3. The real dictionaries */

typedef struct {
    const char *name;
    const char *src;        /* SKK-JISYO.<name>.utf8 */
    const char *img;        /* skk_dict_<name>.bin from tools/skk_prep.py */
    long        exp_nasi_violations;   /* design §6.1; -1 = just report */
    int         exhaustive;            /* O(n^2) pairwise sweep? */
} dictspec_t;

/* Direct pairwise statement of the property, on top of the (equivalent
 * and exhaustive) monotonicity check: for every pair,
 *     packed-key order  ==  raw UTF-8 byte order.
 * O(n^2) on the small blocks, a large random sample on the big ones. */
static void check_pairs(const ent_t *e, size_t n, int exhaustive,
                        const char *what)
{
    size_t bad = 0, tested = 0, alias = 0;
    size_t i, j;

    if (exhaustive) {
        for (i = 0; i < n; i++) {
            for (j = i + 1; j < n; j++) {
                int cb = cmp_raw_bytes(e[i].rd, e[i].rdlen, e[j].rd, e[j].rdlen);
                tested++;
                if (e[i].key < e[j].key) {
                    if (cb >= 0) { bad++; }
                } else if (e[i].key > e[j].key) {
                    if (cb <= 0) { bad++; }
                } else {
                    /* Equal keys must mean an identical first-8-character
                       prefix, otherwise the key aliases unrelated readings
                       and the fallback compare is doing all the work. */
                    size_t pa = prefix8_len(e[i].rd, e[i].rdlen);
                    size_t pb = prefix8_len(e[j].rd, e[j].rdlen);
                    if (pa != pb || memcmp(e[i].rd, e[j].rd, pa) != 0) { alias++; }
                }
            }
        }
    } else {
        size_t iters = 20000000u;
        for (i = 0; i < iters; i++) {
            size_t a = (size_t)(rnd64() % n);
            size_t b = (size_t)(rnd64() % n);
            int cb;
            if (a == b) { continue; }
            cb = cmp_raw_bytes(e[a].rd, e[a].rdlen, e[b].rd, e[b].rdlen);
            tested++;
            if (e[a].key < e[b].key) {
                if (cb >= 0) { bad++; }
            } else if (e[a].key > e[b].key) {
                if (cb <= 0) { bad++; }
            } else {
                size_t pa = prefix8_len(e[a].rd, e[a].rdlen);
                size_t pb = prefix8_len(e[b].rd, e[b].rdlen);
                if (pa != pb || memcmp(e[a].rd, e[b].rd, pa) != 0) { alias++; }
            }
        }
    }
    CHECK(bad == 0, "%s: %zu of %zu pairs order differently under the packed "
          "key than under raw UTF-8 bytes", what, bad, tested);
    CHECK(alias == 0, "%s: %zu equal-key pairs do not share a first-8 prefix",
          what, alias);
    printf("    %-28s %s %zu pairs, %zu disagreements\n", what,
           exhaustive ? "exhaustive" : "sampled", tested, bad);
}

/* Monotonicity of the packed key along a byte-sorted array. Equivalent to
   the pairwise statement (a non-decreasing key over a totally byte-sorted
   sequence gives bytes(a)<bytes(b) => key(a)<=key(b), and the converse
   follows), and it covers EVERY entry rather than a sample. */
static void check_key_monotone(ent_t *e, size_t n, const char *what)
{
    ent_t *c = (ent_t *)malloc((n + 1) * sizeof(ent_t));
    size_t bad = 0, i;

    memcpy(c, e, n * sizeof(ent_t));
    qsort(c, n, sizeof(ent_t), cmp_ent_bytes);
    for (i = 1; i < n; i++) {
        if (c[i - 1].key > c[i].key) {
            if (bad == 0) {
                printf("    first break at %zu: \"%.*s\" (0x%016llx) then "
                       "\"%.*s\" (0x%016llx)\n", i,
                       (int)c[i - 1].rdlen, c[i - 1].rd,
                       (unsigned long long)c[i - 1].key,
                       (int)c[i].rdlen, c[i].rd,
                       (unsigned long long)c[i].key);
            }
            bad++;
        }
    }
    CHECK(bad == 0, "%s: packed key is not monotone over byte order "
          "(%zu breaks of %zu entries)", what, bad, n);
    free(c);
}

/* Sorting by skk_entry_cmp() must produce exactly the sequence that
   sorting by raw UTF-8 bytes does. Compared by reading bytes, so the
   instability of qsort over duplicate readings cannot matter. */
static void check_sort_agrees(const ent_t *e, size_t n, const char *what)
{
    ent_t *a = (ent_t *)malloc((n + 1) * sizeof(ent_t));
    ent_t *b = (ent_t *)malloc((n + 1) * sizeof(ent_t));
    size_t bad = 0, i;

    memcpy(a, e, n * sizeof(ent_t));
    memcpy(b, e, n * sizeof(ent_t));
    qsort(a, n, sizeof(ent_t), cmp_ent);
    qsort(b, n, sizeof(ent_t), cmp_ent_bytes);
    for (i = 0; i < n; i++) {
        if (a[i].rdlen != b[i].rdlen || memcmp(a[i].rd, b[i].rd, a[i].rdlen) != 0) {
            bad++;
        }
    }
    CHECK(bad == 0, "%s: skk_entry_cmp order differs from UTF-8 byte order at "
          "%zu of %zu entries", what, bad, n);
    free(a);
    free(b);
}

typedef struct {
    uint64_t lookups, probes, fullcmp, hits;
    uint64_t miss_lookups, miss_probes, miss_fullcmp;
} probe_acc_t;

/* Look every heading up and demand it finds ITSELF, with the candidate
   list the line actually holds. */
static void roundtrip_block(const skk_dict_t *d, skk_blk_t blk,
                            const ent_t *e, size_t n, const char *what,
                            probe_acc_t *acc)
{
    size_t bad_find = 0, bad_cands = 0, i;
    skk_stats_t st;
    double t0, t1;

    memset(&st, 0, sizeof(st));
    t0 = now_s();
    for (i = 0; i < n; i++) {
        skk_cand_t c[SKK_CAND_MAX];
        size_t got = 0;
        skk_line_t ln;
        cand_iter_t it;
        const char *txt;
        size_t tlen, want = 0;

        if (skk_lookup_stats(d, blk, e[i].rd, e[i].rdlen, c, SKK_CAND_MAX,
                             &got, &st) != SKK_OK) {
            bad_find++;
            continue;
        }
        /* find_entry is what actually decides "found"; a line whose every
           candidate is a v1-skipped form legitimately yields 0. */
        if (!find_entry(d, blk, e[i].rd, e[i].rdlen, &ln, NULL)) {
            if (bad_find == 0) {
                printf("    NOT FOUND: \"%.*s\"\n", (int)e[i].rdlen, e[i].rd);
            }
            bad_find++;
            continue;
        }
        if (ln.reading_len != e[i].rdlen ||
            memcmp(ln.reading, e[i].rd, e[i].rdlen) != 0) {
            bad_find++;
            continue;
        }
        /* Independently count what the line should yield. */
        cand_iter_init(&it, &ln);
        while (cand_next(&it, &txt, &tlen, NULL)) {
            if (want < SKK_CAND_MAX) {
                size_t gl = 0;
                const char *gt = skk_cand_text(d, &c[want], &gl);
                if (!gt || gl != tlen || memcmp(gt, txt, tlen) != 0) {
                    bad_cands++;
                    break;
                }
            }
            want++;
        }
        if (want > SKK_CAND_MAX) { want = SKK_CAND_MAX; }
        if (got != want) {
            bad_cands++;
        }
    }
    t1 = now_s();
    CHECK(bad_find == 0, "%s: %zu of %zu headings did not find themselves",
          what, bad_find, n);
    CHECK(bad_cands == 0, "%s: %zu of %zu candidate lists differ from the line",
          what, bad_cands, n);
    printf("    %-28s %zu lookups, %.2f probes/lookup, %.3f dict touches/lookup"
           ", %.2f us/lookup\n",
           what, n,
           n ? (double)st.probes / (double)n : 0.0,
           n ? (double)st.fullcmp / (double)n : 0.0,
           n ? (t1 - t0) * 1e6 / (double)n : 0.0);
    acc->lookups += n;
    acc->probes  += st.probes;
    acc->fullcmp += st.fullcmp;
    acc->hits    += n - bad_find;
}

/* The sampled tree against the bisection it replaces.
 *
 * A wrong tree does not crash and does not fail the CRC — it steers the
 * descent into the wrong eight keys, and the reading is simply "not in
 * the dictionary". The only thing that can see that is the answer
 * itself, so run every heading (and a guaranteed-absent variant of it,
 * to cover the lower bounds that fall BETWEEN entries) through
 * key_lower_bound twice: once with the tree, once with the same image
 * and levels forced to 0. Every index must match. The probe counts are
 * the measurement the design is claiming — printed, not asserted, since
 * they follow from the block size. */
static void tree_compare_block(const skk_dict_t *d, skk_blk_t blk,
                               const ent_t *e, size_t n, const char *what)
{
    skk_dict_t flat = *d;
    const skk_index_t *ix = &d->blk[blk];
    uint32_t p_tree = 0, p_flat = 0;
    size_t bad = 0, i, tried = 0;

    if (!ix->keys || n == 0) {
        return;
    }
    CHECK(ix->levels > 0, "%s: the image carries no search tree", what);
    flat.blk[blk].levels = 0;

    for (i = 0; i < n; i++) {
        char buf[SKK_READING_MAX * 2];
        static const char *sfx[] = { "ゐ", "ゑ", "ゐゑゐゑゐゑゐゑ" };
        const char *s = sfx[i % 3];
        size_t sl = strlen(s);
        uint64_t k[2];
        int j;

        k[0] = skk_pack(e[i].rd, e[i].rdlen, NULL);
        if (e[i].rdlen + sl < sizeof(buf)) {
            memcpy(buf, e[i].rd, e[i].rdlen);
            memcpy(buf + e[i].rdlen, s, sl);
            k[1] = skk_pack(buf, e[i].rdlen + sl, NULL);
        } else {
            k[1] = k[0];
        }
        for (j = 0; j < 2; j++) {
            uint32_t a = key_lower_bound(ix, k[j], &p_tree);
            uint32_t b = key_lower_bound(&flat.blk[blk], k[j], &p_flat);
            if (a != b) {
                if (bad == 0) {
                    printf("    TREE DISAGREES: key 0x%016llx -> %u, "
                           "bisection -> %u\n",
                           (unsigned long long)k[j], a, b);
                }
                bad++;
            }
            tried++;
        }
    }
    CHECK(bad == 0, "%s: the tree answered differently %zu times out of %zu",
          what, bad, tried);
    printf("    %-28s %zu bounds, %.2f lines/lookup with the tree, "
           "%.2f bisecting (%.0f%% fewer, %u levels)\n",
           what, tried,
           (double)p_tree / (double)tried, (double)p_flat / (double)tried,
           100.0 * (1.0 - (double)p_tree / (double)p_flat), ix->levels);
}

/* Readings guaranteed absent: every real heading with a suffix appended,
   which lands the search on a real probe path rather than off the end. */
static void miss_block(const skk_dict_t *d, skk_blk_t blk,
                       const ent_t *e, size_t n, const char *what,
                       probe_acc_t *acc)
{
    skk_stats_t st;
    size_t found = 0, i, tried = 0;
    static const char *suffix[] = { "ゐ", "ゑ", "ゐゑゐゑゐゑゐゑ" };

    memset(&st, 0, sizeof(st));
    for (i = 0; i < n; i++) {
        char buf[SKK_READING_MAX * 2];
        const char *sfx = suffix[i % 3];
        size_t sl = strlen(sfx);
        skk_cand_t c[SKK_CAND_MAX];
        size_t got = 0;

        if (e[i].rdlen + sl >= sizeof(buf)) { continue; }
        memcpy(buf, e[i].rd, e[i].rdlen);
        memcpy(buf + e[i].rdlen, sfx, sl);
        tried++;
        skk_lookup_stats(d, blk, buf, e[i].rdlen + sl, c, SKK_CAND_MAX, &got, &st);
        if (got != 0) {
            if (found == 0) {
                printf("    PHANTOM: \"%.*s\" matched %zu candidates\n",
                       (int)(e[i].rdlen + sl), buf, got);
            }
            found++;
        }
    }
    CHECK(found == 0, "%s: %zu absent readings were 'found'", what, found);
    printf("    %-28s %zu misses, %.2f probes/lookup, %.3f dict touches/lookup\n",
           what, tried,
           tried ? (double)st.probes / (double)tried : 0.0,
           tried ? (double)st.fullcmp / (double)tried : 0.0);
    acc->miss_lookups += tried;
    acc->miss_probes  += st.probes;
    acc->miss_fullcmp += st.fullcmp;
}

static void collision_report(const ent_t *e, size_t n, const char *what)
{
    size_t i, in_group = 0, largest = 1, run = 1;

    for (i = 1; i <= n; i++) {
        if (i < n && e[i].key == e[i - 1].key) {
            run++;
        } else {
            if (run > 1) {
                in_group += run;
                if (run > largest) { largest = run; }
            }
            run = 1;
        }
    }
    printf("    %-28s %zu entries, %.2f%% in a packed-key collision group, "
           "largest %zu\n", what, n,
           n ? 100.0 * (double)in_group / (double)n : 0.0, largest);
}

static void run_dict(const dictspec_t *sp, const char *srcdir, const char *imgdir,
                     probe_acc_t *acc)
{
    char path[1024];
    rawdict_t rd;
    size_t nasi_v, ari_v;
    uint32_t at = 0;
    int rc;
    skk_dict_t d;
    skk_blob_t blob;
    uint8_t *img;
    void *fr = NULL;
    size_t ilen = 0;
    char label[128];

    snprintf(path, sizeof(path), "%s/SKK-JISYO.%s.utf8", srcdir, sp->name);
    printf("\n== SKK-JISYO.%s  (%s)\n", sp->name, path);
    g_ctx = sp->name;
    if (rawdict_load(&rd, path) != 0) {
        CHECK(0, "could not load/split %s", path);
        return;
    }
    printf("    %zu okuri-nasi, %zu okuri-ari, %zu source bytes\n",
           rd.n[SKK_BLK_NASI], rd.n[SKK_BLK_ARI], rd.raw_len);

    /* ---- the EUC-JP -> UTF-8 reordering, as shipped ---- */
    nasi_v = count_violations(rd.ent[SKK_BLK_NASI], rd.n[SKK_BLK_NASI]);
    ari_v  = count_violations(rd.ent[SKK_BLK_ARI],  rd.n[SKK_BLK_ARI]);
    printf("    order violations in the file: okuri-nasi %zu, okuri-ari %zu\n",
           nasi_v, ari_v);
    if (sp->exp_nasi_violations >= 0) {
        CHECK((long)nasi_v == sp->exp_nasi_violations,
              "okuri-nasi has %zu violations, design says %ld",
              nasi_v, sp->exp_nasi_violations);
    }

    /* skk_index_check() must REJECT the file order when it is broken and
       ACCEPT it when it is not — S is the control that proves the check
       is not simply always failing. */
    {
        skk_blob_t t = rd.span[SKK_BLK_NASI];
        uint64_t *k = (uint64_t *)malloc((rd.n[SKK_BLK_NASI] + 1) * 8);
        uint32_t *o = (uint32_t *)malloc((rd.n[SKK_BLK_NASI] + 1) * 4);
        size_t i;
        for (i = 0; i < rd.n[SKK_BLK_NASI]; i++) {
            k[i] = rd.ent[SKK_BLK_NASI][i].key;
            o[i] = rd.ent[SKK_BLK_NASI][i].off;
        }
        rc = skk_index_check(t, k, o, rd.n[SKK_BLK_NASI], &at);
        if (nasi_v > 0) {
            CHECK(rc == SKK_ERR_FORMAT,
                  "an unsorted okuri-nasi block must be refused (rc %d)", rc);
            if (rc == SKK_ERR_FORMAT && at > 0 && at < rd.n[SKK_BLK_NASI]) {
                printf("    refused at entry %u: \"%.*s\" after \"%.*s\"\n", at,
                       (int)rd.ent[SKK_BLK_NASI][at].rdlen,
                       rd.ent[SKK_BLK_NASI][at].rd,
                       (int)rd.ent[SKK_BLK_NASI][at - 1].rdlen,
                       rd.ent[SKK_BLK_NASI][at - 1].rd);
            }
        } else {
            CHECK(rc == SKK_OK,
                  "okuri-nasi has no violations, it must be accepted (rc %d)", rc);
        }
        free(k);
        free(o);
    }
    /* okuri-ari ships DESCENDING, so the file order must always be
       refused — the prep tool re-sorts it ascending (skk_core.h). */
    if (rd.n[SKK_BLK_ARI] > 1) {
        skk_blob_t t = rd.span[SKK_BLK_ARI];
        uint64_t *k = (uint64_t *)malloc((rd.n[SKK_BLK_ARI] + 1) * 8);
        uint32_t *o = (uint32_t *)malloc((rd.n[SKK_BLK_ARI] + 1) * 4);
        size_t i;
        for (i = 0; i < rd.n[SKK_BLK_ARI]; i++) {
            k[i] = rd.ent[SKK_BLK_ARI][i].key;
            o[i] = rd.ent[SKK_BLK_ARI][i].off;
        }
        CHECK(skk_index_check(t, k, o, rd.n[SKK_BLK_ARI], &at) == SKK_ERR_FORMAT,
              "the descending okuri-ari block must be refused as shipped");
        free(k);
        free(o);
    }

    /* ---- ORDER PRESERVATION: the test everything else rests on ---- */
    snprintf(label, sizeof(label), "%s okuri-nasi key order", sp->name);
    check_key_monotone(rd.ent[SKK_BLK_NASI], rd.n[SKK_BLK_NASI], label);
    snprintf(label, sizeof(label), "%s okuri-ari key order", sp->name);
    check_key_monotone(rd.ent[SKK_BLK_ARI], rd.n[SKK_BLK_ARI], label);
    snprintf(label, sizeof(label), "%s okuri-nasi sort", sp->name);
    check_sort_agrees(rd.ent[SKK_BLK_NASI], rd.n[SKK_BLK_NASI], label);
    snprintf(label, sizeof(label), "%s okuri-ari sort", sp->name);
    check_sort_agrees(rd.ent[SKK_BLK_ARI], rd.n[SKK_BLK_ARI], label);
    snprintf(label, sizeof(label), "%s okuri-nasi pairs", sp->name);
    check_pairs(rd.ent[SKK_BLK_NASI], rd.n[SKK_BLK_NASI], sp->exhaustive, label);
    snprintf(label, sizeof(label), "%s okuri-ari pairs", sp->name);
    check_pairs(rd.ent[SKK_BLK_ARI], rd.n[SKK_BLK_ARI], sp->exhaustive, label);

    /* ---- sort, then build and search an image ---- */
    qsort(rd.ent[SKK_BLK_NASI], rd.n[SKK_BLK_NASI], sizeof(ent_t), cmp_ent);
    qsort(rd.ent[SKK_BLK_ARI],  rd.n[SKK_BLK_ARI],  sizeof(ent_t), cmp_ent);
    snprintf(label, sizeof(label), "%s okuri-nasi collisions", sp->name);
    collision_report(rd.ent[SKK_BLK_NASI], rd.n[SKK_BLK_NASI], label);
    snprintf(label, sizeof(label), "%s okuri-ari collisions", sp->name);
    collision_report(rd.ent[SKK_BLK_ARI], rd.n[SKK_BLK_ARI], label);

    img = image_build(&rd, &ilen, &fr);
    REQUIRE(img != NULL, "image build failed");
    blob.base = img;
    blob.len  = ilen;
    REQUIRE(skk_dict_open(&d, blob, SKK_OPEN_VERIFY) == SKK_OK,
            "self-built image failed to open");
    CHECK(skk_index_verify(&d, SKK_BLK_NASI, &at) == SKK_OK,
          "skk_index_verify(nasi) failed at %u", at);
    CHECK(skk_index_verify(&d, SKK_BLK_ARI, &at) == SKK_OK,
          "skk_index_verify(ari) failed at %u", at);

    snprintf(label, sizeof(label), "%s okuri-nasi roundtrip", sp->name);
    roundtrip_block(&d, SKK_BLK_NASI, rd.ent[SKK_BLK_NASI], rd.n[SKK_BLK_NASI],
                    label, acc);
    snprintf(label, sizeof(label), "%s okuri-ari roundtrip", sp->name);
    roundtrip_block(&d, SKK_BLK_ARI, rd.ent[SKK_BLK_ARI], rd.n[SKK_BLK_ARI],
                    label, acc);
    snprintf(label, sizeof(label), "%s okuri-nasi misses", sp->name);
    miss_block(&d, SKK_BLK_NASI, rd.ent[SKK_BLK_NASI], rd.n[SKK_BLK_NASI],
               label, acc);
    snprintf(label, sizeof(label), "%s okuri-nasi tree", sp->name);
    tree_compare_block(&d, SKK_BLK_NASI, rd.ent[SKK_BLK_NASI],
                       rd.n[SKK_BLK_NASI], label);
    snprintf(label, sizeof(label), "%s okuri-ari tree", sp->name);
    tree_compare_block(&d, SKK_BLK_ARI, rd.ent[SKK_BLK_ARI],
                       rd.n[SKK_BLK_ARI], label);

    /* Boundaries of a real dictionary: the very first and very last key,
       and absent readings at both ends of the index, where lower_bound
       returns 0 and count respectively and the forward walk has nowhere
       to go. The probes are runs of the lowest ('!') and highest ('：')
       characters in the collation table, grown until the array really
       does not contain them — "!" alone IS a heading in M and L. */
    {
        const ent_t *first = &rd.ent[SKK_BLK_NASI][0];
        const ent_t *last  = &rd.ent[SKK_BLK_NASI][rd.n[SKK_BLK_NASI] - 1];
        skk_line_t ln;
        char probe[64];
        size_t plen;
        int k;

        CHECK(find_entry(&d, SKK_BLK_NASI, first->rd, first->rdlen, &ln, NULL),
              "the first entry must be findable");
        CHECK(find_entry(&d, SKK_BLK_NASI, last->rd, last->rdlen, &ln, NULL),
              "the last entry must be findable");

        for (k = 1; k <= 12; k++) {
            memset(probe, '!', (size_t)k);
            if (!absent_in(rd.ent[SKK_BLK_NASI], rd.n[SKK_BLK_NASI], probe,
                           (size_t)k)) {
                continue;
            }
            CHECK(!find_entry(&d, SKK_BLK_NASI, probe, (size_t)k, &ln, NULL),
                  "\"%.*s\" is not in the dictionary and must miss at the "
                  "bottom of the index", k, probe);
            break;
        }
        CHECK(k <= 12, "could not build a low probe absent from the dictionary");

        for (k = 1; k <= 12; k++) {
            int j;
            plen = 0;
            for (j = 0; j < k; j++) {
                memcpy(probe + plen, "\xEF\xBC\x9A", 3);   /* U+FF1A */
                plen += 3;
            }
            if (!absent_in(rd.ent[SKK_BLK_NASI], rd.n[SKK_BLK_NASI], probe, plen)) {
                continue;
            }
            CHECK(!find_entry(&d, SKK_BLK_NASI, probe, plen, &ln, NULL),
                  "U+FF1A x%d must miss at the top of the index", k);
            break;
        }
        CHECK(k <= 12, "could not build a high probe absent from the dictionary");
    }

    /* ---- the real prep-tool image, if one was built ---- */
    if (imgdir && sp->img) {
        size_t plen = 0;
        uint8_t *praw;
        void *pfr = NULL;
        uint8_t *pimg;
        skk_dict_t pd;
        skk_blob_t pblob;

        snprintf(path, sizeof(path), "%s/%s", imgdir, sp->img);
        praw = load_file(path, &plen);
        if (!praw) {
            printf("    (no prep-tool image at %s — skipped)\n", path);
        } else {
            pimg = alloc_aligned(plen, &pfr);
            memcpy(pimg, praw, plen);
            pblob.base = pimg;
            pblob.len  = plen;
            rc = skk_dict_open(&pd, pblob, SKK_OPEN_VERIFY);
            CHECK(rc == SKK_OK, "tools/skk_prep.py image failed to open: rc %d "
                  "(-7 = SKK_ERR_ALPHABET: the generator's collation stamp "
                  "disagrees with skk_alphabet_crc32())", rc);
            if (rc == SKK_OK) {
                size_t i, bad = 0, dup = 0;

                /* The generator merges duplicate readings (L ships one),
                   so the image holds exactly one entry per DISTINCT
                   reading — not merely "no more than the source". */
                for (i = 1; i < rd.n[SKK_BLK_NASI]; i++) {
                    if (rd.ent[SKK_BLK_NASI][i].rdlen ==
                            rd.ent[SKK_BLK_NASI][i - 1].rdlen &&
                        memcmp(rd.ent[SKK_BLK_NASI][i].rd,
                               rd.ent[SKK_BLK_NASI][i - 1].rd,
                               rd.ent[SKK_BLK_NASI][i].rdlen) == 0) {
                        dup++;
                    }
                }
                CHECK(pd.blk[SKK_BLK_NASI].count == rd.n[SKK_BLK_NASI] - dup,
                      "prep image has %u okuri-nasi entries, source has %zu "
                      "distinct readings (%zu duplicates merged)",
                      pd.blk[SKK_BLK_NASI].count, rd.n[SKK_BLK_NASI] - dup, dup);
                CHECK(skk_index_verify(&pd, SKK_BLK_NASI, &at) == SKK_OK,
                      "prep image okuri-nasi not sorted (at %u)", at);
                CHECK(skk_index_verify(&pd, SKK_BLK_ARI, &at) == SKK_OK,
                      "prep image okuri-ari not sorted (at %u)", at);
                /* Every heading of the SOURCE must be findable in the
                   image the generator wrote — writer and reader agreeing
                   is the whole point of the packed key. */
                for (i = 0; i < rd.n[SKK_BLK_NASI]; i++) {
                    skk_line_t ln;
                    if (!find_entry(&pd, SKK_BLK_NASI, rd.ent[SKK_BLK_NASI][i].rd,
                                    rd.ent[SKK_BLK_NASI][i].rdlen, &ln, NULL)) {
                        bad++;
                    }
                }
                CHECK(bad == 0, "%zu source headings missing from the prep image",
                      bad);
                printf("    prep-tool image: %zu B, %u nasi / %u ari, all "
                       "source headings found\n", plen,
                       pd.blk[SKK_BLK_NASI].count, pd.blk[SKK_BLK_ARI].count);
            }
            free(pfr);
            free(praw);
        }
    }

    free(fr);
    rawdict_free(&rd);
}

/* ================================================================== */

int main(int argc, char **argv)
{
    static const dictspec_t specs[] = {
        /* name  source suffix        image                 §6.1 violations  exhaustive */
        { "S",  "S",  "skk_dict_S.bin",   0, 1 },
        { "M",  "M",  "skk_dict_M.bin",   2, 1 },
        { "ML", "ML", "skk_dict_ML.bin", 22, 0 },
        { "L",  "L",  "skk_dict_L.bin", 285, 0 },
    };
    const char *srcdir = (argc > 1) ? argv[1] : ".";
    const char *imgdir = (argc > 2) ? argv[2] : NULL;
    probe_acc_t acc;
    size_t i;
    rawdict_t rd0;
    char path[1024];

    memset(&acc, 0, sizeof(acc));
    printf("skk_dict host test — dictionaries in %s\n", srcdir);

    test_crc32();
    test_alphabet();
    test_pack();
    test_entry_cmp();
    test_utf8();
    test_candidate_parsing();
    test_boundaries();

    /* Error paths need one real image to mutate. */
    snprintf(path, sizeof(path), "%s/SKK-JISYO.S.utf8", srcdir);
    if (rawdict_load(&rd0, path) == 0) {
        qsort(rd0.ent[SKK_BLK_NASI], rd0.n[SKK_BLK_NASI], sizeof(ent_t), cmp_ent);
        qsort(rd0.ent[SKK_BLK_ARI],  rd0.n[SKK_BLK_ARI],  sizeof(ent_t), cmp_ent);
        test_open_errors(&rd0);
        rawdict_free(&rd0);
    } else {
        section("skk_dict_open error paths");
        CHECK(0, "could not load %s", path);
    }

    for (i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        run_dict(&specs[i], srcdir, imgdir, &acc);
    }

    printf("\n== probe totals (design §6.3 predicts log2(N) probes and "
           "1-2 dictionary touches)\n");
    printf("    hits   : %llu lookups, %llu probes (%.2f/lookup), "
           "%llu dictionary touches (%.3f/lookup)\n",
           (unsigned long long)acc.lookups, (unsigned long long)acc.probes,
           acc.lookups ? (double)acc.probes / (double)acc.lookups : 0.0,
           (unsigned long long)acc.fullcmp,
           acc.lookups ? (double)acc.fullcmp / (double)acc.lookups : 0.0);
    printf("    misses : %llu lookups, %llu probes (%.2f/lookup), "
           "%llu dictionary touches (%.3f/lookup)\n",
           (unsigned long long)acc.miss_lookups,
           (unsigned long long)acc.miss_probes,
           acc.miss_lookups ? (double)acc.miss_probes / (double)acc.miss_lookups : 0.0,
           (unsigned long long)acc.miss_fullcmp,
           acc.miss_lookups ? (double)acc.miss_fullcmp / (double)acc.miss_lookups : 0.0);

    printf("\n%s: %d checks passed, %d failed\n",
           g_fail ? "FAILURE" : "SUCCESS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
