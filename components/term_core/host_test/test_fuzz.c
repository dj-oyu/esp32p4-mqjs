/*
 * test_fuzz.c — the fuzz phase 1 is required to ship.
 *
 * Design §5, last bullet: "フェーズ 1 のホストテストにパーサの fuzz(ランダム/
 * 敵対的バイト列で無限ループ・領域外・状態崩れが無いこと)を含める。壊れた pty
 * やリモートの悪意ある出力は「来るもの」として扱う".
 * Header B6: "No input sequence can make the core read or write outside the
 * block given to term_core_init(). Phase 1 ships a fuzz test whose job is to
 * try."
 *
 * What is asserted (and only this — the design fixes no semantics for random
 * bytes):
 *   1. it terminates: every byte handed to feed() is consumed, stats.bytes_in
 *      equals the total fed, and the run finishes;
 *   2. it stays inside its block: the red zones around the arena are intact;
 *   3. the grid stays structurally sane: cursor inside the box, scroll region
 *      inside the box, no unknown mode bits, CONT/WIDE cells consistent, no
 *      surrogate or out-of-range codepoint in a cell, sb_first <= sb_end;
 *   4. the read-only API never writes past the buffer it was given and never
 *      answers for a dead id.
 *
 * Deterministic: fixed seed, no time or address dependence. Byte volume can be
 * raised from the command line (argv[1] = megabytes) for a longer soak.
 */
#include "test_util.h"

/* ---- xorshift64*, fixed seed ---- */
static uint64_t rng_state = 0x243F6A8885A308D3ull;

