/*
 * test_utf8.c — packet-boundary resilience and malformed input.
 *
 * Design §6 row "UTF-8 分割耐性 (新規): パケット境界で千切れたマルチバイト列を
 * 継続バッファで結合。現行 ssh_vt.js はサロゲート結合のみで未対応 — 日本語
 * ファイル名/vim ステータス行で現実に踏むバグを移植時に直す".
 * Header term_core_feed(): "feeding one byte at a time must produce exactly the
 * same grid as feeding the whole buffer at once. Phase 1 tests assert that
 * equivalence over every split point" and "an invalid, overlong, surrogate or
 * out-of-range sequence emits one U+FFFD per maximal invalid subpart and
 * resumes at the byte that ended it (that byte is re-examined, not eaten)".
 */
#include "test_util.h"

#define HIRA_A "\xe3\x81\x82"           /* U+3042 */
#define EMOJI  "\xf0\x9f\x98\x80"       /* U+1F600 */
#define RUNE_REPL "\xef\xbf\xbd"        /* U+FFFD */

/* A stream that exercises text, CJK, 4-byte UTF-8, CSI, SGR, OSC and wrap. */
static const char sample[] =
    "ab\x1b[31m" HIRA_A "cd\x1b[1;3H" EMOJI "\x1b]0;title\x07" "ef"
    "\x1b[7m" HIRA_A HIRA_A "\x1b[2;1Hxyz\x1b[m" "0123456789012345";

static void case_bytewise_equivalence(void)
{
    tcore_t whole = tc_make(TERM_VT, 12, 4);
    tcore_t bytes = tc_make(TERM_VT, 12, 4);
    char why[256];
    size_t n = sizeof sample - 1;

    t_case("feeding one byte at a time produces exactly the same state");
    REQUIRE(whole.c != NULL && bytes.c != NULL);
    feedn(whole.c, sample, n);
    feed_bytewise(bytes.c, sample, n);
    CHK_INT(state_diff(whole.c, bytes.c, why, sizeof why), 0);
    if (why[0]) printf("     difference: %s\n", why);
    {
        term_stats_t a, b;
        term_core_stats(whole.c, &a);
        term_core_stats(bytes.c, &b);
        CHK_INT(a.bytes_in, (long long)n);
        CHK_INT(b.bytes_in, (long long)n);
        CHK_INT(a.chars_written, b.chars_written);
        CHK_INT(a.utf8_errors, b.utf8_errors);
    }
    tc_free(&whole);
    tc_free(&bytes);
}

static void case_every_split_point(void)
{
    size_t n = sizeof sample - 1;
    size_t k;
    int failures = 0;
    char why[256];
    tcore_t whole = tc_make(TERM_VT, 12, 4);

    t_case("every two-way split of the same stream converges to one state");
    REQUIRE(whole.c != NULL);
    feedn(whole.c, sample, n);

    for (k = 0; k <= n; k++) {
        tcore_t split = tc_make(TERM_VT, 12, 4);
        if (!split.c) { failures++; break; }
        feedn(split.c, sample, k);
        feedn(split.c, sample + k, n - k);
        if (state_diff(whole.c, split.c, why, sizeof why) != 0) {
            if (failures == 0) printf("     first bad split at k=%u: %s\n", (unsigned)k, why);
            failures++;
        }
        if (!tc_guards_intact(&split)) {
            printf("     red zone damaged at k=%u\n", (unsigned)k);
            failures++;
        }
        tc_free(&split);
    }
    CHK_INT(failures, 0);
    tc_free(&whole);
}

static void case_pending_across_calls(void)
{
    tcore_t t = tc_make(TERM_VT, 12, 4);
    term_stats_t st;

    t_case("an incomplete sequence at the end of a feed is held, not reported");
    REQUIRE(t.c != NULL);
    feedn(t.c, HIRA_A, 2);
    CHK_STR(row_text(t.c, 0), "");
    CHK_INT(cursor_col(t.c), 0);
    term_core_stats(t.c, &st);
    CHK_INT(st.utf8_errors, 0);

    t_case("the held bytes join the rest on the next feed");
    feedn(t.c, HIRA_A + 2, 1);
    CHK_STR(row_text(t.c, 0), HIRA_A);
    CHK_INT(cursor_col(t.c), 2);
    term_core_stats(t.c, &st);
    CHK_INT(st.utf8_errors, 0);

    t_case("a 4-byte sequence survives being split into four feeds");
    term_core_reset(t.c);
    feedn(t.c, EMOJI, 1);
    feedn(t.c, EMOJI + 1, 1);
    feedn(t.c, EMOJI + 2, 1);
    feedn(t.c, EMOJI + 3, 1);
    CHK_HEX(cell_at(t.c, 0, 0)->cp, 0x1F600);
    term_core_stats(t.c, &st);
    CHK_INT(st.utf8_errors, 0);
    tc_free(&t);
}

