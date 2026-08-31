/*
 * edit_core.c — the public surface: the block layout, the text in and out,
 * the edits, the cursor and the selection.
 *
 * Two rules shape every function here (spec §A.1):
 *
 *   a failed edit changes nothing.  E_FULL / E_LINES / E_UTF8 are decided
 *   before eb_insert_raw / eb_delete_raw are allowed to touch a byte, which
 *   is why the size and line arithmetic is done up front on numbers rather
 *   than discovered half way through a memmove.
 *
 *   what can be clamped is clamped.  Moving left at the start of the text,
 *   deleting past the end, going to line 9999 of an 80 line file and
 *   scrolling past the last line all return EDIT_OK; the caller is not asked
 *   to work out that it hit an edge.  EDIT_E_ARG is reserved for arguments
 *   that cannot mean anything (a NULL, a line number of 0).
 */

#include "edit_internal.h"

static size_t align8(size_t n)
{
    return (n + 7u) & ~(size_t)7u;
}

static bool cfg_ok(const edit_config_t *c)
{
    if (c == NULL)
        return false;
    if (c->max_bytes == 0 || c->max_bytes >= 0xFFFFFFF0u)
        return false;
    if (c->max_lines == 0)
        return false;
    if (c->max_cols == 0 || c->max_rows == 0)
        return false;
    if (c->max_rows > EDIT_MAX_ROWS_HARD)   /* edit_dirty_rows() is 64 bits */
        return false;
    return true;
}

size_t edit_mem_size(const edit_config_t *cfg)
{
    size_t n;

    if (!cfg_ok(cfg))
        return 0;
    n  = align8(sizeof(struct edit));
    n += align8((size_t)cfg->max_bytes + 1u + 2u * EDIT_CANARY_LEN);
    n += align8((size_t)cfg->max_lines * sizeof(uint32_t));
    n += align8((size_t)cfg->max_lines);
    n += align8((size_t)cfg->undo_bytes);
    return n;
}

edit_err_t edit_init(void *block, size_t block_len, const edit_config_t *cfg,
                     edit_t **out)
{
    struct edit *e;
    uint8_t *p;

    if (block == NULL || out == NULL || !cfg_ok(cfg))
        return EDIT_E_ARG;
    if (((uintptr_t)block & 7u) != 0)
        return EDIT_E_ARG;
    if (block_len < edit_mem_size(cfg))
        return EDIT_E_ARG;

    p = (uint8_t *)block;
    e = (struct edit *)block;
    memset(e, 0, sizeof *e);
    e->cfg = *cfg;
    p += align8(sizeof(struct edit));

    e->cap  = cfg->max_bytes + 1u;
    e->base = p;
    e->buf  = p + EDIT_CANARY_LEN;
    p += align8((size_t)e->cap + 2u * EDIT_CANARY_LEN);

    e->line_start = (uint32_t *)(void *)p;
    p += align8((size_t)cfg->max_lines * sizeof(uint32_t));

    e->line_state = p;
    p += align8((size_t)cfg->max_lines);

    e->undo     = p;
    e->undo_cap = cfg->undo_bytes;

    e->gap_begin = 0;
    e->gap_end   = e->cap;
    e->line_count = 1;
    e->line_start[0] = 0;
    e->line_state[0] = (uint8_t)EDIT_LEX_PLAIN;
    e->lex_dirty_min = 1;
    e->goal_col = EDIT_NO_GOAL;
    e->view_cols = cfg->max_cols;
    e->view_rows = cfg->max_rows;
    eu_reset(e);
    eb_stamp_canary(e);
    ev_mark_all(e);

    *out = e;
    return EDIT_OK;
}

/* ------------------------------------------------------- text in and out */

