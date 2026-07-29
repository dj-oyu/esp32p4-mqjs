/*
 * test_modes.c — DECSET/DECRST private modes and the alt screen.
 *
 * Design §6 rows "alt screen (DECSET 1049) — neovim 必須。grid 2 面の理由",
 * "カーソル表示/非表示 (25)", "bracketed paste (2004)"; §3.2 "alt screen:
 * あり(履歴を汚さない)" and TERM_LOG "alt screen: なし"; header
 * term_core_alt_active(): "NOTHING reaches scrollback until 1049 is reset —
 * that non-pollution is the contract, not a side effect".
 */
#include "test_util.h"

static void case_cursor_visibility(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);

    t_case("DECSET 25: visible by default, hidden by ?25l, back with ?25h");
    REQUIRE(t.c != NULL);
    CHK_TRUE(cursor_visible(t.c));
    feed(t.c, "\x1b[?25l");
    CHK_TRUE(!cursor_visible(t.c));
    CHK_INT(term_core_modes(t.c) & TERM_MODE_CURSOR_VISIBLE, 0);
    feed(t.c, "\x1b[?25h");
    CHK_TRUE(cursor_visible(t.c));
    CHK_TRUE((term_core_modes(t.c) & TERM_MODE_CURSOR_VISIBLE) != 0);

    t_case("hiding the cursor does not disturb the grid or the position");
    feed(t.c, "\x1b[2;3Habc\x1b[?25l");
    CHK_STR(row_text(t.c, 1), "  abc");
    CHK_INT(cursor_row(t.c), 1);
    CHK_INT(cursor_col(t.c), 5);
    tc_free(&t);
}

static void case_bracketed_and_appcursor(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);

    t_case("DECSET 2004 (bracketed paste) is off by default and toggles");
    REQUIRE(t.c != NULL);
    CHK_INT(term_core_modes(t.c) & TERM_MODE_BRACKETED, 0);
    feed(t.c, "\x1b[?2004h");
    CHK_TRUE((term_core_modes(t.c) & TERM_MODE_BRACKETED) != 0);
    feed(t.c, "\x1b[?2004l");
    CHK_INT(term_core_modes(t.c) & TERM_MODE_BRACKETED, 0);

    t_case("DECCKM (DECSET 1) toggles TERM_MODE_APP_CURSOR");
    CHK_INT(term_core_modes(t.c) & TERM_MODE_APP_CURSOR, 0);
    feed(t.c, "\x1b[?1h");
    CHK_TRUE((term_core_modes(t.c) & TERM_MODE_APP_CURSOR) != 0);
    feed(t.c, "\x1b[?1l");
    CHK_INT(term_core_modes(t.c) & TERM_MODE_APP_CURSOR, 0);

    t_case("an unknown private mode changes nothing and sets no unknown bits");
    {
        uint32_t before = term_core_modes(t.c);
        feed(t.c, "\x1b[?12345h\x1b[?9999l\x1b[?1000h");
        CHK_INT(term_core_modes(t.c) & ~(uint32_t)TERM_MODE_ALL_KNOWN, 0);
        CHK_INT(term_core_modes(t.c), before);
        CHK_STR(row_text(t.c, 0), "");
    }
    tc_free(&t);
}

static void case_alt_screen(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    char main0[128], main1[128];
    uint32_t sb_end_before;
    int i;

    t_case("DECSET 1049 switches to a blank alt screen and forces a repaint");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;1Hmain-row-0\x1b[2;1Hmain-row-1");
    strcpy(main0, row_text(t.c, 0));
    strcpy(main1, row_text(t.c, 1));
    sb_end_before = term_core_sb_end(t.c);
    term_core_dirty_clear(t.c);

    feed(t.c, "\x1b[?1049h");
    CHK_TRUE(term_core_alt_active(t.c));
    CHK_TRUE((term_core_modes(t.c) & TERM_MODE_ALT_SCREEN) != 0);
    CHK_TRUE(term_core_full_repaint(t.c));
    CHK_STR(row_text(t.c, 0), "");
    CHK_STR(row_text(t.c, 1), "");

    t_case("nothing written on the alt screen reaches scrollback (§3.2)");
    feed(t.c, "\x1b[1;1Halt-row-0");
    for (i = 0; i < 50; i++) feed(t.c, "\x1b[4;1Hscrolling\n");
    CHK_INT(term_core_sb_end(t.c), sb_end_before);
    {
        term_stats_t st;
        term_core_stats(t.c, &st);
        CHK_INT(st.lines_archived, 0);
    }

    t_case("DECRST 1049 returns to an untouched main screen");
    term_core_dirty_clear(t.c);
    feed(t.c, "\x1b[?1049l");
    CHK_TRUE(!term_core_alt_active(t.c));
    CHK_INT(term_core_modes(t.c) & TERM_MODE_ALT_SCREEN, 0);
    CHK_TRUE(term_core_full_repaint(t.c));
    CHK_STR(row_text(t.c, 0), main0);
    CHK_STR(row_text(t.c, 1), main1);
    CHK_INT(term_core_sb_end(t.c), sb_end_before);

    t_case("re-entering the alt screen starts blank again");
    feed(t.c, "\x1b[?1049h");
    CHK_STR(row_text(t.c, 0), "");
    feed(t.c, "\x1b[?1049l");

    t_case("DECRST 1049 while already on the main screen is harmless");
    feed(t.c, "\x1b[?1049l");
    CHK_TRUE(!term_core_alt_active(t.c));
    CHK_STR(row_text(t.c, 0), main0);
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_alt_screen_ignored_in_log_mode(void)
{
    tcore_t t = tc_make(TERM_LOG, 20, 4);
    uint32_t sb_end;
    int i;

    t_case("TERM_LOG: DECSET 1049 is accepted and ignored (header term_mode_t)");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;1Hlog-row-0");
    feed(t.c, "\x1b[?1049h");
    CHK_TRUE(!term_core_alt_active(t.c));
    CHK_INT(term_core_modes(t.c) & TERM_MODE_ALT_SCREEN, 0);
    CHK_STR(row_text(t.c, 0), "log-row-0");

    t_case("TERM_LOG: scrollback keeps accumulating with 1049 'set'");
    sb_end = term_core_sb_end(t.c);
    feed(t.c, "\x1b[4;1H");
    for (i = 0; i < 6; i++) feed(t.c, "\nline");
    CHK_TRUE(term_core_sb_end(t.c) > sb_end);
    feed(t.c, "\x1b[?1049l");
    CHK_TRUE(!term_core_alt_active(t.c));
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

int main(void)
{
    t_suite("modes");
    case_cursor_visibility();
    case_bracketed_and_appcursor();
    case_alt_screen();
    case_alt_screen_ignored_in_log_mode();
    return t_summary();
}
