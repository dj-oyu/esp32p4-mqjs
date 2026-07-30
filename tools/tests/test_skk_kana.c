/*
 * Host unit test for skk_core's romaji->kana converter and SKK state
 * machine (components/skk_core/skk_kana.c), against docs/skk-ime-design.md
 * §5 and the contract written into skk_core.h.
 *
 * The dictionary half (skk_dict.c) is deliberately NOT linked: skk_lookup()
 * and skk_cand_text() are stubbed here with a tiny in-memory fake, so a
 * failure in this binary is a state-machine failure and nothing else. The
 * fake also records what the machine asked for, which is how the okurigana
 * key ("もt" in the okuri-ari block) gets checked.
 *
 * Build/run (host):
 *   gcc -O2 -Wall -Wextra -std=c99 -I components/skk_core/include \
 *       tools/tests/test_skk_kana.c components/skk_core/skk_kana.c \
 *       -o /tmp/test_skk_kana && /tmp/test_skk_kana
 *
 * Same with the allocation check armed — skk_core must never allocate
 * (design §4.3), and this proves it at run time rather than by reading:
 *   gcc -O2 -Wall -Wextra -std=c99 -DSKK_WRAP_ALLOC \
 *       -I components/skk_core/include \
 *       tools/tests/test_skk_kana.c components/skk_core/skk_kana.c \
 *       -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free \
 *       -o /tmp/test_skk_kana && /tmp/test_skk_kana
 *
 * The same claim, structurally (only memcpy/memset/memcmp/strlen may
 * appear — no malloc, no clock, no I/O):
 *   gcc -c -O2 -I components/skk_core/include components/skk_core/skk_kana.c \
 *       -o /tmp/skk_kana.o && nm -u /tmp/skk_kana.o
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "skk_core.h"

/* ------------------------------------------------------------------ */
/* Reporting — every case runs, so one broken rule does not hide the    */
/* next one.                                                            */

static int         g_total;
static int         g_fail;
static const char *g_case = "";

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_total++;                                                            \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            printf("FAIL [%s] %s:%d: ", g_case, __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                              \
            putchar('\n');                                                    \
        }                                                                     \
    } while (0)

static void eqs_(const char *got, size_t gn, const char *want, const char *what,
                 int line)
{
    g_total++;
    if (gn != strlen(want) || (gn && memcmp(got, want, gn) != 0)) {
        g_fail++;
        printf("FAIL [%s] %s:%d: %s = \"%.*s\" (%u B), want \"%s\"\n", g_case,
               __FILE__, line, what, (int)gn, got, (unsigned)gn, want);
    }
}
#define EQS(got, gn, want, what) eqs_((got), (gn), (want), (what), __LINE__)

/* ------------------------------------------------------------------ */
/* Allocation guard (only with -DSKK_WRAP_ALLOC + -Wl,--wrap=...)       */

#ifdef SKK_WRAP_ALLOC
static long g_allocs;
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);
void  __real_free(void *p);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t m);
void *__wrap_realloc(void *p, size_t n);
void  __wrap_free(void *p);

void *__wrap_malloc(size_t n) { g_allocs++; return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t m) { g_allocs++; return __real_calloc(n, m); }
void *__wrap_realloc(void *p, size_t n) { g_allocs++; return __real_realloc(p, n); }
void  __wrap_free(void *p) { if (p) g_allocs++; __real_free(p); }
#endif

/* ------------------------------------------------------------------ */
/* Fake dictionary — the two symbols skk_kana.c imports from skk_dict.c */

#define FK_MAX   8
#define FK_CANDS 8

typedef struct {
    const char *reading;
    int         blk;
    int         n;
    uint32_t    off[FK_CANDS];
    uint16_t    len[FK_CANDS];
} fk_ent_t;

static char       g_blob[512];
static size_t     g_blob_len;
static fk_ent_t   g_fk[FK_MAX];
static int        g_fk_n;
static skk_dict_t g_dict;

/* What the engine last asked the dictionary for. */
static char   g_q[64];
static size_t g_q_len;
static int    g_q_blk;
static int    g_q_calls;

static void fk_add(const char *reading, int blk, const char *const *cands)
{
    fk_ent_t *e = &g_fk[g_fk_n++];

    e->reading = reading;
    e->blk     = blk;
    e->n       = 0;
    for (int i = 0; cands[i] && e->n < FK_CANDS; i++) {
        size_t l = strlen(cands[i]);
        memcpy(g_blob + g_blob_len, cands[i], l);
        e->off[e->n] = (uint32_t)g_blob_len;
        e->len[e->n] = (uint16_t)l;
        e->n++;
        g_blob_len += l;
    }
}

static void fk_init(void)
{
    /* Long enough that ▼ + this overruns SKK_PREEDIT_MAX (192): a
       candidate is bounded by SKK_COMMIT_MAX (256), not by the preedit,
       so pre_add really does clip in v mode. 66 x 3 B = 198 B. */
    static char        longc[199];
    static const char *longcand[] = { longc, NULL };
    static const char *const kanji[] = { "漢字", "感じ", "幹事", NULL };
    static const char *const okuru[] = { "送", "贈", NULL };
    static const char *const motsu[] = { "持", NULL };
    /* Entries that exist only to give TAB something to complete to.
       "かん" is a proper prefix of the three below it, which is the case
       skk_complete() must skip (an entry equal to the prefix is not a
       completion of it). */
    static const char *const kan[]   = { "缶", NULL };
    static const char *const kanja[] = { "患者", NULL };
    static const char *const kangae[]= { "考え", NULL };

    g_blob_len = 0;
    g_fk_n     = 0;
    for (int i = 0; i < 66; i++)
        memcpy(longc + i * 3, "あ", 3);
    longc[198] = '\0';

    fk_add("かんじ", SKK_BLK_NASI, kanji);
    fk_add("おくr", SKK_BLK_ARI, okuru);
    fk_add("ためs", SKK_BLK_ARI, longcand);
    fk_add("もt", SKK_BLK_ARI, motsu);
    fk_add("かん", SKK_BLK_NASI, kan);
    fk_add("かんじゃ", SKK_BLK_NASI, kanja);
    fk_add("かんがえ", SKK_BLK_NASI, kangae);

    memset(&g_dict, 0, sizeof g_dict);
    g_dict.image.base = (const uint8_t *)g_blob;
    g_dict.image.len  = sizeof g_blob;
    g_dict.text       = (const uint8_t *)g_blob;
    g_dict.text_len   = g_blob_len;
}

/* The state machine converts through skk_lookup_stats(), not
   skk_lookup(), so that skk_stats_t.probes/fullcmp/cands/dropped get
   filled without a second search. The stub mirrors the real split:
   _stats is the body, skk_lookup() is it with a NULL `st`. There are no
   probes to report from a linear stub, but lookups/cands must move or
   this test would stop noticing if the wiring regressed. */
int skk_lookup_stats(const skk_dict_t *d, skk_blk_t blk, const char *reading,
                     size_t len, skk_cand_t *out, size_t cap, size_t *out_n,
                     skk_stats_t *st)
{
    (void)d;

    g_q_calls++;
    g_q_len = len < sizeof g_q ? len : sizeof g_q;
    memcpy(g_q, reading, g_q_len);
    g_q_blk = (int)blk;
    if (out_n) *out_n = 0;
    if (st) st->lookups++;

    for (int i = 0; i < g_fk_n; i++) {
        size_t n = 0;
        if (g_fk[i].blk != (int)blk) continue;
        if (strlen(g_fk[i].reading) != len) continue;
        if (memcmp(g_fk[i].reading, reading, len) != 0) continue;
        for (int j = 0; j < g_fk[i].n && n < cap; j++) {
            out[n].off = g_fk[i].off[j];
            out[n].len = g_fk[i].len[j];
            out[n].src = SKK_SRC_DICT;
            n++;
            if (st) st->cands++;
        }
        if (out_n) *out_n = n;
        return SKK_OK;
    }
    return SKK_OK; /* a miss is not an error */
}

int skk_lookup(const skk_dict_t *d, skk_blk_t blk, const char *reading,
               size_t len, skk_cand_t *out, size_t cap, size_t *out_n)
{
    return skk_lookup_stats(d, blk, reading, len, out, cap, out_n, NULL);
}

