/*
 * term_core — implementation.
 *
 * Phase 1 of docs/term-design.md §11. Pure logic: no ESP-IDF, no FreeRTOS,
 * no LVGL, no malloc. Every byte the core touches lives inside the block the
 * caller handed to term_core_init().
 *
 * The behavioural reference is examples/ssh_vt.js (device-verified), plus
 * the additions §6 lists as "new": UTF-8 split resilience, alt screen,
 * cursor visibility, terminal replies, underline/reverse/bold.
 *
 * Layout of the caller's block (all regions 8-byte aligned):
 *
 *   [ struct term_core ]
 *   [ main grid   max_cells * 12 ]
 *   [ alt grid    max_cells * 12 ]   (TERM_VT only)
 *   [ main row flags  max_rows * 4 ]
 *   [ alt row flags   max_rows * 4 ] (TERM_VT only)
 *   [ dirty bitmap    ceil(max_rows/32) * 4 ]
 *   [ scrollback index  lines * 8 ]
 *   [ scrollback arena  bytes ]
 *
 * Scrollback record format. An index entry is 8 bytes — {off, bytes, cells}
 * — and addresses a LOGICAL line, which is a chain of one or more CHUNKS
 * laid down contiguously as each grid row is pushed off the top. A chunk is
 *
 *   uint16 cells | uint16 nruns | uint16 text_len | uint16 pad   (8B header)
 *   term_attr_run_t[nruns]                                       (8B each)
 *   UTF-8 text                                                   (text_len)
 *   pad to a multiple of 4
 *
 * so a soft-wrapped line is joined by appending, with no accumulator buffer
 * and no second copy of the text (§3.1: the join is what keeps history from
 * freezing at yesterday's column count). Chunks never straddle the ring's
 * end; when the write cursor must wrap, the open logical line is closed
 * first and the next row starts a new entry, which splits a pathological
 * line rather than losing it.
 */

#include "term_core.h"

#include <string.h>

/* The single source of truth for column width (§6). Header-only and
 * deliberately free of ESP-IDF headers so it compiles on the host too —
 * see the comment at the top of that file. Included by relative path
 * because phase 1 has no component build wiring yet. */
#include "../ui_tab5/include/ui_cell_width.h"

/* §4.1 says 12 bytes exactly; the grid budget is computed from it. */
typedef char term_cell_is_12_bytes[(sizeof(term_cell_t) == 12) ? 1 : -1];
typedef char term_run_is_8_bytes[(sizeof(term_attr_run_t) == 8) ? 1 : -1];

/* ===================================================================== */
/* Small helpers                                                         */
/* ===================================================================== */

#define TERM_SCREEN_MAIN 0
#define TERM_SCREEN_ALT  1

/* Attributes that participate in a scrollback attribute run. WIDE/CONT are
 * geometry, not attributes, so they must not split a run. */
#define TERM_ATTR_MASK \
    (TERM_CELL_BOLD | TERM_CELL_DIM | TERM_CELL_ITALIC | \
     TERM_CELL_UNDERLINE | TERM_CELL_REVERSE | TERM_CELL_STRIKE)

/* A logical line stops growing here so `bytes`/`cells` stay in uint16 and a
 * single runaway line cannot own the whole arena. */
#define SB_LINE_MAX_BYTES 60000u
#define SB_LINE_MAX_CELLS 60000u
#define SB_CHUNK_HDR      8u

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static size_t align8(size_t v) { return (v + 7u) & ~(size_t)7u; }
static size_t align4(size_t v) { return (v + 3u) & ~(size_t)3u; }

/* ===================================================================== */
/* Internal state                                                        */
/* ===================================================================== */

typedef struct {
    uint16_t fg, bg;      /* RGB565 as most recently selected           */
    int16_t  fg_idx;      /* 0..15 when fg came from term_pal16, else -1 */
    int16_t  bg_idx;
    uint16_t flags;       /* TERM_CELL_* attribute bits                  */
} pen_t;

typedef struct {
    int   cx, cy;
    pen_t pen;
    bool  origin;
    bool  valid;
} saved_cursor_t;

typedef struct {
    uint32_t off;    /* byte offset of the first chunk in the arena */
    uint16_t bytes;  /* total bytes of every chunk of this line     */
    uint16_t cells;  /* display cells, CONT halves included         */
} sb_entry_t;

/* Parser states. Named after §6's list: GROUND/ESC/CSI/OSC/CHARSET/
 * CSI_IGNORE/DCS, plus the two "saw ESC inside a string" sub-states. */
enum {
    ST_GROUND = 0,
    ST_ESC,
    ST_CSI,
    ST_CSI_IGNORE,
    ST_OSC,
    ST_STR,        /* DCS/SOS/PM/APC payload — discarded byte by byte (B4) */
    ST_STR_ESC,
    ST_CHARSET     /* consumes exactly one designator byte */
};

struct term_core {
    /* geometry */
    int cols, rows;
    int max_cols, max_rows, max_cells;
    term_mode_t mode;

    /* screens */
    term_cell_t *screen[2];
    uint32_t    *rowflags[2];
    int          active;

    /* cursor / attributes / modes */
    int      cx, cy;          /* cx may equal cols: a pending wrap */
    uint32_t modes;
    int      scroll_top, scroll_bot;
    pen_t    pen;
    saved_cursor_t saved[2];

    uint32_t last_cp;         /* for REP */
    int      last_w;

    /* damage */
    uint32_t *dirty;
    int       dirty_words;
    bool      full_repaint;

    /* caret sink (§10.2) */
    int  caret_col, caret_row;
    bool caret_vis;

    /* parser */
    int      state;
    int      csi_params[TERM_CSI_MAX_PARAMS];
    int      csi_nparams;
    uint32_t csi_cur;
    bool     csi_have_cur;
    bool     csi_priv;
    bool     csi_overflowed;
    char     osc[TERM_OSC_MAX_LEN];
    int      osc_len;
    bool     osc_over;

    /* incremental UTF-8 decoder */
    uint32_t u8_cp;
    int      u8_need;
    uint8_t  u8_lo, u8_hi;

    /* scrollback */
    uint8_t    *sb_arena;
    size_t      sb_size;
    size_t      sb_w;
    sb_entry_t *sb_index;
    int         sb_lines;
    uint32_t    sb_first, sb_end;
    bool        sb_open;        /* a soft-wrapped line is still growing */
    uint32_t    sb_open_id;

    term_reply_fn reply_cb;
    void         *reply_user;
    term_caret_fn caret_cb;
    void         *caret_user;
    /* §4.4's recording tee. NULL unless a human turned recording on for this
     * live session; term_core.h explains why there is no config field. */
    term_record_fn record_cb;
    void          *record_user;

    term_stats_t stats;
};

/* ===================================================================== */
/* Colour                                                                */
/* ===================================================================== */

uint16_t term_color_xterm256(int n)
{
    int r, g, b, i, ri, gi, bi, rem;

    if (n < 0 || n > 255)
        return TERM_COLOR_FG_DEFAULT;
    if (n < 16)
        return term_pal16_at(n);
    if (n < 232) {
        i = n - 16;
        ri = i / 36;
        rem = i - ri * 36;
        gi = rem / 6;
        bi = rem - gi * 6;
        r = ri ? 55 + ri * 40 : 0;
        g = gi ? 55 + gi * 40 : 0;
        b = bi ? 55 + bi * 40 : 0;
    } else {
        r = g = b = 8 + (n - 232) * 10;
    }
    return TERM_RGB565(r, g, b);
}

static void pen_reset(pen_t *p)
{
    p->fg = TERM_COLOR_FG_DEFAULT;
    p->bg = TERM_COLOR_BG_DEFAULT;
    p->fg_idx = -1;
    p->bg_idx = -1;
    p->flags = 0;
}

/* Final colours as stored in a cell: bold has already been folded into the
 * bright twin and reverse has already swapped the pair (§4.1). */
static void pen_resolve(const pen_t *p, uint16_t *fg, uint16_t *bg)
{
    uint16_t f = p->fg, b = p->bg;

    if ((p->flags & TERM_CELL_BOLD) && p->fg_idx >= 0 && p->fg_idx < 8)
        f = term_pal16_at(p->fg_idx | 8);
    if (p->flags & TERM_CELL_REVERSE) {
        uint16_t t = f;
        f = b;
        b = t;
    }
    *fg = f;
    *bg = b;
}

/* Erased cells take the default pair, except under reverse video where the
 * swapped pair is what makes an inverted screen erase to inverted blanks —
 * the rule ssh_vt.js' blankCell() implements. */
static void blank_colors(const pen_t *p, uint16_t *fg, uint16_t *bg)
{
    if (p->flags & TERM_CELL_REVERSE) {
        pen_resolve(p, fg, bg);
    } else {
        *fg = TERM_COLOR_FG_DEFAULT;
        *bg = TERM_COLOR_BG_DEFAULT;
    }
}

/* ===================================================================== */
/* UTF-8                                                                 */
/* ===================================================================== */

static size_t utf8_len(uint32_t cp)
{
    if (cp < 0x80u) return 1;
    if (cp < 0x800u) return 2;
    if (cp < 0x10000u) return 3;
    return 4;
}

