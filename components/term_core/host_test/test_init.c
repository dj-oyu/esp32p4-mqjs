/*
 * test_init.c — the memory contract and the post-init/post-reset state.
 *
 * Verifies (header "MEMORY CONTRACT", term_core_mem_size/init/reset docs,
 * design §4.1 12-byte cell, §4.2 "all allocation happens once at create").
 */
#include "test_util.h"

static void case_struct_sizes(void)
{
    t_case("cell is exactly 12B and an attr run 8B (design §4.1)");
    CHK_INT(sizeof(term_cell_t), 12);
    CHK_INT(sizeof(term_attr_run_t), 8);
}

static void case_mem_size_rejects_bad_config(void)
{
    term_config_t cfg;

    t_case("mem_size() == 0 for an invalid configuration");

    cfg = tc_cfg(TERM_VT, 0, 24);
    CHK_INT(term_core_mem_size(&cfg), 0);         /* cols must be >= 1 */

    cfg = tc_cfg(TERM_VT, 80, 0);
    CHK_INT(term_core_mem_size(&cfg), 0);         /* rows must be >= 1 */

    cfg = tc_cfg(TERM_VT, -3, 24);
    CHK_INT(term_core_mem_size(&cfg), 0);

    cfg = tc_cfg(TERM_VT, TERM_MAX_COLS_DEFAULT + 1, 10);
    CHK_INT(term_core_mem_size(&cfg), 0);         /* cols > max_cols */

    cfg = tc_cfg(TERM_VT, 10, TERM_MAX_ROWS_DEFAULT + 1);
    CHK_INT(term_core_mem_size(&cfg), 0);         /* rows > max_rows */

    /* cols*rows must be <= max_cells at all times (header, term_config_t) */
    cfg = tc_cfg(TERM_VT, 20, 10);
    cfg.max_cols = 40; cfg.max_rows = 40; cfg.max_cells = 100;
    CHK_INT(term_core_mem_size(&cfg), 0);

    /* the header names "a scrollback arena too small for one maximum-width
     * line" as an invalid configuration */
    cfg = tc_cfg(TERM_VT, 80, 24);
    cfg.scrollback_bytes = 8;
    cfg.scrollback_lines = 1;
    CHK_INT(term_core_mem_size(&cfg), 0);
}

static void case_mem_size_pure_and_shaped(void)
{
    term_config_t vt = tc_cfg(TERM_VT, 80, 24);
    term_config_t log = tc_cfg(TERM_LOG, 80, 24);
    term_config_t nosb = tc_cfg(TERM_VT, 80, 24);
    size_t a, b;

    t_case("mem_size() is a pure function of cfg");
    a = term_core_mem_size(&vt);
    b = term_core_mem_size(&vt);
    CHK_TRUE(a > 0);
    CHK_INT(a, b);

    t_case("TERM_LOG allocates no alt screen, so it needs less than TERM_VT (§3.2)");
    CHK_TRUE(term_core_mem_size(&log) > 0);
    CHK_TRUE(term_core_mem_size(&log) < term_core_mem_size(&vt));

    t_case("scrollback_bytes == lines == 0 is legal (scrollback disabled)");
    nosb.scrollback_bytes = 0;
    nosb.scrollback_lines = 0;
    CHK_TRUE(term_core_mem_size(&nosb) > 0);

    t_case("a bigger grid costs more memory (grid is sized for max, §4.1)");
    {
        term_config_t small = tc_cfg(TERM_VT, 40, 10);
        term_config_t big = tc_cfg(TERM_VT, 40, 10);
        big.max_cols = 142; big.max_rows = 53; big.max_cells = 4260;
        small.max_cols = 40; small.max_rows = 10; small.max_cells = 400;
        CHK_TRUE(term_core_mem_size(&small) < term_core_mem_size(&big));
    }
}

