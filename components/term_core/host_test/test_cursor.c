/*
 * test_cursor.c — cursor addressing, its clamps, DECOM, and the caret sink.
 *
 * Design §6 row 2 ("カーソル操作"), §5/B2 (motion counts clamped to the grid),
 * header TERM_MODE_ORIGIN ("DECOM 6, CUP relative to the scroll region") and
 * term_caret_fn ("fires only when (col,row,visible) differs from the last
 * notified value, at most once per feed()/resize() call") — §10.2's ui.caret
 * sink, which is how ime_core learns where to float the preedit.
 */
#include "test_util.h"

typedef struct {
    int calls;
    int col, row;
    bool vis;
} caret_log_t;

static void caret_sink(void *user, int col, int row, bool visible)
{
    caret_log_t *l = (caret_log_t *)user;
    l->calls++;
    l->col = col;
    l->row = row;
    l->vis = visible;
}

static void case_cup(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 6);

    t_case("CUP is 1-based on the wire and 0-based in the API");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[3;5H");
    CHK_INT(cursor_row(t.c), 2);
    CHK_INT(cursor_col(t.c), 4);

    t_case("CUP with omitted or zero parameters means home");
    feed(t.c, "\x1b[H");
    CHK_INT(cursor_row(t.c), 0);
    CHK_INT(cursor_col(t.c), 0);
    feed(t.c, "\x1b[3;5H\x1b[0;0H");
    CHK_INT(cursor_row(t.c), 0);
    CHK_INT(cursor_col(t.c), 0);

    t_case("CUP past the edges clamps into the grid (B2)");
    feed(t.c, "\x1b[99;99H");
    CHK_INT(cursor_row(t.c), 5);
    CHK_INT(cursor_col(t.c), 19);

    t_case("CUP writes land where the cursor was put");
    term_core_reset(t.c);
    feed(t.c, "\x1b[2;3HX");
    CHK_STR(row_text(t.c, 1), "  X");
    tc_free(&t);
}

static void case_relative_motion(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 6);
    uint32_t sb_end;

    t_case("CUU/CUD/CUF/CUB with an explicit count");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[4;10H");
    feed(t.c, "\x1b[2A");
    CHK_INT(cursor_row(t.c), 1);
    feed(t.c, "\x1b[3B");
    CHK_INT(cursor_row(t.c), 4);
    feed(t.c, "\x1b[4C");
    CHK_INT(cursor_col(t.c), 13);
    feed(t.c, "\x1b[5D");
    CHK_INT(cursor_col(t.c), 8);

    t_case("CUU/CUD/CUF/CUB default to 1");
    feed(t.c, "\x1b[3;3H\x1b[A");
    CHK_INT(cursor_row(t.c), 1);
    feed(t.c, "\x1b[B");
    CHK_INT(cursor_row(t.c), 2);
    feed(t.c, "\x1b[C");
    CHK_INT(cursor_col(t.c), 3);
    feed(t.c, "\x1b[D");
    CHK_INT(cursor_col(t.c), 2);

    t_case("relative motion clamps at the edges and never scrolls (B2)");
    sb_end = term_core_sb_end(t.c);
    feed(t.c, "\x1b[2000000000A");
    CHK_INT(cursor_row(t.c), 0);
    feed(t.c, "\x1b[2000000000B");
    CHK_INT(cursor_row(t.c), 5);
    feed(t.c, "\x1b[2000000000C");
    CHK_INT(cursor_col(t.c), 19);
    feed(t.c, "\x1b[2000000000D");
    CHK_INT(cursor_col(t.c), 0);
    CHK_INT(term_core_sb_end(t.c), sb_end);   /* motion is not scrolling */
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_origin_mode(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 8);

    t_case("DECOM (DECSET 6) sets TERM_MODE_ORIGIN");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[3;6r");           /* region rows 2..5 (0-based) */
    feed(t.c, "\x1b[?6h");
    CHK_TRUE((term_core_modes(t.c) & TERM_MODE_ORIGIN) != 0);

    t_case("with DECOM on, CUP 1;1 is the top of the scroll region");
    feed(t.c, "\x1b[1;1H");
    CHK_INT(cursor_row(t.c), 2);
    CHK_INT(cursor_col(t.c), 0);
    feed(t.c, "\x1b[3;1H");
    CHK_INT(cursor_row(t.c), 4);

    t_case("with DECOM on, CUP cannot address outside the region");
    feed(t.c, "\x1b[99;1H");
    CHK_RANGE(cursor_row(t.c), 2, 5);

    t_case("DECRST 6 restores absolute addressing");
    feed(t.c, "\x1b[?6l");
    CHK_INT(term_core_modes(t.c) & TERM_MODE_ORIGIN, 0);
    feed(t.c, "\x1b[1;1H");
    CHK_INT(cursor_row(t.c), 0);
    tc_free(&t);
}

static void case_caret_callback(void)
{
    term_config_t cfg = tc_cfg(TERM_VT, 20, 6);
    caret_log_t log;
    tcore_t t;

    memset(&log, 0, sizeof log);
    cfg.caret_cb = caret_sink;
    cfg.caret_user = &log;
    t = tc_make_cfg(&cfg);

    t_case("caret sink fires once per feed() with the final position");
    REQUIRE(t.c != NULL);
    log.calls = 0;
    feed(t.c, "abc");
    CHK_INT(log.calls, 1);
    CHK_INT(log.col, 3);
    CHK_INT(log.row, 0);
    CHK_TRUE(log.vis);

    log.calls = 0;
    feed(t.c, "\x1b[1;1H\x1b[2;2H\x1b[3;3H");
    CHK_INT(log.calls, 1);
    CHK_INT(log.col, 2);
    CHK_INT(log.row, 2);

    t_case("caret sink stays silent when the caret did not move");
    log.calls = 0;
    feed(t.c, "\x1b[31;44m");
    CHK_INT(log.calls, 0);
    feed(t.c, "");
    CHK_INT(log.calls, 0);
    feed(t.c, "\x1b[3;3H");           /* same place as before */
    CHK_INT(log.calls, 0);

    t_case("caret sink fires when only visibility changes (DECSET 25)");
    log.calls = 0;
    feed(t.c, "\x1b[?25l");
    CHK_INT(log.calls, 1);
    CHK_TRUE(!log.vis);
    log.calls = 0;
    feed(t.c, "\x1b[?25h");
    CHK_INT(log.calls, 1);
    CHK_TRUE(log.vis);

    t_case("caret sink fires at most once per resize()");
    log.calls = 0;
    CHK_INT(term_core_resize(t.c, 10, 4), 0);
    CHK_RANGE(log.calls, 0, 1);

    t_case("term_core_set_caret_cb(NULL) detaches the sink");
    term_core_set_caret_cb(t.c, NULL, NULL);
    log.calls = 0;
    feed(t.c, "\x1b[1;1Hzz");
    CHK_INT(log.calls, 0);

    t_case("term_core_set_caret_cb() re-binds it (late binding)");
    term_core_set_caret_cb(t.c, caret_sink, &log);
    log.calls = 0;
    feed(t.c, "\x1b[2;2H");
    CHK_INT(log.calls, 1);
    CHK_INT(log.row, 1);
    CHK_INT(log.col, 1);

    tc_free(&t);
}

int main(void)
{
    t_suite("cursor");
    case_cup();
    case_relative_motion();
    case_origin_mode();
    case_caret_callback();
    return t_summary();
}