static size_t utf8_encode(uint32_t cp, uint8_t *out)
{
    if (cp < 0x80u) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (uint8_t)(0xC0u | (cp >> 6));
        out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (uint8_t)(0xE0u | (cp >> 12));
        out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (uint8_t)(0xF0u | (cp >> 18));
    out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* Decode one codepoint from already-validated storage (the scrollback
 * arena). Returns the bytes consumed; never reads past `len`. */
static size_t utf8_decode(const uint8_t *p, size_t len, uint32_t *cp)
{
    uint8_t b0;
    size_t need, i;
    uint32_t v;

    if (len == 0) {
        *cp = 0;
        return 0;
    }
    b0 = p[0];
    if (b0 < 0x80u) {
        *cp = b0;
        return 1;
    }
    if ((b0 & 0xE0u) == 0xC0u) { need = 1; v = b0 & 0x1Fu; }
    else if ((b0 & 0xF0u) == 0xE0u) { need = 2; v = b0 & 0x0Fu; }
    else if ((b0 & 0xF8u) == 0xF0u) { need = 3; v = b0 & 0x07u; }
    else { *cp = 0xFFFDu; return 1; }

    if (len < need + 1) {
        *cp = 0xFFFDu;
        return len;
    }
    for (i = 1; i <= need; i++) {
        if ((p[i] & 0xC0u) != 0x80u) {
            *cp = 0xFFFDu;
            return i;
        }
        v = (v << 6) | (uint32_t)(p[i] & 0x3Fu);
    }
    *cp = v;
    return need + 1;
}

/* ===================================================================== */
/* Layout                                                                */
/* ===================================================================== */

typedef struct {
    size_t total;
    size_t off_grid[2];
    size_t off_rf[2];
    size_t off_dirty;
    size_t off_index;
    size_t off_arena;
    int    max_cols, max_rows, max_cells;
    int    cols, rows;
    int    sb_lines;
    size_t sb_bytes;
    bool   has_alt;
    int    dirty_words;
} layout_t;

static bool layout_compute(const term_config_t *cfg, layout_t *L)
{
    size_t o;

    if (!cfg)
        return false;
    if (cfg->mode != TERM_VT && cfg->mode != TERM_LOG)
        return false;

    memset(L, 0, sizeof(*L));
    L->max_cols = cfg->max_cols > 0 ? cfg->max_cols : TERM_MAX_COLS_DEFAULT;
    L->max_rows = cfg->max_rows > 0 ? cfg->max_rows : TERM_MAX_ROWS_DEFAULT;
    L->max_cells = cfg->max_cells > 0 ? cfg->max_cells : TERM_MAX_CELLS_DEFAULT;
    if (cfg->max_cols < 0 || cfg->max_rows < 0 || cfg->max_cells < 0)
        return false;
    if (L->max_cols < 1 || L->max_rows < 1 || L->max_cells < 1)
        return false;
    /* Guard the products below against overflow on a 32-bit int. */
    if (L->max_cols > 65535 || L->max_rows > 65535 || L->max_cells > (1 << 24))
        return false;

    L->cols = cfg->cols;
    L->rows = cfg->rows;
    if (L->cols < 1 || L->rows < 1)
        return false;
    if (L->cols > L->max_cols || L->rows > L->max_rows)
        return false;
    if ((long)L->cols * (long)L->rows > (long)L->max_cells)
        return false;

    L->has_alt = (cfg->mode == TERM_VT);

    L->sb_lines = cfg->scrollback_lines;
    L->sb_bytes = cfg->scrollback_bytes;
    if (L->sb_lines < 0)
        return false;
    /* Cap the index like sb_bytes is capped below: an INT_MAX line count
     * would overflow (size_t)lines * 8 on a 32-bit size_t. */
    if (L->sb_lines > (1 << 20))
        return false;
    if (L->sb_lines == 0 || L->sb_bytes == 0) {
        L->sb_lines = 0;
        L->sb_bytes = 0;
    } else {
        /* Room for one worst-case maximum-width line: every cell its own
         * attribute run and a 4-byte codepoint. */
        size_t one = align4(SB_CHUNK_HDR + (size_t)L->max_cols * 8u +
                            (size_t)L->max_cols * 4u);
        if (L->sb_bytes < one)
            return false;
        if (L->sb_bytes > 0x40000000u)
            return false;
    }

    L->dirty_words = (L->max_rows + 31) / 32;

    o = align8(sizeof(struct term_core));
    L->off_grid[0] = o;
    o = align8(o + (size_t)L->max_cells * sizeof(term_cell_t));
    if (L->has_alt) {
        L->off_grid[1] = o;
        o = align8(o + (size_t)L->max_cells * sizeof(term_cell_t));
    }
    L->off_rf[0] = o;
    o = align8(o + (size_t)L->max_rows * sizeof(uint32_t));
    if (L->has_alt) {
        L->off_rf[1] = o;
        o = align8(o + (size_t)L->max_rows * sizeof(uint32_t));
    }
    L->off_dirty = o;
    o = align8(o + (size_t)L->dirty_words * sizeof(uint32_t));
    if (L->sb_lines > 0) {
        L->off_index = o;
        o = align8(o + (size_t)L->sb_lines * sizeof(sb_entry_t));
        L->off_arena = o;
        o = align8(o + L->sb_bytes);
    }
    L->total = o;
    return true;
}

size_t term_core_mem_size(const term_config_t *cfg)
{
    layout_t L;

    if (!layout_compute(cfg, &L))
        return 0;
    return L.total;
}

/* ===================================================================== */
/* Grid primitives                                                       */
/* ===================================================================== */

static term_cell_t *grid(term_core_t *c)
{
    return c->screen[c->active];
}

static uint32_t *rowflags(term_core_t *c)
{
    return c->rowflags[c->active];
}

static term_cell_t *row_at(term_core_t *c, int r)
{
    return grid(c) + (size_t)r * (size_t)c->cols;
}

static void mark_dirty(term_core_t *c, int r)
{
    if (r >= 0 && r < c->rows)
        c->dirty[r >> 5] |= (uint32_t)1u << (r & 31);
}

static void mark_all(term_core_t *c)
{
    int i;
    for (i = 0; i < c->dirty_words; i++)
        c->dirty[i] = 0xFFFFFFFFu;
}

static void cell_blank(term_cell_t *cell, uint16_t fg, uint16_t bg)
{
    cell->cp = 0x20u;
    cell->fg = fg;
    cell->bg = bg;
    cell->flags = 0;
    cell->_pad = 0;
}

/* Blank a span of a screen's cells with the default pair. Used for memory
 * that has no content yet (init, resize growth, scroll-exposed rows). */
static void span_blank_default(term_cell_t *p, int n)
{
    int i;
    for (i = 0; i < n; i++)
        cell_blank(p + i, TERM_COLOR_FG_DEFAULT, TERM_COLOR_BG_DEFAULT);
}

/* Break the wide pair that `col` lands in the middle (or the head) of, AND
 * blank `col` itself — ssh_vt.js detachCell() blanks all three cells it can
 * touch (row.ch[c-1]/row.ch[c+1]/row.ch[c]). The self-blank is load-bearing
 * for callers that go on to SHIFT cells (ICH/DCH) rather than overwrite
 * `col`: without it, a surviving CONT half rides the memmove into a spot
 * whose left neighbour is not WIDE, i.e. an orphan. */
static void detach_at(term_core_t *c, term_cell_t *row, int col)
{
    uint16_t fg, bg;

    if (col < 0 || col >= c->cols)
        return;
    blank_colors(&c->pen, &fg, &bg);
    if ((row[col].flags & TERM_CELL_CONT) && col > 0)
        cell_blank(&row[col - 1], fg, bg);
    if (col + 1 < c->cols && (row[col + 1].flags & TERM_CELL_CONT))
        cell_blank(&row[col + 1], fg, bg);
    cell_blank(&row[col], fg, bg);
}

/* Break a wide pair only when `col` lands on its trailing half — the ICH/DCH
 * shift-boundary rule (ssh_vt.js detachBoundary()). A pair wholly on one side
 * of the boundary shifts intact, so a WIDE lead at `col` is left alone; the
 * full detach_at() here would blank its CONT and orphan the lead mid-shift. */
static void detach_boundary(term_core_t *c, term_cell_t *row, int col)
{
    if (col > 0 && col < c->cols && (row[col].flags & TERM_CELL_CONT))
        detach_at(c, row, col);
}

static void erase_cell(term_core_t *c, int r, int col)
{
    term_cell_t *row = row_at(c, r);
    uint16_t fg, bg;

    if (col < 0 || col >= c->cols)
        return;
    detach_at(c, row, col);
    blank_colors(&c->pen, &fg, &bg);
    cell_blank(&row[col], fg, bg);
    mark_dirty(c, r);
}

static void erase_row_range(term_core_t *c, int r, int from, int to)
{
    int col;
    for (col = from; col <= to; col++)
        erase_cell(c, r, col);
    if (from <= to) {
        /* Only an erase that reaches the right edge can un-fill the row, so
         * only that one may drop the soft-wrap link to the next row. */
        if (to >= c->cols - 1)
            rowflags(c)[r] &= ~(uint32_t)TERM_ROW_SOFTWRAP;
        mark_dirty(c, r);
    }
}

static void put_cell(term_core_t *c, int r, int col, uint32_t cp, int w)
{
    term_cell_t *row = row_at(c, r);
    uint16_t fg, bg;

    pen_resolve(&c->pen, &fg, &bg);
    detach_at(c, row, col);
    if (w == 2 && col + 1 < c->cols)
        detach_at(c, row, col + 1);

    row[col].cp = cp;
    row[col].fg = fg;
    row[col].bg = bg;
    row[col].flags = (uint16_t)((c->pen.flags & TERM_ATTR_MASK) |
                                (w == 2 ? TERM_CELL_WIDE : 0));
    row[col]._pad = 0;
    if (w == 2 && col + 1 < c->cols) {
        row[col + 1].cp = TERM_CP_CONT;
        row[col + 1].fg = fg;
        row[col + 1].bg = bg;
        row[col + 1].flags = (uint16_t)((c->pen.flags & TERM_ATTR_MASK) |
                                        TERM_CELL_CONT);
        row[col + 1]._pad = 0;
    }
    mark_dirty(c, r);
    c->stats.chars_written++;
}

/* ===================================================================== */
/* Scrollback                                                            */
/* ===================================================================== */

static bool sb_enabled(const term_core_t *c)
{
    return c->sb_lines > 0 && c->sb_size > 0;
}

static sb_entry_t *sb_slot(term_core_t *c, uint32_t id)
{
    return &c->sb_index[id % (uint32_t)c->sb_lines];
}

static void sb_evict_oldest(term_core_t *c)
{
    if (c->sb_first == c->sb_end)
        return;
    if (c->sb_open && c->sb_open_id == c->sb_first)
        c->sb_open = false;
    c->sb_first++;
    c->stats.lines_evicted++;
}

/* Contiguous free bytes starting at the write cursor, given the ring's
 * current occupancy. */
static size_t sb_avail_at_w(term_core_t *c)
{
    size_t head;

    if (c->sb_first == c->sb_end)
        return c->sb_size - c->sb_w;
    head = c->sb_index[c->sb_first % (uint32_t)c->sb_lines].off;
    if (head > c->sb_w)
        return head - c->sb_w;
    if (head == c->sb_w)
        return 0;                       /* full */
    return c->sb_size - c->sb_w;
}

/* Reserve `need` contiguous bytes for a NEW record, evicting and wrapping as
 * required. Returns the offset, or (size_t)-1 if `need` exceeds the arena. */
static size_t sb_reserve(term_core_t *c, size_t need)
{
    size_t head;

    if (need > c->sb_size)
        return (size_t)-1;
    for (;;) {
        if (c->sb_first == c->sb_end) {
            c->sb_w = 0;
            return 0;
        }
        if (need <= sb_avail_at_w(c))
            return c->sb_w;
        head = c->sb_index[c->sb_first % (uint32_t)c->sb_lines].off;
        if (head <= c->sb_w && need <= head) {
            /* Waste the tail and restart at the bottom of the arena. */
            c->sb_open = false;
            c->sb_w = 0;
            return 0;
        }
        sb_evict_oldest(c);
    }
}

/*
 * Walk one grid row and produce its scrollback form. With runs_dst/text_dst
 * NULL this only measures; the caller measures, reserves, then writes.
 *
 * A WIDE cell whose codepoint is not intrinsically double-width (an emoji
 * promoted by VS16) is followed by a synthetic U+FE0F so the re-wrap on read
 * reproduces the two cells it occupied on screen.
 */
static void row_serialize(const term_core_t *c, const term_cell_t *row, int n,
                          uint8_t *runs_dst, uint8_t *text_dst,
                          size_t *out_text, int *out_runs, int *out_cells)
{
    term_attr_run_t run;
    bool have_run = false;
    size_t text = 0;
    int nruns = 0;
    int cells = 0;
    int col;

    (void)c;
    memset(&run, 0, sizeof(run));

    for (col = 0; col < n; col++) {
        const term_cell_t *cell = &row[col];
        uint16_t attr = (uint16_t)(cell->flags & TERM_ATTR_MASK);

        if (have_run && run.fg == cell->fg && run.bg == cell->bg &&
            run.flags == attr && run.cells < 0xFFFFu) {
            run.cells++;
        } else {
            if (have_run) {
                if (runs_dst)
                    memcpy(runs_dst + (size_t)nruns * sizeof(run), &run,
                           sizeof(run));
                nruns++;
            }
            run.cells = 1;
            run.fg = cell->fg;
            run.bg = cell->bg;
            run.flags = attr;
            have_run = true;
        }
        cells++;

        if (cell->flags & TERM_CELL_CONT)
            continue;   /* the trailing half carries no text */

        if (text_dst)
            text += utf8_encode(cell->cp, text_dst + text);
        else
            text += utf8_len(cell->cp);

        if ((cell->flags & TERM_CELL_WIDE) && ui_cell_width(cell->cp) != 2) {
            if (text_dst)
                text += utf8_encode(0xFE0Fu, text_dst + text);
            else
                text += utf8_len(0xFE0Fu);
        }
    }
    if (have_run) {
        if (runs_dst)
            memcpy(runs_dst + (size_t)nruns * sizeof(run), &run, sizeof(run));
        nruns++;
    }

    *out_text = text;
    *out_runs = nruns;
    *out_cells = cells;
}

/*
 * Archive one grid row. `softwrap` means the row continues into the next
 * one, so the logical line stays open and the following row is appended to
 * the same entry (§3.1).
 */
static void sb_archive_row(term_core_t *c, const term_cell_t *row,
                           bool softwrap)
{
    size_t text_len, need, off;
    int nruns, cells, n;
    uint8_t hdr[SB_CHUNK_HDR];
    uint16_t v;
    bool extend = false;
    sb_entry_t *e;

    if (!sb_enabled(c)) {
        c->sb_open = false;
        return;
    }

    n = c->cols;
    if (!softwrap) {
        /* Trailing blanks are not content; a soft-wrapped row is full by
         * definition, so only the last row of a logical line is trimmed. */
        while (n > 0 && row[n - 1].cp == 0x20u && row[n - 1].flags == 0)
            n--;
    }

    row_serialize(c, row, n, NULL, NULL, &text_len, &nruns, &cells);
    if (text_len > 0xFFFFu) {
        /* The chunk header stores text_len in a uint16. Unreachable at the
         * real geometry (142 cols * 4B ≈ 568B) but a max_cols near the
         * 65535 cap could overflow it; drop the row rather than write a
         * header that lies about its own length (B6). */
        c->sb_open = false;
        return;
    }
    need = align4(SB_CHUNK_HDR + (size_t)nruns * sizeof(term_attr_run_t) +
                  text_len);
    if (need > c->sb_size) {
        c->sb_open = false;
        return;
    }

    if (c->sb_open && c->sb_open_id >= c->sb_first && c->sb_open_id < c->sb_end) {
        e = sb_slot(c, c->sb_open_id);
        if ((size_t)e->bytes + need <= SB_LINE_MAX_BYTES &&
            (size_t)e->cells + (size_t)cells <= SB_LINE_MAX_CELLS) {
            for (;;) {
                if (need <= sb_avail_at_w(c)) {
                    extend = true;
                    break;
                }
                if (c->sb_first == c->sb_open_id)
                    break;      /* the only way on is to drop the open line */
                sb_evict_oldest(c);
            }
        }
    }
    if (!extend)
        c->sb_open = false;

    if (extend) {
        off = c->sb_w;
    } else {
        off = sb_reserve(c, need);
        if (off == (size_t)-1)
            return;
        if ((uint32_t)(c->sb_end - c->sb_first) >= (uint32_t)c->sb_lines)
            sb_evict_oldest(c);
    }

    /* Chunk header. */
    v = (uint16_t)cells;      memcpy(hdr + 0, &v, 2);
    v = (uint16_t)nruns;      memcpy(hdr + 2, &v, 2);
    v = (uint16_t)text_len;   memcpy(hdr + 4, &v, 2);
    v = 0;                    memcpy(hdr + 6, &v, 2);
    memcpy(c->sb_arena + off, hdr, SB_CHUNK_HDR);
    row_serialize(c, row, n,
                  c->sb_arena + off + SB_CHUNK_HDR,
                  c->sb_arena + off + SB_CHUNK_HDR +
                      (size_t)nruns * sizeof(term_attr_run_t),
                  &text_len, &nruns, &cells);

    if (extend) {
        e = sb_slot(c, c->sb_open_id);
        e->bytes = (uint16_t)(e->bytes + need);
        e->cells = (uint16_t)(e->cells + cells);
    } else {
        uint32_t id = c->sb_end++;
        e = sb_slot(c, id);
        e->off = (uint32_t)off;
        e->bytes = (uint16_t)need;
        e->cells = (uint16_t)cells;
        c->sb_open_id = id;
        c->stats.lines_archived++;
    }
    c->sb_w = off + need;
    c->sb_open = softwrap;

    /*
     * §4.4's recording tee, at the one point where display content exists as
     * text (term_core.h, term_record_fn). Deliberately AFTER the arena write
     * and the index update: the callback is somebody else's code, and if it
     * ever misbehaves the scrollback it was told about is already consistent.
     * The bytes handed over are the ones just written, so there is no second
     * serialisation and no buffer of our own — allocation-free, bounded by
     * `cols`, and exactly zero work when no tee is installed.
     */
    if (c->record_cb && text_len)
        c->record_cb(c->record_user,
                     (const char *)(c->sb_arena + off + SB_CHUNK_HDR +
                                    (size_t)nruns * sizeof(term_attr_run_t)),
                     text_len);
}

/* ---- reading back -------------------------------------------------- */

typedef struct {
    const term_core_t *c;
    size_t   off;        /* start of the current chunk    */
    size_t   remain;     /* bytes of the entry not yet visited */
    size_t   t_off;      /* text cursor inside the arena  */
    size_t   t_left;
    size_t   r_off;      /* run cursor inside the arena   */
    int      r_left;
    term_attr_run_t run; /* current run                   */
    int      run_left;
} line_iter_t;

static bool sb_entry_get(const term_core_t *c, uint32_t id, sb_entry_t *out)
{
    if (!sb_enabled(c))
        return false;
    if ((uint32_t)(id - c->sb_first) >= (uint32_t)(c->sb_end - c->sb_first))
        return false;
    *out = c->sb_index[id % (uint32_t)c->sb_lines];
    return true;
}

static bool iter_next_chunk(line_iter_t *it)
{
    uint16_t nruns, text_len;
    size_t total;

    if (it->remain < SB_CHUNK_HDR)
        return false;
    memcpy(&nruns, it->c->sb_arena + it->off + 2, 2);
    memcpy(&text_len, it->c->sb_arena + it->off + 4, 2);
    it->r_off = it->off + SB_CHUNK_HDR;
    it->r_left = nruns;
    it->t_off = it->r_off + (size_t)nruns * sizeof(term_attr_run_t);
    it->t_left = text_len;
    total = align4(SB_CHUNK_HDR + (size_t)nruns * sizeof(term_attr_run_t) +
                   text_len);
    if (total > it->remain) {
        it->remain = 0;
        return false;
    }
    it->off += total;
    it->remain -= total;
    it->run_left = 0;
    return true;
}

static void iter_init(line_iter_t *it, const term_core_t *c,
                      const sb_entry_t *e)
{
    memset(it, 0, sizeof(*it));
    it->c = c;
    it->off = e->off;
    it->remain = e->bytes;
    it->run_left = 0;
}

static void iter_take_run(line_iter_t *it, int cells, term_attr_run_t *out)
{
    int want = cells;

    while (want > 0) {
        if (it->run_left <= 0) {
            if (it->r_left > 0) {
                memcpy(&it->run, it->c->sb_arena + it->r_off, sizeof(it->run));
                it->r_off += sizeof(it->run);
                it->r_left--;
                it->run_left = it->run.cells;
                if (it->run_left <= 0)
                    continue;
            } else {
                break;
            }
        }
        if (out && want == cells)
            *out = it->run;
        if (it->run_left >= want) {
            it->run_left -= want;
            want = 0;
        } else {
            want -= it->run_left;
            it->run_left = 0;
        }
    }
    if (out && want == cells) {
        /* ran out of runs — fall back to the defaults */
        out->cells = (uint16_t)cells;
        out->fg = TERM_COLOR_FG_DEFAULT;
        out->bg = TERM_COLOR_BG_DEFAULT;
        out->flags = 0;
    }
}

/* One display item: a codepoint and the 1 or 2 cells it occupies. */
static bool iter_next(line_iter_t *it, uint32_t *cp, int *w,
                      term_attr_run_t *attr)
{
    size_t used;
    int width;

    while (it->t_left == 0) {
        if (!iter_next_chunk(it))
            return false;
    }
    used = utf8_decode(it->c->sb_arena + it->t_off, it->t_left, cp);
    if (used == 0)
        return false;
    it->t_off += used;
    it->t_left -= used;

    width = ui_cell_width(*cp);
    if (width < 1)
        width = 1;
    if (width == 1 && it->t_left > 0) {
        uint32_t nxt;
        size_t n2 = utf8_decode(it->c->sb_arena + it->t_off, it->t_left, &nxt);
        if (n2 > 0 && nxt == 0xFE0Fu) {
            it->t_off += n2;
            it->t_left -= n2;
            width = 2;
        }
    }
    iter_take_run(it, width, attr);
    *w = width;
    return true;
}

/* ===================================================================== */
/* Scrolling                                                             */
/* ===================================================================== */

static void row_copy(term_core_t *c, int dst, int src)
{
    term_cell_t *g = grid(c);
    uint32_t *rf = rowflags(c);

    memmove(g + (size_t)dst * (size_t)c->cols,
            g + (size_t)src * (size_t)c->cols,
            (size_t)c->cols * sizeof(term_cell_t));
    rf[dst] = rf[src];
}

static void row_clear_default(term_core_t *c, int r)
{
    span_blank_default(row_at(c, r), c->cols);
    rowflags(c)[r] = 0;
    mark_dirty(c, r);
}

/* Scroll [top,bot] up by n. Rows leaving the top of the MAIN screen when
 * the region starts at row 0 are the ones that become history (§3.1). */
static void scroll_up(term_core_t *c, int top, int bot, int n)
{
    int r;
    bool archive;

    if (top < 0 || bot >= c->rows || top > bot || n <= 0)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;

    archive = (c->active == TERM_SCREEN_MAIN) && (top == 0) && sb_enabled(c);
    if (archive) {
        for (r = 0; r < n; r++)
            sb_archive_row(c, row_at(c, top + r),
                           (rowflags(c)[top + r] & TERM_ROW_SOFTWRAP) != 0);
    } else if (c->active == TERM_SCREEN_MAIN && top == 0) {
        c->sb_open = false;
    }

    for (r = top; r + n <= bot; r++)
        row_copy(c, r, r + n);
    for (r = bot - n + 1; r <= bot; r++)
        row_clear_default(c, r);
    for (r = top; r <= bot; r++)
        mark_dirty(c, r);
}

static void scroll_down(term_core_t *c, int top, int bot, int n)
{
    int r;

    if (top < 0 || bot >= c->rows || top > bot || n <= 0)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;

    for (r = bot; r - n >= top; r--)
        row_copy(c, r, r - n);
    for (r = top; r < top + n; r++)
        row_clear_default(c, r);
    for (r = top; r <= bot; r++)
        mark_dirty(c, r);
}

/* IND / LF: down one line, scrolling the region if we are on its last row. */
static void line_feed(term_core_t *c)
{
    if (c->cy == c->scroll_bot)
        scroll_up(c, c->scroll_top, c->scroll_bot, 1);
    else if (c->cy < c->rows - 1)
        c->cy++;
}

static void reverse_index(term_core_t *c)
{
    if (c->cy == c->scroll_top)
        scroll_down(c, c->scroll_top, c->scroll_bot, 1);
    else if (c->cy > 0)
        c->cy--;
}

/* ===================================================================== */
/* Printing                                                              */
/* ===================================================================== */

static void put_char(term_core_t *c, uint32_t cp, int w)
{
    if (c->cx + w > c->cols) {
        if (c->modes & TERM_MODE_AUTOWRAP) {
            rowflags(c)[c->cy] |= TERM_ROW_SOFTWRAP;
            mark_dirty(c, c->cy);
            c->cx = 0;
            line_feed(c);
        } else {
            c->cx = c->cols - w;
            if (c->cx < 0)
                c->cx = 0;
        }
    }
    put_cell(c, c->cy, c->cx, cp, w);
    c->cx += w;
    c->last_cp = cp;
    c->last_w = w;
}

static void print_codepoint(term_core_t *c, uint32_t cp)
{
    int w = ui_cell_width(cp);

    if (w == 0) {
        /* VS16 promotes the preceding text-presentation character to emoji
         * width; other zero-width codepoints (combining marks, ZWJ) do not
         * advance and are not drawn by this cell model (ssh_vt.js). */
        if (cp == 0xFE0Fu && c->cx > 0 && c->cx < c->cols) {
            term_cell_t *row = row_at(c, c->cy);
            if (!(row[c->cx - 1].flags & TERM_CELL_CONT) &&
                !(row[c->cx - 1].flags & TERM_CELL_WIDE)) {
                /* The cell the new CONT half overwrites may itself be the
                 * lead of a wide pair; break that pair first or its own
                 * CONT at cx+1 is orphaned. */
                detach_at(c, row, c->cx);
                row[c->cx - 1].flags |= TERM_CELL_WIDE;
                row[c->cx].cp = TERM_CP_CONT;
                row[c->cx].fg = row[c->cx - 1].fg;
                row[c->cx].bg = row[c->cx - 1].bg;
                row[c->cx].flags = (uint16_t)((row[c->cx - 1].flags &
                                               TERM_ATTR_MASK) |
                                              TERM_CELL_CONT);
                row[c->cx]._pad = 0;
                c->cx++;
                mark_dirty(c, c->cy);
            }
        }
        return;
    }
    put_char(c, cp, w);
}

/* ===================================================================== */
/* Replies (§6)                                                          */
/* ===================================================================== */

static void reply(term_core_t *c, const char *s, size_t len)
{
    c->stats.replies++;
    if (c->reply_cb)
        c->reply_cb(c->reply_user, s, len);
}

static size_t put_uint(char *buf, size_t pos, unsigned v)
{
    char tmp[12];
    size_t n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v);
    while (n)
        buf[pos++] = tmp[--n];
    return pos;
}

