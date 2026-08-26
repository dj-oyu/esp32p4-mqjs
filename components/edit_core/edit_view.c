/*
 * edit_view.c — what the presenter draws: the dirty bookkeeping, the scroll
 * follow, and edit_view_row(), which cuts one screen row into same-colour
 * runs.
 *
 * Column arithmetic is in *cells*, never in bytes, and one cell is one
 * codepoint on the way out.  That is the ui.cells CONT contract: a
 * double-width character is emitted as the character followed by a filler
 * space, because the C blitter sizes its clip box from the codepoint but
 * still advances exactly one column per codepoint (docs/skk-ime-design.md
 * §9.4 — the two halves must not both do the widening or text shears one
 * column per CJK character).  A tab is emitted as spaces to the next tab
 * stop for the same reason.
 *
 * Nothing here hands out a pointer into the core: the caller's utf8 buffer
 * receives a copy and the runs index into that copy.
 */

#include "edit_internal.h"

/* ------------------------------------------------------------ dirty rows */

void ev_mark_all(struct edit *e)
{
    e->dirty_flags |= EDIT_DIRTY_ALL | EDIT_DIRTY_STATUS;
    if (e->view_rows >= 64u)
        e->dirty_rows = ~(uint64_t)0;
    else
        e->dirty_rows = (((uint64_t)1 << e->view_rows) - 1u);
}

void ev_mark_line(struct edit *e, uint32_t line)
{
    if (line < e->top_line)
        return;
    {
        uint32_t row = line - e->top_line;
        if (row < e->view_rows)
            e->dirty_rows |= (uint64_t)1 << row;
    }
}

void ev_mark_from(struct edit *e, uint32_t line)
{
    uint32_t row = (line > e->top_line) ? line - e->top_line : 0u;

    if (row >= e->view_rows)
        return;
    e->dirty_rows |= ~(((uint64_t)1 << row) - 1u);
    if (e->view_rows < 64u)
        e->dirty_rows &= (((uint64_t)1 << e->view_rows) - 1u);
}

/* ------------------------------------------------------- cell arithmetic */

uint32_t ev_cells_of(uint32_t cp, uint32_t col)
{
    if (cp == '\t')
        return EDIT_TAB_WIDTH - (col % EDIT_TAB_WIDTH);
    /* Zero-width codepoints still cost a column: the blitter advances one
     * column per codepoint, so pretending otherwise would shear the row. */
    return (ui_cell_width(cp) == 2) ? 2u : 1u;
}

/*
 * The cursor's cell column, cached.  Walking the line is O(line length) and
 * ev_follow_cursor() wants it on every keystroke, so a 100 KB single-line file
 * would otherwise pay 100 KB per character typed.  The cache is advanced in
 * place by eb_insert_raw() on the typing path and simply dropped by anything
 * else; edit_check() re-derives it whenever the flag says it is valid.
 */
uint32_t ev_cursor_col(const struct edit *ec)
{
    struct edit *e = (struct edit *)(uintptr_t)ec;

    if (!e->cur_col_ok) {
        e->cur_col = ev_col_cells(e, e->cur_line, e->cur);
        e->cur_col_ok = true;
    }
    return e->cur_col;
}

uint32_t ev_col_cells(const struct edit *e, uint32_t line, uint32_t upto)
{
    uint32_t p = eb_line_start(e, line);
    uint32_t end = eb_line_end(e, line);
    uint32_t col = 0;

    if (upto > end)
        upto = end;
    while (p < upto) {
        uint32_t cp, n = eb_decode(e, p, &cp);
        if (n == 0)
            break;
        col += ev_cells_of(cp, col);
        p += n;
    }
    return col;
}

uint32_t ev_col_to_byte(const struct edit *e, uint32_t line, uint32_t cells)
{
    uint32_t p = eb_line_start(e, line);
    uint32_t end = eb_line_end(e, line);
    uint32_t col = 0;

    while (p < end && col < cells) {
        uint32_t cp, n = eb_decode(e, p, &cp);
        if (n == 0)
            break;
        col += ev_cells_of(cp, col);
        p += n;
    }
    return p;
}

void ev_follow_cursor(struct edit *e)
{
    uint32_t cells;

    if (e->cur_line < e->top_line) {
        e->top_line = e->cur_line;
        ev_mark_all(e);
    } else if (e->cur_line >= e->top_line + e->view_rows) {
        e->top_line = e->cur_line - e->view_rows + 1u;
        ev_mark_all(e);
    }
    cells = ev_cursor_col(e);
    if (cells < e->left_col) {
        e->left_col = cells;
        ev_mark_all(e);
    } else if (cells >= e->left_col + e->view_cols) {
        e->left_col = cells - e->view_cols + 1u;
        ev_mark_all(e);
    }
}

/* ------------------------------------------------------------ public API */

