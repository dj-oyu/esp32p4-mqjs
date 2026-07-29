/*
 * test_bounds.c — the parser hard bounds, which are contract, not detail.
 *
 * Design §5: "パーサの実行は共有 UI(LVGL)タスク上なので、暴走は全アプリの
 * 描画を巻き込む SPOF になる" — therefore:
 *   B2  quantity parameters are clamped to the grid ("2^31 行挿入" costs rows)
 *   B3  at most TERM_CSI_MAX_PARAMS parameters, each saturating at
 *       TERM_CSI_PARAM_MAX; excess parameters are dropped, not accumulated,
 *       and the CSI is still dispatched with the first 16
 *   B4  OSC buffered to TERM_OSC_MAX_LEN then discarded but consumed to its
 *       terminator; DCS/SOS/PM/APC never buffered at all
 */
#include "test_util.h"

static void case_param_count_cap(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    term_stats_t st;
    char csi[512];
    int i;

    t_case("16 parameters are all honoured (the 16th still applies)");
    REQUIRE(t.c != NULL);
    strcpy(csi, "\x1b[");
    for (i = 0; i < 15; i++) strcat(csi, "0;");
    strcat(csi, "31m");                       /* exactly TERM_CSI_MAX_PARAMS */
    feed(t.c, "\x1b[H\x1b[2J\x1b[m");
    feed(t.c, csi);
    feed(t.c, "x");
    CHK_HEX(cell_at(t.c, 0, 0)->fg, term_pal16_at(1));
    term_core_stats(t.c, &st);
    CHK_INT(st.csi_overflow, 0);

    t_case("the 17th parameter is dropped, not accumulated (B3)");
    strcpy(csi, "\x1b[");
    for (i = 0; i < TERM_CSI_MAX_PARAMS; i++) strcat(csi, "0;");
    strcat(csi, "31m");                       /* 31 is parameter number 17 */
    feed(t.c, "\x1b[H\x1b[2J\x1b[m");
    feed(t.c, csi);
    feed(t.c, "x");
    CHK_HEX(cell_at(t.c, 0, 0)->fg, TERM_COLOR_FG_DEFAULT);
    term_core_stats(t.c, &st);
    CHK_TRUE(st.csi_overflow >= 1);

    t_case("a CSI with 200 parameters is still dispatched with the first 16");
    strcpy(csi, "\x1b[1");
    for (i = 0; i < 200; i++) strcat(csi, ";1");
    strcat(csi, ";1H");
    feed(t.c, csi);
    CHK_INT(cursor_row(t.c), 0);
    CHK_INT(cursor_col(t.c), 0);
    CHK_TRUE(tc_guards_intact(&t));

    t_case("empty parameters mean defaults and never a stray zero");
    feed(t.c, "\x1b[3;3H\x1b[;H");
    CHK_INT(cursor_row(t.c), 0);
    CHK_INT(cursor_col(t.c), 0);
    feed(t.c, "\x1b[;;;;;;H");
    CHK_INT(cursor_row(t.c), 0);
    tc_free(&t);
}

static void case_param_value_saturation(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 6);

    t_case("a parameter >= 65536 saturates instead of wrapping (B3)");
    REQUIRE(t.c != NULL);
    /* 65538 mod 65536 == 2: a wrapping parser would land on row 1, col 1 */
    feed(t.c, "\x1b[65538;65538H");
    CHK_INT(cursor_row(t.c), 5);
    CHK_INT(cursor_col(t.c), 19);

    /* 4294967298 mod 2^32 == 2 as well */
    feed(t.c, "\x1b[1;1H\x1b[4294967298;4294967298H");
    CHK_INT(cursor_row(t.c), 5);
    CHK_INT(cursor_col(t.c), 19);

    t_case("an absurdly long digit run still saturates, never goes negative");
    feed(t.c, "\x1b[1;1H\x1b[99999999999999999999999999;1H");
    CHK_INT(cursor_row(t.c), 5);
    feed(t.c, "\x1b[1;1H\x1b[99999999999999999999999999A");
    CHK_INT(cursor_row(t.c), 0);
    {
        char why[256];
        CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
        if (why[0]) printf("     invariant: %s\n", why);
    }
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_quantity_clamp_is_cheap(void)
{
    tcore_t t = tc_make(TERM_VT, 142, 30);
    int i;
    char why[256];

    t_case("thousands of absurd-count operations finish (B1+B2 make the "
           "per-frame byte budget of §5 a real budget)");
    REQUIRE(t.c != NULL);
    for (i = 0; i < 3000; i++) {
        feed(t.c, "Z\x1b[2000000000b");
        feed(t.c, "\x1b[2000000000L\x1b[2000000000M");
        feed(t.c, "\x1b[2000000000@\x1b[2000000000P\x1b[2000000000X");
        feed(t.c, "\x1b[2000000000A\x1b[2000000000B\x1b[2000000000C\x1b[2000000000D");
    }
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_osc_limit(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    term_stats_t st;
    char big[TERM_OSC_MAX_LEN * 4];
    int i;

    t_case("a short OSC is not counted as truncated");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b]0;hi\x07");
    term_core_stats(t.c, &st);
    CHK_INT(st.osc_truncated, 0);

    t_case("an OSC past TERM_OSC_MAX_LEN is truncated but consumed to its "
           "terminator so the stream re-syncs (B4)");
    memset(big, 'x', sizeof big);
    feed(t.c, "\x1b]0;");
    feedn(t.c, big, sizeof big);
    feed(t.c, "\x07" "A");
    CHK_STR(row_text(t.c, 0), "A");
    term_core_stats(t.c, &st);
    CHK_TRUE(st.osc_truncated >= 1);

    t_case("the same with an ST terminator");
    feed(t.c, "\x1b[H\x1b[2J");
    feed(t.c, "\x1b]0;");
    for (i = 0; i < 8; i++) feedn(t.c, big, sizeof big);
    feed(t.c, "\x1b\\" "B");
    CHK_STR(row_text(t.c, 0), "B");

    t_case("an unterminated OSC swallows the rest of the stream but nothing "
           "more (no overflow, no crash)");
    feed(t.c, "\x1b[H\x1b[2J");
    feed(t.c, "\x1b]0;");
    for (i = 0; i < 16; i++) feedn(t.c, big, sizeof big);
    CHK_STR(row_text(t.c, 0), "");
    CHK_TRUE(tc_guards_intact(&t));
    feed(t.c, "\x07" "C");
    CHK_STR(row_text(t.c, 0), "C");
    tc_free(&t);
}

static void case_dcs_never_buffers(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    char blob[4096];
    int i;

    t_case("a megabyte of DCS payload is discarded byte by byte (B4)");
    REQUIRE(t.c != NULL);
    memset(blob, 'q', sizeof blob);
    feed(t.c, "\x1bP");
    for (i = 0; i < 256; i++) feedn(t.c, blob, sizeof blob);
    feed(t.c, "\x1b\\" "A");
    CHK_STR(row_text(t.c, 0), "A");
    CHK_TRUE(tc_guards_intact(&t));

    t_case("DCS payload containing ESC-not-ST does not blow up");
    feed(t.c, "\x1b[H\x1b[2J");
    feed(t.c, "\x1bP1;2;3q\x1b[31m\x1b]0;t\x07 still inside \x1b\\" "B");
    {
        char why[256];
        CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
        if (why[0]) printf("     invariant: %s\n", why);
    }
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

int main(void)
{
    t_suite("bounds");
    case_param_count_cap();
    case_param_value_saturation();
    case_quantity_clamp_is_cheap();
    case_osc_limit();
    case_dcs_never_buffers();
    return t_summary();
}