/* ===================================================================== */
/* Modes                                                                 */
/* ===================================================================== */

static void save_cursor(term_core_t *c)
{
    saved_cursor_t *s = &c->saved[c->active];

    s->cx = c->cx;
    s->cy = c->cy;
    s->pen = c->pen;
    s->origin = (c->modes & TERM_MODE_ORIGIN) != 0;
    s->valid = true;
}

static void restore_cursor(term_core_t *c)
{
    saved_cursor_t *s = &c->saved[c->active];

    if (!s->valid) {
        c->cx = 0;
        c->cy = 0;
        return;
    }
    c->cx = clampi(s->cx, 0, c->cols - 1);
    c->cy = clampi(s->cy, 0, c->rows - 1);
    c->pen = s->pen;
    if (s->origin)
        c->modes |= TERM_MODE_ORIGIN;
    else
        c->modes &= ~(uint32_t)TERM_MODE_ORIGIN;
}

static void screen_clear(term_core_t *c, int which)
{
    int r;

    span_blank_default(c->screen[which], c->cols * c->rows);
    for (r = 0; r < c->max_rows; r++)
        c->rowflags[which][r] = 0;
}

static void alt_switch(term_core_t *c, bool on)
{
    if (c->mode != TERM_VT || !c->screen[TERM_SCREEN_ALT]) {
        c->stats.seq_ignored++;
        return;                      /* §3.2: LOG accepts and ignores 1049 */
    }
    if (on) {
        if (c->active == TERM_SCREEN_ALT)
            return;
        save_cursor(c);
        c->active = TERM_SCREEN_ALT;
        c->modes |= TERM_MODE_ALT_SCREEN;
        screen_clear(c, TERM_SCREEN_ALT);
        c->cx = clampi(c->cx, 0, c->cols - 1);
        c->cy = clampi(c->cy, 0, c->rows - 1);
    } else {
        if (c->active == TERM_SCREEN_MAIN)
            return;
        c->active = TERM_SCREEN_MAIN;
        c->modes &= ~(uint32_t)TERM_MODE_ALT_SCREEN;
        restore_cursor(c);
    }
    c->full_repaint = true;
    mark_all(c);
}

