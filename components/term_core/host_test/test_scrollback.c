/*
 * test_scrollback.c — logical lines, the soft-wrap join, and re-wrap on read.
 *
 * Design §3.1: "VT のスクロールバックも論理行で持つ — grid の各行に soft-wrap
 * フラグを持ち、上端から押し出すときに soft-wrap で繋がった行を 1 本の論理行に
 * 結合してからスクロールバックへ入れる。表示時に現在幅で再折り返しする …
 * これで回転(cols 変化)後も VT のスクロールバックが旧幅の折り返し境界のまま
 * 化けない".
 * Header: ids are monotonically increasing, never reused, survive resize; a
 * dead id answers -1; term_core_line_seg_count/segment re-wrap at the current
 * width with the same wide-character rule as live input.
 */
#include "test_util.h"

#define HIRA_A "\xe3\x81\x82"   /* U+3042, 2 columns */

/* 26 distinct characters, so a joined logical line is unambiguous. */
static const char ALPHA25[] = "ABCDEFGHIJKLMNOPQRSTUVWXY";

static void case_hard_lines(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    uint32_t base;
    int i;
    term_stats_t st;

    t_case("hard-wrapped lines archive one-for-one, in order, with ids");
    REQUIRE(t.c != NULL);
    base = term_core_sb_end(t.c);
    for (i = 0; i < 10; i++) {
        char b[32];
        sprintf(b, "line%d\r\n", i);
        feed(t.c, b);
    }
    /* 10 lines written into 4 rows: 7 of them have been pushed off the top. */
    CHK_INT(term_core_sb_end(t.c) - base, 7);
    CHK_INT(term_core_sb_first(t.c), base);
    for (i = 0; i < 7; i++) {
        char want[32];
        sprintf(want, "line%d", i);
        CHK_STR(line_text(t.c, base + (uint32_t)i), want);
        CHK_INT(term_core_line_length(t.c, base + (uint32_t)i), (int)strlen(want));
        CHK_INT(term_core_line_seg_count(t.c, base + (uint32_t)i), 1);
    }
    term_core_stats(t.c, &st);
    CHK_INT(st.lines_archived, 7);

    t_case("a dead id is refused by every scrollback accessor");
    {
        term_cell_t cells[64];
        term_attr_run_t runs[8];
        char buf[64];
        uint32_t dead = term_core_sb_end(t.c);
        CHK_INT(term_core_line_length(t.c, dead), -1);
        CHK_INT(term_core_line_utf8(t.c, dead, buf, sizeof buf), -1);
        CHK_INT(term_core_line_seg_count(t.c, dead), -1);
        CHK_INT(term_core_line_segment(t.c, dead, 0, cells, 64), -1);
        CHK_INT(term_core_line_attrs(t.c, dead, runs, 8), -1);
        CHK_INT(term_core_line_length(t.c, 0xFFFFFFFFu), -1);
        CHK_INT(term_core_line_seg_count(t.c, dead + 1000), -1);
    }
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_softwrap_join(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 4);
    uint32_t id;
    uint32_t count;

    t_case("a soft-wrapped run becomes ONE logical line in scrollback (§3.1)");
    REQUIRE(t.c != NULL);
    feed(t.c, ALPHA25);                 /* 25 columns over 3 rows of 10 */
    CHK_TRUE((term_core_row_flags(t.c, 0) & TERM_ROW_SOFTWRAP) != 0);
    CHK_TRUE((term_core_row_flags(t.c, 1) & TERM_ROW_SOFTWRAP) != 0);
    CHK_INT(term_core_row_flags(t.c, 2) & TERM_ROW_SOFTWRAP, 0);

    feed(t.c, "\x1b[4;1H\n\n\n\n");     /* push all four rows off the top */
    id = term_core_sb_first(t.c);
    count = term_core_sb_end(t.c) - id;
    CHK_RANGE(count, 1, 2);             /* the joined line (+ a blank row) */
    CHK_STR(line_text(t.c, id), ALPHA25);
    CHK_INT(term_core_line_length(t.c, id), 25);

    t_case("the logical line re-wraps at the CURRENT width on read");
    CHK_INT(term_core_line_seg_count(t.c, id), 3);
    {
        term_cell_t cells[64];
        int n = term_core_line_segment(t.c, id, 0, cells, 64);
        CHK_INT(n, 10);
        CHK_HEX(cells[0].cp, 'A');
        CHK_HEX(cells[9].cp, 'J');
        n = term_core_line_segment(t.c, id, 1, cells, 64);
        CHK_INT(n, 10);
        CHK_HEX(cells[0].cp, 'K');
        n = term_core_line_segment(t.c, id, 2, cells, 64);
        CHK_INT(n, 5);
        CHK_HEX(cells[0].cp, 'U');
        CHK_HEX(cells[4].cp, 'Y');
        CHK_INT(term_core_line_segment(t.c, id, 3, cells, 64), -1);
        CHK_INT(term_core_line_segment(t.c, id, -1, cells, 64), -1);
    }

    t_case("segment() honours max_cells");
    {
        term_cell_t cells[4];
        int n = term_core_line_segment(t.c, id, 0, cells, 3);
        CHK_RANGE(n, 0, 3);
    }

    t_case("after a resize the SAME id re-wraps at the NEW width "
           "(rotation must not freeze yesterday's history at yesterday's cols)");
    CHK_INT(term_core_resize(t.c, 25, 4), 0);
    CHK_INT(term_core_line_seg_count(t.c, id), 1);
    CHK_STR(line_text(t.c, id), ALPHA25);
    CHK_INT(term_core_line_length(t.c, id), 25);
    {
        term_cell_t cells[64];
        int n = term_core_line_segment(t.c, id, 0, cells, 64);
        CHK_INT(n, 25);
        CHK_HEX(cells[24].cp, 'Y');
    }

    CHK_INT(term_core_resize(t.c, 5, 4), 0);
    CHK_INT(term_core_line_seg_count(t.c, id), 5);
    CHK_STR(line_text(t.c, id), ALPHA25);
    {
        term_cell_t cells[64];
        int n = term_core_line_segment(t.c, id, 4, cells, 64);
        CHK_INT(n, 5);
        CHK_HEX(cells[0].cp, 'U');
    }
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_wide_rewrap(void)
{
    tcore_t t = tc_make(TERM_VT, 8, 3);
    uint32_t id;

    t_case("a logical line of wide characters counts 2 cells each");
    REQUIRE(t.c != NULL);
    feed(t.c, HIRA_A HIRA_A HIRA_A HIRA_A);      /* 8 columns, fills row 0 */
    feed(t.c, "\x1b[3;1H\n\n\n");
    id = term_core_sb_first(t.c);
    CHK_INT(term_core_line_length(t.c, id), 8);
    CHK_STR(line_text(t.c, id), HIRA_A HIRA_A HIRA_A HIRA_A);

    t_case("re-wrapping at an odd width leaves the cell before the break blank "
           "(a 2-column character never straddles)");
    CHK_INT(term_core_resize(t.c, 5, 3), 0);
    CHK_INT(term_core_line_seg_count(t.c, id), 2);
    {
        term_cell_t cells[16];
        int n = term_core_line_segment(t.c, id, 0, cells, 16);
        /* 4 cells of text; the 5th is the blank the wide char could not use.
         * Whether the segment reports that blank is not fixed by the design,
         * so both 4 and 5 pass — what must NOT happen is a straddling pair. */
        CHK_RANGE(n, 4, 5);
        CHK_HEX(cells[0].cp, 0x3042);
        CHK_HEX(cells[1].cp, TERM_CP_CONT);
        CHK_HEX(cells[2].cp, 0x3042);
        CHK_HEX(cells[3].cp, TERM_CP_CONT);
        if (n == 5) CHK_HEX(cells[4].cp, 0x20);
        n = term_core_line_segment(t.c, id, 1, cells, 16);
        CHK_INT(n, 4);
        CHK_HEX(cells[0].cp, 0x3042);
    }
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_attr_runs(void)
{
    tcore_t t = tc_make(TERM_VT, 12, 3);
    uint32_t id;
    term_attr_run_t runs[16];
    int total, i, sum;

    t_case("attribute runs tile the logical line and sum to its length");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[31maaa\x1b[mbbb");
    feed(t.c, "\x1b[3;1H\n\n\n");
    id = term_core_sb_first(t.c);
    CHK_INT(term_core_line_length(t.c, id), 6);

    memset(runs, 0, sizeof runs);
    total = term_core_line_attrs(t.c, id, runs, 16);
    CHK_TRUE(total >= 2);
    if (total >= 2) {
        int last = total < 16 ? total - 1 : 15;
        sum = 0;
        for (i = 0; i < total && i < 16; i++) sum += runs[i].cells;
        CHK_INT(sum, 6);
        CHK_INT(runs[0].cells, 3);
        CHK_HEX(runs[0].fg, term_pal16_at(1));
        CHK_HEX(runs[last].fg, TERM_COLOR_FG_DEFAULT);
    }

    t_case("line_attrs() counts first when asked for zero runs");
    CHK_INT(term_core_line_attrs(t.c, id, NULL, 0), total);

    t_case("line_attrs() reports the total even when truncated");
    CHK_INT(term_core_line_attrs(t.c, id, runs, 1), total);
    tc_free(&t);
}

static void case_eviction_and_ids(void)
{
    term_config_t cfg = tc_cfg(TERM_VT, 10, 4);
    tcore_t t;
    uint32_t first, end;
    int i;
    term_stats_t st;

    cfg.max_cols = 10;
    cfg.max_rows = 4;
    cfg.max_cells = 40;
    cfg.scrollback_bytes = 512;
    cfg.scrollback_lines = 8;
    t = tc_make_cfg(&cfg);

    t_case("the scrollback ring evicts the oldest and says so");
    REQUIRE(t.c != NULL);
    for (i = 0; i < 60; i++) {
        char b[32];
        sprintf(b, "row%02d\r\n", i);
        feed(t.c, b);
    }
    first = term_core_sb_first(t.c);
    end = term_core_sb_end(t.c);
    CHK_TRUE(first > 0);
    CHK_TRUE(end - first <= 8);
    term_core_stats(t.c, &st);
    CHK_TRUE(st.lines_evicted > 0);
    CHK_INT(st.lines_archived - st.lines_evicted, (long long)(end - first));

    t_case("evicted ids are dead, surviving ids still read back");
    CHK_INT(term_core_line_length(t.c, first - 1), -1);
    CHK_TRUE(term_core_line_length(t.c, first) >= 0);
    CHK_TRUE(term_core_line_length(t.c, end - 1) >= 0);
    /* every surviving line is one of the "rowNN" lines, in order */
    {
        uint32_t id;
        int ordered = 1;
        for (id = first; id != end; id++) {
            const char *s = line_text(t.c, id);
            if (strlen(s) != 5 || strncmp(s, "row", 3) != 0) { ordered = 0; break; }
        }
        CHK_INT(ordered, 1);
    }

    t_case("ids are never reused: scrollback_clear() empties without rewinding");
    end = term_core_sb_end(t.c);
    term_core_scrollback_clear(t.c);
    CHK_INT(term_core_sb_first(t.c), end);
    CHK_INT(term_core_sb_end(t.c), end);
    feed(t.c, "\x1b[4;1Hafter\n");
    CHK_INT(term_core_sb_end(t.c), end + 1);
    CHK_TRUE(term_core_sb_first(t.c) >= end);
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_log_mode_scrollback(void)
{
    tcore_t t = tc_make(TERM_LOG, 12, 3);
    uint32_t id;
    int i;

    t_case("TERM_LOG: scrollback is the primary store and joins soft wraps too");
    REQUIRE(t.c != NULL);
    for (i = 0; i < 6; i++) feed(t.c, "hello world\r\n");   /* 11 cols of 12 */
    CHK_TRUE(term_core_sb_end(t.c) > term_core_sb_first(t.c));
    id = term_core_sb_first(t.c);
    CHK_STR(line_text(t.c, id), "hello world");
    CHK_INT(term_core_line_seg_count(t.c, id), 1);

    t_case("TERM_LOG: a line longer than the width is one logical line");
    term_core_scrollback_clear(t.c);
    feed(t.c, "\x1b[H\x1b[2J");
    feed(t.c, "0123456789abcdefghij\r\n");                  /* 20 > 12 */
    feed(t.c, "\x1b[3;1H\n\n\n");
    id = term_core_sb_first(t.c);
    CHK_STR(line_text(t.c, id), "0123456789abcdefghij");
    CHK_INT(term_core_line_length(t.c, id), 20);
    CHK_INT(term_core_line_seg_count(t.c, id), 2);
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_line_utf8_truncation(void)
{
    tcore_t t = tc_make(TERM_VT, 12, 3);
    uint32_t id;
    char buf[64];
    int n;

    t_case("line_utf8() truncates safely and reports the needed length");
    REQUIRE(t.c != NULL);
    feed(t.c, "abcdefgh");
    feed(t.c, "\x1b[3;1H\n\n\n");
    id = term_core_sb_first(t.c);
    memset(buf, 0x7e, sizeof buf);
    n = term_core_line_utf8(t.c, id, buf, 4);
    CHK_INT(n, 8);
    CHK_TRUE(t_strnlen(buf, 4) <= 3);   /* NUL-terminated inside out_size */
    CHK_INT(buf[4], 0x7e);
    memset(buf, 0x7e, sizeof buf);
    CHK_INT(term_core_line_utf8(t.c, id, buf, 0), 8);
    CHK_INT(buf[0], 0x7e);
    tc_free(&t);
}

int main(void)
{
    t_suite("scrollback");
    case_hard_lines();
    case_softwrap_join();
    case_wide_rewrap();
    case_attr_runs();
    case_eviction_and_ids();
    case_log_mode_scrollback();
    case_line_utf8_truncation();
    return t_summary();
}