void edit_set_view(edit_t *e, uint16_t cols, uint16_t rows)
{
    if (e == NULL)
        return;
    if (cols == 0)
        cols = 1;
    if (rows == 0)
        rows = 1;
    if (cols > e->cfg.max_cols)
        cols = e->cfg.max_cols;
    if (rows > e->cfg.max_rows)
        rows = e->cfg.max_rows;
    e->view_cols = cols;
    e->view_rows = rows;
    ev_follow_cursor(e);
    ev_mark_all(e);
}

edit_err_t edit_scroll(edit_t *e, int32_t lines)
{
    int64_t t;

    if (e == NULL)
        return EDIT_E_ARG;
    t = (int64_t)e->top_line + (int64_t)lines;
    if (t < 0)
        t = 0;
    if (t > (int64_t)e->line_count - 1)
        t = (int64_t)e->line_count - 1;
    if ((uint32_t)t != e->top_line) {
        e->top_line = (uint32_t)t;
        ev_mark_all(e);
    }
    return EDIT_OK;
}

uint32_t edit_dirty_flags(const edit_t *e)
{
    return (e == NULL) ? 0u : e->dirty_flags;
}

uint64_t edit_dirty_rows(const edit_t *e)
{
    return (e == NULL) ? 0u : e->dirty_rows;
}

void edit_dirty_clear(edit_t *e)
{
    if (e == NULL)
        return;
    e->dirty_flags = 0;
    e->dirty_rows = 0;
}

bool edit_cursor_view(const edit_t *e, uint16_t *row, uint16_t *col)
{
    uint32_t cells;

    if (e == NULL || row == NULL || col == NULL)
        return false;
    if (e->cur_line < e->top_line || e->cur_line >= e->top_line + e->view_rows)
        return false;
    cells = ev_cursor_col(e);
    if (cells < e->left_col || cells >= e->left_col + e->view_cols)
        return false;
    *row = (uint16_t)(e->cur_line - e->top_line);
    *col = (uint16_t)(cells - e->left_col);
    return true;
}

/* -------------------------------------------------------------- row cuts */

typedef struct {
    char       *utf8;
    size_t      cap;
    size_t      len;
    edit_run_t *runs;
    int         runs_cap;
    int         n;
    uint32_t    col;        /* absolute cell column inside the line */
    uint32_t    left, right;
    int         err;        /* 0, or the negative return of edit_view_row */
} emit_t;

static uint32_t utf8_enc(uint32_t cp, char *out)
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static bool push_cell(emit_t *m, uint32_t screen_col, uint32_t cp,
                      uint8_t cls, uint8_t flags)
{
    char tmp[4];
    uint32_t n = utf8_enc(cp, tmp);
    edit_run_t *r;

    if (m->len + n > m->cap) {
        m->err = -3;
        return false;
    }
    if (m->n > 0) {
        r = &m->runs[m->n - 1];
        if (r->cls == cls && r->flags == flags &&
            (uint32_t)r->col + (uint32_t)r->ncells == screen_col) {
            memcpy(m->utf8 + m->len, tmp, n);
            m->len += n;
            r->ncells = (uint16_t)(r->ncells + 1u);
            r->utf8_len = (uint16_t)(r->utf8_len + n);
            return true;
        }
    }
    if (m->n >= m->runs_cap) {
        m->err = -2;
        return false;
    }
    r = &m->runs[m->n++];
    r->col      = (uint16_t)screen_col;
    r->ncells   = 1;
    r->cls      = cls;
    r->flags    = flags;
    r->utf8_off = (uint16_t)m->len;
    r->utf8_len = (uint16_t)n;
    memcpy(m->utf8 + m->len, tmp, n);
    m->len += n;
    return true;
}

/* One codepoint occupying `w` cells.  Returns false once the row is full. */
static bool put_group(emit_t *m, uint32_t cp, uint32_t w, uint8_t cls, uint8_t flags)
{
    uint32_t col = m->col;

    if (m->err != 0)
        return false;
    /* A tab is not a glyph: it is drawn as the spaces it stands for, which is
     * also what keeps one codepoint per cell on the way out. */
    if (cp == '\t')
        cp = (uint32_t)' ';
    if (col >= m->right) {
        m->col += w;
        return false;
    }
    if (col + w <= m->left) {
        m->col += w;
        return true;
    }
    for (uint32_t k = 0; k < w; k++) {
        uint32_t c = col + k;
        uint32_t ch;
        if (c < m->left || c >= m->right)
            continue;
        /* The head cell carries the character; the rest are the filler
         * spaces the CONT contract asks for.  A group clipped by the left
         * edge loses its head, so all of its visible cells are fillers. */
        ch = (k == 0 && col >= m->left) ? cp : (uint32_t)' ';
        if (!push_cell(m, c - m->left, ch, cls, flags))
            return false;
    }
    m->col += w;
    return m->col < m->right;
}

static bool put_char(emit_t *m, uint32_t cp, uint8_t cls, uint8_t flags)
{
    return put_group(m, cp, ev_cells_of(cp, m->col), cls, flags);
}

