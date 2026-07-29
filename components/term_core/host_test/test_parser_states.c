/*
 * test_parser_states.c — the escape state machine itself.
 *
 * Design §6 row 1: "ESC/CSI/OSC/DCS ステートマシン —
 * GROUND/ESC/CSI/OSC/CHARSET/CSI_IGNORE/DCS".
 * Header B4: OSC is buffered to a limit but consumed to its terminator so the
 * stream re-syncs; DCS/SOS/PM/APC payload is never buffered, discarded to ST.
 * The through-line of every case here: after any sequence, ordinary text must
 * land in the grid unharmed.
 */
#include "test_util.h"

static void case_ground_text(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    term_stats_t st;

    t_case("GROUND: printable text advances the cursor one cell per char");
    REQUIRE(t.c != NULL);
    feed(t.c, "hi");
    CHK_STR(row_text(t.c, 0), "hi");
    CHK_INT(cursor_col(t.c), 2);
    CHK_INT(cursor_row(t.c), 0);
    term_core_stats(t.c, &st);
    CHK_INT(st.bytes_in, 2);
    CHK_INT(st.chars_written, 2);
    tc_free(&t);
}

static void case_c0_cr_lf_bs(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);

    t_case("C0: CR homes the column, LF moves down without homing");
    REQUIRE(t.c != NULL);
    feed(t.c, "ab\n");
    CHK_INT(cursor_row(t.c), 1);
    CHK_INT(cursor_col(t.c), 2);   /* LF is not CRLF (LNM off) */
    feed(t.c, "\r");
    CHK_INT(cursor_col(t.c), 0);
    CHK_INT(cursor_row(t.c), 1);

    t_case("C0: CR then text overwrites from column 0");
    term_core_reset(t.c);
    feed(t.c, "abc\rX");
    CHK_STR(row_text(t.c, 0), "Xbc");
    CHK_INT(cursor_col(t.c), 1);

    t_case("C0: BS steps back one column");
    term_core_reset(t.c);
    feed(t.c, "abc\b\bX");
    CHK_STR(row_text(t.c, 0), "aXc");
    CHK_INT(cursor_col(t.c), 2);

    t_case("C0: NUL is not a glyph");
    term_core_reset(t.c);
    feedn(t.c, "a\0b", 3);
    {
        const term_cell_t *row = term_core_row(t.c, 0);
        int i, zeros = 0;
        REQUIRE(row != NULL);
        for (i = 0; i < 20; i++) if (row[i].cp == 0) zeros++;
        CHK_INT(zeros, 0);
    }
    tc_free(&t);
}

static void case_esc_csi_resync(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);

    t_case("ESC->CSI: the sequence is consumed, the text around it is not");
    REQUIRE(t.c != NULL);
    feed(t.c, "A\x1b[31mB");
    CHK_STR(row_text(t.c, 0), "AB");
    CHK_INT(cursor_col(t.c), 2);

    t_case("a CSI split across feed() calls still parses (state is carried)");
    term_core_reset(t.c);
    feed(t.c, "\x1b");
    feed(t.c, "[3");
    feed(t.c, "1m");
    feed(t.c, "X");
    CHK_STR(row_text(t.c, 0), "X");
    CHK_HEX(cell_at(t.c, 0, 0)->fg, term_pal16_at(1));

    t_case("a stray ESC restarts the sequence instead of derailing the stream");
    term_core_reset(t.c);
    feed(t.c, "\x1b\x1b[31mX");
    CHK_STR(row_text(t.c, 0), "X");
    /* The second ESC starts a complete, well-formed CSI SGR (§6 "SGR
     * 16/256/truecolor"), so it must be *executed*, not merely swallowed:
     * discarding ESC..final would also leave "X" alone. */
    CHK_HEX(cell_at(t.c, 0, 0)->fg, term_pal16_at(1));

    t_case("CSI_IGNORE: a malformed parameter list is dropped and text resumes");
    term_core_reset(t.c);
    feed(t.c, "\x1b[1;2;3:4;5<=>?m" "Z");
    CHK_STR(row_text(t.c, 0), "Z");
    CHK_INT(cursor_row(t.c), 0);

    t_case("an unsupported but well-formed CSI counts as seq_ignored");
    {
        term_stats_t st;
        term_core_reset(t.c);
        feed(t.c, "\x1b[5y");
        term_core_stats(t.c, &st);
        CHK_TRUE(st.seq_ignored >= 1);
        CHK_STR(row_text(t.c, 0), "");
    }
    tc_free(&t);
}

