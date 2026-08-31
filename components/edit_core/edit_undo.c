/*
 * edit_undo.c — the byte-capped undo/redo ring (spec §A.1, §E-13).
 *
 * Records are variable length and stored back to back in a byte ring:
 *
 *     +0  kind      u8    EDIT_U_INSERT / EDIT_U_DELETE
 *     +1  flags     u8    EDIT_U_CHAIN = "keep going into the record before me"
 *     +2  reserved  u16
 *     +4  off       u32   logical byte offset the operation happened at
 *     +8  len       u32   length of the text that follows
 *     +12 text[len]
 *     +12+len total u32   = 16 + len, so the ring can also be read backwards
 *
 * Three absolute (never wrapping) counters address it:
 *
 *     u_head   oldest byte still kept
 *     u_tail   end of the newest record that can still be undone
 *     u_top    end of the newest record; [u_tail, u_top) is the redo tail
 *
 * head <= tail <= top and top - head <= undo_cap.  Pushing a record drops the
 * redo tail (top = tail) first — the classic linear history.
 *
 * Eviction is from the head only, so LIFO undo from the tail always replays
 * states that really existed: an undo can therefore never overflow max_bytes
 * or max_lines, and needs no limit checks of its own.
 *
 * Coalescing: consecutive single-character insertions grow the newest record
 * instead of pushing a new one, so a typed word undoes in one step.  A newline
 * breaks the run (pressing Enter is its own undo step), and so does anything
 * that is not a plain forward-typed character.
 */

#include "edit_internal.h"

static uint32_t ring_idx(const struct edit *e, uint64_t at)
{
    return (uint32_t)(at % (uint64_t)e->undo_cap);
}

static void ring_put(struct edit *e, uint64_t at, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i, first;

    if (n == 0)
        return;
    i = ring_idx(e, at);
    first = e->undo_cap - i;
    if (first > n)
        first = n;
    memcpy(e->undo + i, s, first);
    if (n > first)
        memcpy(e->undo, s + first, n - first);
}

static void ring_get(const struct edit *e, uint64_t at, void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t i, first;

    if (n == 0)
        return;
    i = ring_idx(e, at);
    first = e->undo_cap - i;
    if (first > n)
        first = n;
    memcpy(d, e->undo + i, first);
    if (n > first)
        memcpy(d + first, e->undo, n - first);
}

static void ring_put_u32(struct edit *e, uint64_t at, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
    b[2] = (uint8_t)((v >> 16) & 0xFFu);
    b[3] = (uint8_t)((v >> 24) & 0xFFu);
    ring_put(e, at, b, 4);
}