/* Prefix completion over the fake dictionary, in INSERTION order. The
   real one walks the sorted index, and that ordering is what
   test_skk_dict.c and test_skk_e2e.c check against real dictionaries;
   what belongs here is the state machine's cycling, wrapping and
   prefix-anchoring, which insertion order exercises just as well. */
int skk_complete(const skk_dict_t *d, skk_blk_t blk,
                 const char *prefix, size_t plen, size_t nth,
                 const char **out, size_t *out_len, uint32_t *probes)
{
    size_t seen = 0;

    (void)d;
    if (probes) (*probes)++;
    if (!prefix || plen == 0 || !out || !out_len) return SKK_ERR_ARG;

    for (int i = 0; i < g_fk_n; i++) {
        size_t rl = strlen(g_fk[i].reading);
        if (g_fk[i].blk != (int)blk) continue;
        if (rl <= plen) continue;                    /* not longer: no completion */
        if (memcmp(g_fk[i].reading, prefix, plen) != 0) continue;
        if (seen++ == nth) {
            *out     = g_fk[i].reading;
            *out_len = rl;
            return 1;
        }
    }
    return 0;
}

const char *skk_cand_text(const skk_dict_t *d, const skk_cand_t *c, size_t *len)
{
    if (len) *len = c->len;
    return (const char *)d->image.base + c->off;
}

/* ------------------------------------------------------------------ */
/* Driving the engine                                                   */

static skk_t  g_s;
static char   g_commit[2048];
static size_t g_commit_len;

static void begin(const char *name)
{
    g_case = name;
    skk_init(&g_s);
    skk_attach(&g_s, &g_dict);
    skk_enable(&g_s, true);
    g_commit_len = 0;
    g_commit[0]  = '\0';
    g_q_calls    = 0;
    g_q_len      = 0;
    g_q_blk      = -1;
}

/* One key. Enforces the two invariants the header states for EVERY
   return, then accumulates whatever was committed. */
static uint32_t key(const char *k, size_t n)
{
    uint32_t st = skk_key(&g_s, k, n);

    CHECK(st == 0 || (st & SKK_ST_CONSUMED), "st=0x%02x is non-zero without CONSUMED",
          st);
    if (st & SKK_ST_CANDS)
        CHECK(skk_sel(&g_s) == 0, "CANDS set but sel=%d", skk_sel(&g_s));
    if (st & SKK_ST_COMMIT) {
        size_t      cl = 0;
        const char *c  = skk_commit(&g_s, &cl);
        CHECK(cl > 0, "COMMIT set with an empty commit string");
        CHECK(c[cl] == '\0', "commit string is not NUL terminated");
        if (g_commit_len + cl < sizeof g_commit) {
            memcpy(g_commit + g_commit_len, c, cl);
            g_commit_len += cl;
            g_commit[g_commit_len] = '\0';
        }
    }
    return st;
}

/* Feed a string one UTF-8 character at a time, the way the keyboard
   delivers it. */
static uint32_t feed(const char *keys)
{
    uint32_t st = 0;
    size_t   n  = strlen(keys);

    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        while (j < n && ((unsigned char)keys[j] & 0xC0) == 0x80) j++;
        st = key(keys + i, j - i);
        i  = j;
    }
    return st;
}

/* A "\0name" token, whose length is load-bearing. */
static uint32_t tok(const char *name)
{
    char   buf[8];
    size_t n = strlen(name);

    buf[0] = '\0';
    memcpy(buf + 1, name, n);
    return key(buf, n + 1);
}

static const char *pre(size_t *len)
{
    return skk_preedit(&g_s, len);
}

static void eq_pre(const char *want, int line)
{
    size_t      n = 0;
    const char *p = pre(&n);
    eqs_(p, n, want, "preedit", line);
}
#define EQ_PRE(want) eq_pre((want), __LINE__)

static bool valid_utf8(const char *p, size_t n)
{
    for (size_t i = 0; i < n;) {
        size_t adv = 1;
        if (skk_utf8_decode(p, n, i, &adv) == 0xFFFD && adv == 1) return false;
        i += adv;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 1. UTF-8 helpers                                                     */

static void t_utf8(void)
{
    const char *s   = "aかん";  /* 1 + 3 + 3 */
    size_t      n   = strlen(s);
    size_t      adv = 0;

    g_case = "utf8";
    CHECK(n == 7, "fixture length %u", (unsigned)n);

    CHECK(skk_utf8_next(s, n, 0) == 1, "next(0)=%u", (unsigned)skk_utf8_next(s, n, 0));
    CHECK(skk_utf8_next(s, n, 1) == 4, "next(1)=%u", (unsigned)skk_utf8_next(s, n, 1));
    CHECK(skk_utf8_next(s, n, 4) == 7, "next(4)=%u", (unsigned)skk_utf8_next(s, n, 4));
    CHECK(skk_utf8_next(s, n, 7) == 7, "next(len) must clamp");
    CHECK(skk_utf8_next(s, n, 99) == 7, "next past the end must clamp");

    CHECK(skk_utf8_prev(s, n, 7) == 4, "prev(7)=%u", (unsigned)skk_utf8_prev(s, n, 7));
    CHECK(skk_utf8_prev(s, n, 4) == 1, "prev(4)=%u", (unsigned)skk_utf8_prev(s, n, 4));
    CHECK(skk_utf8_prev(s, n, 1) == 0, "prev(1)=%u", (unsigned)skk_utf8_prev(s, n, 1));
    CHECK(skk_utf8_prev(s, n, 0) == 0, "prev(0) must clamp");

    /* A stray continuation byte retreats/advances by exactly one and
       never swallows the character before it. */
    CHECK(skk_utf8_prev("ab\x80", 3, 3) == 2, "prev over a stray 0x80");
    CHECK(skk_utf8_next("\x80""ab", 3, 0) == 1, "next over a stray 0x80");
    /* A truncated sequence eats only itself. */
    CHECK(skk_utf8_next("\xe3\x81", 2, 0) == 2, "next over a truncated 3-byte");

    CHECK(skk_utf8_decode(s, n, 1, &adv) == 0x304B && adv == 3, "decode か");
    CHECK(skk_utf8_decode("\xe3\x81", 2, 0, &adv) == 0xFFFD && adv == 1,
          "truncated 3-byte must decode as FFFD/adv=1");
    CHECK(skk_utf8_decode("\x80", 1, 0, &adv) == 0xFFFD && adv == 1,
          "lone continuation byte");
    CHECK(skk_utf8_decode("\xc0\x80", 2, 0, &adv) == 0xFFFD && adv == 1,
          "overlong 2-byte NUL must be rejected");
    CHECK(skk_utf8_decode("\xe0\x80\x80", 3, 0, &adv) == 0xFFFD,
          "overlong 3-byte must be rejected");
    CHECK(skk_utf8_decode("\xed\xa0\x80", 3, 0, &adv) == 0xFFFD,
          "surrogate must be rejected");
    CHECK(skk_utf8_decode("\xf0\x9f\x98\x80", 4, 0, &adv) == 0x1F600 && adv == 4,
          "4-byte emoji");
    CHECK(skk_utf8_decode("\xef\xbf\xbd", 3, 0, &adv) == 0xFFFD && adv == 3,
          "a genuine U+FFFD decodes with adv=3");
}

/* ------------------------------------------------------------------ */
/* 2. Romaji -> kana                                                    */

/* Type `roma` into a fresh KANA-mode engine; the concatenation of every
   commit is the kana produced, and the preedit is whatever romaji is
   still pending. */
static void roma_case(const char *roma, const char *kana, const char *pending)
{
    char label[64];

    snprintf(label, sizeof label, "roma \"%s\"", roma);
    begin(label);
    feed(roma);
    eqs_(g_commit, g_commit_len, kana, "kana", __LINE__);
    {
        size_t      n = 0;
        const char *p = pre(&n);
        eqs_(p, n, pending, "pending romaji", __LINE__);
    }
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "mode drifted to %d", skk_mode(&g_s));
}

static void t_roma(void)
{
    static const struct {
        const char *roma, *kana, *pending;
    } C[] = {
        /* bare vowels */
        { "aiueo", "あいうえお", "" },
        /* basic gojuon + voiced */
        { "kakikukeko", "かきくけこ", "" },
        { "gagigugego", "がぎぐげご", "" },
        { "sasisuseso", "さしすせそ", "" },
        { "tatituteto", "たちつてと", "" },
        { "hahihuheho", "はひふへほ", "" },
        { "mamimumemo", "まみむめも", "" },
        { "rarirurero", "らりるれろ", "" },
        { "wawo", "わを", "" },
        /* youon */
        { "kya", "きゃ", "" },
        { "kyu", "きゅ", "" },
        { "kyo", "きょ", "" },
        { "nyu", "にゅ", "" },
        { "ryo", "りょ", "" },
        { "sha", "しゃ", "" },
        { "shi", "し", "" },
        { "chi", "ち", "" },
        { "tsu", "つ", "" },
        { "ja", "じゃ", "" },
        /* foreign sounds */
        { "fa", "ふぁ", "" },
        { "fi", "ふぃ", "" },
        { "ti", "ち", "" },   /* "ti" is chi; "thi" is the foreign てぃ */
        { "thi", "てぃ", "" },
        { "va", "ゔぁ", "" },
        { "vu", "ゔ", "" },
        { "je", "じぇ", "" },
        { "wi", "うぃ", "" },
        { "tsa", "つぁ", "" },
        /* sokuon: a doubled consonant is a small tsu, and it stays
           pending until the next vowel arrives */
        { "kk", "っ", "k" },
        { "kka", "っか", "" },
        { "kissa", "きっさ", "" },
        { "kitte", "きって", "" },
        { "matcha", "まっちゃ", "" },  /* the asymmetric t+c pair */
        { "ippai", "いっぱい", "" },
        { "xtu", "っ", "" },
        { "xtsu", "っ", "" },
        /* NB: "ltu"/"ltsu" are unreachable from KANA mode on purpose —
           'l' is the ASCII escape (see status/mode). They are reachable
           inside a ~ span, where 'l' is not a command; that is checked
           in midashi/ltu. */
        /* hatsuon: n resolves when it cannot be extended */
        { "n\n", "ん", "" },          /* Enter flushes a pending tail */
        { "nn\n", "ん", "" },
        { "n'", "ん", "" },
        { "xn", "ん", "" },
        { "nk", "ん", "k" },
        { "n.", "ん。", "" },
        { "nna", "んな", "" },
        { "konnichiha", "こんにちは", "" },
        { "gennki", "げんき", "" },
        { "annnai", "あんない", "" },
        { "nya", "にゃ", "" },
        /* punctuation with a kana form; "-" matters because ぺーじ is
           exactly the reading class §6.1's sort trap breaks */
        { "-.,", "ー。、", "" },
        { "pe-ji", "ぺーじ", "" },
        /* invalid combinations: the unusable prefix is dropped, never
           turned into kana, and never left half in the buffer */
        { "kz", "", "z" },
        { "kys", "", "s" },
        { "ky", "", "ky" },
        /* characters with no kana form stay themselves */
        { "1", "1", "" },
        { "?", "?", "" },
        { "a1", "あ1", "" },
    };

    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++)
        roma_case(C[i].roma, C[i].kana, C[i].pending);
}

