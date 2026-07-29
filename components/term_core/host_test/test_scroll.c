/*
 * test_scroll.c — DECSTBM, scrolling inside the region, and what reaches
 * scrollback.
 *
 * Design §6 row 3 ("スクロールリージョン"); §3.1 "上端から押し出すときに …
 * スクロールバックへ入れる" — only lines pushed off the TOP of the grid are
 * archived; header term_core_scroll_region()/TERM_ROW_SOFTWRAP.
 */
#include "test_util.h"

/* 10 columns x 6 rows, each row labelled r0..r5, cursor left at home. */
static tcore_t make_rows(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 6);
    int i;
    char b[32];
    if (!t.c) return t;
    for (i = 0; i < 6; i++) {
        sprintf(b, "\x1b[%d;1Hr%d", i + 1, i);
        feed(t.c, b);
    }
    feed(t.c, "\x1b[H");
    term_core_dirty_clear(t.c);
    return t;
}

static void case_decstbm(void)
{
    tcore_t t = make_rows();
    int top = -1, bot = -1;

    t_case("DECSTBM sets the region, 1-based on the wire");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[3;6r");
    term_core_scroll_region(t.c, &top, &bot);
    CHK_INT(top, 2);
    CHK_INT(bot, 5);

    t_case("DECSTBM leaves the cursor at the top of the addressable area");
    CHK_INT(cursor_col(t.c), 0);
    CHK_TRUE(cursor_row(t.c) == 0 || cursor_row(t.c) == 2);

    t_case("CSI r with no parameters restores the full screen");
    feed(t.c, "\x1b[r");
    term_core_scroll_region(t.c, &top, &bot);
    CHK_INT(top, 0);
    CHK_INT(bot, 5);
    tc_free(&t);
}

static void case_scroll_in_region(void)
{
    tcore_t t = make_rows();
    uint32_t sb_end;

    t_case("LF at the bottom of the region scrolls only inside the region");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[3;5r");        /* rows 2..4 */
    sb_end = term_core_sb_end(t.c);
    feed(t.c, "\x1b[5;1H");        /* bottom of the region */
    feed(t.c, "\n");
    CHK_STR(row_text(t.c, 0), "r0");
    CHK_STR(row_text(t.c, 1), "r1");
    CHK_STR(row_text(t.c, 2), "r3");
    CHK_STR(row_text(t.c, 3), "r4");
    CHK_STR(row_text(t.c, 4), "");
    CHK_STR(row_text(t.c, 5), "r5");
    CHK_INT(cursor_row(t.c), 4);   /* stays at the region bottom */

    t_case("a line leaving the top of a mid-screen region is NOT archived "
           "(§3.1 archives what is pushed off the top of the grid)");
    CHK_INT(term_core_sb_end(t.c), sb_end);
    tc_free(&t);
}

static void case_scroll_archives(void)
{
    tcore_t t = make_rows();
    uint32_t first, end;

    t_case("LF at the bottom of a full-screen region archives the top line");
    REQUIRE(t.c != NULL);
    end = term_core_sb_end(t.c);
    feed(t.c, "\x1b[6;1H\n");
    CHK_INT(term_core_sb_end(t.c), end + 1);
    CHK_STR(row_text(t.c, 0), "r1");
    CHK_STR(row_text(t.c, 4), "r5");
    CHK_STR(row_text(t.c, 5), "");
    CHK_INT(cursor_row(t.c), 5);
    first = term_core_sb_first(t.c);
    CHK_STR(line_text(t.c, end), "r0");
    CHK_TRUE(first <= end);

    t_case("repeated scrolling archives in order");
    feed(t.c, "\n\n");
    CHK_INT(term_core_sb_end(t.c), end + 3);
    CHK_STR(line_text(t.c, end + 1), "r1");
    CHK_STR(line_text(t.c, end + 2), "r2");
    {
        term_stats_t st;
        term_core_stats(t.c, &st);
        CHK_INT(st.lines_archived, 3);
    }
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_reverse_index(void)
{
    tcore_t t = make_rows();
    uint32_t sb_end;

    t_case("RI (ESC M) below the region top just moves the cursor up");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[3;1H\x1bM");
    CHK_INT(cursor_row(t.c), 1);
    CHK_STR(row_text(t.c, 0), "r0");
    CHK_STR(row_text(t.c, 2), "r2");

    t_case("RI at the top of the screen scrolls the region down");
    sb_end = term_core_sb_end(t.c);
    feed(t.c, "\x1b[1;1H\x1bM");
    CHK_INT(cursor_row(t.c), 0);
    CHK_STR(row_text(t.c, 0), "");
    CHK_STR(row_text(t.c, 1), "r0");
    CHK_STR(row_text(t.c, 5), "r4");
    CHK_INT(term_core_sb_end(t.c), sb_end); /* the bottom line is lost, not archived */
    tc_free(&t);
}

static void case_il_dl_respect_region(void)
{
    tcore_t t = make_rows();

    t_case("IL inside a region does not disturb rows outside it");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[2;4r");        /* rows 1..3 */
    feed(t.c, "\x1b[2;1H\x1b[1L");
    CHK_STR(row_text(t.c, 0), "r0");
    CHK_STR(row_text(t.c, 1), "");
    CHK_STR(row_text(t.c, 2), "r1");
    CHK_STR(row_text(t.c, 3), "r2");
    CHK_STR(row_text(t.c, 4), "r4");
    CHK_STR(row_text(t.c, 5), "r5");
    tc_free(&t);

    t_case("DL inside a region does not disturb rows outside it");
    t = make_rows();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[2;4r");
    feed(t.c, "\x1b[2;1H\x1b[1M");
    CHK_STR(row_text(t.c, 0), "r0");
    CHK_STR(row_text(t.c, 1), "r2");
    CHK_STR(row_text(t.c, 2), "r3");
    CHK_STR(row_text(t.c, 3), "");
    CHK_STR(row_text(t.c, 4), "r4");
    CHK_STR(row_text(t.c, 5), "r5");
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

int main(void)
{
    t_suite("scroll");
    case_decstbm();
    case_scroll_in_region();
    case_scroll_archives();
    case_reverse_index();
    case_il_dl_respect_region();
    return t_summary();
}
