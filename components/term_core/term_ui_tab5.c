/*
 * term_ui_tab5.c — drain on the frame tick, blit the damage.
 *
 * docs/term-design.md §9: "描画は既存 ui.cells の blitter
 * (blit_glyph/compose_glyph_a8、PPA、scroll の memmove)を再利用し、
 * dirty 行だけ blit する". So this file does not draw a single pixel of
 * its own — it turns a term's dirty rows into the same UI_CMD_CELLS runs
 * that ui.cells has been shipping since the JS terminal, and the
 * existing renderer (PPA compose+blend, per-glyph CPU fallback) does the
 * work.
 *
 * Everything here runs on the UI frame task, inside
 * term_registry_ui_visit, i.e. under the registry's table lock. That is
 * why it calls no registry function other than the visit it is already
 * inside (term_registry.h says so explicitly) and touches only the core
 * handle it was handed.
 *
 * Device-only; the whole file is ESP_PLATFORM-guarded so the host test
 * runner compiles it to nothing.
 */

/* Not-empty-translation-unit insurance for the host build. */
typedef int term_ui_tab5_tu_t;

#ifdef ESP_PLATFORM

#include <stdlib.h>
#include <string.h>

#include "term_ui_tab5.h"
#include "term_hist.h"
#include "ui_tab5.h"

/* ------------------------------------------------------------------ */
/* colour                                                              */
/* ------------------------------------------------------------------ */

/*
 * Cells hold RGB565 (§4.1: the canvas' own format, so SGR truecolour is
 * lossless as far as the panel is concerned). ui_cmd_t carries
 * 0xRRGGBB, which the renderer converts back with lv_color_to_u16 —
 * plain truncation. Expanding each channel into the TOP of its byte
 * makes that round trip exact.
 */
static uint32_t rgb565_to_888(uint16_t c)
{
    uint32_t r = (uint32_t)((c >> 11) & 0x1F) << 3;
    uint32_t g = (uint32_t)((c >> 5) & 0x3F) << 2;
    uint32_t b = (uint32_t)(c & 0x1F) << 3;
    return (r << 16) | (g << 8) | b;
}

/* ------------------------------------------------------------------ */
/* one run of same-coloured cells                                      */
/* ------------------------------------------------------------------ */

