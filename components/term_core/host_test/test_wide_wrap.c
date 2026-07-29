/*
 * test_wide_wrap.c — double-width CONT cells, autowrap, and row_utf8().
 *
 * Design §6 row 5 ("全角 CONT セル"), §3.1 (soft-wrap flag on the row),
 * header TERM_CP_CONT ("A CONT cell carries the lead cell's fg/bg so a
 * reverse-video run paints continuously"), header term_core_line_segment
 * ("a 2-column character never straddles the right edge; the cell before the
 * break is left blank" — the same rule as live input), and §7.2's readout
 * format (CONT skipped, trailing blanks trimmed, no attributes).
 */
#include "test_util.h"

#define HIRA_A "\xe3\x81\x82"   /* U+3042 HIRAGANA A, 2 columns */
#define HIRA_I "\xe3\x81\x84"   /* U+3044 HIRAGANA I, 2 columns */

static void case_wide_cell_pair(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 3);
    const term_cell_t *row;

    t_case("a double-width character occupies a WIDE lead + a CONT trailer");
    REQUIRE(t.c != NULL);
    feed(t.c, HIRA_A);
    row = term_core_row(t.c, 0);
    REQUIRE(row != NULL);
    CHK_HEX(row[0].cp, 0x3042);
    CHK_TRUE((row[0].flags & TERM_CELL_WIDE) != 0);
    CHK_TRUE((row[0].flags & TERM_CELL_CONT) == 0);
    CHK_HEX(row[1].cp, TERM_CP_CONT);
    CHK_TRUE((row[1].flags & TERM_CELL_CONT) != 0);
    CHK_INT(cursor_col(t.c), 2);

    t_case("the CONT cell carries the lead's colours (continuous reverse runs)");
    feed(t.c, "\x1b[H\x1b[2J\x1b[31;44m" HIRA_A);
    row = term_core_row(t.c, 0);
    REQUIRE(row != NULL);
    CHK_HEX(row[1].fg, row[0].fg);
    CHK_HEX(row[1].bg, row[0].bg);
    CHK_HEX(row[0].fg, term_pal16_at(1));

    t_case("narrow and wide characters interleave correctly");
    feed(t.c, "\x1b[H\x1b[2J\x1b[m" "a" HIRA_A "b");
    CHK_STR(row_text(t.c, 0), "a" HIRA_A "b");
    CHK_INT(cursor_col(t.c), 4);
    {
        char why[256];
        CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
        if (why[0]) printf("     invariant: %s\n", why);
    }
    tc_free(&t);
}

static void case_overwrite_halves(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 3);
    char why[256];

    t_case("overwriting the lead half leaves no orphan CONT behind");
    REQUIRE(t.c != NULL);
    feed(t.c, HIRA_A HIRA_I);
    feed(t.c, "\x1b[1;1H" "x");
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);
    CHK_HEX(cell_at(t.c, 0, 0)->cp, 'x');
    CHK_TRUE((cell_at(t.c, 0, 1)->flags & TERM_CELL_CONT) == 0);

    t_case("overwriting the trailing half leaves no orphan WIDE behind");
    feed(t.c, "\x1b[H\x1b[2J" HIRA_A HIRA_I);
    feed(t.c, "\x1b[1;2H" "y");
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);
    CHK_HEX(cell_at(t.c, 0, 1)->cp, 'y');
    CHK_TRUE((cell_at(t.c, 0, 0)->flags & TERM_CELL_WIDE) == 0);
    tc_free(&t);
}