static uint64_t rnd64(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static unsigned rnd(unsigned n) { return (unsigned)(rnd64() >> 33) % (n ? n : 1); }

/* ---- corpora ---- */

static const char *csi_finals = "ABCDEFGHJKLMPSTXZbcdfghlmnpqrstuvwxy@`";

/* Uniform noise. */
static size_t gen_random(uint8_t *buf, size_t cap)
{
    size_t n = 1 + rnd((unsigned)cap - 1), i;
    for (i = 0; i < n; i++) buf[i] = (uint8_t)rnd(256);
    return n;
}

/* Escape soup: well-shaped-ish sequences with hostile parameters. */
static size_t gen_escapes(uint8_t *buf, size_t cap)
{
    size_t n = 0;
    while (n + 48 < cap) {
        unsigned k = rnd(12);
        if (k < 5) {
            /* CSI with 0..24 parameters, values from tiny to absurd */
            unsigned np = rnd(25), p;
            buf[n++] = 0x1b;
            buf[n++] = '[';
            if (rnd(6) == 0) buf[n++] = (uint8_t)"?><=!"[rnd(5)];
            for (p = 0; p < np && n + 16 < cap; p++) {
                unsigned v;
                switch (rnd(5)) {
                    case 0:  v = rnd(4); break;
                    case 1:  v = rnd(200); break;
                    case 2:  v = 65535u; break;
                    case 3:  v = 65536u + rnd(1000); break;
                    default: v = 2000000000u; break;
                }
                n += (size_t)sprintf((char *)buf + n, "%u", v);
                if (p + 1 < np) buf[n++] = (rnd(8) == 0) ? ':' : ';';
            }
            buf[n++] = (uint8_t)csi_finals[rnd((unsigned)strlen(csi_finals))];
        } else if (k < 6) {
            /* private mode set/reset, including the ones we implement */
            static const int modes[] = {1, 6, 7, 25, 1000, 1006, 1049, 2004, 12345};
            n += (size_t)sprintf((char *)buf + n, "\x1b[?%dh", modes[rnd(9)]);
            n += (size_t)sprintf((char *)buf + n, "\x1b[?%dl", modes[rnd(9)]);
        } else if (k < 7) {
            /* SGR, including 256 and truecolour with missing arguments */
            switch (rnd(6)) {
                case 0: n += (size_t)sprintf((char *)buf + n, "\x1b[%um", rnd(120)); break;
                case 1: n += (size_t)sprintf((char *)buf + n, "\x1b[38;5;%um", rnd(400)); break;
                case 2: n += (size_t)sprintf((char *)buf + n, "\x1b[48;5;%um", rnd(400)); break;
                case 3: n += (size_t)sprintf((char *)buf + n, "\x1b[38;2;%u;%u;%um", rnd(300), rnd(300), rnd(300)); break;
                case 4: n += (size_t)sprintf((char *)buf + n, "\x1b[38;2;%um", rnd(300)); break;
                default: n += (size_t)sprintf((char *)buf + n, "\x1b[38;5m"); break;
            }
        } else if (k < 8) {
            /* OSC, sometimes unterminated */
            unsigned len = rnd(400), i;
            buf[n++] = 0x1b; buf[n++] = ']';
            n += (size_t)sprintf((char *)buf + n, "%u;", rnd(200));
            for (i = 0; i < len && n + 4 < cap; i++) buf[n++] = (uint8_t)(0x20 + rnd(90));
            if (rnd(4)) { if (rnd(2)) buf[n++] = 0x07; else { buf[n++] = 0x1b; buf[n++] = '\\'; } }
        } else if (k < 9) {
            /* DCS / SOS / PM / APC, sometimes unterminated */
            unsigned len = rnd(300), i;
            buf[n++] = 0x1b;
            buf[n++] = (uint8_t)"PX^_"[rnd(4)];
            for (i = 0; i < len && n + 4 < cap; i++) buf[n++] = (uint8_t)(0x20 + rnd(90));
            if (rnd(4)) { buf[n++] = 0x1b; buf[n++] = '\\'; }
        } else if (k < 10) {
            /* bare ESC games and charset designators */
            buf[n++] = 0x1b;
            if (rnd(2)) buf[n++] = (uint8_t)"()*+"[rnd(4)];
            buf[n++] = (uint8_t)(0x20 + rnd(95));
        } else {
            /* text, control characters and UTF-8 (valid, truncated, illegal) */
            unsigned len = rnd(40), i;
            for (i = 0; i < len && n + 8 < cap; i++) {
                switch (rnd(9)) {
                    case 0: buf[n++] = (uint8_t)"\r\n\b\t\a\v\f\x1b"[rnd(8)]; break;
                    case 1: buf[n++] = 0xe3; buf[n++] = 0x81; buf[n++] = (uint8_t)(0x82 + rnd(20)); break;
                    case 2: buf[n++] = 0xe3; buf[n++] = 0x81; break;               /* truncated */
                    case 3: buf[n++] = 0xf0; buf[n++] = 0x9f; buf[n++] = 0x98; buf[n++] = 0x80; break;
                    case 4: buf[n++] = (uint8_t)(0x80 + rnd(0x80)); break;         /* stray */
                    case 5: buf[n++] = 0xed; buf[n++] = 0xa0; buf[n++] = 0x80; break; /* surrogate */
                    case 6: buf[n++] = 0xf4; buf[n++] = 0x90; buf[n++] = 0x80; buf[n++] = 0x80; break;
                    case 7: buf[n++] = 0xef; buf[n++] = 0xb8; buf[n++] = 0x8f; break; /* VS16: promotes the
                        cell to its left — the corpus must be able to land one on a wide lead */
                    default: buf[n++] = (uint8_t)(0x20 + rnd(95)); break;
                }
            }
        }
        if (rnd(3) == 0) break;
    }
    if (n == 0) { buf[0] = 'a'; n = 1; }
    return n;
}

/* ---- readers, with canaries, exercised mid-fuzz ---- */

static int exercise_readers(term_core_t *c)
{
    static char buf[512];
    static term_cell_t cells[300];
    static term_attr_run_t runs[8];
    uint32_t first = term_core_sb_first(c), end = term_core_sb_end(c), id;
    int rows = term_core_rows(c);
    int r;
    size_t small = 1 + (size_t)rnd(24);

    for (r = -1; r <= rows; r++) {
        memset(buf, 0x5a, sizeof buf);
        (void)term_core_row_utf8(c, r, buf, small);
        if (buf[small] != 0x5a) return 1;               /* wrote past out_size */
        if (r >= 0 && r < rows) {
            if (term_core_row_utf8(c, r, buf, small) < 0) return 2;
            if (term_core_row(c, r) == NULL) return 3;
        }
        (void)term_core_row_dirty(c, r);
    }
    if (term_core_dirty_next(c, 0) >= rows) return 4;

    /* dead ids must stay dead */
    if (term_core_line_length(c, end) != -1) return 5;
    if (term_core_line_seg_count(c, end + 7) != -1) return 6;
    if (first > 0 && term_core_line_length(c, first - 1) != -1) return 7;

    if (first != end) {
        id = first + (uint32_t)rnd((unsigned)(end - first));
        memset(buf, 0x5a, sizeof buf);
        if (term_core_line_utf8(c, id, buf, small) < 0) return 8;
        if (buf[small] != 0x5a) return 9;
        if (term_core_line_length(c, id) < 0) return 10;
        {
            int segs = term_core_line_seg_count(c, id);
            int n;
            if (segs < 1) return 11;
            n = term_core_line_segment(c, id, rnd((unsigned)segs), cells, 300);
            if (n < 0 || n > 300 || n > term_core_cols(c)) return 12;
            if (term_core_line_segment(c, id, segs, cells, 300) != -1) return 13;
        }
        if (term_core_line_attrs(c, id, runs, 8) < 0) return 14;
        if (term_core_line_attrs(c, id, NULL, 0) < 0) return 15;
    }
    return 0;
}

static void reply_counter(void *user, const char *bytes, size_t len)
{
    unsigned long *n = (unsigned long *)user;
    if (bytes && len) (*n)++;
}

/* ---- the soak ---- */

typedef struct {
    const char *name;
    term_mode_t mode;
    int cols, rows;
    size_t sb_bytes;
    int sb_lines;
} fuzz_cfg_t;

static void soak(const fuzz_cfg_t *fc, size_t budget, int adversarial)
{
    term_config_t cfg = tc_cfg(fc->mode, fc->cols, fc->rows);
    unsigned long replies = 0;
    tcore_t t;
    uint8_t buf[4096];
    uint64_t total = 0;
    size_t chunk;
    int op = 0, bad = 0;
    char why[256];
    term_stats_t st;

    static char label[128];

    cfg.scrollback_bytes = fc->sb_bytes;
    cfg.scrollback_lines = fc->sb_lines;
    cfg.reply_cb = reply_counter;
    cfg.reply_user = &replies;
    t = tc_make_cfg(&cfg);

    sprintf(label, "%s corpus / %s", adversarial ? "adversarial-escape" : "uniform-random", fc->name);
    t_case(label);
    REQUIRE(t.c != NULL);

    while (total < budget && bad == 0) {
        chunk = adversarial ? gen_escapes(buf, sizeof buf) : gen_random(buf, sizeof buf);
        term_core_feed(t.c, buf, chunk);
        total += chunk;

        if ((++op & 7) == 0) {
            if (invariants_ok(t.c, why, sizeof why) != 0) {
                printf("     invariant broken after %llu bytes: %s\n",
                       (unsigned long long)total, why);
                bad++;
            }
            if (!tc_guards_intact(&t)) {
                printf("     RED ZONE DAMAGED after %llu bytes\n", (unsigned long long)total);
                bad++;
            }
            {
                int rc = exercise_readers(t.c);
                if (rc) {
                    printf("     reader contract broken (code %d) after %llu bytes\n",
                           rc, (unsigned long long)total);
                    bad++;
                }
            }
        }
        if ((op & 63) == 0) {
            /* rotations, damage-set churn and history wipes, mid-stream */
            int c2 = 1 + (int)rnd((unsigned)term_core_cols(t.c) * 2u);
            int r2 = 1 + (int)rnd((unsigned)term_core_rows(t.c) * 2u);
            (void)term_core_resize(t.c, c2, r2);      /* -1 is a legal answer */
            term_core_dirty_clear(t.c);
            if ((op & 255) == 0) term_core_scrollback_clear(t.c);
            if ((op & 511) == 0) term_core_reset(t.c);
            if (invariants_ok(t.c, why, sizeof why) != 0) {
                printf("     invariant broken after a resize/reset: %s\n", why);
                bad++;
            }
        }
    }

    CHK_INT(bad, 0);

    term_core_stats(t.c, &st);
    CHK_INT(st.bytes_in, (long long)total);   /* every byte consumed, once */
    CHK_TRUE(tc_guards_intact(&t));
    printf("     %s: %llu bytes, %lu replies, %lu archived, %lu evicted, "
           "%lu utf8 errors, %lu csi overflow, %lu osc truncated\n",
           fc->name, (unsigned long long)total, replies,
           (unsigned long)st.lines_archived, (unsigned long)st.lines_evicted,
           (unsigned long)st.utf8_errors, (unsigned long)st.csi_overflow,
           (unsigned long)st.osc_truncated);
    tc_free(&t);
}

static void case_byte_at_a_time(size_t budget)
{
    tcore_t whole = tc_make(TERM_VT, 40, 12);
    tcore_t split = tc_make(TERM_VT, 40, 12);
    uint8_t buf[2048];
    uint64_t total = 0;
    char why[256];
    int bad = 0;

    t_case("adversarial bytes fed whole == fed one byte at a time");
    REQUIRE(whole.c != NULL && split.c != NULL);
    while (total < budget && !bad) {
        size_t chunk = gen_escapes(buf, sizeof buf);
        size_t i;
        term_core_feed(whole.c, buf, chunk);
        for (i = 0; i < chunk; i++) term_core_feed(split.c, buf + i, 1);
        total += chunk;
        if (state_diff(whole.c, split.c, why, sizeof why) != 0) {
            printf("     diverged after %llu bytes: %s\n", (unsigned long long)total, why);
            bad++;
        }
    }
    CHK_INT(bad, 0);
    CHK_TRUE(tc_guards_intact(&whole));
    CHK_TRUE(tc_guards_intact(&split));
    tc_free(&whole);
    tc_free(&split);
}

int main(int argc, char **argv)
{
    size_t mb = 4;
    size_t per;
    static const fuzz_cfg_t cfgs[] = {
        { "VT 142x30, default scrollback",  TERM_VT,  142, 30, 48u * 1024u, 1024 },
        { "VT 80x53, default scrollback",   TERM_VT,   80, 53, 48u * 1024u, 1024 },
        { "VT 10x4, tiny scrollback",       TERM_VT,   10,  4,      2048u,     8 },
        { "VT 40x12, no scrollback",        TERM_VT,   40, 12,          0,     0 },
        { "LOG 40x12, default scrollback",  TERM_LOG,  40, 12, 48u * 1024u, 1024 },
    };
    size_t i;

    t_suite("fuzz");
    if (argc > 1) {
        long v = atol(argv[1]);
        if (v > 0) mb = (size_t)v;
    }
    printf("     seed=0x243F6A8885A308D3, volume=%uMB per corpus\n", (unsigned)mb);

    per = mb * 1024u * 1024u / (sizeof cfgs / sizeof cfgs[0]);

    for (i = 0; i < sizeof cfgs / sizeof cfgs[0]; i++) soak(&cfgs[i], per, 1);
    for (i = 0; i < sizeof cfgs / sizeof cfgs[0]; i++) soak(&cfgs[i], per / 2, 0);
    case_byte_at_a_time(per / 8);

    return t_summary();
}