edit_err_t edit_set_text(edit_t *e, const char *utf8, size_t len)
{
    uint32_t n, nl, i, k;

    if (e == NULL || (utf8 == NULL && len != 0))
        return EDIT_E_ARG;
    if (len > (size_t)e->cfg.max_bytes)
        return EDIT_E_FULL;
    n = (uint32_t)len;
    if (!eb_utf8_valid(utf8, n))
        return EDIT_E_UTF8;
    nl = eb_count_nl(utf8, n);
    if (nl + 1u > e->cfg.max_lines)
        return EDIT_E_LINES;

    if (n > 0)
        memcpy(e->buf, utf8, n);
    e->gap_begin = n;
    e->gap_end   = e->cap;

    e->line_start[0] = 0;
    e->line_state[0] = (uint8_t)EDIT_LEX_PLAIN;
    k = 1;
    for (i = 0; i < n; i++) {
        if (utf8[i] != '\n')
            continue;
        /* logical i+1; phys(i+1) is i+1 unless it is the very gap edge */
        e->line_start[k] = (i + 1u < e->gap_begin) ? (i + 1u) : e->cap;
        e->line_state[k] = (uint8_t)EDIT_LEX_DIRTY;   /* lexed when first drawn */
        k++;
    }
    e->line_count = k;
    e->lex_dirty_min = 1;

    e->cur = 0;
    e->cur_line = 0;
    e->cur_col_ok = false;
    e->goal_col = EDIT_NO_GOAL;
    e->sel_active = false;
    e->sel_anchor = 0;
    e->preedit_len = 0;
    e->mark_line1 = 0;
    e->mark_col1 = 0;
    e->top_line = 0;
    e->left_col = 0;
    e->modified = false;
    eu_reset(e);
    eb_stamp_canary(e);
    ev_mark_all(e);
    return EDIT_OK;
}

size_t edit_text_len(const edit_t *e)
{
    return (e == NULL) ? 0u : (size_t)eb_len(e);
}

size_t edit_copy_text(const edit_t *e, size_t off, char *dst, size_t cap)
{
    uint32_t len, n;

    if (e == NULL || dst == NULL || cap == 0)
        return 0;
    len = eb_len(e);
    if (off >= (size_t)len)
        return 0;
    n = len - (uint32_t)off;
    if ((size_t)n > cap)
        n = (uint32_t)cap;
    eb_copy_out(e, (uint32_t)off, n, dst);
    return (size_t)n;
}

/* ------------------------------------------------------------- the edits */

static void after_edit(struct edit *e, uint32_t line0, uint32_t lc_before)
{
    e->modified = true;
    e->edit_count++;
    e->goal_col = EDIT_NO_GOAL;
    if (e->line_count != lc_before)
        ev_mark_from(e, line0);
    else
        ev_mark_line(e, line0);
    e->dirty_flags |= EDIT_DIRTY_CURSOR | EDIT_DIRTY_STATUS;
    ev_mark_line(e, e->cur_line);
    ev_follow_cursor(e);
}

edit_err_t edit_insert(edit_t *e, const char *utf8, size_t len)
{
    uint32_t n, a = 0, b = 0, d = 0, sel_nl = 0, new_nl, lc_before, line0;
    bool replace;

    if (e == NULL || (utf8 == NULL && len != 0))
        return EDIT_E_ARG;
    if (len > (size_t)e->cfg.max_bytes)
        return EDIT_E_FULL;
    n = (uint32_t)len;
    if (n > 0 && !eb_utf8_valid(utf8, n))
        return EDIT_E_UTF8;

    replace = edit_selection(e, NULL, NULL);
    if (replace) {
        size_t sa, sb;
        (void)edit_selection(e, &sa, &sb);
        a = (uint32_t)sa;
        b = (uint32_t)sb;
        d = b - a;
        sel_nl = eb_count_nl_range(e, a, b);
    }
    if (n == 0 && !replace)
        return EDIT_OK;

    /* Everything that can fail is decided here, before the first byte moves. */
    if ((uint64_t)eb_len(e) - d + n > (uint64_t)e->cfg.max_bytes)
        return EDIT_E_FULL;
    new_nl = eb_count_nl(utf8, n);
    if ((uint64_t)e->line_count - sel_nl + new_nl > (uint64_t)e->cfg.max_lines)
        return EDIT_E_LINES;

    lc_before = e->line_count;
    line0 = replace ? eb_find_line(e, a) : e->cur_line;

    if (replace) {
        e->sel_active = false;
        eu_record_delete(e, a, d, false);
        eb_delete_raw(e, a, b);
    }
    if (n > 0) {
        eu_record_insert(e, e->cur, utf8, n, replace);
        eb_insert_raw(e, utf8, n);
    }
    after_edit(e, line0, lc_before);
    return EDIT_OK;
}