static void case_charset(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);

    t_case("CHARSET: ESC ( B consumes its designator and prints nothing");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b(B" "A");
    CHK_STR(row_text(t.c, 0), "A");
    CHK_INT(cursor_col(t.c), 1);

    t_case("CHARSET: a G1 designator does not leak into the grid either");
    term_core_reset(t.c);
    feed(t.c, "\x1b)0" "B");
    CHK_STR(row_text(t.c, 0), "B");
    tc_free(&t);
}

static void case_osc(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    term_stats_t st;

    t_case("OSC terminated by BEL: payload never reaches the grid");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b]0;window title\x07" "A");
    CHK_STR(row_text(t.c, 0), "A");
    CHK_INT(cursor_col(t.c), 1);

    t_case("OSC terminated by ST (ESC backslash)");
    term_core_reset(t.c);
    feed(t.c, "\x1b]0;window title\x1b\\" "B");
    CHK_STR(row_text(t.c, 0), "B");

    t_case("OSC payload may contain semicolons, UTF-8 and digits");
    term_core_reset(t.c);
    feed(t.c, "\x1b]52;c;\xe6\x97\xa5\xe6\x9c\xac;YWJj\x07" "C");
    CHK_STR(row_text(t.c, 0), "C");

    t_case("an ESC inside an OSC aborts the string and introduces the next "
           "sequence (ssh_vt.js: ESC in OSC -> ESC state), so a following "
           "CSI executes instead of leaking into the grid as text");
    term_core_reset(t.c);
    feed(t.c, "\x1b]0;title\x1b[31m" "E");
    CHK_STR(row_text(t.c, 0), "E");
    CHK_HEX(cell_at(t.c, 0, 0)->fg, term_pal16_at(1));

    t_case("a truncated OSC does not truncate the stream (B4 re-sync)");
    term_core_reset(t.c);
    {
        char big[600];
        memset(big, 'T', sizeof big);
        feed(t.c, "\x1b]0;");
        feedn(t.c, big, sizeof big);
        feed(t.c, "\x07" "D");
        CHK_STR(row_text(t.c, 0), "D");
        term_core_stats(t.c, &st);
        CHK_TRUE(st.osc_truncated >= 1);
    }
    tc_free(&t);
}

static void case_dcs_and_friends(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    int i;

    t_case("DCS: payload discarded to ST, then text resumes");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1bP1$r0m\x1b\\" "A");
    CHK_STR(row_text(t.c, 0), "A");

    t_case("DCS: a huge payload is never buffered (B4) and never printed");
    term_core_reset(t.c);
    feed(t.c, "\x1bP");
    for (i = 0; i < 1000; i++) feed(t.c, "0123456789abcdefghijklmnopqrstuvwxyz;:?");
    feed(t.c, "\x1b\\" "B");
    CHK_STR(row_text(t.c, 0), "B");
    CHK_INT(cursor_row(t.c), 0);
    CHK_INT(cursor_col(t.c), 1);

    t_case("SOS / PM / APC strings are discarded to ST as well");
    term_core_reset(t.c);
    feed(t.c, "\x1bXsos payload\x1b\\" "1");
    feed(t.c, "\x1b^pm payload\x1b\\" "2");
    feed(t.c, "\x1b_apc payload\x1b\\" "3");
    CHK_STR(row_text(t.c, 0), "123");

    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

int main(void)
{
    t_suite("parser-states");
    case_ground_text();
    case_c0_cr_lf_bs();
    case_esc_csi_resync();
    case_charset();
    case_osc();
    case_dcs_and_friends();
    return t_summary();
}