static void case_autowrap(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 3);
    uint32_t sb_end;

    t_case("writing exactly `cols` characters does not leave the row yet "
           "(deferred wrap: the cursor stays addressable inside the grid)");
    REQUIRE(t.c != NULL);
    write_run(t.c, 'a', 10);
    CHK_INT(cursor_row(t.c), 0);
    CHK_RANGE(cursor_col(t.c), 0, 9);
    CHK_STR(row_text(t.c, 1), "");

    t_case("the next character wraps to the next row and marks SOFTWRAP");
    feed(t.c, "b");
    CHK_INT(cursor_row(t.c), 1);
    CHK_INT(cursor_col(t.c), 1);
    CHK_STR(row_text(t.c, 0), "aaaaaaaaaa");
    CHK_STR(row_text(t.c, 1), "b");
    CHK_TRUE((term_core_row_flags(t.c, 0) & TERM_ROW_SOFTWRAP) != 0);
    CHK_INT(term_core_row_flags(t.c, 1) & TERM_ROW_SOFTWRAP, 0);

    t_case("filling the LAST cell of the LAST row must not scroll by itself");
    term_core_reset(t.c);
    sb_end = term_core_sb_end(t.c);
    feed(t.c, "\x1b[1;1Htop\x1b[3;10HX");
    CHK_STR(row_text(t.c, 0), "top");
    CHK_INT(term_core_sb_end(t.c), sb_end);
    CHK_HEX(cell_at(t.c, 2, 9)->cp, 'X');

    t_case("one more character after that wraps and scrolls exactly once");
    feed(t.c, "Y");
    CHK_INT(term_core_sb_end(t.c), sb_end + 1);
    CHK_INT(cursor_row(t.c), 2);
    CHK_INT(cursor_col(t.c), 1);

    t_case("DECAWM off (DECSET ?7l): the last cell is overwritten instead");
    term_core_reset(t.c);
    feed(t.c, "\x1b[?7l");
    CHK_INT(term_core_modes(t.c) & TERM_MODE_AUTOWRAP, 0);
    write_run(t.c, 'a', 9);
    feed(t.c, "XYZ");
    CHK_INT(cursor_row(t.c), 0);
    CHK_STR(row_text(t.c, 1), "");
    CHK_HEX(cell_at(t.c, 0, 9)->cp, 'Z');
    feed(t.c, "\x1b[?7h");
    CHK_TRUE((term_core_modes(t.c) & TERM_MODE_AUTOWRAP) != 0);
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_wide_never_straddles(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 3);
    const term_cell_t *row;
    char why[256];

    t_case("a 2-column character at the right edge moves to the next row and "
           "leaves the cell before the break blank");
    REQUIRE(t.c != NULL);
    write_run(t.c, 'a', 9);          /* one free column left */
    feed(t.c, HIRA_A);
    row = term_core_row(t.c, 0);
    REQUIRE(row != NULL);
    CHK_HEX(row[9].cp, 0x20);
    CHK_INT(row[9].flags & (TERM_CELL_WIDE | TERM_CELL_CONT), 0);
    CHK_TRUE((term_core_row_flags(t.c, 0) & TERM_ROW_SOFTWRAP) != 0);
    row = term_core_row(t.c, 1);
    REQUIRE(row != NULL);
    CHK_HEX(row[0].cp, 0x3042);
    CHK_TRUE((row[0].flags & TERM_CELL_WIDE) != 0);
    CHK_HEX(row[1].cp, TERM_CP_CONT);
    CHK_INT(cursor_row(t.c), 1);
    CHK_INT(cursor_col(t.c), 2);
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);

    t_case("a run of wide characters wraps without ever straddling");
    term_core_reset(t.c);
    feed(t.c, HIRA_A HIRA_A HIRA_A HIRA_A HIRA_A HIRA_A);  /* 12 columns */
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);
    CHK_STR(row_text(t.c, 0), HIRA_A HIRA_A HIRA_A HIRA_A HIRA_A);
    CHK_STR(row_text(t.c, 1), HIRA_A);
    tc_free(&t);
}

#define VS16 "\xef\xb8\x8f"     /* U+FE0F VARIATION SELECTOR-16, width 0 */