static void set_mode(term_core_t *c, int n, bool on, bool priv)
{
    if (!priv) {
        /* SM/RM: nothing in §6's table needs the ANSI modes. */
        c->stats.seq_ignored++;
        return;
    }
    switch (n) {
    case 1:
        if (on) c->modes |= TERM_MODE_APP_CURSOR;
        else    c->modes &= ~(uint32_t)TERM_MODE_APP_CURSOR;
        break;
    case 6:
        if (on) c->modes |= TERM_MODE_ORIGIN;
        else    c->modes &= ~(uint32_t)TERM_MODE_ORIGIN;
        c->cx = 0;
        c->cy = on ? c->scroll_top : 0;
        break;
    case 7:
        if (on) c->modes |= TERM_MODE_AUTOWRAP;
        else    c->modes &= ~(uint32_t)TERM_MODE_AUTOWRAP;
        break;
    case 25:
        if (on) c->modes |= TERM_MODE_CURSOR_VISIBLE;
        else    c->modes &= ~(uint32_t)TERM_MODE_CURSOR_VISIBLE;
        break;
    case 47:
    case 1047:
        alt_switch(c, on);
        break;
    case 1048:
        if (on) save_cursor(c);
        else    restore_cursor(c);
        break;
    case 1049:
        if (on) {
            save_cursor(c);
            alt_switch(c, true);
        } else {
            alt_switch(c, false);
        }
        break;
    case 2004:
        if (on) c->modes |= TERM_MODE_BRACKETED;
        else    c->modes &= ~(uint32_t)TERM_MODE_BRACKETED;
        break;
    default:
        c->stats.seq_ignored++;
        break;
    }
}

/* ===================================================================== */
/* SGR                                                                   */
/* ===================================================================== */

static void sgr_apply(term_core_t *c)
{
    int i;
    int n = c->csi_nparams;

    if (n == 0) {
        pen_reset(&c->pen);
        return;
    }
    for (i = 0; i < n; i++) {
        int v = c->csi_params[i];

        if (v == 0) {
            pen_reset(&c->pen);
        } else if (v == 1) {
            c->pen.flags |= TERM_CELL_BOLD;
        } else if (v == 2) {
            c->pen.flags |= TERM_CELL_DIM;
        } else if (v == 3) {
            c->pen.flags |= TERM_CELL_ITALIC;
        } else if (v == 4) {
            c->pen.flags |= TERM_CELL_UNDERLINE;
        } else if (v == 7) {
            c->pen.flags |= TERM_CELL_REVERSE;
        } else if (v == 9) {
            c->pen.flags |= TERM_CELL_STRIKE;
        } else if (v == 21 || v == 22) {
            c->pen.flags &= (uint16_t)~(TERM_CELL_BOLD | TERM_CELL_DIM);
        } else if (v == 23) {
            c->pen.flags &= (uint16_t)~TERM_CELL_ITALIC;
        } else if (v == 24) {
            c->pen.flags &= (uint16_t)~TERM_CELL_UNDERLINE;
        } else if (v == 27) {
            c->pen.flags &= (uint16_t)~TERM_CELL_REVERSE;
        } else if (v == 29) {
            c->pen.flags &= (uint16_t)~TERM_CELL_STRIKE;
        } else if (v >= 30 && v <= 37) {
            c->pen.fg_idx = (int16_t)(v - 30);
            c->pen.fg = term_pal16_at(v - 30);
        } else if (v == 39) {
            c->pen.fg_idx = -1;
            c->pen.fg = TERM_COLOR_FG_DEFAULT;
        } else if (v >= 40 && v <= 47) {
            c->pen.bg_idx = (int16_t)(v - 40);
            c->pen.bg = term_pal16_at(v - 40);
        } else if (v == 49) {
            c->pen.bg_idx = -1;
            c->pen.bg = TERM_COLOR_BG_DEFAULT;
        } else if (v >= 90 && v <= 97) {
            c->pen.fg_idx = (int16_t)(v - 90 + 8);
            c->pen.fg = term_pal16_at(v - 90 + 8);
        } else if (v >= 100 && v <= 107) {
            c->pen.bg_idx = (int16_t)(v - 100 + 8);
            c->pen.bg = term_pal16_at(v - 100 + 8);
        } else if (v == 38 || v == 48) {
            int sel = (i + 1 < n) ? c->csi_params[i + 1] : -1;
            if (sel == 5 && i + 2 < n) {
                uint16_t col = term_color_xterm256(c->csi_params[i + 2]);
                int idx = c->csi_params[i + 2] < 16 ? c->csi_params[i + 2] : -1;
                if (v == 38) { c->pen.fg = col; c->pen.fg_idx = (int16_t)idx; }
                else         { c->pen.bg = col; c->pen.bg_idx = (int16_t)idx; }
                i += 2;
            } else if (sel == 2 && i + 4 < n) {
                int r = c->csi_params[i + 2];
                int g = c->csi_params[i + 3];
                int b = c->csi_params[i + 4];
                if (r <= 255 && g <= 255 && b <= 255) {
                    uint16_t col = TERM_RGB565(r, g, b);
                    if (v == 38) { c->pen.fg = col; c->pen.fg_idx = -1; }
                    else         { c->pen.bg = col; c->pen.bg_idx = -1; }
                }
                i += 4;
            } else {
                c->stats.seq_ignored++;
            }
        } else {
            c->stats.seq_ignored++;
        }
    }
}

