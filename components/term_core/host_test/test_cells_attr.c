/*
 * test_cells_attr.c — underline and strike reaching the renderer's seam
 * (§6, phase 4 item 4).
 *
 * §6: "underline — セル flags + blitter に 1 本線描画(小規模)。実装
 * (フェーズ 4): ラン単位で fill_rect 1 本(セル単位ではない)。ランは既に
 * fg/bg で切れているので、そこに属性の対を足しただけ。strike も同じ経路で
 * 出る。位置は実機未確認".
 *
 * The PIXELS are device-only and this suite does not pretend otherwise: what
 * is host-testable is that the flags a parsed SGR sets arrive, with the values
 * term_core.h documents, in the cells the renderer is handed —
 * term_registry_ui_visit's callback ("the callback receives the live core …
 * The callback is where the ui_tab5 glue lives", term_registry.h) and
 * term_core_line_attrs for anything that has scrolled off.
 *
 * The translation into ui_tab5.h's UI_CELL_ATTR_UNDERLINE (1<<0) /
 * UI_CELL_ATTR_STRIKE (1<<1) happens on the device side of that seam, in
 * term_ui_tab5.c, and is therefore asserted on the device and not here. What
 * this suite pins is the input to that translation.
 */
#include "pipe_util.h"

/* ===================================================================== */
/* A visitor that copies one row of cells out of the live core            */
/* ===================================================================== */

#define P4_ROWS_KEPT 8
#define P4_COLS_KEPT 64

typedef struct {
    int         visits;
    term_id_t   id;
    term_view_t view;
    int         cols, rows;
    int         lock_depth_seen;
    term_cell_t cell[P4_ROWS_KEPT][P4_COLS_KEPT];
    uint32_t    row_flags[P4_ROWS_KEPT];
} p4_grid_t;

static p4_grid_t p4_grid;

static void p4_visit(term_id_t id, term_core_t *core, const term_view_t *view,
                     void *user)
{
    p4_grid_t *g = (p4_grid_t *)user;
    int r, c, cols, rows;

    g->visits++;
    g->id = id;
    if (view) g->view = *view;
    if (fp.depth > g->lock_depth_seen) g->lock_depth_seen = fp.depth;
    if (!core) return;

    cols = term_core_cols(core);
    rows = term_core_rows(core);
    g->cols = cols;
    g->rows = rows;
    for (r = 0; r < rows && r < P4_ROWS_KEPT; r++) {
        const term_cell_t *row = term_core_row(core, r);
        g->row_flags[r] = term_core_row_flags(core, r);
        for (c = 0; c < cols && c < P4_COLS_KEPT; c++)
            g->cell[r][c] = row ? row[c] : g->cell[r][c];
    }
    /* §9 / term_registry.h: the renderer, and only the renderer, clears
     * damage. A visitor that never did would leave every term dirty. */
    term_core_dirty_clear(core);
}

/* Run a frame and snapshot the grid the renderer was shown. */
static int p4_paint(void)
{
    memset(&p4_grid, 0, sizeof p4_grid);
    fp_pump();
    (void)term_registry_ui_drain();
    return term_registry_ui_visit(p4_visit, &p4_grid);
}

static uint16_t p4_flags_at(int row, int col)
{
    if (row < 0 || row >= P4_ROWS_KEPT || col < 0 || col >= P4_COLS_KEPT)
        return 0xFFFFu;
    return p4_grid.cell[row][col].flags;
}

static uint32_t p4_cp_at(int row, int col)
{
    if (row < 0 || row >= P4_ROWS_KEPT || col < 0 || col >= P4_COLS_KEPT)
        return 0xFFFFFFFFu;
    return p4_grid.cell[row][col].cp;
}

static void p4_feed(term_id_t id, const char *s)
{
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)s, strlen(s), NULL),
            TERM_OK);
}

static term_id_t p4_visible_term(const char *name)
{
    term_id_t id = reg_new(name, OWNER_A, TERM_VT, false, 40, 6);
    if (id != TERM_ID_INVALID) (void)p4_show(id, OWNER_A, 0, 0, true);
    return id;
}

/* ===================================================================== */
/* The flags themselves                                                  */
/* ===================================================================== */

