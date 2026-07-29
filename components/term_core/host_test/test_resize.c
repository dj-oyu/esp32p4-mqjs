/*
 * test_resize.c — the rotation path.
 *
 * Header term_core_resize(): returns -1 only for exceeding the maxima fixed at
 * init (B5, never for want of memory); content is carried over rather than
 * discarded ("the device lesson recorded in ssh_vt.js is that dropping the grid
 * and waiting for the remote to redraw leaves a plain shell prompt on a blank
 * screen"); rows lost off the top are pushed into scrollback; new area is
 * blanked in the current default colours; marks a full repaint; clamps the
 * cursor and the scroll region; alt-screen content is never archived.
 * Design §4.1: the grid is allocated once for the worst case so a rotation is a
 * resize inside memory that already exists.
 */
#include "test_util.h"

static tcore_t make_labelled(int cols, int rows)
{
    tcore_t t = tc_make(TERM_VT, cols, rows);
    int i;
    char b[64];
    if (!t.c) return t;
    for (i = 0; i < rows; i++) {
        sprintf(b, "\x1b[%d;1Hr%d", i + 1, i);
        feed(t.c, b);
    }
    return t;
}

static void case_refuses_out_of_range(void)
{
    tcore_t t = tc_make(TERM_VT, 80, 24);

    t_case("resize() refuses geometry beyond the maxima fixed at init (B5)");
    REQUIRE(t.c != NULL);
    CHK_INT(term_core_resize(t.c, TERM_MAX_COLS_DEFAULT + 1, 10), -1);
    CHK_INT(term_core_resize(t.c, 10, TERM_MAX_ROWS_DEFAULT + 1), -1);
    CHK_INT(term_core_resize(t.c, 0, 10), -1);
    CHK_INT(term_core_resize(t.c, 10, 0), -1);
    CHK_INT(term_core_resize(t.c, -5, -5), -1);
    /* cols*rows must stay <= max_cells */
    CHK_INT(term_core_resize(t.c, TERM_MAX_COLS_DEFAULT, TERM_MAX_ROWS_DEFAULT), -1);

    t_case("a refused resize changes nothing");
    CHK_INT(term_core_cols(t.c), 80);
    CHK_INT(term_core_rows(t.c), 24);

    t_case("both worst-case orientations of §4.1 are accepted");
    CHK_INT(term_core_resize(t.c, 142, 30), 0);
    CHK_INT(term_core_cols(t.c), 142);
    CHK_INT(term_core_rows(t.c), 30);
    CHK_INT(term_core_resize(t.c, 80, 53), 0);
    CHK_INT(term_core_cols(t.c), 80);
    CHK_INT(term_core_rows(t.c), 53);
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_keeps_content(void)
{
    tcore_t t = make_labelled(20, 6);

    t_case("growing keeps the content and blanks the new area in the defaults");
    REQUIRE(t.c != NULL);
    term_core_dirty_clear(t.c);
    CHK_INT(term_core_resize(t.c, 30, 8), 0);
    CHK_TRUE(term_core_full_repaint(t.c));
    CHK_STR(row_text(t.c, 0), "r0");
    CHK_STR(row_text(t.c, 5), "r5");
    CHK_STR(row_text(t.c, 7), "");
    {
        const term_cell_t *row = term_core_row(t.c, 7);
        REQUIRE(row != NULL);
        CHK_HEX(row[0].cp, 0x20);
        CHK_HEX(row[0].fg, TERM_COLOR_FG_DEFAULT);
        CHK_HEX(row[0].bg, TERM_COLOR_BG_DEFAULT);
        CHK_HEX(row[0].flags, 0);
        row = term_core_row(t.c, 0);
        CHK_HEX(row[25].cp, 0x20);
        CHK_HEX(row[25].bg, TERM_COLOR_BG_DEFAULT);
    }

    t_case("short lines are untouched by a column change");
    CHK_INT(term_core_resize(t.c, 8, 8), 0);
    CHK_STR(row_text(t.c, 0), "r0");
    CHK_STR(row_text(t.c, 5), "r5");
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_rows_off_the_top(void)
{
    tcore_t t = make_labelled(20, 8);
    uint32_t end;

    t_case("shrinking rows pushes the lines lost off the TOP into scrollback");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[8;1H");            /* cursor on the last row, shell-like */
    end = term_core_sb_end(t.c);
    CHK_INT(term_core_resize(t.c, 20, 4), 0);
    CHK_INT(term_core_rows(t.c), 4);
    CHK_INT(term_core_sb_end(t.c) - end, 4);
    CHK_STR(line_text(t.c, end), "r0");
    CHK_STR(line_text(t.c, end + 3), "r3");
    CHK_STR(row_text(t.c, 0), "r4");
    CHK_STR(row_text(t.c, 3), "r7");
    CHK_RANGE(cursor_row(t.c), 0, 3);
    tc_free(&t);
}

static void case_clamps(void)
{
    tcore_t t = make_labelled(20, 8);
    int top = -1, bot = -1;

    t_case("resize clamps the cursor into the new box");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[8;20H");
    CHK_INT(term_core_resize(t.c, 6, 3), 0);
    CHK_RANGE(cursor_col(t.c), 0, 5);
    CHK_RANGE(cursor_row(t.c), 0, 2);

    t_case("resize clamps the scroll region into the new box");
    CHK_INT(term_core_resize(t.c, 20, 8), 0);
    feed(t.c, "\x1b[4;7r");
    CHK_INT(term_core_resize(t.c, 20, 4), 0);
    term_core_scroll_region(t.c, &top, &bot);
    CHK_RANGE(top, 0, 3);
    CHK_RANGE(bot, top, 3);
    tc_free(&t);
}

static void case_alt_screen_resize(void)
{
    tcore_t t = make_labelled(20, 6);
    uint32_t end;

    t_case("resizing on the alt screen archives nothing");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[?1049h");
    feed(t.c, "\x1b[1;1Halt");
    end = term_core_sb_end(t.c);
    CHK_INT(term_core_resize(t.c, 10, 3), 0);
    CHK_INT(term_core_sb_end(t.c), end);
    CHK_TRUE(term_core_alt_active(t.c));

    t_case("leaving the alt screen still finds the main screen there");
    CHK_INT(term_core_resize(t.c, 20, 6), 0);
    feed(t.c, "\x1b[?1049l");
    CHK_STR(row_text(t.c, 0), "r0");
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_rotation_stress(void)
{
    tcore_t t = tc_make(TERM_VT, 142, 30);
    int i;
    char why[256];
    int bad = 0;

    t_case("alternating orientations keeps every invariant and every red zone");
    REQUIRE(t.c != NULL);
    for (i = 0; i < 40; i++) {
        int landscape = (i & 1) == 0;
        feed(t.c, "\x1b[31m" "some text that is wide enough to wrap somewhere "
                  "in one orientation but not in the other\r\n");
        feed(t.c, "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86 mixed width\r\n");
        if (term_core_resize(t.c, landscape ? 80 : 142, landscape ? 53 : 30) != 0) bad++;
        if (invariants_ok(t.c, why, sizeof why) != 0) {
            if (!bad) printf("     invariant at i=%d: %s\n", i, why);
            bad++;
        }
        if (!tc_guards_intact(&t)) { printf("     red zone damaged at i=%d\n", i); bad++; break; }
    }
    CHK_INT(bad, 0);

    t_case("scrollback ids survive rotation and still read back");
    if (term_core_sb_end(t.c) > term_core_sb_first(t.c)) {
        uint32_t id = term_core_sb_first(t.c);
        int len_before = term_core_line_length(t.c, id);
        char text_before[1024];
        strcpy(text_before, line_text(t.c, id));
        CHK_INT(term_core_resize(t.c, 100, 20), 0);
        CHK_INT(term_core_line_length(t.c, id), len_before);
        CHK_STR(line_text(t.c, id), text_before);
        CHK_TRUE(term_core_line_seg_count(t.c, id) >= 1);
    }
    tc_free(&t);
}

int main(void)
{
    t_suite("resize");
    case_refuses_out_of_range();
    case_keeps_content();
    case_rows_off_the_top();
    case_clamps();
    case_alt_screen_resize();
    case_rotation_stress();
    return t_summary();
}