/* ===================================================================== */
/* CSI dispatch                                                          */
/* ===================================================================== */

static int getp(const term_core_t *c, int i, int deflt)
{
    if (i >= c->csi_nparams)
        return deflt;
    return c->csi_params[i] ? c->csi_params[i] : deflt;
}

/* Row limits for cursor addressing: the scroll region under DECOM (§6). */
static int origin_top(const term_core_t *c)
{
    return (c->modes & TERM_MODE_ORIGIN) ? c->scroll_top : 0;
}

static int origin_bot(const term_core_t *c)
{
    return (c->modes & TERM_MODE_ORIGIN) ? c->scroll_bot : c->rows - 1;
}

static void erase_display(term_core_t *c, int mode)
{
    int r;

    if (mode == 1) {
        for (r = 0; r < c->cy; r++)
            erase_row_range(c, r, 0, c->cols - 1);
        erase_row_range(c, c->cy, 0, clampi(c->cx, 0, c->cols - 1));
    } else if (mode == 2 || mode == 3) {
        for (r = 0; r < c->rows; r++)
            erase_row_range(c, r, 0, c->cols - 1);
        if (mode == 3) {
            /* xterm's "erase saved lines" — the only sequence whose whole
             * purpose is wiping history, so unlike RIS it does wipe it. */
            term_core_scrollback_clear(c);
        }
    } else {
        erase_row_range(c, c->cy, clampi(c->cx, 0, c->cols - 1), c->cols - 1);
        for (r = c->cy + 1; r < c->rows; r++)
            erase_row_range(c, r, 0, c->cols - 1);
    }
}

static void erase_line(term_core_t *c, int mode)
{
    int cx = clampi(c->cx, 0, c->cols - 1);

    if (mode == 1)
        erase_row_range(c, c->cy, 0, cx);
    else if (mode == 2)
        erase_row_range(c, c->cy, 0, c->cols - 1);
    else
        erase_row_range(c, c->cy, cx, c->cols - 1);
}

static void insert_chars(term_core_t *c, int n)
{
    term_cell_t *row = row_at(c, c->cy);
    int cx = clampi(c->cx, 0, c->cols - 1);
    uint16_t fg, bg;
    int i;

    n = clampi(n, 1, c->cols - cx);
    detach_boundary(c, row, cx);
    detach_boundary(c, row, c->cols - n);
    memmove(row + cx + n, row + cx,
            (size_t)(c->cols - cx - n) * sizeof(term_cell_t));
    blank_colors(&c->pen, &fg, &bg);
    for (i = cx; i < cx + n; i++)
        cell_blank(&row[i], fg, bg);
    mark_dirty(c, c->cy);
}

static void delete_chars(term_core_t *c, int n)
{
    term_cell_t *row = row_at(c, c->cy);
    int cx = clampi(c->cx, 0, c->cols - 1);
    uint16_t fg, bg;
    int i;

    n = clampi(n, 1, c->cols - cx);
    detach_boundary(c, row, cx);
    detach_boundary(c, row, cx + n);
    memmove(row + cx, row + cx + n,
            (size_t)(c->cols - cx - n) * sizeof(term_cell_t));
    blank_colors(&c->pen, &fg, &bg);
    for (i = c->cols - n; i < c->cols; i++)
        cell_blank(&row[i], fg, bg);
    mark_dirty(c, c->cy);
}

static void tab_forward(term_core_t *c, int n)
{
    int i;

    n = clampi(n, 1, c->cols);
    for (i = 0; i < n; i++)
        c->cx = clampi((c->cx + 8) & ~7, 0, c->cols - 1);
}

static void tab_back(term_core_t *c, int n)
{
    int i;

    n = clampi(n, 1, c->cols);
    for (i = 0; i < n; i++)
        c->cx = clampi(((c->cx - 1) & ~7), 0, c->cols - 1);
}

static void csi_dispatch(term_core_t *c, uint8_t fin)
{
    int n, r, i;
    char buf[24];
    size_t len;

    switch (fin) {
    case 'A':
        c->cy = clampi(c->cy - getp(c, 0, 1), origin_top(c), c->rows - 1);
        c->cx = clampi(c->cx, 0, c->cols - 1);
        break;
    case 'B':
        c->cy = clampi(c->cy + getp(c, 0, 1), 0, origin_bot(c));
        c->cx = clampi(c->cx, 0, c->cols - 1);
        break;
    case 'C':
        c->cx = clampi(clampi(c->cx, 0, c->cols - 1) + getp(c, 0, 1),
                       0, c->cols - 1);
        break;
    case 'D':
        c->cx = clampi(clampi(c->cx, 0, c->cols - 1) - getp(c, 0, 1),
                       0, c->cols - 1);
        break;
    case 'E':
        c->cx = 0;
        c->cy = clampi(c->cy + getp(c, 0, 1), 0, origin_bot(c));
        break;
    case 'F':
        c->cx = 0;
        c->cy = clampi(c->cy - getp(c, 0, 1), origin_top(c), c->rows - 1);
        break;
    case 'G':
    case '`':
        c->cx = clampi(getp(c, 0, 1) - 1, 0, c->cols - 1);
        break;
    case 'd':
        c->cy = clampi(origin_top(c) + getp(c, 0, 1) - 1,
                       origin_top(c), origin_bot(c));
        break;
    case 'a':
        c->cx = clampi(clampi(c->cx, 0, c->cols - 1) + getp(c, 0, 1),
                       0, c->cols - 1);
        break;
    case 'e':
        c->cy = clampi(c->cy + getp(c, 0, 1), 0, origin_bot(c));
        break;
    case 'H':
    case 'f':
        c->cy = clampi(origin_top(c) + getp(c, 0, 1) - 1,
                       origin_top(c), origin_bot(c));
        c->cx = clampi(getp(c, 1, 1) - 1, 0, c->cols - 1);
        break;
    case 'I':
        tab_forward(c, getp(c, 0, 1));
        break;
    case 'Z':
        tab_back(c, getp(c, 0, 1));
        break;
    case 'J':
        erase_display(c, c->csi_nparams ? c->csi_params[0] : 0);
        break;
    case 'K':
        erase_line(c, c->csi_nparams ? c->csi_params[0] : 0);
        break;
    case 'm':
        sgr_apply(c);
        break;
    case 'L':   /* IL */
        if (c->cy >= c->scroll_top && c->cy <= c->scroll_bot) {
            n = clampi(getp(c, 0, 1), 1, c->scroll_bot - c->cy + 1);
            scroll_down(c, c->cy, c->scroll_bot, n);
        }
        break;
    case 'M':   /* DL */
        if (c->cy >= c->scroll_top && c->cy <= c->scroll_bot) {
            n = clampi(getp(c, 0, 1), 1, c->scroll_bot - c->cy + 1);
            scroll_up(c, c->cy, c->scroll_bot, n);
        }
        break;
    case 'S':
        scroll_up(c, c->scroll_top, c->scroll_bot,
                  clampi(getp(c, 0, 1), 1, c->rows));
        break;
    case 'T':
        scroll_down(c, c->scroll_top, c->scroll_bot,
                    clampi(getp(c, 0, 1), 1, c->rows));
        break;
    case 'P':
        delete_chars(c, getp(c, 0, 1));
        break;
    case '@':
        insert_chars(c, getp(c, 0, 1));
        break;
    case 'X': { /* ECH */
        int cx = clampi(c->cx, 0, c->cols - 1);
        n = clampi(getp(c, 0, 1), 1, c->cols - cx);
        erase_row_range(c, c->cy, cx, cx + n - 1);
        break;
    }
    case 'b':   /* REP */
        if (c->last_cp && c->last_w > 0) {
            n = clampi(getp(c, 0, 1), 1, c->cols * c->rows);
            for (i = 0; i < n; i++)
                put_char(c, c->last_cp, c->last_w);
        }
        break;
    case 'r':   /* DECSTBM */
        c->scroll_top = clampi(getp(c, 0, 1) - 1, 0, c->rows - 1);
        c->scroll_bot = clampi(getp(c, 1, c->rows) - 1, 0, c->rows - 1);
        if (c->scroll_bot <= c->scroll_top) {
            c->scroll_top = 0;
            c->scroll_bot = c->rows - 1;
        }
        c->cx = 0;
        c->cy = origin_top(c);
        break;
    case 's':
        save_cursor(c);
        break;
    case 'u':
        restore_cursor(c);
        break;
    case 'h':
    case 'l':
        for (i = 0; i < c->csi_nparams; i++)
            set_mode(c, c->csi_params[i], fin == 'h', c->csi_priv);
        if (c->csi_nparams == 0)
            c->stats.seq_ignored++;
        break;
    case 'n':   /* DSR */
        if (c->csi_priv) {
            c->stats.seq_ignored++;
            break;
        }
        n = getp(c, 0, 0);
        if (n == 5) {
            reply(c, "\x1b[0n", 4);
        } else if (n == 6) {
            len = 0;
            buf[len++] = 0x1b;
            buf[len++] = '[';
            len = put_uint(buf, len, (unsigned)(c->cy - origin_top(c) + 1));
            buf[len++] = ';';
            r = clampi(c->cx, 0, c->cols - 1);
            len = put_uint(buf, len, (unsigned)(r + 1));
            buf[len++] = 'R';
            reply(c, buf, len);
        } else {
            c->stats.seq_ignored++;
        }
        break;
    case 'c':   /* primary DA: VT100 with advanced video (ssh_vt.js) */
        if (c->csi_priv) {
            c->stats.seq_ignored++;
            break;
        }
        reply(c, "\x1b[?1;2c", 7);
        break;
    default:
        c->stats.seq_ignored++;
        break;
    }
}