static void case_the_bits_are_the_documented_ones(void)
{
    t_case("the attribute bits are what term_core.h fixes them to be");
    /* Compile-time contract, restated as a check so a renumbering that
     * silently changes the wire format between the parser and the blitter
     * fails here rather than on a panel nobody is looking at. */
    CHK_HEX(TERM_CELL_UNDERLINE, 1u << 5);
    CHK_HEX(TERM_CELL_STRIKE, 1u << 7);
    CHK_TRUE((TERM_CELL_UNDERLINE & TERM_CELL_STRIKE) == 0);
    CHK_TRUE((TERM_CELL_UNDERLINE & (TERM_CELL_WIDE | TERM_CELL_CONT)) == 0);
    CHK_TRUE((TERM_CELL_STRIKE & (TERM_CELL_WIDE | TERM_CELL_CONT)) == 0);
}

static void case_sgr_4_and_9_reach_the_visitor(void)
{
    term_id_t id;

    t_case("SGR 4 underlines the cells it precedes, and only those");
    CHK_TRUE(reg_boot());
    id = p4_visible_term("v");
    REQUIRE(id != TERM_ID_INVALID);

    p4_feed(id, "no\x1b[4mUL\x1b[24mof");
    CHK_INT(p4_paint(), 1);
    CHK_INT(p4_grid.id, id);

    CHK_INT(p4_cp_at(0, 0), 'n');
    CHK_HEX(p4_flags_at(0, 0) & TERM_CELL_UNDERLINE, 0);
    CHK_HEX(p4_flags_at(0, 1) & TERM_CELL_UNDERLINE, 0);
    CHK_INT(p4_cp_at(0, 2), 'U');
    CHK_HEX(p4_flags_at(0, 2) & TERM_CELL_UNDERLINE, TERM_CELL_UNDERLINE);
    CHK_INT(p4_cp_at(0, 3), 'L');
    CHK_HEX(p4_flags_at(0, 3) & TERM_CELL_UNDERLINE, TERM_CELL_UNDERLINE);
    CHK_INT(p4_cp_at(0, 4), 'o');
    CHK_HEX(p4_flags_at(0, 4) & TERM_CELL_UNDERLINE, 0);
    CHK_HEX(p4_flags_at(0, 5) & TERM_CELL_UNDERLINE, 0);

    t_case("SGR 9 strikes, SGR 29 stops striking, and they do not mix up");
    p4_feed(id, "\r\n" "a\x1b[9mST\x1b[29mb");
    CHK_INT(p4_paint(), 1);
    CHK_HEX(p4_flags_at(1, 0) & TERM_CELL_STRIKE, 0);
    CHK_INT(p4_cp_at(1, 1), 'S');
    CHK_HEX(p4_flags_at(1, 1) & TERM_CELL_STRIKE, TERM_CELL_STRIKE);
    CHK_HEX(p4_flags_at(1, 1) & TERM_CELL_UNDERLINE, 0);
    CHK_HEX(p4_flags_at(1, 2) & TERM_CELL_STRIKE, TERM_CELL_STRIKE);
    CHK_HEX(p4_flags_at(1, 3) & TERM_CELL_STRIKE, 0);

    t_case("both at once set both bits — the run carries the PAIR (§6)");
    p4_feed(id, "\r\n" "\x1b[4;9mXY\x1b[m.");
    CHK_INT(p4_paint(), 1);
    CHK_HEX(p4_flags_at(2, 0) & (TERM_CELL_UNDERLINE | TERM_CELL_STRIKE),
            TERM_CELL_UNDERLINE | TERM_CELL_STRIKE);
    CHK_HEX(p4_flags_at(2, 1) & (TERM_CELL_UNDERLINE | TERM_CELL_STRIKE),
            TERM_CELL_UNDERLINE | TERM_CELL_STRIKE);
    CHK_HEX(p4_flags_at(2, 2) & (TERM_CELL_UNDERLINE | TERM_CELL_STRIKE), 0);

    t_case("SGR 0 clears them; SGR 24/29 clear one without touching the other");
    p4_feed(id, "\r\n" "\x1b[4;9mA\x1b[24mB\x1b[9;4mC\x1b[29mD");
    CHK_INT(p4_paint(), 1);
    CHK_HEX(p4_flags_at(3, 0) & (TERM_CELL_UNDERLINE | TERM_CELL_STRIKE),
            TERM_CELL_UNDERLINE | TERM_CELL_STRIKE);
    CHK_HEX(p4_flags_at(3, 1) & TERM_CELL_UNDERLINE, 0);
    CHK_HEX(p4_flags_at(3, 1) & TERM_CELL_STRIKE, TERM_CELL_STRIKE);
    CHK_HEX(p4_flags_at(3, 2) & (TERM_CELL_UNDERLINE | TERM_CELL_STRIKE),
            TERM_CELL_UNDERLINE | TERM_CELL_STRIKE);
    CHK_HEX(p4_flags_at(3, 3) & TERM_CELL_STRIKE, 0);
    CHK_HEX(p4_flags_at(3, 3) & TERM_CELL_UNDERLINE, TERM_CELL_UNDERLINE);

    t_case("the attributes do not leak into the colours the blitter uses");
    /* fg/bg are final colours (term_core.h); an underline must not have
     * changed them, or a run would break where it should not. */
    CHK_HEX(p4_grid.cell[0][2].fg, p4_grid.cell[0][0].fg);
    CHK_HEX(p4_grid.cell[0][2].bg, p4_grid.cell[0][0].bg);

    t_case("the visitor is handed the view rect it was shown with");
    CHK_INT(p4_grid.view.visible, 1);
    CHK_INT(p4_grid.cols, 40);
    CHK_INT(p4_grid.rows, 6);

    CHK_INT(reg_shutdown(), 0);
}