static size_t utf8_put(char *p, uint32_t cp)
{
    if (cp < 0x80) {
        p[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        p[0] = (char)(0xC0 | (cp >> 6));
        p[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        p[0] = (char)(0xE0 | (cp >> 12));
        p[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        p[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    p[0] = (char)(0xF0 | (cp >> 18));
    p[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    p[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    p[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/*
 * The two attributes the cell contract does not fold into fg/bg (§6):
 * everything else — reverse, bold — is already in the colours by the time
 * a cell exists, so this is the whole of the renderer's attribute logic.
 * A run carries one value, which is why runs break on it below.
 */
static uint16_t run_attrs(const term_cell_t *c)
{
    uint16_t a = 0;
    if (c->flags & TERM_CELL_UNDERLINE)
        a |= UI_CELL_ATTR_UNDERLINE;
    if (c->flags & TERM_CELL_STRIKE)
        a |= UI_CELL_ATTR_STRIKE;
    return a;
}

/*
 * Post cells [from, to) of `row` as one UI_CMD_CELLS run. Returns false
 * when the UI queue refused it, in which case the caller must NOT clear
 * the damage — the pixels never happened.
 *
 * The ui.cells contract (mqjs_runtime.c js_ui_cells): one codepoint per
 * column, and a width-2 codepoint must be followed by a filler for the
 * column it covers. A CONT cell is exactly that filler, so it becomes a
 * space.
 */
static bool post_run(const term_cell_t *cells, int from, int to,
                     int cell_col, int cell_row, uint16_t fg, uint16_t bg,
                     uint16_t attrs)
{
    int n = to - from, i;
    size_t pos = 0;
    char *text;

    if (n <= 0)
        return true;
    text = malloc((size_t)n * 4u + 1u);
    if (!text)
        return false;
    for (i = from; i < to; i++) {
        uint32_t cp = cells[i].cp;
        if ((cells[i].flags & TERM_CELL_CONT) || cp == TERM_CP_CONT)
            cp = 0x20;
        else if (cp < 0x20)
            cp = 0x20; /* a control code has no glyph */
        pos += utf8_put(text + pos, cp);
    }
    text[pos] = '\0';

    ui_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.op = UI_CMD_CELLS;
    cmd.x = (int16_t)(cell_col + from);
    cmd.y = (int16_t)cell_row;
    cmd.w = (int16_t)attrs; /* §6's rules; see ui_tab5.h UI_CELL_ATTR_* */
    cmd.color = rgb565_to_888(fg);
    cmd.bg = rgb565_to_888(bg);
    cmd.text = text;
    if (!ui_tab5_cmd(&cmd)) {
        free(text); /* refused: ownership stayed with us */
        return false;
    }
    return true;
}

/* Split one grid row into runs of one fg/bg/attribute triple and post
   them. Breaking on the attributes as well as the colours is what lets
   the renderer draw one rule per run instead of one per cell. */
static bool blit_row(const term_cell_t *cells, int ncols,
                     int cell_col, int cell_row)
{
    int start = 0, c;
    bool ok = true;
    uint16_t attrs;

    if (ncols <= 0)
        return true;
    attrs = run_attrs(&cells[0]);
    for (c = 1; c <= ncols; c++) {
        uint16_t a = (c < ncols) ? run_attrs(&cells[c]) : attrs;
        bool boundary = (c == ncols) ||
                        cells[c].fg != cells[start].fg ||
                        cells[c].bg != cells[start].bg ||
                        a != attrs;
        if (!boundary)
            continue;
        if (!post_run(cells, start, c, cell_col, cell_row,
                      cells[start].fg, cells[start].bg, attrs))
            ok = false;
        start = c;
        attrs = a;
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* the caret                                                           */
/* ------------------------------------------------------------------ */

/*
 * Painted as a reversed cell, and remembered per slot so the cell it
 * left behind gets its own pixels back. The alternative — asking
 * term_core to mark the caret cell dirty — would put a display concern
 * inside the pure core, and §10.2 already draws the line the other way
 * round (the core reports caret movement, it does not render it).
 */
typedef struct {
    bool on;
    int  col, row;
} caret_t;

static caret_t s_caret[TERM_SLOT_COUNT];

static void repaint_cell(term_core_t *core, int col, int row,
                         int cell_col, int cell_row, bool invert)
{
    const term_cell_t *cells = term_core_row(core, row);
    if (!cells || col < 0 || col >= term_core_cols(core))
        return;
    if (invert)
        post_run(cells, col, col + 1, cell_col, cell_row,
                 cells[col].bg, cells[col].fg, run_attrs(&cells[col]));
    else
        post_run(cells, col, col + 1, cell_col, cell_row,
                 cells[col].fg, cells[col].bg, run_attrs(&cells[col]));
}

/* ------------------------------------------------------------------ */
/* history view (§3.1)                                                 */
/* ------------------------------------------------------------------ */

/* 履歴の外に出た段。地の色で 1 段ぶん塗り直す。 */
static bool hist_blank(int ncols, int cell_col, int cell_row)
{
    static term_cell_t row[TERM_MAX_COLS_DEFAULT + 2];
    int i;

    if (ncols <= 0)
        return true;
    if (ncols > (int)(sizeof row / sizeof row[0]))
        ncols = (int)(sizeof row / sizeof row[0]);
    for (i = 0; i < ncols; i++) {
        memset(&row[i], 0, sizeof row[i]);
        row[i].cp = 0x20;
    }
    return blit_row(row, ncols, cell_col, cell_row);
}

/* 履歴の段を 1 本描く。生きている行と同じ blit_row に落とす。 */
static bool hist_blit(term_core_t *core, uint32_t id, int seg, int ncols,
                      int cell_col, int cell_row)
{
    /* UI フレームタスクしか触らないので static でよい。段の幅は
       max_cols (142) が上限。スタックに置くと 1.7KB を毎段積むことになる。 */
    static term_cell_t row[TERM_MAX_COLS_DEFAULT + 2];
    int n;

    if (ncols > (int)(sizeof row / sizeof row[0]))
        ncols = (int)(sizeof row / sizeof row[0]);
    n = term_core_line_segment(core, id, seg, row, ncols);
    if (n < 0)
        return true;              /* 消えた id: 空段のまま置く */
    /* seg が短くても段の残りは埋めておく —— 前の内容が残る。 */
    for (; n < ncols; n++) {
        memset(&row[n], 0, sizeof row[n]);
        row[n].cp = 0x20;
    }
    return blit_row(row, ncols, cell_col, cell_row);
}

/* ------------------------------------------------------------------ */
/* the visit callback                                                  */
/* ------------------------------------------------------------------ */

static void visit(term_id_t id, term_core_t *core, const term_view_t *view,
                  void *user)
{
    int cw = 0, ch = 0;
    int col0, row0, vis_rows, vis_cols, r;
    bool full, ok = true;
    caret_t *cp = &s_caret[term_id_slot(id)];
    int ccol = 0, crow = 0;
    bool cvis = false;

    (void)user;
    ui_tab5_cell_size(&cw, &ch);
    if (cw <= 0 || ch <= 0)
        return; /* UI down: nothing to draw into */

    col0 = view->x / cw;
    row0 = view->y / ch;
    vis_cols = view->w > 0 ? view->w / cw : term_core_cols(core);
    vis_rows = view->h > 0 ? view->h / ch : term_core_rows(core);
    if (vis_cols > term_core_cols(core))
        vis_cols = term_core_cols(core);
    if (vis_rows > term_core_rows(core))
        vis_rows = term_core_rows(core);
    if (vis_cols <= 0 || vis_rows <= 0)
        return;

    full = term_core_full_repaint(core);

    /* さかのぼって見ている間は damage が意味を持たない —— 変わったのは
       セルではなく「どの段が硝子の上に居るか」なので、毎回全部描く。
       位置が動いた瞬間は term_registry_scroll が repaint_all を予約済み。 */
    int scroll = term_registry_scroll_rows(id);
    if (scroll > 0) {
        int grid_rows = term_core_rows(core);
        /* **描く前に切り詰める。** 無効な位置で 1 フレーム描いてから直すと、
           その「上端が空いたフレーム」が残る —— 端末は入力が無い間フレームを
           回さないので、誰も描き直さない。上限を先に 1 回求めれば、その状態が
           そもそも存在しない (走査も段ごとから 1 回に減る)。 */
        int avail = hist_rows_avail(core, scroll);
        if (scroll > avail) {
            scroll = avail;
            term_registry_scroll_reached(id, avail);
        }
        if (scroll <= 0)
            goto live;               /* 履歴が無い: 生きた画面のまま */
        cp->on = false;              /* 履歴にカーソルは無い */
        for (r = 0; r < vis_rows; r++) {
            uint32_t lid = 0;
            int seg = 0;
            int pos = (vis_rows - 1 - r) + scroll;
            if (!hist_row_at(core, pos, grid_rows, &lid, &seg)) {
                if (!hist_blank(vis_cols, col0, row0 + r))
                    ok = false;      /* ここへは来ないはず (上で切り詰めた) */
                continue;
            }
            if (lid == 0) {
                const term_cell_t *cells = term_core_row(core, seg);
                if (cells && !blit_row(cells, vis_cols, col0, row0 + r))
                    ok = false;
            } else if (!hist_blit(core, lid, seg, vis_cols, col0, row0 + r)) {
                ok = false;
            }
        }
        if (ok)
            term_core_dirty_clear(core);
        return;
    }
live:

    /* Lift the caret before repainting, so a row that carries it is
       drawn from the grid and not from the inverted copy. */
    if (cp->on && cp->row < vis_rows &&
        (full || term_core_row_dirty(core, cp->row)))
        cp->on = false;

    for (r = 0; r < vis_rows; r++) {
        const term_cell_t *cells;
        if (!full && !term_core_row_dirty(core, r))
            continue;
        cells = term_core_row(core, r);
        if (!cells)
            continue;
        if (!blit_row(cells, vis_cols, col0, row0 + r))
            ok = false;
    }

    term_core_cursor(core, &ccol, &crow, &cvis);
    if (cp->on && (cp->col != ccol || cp->row != crow || !cvis)) {
        repaint_cell(core, cp->col, cp->row, col0, row0 + cp->row, false);
        cp->on = false;
    }
    if (cvis && ccol < vis_cols && crow < vis_rows &&
        (!cp->on || cp->col != ccol || cp->row != crow)) {
        repaint_cell(core, ccol, crow, col0, row0 + crow, true);
        cp->on = true;
        cp->col = ccol;
        cp->row = crow;
    }

    /* §9: the renderer, and only the renderer, clears the damage — and
       only when the pixels actually made it into the command queue. A
       run the UI refused must stay dirty, or the screen keeps a hole
       until that cell happens to change again. */
    if (ok)
        term_core_dirty_clear(core);
}

/* ------------------------------------------------------------------ */
/* the frame hook                                                      */
/* ------------------------------------------------------------------ */

static void frame(void *arg)
{
    (void)arg;
    if (term_registry_ui_drain())
        term_registry_ui_visit(visit, NULL);
}

/* ------------------------------------------------------------------ */
/* bring-up                                                            */
/* ------------------------------------------------------------------ */

static bool s_started;

term_err_t term_ui_tab5_start(const term_registry_config_t *cfg)
{
    term_err_t e;

    if (s_started)
        return TERM_OK;
    if (!term_port_installed() && !term_port_install(term_port_freertos()))
        return TERM_ERR_NOT_READY;
    if (!term_registry_ready()) {
        e = term_registry_init(cfg);
        if (e != TERM_OK)
            return e;
    }
    memset(s_caret, 0, sizeof s_caret);
    term_port_freertos_start_reaper();
    ui_tab5_set_frame_cb(frame, NULL);
    s_started = true;
    return TERM_OK;
}

#endif /* ESP_PLATFORM */
