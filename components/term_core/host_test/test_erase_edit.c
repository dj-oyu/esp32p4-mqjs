/*
 * test_erase_edit.c — ED / EL / ECH / ICH / DCH / IL / DL / REP and their clamps.
 *
 * Design §6 row 2 ("カーソル操作・ED/EL・ICH/DCH/IL/DL"), §5 and header B2:
 * "数量系パラメータ(REP・IL/DL・ICH/DCH の反復数)は grid 寸法で clamp —
 * 「2^31 行挿入」は rows 分の仕事にしかならない".
 */
#include "test_util.h"

/* 10x5 grid filled with five distinguishable rows. */
static tcore_t make_filled(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 5);
    if (!t.c) return t;
    /* Addressed with CUP rather than CRLF so this fixture does not depend on
     * the autowrap edge (that has its own suite). */
    feed(t.c, "\x1b[1;1Habcdefghij");
    feed(t.c, "\x1b[2;1H0000000000");
    feed(t.c, "\x1b[3;1H1111111111");
    feed(t.c, "\x1b[4;1H2222222222");
    feed(t.c, "\x1b[5;1H3333333333");
    feed(t.c, "\x1b[H");
    term_core_dirty_clear(t.c);
    return t;
}

static void case_el(void)
{
    tcore_t t;

    t_case("EL 0 erases from the cursor to the end of the line");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;5H\x1b[0K");
    CHK_STR(row_text(t.c, 0), "abcd");
    CHK_INT(cursor_col(t.c), 4);          /* EL does not move the cursor */
    CHK_TRUE(term_core_row_dirty(t.c, 0));
    CHK_STR(row_text(t.c, 1), "0000000000");
    tc_free(&t);

    t_case("EL with no parameter behaves as EL 0");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;5H\x1b[K");
    CHK_STR(row_text(t.c, 0), "abcd");
    tc_free(&t);

    t_case("EL 1 erases from the start of the line through the cursor");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;5H\x1b[1K");
    CHK_STR(row_text(t.c, 0), "     fghij");
    tc_free(&t);

    t_case("EL 2 erases the whole line");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;5H\x1b[2K");
    CHK_STR(row_text(t.c, 0), "");
    CHK_STR(row_text(t.c, 1), "0000000000");
    tc_free(&t);

    t_case("erased cells become blanks with no leftover attributes");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;1H\x1b[2K");
    {
        const term_cell_t *row = term_core_row(t.c, 0);
        int i, bad = -1;
        REQUIRE(row != NULL);
        for (i = 0; i < 10; i++) {
            if (row[i].cp != 0x20u ||
                (row[i].flags & (TERM_CELL_WIDE | TERM_CELL_CONT | TERM_CELL_UNDERLINE)) != 0) {
                bad = i;
                break;
            }
        }
        CHK_INT(bad, -1);
    }
    tc_free(&t);
}

static void case_ed(void)
{
    tcore_t t;
    uint32_t sb_end;

    t_case("ED 0 erases from the cursor to the end of the screen");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[3;5H\x1b[0J");
    CHK_STR(row_text(t.c, 0), "abcdefghij");
    CHK_STR(row_text(t.c, 1), "0000000000");
    CHK_STR(row_text(t.c, 2), "1111");
    CHK_STR(row_text(t.c, 3), "");
    CHK_STR(row_text(t.c, 4), "");
    tc_free(&t);

    t_case("ED 1 erases from the start of the screen through the cursor");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[3;5H\x1b[1J");
    CHK_STR(row_text(t.c, 0), "");
    CHK_STR(row_text(t.c, 1), "");
    CHK_STR(row_text(t.c, 2), "     11111");
    CHK_STR(row_text(t.c, 3), "2222222222");
    tc_free(&t);

    t_case("ED 2 erases the whole screen and archives nothing (§3.1: only "
           "lines pushed off the top become scrollback)");
    t = make_filled();
    REQUIRE(t.c != NULL);
    sb_end = term_core_sb_end(t.c);
    feed(t.c, "\x1b[3;5H\x1b[2J");
    CHK_STR(row_text(t.c, 0), "");
    CHK_STR(row_text(t.c, 2), "");
    CHK_STR(row_text(t.c, 4), "");
    CHK_INT(term_core_sb_end(t.c), sb_end);
    CHK_TRUE(term_core_row_dirty(t.c, 0));
    CHK_TRUE(term_core_row_dirty(t.c, 4));
    tc_free(&t);
}

static void case_ech(void)
{
    tcore_t t;

    t_case("ECH blanks n cells in place without moving the cursor");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;3H\x1b[3X");
    CHK_STR(row_text(t.c, 0), "ab   fghij");
    CHK_INT(cursor_col(t.c), 2);
    tc_free(&t);

    t_case("ECH with an absurd count stops at the end of the line (B2)");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;3H\x1b[2000000000X");
    CHK_STR(row_text(t.c, 0), "ab");
    CHK_STR(row_text(t.c, 1), "0000000000");
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_ich_dch(void)
{
    tcore_t t;

    t_case("ICH inserts blanks and pushes the tail off the right edge");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;4H\x1b[2@");
    CHK_STR(row_text(t.c, 0), "abc  defgh");
    CHK_INT(cursor_col(t.c), 3);
    CHK_STR(row_text(t.c, 1), "0000000000");
    tc_free(&t);

    t_case("ICH clamped: an absurd count blanks the rest of the line only (B2)");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;4H\x1b[2000000000@");
    CHK_STR(row_text(t.c, 0), "abc");
    CHK_STR(row_text(t.c, 1), "0000000000");
    tc_free(&t);

    t_case("DCH deletes and pulls the tail left, blanking the end");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;4H\x1b[2P");
    CHK_STR(row_text(t.c, 0), "abcfghij");
    CHK_INT(cursor_col(t.c), 3);
    tc_free(&t);

    t_case("DCH clamped: an absurd count empties the rest of the line (B2)");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;4H\x1b[2000000000P");
    CHK_STR(row_text(t.c, 0), "abc");
    CHK_STR(row_text(t.c, 1), "0000000000");
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_il_dl(void)
{
    tcore_t t;
    uint32_t sb_end;

    t_case("IL inserts a blank line, pushing the lower lines down and off");
    t = make_filled();
    REQUIRE(t.c != NULL);
    sb_end = term_core_sb_end(t.c);
    feed(t.c, "\x1b[3;1H\x1b[1L");
    CHK_STR(row_text(t.c, 0), "abcdefghij");
    CHK_STR(row_text(t.c, 1), "0000000000");
    CHK_STR(row_text(t.c, 2), "");
    CHK_STR(row_text(t.c, 3), "1111111111");
    CHK_STR(row_text(t.c, 4), "2222222222");
    CHK_INT(term_core_sb_end(t.c), sb_end);  /* pushed off the BOTTOM: lost */
    tc_free(&t);

    t_case("IL clamped: an absurd count blanks to the end of the region (B2)");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[3;1H\x1b[2000000000L");
    CHK_STR(row_text(t.c, 0), "abcdefghij");
    CHK_STR(row_text(t.c, 1), "0000000000");
    CHK_STR(row_text(t.c, 2), "");
    CHK_STR(row_text(t.c, 3), "");
    CHK_STR(row_text(t.c, 4), "");
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);

    t_case("DL removes a line and pulls the lower lines up");
    t = make_filled();
    REQUIRE(t.c != NULL);
    sb_end = term_core_sb_end(t.c);
    feed(t.c, "\x1b[3;1H\x1b[1M");
    CHK_STR(row_text(t.c, 0), "abcdefghij");
    CHK_STR(row_text(t.c, 1), "0000000000");
    CHK_STR(row_text(t.c, 2), "2222222222");
    CHK_STR(row_text(t.c, 3), "3333333333");
    CHK_STR(row_text(t.c, 4), "");
    CHK_INT(term_core_sb_end(t.c), sb_end);
    tc_free(&t);

    t_case("DL clamped: an absurd count empties to the end of the region (B2)");
    t = make_filled();
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[3;1H\x1b[2000000000M");
    CHK_STR(row_text(t.c, 0), "abcdefghij");
    CHK_STR(row_text(t.c, 1), "0000000000");
    CHK_STR(row_text(t.c, 2), "");
    CHK_STR(row_text(t.c, 4), "");
    tc_free(&t);
}

static void case_rep(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 3);
    const term_cell_t *row;
    int i, run;

    t_case("REP repeats the preceding character (count as written, ECMA-48 "
           "leaves total = n or n+1 ambiguous, so only the run is asserted)");
    REQUIRE(t.c != NULL);
    feed(t.c, "A\x1b[3b");
    row = term_core_row(t.c, 0);
    REQUIRE(row != NULL);
    run = 0;
    for (i = 0; i < 10; i++) {
        if (row[i].cp == (uint32_t)'A') run++;
        else break;
    }
    CHK_RANGE(run, 3, 4);
    CHK_INT(cursor_col(t.c), run);
    for (i = run; i < 10; i++) CHK_HEX(row[i].cp, 0x20);

    t_case("an absurd REP costs at most a screen of work and stays in bounds (B2)");
    term_core_reset(t.c);
    feed(t.c, "B\x1b[2000000000b");
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
    t_suite("erase-edit");
    case_el();
    case_ed();
    case_ech();
    case_ich_dch();
    case_il_dl();
    case_rep();
    return t_summary();
}
