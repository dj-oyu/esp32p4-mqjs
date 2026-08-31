/*
 * edit_check.c — the six invariants of spec §A.1, all of them, by recount.
 *
 * This is the thing the host tests and the fuzzer lean on, so it re-derives
 * rather than re-reads: the line index is checked against a fresh newline
 * scan of the text, the cursor's line against a fresh binary search, the
 * lexer states against a fresh lex of the line above.  It costs O(text) and
 * must never be called from the typing path (spec §A.1: "打鍵経路では呼ばない";
 * on the device it sits behind a Kconfig).
 *
 * A note for whoever adds an invariant: check something that could be *false*.
 * Re-reading the same field the code just wrote proves nothing — the fs_core
 * lesson in docs/native-editor-plan.md §1 is that a check earns its place
 * only when a real defect makes it fail.
 */

#include "edit_internal.h"

/* Walk the whole text as UTF-8, and report whether `probe` fell on a
 * character boundary on the way through. */
static bool walk_utf8(const struct edit *e, uint32_t probe, bool *probe_on_edge)
{
    uint32_t len = eb_len(e), p = 0;

    *probe_on_edge = (probe == 0);
    while (p < len) {
        uint8_t c = eb_at(e, p);
        uint32_t need, cp, lo, k;

        if (c < 0x80u) { need = 0; cp = c; lo = 0; }
        else if (c >= 0xC2u && c <= 0xDFu) { need = 1; cp = c & 0x1Fu; lo = 0x80u; }
        else if (c >= 0xE0u && c <= 0xEFu) { need = 2; cp = c & 0x0Fu; lo = 0x800u; }
        else if (c >= 0xF0u && c <= 0xF4u) { need = 3; cp = c & 0x07u; lo = 0x10000u; }
        else return false;

        if (need > len - p - 1u)
            return false;
        for (k = 1; k <= need; k++) {
            uint8_t cc = eb_at(e, p + k);
            if ((cc & 0xC0u) != 0x80u)
                return false;
            cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
        }
        if (cp < lo || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu)
            return false;
        p += need + 1u;
        if (p == probe)
            *probe_on_edge = true;
    }
    return p == len;
}

bool edit_check(const edit_t *e)
{
    uint32_t len, i, k;
    bool on_edge;

    if (e == NULL)
        return false;

    /* 1. gap bounds and the length identity */
    if (!(e->gap_begin <= e->gap_end && e->gap_end <= e->cap))
        return false;
    if (e->cap != e->cfg.max_bytes + 1u)
        return false;
    len = eb_len(e);
    if (len > e->cfg.max_bytes)
        return false;
    if (eb_gap(e) == 0)
        return false;                    /* the spare byte must survive */

    /* 6. canaries (both ends of the buffer, and both ends of the gap) */
    if (!eb_canary_ok(e))
        return false;

    /* 2. the line index */
    if (e->line_count == 0 || e->line_count > e->cfg.max_lines)
        return false;
    if (e->line_start[0] != 0)
        return false;
    k = 1;
    for (i = 0; i < len; i++) {
        uint32_t p;
        if (eb_at(e, i) != '\n')
            continue;
        if (k >= e->line_count)
            return false;                /* more newlines than lines */
        p = e->line_start[k];
        if (p >= e->gap_begin && p < e->gap_end)
            return false;                /* an entry pointing into the gap */
        if (p > e->cap)
            return false;
        if (eb_logi(e, p) != i + 1u)
            return false;
        k++;
    }
    if (k != e->line_count)
        return false;                    /* fewer newlines than lines */

    /* 3. the cursor: a character boundary, and a line index that still
     *    matches a fresh search.  The selection anchor gets the same test. */
    if (e->cur > len)
        return false;
    if (!walk_utf8(e, e->cur, &on_edge) || !on_edge)
        return false;
    if (e->cur_line != eb_find_line(e, e->cur))
        return false;
    if (e->cur < eb_line_start(e, e->cur_line) || e->cur > eb_line_end(e, e->cur_line))
        return false;
    if (e->cur_col_ok && e->cur_col != ev_col_cells(e, e->cur_line, e->cur))
        return false;                    /* the cached column drifted */
    if (e->sel_active) {
        if (e->sel_anchor > len)
            return false;
        if (!walk_utf8(e, e->sel_anchor, &on_edge) || !on_edge)
            return false;
    }

    /* 4. the lexer states, except the lines whose recompute bit is set */
    if (e->line_state[0] != (uint8_t)EDIT_LEX_PLAIN)
        return false;
    if (e->lex_dirty_min > e->line_count)
        return false;
    for (i = 0; i < e->line_count; i++) {
        uint8_t st = e->line_state[i];
        if ((st & ~(uint8_t)(EDIT_LEX_MASK | EDIT_LEX_DIRTY)) != 0)
            return false;
        if ((st & EDIT_LEX_MASK) > EDIT_LEX_TEMPLATE)
            return false;
        if ((st & EDIT_LEX_DIRTY) && i < e->lex_dirty_min)
            return false;                /* the lower bound has to be a bound */
        if (i == 0 || (st & EDIT_LEX_DIRTY))
            continue;
        if ((st & EDIT_LEX_MASK) !=
            lex_line_end_state(e, i - 1u, (uint8_t)(e->line_state[i - 1u] & EDIT_LEX_MASK)))
            return false;
    }

    /* 5. the undo ring */
    if (!eu_walk(e))
        return false;

    /* the view, which is not one of the six but is cheap to keep honest */
    if (e->view_cols == 0 || e->view_rows == 0)
        return false;
    if (e->view_cols > e->cfg.max_cols || e->view_rows > e->cfg.max_rows)
        return false;
    if (e->top_line >= e->line_count)
        return false;
    if (e->preedit_len > EDIT_PREEDIT_MAX)
        return false;
    return true;
}