/* ===================================================================== */
/* State machine                                                         */
/* ===================================================================== */

static void csi_begin(term_core_t *c)
{
    c->state = ST_CSI;
    c->csi_nparams = 0;
    c->csi_cur = 0;
    c->csi_have_cur = false;
    c->csi_priv = false;
    c->csi_overflowed = false;
}

static void csi_push(term_core_t *c)
{
    if (c->csi_nparams < TERM_CSI_MAX_PARAMS) {
        c->csi_params[c->csi_nparams++] =
            c->csi_have_cur ? (int)c->csi_cur : 0;
    } else if (!c->csi_overflowed) {
        c->csi_overflowed = true;
        c->stats.csi_overflow++;
    }
    c->csi_cur = 0;
    c->csi_have_cur = false;
}

static void osc_finish(term_core_t *c)
{
    /* Nothing in phase 1 consumes a window title or a colour query; the
     * payload is parsed, bounded and dropped (B4). */
    c->stats.seq_ignored++;
    c->osc_len = 0;
    c->osc_over = false;
}

static void ground_byte(term_core_t *c, uint8_t b);

static void utf8_flush_error(term_core_t *c)
{
    c->u8_need = 0;
    c->stats.utf8_errors++;
    print_codepoint(c, 0xFFFDu);
}

static void ground_byte(term_core_t *c, uint8_t b)
{
    /* Continuation of a multi-byte sequence, possibly split across feeds. */
    if (c->u8_need > 0) {
        if (b >= c->u8_lo && b <= c->u8_hi) {
            c->u8_cp = (c->u8_cp << 6) | (uint32_t)(b & 0x3Fu);
            c->u8_lo = 0x80u;
            c->u8_hi = 0xBFu;
            if (--c->u8_need == 0)
                print_codepoint(c, c->u8_cp);
            return;
        }
        /* Maximal invalid subpart ends here; this byte is re-examined. */
        utf8_flush_error(c);
    }

    if (b < 0x80u) {
        switch (b) {
        case 0x1b:
            c->state = ST_ESC;
            return;
        case 0x0d:
            c->cx = 0;
            mark_dirty(c, c->cy);
            return;
        case 0x0a:
        case 0x0b:
        case 0x0c:
            line_feed(c);
            return;
        case 0x08:
            /* A backspace on a pending wrap cancels the wrap and stays in
             * the last column rather than stepping a second time. */
            if (c->cx > c->cols - 1)
                c->cx = c->cols - 1;
            else if (c->cx > 0)
                c->cx--;
            return;
        case 0x09:
            tab_forward(c, 1);
            return;
        case 0x0e:
        case 0x0f:
        case 0x07:
            return;                 /* SO/SI/BEL: nothing to do here */
        case 0x18:
        case 0x1a:
            c->state = ST_GROUND;   /* CAN/SUB abort a sequence         */
            return;
        case 0x7f:
            return;                 /* DEL is ignored                    */
        default:
            break;
        }
        if (b < 0x20u)
            return;                 /* remaining C0: no effect           */
        print_codepoint(c, b);
        return;
    }

    /* Lead byte of a multi-byte sequence (WHATWG bounds, so overlong,
     * surrogate and > U+10FFFF forms are rejected at the second byte). */
    c->u8_lo = 0x80u;
    c->u8_hi = 0xBFu;
    if (b >= 0xC2u && b <= 0xDFu) {
        c->u8_need = 1;
        c->u8_cp = b & 0x1Fu;
    } else if (b >= 0xE0u && b <= 0xEFu) {
        c->u8_need = 2;
        c->u8_cp = b & 0x0Fu;
        if (b == 0xE0u) c->u8_lo = 0xA0u;
        if (b == 0xEDu) c->u8_hi = 0x9Fu;
    } else if (b >= 0xF0u && b <= 0xF4u) {
        c->u8_need = 3;
        c->u8_cp = b & 0x07u;
        if (b == 0xF0u) c->u8_lo = 0x90u;
        if (b == 0xF4u) c->u8_hi = 0x8Fu;
    } else {
        c->stats.utf8_errors++;
        print_codepoint(c, 0xFFFDu);
    }
}

static void esc_byte(term_core_t *c, uint8_t b)
{
    if (b == 0x1b) {
        /* ESC anywhere restarts the sequence rather than aborting it: the
         * abandoned prefix is discarded and this ESC introduces the next
         * one. Dropping to GROUND instead would spill the following
         * "[31m" into the screen as text. */
        c->state = ST_ESC;
        return;
    }
    c->state = ST_GROUND;
    switch (b) {
    case '[':
        csi_begin(c);
        return;
    case ']':
        c->state = ST_OSC;
        c->osc_len = 0;
        c->osc_over = false;
        return;
    case 'P':   /* DCS */
    case 'X':   /* SOS */
    case '^':   /* PM  */
    case '_':   /* APC */
        c->state = ST_STR;
        return;
    case '(':
    case ')':
    case '*':
    case '+':
    case '#':
    case '%':
        c->state = ST_CHARSET;
        return;
    case 'M':
        reverse_index(c);
        return;
    case 'D':
        line_feed(c);
        return;
    case 'E':
        c->cx = 0;
        line_feed(c);
        return;
    case '7':
        save_cursor(c);
        return;
    case '8':
        restore_cursor(c);
        return;
    case 'c':
        term_core_reset(c);
        return;
    case '\\':  /* stray ST */
        return;
    case '=':
    case '>':
        return;                 /* keypad mode: input-layer business */
    default:
        c->stats.seq_ignored++;
        return;
    }
}

static void step(term_core_t *c, uint8_t b)
{
    switch (c->state) {
    case ST_GROUND:
        ground_byte(c, b);
        break;

    case ST_ESC:
        esc_byte(c, b);
        break;

    case ST_CHARSET:
        /* Same restart rule as ST_ESC/ST_CSI: an ESC here is the head of a
         * new sequence, not the designator byte. */
        if (b == 0x1b) {
            c->state = ST_ESC;
            break;
        }
        c->state = ST_GROUND;
        c->stats.seq_ignored++;
        break;

    case ST_CSI:
        if (b == '?') {
            c->csi_priv = true;
        } else if (b >= '0' && b <= '9') {
            if (c->csi_nparams < TERM_CSI_MAX_PARAMS) {
                c->csi_cur = c->csi_cur * 10u + (uint32_t)(b - '0');
                if (c->csi_cur > TERM_CSI_PARAM_MAX)
                    c->csi_cur = TERM_CSI_PARAM_MAX;
                c->csi_have_cur = true;
            } else if (!c->csi_overflowed) {
                c->csi_overflowed = true;
                c->stats.csi_overflow++;
            }
        } else if (b == ';') {
            csi_push(c);
        } else if (b == ':' || b == '>' || b == '<' || b == '=' ||
                   (b >= 0x20 && b <= 0x2F)) {
            /* Sub-parameters and intermediates this core does not
             * implement: swallow the whole sequence rather than risk
             * reading it as a different, implemented one (ssh_vt.js). */
            c->state = ST_CSI_IGNORE;
            c->stats.seq_ignored++;
        } else if (b >= 0x40 && b <= 0x7E) {
            if (c->csi_have_cur || c->csi_nparams > 0)
                csi_push(c);
            csi_dispatch(c, b);
            c->state = ST_GROUND;
        } else if (b == 0x1b) {
            c->state = ST_ESC;
        } else if (b < 0x20) {
            /* C0 inside a CSI is dropped, as in ssh_vt.js. */
        } else {
            c->state = ST_CSI_IGNORE;
        }
        break;

    case ST_CSI_IGNORE:
        if (b >= 0x40 && b <= 0x7E)
            c->state = ST_GROUND;
        else if (b == 0x1b)
            c->state = ST_ESC;
        break;

    case ST_OSC:
        if (b == 0x07) {
            osc_finish(c);
            c->state = ST_GROUND;
        } else if (b == 0x1b) {
            /* ESC ends the string and introduces the next sequence
             * (ssh_vt.js: code === 27 -> ST_ESC). ESC \ then terminates via
             * the stray-ST arm of esc_byte; ESC [ starts a CSI — dropping
             * the ESC here instead would spill "[31m" into the grid. */
            osc_finish(c);
            c->state = ST_ESC;
        } else if (b < 0x20 && b != 0x09) {
            /* A bare control aborts the string; re-sync in GROUND. */
            osc_finish(c);
            c->state = ST_GROUND;
        } else if (c->osc_len < TERM_OSC_MAX_LEN) {
            c->osc[c->osc_len++] = (char)b;
        } else if (!c->osc_over) {
            c->osc_over = true;
            c->stats.osc_truncated++;
        }
        break;

    case ST_STR:
        if (b == 0x1b)
            c->state = ST_STR_ESC;
        break;

    case ST_STR_ESC:
        c->state = ST_STR;
        if (b == '\\') {
            c->state = ST_GROUND;
            c->stats.seq_ignored++;
        } else if (b != 0x1b) {
            c->state = ST_STR;
        }
        break;

    default:
        c->state = ST_GROUND;
        break;
    }
}

static void notify_caret(term_core_t *c)
{
    int col = clampi(c->cx, 0, c->cols - 1);
    int row = clampi(c->cy, 0, c->rows - 1);
    bool vis = (c->modes & TERM_MODE_CURSOR_VISIBLE) != 0;

    if (col == c->caret_col && row == c->caret_row && vis == c->caret_vis)
        return;
    c->caret_col = col;
    c->caret_row = row;
    c->caret_vis = vis;
    if (c->caret_cb)
        c->caret_cb(c->caret_user, col, row, vis);
}

void term_core_feed(term_core_t *c, const uint8_t *bytes, size_t len)
{
    size_t i;
    int prev_row;

    if (!c || (!bytes && len > 0))
        return;
    c->stats.bytes_in += (uint64_t)len;
    prev_row = clampi(c->cy, 0, c->rows - 1);
    for (i = 0; i < len; i++)
        step(c, bytes[i]);
    /* The cursor itself is drawn by the renderer, so both the row it left
     * and the row it landed on need repainting. */
    mark_dirty(c, prev_row);
    mark_dirty(c, clampi(c->cy, 0, c->rows - 1));
    notify_caret(c);
}

/* ===================================================================== */
/* Construction                                                          */
/* ===================================================================== */