/* ------------------------------------------------------------------ */
/* 3. Status bitmask                                                    */

static void t_status(void)
{
    uint32_t st;

    begin("status/kana");
    st = feed("k");
    CHECK(st == (SKK_ST_CONSUMED | SKK_ST_PREEDIT), "'k' -> 0x%02x", st);
    st = feed("a");
    CHECK(st == (SKK_ST_CONSUMED | SKK_ST_PREEDIT | SKK_ST_COMMIT), "'a' -> 0x%02x",
          st);
    EQS(g_commit, g_commit_len, "か", "commit");

    /* A control byte the IME has no use for is pure passthrough. */
    st = key("\x01", 1);
    CHECK(st == SKK_ST_PASSTHROUGH, "C-a -> 0x%02x, want PASSTHROUGH", st);
    st = key("\x1b[A", 3);
    CHECK(st == SKK_ST_PASSTHROUGH, "ESC-prefixed -> 0x%02x", st);
    st = tok("home");
    CHECK(st == SKK_ST_PASSTHROUGH, "unknown token -> 0x%02x", st);

    /* Mode changes report MODE. */
    begin("status/mode");
    st = feed("l");
    CHECK(st == (SKK_ST_CONSUMED | SKK_ST_PREEDIT | SKK_ST_MODE), "'l' -> 0x%02x",
          st);
    CHECK(skk_mode(&g_s) == SKK_MODE_ASCII, "'l' must reach ASCII");
    st = feed("a");
    CHECK(st == SKK_ST_PASSTHROUGH, "ASCII mode must pass everything through");

    CHECK(skk_set_mode(&g_s, SKK_MODE_KANA) == SKK_OK, "set_mode(KANA)");
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "back in KANA");
    CHECK(skk_set_mode(&g_s, SKK_MODE_MIDASHI) == SKK_ERR_ARG,
          "set_mode(MIDASHI) must be refused");
    CHECK(skk_set_mode(&g_s, SKK_MODE_SELECT) == SKK_ERR_ARG,
          "set_mode(SELECT) must be refused");

    /* q toggles katakana; kana then commits katakana directly. */
    begin("status/katakana");
    st = feed("q");
    CHECK(st == (SKK_ST_CONSUMED | SKK_ST_PREEDIT | SKK_ST_MODE), "'q' -> 0x%02x",
          st);
    CHECK(skk_mode(&g_s) == SKK_MODE_KATA, "'q' must reach KATA");
    feed("kana");
    EQS(g_commit, g_commit_len, "カナ", "katakana commit");
    st = feed("q");
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "'q' must toggle back");
    CHECK(st & SKK_ST_MODE, "toggling back must report MODE");

    /* Starting the ~ span reports MODE + PREEDIT, never COMMIT. */
    begin("status/midashi");
    st = feed("K");
    CHECK(st == (SKK_ST_CONSUMED | SKK_ST_PREEDIT | SKK_ST_MODE), "'K' -> 0x%02x",
          st);
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "'K' must open ~");
}

/* ------------------------------------------------------------------ */
/* 4. Passthrough is free: no state change (and, with the wrap build,    */
/*    no allocation)                                                     */

static void t_passthrough(void)
{
    skk_t before;
    skk_t after;

    begin("passthrough/kana");
    /* A clean state, no pending romaji and no commit outstanding. */
    memcpy(&before, &g_s, sizeof before);

    CHECK(key("\x01", 1) == 0, "C-a must pass through");
    CHECK(key("\x1b[C", 3) == 0, "Alt/ESC sequence must pass through");
    CHECK(tok("home") == 0, "an unknown token must pass through");
    CHECK(key("\x7f", 1) == 0, "DEL in KANA must pass through");
    CHECK(key("\n", 1) == 0, "Enter with nothing pending must pass through");
    CHECK(key(" ", 1) == 0, "Space with nothing pending must pass through");
    CHECK(key("あ", 3) == 0, "a bare kana in KANA mode passes through");

    memcpy(&after, &g_s, sizeof after);
    memset(&before.stats, 0, sizeof before.stats);
    memset(&after.stats, 0, sizeof after.stats);
    CHECK(memcmp(&before, &after, sizeof before) == 0,
          "passthrough changed engine state");

    /* Disabled: nothing is consumed at all, whatever the key. */
    begin("passthrough/disabled");
    skk_enable(&g_s, false);
    memcpy(&before, &g_s, sizeof before);
    CHECK(key("a", 1) == 0, "disabled must not consume 'a'");
    CHECK(key("K", 1) == 0, "disabled must not consume 'K'");
    CHECK(key(" ", 1) == 0, "disabled must not consume Space");
    memcpy(&after, &g_s, sizeof after);
    memset(&before.stats, 0, sizeof before.stats);
    memset(&after.stats, 0, sizeof after.stats);
    CHECK(memcmp(&before, &after, sizeof before) == 0,
          "a disabled engine changed state");
    CHECK(skk_enabled(&g_s) == false, "skk_enabled must report off");

    /* ASCII mode: armed, but transparent. */
    begin("passthrough/ascii");
    feed("l");
    memcpy(&before, &g_s, sizeof before);
    CHECK(feed("Hello, world!") == 0, "ASCII mode must consume nothing");
    memcpy(&after, &g_s, sizeof after);
    memset(&before.stats, 0, sizeof before.stats);
    memset(&after.stats, 0, sizeof after.stats);
    CHECK(memcmp(&before, &after, sizeof before) == 0,
          "ASCII mode changed engine state");
}