static void case_a_blank_cell_carries_the_rule(void)
{
    term_id_t id;

    t_case("an underlined SPACE is still underlined — the rule spans the run");
    CHK_TRUE(reg_boot());
    id = p4_visible_term("v");
    REQUIRE(id != TERM_ID_INVALID);

    p4_feed(id, "\x1b[4mA B\x1b[24m");
    CHK_INT(p4_paint(), 1);
    CHK_INT(p4_cp_at(0, 1), 0x20);
    CHK_HEX(p4_flags_at(0, 1) & TERM_CELL_UNDERLINE, TERM_CELL_UNDERLINE);

    t_case("...but a cell nobody wrote is not");
    CHK_INT(p4_cp_at(0, 10), 0x20);
    CHK_HEX(p4_flags_at(0, 10) & TERM_CELL_UNDERLINE, 0);

    t_case("EL erases the attribute with the text (a cleared cell is plain)");
    p4_feed(id, "\r\x1b[K");
    CHK_INT(p4_paint(), 1);
    CHK_HEX(p4_flags_at(0, 0) & TERM_CELL_UNDERLINE, 0);
    CHK_HEX(p4_flags_at(0, 1) & TERM_CELL_UNDERLINE, 0);

    CHK_INT(reg_shutdown(), 0);
}

