/*
 * test_util.h — the whole harness for the term_core phase-1 host tests.
 *
 * Deliberately naive: assert-style CHECK macros that record a failure and
 * keep going, so one broken behaviour does not hide the next twenty. Every
 * expectation in these tests is derived from docs/term-design.md and
 * components/term_core/term_core.h ONLY. Where the design does not pin a
 * concrete value (palette entries, the DA identity, ...) the test asserts an
 * invariant against the header's own constants instead of inventing numbers.
 *
 * C99, no external dependencies.
 */
#ifndef TERM_TEST_UTIL_H
#define TERM_TEST_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "term_core.h"

/* ===================================================================== */
/* Harness                                                               */
/* ===================================================================== */

static const char *t_suite_name = "suite";
static const char *t_case_name = "(none)";
static int t_checks = 0;
static int t_fails = 0;

static inline void t_suite(const char *name) { t_suite_name = name; }
static inline void t_case(const char *name) { t_case_name = name; }

static inline void t_head(const char *file, int line)
{
    t_fails++;
    printf("FAIL [%s] %s\n     at %s:%d\n", t_suite_name, t_case_name, file, line);
}

static inline int t_summary(void)
{
    printf("%s [%s] %d checks, %d failures\n",
           t_fails ? "SUITE-FAIL" : "SUITE-OK", t_suite_name, t_checks, t_fails);
    fflush(stdout);
    return t_fails ? 1 : 0;
}

/* Printable rendition of a byte string, for expected/actual output. Four
 * rotating buffers so two of them can appear in one printf. */
static inline const char *t_esc(const char *s)
{
    static char buf[4][512];
    static int which = 0;
    char *out;
    size_t i, o = 0;
    which = (which + 1) & 3;
    out = buf[which];
    if (!s) { strcpy(out, "(null)"); return out; }
    for (i = 0; s[i] && o < sizeof(buf[0]) - 8; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == 0x1b) { out[o++] = '<'; out[o++] = 'E'; out[o++] = 'S'; out[o++] = 'C'; out[o++] = '>'; }
        else if (ch >= 0x20 && ch < 0x7f) { out[o++] = (char)ch; }
        else { o += (size_t)sprintf(out + o, "\\x%02X", ch); }
    }
    out[o] = 0;
    return out;
}