/* ------------------------------------------------------------------ */
/* TAB completion (design §6.8)                                         */

static void t_complete(void)
{
    /* One TAB extends the reading; more TABs cycle; the cycle wraps. The
       fake dictionary answers in insertion order, so after "かん" come
       かんじ, かんじゃ, かんがえ — and "かん" itself is skipped, because
       an entry equal to the prefix completes nothing. */
    begin("complete/cycle");
    feed("Kan");
    EQ_PRE("▽かn");           /* "n" is still extendable: a tail, not ん */
    feed("\t");
    EQ_PRE("▽かんじ");
    feed("\t");
    EQ_PRE("▽かんじゃ");
    feed("\t");
    EQ_PRE("▽かんがえ");
    feed("\t");
    EQ_PRE("▽かんじ");        /* wrapped */

    /* The prefix stays the ORIGINAL reading, not whatever TAB last put
       there — otherwise the second TAB would complete かんじ and the
       cycle would walk away from what the user typed. */
    begin("complete/prefix-is-stable");
    feed("Kan\t\t");
    EQ_PRE("▽かんじゃ");
    CHECK(g_s.comp_len == strlen("かん"), "comp_len=%u", (unsigned)g_s.comp_len);

    /* Any other key ends the cycle: TAB after it starts a new one from
       the reading as it now stands. */
    begin("complete/reset");
    feed("Kan\t");
    EQ_PRE("▽かんじ");
    feed("\t");
    EQ_PRE("▽かんじゃ");
    feed("\b");                /* not TAB: ends the cycle */
    EQ_PRE("▽かんじ");
    CHECK(g_s.comp_len == 0, "a non-TAB key must clear comp_len");
    feed("\t");
    EQ_PRE("▽かんじゃ");      /* new cycle, prefix is now かんじ */

    /* Nothing to complete: consumed, but the reading is untouched. */
    begin("complete/no-match");
    feed("Ku");
    EQ_PRE("▽く");             /* nothing in the fixture starts with く */
    CHECK((feed("\t") & SKK_ST_CONSUMED) != 0, "TAB must be consumed");
    EQ_PRE("▽く");
    CHECK(g_s.comp_len == 0, "a failed completion must not arm a cycle");

    /* An empty span has no prefix; TAB is still swallowed rather than
       reaching the app under an open preedit. */
    begin("complete/empty");
    feed("Q");
    EQ_PRE("▽");
    CHECK((feed("\t") & SKK_ST_CONSUMED) != 0, "TAB must be consumed");
    EQ_PRE("▽");

    /* A pending romaji tail is not part of the prefix, so it is flushed
       before the search — "かんj" would match nothing. */
    begin("complete/flushes-tail");
    feed("Kanj");
    EQ_PRE("▽かんj");
    feed("\t");
    EQ_PRE("▽かんじ");        /* "j" is a fragment: dropped, prefix is かん */

    /* Outside MIDASHI, TAB is not a completion key. */
    begin("complete/okuri");
    feed("KanG");
    CHECK(skk_mode(&g_s) == SKK_MODE_OKURI, "mode=%d", skk_mode(&g_s));
    feed("\t");
    CHECK(g_s.comp_len == 0, "OKURI must not start a completion");
}

/* ------------------------------------------------------------------ */
/* 5. The ~ span                                                        */

static void t_midashi(void)
{
    uint32_t st;

    begin("midashi/basic");
    feed("Kanji");
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "mode=%d", skk_mode(&g_s));
    EQ_PRE("▽かんじ");

    /* The romaji tail trails the span it is being typed into. "ny" is
       still extendable ("nya"), so it stays romaji rather than
       resolving the n to ん. */
    begin("midashi/tail");
    feed("Kany");
    EQ_PRE("▽かny");
    feed("a");
    EQ_PRE("▽かにゃ");

    /* 'l' is a command in KANA mode but plain romaji inside the span. */
    begin("midashi/ltu");
    feed("Kaltu");
    EQ_PRE("▽かっ");
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "'l' must not escape from ~");

    /* Enter commits the reading exactly as typed. */
    begin("midashi/enter");
    feed("Kanji");
    st = feed("\n");
    CHECK(st & SKK_ST_COMMIT, "Enter in ~ must commit, got 0x%02x", st);
    CHECK(st & SKK_ST_MODE, "leaving ~ must report MODE");
    EQS(g_commit, g_commit_len, "かんじ", "commit");
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "back to KANA");
    EQ_PRE("");

    /* Enter with nothing in the span must NOT announce a commit. */
    begin("midashi/enter-empty");
    feed("K");
    st = feed("\n");
    CHECK(!(st & SKK_ST_COMMIT), "empty ~ + Enter must not set COMMIT (0x%02x)", st);
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "the span must still close");

    /* q commits the reading as katakana. */
    begin("midashi/katakana");
    feed("Kanji");
    st = feed("q");
    CHECK(st & SKK_ST_COMMIT, "q in ~ must commit katakana, got 0x%02x", st);
    EQS(g_commit, g_commit_len, "カンジ", "katakana commit");
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "back to KANA");

    /* C-g throws the span away without committing anything. */
    begin("midashi/cancel");
    feed("Kanji");
    st = feed("\x07");
    CHECK(st & (SKK_ST_PREEDIT | SKK_ST_MODE), "C-g -> 0x%02x", st);
    CHECK(!(st & SKK_ST_COMMIT), "C-g must not commit");
    CHECK(g_commit_len == 0, "C-g produced \"%s\"", g_commit);
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "C-g returns to base mode");
    EQ_PRE("");

    /* ESC does the same. */
    begin("midashi/esc");
    feed("Kanji");
    st = key("\x1b", 1);
    CHECK(st & SKK_ST_CONSUMED, "ESC in ~ must be consumed");
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "ESC cancels the span");

    /* Arrows must not escape from under an open preedit. */
    begin("midashi/arrows");
    feed("Kanji");
    st = tok("left");
    CHECK(st & SKK_ST_CONSUMED, "arrows must be swallowed inside ~");
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "an arrow must not close ~");
    EQ_PRE("▽かんじ");

    /* TAB has no completion in v1 but must not leak to the app. */
    begin("midashi/tab");
    feed("Kanji");
    st = feed("\t");
    CHECK(st & SKK_ST_CONSUMED, "TAB inside ~ must be consumed");

    /* Space with an empty reading cancels rather than converting. */
    begin("midashi/space-empty");
    feed("K");
    st = feed(" ");
    CHECK(st & SKK_ST_CONSUMED, "Space in ~ must be consumed");
    CHECK(g_q_calls == 0, "an empty reading must not hit the dictionary");
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "empty ~ + Space cancels");

    /* An uppercase letter cannot start the okurigana before there is a
       reading: "KA" is ▽か, not ▽*か. */
    begin("midashi/upper-at-start");
    feed("KA");
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "mode=%d, want MIDASHI",
          skk_mode(&g_s));
    EQ_PRE("▽か");
    begin("midashi/upper-mid-roma");
    feed("KyO");
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "mode=%d, want MIDASHI",
          skk_mode(&g_s));
    EQ_PRE("▽きょ");

    /* A kana delivered whole (soft keyboard) lands in the reading. */
    begin("midashi/direct-kana");
    feed("K");
    key("\x07", 1); /* drop the pending "k" */
    feed("A");
    key("ん", 3);
    EQ_PRE("▽あん");

    /* A miss leaves the span alone so the user can keep typing. */
    begin("midashi/miss");
    feed("Nai");
    st = feed(" ");
    CHECK(g_q_calls == 1, "Space must query once, did %d", g_q_calls);
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "a miss must stay in ~");
    CHECK(!(st & SKK_ST_CANDS), "a miss must not announce candidates");
    EQ_PRE("▽ない");
}