/* The composing string is drawn over the cursor and is not in the text. */
static bool put_preedit(emit_t *m, const struct edit *e, bool *cursor_used)
{
    uint32_t i = 0;
    bool first = true;

    while (i < e->preedit_len) {
        uint8_t c = e->preedit[i];
        uint32_t cp = c, n = 1;
        if ((c & 0xE0u) == 0xC0u) n = 2;
        else if ((c & 0xF0u) == 0xE0u) n = 3;
        else if ((c & 0xF8u) == 0xF0u) n = 4;
        if (i + n > e->preedit_len)
            n = 1;
        if (n == 1) {
            cp = c;
        } else {
            cp = c & (uint32_t)(0xFFu >> (n + 1u));
            for (uint32_t k = 1; k < n; k++)
                cp = (cp << 6) | (uint32_t)(e->preedit[i + k] & 0x3Fu);
        }
        if (!put_char(m, cp, (uint8_t)EDIT_CLS_PREEDIT,
                      first ? (uint8_t)EDIT_RUN_CURSOR : (uint8_t)0))
            return false;
        first = false;
        i += n;
    }
    if (e->preedit_len > 0)
        *cursor_used = true;
    return true;
}

int edit_view_row(const edit_t *e, uint16_t row, edit_run_t *runs, int runs_cap,
                  char *utf8, size_t utf8_cap)
{
    /* The lexer cache is the one mutable thing behind this const: resolving a
     * deferred cascade is what "遅延で解く" in spec §A.1 means, and the row
     * being drawn is the only place that knows how far down to resolve. */
    struct edit *m = (struct edit *)(uintptr_t)e;
    emit_t em;
    lex_iter_t it;
    uint32_t line, ls, le, p, tf, tt, sel_from = 0, sel_to = 0, cursor_byte, mark_cell;
    uint8_t cls;
    bool cursor_used = false, has_sel;

    if (e == NULL || runs == NULL || runs_cap <= 0 || utf8 == NULL)
        return -1;
    if (row >= e->view_rows)
        return -1;

    line = e->top_line + row;
    if (line >= e->line_count)
        return 0;

    lex_ensure(m, e->top_line + e->view_rows - 1u);

    has_sel = edit_selection(e, NULL, NULL);
    if (has_sel) {
        size_t a, b;
        (void)edit_selection(e, &a, &b);
        sel_from = (uint32_t)a;
        sel_to   = (uint32_t)b;
    }
    cursor_byte = (e->cur_line == line) ? e->cur : 0xFFFFFFFFu;
    mark_cell   = (e->mark_line1 == line + 1u && e->mark_col1 > 0)
                      ? e->mark_col1 - 1u : 0xFFFFFFFFu;

    em.utf8 = utf8;
    em.cap = utf8_cap;
    em.len = 0;
    em.runs = runs;
    em.runs_cap = runs_cap;
    em.n = 0;
    em.col = 0;
    em.left = e->left_col;
    em.right = e->left_col + e->view_cols;
    em.err = 0;

    ls = eb_line_start(e, line);
    le = eb_line_end(e, line);
    lex_begin(&it, e, ls, le, (uint8_t)(e->line_state[line] & EDIT_LEX_MASK));

    while (em.err == 0 && lex_next(&it, &tf, &tt, &cls)) {
        for (p = tf; p < tt; ) {
            uint32_t cp, n, w;
            uint8_t flags = 0;

            if (p == cursor_byte && !put_preedit(&em, e, &cursor_used))
                break;
            n = eb_decode(e, p, &cp);
            if (n == 0)
                break;
            w = ev_cells_of(cp, em.col);
            if (has_sel && p >= sel_from && p < sel_to)
                flags |= (uint8_t)EDIT_RUN_SELECTED;
            if (p == cursor_byte && !cursor_used) {
                flags |= (uint8_t)EDIT_RUN_CURSOR;
                cursor_used = true;
            }
            if (mark_cell != 0xFFFFFFFFu && em.col <= mark_cell && mark_cell < em.col + w)
                flags |= (uint8_t)EDIT_RUN_MARK;
            if (!put_group(&em, cp, w, cls, flags))
                break;
            p += n;
        }
        if (em.err != 0 || em.col >= em.right)
            break;
    }

    /* The cursor (and any preedit) can sit one past the last character. */
    if (em.err == 0 && cursor_byte == le && !cursor_used) {
        uint8_t flags = (uint8_t)EDIT_RUN_CURSOR;
        if (e->preedit_len > 0) {
            (void)put_preedit(&em, e, &cursor_used);
        } else {
            if (mark_cell != 0xFFFFFFFFu && em.col <= mark_cell && mark_cell < em.col + 1u)
                flags |= (uint8_t)EDIT_RUN_MARK;
            (void)put_group(&em, (uint32_t)' ', 1u, (uint8_t)EDIT_CLS_PLAIN, flags);
        }
    }

    if (em.err != 0)
        return em.err;
    return em.n;
}