edit_err_t edit_delete(edit_t *e, int32_t n)
{
    uint32_t a, b, len, lc_before, line0;
    int64_t i, count;

    if (e == NULL)
        return EDIT_E_ARG;
    if (n == 0)
        return EDIT_OK;

    len = eb_len(e);
    if (n < 0) {
        count = -(int64_t)n;
        a = e->cur;
        for (i = 0; i < count && a > 0; i++)
            a = eb_prev_char(e, a);
        b = e->cur;
    } else {
        count = n;
        a = e->cur;
        b = e->cur;
        for (i = 0; i < count && b < len; i++)
            b = eb_next_char(e, b);
    }
    if (a >= b)
        return EDIT_OK;                 /* clamped at an edge: nothing to do */

    lc_before = e->line_count;
    line0 = eb_find_line(e, a);
    eu_record_delete(e, a, b - a, false);
    eb_delete_raw(e, a, b);
    after_edit(e, line0, lc_before);
    return EDIT_OK;
}

edit_err_t edit_undo(edit_t *e)
{
    if (e == NULL)
        return EDIT_E_ARG;
    if (!eu_undo_step(e))
        return EDIT_E_STATE;
    e->sel_active = false;
    e->modified = true;
    e->edit_count++;
    e->goal_col = EDIT_NO_GOAL;
    e->dirty_flags |= EDIT_DIRTY_CURSOR | EDIT_DIRTY_STATUS;
    ev_mark_all(e);
    ev_follow_cursor(e);
    return EDIT_OK;
}

edit_err_t edit_redo(edit_t *e)
{
    if (e == NULL)
        return EDIT_E_ARG;
    if (!eu_redo_step(e))
        return EDIT_E_STATE;
    e->sel_active = false;
    e->modified = true;
    e->edit_count++;
    e->goal_col = EDIT_NO_GOAL;
    e->dirty_flags |= EDIT_DIRTY_CURSOR | EDIT_DIRTY_STATUS;
    ev_mark_all(e);
    ev_follow_cursor(e);
    return EDIT_OK;
}

/* -------------------------------------------------- cursor and selection */

static bool is_word_byte(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c >= 0x80u;
}

static void moved(struct edit *e, uint32_t old_line, bool keep_goal)
{
    e->cur_line = eb_find_line(e, e->cur);
    e->cur_col_ok = false;
    if (!keep_goal)
        e->goal_col = EDIT_NO_GOAL;
    ev_mark_line(e, old_line);
    ev_mark_line(e, e->cur_line);
    e->dirty_flags |= EDIT_DIRTY_CURSOR | EDIT_DIRTY_STATUS;
    ev_follow_cursor(e);
}