/* ------------------------------------------------------------------ */
/* 6. Okurigana                                                         */

static void t_okuri(void)
{
    uint32_t st;

    begin("okuri/basic");
    feed("Oku");
    EQ_PRE("▽おく");
    feed("R");
    CHECK(skk_mode(&g_s) == SKK_MODE_OKURI, "R must open the okurigana");
    EQ_PRE("▽おく*r"); /* the tail trails the okurigana it belongs to */
    st = feed("i");
    CHECK(st & SKK_ST_CANDS, "a complete okurigana converts by itself (0x%02x)", st);
    CHECK(g_q_blk == SKK_BLK_ARI, "okuri-ari block expected, got %d", g_q_blk);
    EQS(g_q, g_q_len, "おくr", "dictionary key");
    CHECK(skk_mode(&g_s) == SKK_MODE_SELECT, "mode=%d", skk_mode(&g_s));
    EQ_PRE("▼送り");
    st = feed("\n");
    CHECK(st & SKK_ST_COMMIT, "Enter must commit the selection");
    EQS(g_commit, g_commit_len, "送り", "commit");

    /* The stem is the pending consonant, so "MotTe" keys on もt and
       builds って — not もっ + て. */
    begin("okuri/sokuon-stem");
    feed("MotTe");
    EQS(g_q, g_q_len, "もt", "dictionary key");
    CHECK(g_q_blk == SKK_BLK_ARI, "okuri-ari block expected, got %d", g_q_blk);
    EQ_PRE("▼持って");
    feed("\n");
    EQS(g_commit, g_commit_len, "持って", "commit");

    /* The okurigana does not fire until its first kana is complete. */
    begin("okuri/incomplete");
    feed("OkuR");
    CHECK(g_q_calls == 0, "a bare consonant must not query the dictionary");
    CHECK(skk_mode(&g_s) == SKK_MODE_OKURI, "still in the okurigana");

    /* Backspace unwinds the okurigana, then the marker itself. */
    begin("okuri/backspace");
    feed("TaRi");   /* not in the fake dictionary: stays in OKURI */
    CHECK(skk_mode(&g_s) == SKK_MODE_OKURI, "a miss must stay in the okurigana");
    EQ_PRE("▽た*り");
    feed("\b");
    EQ_PRE("▽た");
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "an emptied okurigana returns to ~");
    feed("\b");
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "an emptied ~ closes");

    /* Enter in the okurigana commits reading + okurigana as typed. */
    begin("okuri/enter");
    feed("TaRi");
    feed("\n");
    EQS(g_commit, g_commit_len, "たり", "commit");
}

/* ------------------------------------------------------------------ */
/* 7. Candidate selection                                               */

static void t_select(void)
{
    uint32_t st;

    begin("select/cycle");
    feed("Kanji");
    st = feed(" ");
    CHECK(st & SKK_ST_CANDS, "Space must convert, got 0x%02x", st);
    CHECK(st & SKK_ST_MODE, "entering v must report MODE");
    CHECK(!(st & SKK_ST_SEL), "CANDS already implies sel=0; SEL is redundant");
    CHECK(g_q_blk == SKK_BLK_NASI, "okuri-nasi block expected, got %d", g_q_blk);
    EQS(g_q, g_q_len, "かんじ", "dictionary key");
    CHECK(skk_cand_count(&g_s) == 3, "cand_count=%d", skk_cand_count(&g_s));
    CHECK(skk_sel(&g_s) == 0, "sel=%d", skk_sel(&g_s));
    EQ_PRE("▼漢字");
    {
        size_t      n = 0;
        const char *c = skk_cand(&g_s, 1, &n);
        eqs_(c ? c : "", n, "感じ", "candidate 1", __LINE__);
        CHECK(skk_cand(&g_s, 3, &n) == NULL, "out-of-range candidate must be NULL");
        CHECK(skk_cand(&g_s, -1, &n) == NULL, "negative index must be NULL");
    }

    st = feed(" ");
    CHECK(st == (SKK_ST_CONSUMED | SKK_ST_SEL | SKK_ST_PREEDIT),
          "Space in v -> 0x%02x", st);
    CHECK(skk_sel(&g_s) == 1, "sel=%d", skk_sel(&g_s));
    EQ_PRE("▼感じ");
    feed(" ");
    CHECK(skk_sel(&g_s) == 2, "sel=%d", skk_sel(&g_s));

    /* Past the last candidate there is nowhere to go (no dictionary
       registration in v1) — but the key is still ours, or the app would
       insert a space into the middle of a conversion. */
    st = feed(" ");
    CHECK(st & SKK_ST_CONSUMED, "Space at the last candidate -> 0x%02x", st);
    CHECK(skk_sel(&g_s) == 2, "sel must clamp, got %d", skk_sel(&g_s));

    /* x walks back, and off the front it returns to the ~ span. */
    feed("x");
    CHECK(skk_sel(&g_s) == 1, "sel=%d", skk_sel(&g_s));
    feed("x");
    CHECK(skk_sel(&g_s) == 0, "sel=%d", skk_sel(&g_s));
    st = feed("x");
    CHECK(st & SKK_ST_MODE, "leaving v must report MODE");
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "mode=%d", skk_mode(&g_s));
    CHECK(skk_sel(&g_s) == -1, "sel must be -1 outside v, got %d", skk_sel(&g_s));
    CHECK(skk_cand_count(&g_s) == 0, "candidates must be dropped");
    EQ_PRE("▽かんじ");
    CHECK(g_commit_len == 0, "walking back must not commit");

    /* Arrow keys select too. */
    begin("select/arrows");
    feed("Kanji ");
    CHECK(tok("down") & SKK_ST_SEL, "down must move the selection");
    CHECK(skk_sel(&g_s) == 1, "sel=%d", skk_sel(&g_s));
    CHECK(tok("up") & SKK_ST_SEL, "up must move the selection");
    CHECK(skk_sel(&g_s) == 0, "sel=%d", skk_sel(&g_s));

    /* C-g in v goes back to ~ without committing. */
    begin("select/cancel");
    feed("Kanji ");
    feed("\x07");
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "C-g must return to ~");
    CHECK(g_commit_len == 0, "C-g must not commit");

    /* Enter commits the selection. */
    begin("select/enter");
    feed("Kanji  ");
    st = feed("\n");
    CHECK(st == (SKK_ST_CONSUMED | SKK_ST_COMMIT | SKK_ST_PREEDIT | SKK_ST_MODE),
          "Enter in v -> 0x%02x", st);
    EQS(g_commit, g_commit_len, "感じ", "commit");
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "back to base mode");
    EQ_PRE("");

    /* Any other key commits the selection and then acts: one call sets
       both COMMIT and PREEDIT. */
    begin("select/implicit-commit");
    feed("Kanji ");
    st = feed("k");
    CHECK(st & SKK_ST_COMMIT, "the pending key must commit first (0x%02x)", st);
    CHECK(st & SKK_ST_PREEDIT, "... and then act (0x%02x)", st);
    EQS(g_commit, g_commit_len, "漢字", "commit");
    EQ_PRE("k");
    feed("a");
    EQS(g_commit, g_commit_len, "漢字か", "commit after the re-fed key");

    /* An uppercase key commits and opens a new span in the same call. */
    begin("select/implicit-then-midashi");
    feed("Kanji ");
    st = feed("K");
    CHECK((st & SKK_ST_COMMIT) && (st & SKK_ST_MODE), "0x%02x", st);
    EQS(g_commit, g_commit_len, "漢字", "commit");
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "a new ~ must be open");

    /* A whole kana arriving in v commits and is appended behind it. */
    begin("select/implicit-kana");
    feed("Kanji ");
    key("。", 3);
    EQS(g_commit, g_commit_len, "漢字。", "commit");
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "mode=%d", skk_mode(&g_s));

    /* Walking back from a conversion that had an okurigana must restore
       the okurigana, not silently strand it. */
    begin("select/back-to-okuri");
    feed("OkuRi");
    CHECK(skk_mode(&g_s) == SKK_MODE_SELECT, "mode=%d", skk_mode(&g_s));
    feed("x");
    CHECK(skk_mode(&g_s) == SKK_MODE_OKURI, "mode=%d, want OKURI", skk_mode(&g_s));
    EQ_PRE("▽おく*り");
    feed(" ");
    CHECK(skk_mode(&g_s) == SKK_MODE_SELECT, "Space must convert again");
    EQS(g_q, g_q_len, "おくr", "dictionary key after walking back");
}