static void case_init_rejects(void)
{
    term_config_t cfg = tc_cfg(TERM_VT, 80, 24);
    size_t need = term_core_mem_size(&cfg);
    uint8_t *raw;
    uintptr_t a;
    uint8_t *base;

    t_case("init() refuses a short block and a misaligned block");
    REQUIRE(need > 0);
    raw = (uint8_t *)malloc(need + 64);
    REQUIRE(raw != NULL);
    a = ((uintptr_t)raw + 7u) & ~(uintptr_t)7u;
    base = (uint8_t *)a;

    CHK_TRUE(term_core_init(base, need - 1, &cfg) == NULL);
    CHK_TRUE(term_core_init(base + 1, need, &cfg) == NULL);   /* misaligned */

    t_case("init() refuses an invalid configuration");
    {
        term_config_t bad = tc_cfg(TERM_VT, 0, 24);
        CHK_TRUE(term_core_init(base, need, &bad) == NULL);
    }

    t_case("init() accepts an over-sized block");
    CHK_TRUE(term_core_init(base, need + 8, &cfg) != NULL);

    free(raw);
}

static void case_initial_state(void)
{
    tcore_t t = tc_make(TERM_VT, 80, 24);
    int r, col, top = -1, bot = -1;
    uint32_t modes;
    term_stats_t st;

    t_case("post-init state: geometry, cursor, modes, region (init doc)");
    REQUIRE(t.c != NULL);
    CHK_INT(term_core_cols(t.c), 80);
    CHK_INT(term_core_rows(t.c), 24);
    CHK_INT(cursor_col(t.c), 0);
    CHK_INT(cursor_row(t.c), 0);
    CHK_TRUE(cursor_visible(t.c));

    modes = term_core_modes(t.c);
    CHK_TRUE((modes & TERM_MODE_CURSOR_VISIBLE) != 0);
    CHK_TRUE((modes & TERM_MODE_AUTOWRAP) != 0);
    CHK_INT(modes & (TERM_MODE_BRACKETED | TERM_MODE_ALT_SCREEN |
                     TERM_MODE_APP_CURSOR | TERM_MODE_ORIGIN), 0);
    CHK_TRUE(!term_core_alt_active(t.c));

    term_core_scroll_region(t.c, &top, &bot);
    CHK_INT(top, 0);
    CHK_INT(bot, 23);

    t_case("post-init grid: every cell blank in the default colours");
    {
        int bad_r = -1, bad_c = -1;
        uint32_t softwrap_seen = 0;
        for (r = 0; r < 24 && bad_r < 0; r++) {
            const term_cell_t *row = term_core_row(t.c, r);
            REQUIRE(row != NULL);
            softwrap_seen |= term_core_row_flags(t.c, r) & TERM_ROW_SOFTWRAP;
            for (col = 0; col < 80; col++) {
                if (row[col].cp != 0x20u || row[col].fg != TERM_COLOR_FG_DEFAULT ||
                    row[col].bg != TERM_COLOR_BG_DEFAULT || row[col].flags != 0) {
                    bad_r = r; bad_c = col;
                    CHK_HEX(row[col].cp, 0x20);
                    CHK_HEX(row[col].fg, TERM_COLOR_FG_DEFAULT);
                    CHK_HEX(row[col].bg, TERM_COLOR_BG_DEFAULT);
                    CHK_HEX(row[col].flags, 0);
                    break;
                }
            }
        }
        CHK_INT(bad_r, -1);
        CHK_INT(bad_c, -1);
        CHK_INT(softwrap_seen, 0);
    }

    t_case("post-init readout edges and empty scrollback");
    CHK_TRUE(term_core_row(t.c, -1) == NULL);
    CHK_TRUE(term_core_row(t.c, 24) == NULL);
    CHK_STR(row_text(t.c, 0), "");
    CHK_INT(term_core_sb_first(t.c), term_core_sb_end(t.c));

    t_case("post-init counters are all zero");
    memset(&st, 0xff, sizeof st);
    term_core_stats(t.c, &st);
    CHK_INT(st.bytes_in, 0);
    CHK_INT(st.chars_written, 0);
    CHK_INT(st.lines_archived, 0);
    CHK_INT(st.lines_evicted, 0);
    CHK_INT(st.utf8_errors, 0);
    CHK_INT(st.csi_overflow, 0);
    CHK_INT(st.osc_truncated, 0);
    CHK_INT(st.seq_ignored, 0);
    CHK_INT(st.replies, 0);

    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_reset(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 6);
    uint32_t sb_end_before;
    int top = -1, bot = -1;
    const term_cell_t *cell;

    t_case("term_core_reset(): RIS blanks the screens and restores defaults");
    REQUIRE(t.c != NULL);

    /* dirty the world: text, SGR, region, modes, cursor, scrollback */
    feed(t.c, "\x1b[31;44mline0\r\nline1\r\nline2\r\nline3\r\nline4\r\nline5\r\nline6\r\nline7\r\n");
    feed(t.c, "\x1b[2;5r\x1b[?25l\x1b[?2004h\x1b[?1h\x1b[?7l\x1b[?6h");
    sb_end_before = term_core_sb_end(t.c);
    CHK_TRUE(sb_end_before > term_core_sb_first(t.c)); /* something archived */

    term_core_reset(t.c);

    CHK_INT(cursor_col(t.c), 0);
    CHK_INT(cursor_row(t.c), 0);
    CHK_TRUE(cursor_visible(t.c));
    CHK_INT(term_core_modes(t.c) & (TERM_MODE_BRACKETED | TERM_MODE_ALT_SCREEN |
                                    TERM_MODE_APP_CURSOR | TERM_MODE_ORIGIN), 0);
    CHK_TRUE((term_core_modes(t.c) & TERM_MODE_AUTOWRAP) != 0);
    CHK_TRUE((term_core_modes(t.c) & TERM_MODE_CURSOR_VISIBLE) != 0);
    term_core_scroll_region(t.c, &top, &bot);
    CHK_INT(top, 0);
    CHK_INT(bot, 5);
    CHK_STR(row_text(t.c, 0), "");
    CHK_STR(row_text(t.c, 5), "");
    CHK_TRUE(!term_core_alt_active(t.c));

    t_case("reset() marks a full repaint (header: \"Marks a full repaint\")");
    CHK_TRUE(term_core_full_repaint(t.c));

    t_case("reset() does NOT clear scrollback (header: not a history wipe)");
    CHK_INT(term_core_sb_end(t.c), sb_end_before);
    CHK_TRUE(term_core_sb_end(t.c) > term_core_sb_first(t.c));

    t_case("reset() restores the SGR state");
    feed(t.c, "x");
    cell = cell_at(t.c, 0, 0);
    REQUIRE(cell != NULL);
    CHK_HEX(cell->fg, TERM_COLOR_FG_DEFAULT);
    CHK_HEX(cell->bg, TERM_COLOR_BG_DEFAULT);
    CHK_HEX(cell->flags, 0);

    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_reset_from_alt(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 6);

    t_case("reset() from the alt screen returns to a blank main screen");
    REQUIRE(t.c != NULL);
    feed(t.c, "main\x1b[?1049h" "alt");
    CHK_TRUE(term_core_alt_active(t.c));
    term_core_reset(t.c);
    CHK_TRUE(!term_core_alt_active(t.c));
    CHK_STR(row_text(t.c, 0), "");
    tc_free(&t);
}

static void case_scrollback_disabled(void)
{
    term_config_t cfg = tc_cfg(TERM_VT, 20, 4);
    tcore_t t;
    int i;

    t_case("scrollback disabled: lines pushed off the top are dropped");
    cfg.scrollback_bytes = 0;
    cfg.scrollback_lines = 0;
    t = tc_make_cfg(&cfg);
    REQUIRE(t.c != NULL);
    for (i = 0; i < 20; i++) feed(t.c, "abc\r\n");
    CHK_INT(term_core_sb_first(t.c), term_core_sb_end(t.c));
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

int main(void)
{
    t_suite("init");
    case_struct_sizes();
    case_mem_size_rejects_bad_config();
    case_mem_size_pure_and_shaped();
    case_init_rejects();
    case_initial_state();
    case_reset();
    case_reset_from_alt();
    case_scrollback_disabled();
    return t_summary();
}