edit_err_t edit_move(edit_t *e, edit_motion_t m, uint32_t n)
{
    uint32_t old_line, len, i;
    bool keep_goal = false;

    if (e == NULL)
        return EDIT_E_ARG;
    if (n == 0)
        n = 1;                          /* a motion with no count is one step */
    old_line = e->cur_line;
    len = eb_len(e);

    switch (m) {
    case EDIT_M_LEFT:
        for (i = 0; i < n && e->cur > 0; i++)
            e->cur = eb_prev_char(e, e->cur);
        break;
    case EDIT_M_RIGHT:
        for (i = 0; i < n && e->cur < len; i++)
            e->cur = eb_next_char(e, e->cur);
        break;
    case EDIT_M_UP:
    case EDIT_M_DOWN:
    case EDIT_M_PGUP:
    case EDIT_M_PGDN: {
        uint32_t step = (m == EDIT_M_PGUP || m == EDIT_M_PGDN) ? e->view_rows : 1u;
        uint64_t away = (uint64_t)step * (uint64_t)n;
        uint32_t goal = (e->goal_col == EDIT_NO_GOAL)
                            ? ev_cursor_col(e) : e->goal_col;
        uint32_t line;
        if (m == EDIT_M_UP || m == EDIT_M_PGUP)
            line = (away >= (uint64_t)e->cur_line) ? 0u : e->cur_line - (uint32_t)away;
        else
            line = (away >= (uint64_t)(e->line_count - 1u - e->cur_line))
                       ? e->line_count - 1u : e->cur_line + (uint32_t)away;
        e->cur = ev_col_to_byte(e, line, goal);
        e->goal_col = goal;
        keep_goal = true;
        break;
    }
    case EDIT_M_HOME:
        e->cur = eb_line_start(e, e->cur_line);
        break;
    case EDIT_M_END:
        e->cur = eb_line_end(e, e->cur_line);
        break;
    case EDIT_M_DOC_HOME:
        e->cur = 0;
        break;
    case EDIT_M_DOC_END:
        e->cur = len;
        break;
    case EDIT_M_WORD_LEFT:
        for (i = 0; i < n && e->cur > 0; i++) {
            while (e->cur > 0 && !is_word_byte(eb_at(e, eb_prev_char(e, e->cur))))
                e->cur = eb_prev_char(e, e->cur);
            while (e->cur > 0 && is_word_byte(eb_at(e, eb_prev_char(e, e->cur))))
                e->cur = eb_prev_char(e, e->cur);
        }
        break;
    case EDIT_M_WORD_RIGHT:
        for (i = 0; i < n && e->cur < len; i++) {
            while (e->cur < len && !is_word_byte(eb_at(e, e->cur)))
                e->cur = eb_next_char(e, e->cur);
            while (e->cur < len && is_word_byte(eb_at(e, e->cur)))
                e->cur = eb_next_char(e, e->cur);
        }
        break;
    default:
        return EDIT_E_ARG;
    }
    moved(e, old_line, keep_goal);
    return EDIT_OK;
}

edit_err_t edit_goto(edit_t *e, uint32_t line1, uint32_t col1)
{
    uint32_t old_line, line;

    if (e == NULL)
        return EDIT_E_ARG;
    if (line1 == 0)
        return EDIT_E_ARG;
    old_line = e->cur_line;
    line = line1 - 1u;
    if (line >= e->line_count)
        line = e->line_count - 1u;
    e->cur = ev_col_to_byte(e, line, (col1 > 0) ? col1 - 1u : 0u);
    moved(e, old_line, false);
    return EDIT_OK;
}

void edit_select_begin(edit_t *e)
{
    if (e == NULL)
        return;
    e->sel_anchor = e->cur;
    e->sel_active = true;
    e->dirty_flags |= EDIT_DIRTY_STATUS;
}

void edit_select_end(edit_t *e)
{
    if (e == NULL || !e->sel_active)
        return;
    e->sel_active = false;
    ev_mark_all(e);
}

bool edit_selection(const edit_t *e, size_t *from_byte, size_t *to_byte)
{
    uint32_t a, b;

    if (e == NULL || !e->sel_active || e->sel_anchor == e->cur)
        return false;
    a = (e->sel_anchor < e->cur) ? e->sel_anchor : e->cur;
    b = (e->sel_anchor < e->cur) ? e->cur : e->sel_anchor;
    if (from_byte != NULL)
        *from_byte = a;
    if (to_byte != NULL)
        *to_byte = b;
    return true;
}