/* ------------------------------------------------------------------ */
/* 8. Backspace and UTF-8 boundaries                                    */

static void t_backspace(void)
{
    begin("backspace/reading");
    feed("Kanjik");
    EQ_PRE("▽かんじk");
    feed("\b");
    EQ_PRE("▽かんじ");   /* the romaji tail goes first, one byte */
    feed("\b");
    EQ_PRE("▽かん");     /* then whole characters, 3 bytes at a time */
    feed("\b");
    EQ_PRE("▽か");
    feed("\b");
    EQ_PRE("");
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "an emptied ~ closes");
    CHECK(g_commit_len == 0, "backspace must never commit");

    /* DEL behaves as BS inside the span. */
    begin("backspace/del");
    feed("Kanji");
    key("\x7f", 1);
    EQ_PRE("▽かん");

    /* A 4-byte character comes off in one piece. */
    begin("backspace/4byte");
    feed("K");
    key("\x07", 1);
    feed("A");
    key("\xf0\x9f\x98\x80", 4);
    EQ_PRE("▽あ\xf0\x9f\x98\x80");
    feed("\b");
    EQ_PRE("▽あ");

    /* In KANA mode backspace only ever eats a pending romaji tail; with
       nothing pending it belongs to the app. */
    begin("backspace/kana");
    feed("ky");
    EQ_PRE("ky");
    feed("\b");
    EQ_PRE("k");
    feed("\b");
    EQ_PRE("");
    CHECK(key("\b", 1) == 0, "BS with nothing pending must pass through");
}

/* ------------------------------------------------------------------ */
/* 9. Buffer limits                                                     */

static void t_limits(void)
{
    size_t      n = 0;
    const char *p;

    /* Overflow the reading: it must stop growing, stay whole characters,
       and never wrap or truncate mid-sequence. */
    begin("limits/reading");
    feed("K");
    for (int i = 0; i < 200; i++) feed("a");
    p = pre(&n);
    CHECK(g_s.reading_len <= SKK_READING_MAX, "reading_len=%u", g_s.reading_len);
    CHECK(g_s.reading_len == 96, "reading_len=%u, want a whole-character fill",
          g_s.reading_len);
    CHECK(valid_utf8(p, n), "the preedit is not valid UTF-8");
    CHECK(n == 3 + 96, "preedit is %u B", (unsigned)n);
    CHECK(g_commit_len == 0, "an overflowing reading must not commit");
    feed("\n");
    CHECK(g_commit_len == 96, "commit is %u B", (unsigned)g_commit_len);
    CHECK(valid_utf8(g_commit, g_commit_len), "the commit is not valid UTF-8");

    /* Overflow the okurigana the same way. */
    begin("limits/okuri");
    feed("TaRi");
    for (int i = 0; i < 40; i++) feed("ri");
    CHECK(g_s.okuri_len <= SKK_OKURI_MAX, "okuri_len=%u", g_s.okuri_len);
    p = pre(&n);
    CHECK(valid_utf8(p, n), "the preedit is not valid UTF-8");
    CHECK(n < SKK_PREEDIT_MAX, "preedit ran past its buffer");

    /* Overflow the romaji buffer: nothing in the table is longer than
       four bytes, so a long consonant run must resolve, not overrun. */
    begin("limits/roma");
    feed("kkkkkkkkkkkkkkkkkkkk");
    CHECK(g_s.roma_len <= SKK_ROMA_MAX, "roma_len=%u", g_s.roma_len);
    CHECK(g_s.roma_len == 1, "roma_len=%u, want the last k pending", g_s.roma_len);
    CHECK(g_commit_len == 19 * 3, "%u B of small tsu", (unsigned)g_commit_len);

    /* A long reading plus its okurigana still fits the commit buffer. */
    begin("limits/commit");
    feed("K");
    for (int i = 0; i < 200; i++) feed("a");
    feed("Ri");
    feed("\n");
    CHECK(g_commit_len < SKK_COMMIT_MAX, "commit_len=%u", (unsigned)g_commit_len);
    CHECK(valid_utf8(g_commit, g_commit_len), "the commit is not valid UTF-8");
}

/* ------------------------------------------------------------------ */
/* 10. Malformed UTF-8 must never reach a buffer                        */

static void t_badutf8(void)
{
    uint32_t st;
    size_t   n = 0;

    /* In KANA mode a byte the engine cannot make sense of is the app's
       problem, and nothing moves. */
    begin("badutf8/kana");
    st = key("\x80", 1);
    CHECK(st == 0, "a lone continuation byte -> 0x%02x", st);
    st = key("\xe3\x81", 2);
    CHECK(st == 0, "a truncated sequence -> 0x%02x", st);
    CHECK(g_s.reading_len == 0 && g_s.roma_len == 0, "state moved");

    /* Inside the span it is swallowed rather than let into the reading:
       a stray continuation byte there would make the preedit
       un-renderable and the dictionary key garbage. */
    begin("badutf8/midashi");
    feed("Kanji");
    st = key("\x80", 1);
    CHECK(st & SKK_ST_CONSUMED, "malformed byte inside ~ -> 0x%02x", st);
    EQ_PRE("▽かんじ");
    st = key("\xe3\x81", 2);
    CHECK(st & SKK_ST_CONSUMED, "truncated sequence inside ~ -> 0x%02x", st);
    EQ_PRE("▽かんじ");
    st = key("\xff\xfe", 2);
    CHECK(st & SKK_ST_CONSUMED, "invalid lead byte inside ~ -> 0x%02x", st);
    EQ_PRE("▽かんじ");
    CHECK(valid_utf8((const char *)g_s.reading, g_s.reading_len),
          "the reading is not valid UTF-8");

    /* A genuine U+FFFD is a real character and goes in. */
    begin("badutf8/replacement-char");
    feed("K");
    key("\x07", 1);
    feed("A");
    key("\xef\xbf\xbd", 3);
    (void)pre(&n);
    CHECK(g_s.reading_len == 6, "reading_len=%u", g_s.reading_len);
    CHECK(valid_utf8((const char *)g_s.reading, g_s.reading_len),
          "the reading is not valid UTF-8");

    /* Every accessor survives a NULL engine. */
    g_case = "badutf8/null";
    CHECK(skk_key(NULL, "a", 1) == 0, "skk_key(NULL)");
    CHECK(skk_mode(NULL) == SKK_MODE_ASCII, "skk_mode(NULL)");
    CHECK(skk_preedit(NULL, &n) != NULL && n == 0, "skk_preedit(NULL)");
    CHECK(skk_commit(NULL, &n) != NULL && n == 0, "skk_commit(NULL)");
    CHECK(skk_cand_count(NULL) == 0, "skk_cand_count(NULL)");
    CHECK(skk_sel(NULL) == -1, "skk_sel(NULL)");
    CHECK(skk_cand(NULL, 0, &n) == NULL, "skk_cand(NULL)");
    CHECK(skk_set_mode(NULL, SKK_MODE_KANA) == SKK_ERR_ARG, "skk_set_mode(NULL)");
    skk_init(NULL);
    skk_reset(NULL);
    skk_enable(NULL, true);
    skk_attach(NULL, NULL);
    CHECK(skk_enabled(NULL) == false, "skk_enabled(NULL)");

    /* A zero-length key is not a key. */
    begin("badutf8/empty-key");
    CHECK(key("", 0) == 0, "an empty key must pass through");
    CHECK(skk_key(&g_s, NULL, 4) == 0, "a NULL key must pass through");
}

/* ------------------------------------------------------------------ */
/* 11. Reset / detach / stats                                           */