static void core_soft_reset(term_core_t *c)
{
    c->cx = 0;
    c->cy = 0;
    c->modes = TERM_MODE_CURSOR_VISIBLE | TERM_MODE_AUTOWRAP;
    c->scroll_top = 0;
    c->scroll_bot = c->rows - 1;
    pen_reset(&c->pen);
    memset(c->saved, 0, sizeof(c->saved));
    c->last_cp = 0;
    c->last_w = 0;
    c->state = ST_GROUND;
    c->csi_nparams = 0;
    c->csi_cur = 0;
    c->csi_have_cur = false;
    c->csi_priv = false;
    c->csi_overflowed = false;
    c->osc_len = 0;
    c->osc_over = false;
    c->u8_need = 0;
    c->u8_cp = 0;
    c->u8_lo = 0x80u;
    c->u8_hi = 0xBFu;
    c->active = TERM_SCREEN_MAIN;
}

term_core_t *term_core_init(void *mem, size_t mem_size, const term_config_t *cfg)
{
    layout_t L;
    term_core_t *c;
    uint8_t *base = (uint8_t *)mem;
    int i;

    if (!mem || ((uintptr_t)mem & 7u) != 0)
        return NULL;
    if (!layout_compute(cfg, &L))
        return NULL;
    if (mem_size < L.total)
        return NULL;

    memset(mem, 0, L.total);
    c = (term_core_t *)mem;

    c->mode = cfg->mode;
    c->cols = L.cols;
    c->rows = L.rows;
    c->max_cols = L.max_cols;
    c->max_rows = L.max_rows;
    c->max_cells = L.max_cells;

    c->screen[0] = (term_cell_t *)(base + L.off_grid[0]);
    c->rowflags[0] = (uint32_t *)(base + L.off_rf[0]);
    if (L.has_alt) {
        c->screen[1] = (term_cell_t *)(base + L.off_grid[1]);
        c->rowflags[1] = (uint32_t *)(base + L.off_rf[1]);
    }
    c->dirty = (uint32_t *)(base + L.off_dirty);
    c->dirty_words = L.dirty_words;

    if (L.sb_lines > 0) {
        c->sb_index = (sb_entry_t *)(base + L.off_index);
        c->sb_arena = base + L.off_arena;
        c->sb_size = L.sb_bytes;
        c->sb_lines = L.sb_lines;
    }

    for (i = 0; i < 2; i++) {
        if (!c->screen[i])
            continue;
        span_blank_default(c->screen[i], c->max_cells);
    }

    core_soft_reset(c);
    c->reply_cb = cfg->reply_cb;
    c->reply_user = cfg->reply_user;
    c->caret_cb = cfg->caret_cb;
    c->caret_user = cfg->caret_user;

    c->caret_col = 0;
    c->caret_row = 0;
    c->caret_vis = true;
    c->full_repaint = true;
    mark_all(c);
    return c;
}

void term_core_reset(term_core_t *c)
{
    int i;

    if (!c)
        return;
    core_soft_reset(c);
    for (i = 0; i < 2; i++) {
        if (!c->screen[i])
            continue;
        span_blank_default(c->screen[i], c->max_cells);
        memset(c->rowflags[i], 0, (size_t)c->max_rows * sizeof(uint32_t));
    }
    /* A reset is not a history wipe (§ header contract). */
    c->sb_open = false;
    c->full_repaint = true;
    mark_all(c);
    notify_caret(c);
}

void term_core_set_reply_cb(term_core_t *c, term_reply_fn fn, void *user)
{
    if (!c)
        return;
    c->reply_cb = fn;
    c->reply_user = user;
}

void term_core_set_caret_cb(term_core_t *c, term_caret_fn fn, void *user)
{
    if (!c)
        return;
    c->caret_cb = fn;
    c->caret_user = user;
}

void term_core_set_record_cb(term_core_t *c, term_record_fn fn, void *user)
{
    if (!c)
        return;
    c->record_cb = fn;
    c->record_user = user;
}

bool term_core_recording(const term_core_t *c)
{
    return c && c->record_cb != NULL;
}

/* ===================================================================== */
/* Geometry                                                              */
/* ===================================================================== */

int term_core_cols(const term_core_t *c) { return c ? c->cols : 0; }
int term_core_rows(const term_core_t *c) { return c ? c->rows : 0; }

/* Move `keep` rows up by `drop` at the OLD stride, archiving the rows that
 * fall off the top of the main screen. */
static void resize_drop_top(term_core_t *c, int which, int old_rows,
                            int old_cols, int drop)
{
    term_cell_t *g = c->screen[which];
    uint32_t *rf = c->rowflags[which];
    int r;

    if (drop <= 0)
        return;
    if (which == TERM_SCREEN_MAIN && sb_enabled(c)) {
        for (r = 0; r < drop && r < old_rows; r++)
            sb_archive_row(c, g + (size_t)r * (size_t)old_cols,
                           (rf[r] & TERM_ROW_SOFTWRAP) != 0);
    }
    for (r = 0; r + drop < old_rows; r++) {
        memmove(g + (size_t)r * (size_t)old_cols,
                g + (size_t)(r + drop) * (size_t)old_cols,
                (size_t)old_cols * sizeof(term_cell_t));
        rf[r] = rf[r + drop];
    }
}

static void resize_restride(term_core_t *c, int which, int old_cols,
                            int keep_rows, int new_cols, int new_rows)
{
    term_cell_t *g = c->screen[which];
    int keep_cols = old_cols < new_cols ? old_cols : new_cols;
    int r, col;

    if (new_cols != old_cols) {
        if (new_cols < old_cols) {
            for (r = 0; r < keep_rows; r++)
                memmove(g + (size_t)r * (size_t)new_cols,
                        g + (size_t)r * (size_t)old_cols,
                        (size_t)keep_cols * sizeof(term_cell_t));
        } else {
            for (r = keep_rows - 1; r >= 0; r--)
                memmove(g + (size_t)r * (size_t)new_cols,
                        g + (size_t)r * (size_t)old_cols,
                        (size_t)keep_cols * sizeof(term_cell_t));
        }
    }
    for (r = 0; r < keep_rows; r++) {
        term_cell_t *row = g + (size_t)r * (size_t)new_cols;
        if (new_cols > keep_cols)
            span_blank_default(row + keep_cols, new_cols - keep_cols);
        /* A wide pair may have been cut in half by the new right edge. */
        if (keep_cols > 0 && (row[keep_cols - 1].flags & TERM_CELL_WIDE))
            cell_blank(&row[keep_cols - 1], TERM_COLOR_FG_DEFAULT,
                       TERM_COLOR_BG_DEFAULT);
        for (col = 0; col < new_cols; col++) {
            if ((row[col].flags & TERM_CELL_CONT) &&
                (col == 0 || !(row[col - 1].flags & TERM_CELL_WIDE)))
                cell_blank(&row[col], TERM_COLOR_FG_DEFAULT,
                           TERM_COLOR_BG_DEFAULT);
        }
    }
    for (r = keep_rows; r < new_rows; r++) {
        span_blank_default(g + (size_t)r * (size_t)new_cols, new_cols);
        c->rowflags[which][r] = 0;
    }
}

int term_core_resize(term_core_t *c, int cols, int rows)
{
    int old_cols, old_rows, drop, drop_bottom, drop_top, keep_rows, i;

    if (!c)
        return -1;
    if (cols < 1 || rows < 1 || cols > c->max_cols || rows > c->max_rows)
        return -1;
    if ((long)cols * (long)rows > (long)c->max_cells)
        return -1;
    if (cols == c->cols && rows == c->rows)
        return 0;

    old_cols = c->cols;
    old_rows = c->rows;

    drop = old_rows - rows;
    drop_top = 0;
    drop_bottom = 0;
    if (drop > 0) {
        /* Prefer to shed blank rows below the cursor: shedding from the top
         * would archive a short screen's prompt into history. */
        term_cell_t *g = c->screen[c->active];
        int r = old_rows - 1;
        while (drop_bottom < drop && r > c->cy) {
            term_cell_t *row = g + (size_t)r * (size_t)old_cols;
            int col;
            bool blank = true;
            for (col = 0; col < old_cols; col++) {
                if (row[col].cp != 0x20u || row[col].flags != 0) {
                    blank = false;
                    break;
                }
            }
            if (!blank)
                break;
            drop_bottom++;
            r--;
        }
        drop_top = drop - drop_bottom;
    }
    keep_rows = old_rows - drop_top - drop_bottom;
    if (keep_rows > rows)
        keep_rows = rows;
    if (keep_rows < 0)
        keep_rows = 0;

    for (i = 0; i < 2; i++) {
        if (!c->screen[i])
            continue;
        resize_drop_top(c, i, old_rows, old_cols, drop_top);
    }
    /* Any half-built logical line cannot be continued across a geometry
     * change: close it so the next archived row starts a fresh entry. */
    c->sb_open = false;

    c->cols = cols;
    c->rows = rows;
    for (i = 0; i < 2; i++) {
        if (!c->screen[i])
            continue;
        resize_restride(c, i, old_cols, keep_rows, cols, rows);
    }

    c->cy = clampi(c->cy - drop_top, 0, rows - 1);
    c->cx = clampi(c->cx, 0, cols - 1);
    c->scroll_top = clampi(c->scroll_top, 0, rows - 1);
    c->scroll_bot = clampi(c->scroll_bot, 0, rows - 1);
    if (c->scroll_bot <= c->scroll_top) {
        c->scroll_top = 0;
        c->scroll_bot = rows - 1;
    }
    for (i = 0; i < 2; i++) {
        c->saved[i].cx = clampi(c->saved[i].cx, 0, cols - 1);
        c->saved[i].cy = clampi(c->saved[i].cy, 0, rows - 1);
    }

    c->full_repaint = true;
    mark_all(c);
    notify_caret(c);
    return 0;
}

/* ===================================================================== */
/* Grid readout                                                          */
/* ===================================================================== */

const term_cell_t *term_core_row(const term_core_t *c, int row)
{
    if (!c || row < 0 || row >= c->rows)
        return NULL;
    return c->screen[c->active] + (size_t)row * (size_t)c->cols;
}

uint32_t term_core_row_flags(const term_core_t *c, int row)
{
    if (!c || row < 0 || row >= c->rows)
        return 0;
    return c->rowflags[c->active][row];
}

void term_core_cursor(const term_core_t *c, int *col, int *row, bool *visible)
{
    if (!c)
        return;
    if (col)
        *col = clampi(c->cx, 0, c->cols - 1);
    if (row)
        *row = clampi(c->cy, 0, c->rows - 1);
    if (visible)
        *visible = (c->modes & TERM_MODE_CURSOR_VISIBLE) != 0;
}

uint32_t term_core_modes(const term_core_t *c)
{
    return c ? c->modes : 0;
}