#define CHK_TRUE(cond)                                                        \
    do {                                                                      \
        t_checks++;                                                           \
        if (!(cond)) {                                                        \
            t_head(__FILE__, __LINE__);                                       \
            printf("     expr    : %s\n     expected: true\n     actual  : false\n", #cond); \
        }                                                                     \
    } while (0)

#define CHK_INT(actual, expected)                                             \
    do {                                                                      \
        long long a_ = (long long)(actual), e_ = (long long)(expected);       \
        t_checks++;                                                           \
        if (a_ != e_) {                                                       \
            t_head(__FILE__, __LINE__);                                       \
            printf("     expr    : %s\n     expected: %lld\n     actual  : %lld\n", #actual, e_, a_); \
        }                                                                     \
    } while (0)

#define CHK_HEX(actual, expected)                                             \
    do {                                                                      \
        unsigned long a_ = (unsigned long)(actual), e_ = (unsigned long)(expected); \
        t_checks++;                                                           \
        if (a_ != e_) {                                                       \
            t_head(__FILE__, __LINE__);                                       \
            printf("     expr    : %s\n     expected: 0x%04lX\n     actual  : 0x%04lX\n", #actual, e_, a_); \
        }                                                                     \
    } while (0)

#define CHK_STR(actual, expected)                                             \
    do {                                                                      \
        const char *a_ = (actual); const char *e_ = (expected);               \
        t_checks++;                                                           \
        if (!a_ || !e_ || strcmp(a_, e_) != 0) {                              \
            t_head(__FILE__, __LINE__);                                       \
            printf("     expr    : %s\n     expected: \"%s\"\n     actual  : \"%s\"\n", \
                   #actual, t_esc(e_), t_esc(a_));                            \
        }                                                                     \
    } while (0)

#define CHK_RANGE(actual, lo, hi)                                             \
    do {                                                                      \
        long long a_ = (long long)(actual), l_ = (long long)(lo), h_ = (long long)(hi); \
        t_checks++;                                                           \
        if (a_ < l_ || a_ > h_) {                                             \
            t_head(__FILE__, __LINE__);                                       \
            printf("     expr    : %s\n     expected: within [%lld,%lld]\n     actual  : %lld\n", \
                   #actual, l_, h_, a_);                                      \
        }                                                                     \
    } while (0)

/* Fatal inside a void case function: report and bail out of the case. */
#define REQUIRE(cond)                                                         \
    do {                                                                      \
        t_checks++;                                                           \
        if (!(cond)) {                                                        \
            t_head(__FILE__, __LINE__);                                       \
            printf("     expr    : %s\n     expected: true (case aborted)\n     actual  : false\n", #cond); \
            return;                                                           \
        }                                                                     \
    } while (0)

/* ===================================================================== */
/* Core construction with red zones (§ header B6: the core must never      */
/* touch a byte outside the block handed to term_core_init)               */
/* ===================================================================== */

#define TC_GUARD 64u
#define TC_GUARD_BYTE 0xA5

typedef struct {
    term_core_t *c;
    uint8_t *raw;
    uint8_t *base;
    size_t size;
} tcore_t;

static inline term_config_t tc_cfg(term_mode_t mode, int cols, int rows)
{
    term_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.mode = mode;
    cfg.cols = cols;
    cfg.rows = rows;
    cfg.scrollback_bytes = 48u * 1024u; /* §4.1 defaults */
    cfg.scrollback_lines = 1024;
    return cfg;
}

static inline tcore_t tc_make_cfg(const term_config_t *cfg)
{
    tcore_t t;
    size_t need, total;
    uintptr_t a;
    memset(&t, 0, sizeof t);
    need = term_core_mem_size(cfg);
    if (need == 0) return t;
    total = need + 2u * TC_GUARD + 8u;
    t.raw = (uint8_t *)malloc(total);
    if (!t.raw) return t;
    memset(t.raw, TC_GUARD_BYTE, total);
    a = ((uintptr_t)t.raw + TC_GUARD + 7u) & ~(uintptr_t)7u;
    t.base = (uint8_t *)a;
    t.size = need;
    t.c = term_core_init(t.base, need, cfg);
    return t;
}

static inline tcore_t tc_make(term_mode_t mode, int cols, int rows)
{
    term_config_t cfg = tc_cfg(mode, cols, rows);
    return tc_make_cfg(&cfg);
}

/* Returns 1 when both red zones are intact. */
static inline int tc_guards_intact(const tcore_t *t)
{
    size_t i;
    if (!t->raw) return 1;
    for (i = 0; i < TC_GUARD; i++) {
        if (t->base[-(ptrdiff_t)TC_GUARD + (ptrdiff_t)i] != TC_GUARD_BYTE) return 0;
        if (t->base[t->size + i] != TC_GUARD_BYTE) return 0;
    }
    return 1;
}

static inline void tc_free(tcore_t *t)
{
    free(t->raw);
    t->raw = NULL;
    t->c = NULL;
}

/* ===================================================================== */
/* Feed / readout helpers                                                */
/* ===================================================================== */

/* Bounded strlen: an implementation that forgets to NUL-terminate must fail a
 * check, not run the test process off the end of its own buffer. */
static inline size_t t_strnlen(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}

static inline void feed(term_core_t *c, const char *s)
{
    term_core_feed(c, (const uint8_t *)s, strlen(s));
}

static inline void feedn(term_core_t *c, const void *p, size_t n)
{
    term_core_feed(c, (const uint8_t *)p, n);
}

/* Feed the same bytes one at a time (UTF-8 / escape split resilience). */
static inline void feed_bytewise(term_core_t *c, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) term_core_feed(c, (const uint8_t *)s + i, 1);
}

/* A row that should exist but does not must fail a check, not crash the run:
 * cell_at() hands back a poison cell instead of NULL so the surrounding
 * expectations report a readable mismatch. */
static const term_cell_t tc_dead_cell = { 0xDEADu, 0xDEAD, 0xDEAD, 0xDEAD, 0 };

static inline const term_cell_t *cell_at(const term_core_t *c, int row, int col)
{
    const term_cell_t *r = term_core_row(c, row);
    return r ? &r[col] : &tc_dead_cell;
}

/* term_core_row_utf8 into a rotating static buffer, with a canary check. */
static inline const char *row_text(const term_core_t *c, int row)
{
    static char buf[4][2048];
    static int which = 0;
    int n;
    which = (which + 1) & 3;
    memset(buf[which], 0x7e, sizeof buf[0]);
    n = term_core_row_utf8(c, row, buf[which], 1024);
    if (n < 0) { strcpy(buf[which], "(err)"); return buf[which]; }
    if (buf[which][1200] != 0x7e) { strcpy(buf[which], "(OVERRUN)"); return buf[which]; }
    buf[which][1023] = 0;
    return buf[which];
}

static inline const char *line_text(const term_core_t *c, uint32_t id)
{
    static char buf[4][2048];
    static int which = 0;
    int n;
    which = (which + 1) & 3;
    memset(buf[which], 0x7e, sizeof buf[0]);
    n = term_core_line_utf8(c, id, buf[which], 1024);
    if (n < 0) { strcpy(buf[which], "(err)"); return buf[which]; }
    if (buf[which][1200] != 0x7e) { strcpy(buf[which], "(OVERRUN)"); return buf[which]; }
    buf[which][1023] = 0;
    return buf[which];
}

static inline int cursor_col(const term_core_t *c)
{
    int col = -1;
    term_core_cursor(c, &col, NULL, NULL);
    return col;
}

static inline int cursor_row(const term_core_t *c)
{
    int row = -1;
    term_core_cursor(c, NULL, &row, NULL);
    return row;
}

static inline bool cursor_visible(const term_core_t *c)
{
    bool v = false;
    term_core_cursor(c, NULL, NULL, &v);
    return v;
}

/* Fill the active row `row` with `n` copies of `ch` starting at column 0,
 * leaving the cursor where the writes ended. */
static inline void write_run(term_core_t *c, char ch, int n)
{
    char b[256];
    int i;
    while (n > 0) {
        int k = n > 200 ? 200 : n;
        for (i = 0; i < k; i++) b[i] = ch;
        b[k] = 0;
        feed(c, b);
        n -= k;
    }
}

static inline void cup(term_core_t *c, int row1, int col1) /* 1-based, as CSI H */
{
    char b[64];
    sprintf(b, "\x1b[%d;%dH", row1, col1);
    feed(c, b);
}

/* ===================================================================== */
/* Whole-state comparison (used by the UTF-8 / escape split tests)        */
/* ===================================================================== */

/* Returns 0 when the two cores are indistinguishable through the public
 * readout API; otherwise fills `why` and returns non-zero. */
static inline int state_diff(const term_core_t *a, const term_core_t *b, char *why, size_t whyn)
{
    int rows, cols, r, ccol, crow, bcol, brow;
    bool cvis, bvis;
    uint32_t id;
    rows = term_core_rows(a);
    cols = term_core_cols(a);
    if (rows != term_core_rows(b) || cols != term_core_cols(b)) {
        snprintf(why, whyn, "geometry %dx%d vs %dx%d", cols, rows, term_core_cols(b), term_core_rows(b));
        return 1;
    }
    for (r = 0; r < rows; r++) {
        const term_cell_t *ra = term_core_row(a, r);
        const term_cell_t *rb = term_core_row(b, r);
        int col;
        if (!ra || !rb) { snprintf(why, whyn, "row %d NULL", r); return 1; }
        for (col = 0; col < cols; col++) {
            if (ra[col].cp != rb[col].cp || ra[col].fg != rb[col].fg ||
                ra[col].bg != rb[col].bg || ra[col].flags != rb[col].flags) {
                snprintf(why, whyn,
                         "cell (%d,%d): cp %08lX/%08lX fg %04X/%04X bg %04X/%04X fl %04X/%04X",
                         r, col, (unsigned long)ra[col].cp, (unsigned long)rb[col].cp,
                         ra[col].fg, rb[col].fg, ra[col].bg, rb[col].bg,
                         ra[col].flags, rb[col].flags);
                return 1;
            }
        }
        if (term_core_row_flags(a, r) != term_core_row_flags(b, r)) {
            snprintf(why, whyn, "row %d flags %lu vs %lu", r,
                     (unsigned long)term_core_row_flags(a, r), (unsigned long)term_core_row_flags(b, r));
            return 1;
        }
    }
    term_core_cursor(a, &ccol, &crow, &cvis);
    term_core_cursor(b, &bcol, &brow, &bvis);
    if (ccol != bcol || crow != brow || cvis != bvis) {
        snprintf(why, whyn, "cursor (%d,%d,%d) vs (%d,%d,%d)", ccol, crow, (int)cvis, bcol, brow, (int)bvis);
        return 1;
    }
    if (term_core_modes(a) != term_core_modes(b)) {
        snprintf(why, whyn, "modes %lu vs %lu", (unsigned long)term_core_modes(a), (unsigned long)term_core_modes(b));
        return 1;
    }
    if (term_core_sb_first(a) != term_core_sb_first(b) || term_core_sb_end(a) != term_core_sb_end(b)) {
        snprintf(why, whyn, "scrollback [%lu,%lu) vs [%lu,%lu)",
                 (unsigned long)term_core_sb_first(a), (unsigned long)term_core_sb_end(a),
                 (unsigned long)term_core_sb_first(b), (unsigned long)term_core_sb_end(b));
        return 1;
    }
    for (id = term_core_sb_first(a); id != term_core_sb_end(a); id++) {
        char ba[1024], bb[1024];
        int na = term_core_line_utf8(a, id, ba, sizeof ba);
        int nb = term_core_line_utf8(b, id, bb, sizeof bb);
        if (na != nb || strcmp(ba, bb) != 0) {
            snprintf(why, whyn, "scrollback line %lu differs: \"%.80s\" vs \"%.80s\"",
                     (unsigned long)id, ba, bb);
            return 1;
        }
    }
    why[0] = 0;
    return 0;
}

/* ===================================================================== */
/* Grid invariants (§5 fuzz bullet; reused by several suites)            */
/* ===================================================================== */

#define TERM_MODE_ALL_KNOWN                                                   \
    (TERM_MODE_CURSOR_VISIBLE | TERM_MODE_BRACKETED | TERM_MODE_ALT_SCREEN |  \
     TERM_MODE_APP_CURSOR | TERM_MODE_AUTOWRAP | TERM_MODE_ORIGIN)

/* Returns 0 when every structural invariant holds, else fills `why`. */
static inline int invariants_ok(const term_core_t *c, char *why, size_t whyn)
{
    int rows = term_core_rows(c);
    int cols = term_core_cols(c);
    int r, col, ccol, crow, top, bot;

    if (cols < 1 || rows < 1) { snprintf(why, whyn, "geometry %dx%d", cols, rows); return 1; }

    term_core_cursor(c, &ccol, &crow, NULL);
    if (ccol < 0 || ccol >= cols || crow < 0 || crow >= rows) {
        snprintf(why, whyn, "cursor (%d,%d) outside %dx%d", ccol, crow, cols, rows);
        return 1;
    }

    term_core_scroll_region(c, &top, &bot);
    if (top < 0 || bot < top || bot >= rows) {
        snprintf(why, whyn, "scroll region [%d,%d] in %d rows", top, bot, rows);
        return 1;
    }

    if (term_core_modes(c) & ~(uint32_t)TERM_MODE_ALL_KNOWN) {
        snprintf(why, whyn, "unknown mode bits set: %08lX", (unsigned long)term_core_modes(c));
        return 1;
    }

    if (term_core_row(c, -1) != NULL || term_core_row(c, rows) != NULL) {
        snprintf(why, whyn, "term_core_row() answered an out-of-range row");
        return 1;
    }

    for (r = 0; r < rows; r++) {
        const term_cell_t *row = term_core_row(c, r);
        if (!row) { snprintf(why, whyn, "row %d NULL", r); return 1; }
        for (col = 0; col < cols; col++) {
            uint32_t cp = row[col].cp;
            int is_cont_flag = (row[col].flags & TERM_CELL_CONT) != 0;
            int is_cont_cp = (cp == TERM_CP_CONT);
            if (is_cont_flag != is_cont_cp) {
                snprintf(why, whyn, "cell (%d,%d) CONT flag/cp disagree: cp=%08lX flags=%04X",
                         r, col, (unsigned long)cp, row[col].flags);
                return 1;
            }
            if (!is_cont_cp && cp > 0x10FFFFu) {
                snprintf(why, whyn, "cell (%d,%d) cp out of Unicode range: %08lX",
                         r, col, (unsigned long)cp);
                return 1;
            }
            if (!is_cont_cp && cp >= 0xD800u && cp <= 0xDFFFu) {
                snprintf(why, whyn, "cell (%d,%d) holds a surrogate: %08lX",
                         r, col, (unsigned long)cp);
                return 1;
            }
            if (is_cont_cp) {
                if (col == 0 || !(row[col - 1].flags & TERM_CELL_WIDE)) {
                    snprintf(why, whyn, "orphan CONT at (%d,%d)", r, col);
                    return 1;
                }
            }
            if ((row[col].flags & TERM_CELL_WIDE) && col + 1 < cols) {
                if (!(row[col + 1].flags & TERM_CELL_CONT)) {
                    snprintf(why, whyn, "WIDE at (%d,%d) without a CONT successor", r, col);
                    return 1;
                }
            }
        }
    }

    if (term_core_sb_first(c) > term_core_sb_end(c)) {
        snprintf(why, whyn, "sb_first %lu > sb_end %lu",
                 (unsigned long)term_core_sb_first(c), (unsigned long)term_core_sb_end(c));
        return 1;
    }
    why[0] = 0;
    return 0;
}

#endif /* TERM_TEST_UTIL_H */
