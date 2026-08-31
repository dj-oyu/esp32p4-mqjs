/*
 * edit_buf.c — the gap buffer, the line index and the UTF-8 walkers.
 *
 * Everything here is *raw*: it does not validate, does not record undo and
 * does not touch the dirty flags.  The callers in edit_core.c do all three
 * before calling in, which is how "a failed edit changes nothing" is kept —
 * the checks happen while the buffer is still untouched.
 *
 * The one subtlety worth reading twice is the line index.  Entries are
 * physical offsets (edit_internal.h explains why), so three operations have
 * to keep them canonical:
 *
 *   moving the gap   shifts every entry the memmove moved, by exactly the gap
 *                    size.  The entries are sorted, so the affected span is
 *                    two binary searches wide.
 *   inserting        never moves an entry after the cursor: they all live at
 *                    or past gap_end and the gap only grows leftwards into
 *                    itself.  The one fixup is the line that *starts* at the
 *                    cursor — it is stored as gap_end, and the inserted text
 *                    belongs to it, so it becomes the old gap_begin.
 *   deleting         drops the entries whose newline was swallowed, and
 *                    re-canonicalises the entry that starts exactly at the
 *                    cut (it was gap_end; the gap just grew, so it is the new
 *                    gap_end).
 */

#include "edit_internal.h"

/* Distinct patterns so that a stray write, a memmove with the wrong length
 * and a memset of the wrong region are all told apart when edit_check trips. */
#define CAN_HEAD  0xA5u
#define CAN_TAIL  0x5Au
#define CAN_GAPL  0xC3u
#define CAN_GAPR  0x3Cu

static void fill(uint8_t *p, uint8_t v, uint32_t n)
{
    memset(p, (int)v, (size_t)n);
}

static bool all_are(const uint8_t *p, uint8_t v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != v)
            return false;
    return true;
}

/*
 * Invariant 6.  The two outer canaries always exist; the two inside the gap
 * only when the gap is wide enough to hold both without meeting (the gap
 * shrinks to a single byte when the text is at max_bytes).  They are stamped
 * after every operation that moves an edge of the gap.
 */
void eb_stamp_canary(struct edit *e)
{
    fill(e->base, CAN_HEAD, EDIT_CANARY_LEN);
    fill(e->base + EDIT_CANARY_LEN + e->cap, CAN_TAIL, EDIT_CANARY_LEN);
    if (eb_gap(e) >= 2u * EDIT_CANARY_LEN) {
        fill(e->buf + e->gap_begin, CAN_GAPL, EDIT_CANARY_LEN);
        fill(e->buf + e->gap_end - EDIT_CANARY_LEN, CAN_GAPR, EDIT_CANARY_LEN);
    }
}

bool eb_canary_ok(const struct edit *e)
{
    if (!all_are(e->base, CAN_HEAD, EDIT_CANARY_LEN))
        return false;
    if (!all_are(e->base + EDIT_CANARY_LEN + e->cap, CAN_TAIL, EDIT_CANARY_LEN))
        return false;
    if (eb_gap(e) >= 2u * EDIT_CANARY_LEN) {
        if (!all_are(e->buf + e->gap_begin, CAN_GAPL, EDIT_CANARY_LEN))
            return false;
        if (!all_are(e->buf + e->gap_end - EDIT_CANARY_LEN, CAN_GAPR, EDIT_CANARY_LEN))
            return false;
    }
    return true;
}

/* First index in [1, line_count) whose physical offset is >= p.  Index 0 is
 * excluded on purpose: it is the literal 0 that never converts. */