bool term_core_alt_active(const term_core_t *c)
{
    return c ? (c->active == TERM_SCREEN_ALT) : false;
}

void term_core_scroll_region(const term_core_t *c, int *top, int *bot)
{
    if (!c)
        return;
    if (top)
        *top = c->scroll_top;
    if (bot)
        *bot = c->scroll_bot;
}

/* Append one codepoint to a bounded output buffer, counting what a big
 * enough buffer would have needed. */
static size_t emit_cp(char *out, size_t out_size, size_t pos, uint32_t cp)
{
    uint8_t tmp[4];
    size_t n = utf8_encode(cp, tmp);
    size_t i;

    for (i = 0; i < n; i++) {
        if (out && pos + i + 1 < out_size)
            out[pos + i] = (char)tmp[i];
    }
    return pos + n;
}

/* cells 配列を row_utf8 と**同じ規則**で UTF-8 にする (末尾の空白を落とし、
 * CONT セルは飛ばす)。履歴のセグメントを画面と同じ形で読むために要る ——
 * 規則が分かれると、選択した範囲とコピーされた文字列がずれる。 */
int term_core_cells_utf8(const term_cell_t *cells, int n, char *out,
                         size_t out_size)
{
    size_t pos = 0;
    int col;

    if (out && out_size > 0)
        out[0] = '\0';
    if (!cells || n < 0)
        return -1;
    while (n > 0 && cells[n - 1].cp == 0x20u)
        n--;
    for (col = 0; col < n; col++) {
        if (cells[col].flags & TERM_CELL_CONT)
            continue;
        pos = emit_cp(out, out_size, pos, cells[col].cp);
    }
    if (out && out_size > 0)
        out[pos < out_size ? pos : out_size - 1] = '\0';
    return (int)pos;
}

int term_core_row_utf8(const term_core_t *c, int row, char *out, size_t out_size)
{
    const term_cell_t *cells;
    int n, col;
    size_t pos = 0;

    if (out && out_size > 0)
        out[0] = '\0';
    if (!c || row < 0 || row >= c->rows)
        return -1;
    cells = term_core_row(c, row);
    n = c->cols;
    while (n > 0 && cells[n - 1].cp == 0x20u)
        n--;
    for (col = 0; col < n; col++) {
        if (cells[col].flags & TERM_CELL_CONT)
            continue;
        pos = emit_cp(out, out_size, pos, cells[col].cp);
    }
    if (out && out_size > 0)
        out[pos < out_size ? pos : out_size - 1] = '\0';
    return (int)pos;
}

uint32_t term_core_screen_hash(const term_core_t *c)
{
    /* FNV-1a over the display-bearing fields of every active cell. Chosen
       for being one multiply and one xor per word with no table: the caller
       runs this at most once per settle interval, but it walks up to 51 KiB
       of PSRAM and the point is to be cheaper than serialising. */
    uint32_t h = 2166136261u;
    const term_cell_t *cells;
    size_t i, n;

    if (!c)
        return 0;
    /* Geometry first, so a resize that happens to leave the same cells in
       place still reads as a different screen. */
    h = (h ^ (uint32_t)c->cols) * 16777619u;
    h = (h ^ (uint32_t)c->rows) * 16777619u;
    h = (h ^ (uint32_t)c->active) * 16777619u;

    cells = c->screen[c->active];
    if (!cells)
        return h;
    n = (size_t)c->cols * (size_t)c->rows;
    for (i = 0; i < n; i++) {
        /* cp, fg, bg and flags — everything a human can see. `flags` holds
           only attributes and WIDE/CONT geometry (the damage set is a
           separate bitmap), so all of it belongs in the hash. */
        h = (h ^ cells[i].cp) * 16777619u;
        h = (h ^ (uint32_t)cells[i].fg) * 16777619u;
        h = (h ^ (uint32_t)cells[i].bg) * 16777619u;
        h = (h ^ (uint32_t)cells[i].flags) * 16777619u;
    }
    return h;
}

/* ===================================================================== */
/* Dirty rows                                                            */
/* ===================================================================== */

bool term_core_row_dirty(const term_core_t *c, int row)
{
    if (!c || row < 0 || row >= c->rows)
        return false;
    return (c->dirty[row >> 5] & ((uint32_t)1u << (row & 31))) != 0;
}

int term_core_dirty_next(const term_core_t *c, int from_row)
{
    int w, bit, r;

    if (!c)
        return -1;
    if (from_row < 0)
        from_row = 0;
    if (from_row >= c->rows)
        return -1;
    w = from_row >> 5;
    bit = from_row & 31;
    for (; w < c->dirty_words; w++) {
        uint32_t word = c->dirty[w] >> bit;
        if (word) {
            while ((word & 1u) == 0) {
                word >>= 1;
                bit++;
            }
            r = (w << 5) + bit;
            return r < c->rows ? r : -1;
        }
        bit = 0;
    }
    return -1;
}

bool term_core_full_repaint(const term_core_t *c)
{
    return c ? c->full_repaint : false;
}

void term_core_repaint_all(term_core_t *c)
{
    if (c)
        c->full_repaint = true;
}

void term_core_dirty_clear(term_core_t *c)
{
    int i;

    if (!c)
        return;
    for (i = 0; i < c->dirty_words; i++)
        c->dirty[i] = 0;
    c->full_repaint = false;
}

/* ===================================================================== */
/* Scrollback API                                                        */
/* ===================================================================== */

uint32_t term_core_sb_first(const term_core_t *c)
{
    return c ? c->sb_first : 0;
}

uint32_t term_core_sb_end(const term_core_t *c)
{
    return c ? c->sb_end : 0;
}

int term_core_line_length(const term_core_t *c, uint32_t id)
{
    sb_entry_t e;

    if (!c || !sb_entry_get(c, id, &e))
        return -1;
    return (int)e.cells;
}

int term_core_line_utf8(const term_core_t *c, uint32_t id, char *out,
                        size_t out_size)
{
    sb_entry_t e;
    line_iter_t it;
    uint32_t cp;
    int w;
    size_t pos = 0;

    if (out && out_size > 0)
        out[0] = '\0';
    if (!c || !sb_entry_get(c, id, &e))
        return -1;
    iter_init(&it, c, &e);
    while (iter_next(&it, &cp, &w, NULL))
        pos = emit_cp(out, out_size, pos, cp);
    if (out && out_size > 0)
        out[pos < out_size ? pos : out_size - 1] = '\0';
    return (int)pos;
}

int term_core_line_attrs(const term_core_t *c, uint32_t id,
                         term_attr_run_t *out, int max_runs)
{
    sb_entry_t e;
    line_iter_t it;
    term_attr_run_t cur;
    bool have = false;
    int total = 0;

    if (!c || !sb_entry_get(c, id, &e))
        return -1;
    if (max_runs < 0)
        max_runs = 0;
    memset(&cur, 0, sizeof(cur));

    iter_init(&it, c, &e);
    for (;;) {
        term_attr_run_t r;
        if (it.remain == 0 && it.r_left == 0) {
            if (!iter_next_chunk(&it))
                break;
        }
        if (it.r_left == 0) {
            if (!iter_next_chunk(&it))
                break;
            continue;
        }
        memcpy(&r, c->sb_arena + it.r_off, sizeof(r));
        it.r_off += sizeof(r);
        it.r_left--;
        if (have && cur.fg == r.fg && cur.bg == r.bg && cur.flags == r.flags &&
            (uint32_t)cur.cells + r.cells <= 0xFFFFu) {
            cur.cells = (uint16_t)(cur.cells + r.cells);
            continue;
        }
        if (have) {
            if (out && total < max_runs)
                out[total] = cur;
            total++;
        }
        cur = r;
        have = true;
    }
    if (have) {
        if (out && total < max_runs)
            out[total] = cur;
        total++;
    }
    return total;
}

int term_core_line_seg_count(const term_core_t *c, uint32_t id)
{
    sb_entry_t e;
    line_iter_t it;
    uint32_t cp;
    int w, col = 0, segs = 1;

    if (!c || !sb_entry_get(c, id, &e))
        return -1;
    iter_init(&it, c, &e);
    while (iter_next(&it, &cp, &w, NULL)) {
        if (col + w > c->cols) {
            segs++;
            col = 0;
        }
        col += w;
    }
    return segs;
}

int term_core_line_segment(const term_core_t *c, uint32_t id, int seg,
                           term_cell_t *out, int max_cells)
{
    sb_entry_t e;
    line_iter_t it;
    term_attr_run_t attr;
    uint32_t cp;
    int w, col = 0, cur_seg = 0, written = 0;
    int limit;

    if (!c || seg < 0 || !sb_entry_get(c, id, &e))
        return -1;
    limit = c->cols;
    if (max_cells < limit)
        limit = max_cells;
    if (limit < 0)
        limit = 0;
    if (out) {
        int i;
        for (i = 0; i < limit; i++)
            cell_blank(&out[i], TERM_COLOR_FG_DEFAULT, TERM_COLOR_BG_DEFAULT);
    }

    iter_init(&it, c, &e);
    while (iter_next(&it, &cp, &w, &attr)) {
        if (col + w > c->cols) {
            if (cur_seg == seg) {
                /* A double-width character never straddles the edge; the
                 * column it could not fit into stays blank and belongs to
                 * this segment. */
                written = col + 1;
                if (written > limit)
                    written = limit;
                return written;
            }
            cur_seg++;
            col = 0;
        }
        if (cur_seg == seg) {
            if (out && col < limit) {
                out[col].cp = cp;
                out[col].fg = attr.fg;
                out[col].bg = attr.bg;
                out[col].flags = (uint16_t)(attr.flags |
                                            (w == 2 ? TERM_CELL_WIDE : 0));
                out[col]._pad = 0;
                if (w == 2 && col + 1 < limit) {
                    out[col + 1].cp = TERM_CP_CONT;
                    out[col + 1].fg = attr.fg;
                    out[col + 1].bg = attr.bg;
                    out[col + 1].flags = (uint16_t)(attr.flags |
                                                    TERM_CELL_CONT);
                    out[col + 1]._pad = 0;
                }
            }
            written = col + w;
            if (written > limit)
                written = limit;
        } else if (cur_seg > seg) {
            return written;
        }
        col += w;
    }
    if (cur_seg < seg)
        return -1;
    return written;
}

void term_core_scrollback_clear(term_core_t *c)
{
    if (!c)
        return;
    c->sb_first = c->sb_end;
    c->sb_open = false;
    c->sb_w = 0;
}

/* ===================================================================== */
/* Stats                                                                 */
/* ===================================================================== */

void term_core_stats(const term_core_t *c, term_stats_t *out)
{
    if (!out)
        return;
    if (!c) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = c->stats;
}