static void t_misc(void)
{
    skk_stats_t st;

    /* reset throws the span away without committing it. */
    begin("misc/reset");
    feed("Kanji ");
    skk_reset(&g_s);
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "reset must return to base mode");
    CHECK(skk_cand_count(&g_s) == 0, "reset must drop the candidates");
    CHECK(skk_sel(&g_s) == -1, "reset must drop the selection");
    EQ_PRE("");
    {
        size_t n = 1;
        CHECK(skk_commit(&g_s, &n) && n == 0, "reset must clear the commit");
    }

    /* set_mode keeps ASCII reachable and remembers the base mode. */
    begin("misc/set-mode");
    feed("q");
    CHECK(skk_mode(&g_s) == SKK_MODE_KATA, "q -> KATA");
    feed("K");
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "K opens ~ from KATA too");
    feed("a\n");
    CHECK(skk_mode(&g_s) == SKK_MODE_KATA, "the span returns to KATA");
    EQS(g_commit, g_commit_len, "か", "the reading stays hiragana");
    CHECK(skk_set_mode(&g_s, SKK_MODE_ASCII) == SKK_OK, "set_mode(ASCII)");
    CHECK(feed("abc") == 0, "ASCII passes through");
    CHECK(skk_set_mode(&g_s, SKK_MODE_KANA) == SKK_OK, "set_mode(KANA)");
    CHECK(skk_mode(&g_s) == SKK_MODE_KANA, "mode=%d", skk_mode(&g_s));

    /* Without a dictionary, conversion must not misbehave. */
    begin("misc/no-dict");
    skk_attach(&g_s, NULL);
    feed("Kanji");
    CHECK(feed(" ") & SKK_ST_CONSUMED, "Space must still be consumed");
    CHECK(skk_mode(&g_s) == SKK_MODE_MIDASHI, "no dictionary: stay in ~");
    CHECK(g_q_calls == 0, "no dictionary: no query");
    feed("\n");
    EQS(g_commit, g_commit_len, "かんじ", "the kana is still available");

    /* Counters. */
    begin("misc/stats");
    skk_stats_reset(&g_s);
    feed("Kanji");            /* 5 keys, all consumed */
    feed(" ");                /* 1 key, 1 lookup */
    feed("\n");               /* 1 key, 1 commit */
    key("\x01", 1);           /* 1 key, not consumed */
    skk_stats(&g_s, &st);
    CHECK(st.keys == 8, "keys=%u", st.keys);
    CHECK(st.consumed == 7, "consumed=%u", st.consumed);
    CHECK(st.lookups == 1, "lookups=%u", st.lookups);
    CHECK(st.cands == 3, "cands=%u", st.cands);
    CHECK(st.commits == 1, "commits=%u", st.commits);
    skk_stats_reset(&g_s);
    skk_stats(&g_s, &st);
    CHECK(st.keys == 0 && st.commits == 0, "stats_reset");
}

/* ------------------------------------------------------------------ */
/* 12. Soak: arbitrary bytes, including malformed UTF-8, must never     */
/*     break an invariant. Worth its weight under -fsanitize=address.   */

static uint32_t g_rng = 0x12345678u;
static uint32_t urand(void)
{
    g_rng = 1664525u * g_rng + 1013904223u;
    return g_rng >> 8;
}

static bool invariants(uint32_t st, const char **why)
{
    size_t n = 0;

    if (st && !(st & SKK_ST_CONSUMED)) { *why = "non-zero status without CONSUMED"; return false; }
    if ((st & SKK_ST_CANDS) && skk_sel(&g_s) != 0) { *why = "CANDS without sel==0"; return false; }
    if (g_s.mode > SKK_MODE_SELECT) { *why = "mode out of range"; return false; }
    if (g_s.roma_len > SKK_ROMA_MAX) { *why = "roma overflow"; return false; }
    for (unsigned i = 0; i < g_s.roma_len; i++)
        if (g_s.roma[i] < 0x21 || g_s.roma[i] > 0x7e) { *why = "non-printable in roma"; return false; }
    if (g_s.reading_len > SKK_READING_MAX) { *why = "reading overflow"; return false; }
    if (!valid_utf8(g_s.reading, g_s.reading_len)) { *why = "reading is not UTF-8"; return false; }
    if (g_s.okuri_len > SKK_OKURI_MAX) { *why = "okuri overflow"; return false; }
    if (!valid_utf8(g_s.okuri, g_s.okuri_len)) { *why = "okuri is not UTF-8"; return false; }
    if (g_s.cand_n > SKK_CAND_MAX) { *why = "cand overflow"; return false; }
    if (g_s.sel < -1 || (g_s.cand_n && g_s.sel >= (int)g_s.cand_n) ||
        (!g_s.cand_n && g_s.sel != -1)) { *why = "sel out of range"; return false; }
    if ((g_s.mode == SKK_MODE_SELECT) != (g_s.sel >= 0)) { *why = "sel disagrees with mode"; return false; }
    if (g_s.preedit_len >= SKK_PREEDIT_MAX) { *why = "preedit overflow"; return false; }
    if (g_s.preedit[g_s.preedit_len] != '\0') { *why = "preedit not terminated"; return false; }
    if (g_s.commit_len >= SKK_COMMIT_MAX) { *why = "commit overflow"; return false; }
    if (g_s.commit[g_s.commit_len] != '\0') { *why = "commit not terminated"; return false; }
    (void)skk_preedit(&g_s, &n);
    if (n != g_s.preedit_len) { *why = "preedit accessor disagrees"; return false; }
    if (!valid_utf8(g_s.commit, g_s.commit_len)) { *why = "commit is not UTF-8"; return false; }
    /* The preedit is display text: a partial sequence in it would make
       the cell renderer draw garbage. */
    if (!valid_utf8(g_s.preedit, g_s.preedit_len)) { *why = "preedit is not UTF-8"; return false; }
    return true;
}

/* The spans must PARTITION the preedit — no gaps, no overlaps, no empty
   entries, ending exactly at the last byte. A renderer walks them
   instead of the string, so a hole is a hole in what gets painted. */
static void spans_partition(const char *what)
{
    const skk_span_t *sp = NULL;
    int n = skk_preedit_spans(&g_s, &sp);
    size_t plen = 0;
    const char *pre = skk_preedit(&g_s, &plen);
    size_t at = 0;
    int i;

    (void)pre;
    for (i = 0; i < n; i++) {
        CHECK(sp[i].off == at, "%s: span %d starts at %u, want %u",
              what, i, (unsigned)sp[i].off, (unsigned)at);
        CHECK(sp[i].len > 0, "%s: span %d is empty", what, i);
        at += sp[i].len;
    }
    CHECK(at == plen, "%s: spans cover %u of %u bytes",
          what, (unsigned)at, (unsigned)plen);
}

/* kinds[] in order, -1 terminated */
static void spans_are(const char *what, const int *kinds)
{
    const skk_span_t *sp = NULL;
    int n = skk_preedit_spans(&g_s, &sp);
    int i = 0;

    while (kinds[i] >= 0)
        i++;
    CHECK(n == i, "%s: %d spans, want %d", what, n, i);
    if (n != i)
        return;
    for (i = 0; kinds[i] >= 0; i++)
        CHECK(sp[i].kind == (uint16_t)kinds[i],
              "%s: span %d kind=%u, want %d", what, i,
              (unsigned)sp[i].kind, kinds[i]);
    spans_partition(what);
}

/* Byte-compare one span against a literal. */
static void span_is(const char *what, int i, const char *want)
{
    const skk_span_t *sp = NULL;
    int n = skk_preedit_spans(&g_s, &sp);
    const char *pre = skk_preedit(&g_s, NULL);

    if (i >= n) {
        CHECK(0, "%s: no span %d (have %d)", what, i, n);
        return;
    }
    CHECK(sp[i].len == strlen(want) &&
          memcmp(pre + sp[i].off, want, sp[i].len) == 0,
          "%s: span %d = \"%.*s\", want \"%s\"", what, i,
          (int)sp[i].len, pre + sp[i].off, want);
}