static uint32_t idx_lower_bound(const struct edit *e, uint32_t p)
{
    uint32_t lo = 1, hi = e->line_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (e->line_start[mid] < p)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

uint32_t eb_find_line(const struct edit *e, uint32_t l)
{
    uint32_t lo = 0, hi = e->line_count;   /* last i with line_start(i) <= l */
    while (lo + 1u < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (eb_line_start(e, mid) <= l)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

void eb_move_gap(struct edit *e, uint32_t l)
{
    uint32_t gs = eb_gap(e);

    if (l == e->gap_begin)
        return;

    if (l < e->gap_begin) {
        uint32_t k = e->gap_begin - l;
        uint32_t i = idx_lower_bound(e, l);
        uint32_t j = idx_lower_bound(e, e->gap_begin);
        for (; i < j; i++)
            e->line_start[i] += gs;
        memmove(e->buf + e->gap_end - k, e->buf + l, k);
        e->gap_begin -= k;
        e->gap_end   -= k;
        e->stats.gap_moves++;
        e->stats.gap_bytes_moved += k;
    } else {
        uint32_t k = l - e->gap_begin;
        uint32_t i = idx_lower_bound(e, e->gap_end);
        uint32_t j = idx_lower_bound(e, e->gap_end + k);
        for (; i < j; i++)
            e->line_start[i] -= gs;
        memmove(e->buf + e->gap_begin, e->buf + e->gap_end, k);
        e->gap_begin += k;
        e->gap_end   += k;
        e->stats.gap_moves++;
        e->stats.gap_bytes_moved += k;
    }
    eb_stamp_canary(e);
}

void eb_copy_out(const struct edit *e, uint32_t from, uint32_t n, void *dst)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t p;

    if (n == 0)
        return;
    p = eb_phys(e, from);
    if (p < e->gap_begin) {
        uint32_t first = e->gap_begin - p;
        if (first > n)
            first = n;
        memcpy(d, e->buf + p, first);
        if (n > first)
            memcpy(d + first, e->buf + e->gap_end, n - first);
    } else {
        memcpy(d, e->buf + p, n);
    }
}

uint32_t eb_count_nl(const char *s, uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++)
        if (s[i] == '\n')
            c++;
    return c;
}

uint32_t eb_count_nl_range(const struct edit *e, uint32_t a, uint32_t b)
{
    uint32_t c = 0;
    for (uint32_t i = a; i < b; i++)
        if (eb_at(e, i) == '\n')
            c++;
    return c;
}

void eb_insert_raw(struct edit *e, const char *s, uint32_t n)
{
    uint32_t off, gb, ge, cl, m;
    uint32_t col = e->cur_col;
    bool col_ok = e->cur_col_ok;

    if (n == 0)
        return;

    off = e->cur;
    eb_move_gap(e, off);
    gb = e->gap_begin;
    ge = e->gap_end;
    cl = e->cur_line;

    /* The line that starts exactly here is stored as gap_end; the text about
     * to be written belongs to it, so it now starts at the old gap_begin. */
    if (cl > 0 && e->line_start[cl] == ge)
        e->line_start[cl] = gb;

    memcpy(e->buf + gb, s, n);
    e->gap_begin = gb + n;

    m = eb_count_nl(s, n);
    if (m > 0) {
        uint32_t tail = e->line_count - cl - 1u;
        uint32_t k = 0;
        memmove(&e->line_start[cl + 1u + m], &e->line_start[cl + 1u],
                (size_t)tail * sizeof(uint32_t));
        memmove(&e->line_state[cl + 1u + m], &e->line_state[cl + 1u], (size_t)tail);
        e->stats.newline_index_moves += tail;
        for (uint32_t j = 0; j < n; j++) {
            if (s[j] != '\n')
                continue;
            uint32_t p = gb + j + 1u;
            if (p == e->gap_begin)     /* the newline was the last byte written */
                p = ge;
            e->line_start[cl + 1u + k] = p;
            e->line_state[cl + 1u + k] = (uint8_t)EDIT_LEX_DIRTY;
            k++;
        }
        e->line_count += m;
    }

    if (e->sel_active && e->sel_anchor >= off)
        e->sel_anchor += n;

    e->cur      = off + n;
    e->cur_line = cl + m;

    /* Typing keeps the cached cursor column: the text goes in at the cursor,
     * so the new column is the old one plus the cells just written.  A newline
     * (or a stale cache) drops it and the next reader walks the line. */
    if (col_ok && m == 0) {
        uint32_t i = 0;
        while (i < n) {
            uint32_t cp;
            uint32_t adv = eb_decode_mem(s + i, n - i, &cp);
            col += ev_cells_of(cp, col);
            i += adv;
        }
        e->cur_col = col;
        e->cur_col_ok = true;
    } else {
        e->cur_col_ok = false;
    }
    eb_stamp_canary(e);
    lex_touch(e, cl);
    /* A split changes the content of two lines, not one: the tail of the old
     * line now lives in the last inserted line, so the line *below* the split
     * has to be restated as well.  (Its stored state was derived from a line
     * whose content no longer exists.) */
    if (m > 0)
        lex_touch(e, cl + m);
}

void eb_delete_raw(struct edit *e, uint32_t a, uint32_t b)
{
    uint32_t d, ge, i0, i1, keep;

    if (a >= b)
        return;

    eb_move_gap(e, a);
    d  = b - a;
    ge = e->gap_end;

    /* Entries in (ge, ge+d] lost their newline; the entry at ge is the line
     * that starts at the cut and survives, re-stamped to the new gap_end. */
    i0 = idx_lower_bound(e, ge);
    i1 = idx_lower_bound(e, ge + d + 1u);
    keep = i0;
    if (i0 < e->line_count && e->line_start[i0] == ge) {
        e->line_start[i0] = ge + d;
        keep = i0 + 1u;
    }
    if (i1 > keep) {
        uint32_t tail = e->line_count - i1;
        memmove(&e->line_start[keep], &e->line_start[i1], (size_t)tail * sizeof(uint32_t));
        memmove(&e->line_state[keep], &e->line_state[i1], (size_t)tail);
        e->stats.newline_index_moves += tail;
        e->line_count -= (i1 - keep);
    }

    e->gap_end = ge + d;

    if (e->sel_active) {
        if (e->sel_anchor >= b)
            e->sel_anchor -= d;
        else if (e->sel_anchor > a)
            e->sel_anchor = a;
    }

    e->cur      = a;
    e->cur_line = eb_find_line(e, a);
    e->cur_col_ok = false;
    eb_stamp_canary(e);
    lex_touch(e, e->cur_line);
}

/* ------------------------------------------------------------------ UTF-8 */

bool eb_utf8_valid(const char *s, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;

    while (i < n) {
        uint8_t c = p[i];
        uint32_t need, cp, lo;

        if (c < 0x80u) { i++; continue; }
        if (c >= 0xC2u && c <= 0xDFu) { need = 1; cp = c & 0x1Fu; lo = 0x80u; }
        else if (c >= 0xE0u && c <= 0xEFu) { need = 2; cp = c & 0x0Fu; lo = 0x800u; }
        else if (c >= 0xF0u && c <= 0xF4u) { need = 3; cp = c & 0x07u; lo = 0x10000u; }
        else return false;                      /* 0x80..0xC1, 0xF5..0xFF */

        if (n - i - 1u < need)          /* truncated: not enough continuations */
            return false;
        for (uint32_t k = 1; k <= need; k++) {
            uint8_t cc = p[i + k];
            if ((cc & 0xC0u) != 0x80u)
                return false;
            cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
        }
        if (cp < lo)                            /* overlong */
            return false;
        if (cp >= 0xD800u && cp <= 0xDFFFu)     /* surrogate */
            return false;
        if (cp > 0x10FFFFu)
            return false;
        i += need + 1u;
    }
    return true;
}

uint32_t eb_decode(const struct edit *e, uint32_t l, uint32_t *cp)
{
    uint32_t len = eb_len(e);
    uint8_t  c;
    uint32_t need, v;

    if (l >= len) {
        *cp = 0;
        return 0;
    }
    c = eb_at(e, l);
    if (c < 0x80u) { *cp = c; return 1; }
    if ((c & 0xE0u) == 0xC0u) { need = 1; v = c & 0x1Fu; }
    else if ((c & 0xF0u) == 0xE0u) { need = 2; v = c & 0x0Fu; }
    else if ((c & 0xF8u) == 0xF0u) { need = 3; v = c & 0x07u; }
    else { *cp = c; return 1; }                 /* cannot happen: text is validated */

    if (need > len - l - 1u) {                  /* likewise unreachable */
        *cp = c;
        return 1;
    }
    for (uint32_t k = 1; k <= need; k++)
        v = (v << 6) | (uint32_t)(eb_at(e, l + k) & 0x3Fu);
    *cp = v;
    return need + 1u;
}

uint32_t eb_decode_mem(const char *s, uint32_t n, uint32_t *cp)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t need, v;

    if (n == 0) {
        *cp = 0;
        return 0;
    }
    if (p[0] < 0x80u) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0u) == 0xC0u) { need = 1; v = p[0] & 0x1Fu; }
    else if ((p[0] & 0xF0u) == 0xE0u) { need = 2; v = p[0] & 0x0Fu; }
    else if ((p[0] & 0xF8u) == 0xF0u) { need = 3; v = p[0] & 0x07u; }
    else { *cp = p[0]; return 1; }              /* validated input: unreachable */
    if (need > n - 1u) {
        *cp = p[0];
        return 1;
    }
    for (uint32_t k = 1; k <= need; k++)
        v = (v << 6) | (uint32_t)(p[k] & 0x3Fu);
    *cp = v;
    return need + 1u;
}

uint32_t eb_next_char(const struct edit *e, uint32_t l)
{
    uint32_t cp;
    uint32_t n = eb_decode(e, l, &cp);
    return (n == 0) ? l : l + n;
}

uint32_t eb_prev_char(const struct edit *e, uint32_t l)
{
    while (l > 0) {
        l--;
        if ((eb_at(e, l) & 0xC0u) != 0x80u)
            break;
    }
    return l;
}