static void case_ich_dch_wide_boundary(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 3);
    char why[256];

    t_case("ICH whose insert point splits a wide pair blanks the pair "
           "(no orphan CONT rides the shift)");
    REQUIRE(t.c != NULL);
    feed(t.c, "a" HIRA_A "b");            /* a(0) wide(1,2) b(3) */
    feed(t.c, "\x1b[1;3H\x1b[1@");        /* insert boundary on the CONT half */
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);
    CHK_STR(row_text(t.c, 0), "a   b");

    t_case("ICH at a WIDE lead shifts the pair intact, not halved");
    feed(t.c, "\x1b[H\x1b[2J" "ab" HIRA_A);   /* a b wide(2,3) */
    feed(t.c, "\x1b[1;3H\x1b[1@");
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);
    CHK_STR(row_text(t.c, 0), "ab " HIRA_A);
    CHK_HEX(cell_at(t.c, 0, 3)->cp, 0x3042);
    CHK_HEX(cell_at(t.c, 0, 4)->cp, TERM_CP_CONT);

    t_case("ICH pushing a pair over the right edge drops both halves");
    feed(t.c, "\x1b[H\x1b[2J\x1b[1;9H" HIRA_A);   /* pair at (8,9) */
    feed(t.c, "\x1b[1;1H\x1b[1@");
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);

    t_case("DCH whose count boundary lands on a CONT half leaves no orphan");
    feed(t.c, "\x1b[H\x1b[2J" "a" HIRA_A "b");    /* a(0) wide(1,2) b(3) */
    feed(t.c, "\x1b[1;1H\x1b[2P");        /* delete cells 0,1: boundary at the CONT */
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);
    CHK_STR(row_text(t.c, 0), " b");

    t_case("DCH of just the lead takes its CONT with it (pair straddles cx+n)");
    feed(t.c, "\x1b[H\x1b[2J" HIRA_A "b");        /* wide(0,1) b(2) */
    feed(t.c, "\x1b[1;1H\x1b[1P");
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);
    CHK_STR(row_text(t.c, 0), " b");
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_vs16_promotion(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 3);
    char why[256];

    t_case("VS16 promotes the preceding narrow character to a wide pair "
           "(§6 全角 CONT; ssh_vt.js feed() width-0 branch)");
    REQUIRE(t.c != NULL);
    feed(t.c, "A" VS16 "B");
    CHK_TRUE((cell_at(t.c, 0, 0)->flags & TERM_CELL_WIDE) != 0);
    CHK_HEX(cell_at(t.c, 0, 1)->cp, TERM_CP_CONT);
    CHK_HEX(cell_at(t.c, 0, 2)->cp, 'B');
    CHK_INT(cursor_col(t.c), 3);
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);

    t_case("VS16 whose CONT overwrites a wide lead breaks that pair first");
    feed(t.c, "\x1b[H\x1b[2J" "X" HIRA_A);        /* X(0) wide(1,2) */
    feed(t.c, "\x1b[1;2H" VS16);                  /* promote X: CONT lands on the lead */
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);
    CHK_TRUE((cell_at(t.c, 0, 0)->flags & TERM_CELL_WIDE) != 0);
    CHK_HEX(cell_at(t.c, 0, 1)->cp, TERM_CP_CONT);
    CHK_HEX(cell_at(t.c, 0, 2)->cp, 0x20);

    t_case("VS16 after an already-wide character changes nothing");
    feed(t.c, "\x1b[H\x1b[2J" HIRA_A VS16 "Z");
    CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
    if (why[0]) printf("     invariant: %s\n", why);
    CHK_HEX(cell_at(t.c, 0, 0)->cp, 0x3042);
    CHK_HEX(cell_at(t.c, 0, 2)->cp, 'Z');
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_row_utf8_format(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 3);
    char buf[64];
    int n;

    t_case("row_utf8(): trailing blanks are trimmed, leading ones are not");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[1;3Hab");
    CHK_STR(row_text(t.c, 0), "  ab");

    t_case("row_utf8(): a blank row is the empty string");
    CHK_STR(row_text(t.c, 1), "");

    t_case("row_utf8(): returns the needed length and truncates safely");
    feed(t.c, "\x1b[2;1Habcdefghij");
    memset(buf, 0x7e, sizeof buf);
    n = term_core_row_utf8(t.c, 1, buf, 5);
    CHK_INT(n, 10);
    CHK_TRUE(t_strnlen(buf, 5) <= 4);   /* NUL-terminated inside out_size */
    CHK_INT(buf[5], 0x7e);        /* nothing written past out_size */

    t_case("row_utf8(): out_size 0 writes nothing and still reports the length");
    memset(buf, 0x7e, sizeof buf);
    n = term_core_row_utf8(t.c, 1, buf, 0);
    CHK_INT(n, 10);
    CHK_INT(buf[0], 0x7e);

    t_case("row_utf8(): an out-of-range row is refused, not guessed");
    CHK_TRUE(term_core_row_utf8(t.c, -1, buf, sizeof buf) < 0);
    CHK_TRUE(term_core_row_utf8(t.c, 3, buf, sizeof buf) < 0);

    t_case("row_utf8() never mutates the core (§7.2: a probe must not perturb "
           "what it measures)");
    term_core_dirty_clear(t.c);
    (void)term_core_row_utf8(t.c, 0, buf, sizeof buf);
    (void)term_core_row_utf8(t.c, 1, buf, sizeof buf);
    CHK_INT(term_core_dirty_next(t.c, 0), -1);
    CHK_TRUE(!term_core_full_repaint(t.c));
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

int main(void)
{
    t_suite("wide-wrap");
    case_wide_cell_pair();
    case_overwrite_halves();
    case_autowrap();
    case_wide_never_straddles();
    case_ich_dch_wide_boundary();
    case_vs16_promotion();
    case_row_utf8_format();
    return t_summary();
}