static void case_malformed(void)
{
    tcore_t t = tc_make(TERM_VT, 12, 4);
    term_stats_t st;

    t_case("a truncated sequence yields one U+FFFD and does not eat the byte "
           "that ended it");
    REQUIRE(t.c != NULL);
    feed(t.c, "\xe3\x81" "A");
    CHK_STR(row_text(t.c, 0), RUNE_REPL "A");
    term_core_stats(t.c, &st);
    CHK_INT(st.utf8_errors, 1);

    t_case("a lone continuation byte and 0xFE/0xFF are each replaced");
    term_core_reset(t.c);
    feed(t.c, "\x80" "\xfe" "\xff" "B");
    CHK_STR(row_text(t.c, 0), RUNE_REPL RUNE_REPL RUNE_REPL "B");

    t_case("an ESC after a truncated sequence is re-examined, not swallowed");
    term_core_reset(t.c);
    feed(t.c, "\xf0\x9f" "\x1b[31m" "R");
    {
        const term_cell_t *row = term_core_row(t.c, 0);
        REQUIRE(row != NULL);
        CHK_HEX(row[0].cp, 0xFFFD);
        CHK_HEX(row[1].cp, 'R');
        CHK_HEX(row[1].fg, term_pal16_at(1));   /* the CSI took effect */
    }

    t_case("an overlong encoding never becomes the character it aliases");
    term_core_reset(t.c);
    feed(t.c, "\xc0\xaf");                      /* overlong '/' */
    {
        const term_cell_t *row = term_core_row(t.c, 0);
        int i, slashes = 0;
        REQUIRE(row != NULL);
        for (i = 0; i < 12; i++) if (row[i].cp == (uint32_t)'/') slashes++;
        CHK_INT(slashes, 0);
        CHK_HEX(row[0].cp, 0xFFFD);
    }

    t_case("a surrogate encoding never reaches a cell");
    term_core_reset(t.c);
    feed(t.c, "\xed\xa0\x80" "C");
    {
        char why[256];
        CHK_INT(invariants_ok(t.c, why, sizeof why), 0);   /* rejects D800-DFFF */
        if (why[0]) printf("     invariant: %s\n", why);
        CHK_HEX(cell_at(t.c, 0, 0)->cp, 0xFFFD);
    }

    t_case("a codepoint beyond U+10FFFF never reaches a cell");
    term_core_reset(t.c);
    feed(t.c, "\xf4\x90\x80\x80" "D");
    {
        char why[256];
        CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
        if (why[0]) printf("     invariant: %s\n", why);
        CHK_HEX(cell_at(t.c, 0, 0)->cp, 0xFFFD);
    }

    t_case("utf8_errors counts the substitutions");
    term_core_reset(t.c);
    {
        term_stats_t before, after;
        term_core_stats(t.c, &before);
        feed(t.c, "\xff\xff\xff");
        term_core_stats(t.c, &after);
        CHK_INT(after.utf8_errors - before.utf8_errors, 3);
    }
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_malformed_split_equivalence(void)
{
    static const char junk[] =
        "\xe3\x81" "A" "\x80\xfe\xff" "\xf0\x9f\x98" "\x1b[32m" "ok"
        "\xc0\xaf" "\xed\xa0\x80" "\xf4\x90\x80\x80" "end";
    size_t n = sizeof junk - 1;
    size_t k;
    int failures = 0;
    char why[256];
    tcore_t whole = tc_make(TERM_VT, 16, 4);

    t_case("malformed input is split-invariant too");
    REQUIRE(whole.c != NULL);
    feedn(whole.c, junk, n);
    for (k = 0; k <= n; k++) {
        tcore_t split = tc_make(TERM_VT, 16, 4);
        if (!split.c) { failures++; break; }
        feedn(split.c, junk, k);
        feedn(split.c, junk + k, n - k);
        if (state_diff(whole.c, split.c, why, sizeof why) != 0) {
            if (failures == 0) printf("     first bad split at k=%u: %s\n", (unsigned)k, why);
            failures++;
        }
        tc_free(&split);
    }
    CHK_INT(failures, 0);
    tc_free(&whole);
}

int main(void)
{
    t_suite("utf8");
    case_bytewise_equivalence();
    case_every_split_point();
    case_pending_across_calls();
    case_malformed();
    case_malformed_split_equivalence();
    return t_summary();
}