static void case_a_wide_char_keeps_the_rule_across_both_halves(void)
{
    term_id_t id;

    t_case("an underlined double-width character underlines both its cells");
    /* Otherwise §6's "one fill_rect per run" would stop halfway across the
     * glyph: the run breaks on the attribute pair, so a CONT half without
     * the bit is a rule that covers the left half of a CJK character only. */
    CHK_TRUE(reg_boot());
    id = p4_visible_term("v");
    REQUIRE(id != TERM_ID_INVALID);

    p4_feed(id, "\x1b[4;9m\xe6\x97\xa5\x1b[m");   /* U+65E5, width 2 */
    CHK_INT(p4_paint(), 1);
    CHK_INT(p4_cp_at(0, 0), 0x65E5);
    CHK_HEX(p4_flags_at(0, 0) & TERM_CELL_WIDE, TERM_CELL_WIDE);
    CHK_HEX(p4_flags_at(0, 0) & (TERM_CELL_UNDERLINE | TERM_CELL_STRIKE),
            TERM_CELL_UNDERLINE | TERM_CELL_STRIKE);
    CHK_INT(p4_cp_at(0, 1), TERM_CP_CONT);
    CHK_HEX(p4_flags_at(0, 1) & TERM_CELL_CONT, TERM_CELL_CONT);
    CHK_HEX(p4_flags_at(0, 1) & (TERM_CELL_UNDERLINE | TERM_CELL_STRIKE),
            TERM_CELL_UNDERLINE | TERM_CELL_STRIKE);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The other half of the seam: scrollback attribute runs                 */
/* ===================================================================== */

/* What a history row looks like through the core the visitor was handed. */
#define P4_RUNS_KEPT 16

typedef struct {
    uint32_t        line;                 /* asked for                     */
    int             visits;
    int             nruns;
    term_attr_run_t run[P4_RUNS_KEPT];
    int             seg_count;
    int             seg_cells;
    term_cell_t     seg[P4_COLS_KEPT];
    char            text[256];
} p4_sb_t;

static p4_sb_t p4_sb;

static void p4_sb_visit(term_id_t id, term_core_t *core, const term_view_t *view,
                        void *user)
{
    p4_sb_t *s = (p4_sb_t *)user;
    (void)id; (void)view;
    if (!core) return;
    s->visits++;
    s->nruns = term_core_line_attrs(core, s->line, s->run, P4_RUNS_KEPT);
    s->seg_count = term_core_line_seg_count(core, s->line);
    s->seg_cells = term_core_line_segment(core, s->line, 0, s->seg,
                                          P4_COLS_KEPT);
    (void)term_core_line_utf8(core, s->line, s->text, sizeof s->text);
    term_core_dirty_clear(core);
}

static void case_scrollback_keeps_the_runs(void)
{
    term_id_t id;
    int i, cells_underlined = 0, cells_total = 0;

    t_case("a line that scrolled off keeps its underline in its attr runs");
    CHK_TRUE(reg_boot());
    id = p4_visible_term("v");
    REQUIRE(id != TERM_ID_INVALID);

    p4_feed(id, "ab\x1b[4mUL\x1b[24mcd\r\n");
    for (i = 0; i < 8; i++) {
        char l[16];
        sprintf(l, "f%d\r\n", i);
        p4_feed(id, l);
    }
    p4_paint();
    p4_paint();

    memset(&p4_sb, 0, sizeof p4_sb);
    p4_sb.line = 0;                        /* the first archived line */
    CHK_INT(term_registry_ui_visit(p4_sb_visit, &p4_sb), 1);
    CHK_INT(p4_sb.visits, 1);
    CHK_STR(p4_sb.text, "abULcd");

    t_case("the runs tile the line and carry the attribute pair (§4.1)");
    CHK_TRUE(p4_sb.nruns >= 2);
    CHK_TRUE(p4_sb.nruns <= P4_RUNS_KEPT);
    for (i = 0; i < p4_sb.nruns && i < P4_RUNS_KEPT; i++) {
        cells_total += p4_sb.run[i].cells;
        if (p4_sb.run[i].flags & TERM_CELL_UNDERLINE)
            cells_underlined += p4_sb.run[i].cells;
    }
    CHK_INT(cells_total, 6);
    CHK_INT(cells_underlined, 2);

    t_case("...and re-materialising the line puts the rule on the same cells");
    CHK_INT(p4_sb.seg_count, 1);
    CHK_INT(p4_sb.seg_cells, 6);
    CHK_INT((int)p4_sb.seg[0].cp, 'a');
    CHK_HEX(p4_sb.seg[0].flags & TERM_CELL_UNDERLINE, 0);
    CHK_INT((int)p4_sb.seg[2].cp, 'U');
    CHK_HEX(p4_sb.seg[2].flags & TERM_CELL_UNDERLINE, TERM_CELL_UNDERLINE);
    CHK_INT((int)p4_sb.seg[3].cp, 'L');
    CHK_HEX(p4_sb.seg[3].flags & TERM_CELL_UNDERLINE, TERM_CELL_UNDERLINE);
    CHK_INT((int)p4_sb.seg[4].cp, 'c');
    CHK_HEX(p4_sb.seg[4].flags & TERM_CELL_UNDERLINE, 0);

    t_case("the text-only reads still carry no attributes at all (§7.2)");
    /* snapshot/read are the probe format: "one line per row … no
     * attributes". A probe that leaked SGR would be a class-A data path. */
    CHK_TRUE(strchr(reg_snap(id, OWNER_A), 0x1b) == NULL);
    {
        char out[512];
        term_read_result_t res;
        memset(out, 0, sizeof out);
        memset(&res, 0, sizeof res);
        CHK_INT(term_registry_read(id, OWNER_A, 0, 1, out, sizeof out, &res),
                TERM_OK);
        CHK_STR(out, "abULcd");
    }

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("cells_attr");
    case_the_bits_are_the_documented_ones();
    case_sgr_4_and_9_reach_the_visitor();
    case_a_blank_cell_carries_the_rule();
    case_a_wide_char_keeps_the_rule_across_both_halves();
    case_scrollback_keeps_the_runs();
    fp_reclaim_all();
    return t_summary();
}