size_t edit_copy_selection(const edit_t *e, char *dst, size_t cap)
{
    size_t a, b, n;

    if (dst == NULL || cap == 0 || !edit_selection(e, &a, &b))
        return 0;
    n = b - a;
    if (n > cap)
        n = cap;
    eb_copy_out(e, (uint32_t)a, (uint32_t)n, dst);
    return n;
}

edit_err_t edit_delete_selection(edit_t *e)
{
    size_t a, b;
    uint32_t lc_before, line0;

    if (e == NULL)
        return EDIT_E_ARG;
    if (!edit_selection(e, &a, &b))
        return EDIT_E_STATE;

    lc_before = e->line_count;
    line0 = eb_find_line(e, (uint32_t)a);
    e->sel_active = false;
    eu_record_delete(e, (uint32_t)a, (uint32_t)(b - a), false);
    eb_delete_raw(e, (uint32_t)a, (uint32_t)b);
    after_edit(e, line0, lc_before);
    return EDIT_OK;
}

edit_pos_t edit_cursor(const edit_t *e)
{
    edit_pos_t p;

    p.line1 = 1;
    p.col1 = 1;
    p.byte = 0;
    if (e == NULL)
        return p;
    p.line1 = e->cur_line + 1u;
    p.col1 = ev_cursor_col(e) + 1u;
    p.byte = e->cur;
    return p;
}

uint32_t edit_line_count(const edit_t *e)
{
    return (e == NULL) ? 0u : e->line_count;
}

/* ------------------------------------------------------- preedit / marks */

edit_err_t edit_set_preedit(edit_t *e, const char *utf8, size_t len)
{
    if (e == NULL || (utf8 == NULL && len != 0))
        return EDIT_E_ARG;
    if (len > EDIT_PREEDIT_MAX)
        return EDIT_E_ARG;
    if (len > 0 && !eb_utf8_valid(utf8, len))
        return EDIT_E_UTF8;
    if (len > 0)
        memcpy(e->preedit, utf8, len);
    e->preedit_len = (uint16_t)len;
    ev_mark_line(e, e->cur_line);
    e->dirty_flags |= EDIT_DIRTY_CURSOR;
    return EDIT_OK;
}

edit_err_t edit_set_mark(edit_t *e, uint32_t line1, uint32_t col1)
{
    if (e == NULL)
        return EDIT_E_ARG;
    if (line1 == 0)
        return EDIT_E_ARG;
    if (e->mark_line1 != 0)
        ev_mark_line(e, e->mark_line1 - 1u);
    if (line1 > e->line_count)
        line1 = e->line_count;
    e->mark_line1 = line1;
    e->mark_col1 = (col1 == 0) ? 1u : col1;
    ev_mark_line(e, line1 - 1u);
    e->dirty_flags |= EDIT_DIRTY_STATUS;
    return EDIT_OK;
}

void edit_clear_mark(edit_t *e)
{
    if (e == NULL || e->mark_line1 == 0)
        return;
    ev_mark_line(e, e->mark_line1 - 1u);
    e->mark_line1 = 0;
    e->mark_col1 = 0;
    e->dirty_flags |= EDIT_DIRTY_STATUS;
}

/* ------------------------------------------------------------- the state */

bool edit_modified(const edit_t *e)
{
    return (e == NULL) ? false : e->modified;
}

void edit_mark_saved(edit_t *e)
{
    if (e == NULL)
        return;
    e->modified = false;
    e->dirty_flags |= EDIT_DIRTY_STATUS;
}

uint32_t edit_edit_count(const edit_t *e)
{
    return (e == NULL) ? 0u : e->edit_count;
}

void edit_stats(const edit_t *e, edit_stats_t *out)
{
    if (out == NULL)
        return;
    if (e == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = e->stats;
}