static uint32_t ring_get_u32(const struct edit *e, uint64_t at)
{
    uint8_t b[4];
    ring_get(e, at, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* True when the bytes are exactly one UTF-8 character — the condition for
 * "consecutive single-character insertion" in spec §A.1. */
static bool one_char(const char *s, uint32_t n)
{
    uint8_t c = (uint8_t)s[0];
    uint32_t want;

    if (n == 0 || n > 4u)
        return false;
    if (c < 0x80u) want = 1;
    else if ((c & 0xE0u) == 0xC0u) want = 2;
    else if ((c & 0xF0u) == 0xE0u) want = 3;
    else if ((c & 0xF8u) == 0xF0u) want = 4;
    else return false;
    return want == n;
}

typedef struct {
    uint64_t start;
    uint32_t total;
    uint8_t  kind, flags;
    uint32_t off, len;
} urec_t;

static void rec_at(const struct edit *e, uint64_t start, urec_t *r)
{
    uint8_t h[2];
    ring_get(e, start, h, 2);
    r->start = start;
    r->kind  = h[0];
    r->flags = h[1];
    r->off   = ring_get_u32(e, start + 4u);
    r->len   = ring_get_u32(e, start + 8u);
    r->total = EDIT_U_OVERHEAD + r->len;
}

void eu_reset(struct edit *e)
{
    e->u_head = e->u_tail = e->u_top = 0;
    e->u_coalesce = false;
}

/* Make `need` bytes free without touching anything at or after `protect`. */
static bool ring_reserve(struct edit *e, uint32_t need, uint64_t protect)
{
    while ((uint64_t)e->undo_cap - (e->u_top - e->u_head) < (uint64_t)need) {
        urec_t r;
        if (e->u_head >= protect)
            return false;
        rec_at(e, e->u_head, &r);
        if (r.total < EDIT_U_OVERHEAD || (uint64_t)r.total > e->u_top - e->u_head)
            return false;                      /* corrupt: refuse rather than loop */
        e->u_head += r.total;
        e->stats.undo_evictions++;
    }
    return true;
}

/* Text source for a record: either a caller buffer or a span of the text. */
static void put_text(struct edit *e, uint64_t at, const char *s, uint32_t off, uint32_t n)
{
    if (s != NULL) {
        ring_put(e, at, s, n);
    } else {
        uint8_t tmp[64];
        uint32_t done = 0;
        while (done < n) {
            uint32_t c = n - done;
            if (c > sizeof tmp)
                c = (uint32_t)sizeof tmp;
            eb_copy_out(e, off + done, c, tmp);
            ring_put(e, at + done, tmp, c);
            done += c;
        }
    }
}

static void push(struct edit *e, uint8_t kind, uint32_t off, const char *s,
                 uint32_t src_off, uint32_t n, bool chain)
{
    uint32_t total = EDIT_U_OVERHEAD + n;
    uint8_t  h[2];

    e->u_top = e->u_tail;                      /* a new edit drops the redo tail */
    if (e->undo_cap == 0)
        return;
    if (!ring_reserve(e, total, e->u_top)) {
        /* The record cannot fit even in an empty ring.  Keeping the older
         * records would leave a hole in the history, so the history goes. */
        eu_reset(e);
        e->stats.undo_evictions++;
        return;
    }
    h[0] = kind;
    h[1] = chain ? (uint8_t)EDIT_U_CHAIN : 0u;
    ring_put(e, e->u_top, h, 2);
    h[0] = h[1] = 0;
    ring_put(e, e->u_top + 2u, h, 2);          /* reserved */
    ring_put_u32(e, e->u_top + 4u, off);
    ring_put_u32(e, e->u_top + 8u, n);
    put_text(e, e->u_top + EDIT_U_HDR, s, src_off, n);
    ring_put_u32(e, e->u_top + EDIT_U_HDR + n, total);
    e->u_top += total;
    e->u_tail = e->u_top;
}

void eu_record_insert(struct edit *e, uint32_t off, const char *s, uint32_t n, bool chain)
{
    if (e->u_applying || n == 0)
        return;

    /* Grow the newest record when this is the next typed character. */
    if (!chain && e->u_coalesce && e->undo_cap != 0 &&
        e->u_tail == e->u_top && e->u_tail > e->u_head && one_char(s, n) &&
        s[0] != '\n') {
        urec_t r;
        uint32_t total = ring_get_u32(e, e->u_tail - EDIT_U_FOOT);
        if (total >= EDIT_U_OVERHEAD && (uint64_t)total <= e->u_tail - e->u_head) {
            rec_at(e, e->u_tail - total, &r);
            if (r.kind == EDIT_U_INSERT && r.total == total && r.off + r.len == off &&
                ring_reserve(e, n, r.start)) {
                ring_put(e, r.start + EDIT_U_HDR + r.len, s, n);
                ring_put_u32(e, r.start + 8u, r.len + n);
                ring_put_u32(e, r.start + EDIT_U_HDR + r.len + n, r.total + n);
                e->u_top += n;
                e->u_tail = e->u_top;
                return;
            }
        }
    }

    push(e, (uint8_t)EDIT_U_INSERT, off, s, 0, n, chain);
    /* Only a lone character may start a coalescing run; a paste must not
     * swallow the characters typed after it. */
    e->u_coalesce = (!chain && one_char(s, n) && s[0] != '\n');
}

void eu_record_delete(struct edit *e, uint32_t off, uint32_t n, bool chain)
{
    if (e->u_applying || n == 0)
        return;
    push(e, (uint8_t)EDIT_U_DELETE, off, NULL, off, n, chain);
    e->u_coalesce = false;
}

/* Re-insert a record's text at the cursor, in ring-order chunks. */
static void insert_from_ring(struct edit *e, const urec_t *r)
{
    uint8_t tmp[64];
    uint32_t done = 0;

    while (done < r->len) {
        uint32_t c = r->len - done;
        if (c > sizeof tmp)
            c = (uint32_t)sizeof tmp;
        ring_get(e, r->start + EDIT_U_HDR + done, tmp, c);
        eb_insert_raw(e, (const char *)tmp, c);
        done += c;
    }
}

/*
 * One undo/redo step, group included.  Returns false when there is nothing
 * left; the public wrappers in edit_core.c turn that into EDIT_E_STATE and
 * own the dirty flags, the modified flag and the cursor follow.
 */
bool eu_undo_step(struct edit *e)
{
    bool applied = false;

    if (e->u_tail == e->u_head)
        return false;

    e->u_applying = true;
    for (;;) {
        urec_t r;
        uint32_t total = ring_get_u32(e, e->u_tail - EDIT_U_FOOT);
        /* Corrupt ring: drop the history rather than return "did something"
         * without moving u_tail — a caller looping until EDIT_E_STATE would
         * never come back. */
        if (total < EDIT_U_OVERHEAD || (uint64_t)total > e->u_tail - e->u_head) {
            eu_reset(e);
            break;
        }
        rec_at(e, e->u_tail - total, &r);
        if (r.total != total) {
            eu_reset(e);
            break;
        }

        e->cur      = r.off;
        e->cur_line = eb_find_line(e, r.off);
        e->cur_col_ok = false;
        if (r.kind == EDIT_U_INSERT)
            eb_delete_raw(e, r.off, r.off + r.len);
        else
            insert_from_ring(e, &r);

        e->u_tail = r.start;
        applied = true;
        if (!(r.flags & EDIT_U_CHAIN) || e->u_tail == e->u_head)
            break;
    }
    e->u_applying = false;
    e->u_coalesce = false;
    return applied;
}

bool eu_redo_step(struct edit *e)
{
    bool applied = false;

    if (e->u_tail == e->u_top)
        return false;

    e->u_applying = true;
    for (;;) {
        urec_t r;
        rec_at(e, e->u_tail, &r);
        if (r.total < EDIT_U_OVERHEAD || (uint64_t)r.total > e->u_top - e->u_tail) {
            e->u_top = e->u_tail;              /* corrupt: drop the redo tail */
            break;
        }

        e->cur      = r.off;
        e->cur_line = eb_find_line(e, r.off);
        e->cur_col_ok = false;
        if (r.kind == EDIT_U_INSERT)
            insert_from_ring(e, &r);
        else
            eb_delete_raw(e, r.off, r.off + r.len);

        e->u_tail = r.start + r.total;
        applied = true;
        if (e->u_tail == e->u_top)
            break;
        {
            urec_t nx;
            rec_at(e, e->u_tail, &nx);
            if (!(nx.flags & EDIT_U_CHAIN))
                break;
        }
    }
    e->u_applying = false;
    e->u_coalesce = false;
    return applied;
}

/* Invariant 5: the record boundaries add up, head <= tail <= top, every
 * footer agrees with its header, and the ring holds no more than its cap. */
bool eu_walk(const struct edit *e)
{
    uint64_t p;

    if (!(e->u_head <= e->u_tail && e->u_tail <= e->u_top))
        return false;
    if (e->u_top - e->u_head > (uint64_t)e->undo_cap)
        return false;
    if (e->undo_cap == 0)
        return e->u_head == e->u_top;

    p = e->u_head;
    while (p < e->u_top) {
        urec_t r;
        rec_at(e, p, &r);
        if (r.kind != EDIT_U_INSERT && r.kind != EDIT_U_DELETE)
            return false;
        if ((r.flags & ~(uint8_t)EDIT_U_CHAIN) != 0)
            return false;
        if (r.total < EDIT_U_OVERHEAD || (uint64_t)r.total > e->u_top - p)
            return false;
        if (ring_get_u32(e, p + EDIT_U_HDR + r.len) != r.total)
            return false;
        p += r.total;
    }
    if (p != e->u_top)
        return false;

    /* The undo point has to sit on a record boundary too. */
    p = e->u_head;
    while (p < e->u_tail) {
        urec_t r;
        rec_at(e, p, &r);
        p += r.total;
    }
    return p == e->u_tail;
}