static void t_spans(void)
{
    static const int K_NONE[]    = { -1 };
    static const int K_ROMA[]    = { SKK_SPAN_ROMA, -1 };
    static const int K_READ[]    = { SKK_SPAN_MARK, SKK_SPAN_READING, -1 };
    static const int K_READ_R[]  = { SKK_SPAN_MARK, SKK_SPAN_READING,
                                     SKK_SPAN_ROMA, -1 };
    static const int K_OKURI[]   = { SKK_SPAN_MARK, SKK_SPAN_READING,
                                     SKK_SPAN_SEP, SKK_SPAN_OKURI, -1 };
    static const int K_SELECT[]  = { SKK_SPAN_MARK, SKK_SPAN_CAND,
                                     SKK_SPAN_OKURI, -1 };
    size_t plen;

    /* Nothing composing: nothing to paint. */
    begin("spans/idle");
    spans_are("idle", K_NONE);

    /* Plain kana leaves only the romaji that has not resolved yet. */
    begin("spans/roma");
    feed("ky");
    EQ_PRE("ky");
    spans_are("roma", K_ROMA);
    span_is("roma", 0, "ky");

    begin("spans/midashi");
    feed("Kanji");
    EQ_PRE("▽かんじ");
    spans_are("midashi", K_READ);
    span_is("midashi", 0, "▽");     /* the real U+25BD, not an ASCII stand-in */
    span_is("midashi", 1, "かんじ");

    /* An unresolved tail is its own span, so the renderer can leave it
       undecorated while the settled reading is underlined. */
    begin("spans/midashi-tail");
    feed("Kany");
    EQ_PRE("▽かny");
    spans_are("midashi-tail", K_READ_R);
    span_is("midashi-tail", 1, "か");
    span_is("midashi-tail", 2, "ny");

    /* An empty part records NOTHING rather than a zero-length span:
       "K" opens the span but its reading is still empty, and only the
       unresolved "k" follows the marker. */
    begin("spans/empty-reading");
    feed("K");
    EQ_PRE("▽k");
    spans_are("empty-reading", (const int[]){ SKK_SPAN_MARK,
                                              SKK_SPAN_ROMA, -1 });
    span_is("empty-reading", 1, "k");

    /* The okurigana consonant is romaji until its kana lands, so the
       '*' is already its own span while "r" still trails. */
    begin("spans/okuri-sep");
    feed("OkuR");
    EQ_PRE("▽おく*r");
    spans_are("okuri-sep", (const int[]){ SKK_SPAN_MARK, SKK_SPAN_READING,
                                          SKK_SPAN_SEP, SKK_SPAN_ROMA, -1 });
    span_is("okuri-sep", 1, "おく");
    span_is("okuri-sep", 2, "*");
    span_is("okuri-sep", 3, "r");
    (void)K_OKURI; /* the ~ form with kana in the okurigana converts on
                      the spot, so it never reaches a repaint */

    /* ---- THE CASE A PARSER CANNOT DO ----
       In v the candidate and the okurigana are concatenated with no
       separator, so "▼送る" gives a downstream reader no way to know
       whether る belongs to the candidate or is the okurigana. The
       spans say. */
    begin("spans/select");
    feed("OkuRu");
    CHECK(skk_mode(&g_s) == SKK_MODE_SELECT, "mode=%d", skk_mode(&g_s));
    EQ_PRE("▼送る");
    spans_are("select", K_SELECT);
    span_is("select", 0, "▼");
    span_is("select", 1, "送");   /* candidate: NOT "送る" */
    span_is("select", 2, "る");   /* okurigana */

    /* Walking the candidates re-spans against the new one. */
    feed(" ");
    EQ_PRE("▼贈る");
    spans_are("select-next", K_SELECT);
    span_is("select-next", 1, "贈");
    span_is("select-next", 2, "る");

    /* Committing leaves nothing composed. */
    feed("\n");
    spans_are("after-commit", K_NONE);

    /* A DROPPED APPEND. A candidate is bounded by SKK_COMMIT_MAX (256),
       not by SKK_PREEDIT_MAX (192), so v mode is where pre_add refuses
       one — and it drops the whole append rather than truncating it. A
       span that claimed those bytes anyway would send the renderer far
       past the end of the string, so the partition check is the net.
       First prove the drop really happened, or it proves nothing. */
    begin("spans/dropped");
    feed("TameSu");
    CHECK(skk_mode(&g_s) == SKK_MODE_SELECT, "mode=%d", skk_mode(&g_s));
    plen = strlen(skk_preedit(&g_s, NULL));
    CHECK(plen < 198, "the 198 B candidate was not dropped (preedit %u B) "
          "— the checks below would prove nothing", (unsigned)plen);
    spans_are("dropped", (const int[]){ SKK_SPAN_MARK, SKK_SPAN_OKURI, -1 });
    span_is("dropped", 1, "す");
}

static void t_soak(void)
{
    /* Random bytes almost never spell a reading the dictionary has, so
       conversions are scripted in on purpose: the soak has to reach v
       mode with and without an okurigana, not just wander around ~. */
    static const char *const script[] = {
        "Kanji ", "Kanji  \n", "Kanji x", "Kanji xx", "OkuRi", "MotTe\n",
        "Kanji q", "Kanji \b", "Kanji \x07", "Kanji K", "OkuRi x ",
    };
    static const char *const toks[] = { "left", "right", "up", "down",
                                        "esc",  "del",   "home" };
    const char *why = NULL;

    begin("soak");
    for (int i = 0; i < 200000; i++) {
        char     buf[8];
        size_t   len;
        uint32_t st;
        uint32_t r = urand();

        memset(buf, 0, sizeof buf);
        if ((r >> 20) % 32u == 0u) { /* a scripted conversion */
            const char *sc = script[(r >> 3) % (sizeof script / sizeof script[0])];
            /* A stray 'l' will have parked the engine in ASCII, which
               only the app can leave — exactly as skk_mode_t documents. */
            skk_set_mode(&g_s, ((r >> 16) & 1u) ? SKK_MODE_KANA : SKK_MODE_KATA);
            for (size_t j = 0; sc[j]; j++) {
                st = skk_key(&g_s, sc + j, 1);
                if (!invariants(st, &why)) {
                    CHECK(false, "iteration %d, script \"%s\"[%u]: %s", i, sc,
                          (unsigned)j, why);
                    return;
                }
            }
            continue;
        }
        switch (r % 8u) {
        case 0: /* a "\0name" token */
            buf[0] = '\0';
            len = strlen(toks[(r >> 3) % 7u]);
            memcpy(buf + 1, toks[(r >> 3) % 7u], len);
            len++;
            break;
        case 1: /* a whole kana */
            len = 3;
            memcpy(buf, "\xe3\x81\x82", 3);
            buf[2] = (char)(0x81 + (int)((r >> 3) % 0x13u));
            break;
        case 2: /* arbitrary bytes, very often malformed UTF-8 */
            len = 1 + (r >> 3) % 4u;
            for (size_t j = 0; j < len; j++) buf[j] = (char)(urand() & 0xff);
            break;
        default: /* one printable/control ASCII byte, the common case */
            len = 1;
            buf[0] = (char)(0x07 + (r >> 3) % 0x79u);
            break;
        }

        st = skk_key(&g_s, buf, len);
        if (!invariants(st, &why)) {
            CHECK(false, "iteration %d (%s): key len %u [%02x %02x %02x %02x]",
                  i, why, (unsigned)len, (unsigned char)buf[0],
                  (unsigned char)buf[1], (unsigned char)buf[2],
                  (unsigned char)buf[3]);
            return;
        }
        if ((i % 977) == 0) skk_reset(&g_s);
        if ((i % 61) == 0) skk_set_mode(&g_s, SKK_MODE_KANA);
    }
    CHECK(true, "soak");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    fk_init();

    t_utf8();
    t_roma();
    t_status();
    t_passthrough();
    t_midashi();
    t_complete();
    t_okuri();
    t_select();
    t_backspace();
    t_limits();
    t_badutf8();
    t_misc();
    t_spans();
    t_soak();

#ifdef SKK_WRAP_ALLOC
    g_case = "alloc";
    CHECK(g_allocs == 0, "%ld allocator calls; skk_core must not allocate",
          g_allocs);
    printf("allocator calls: %ld\n", g_allocs);
#else
    printf("allocation check not armed (build with -DSKK_WRAP_ALLOC "
           "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free)\n");
#endif

    printf("skk_kana: %d checks, %d failures\n", g_total, g_fail);
    return g_fail != 0;
}
