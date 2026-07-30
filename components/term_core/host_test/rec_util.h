/*
 * rec_util.h — shared scaffolding for the phase-5 recording suites.
 *
 * Phase 5 is the only feature in this component whose failure mode is a
 * PRIVACY INCIDENT rather than a glitch, so the observation apparatus is built
 * to answer one question without trusting any bookkeeping: "is this text
 * anywhere in the black box region, at all?"
 *
 * Three things this adds on top of pipe_util.h (registry + fakes) and
 * lp_util.h (the ring's own helpers):
 *
 *   - p5_region_hits(): a byte search of the WHOLE live LP region, headers,
 *     writer table, both partitions and the dead space behind `head`
 *     included. term_registry.h R5 claims that with recording off there is no
 *     route from a term to this memory; a check of the record LIST could not
 *     tell that apart from a route that writes and then loses the record, and
 *     a stale write is still a plaintext leak to whoever holds the signing
 *     key (R4). So the assertion is about the bytes, not the index.
 *
 *   - p5_mark(): everything countable at one instant — per-class live record
 *     counts and payload bytes out of the ring, the ring's own
 *     appended/evicted per partition, and term_registry_stats_t. Deltas over
 *     a marked interval are how "this event and only this event moved that
 *     counter" is decided. p5_no_eviction() guards the one case where a live
 *     count delta would lie (records evicted inside the interval).
 *
 *   - p5_walk(): the layout walk of lp_util.h, extended for what phase 5 put
 *     in the format — TERM_LP_CLASS_TERM records living in the APP partition
 *     and TERM_LP_F_SCREEN in the flag nibble. lp_util.h's lp_walk() rejects
 *     both by construction (it predates the class), so phase 5 needs its own
 *     rather than a loosened shared one: the phase-3/4 suites should keep
 *     failing if an APP partition of theirs ever grows a class-2 record.
 *
 * THE RING IS PROCESS-WIDE, THE REGISTRY IS NOT. term_lp_ring_boot() formats
 * the .bss region once per process and every suite here shares it, while
 * reg_boot()/reg_shutdown() build and destroy a registry per case. Four
 * consequences the cases below live with deliberately:
 *   - every count is a DELTA across p5_mark(), never an absolute;
 *   - every needle is unique per case, because a needle a previous case
 *     legitimately recorded would still be in the region;
 *   - term_registry_stats_t belongs to the TABLE and starts again from zero
 *     with it, so a p5_mark() taken before a reg_boot() must not be compared
 *     against one taken after (the ring half of a mark still may be);
 *   - the ring's writer table interns 8 names for the life of the PROCESS
 *     (term_lp_ring.h P2), so a case that asserts an attribution has to run
 *     before its suite has used the table up.
 *
 * Written against term_registry.h (the RECORDING block, R1-R5), term_core.h
 * (term_record_fn, term_core_screen_hash), term_lp_ring.h (P3, RECORDING
 * MODE), PHASE5_MANIFEST.md and docs/term-design.md §4.4 only.
 */
#ifndef TERM_REC_UTIL_H
#define TERM_REC_UTIL_H

#include "pipe_util.h"
#include "lp_util.h"

/* ===================================================================== */
/* The live ring                                                         */
/* ===================================================================== */

/* term_lp_ring.h: every entry point in "The device singleton" boots the ring
 * first, so this is also the way a suite makes sure a region exists before it
 * takes its first mark. */
static inline const term_lp_ring_t *p5_live(void)
{
    return term_lp_source(TERM_LP_SRC_LIVE);
}

/* ===================================================================== */
/* The byte search (R5 / P3: "no route", not "no record")                */
/* ===================================================================== */

/*
 * Occurrences of `needle` (NUL-terminated, matched as raw bytes) anywhere in
 * the live region. Counts non-overlapping hits and searches every byte of it,
 * including the two header slots, the writer table and the bytes behind each
 * partition's head that no record refers to any more.
 */
static inline unsigned p5_region_hits_n(const void *needle, size_t nlen)
{
    const term_lp_ring_t *r = p5_live();
    const uint8_t *hay;
    const uint8_t *nd = (const uint8_t *)needle;
    size_t i, n;
    unsigned hits = 0;
    if (!r || !r->base || nlen == 0 || r->bytes < nlen) return 0;
    hay = r->base;
    n = r->bytes;
    for (i = 0; i + nlen <= n; ) {
        if (memcmp(hay + i, nd, nlen) == 0) { hits++; i += nlen; }
        else i++;
    }
    return hits;
}

static inline unsigned p5_region_hits(const char *needle)
{
    return p5_region_hits_n(needle, needle ? strlen(needle) : 0);
}

/* ===================================================================== */
/* Records, decoded                                                      */
/* ===================================================================== */

/* Room for a couple of full screen captures plus the traffic around them. */
#define P5_MAX_RECS 600

typedef struct {
    int             part;                       /* TERM_LP_PART_*          */
    term_lp_class_t cls;
    unsigned        flags;
    uint32_t        t_ms;
    uint32_t        index;
    uint16_t        len;
    char            writer[TERM_LP_WRITER_MAX];
    char            text[TERM_LP_REC_MAX + 1];
} p5_rec_t;

typedef struct {
    unsigned n;
    unsigned overflow;
    p5_rec_t r[P5_MAX_RECS];
} p5_bag_t;

/* Far too big for a stack frame: a suite declares one as a file-scope
 * static, exactly as lp_util.h's bags are used. */

static inline void p5_collect_part(p5_bag_t *g, int part, int append)
{
    const term_lp_ring_t *r = p5_live();
    term_lp_iter_t it;
    term_lp_record_t rec;
    if (!append) memset(g, 0, sizeof *g);
    if (!r) return;
    term_lp_iter_begin(&it, r, part);
    while (term_lp_iter_next(&it, &rec)) {
        p5_rec_t *d;
        size_t wn;
        if (g->n >= P5_MAX_RECS) { g->overflow++; continue; }
        d = &g->r[g->n];
        memset(d, 0, sizeof *d);
        d->part  = part;
        d->cls   = rec.cls;
        d->flags = rec.flags;
        d->t_ms  = rec.t_ms;
        d->index = rec.index;
        d->len   = rec.len;
        if (rec.writer) {
            wn = t_strnlen(rec.writer, TERM_LP_WRITER_MAX);
            if (wn > TERM_LP_WRITER_MAX - 1u) wn = TERM_LP_WRITER_MAX - 1u;
            memcpy(d->writer, rec.writer, wn);
        }
        if (rec.text && rec.len) {
            size_t tn = rec.len < TERM_LP_REC_MAX ? rec.len : TERM_LP_REC_MAX;
            memcpy(d->text, rec.text, tn);
        }
        g->n++;
    }
}

/* Both partitions, SYS first, each record tagged with where it came from. */
static inline void p5_collect(p5_bag_t *g)
{
    p5_collect_part(g, TERM_LP_PART_SYS, 0);
    p5_collect_part(g, TERM_LP_PART_APP, 1);
}

/* Is this record one of a recorded session's? */
static inline int p5_is_ses(const p5_rec_t *d)
{
    return d->cls == TERM_LP_CLASS_TERM && !(d->flags & TERM_LP_F_SCREEN);
}

static inline int p5_is_scr(const p5_rec_t *d)
{
    return d->cls == TERM_LP_CLASS_TERM && (d->flags & TERM_LP_F_SCREEN) != 0;
}

/* The i-th record of the bag that satisfies `pred`, or NULL. */
static inline const p5_rec_t *p5_nth(const p5_bag_t *g,
                                     int (*pred)(const p5_rec_t *), unsigned i)
{
    unsigned k, seen = 0;
    for (k = 0; k < g->n; k++) {
        if (!pred(&g->r[k])) continue;
        if (seen++ == i) return &g->r[k];
    }
    return NULL;
}

/* Text of the i-th matching record, or "" — never NULL, so a CHK_STR of a
 * record that never landed prints a readable mismatch. */
static inline const char *p5_nth_text(const p5_bag_t *g,
                                      int (*pred)(const p5_rec_t *), unsigned i)
{
    const p5_rec_t *d = p5_nth(g, pred, i);
    return d ? d->text : "";
}

static inline unsigned p5_bag_count(const p5_bag_t *g, int (*pred)(const p5_rec_t *))
{
    unsigned k, n = 0;
    for (k = 0; k < g->n; k++) if (pred(&g->r[k])) n++;
    return n;
}

/* How many records of the bag contain `needle` in their payload. */
static inline unsigned p5_bag_with(const p5_bag_t *g, const char *needle)
{
    unsigned k, n = 0;
    for (k = 0; k < g->n; k++)
        if (needle[0] && strstr(g->r[k].text, needle)) n++;
    return n;
}

/*
 * A capture is "a marker record plus one record per row, all tagged
 * TERM_LP_CLASS_TERM | TERM_LP_F_SCREEN" (term_registry.h R3). The marker's
 * TEXT is not in the header; PHASE5_MANIFEST.md's "Reading it back on the PC"
 * fixes its shape as "--- screen <trigger> <cols>x<rows> rows=<n> ---",
 * because tools/bb_pull.py --session splits a transcript on exactly that. So
 * the structure below is asserted from the header and the prefix is asserted
 * once, from the ledger.
 */
#define P5_MARKER_PREFIX "--- screen "

static inline int p5_is_marker(const p5_rec_t *d)
{
    return p5_is_scr(d) &&
           strncmp(d->text, P5_MARKER_PREFIX, strlen(P5_MARKER_PREFIX)) == 0;
}

/* ===================================================================== */
/* One instant, fully counted                                            */
/* ===================================================================== */

typedef struct {
    /* live records, by what phase 5 cares about */
    long ses, scr, app, sys;
    long ses_bytes, scr_bytes;
    /* R1's structural claim about the partitions: a TERM record can only
     * ever be in APP (term_lp_ring.h "RECORDING MODE"). */
    long term_in_sys;
    /* the ring's own monotonic bookkeeping, which eviction cannot rewind */
    long appended[TERM_LP_PART_COUNT];
    long evicted[TERM_LP_PART_COUNT];
    long refused;
    term_registry_stats_t rs;
} p5_mark_t;

static inline void p5_mark(p5_mark_t *m)
{
    static p5_bag_t bag;   /* one shared scratch bag: p5_mark is never
                            * called from inside a p5_collect walk */
    term_lp_stats_t ls;
    unsigned k;
    int p;

    memset(m, 0, sizeof *m);
    memset(&ls, 0, sizeof ls);
    term_lp_stats(&ls);
    for (p = 0; p < TERM_LP_PART_COUNT; p++) {
        m->appended[p] = (long)ls.appended[p];
        m->evicted[p]  = (long)ls.evicted[p];
    }
    m->refused = (long)ls.refused;
    term_registry_stats(&m->rs);

    p5_collect(&bag);
    for (k = 0; k < bag.n; k++) {
        const p5_rec_t *d = &bag.r[k];
        if (p5_is_ses(d)) { m->ses++; m->ses_bytes += d->len; }
        else if (p5_is_scr(d)) { m->scr++; m->scr_bytes += d->len; }
        else if (d->cls == TERM_LP_CLASS_APP) m->app++;
        else if (d->cls == TERM_LP_CLASS_SYS) m->sys++;
        if (d->cls == TERM_LP_CLASS_TERM && d->part == TERM_LP_PART_SYS)
            m->term_in_sys++;
    }
}

/* True when no record was evicted between the two marks, i.e. when a live
 * record-count delta is the whole truth about what was appended. */
static inline int p5_no_eviction(const p5_mark_t *a, const p5_mark_t *b)
{
    int p;
    for (p = 0; p < TERM_LP_PART_COUNT; p++)
        if (b->evicted[p] != a->evicted[p]) return 0;
    return 1;
}

/* Shorthands for the deltas the cases assert on. */
#define P5_D(field)  (b.field - a.field)
#define P5_DS(field) ((long)b.rs.field - (long)a.rs.field)

/* ===================================================================== */
/* The nine rec_* counters, as one vector                                */
/* ===================================================================== */

/*
 * term_registry.h documents nine counters and what each one means. "The
 * counter for this event moved" is only half the claim a suite should make;
 * the other half is "and none of the other eight did", which is what catches
 * a counter bumped on the wrong edge. So the deltas travel as a vector and
 * every case states the whole vector, with P5_ANY where the number is not the
 * point of that case.
 */
#define P5_ANY (-1L)

typedef struct {
    long on, off, cleared, lines, screens, rows, deferred, trunc, panic;
} p5_recd_t;

static inline p5_recd_t p5_recd(const p5_mark_t *a, const p5_mark_t *b)
{
    p5_recd_t d;
    d.on       = (long)b->rs.rec_on            - (long)a->rs.rec_on;
    d.off      = (long)b->rs.rec_off           - (long)a->rs.rec_off;
    d.cleared  = (long)b->rs.rec_cleared       - (long)a->rs.rec_cleared;
    d.lines    = (long)b->rs.rec_lines         - (long)a->rs.rec_lines;
    d.screens  = (long)b->rs.rec_screens       - (long)a->rs.rec_screens;
    d.rows     = (long)b->rs.rec_screen_rows   - (long)a->rs.rec_screen_rows;
    d.deferred = (long)b->rs.rec_snaps_deferred- (long)a->rs.rec_snaps_deferred;
    d.trunc    = (long)b->rs.rec_screen_trunc  - (long)a->rs.rec_screen_trunc;
    d.panic    = (long)b->rs.rec_panic_rows    - (long)a->rs.rec_panic_rows;
    return d;
}

/* All nine at once. `want` fields of P5_ANY are not checked. */
static inline void p5_chk_recd(const p5_recd_t *got, const p5_recd_t *want,
                               const char *what, const char *file, int line)
{
    static const char *names[9] = { "rec_on", "rec_off", "rec_cleared",
        "rec_lines", "rec_screens", "rec_screen_rows", "rec_snaps_deferred",
        "rec_screen_trunc", "rec_panic_rows" };
    const long *g = &got->on;
    const long *w = &want->on;
    int i;
    for (i = 0; i < 9; i++) {
        if (w[i] == P5_ANY) continue;
        t_checks++;
        if (g[i] != w[i]) {
            t_head(file, line);
            printf("     %s: delta of %s\n     expected: %ld\n     actual  : %ld\n",
                   what, names[i], w[i], g[i]);
        }
    }
}

/* Usage: P5_CHK_RECD(&a, &b, "close", (p5_recd_t){...}) — the braces are the
 * nine fields in declaration order. */
#define P5_CHK_RECD(pa, pb, what, ...)                                        \
    do {                                                                      \
        p5_recd_t got_ = p5_recd((pa), (pb));                                 \
        p5_recd_t want_ = __VA_ARGS__;                                        \
        p5_chk_recd(&got_, &want_, (what), __FILE__, __LINE__);               \
    } while (0)

/* Every counter still: the shape of "this did nothing at all". */
#define P5_RECD_ZERO ((p5_recd_t){ 0,0,0,0,0,0,0,0,0 })

/* Every rec_* counter of term_registry_stats_t, as one comparison: 1 when
 * the nine are byte-identical between two marks. */
static inline int p5_rec_stats_unchanged(const p5_mark_t *a, const p5_mark_t *b)
{
    return a->rs.rec_on == b->rs.rec_on &&
           a->rs.rec_off == b->rs.rec_off &&
           a->rs.rec_cleared == b->rs.rec_cleared &&
           a->rs.rec_lines == b->rs.rec_lines &&
           a->rs.rec_screens == b->rs.rec_screens &&
           a->rs.rec_screen_rows == b->rs.rec_screen_rows &&
           a->rs.rec_snaps_deferred == b->rs.rec_snaps_deferred &&
           a->rs.rec_screen_trunc == b->rs.rec_screen_trunc &&
           a->rs.rec_panic_rows == b->rs.rec_panic_rows;
}

static inline void p5_print_rec_stats(const char *tag, const p5_mark_t *m)
{
    printf("     %s: on=%lu off=%lu cleared=%lu lines=%lu screens=%lu "
           "rows=%lu deferred=%lu trunc=%lu panic=%lu\n", tag,
           (unsigned long)m->rs.rec_on, (unsigned long)m->rs.rec_off,
           (unsigned long)m->rs.rec_cleared, (unsigned long)m->rs.rec_lines,
           (unsigned long)m->rs.rec_screens, (unsigned long)m->rs.rec_screen_rows,
           (unsigned long)m->rs.rec_snaps_deferred,
           (unsigned long)m->rs.rec_screen_trunc,
           (unsigned long)m->rs.rec_panic_rows);
}

/* ===================================================================== */
/* The layout walk, phase-5 aware                                        */
/* ===================================================================== */

/*
 * lp_util.h's lp_walk() with two admissions phase 5 made to the on-memory
 * format (term_lp_ring.h "RECORDING MODE"):
 *
 *   - TERM_LP_CLASS_TERM is legal in the APP partition (and ONLY there: SYS
 *     stays structurally out of reach of session content, which is the
 *     anti-eviction-DoS reserve of P1);
 *   - TERM_LP_F_SCREEN is a legal flag, and only on a class-TERM record
 *     ("Only ever set together with TERM_LP_CLASS_TERM").
 *
 * Everything else is unchanged: 8-byte alignment, len <= REC_MAX, no record
 * straddling a partition end, a PAD filling the remainder, the chain from
 * `tail` consuming exactly `used` bytes and landing on `head`, and the
 * content count matching the descriptor.
 */
static inline int p5_walk(const uint8_t *base, const term_lp_hdr_t *h, int part,
                          unsigned *out_content, unsigned *out_pads,
                          char *why, size_t whyn)
{
    const term_lp_part_t *p = &h->part[part];
    uint32_t off, left;
    unsigned content = 0, pads = 0;

    if (out_content) *out_content = 0;
    if (out_pads) *out_pads = 0;
    why[0] = 0;

    if (p->off != lp_part_off(part) || p->cap != lp_part_cap(part)) {
        snprintf(why, whyn, "part %d descriptor off/cap = %lu/%lu, layout says %lu/%lu",
                 part, (unsigned long)p->off, (unsigned long)p->cap,
                 (unsigned long)lp_part_off(part), (unsigned long)lp_part_cap(part));
        return 1;
    }
    if (p->head >= p->cap || p->tail >= p->cap || p->used > p->cap ||
        (p->head % TERM_LP_REC_ALIGN) || (p->tail % TERM_LP_REC_ALIGN) ||
        (p->used % TERM_LP_REC_ALIGN)) {
        snprintf(why, whyn, "part %d head/tail/used = %lu/%lu/%lu (cap %lu)", part,
                 (unsigned long)p->head, (unsigned long)p->tail,
                 (unsigned long)p->used, (unsigned long)p->cap);
        return 1;
    }
    if (p->records + p->evicted != p->appended) {
        snprintf(why, whyn, "part %d records+evicted != appended: %lu+%lu vs %lu",
                 part, (unsigned long)p->records, (unsigned long)p->evicted,
                 (unsigned long)p->appended);
        return 1;
    }

    off = p->tail;
    left = p->used;
    while (left > 0) {
        term_lp_rec_t rec;
        uint32_t size;
        unsigned cls, flags;

        if ((off % TERM_LP_REC_ALIGN) || off + TERM_LP_REC_HDR > p->cap) {
            snprintf(why, whyn, "part %d bad record offset %lu (cap %lu)", part,
                     (unsigned long)off, (unsigned long)p->cap);
            return 1;
        }
        memcpy(&rec, base + p->off + off, sizeof rec);
        cls = (unsigned)TERM_LP_CLASS_OF(rec.cls);
        flags = TERM_LP_FLAGS_OF(rec.cls);
        if (rec.len > TERM_LP_REC_MAX) {
            snprintf(why, whyn, "part %d record at %lu has len %u > %u", part,
                     (unsigned long)off, rec.len, TERM_LP_REC_MAX);
            return 1;
        }
        if (flags & ~(unsigned)(TERM_LP_F_TRUNC | TERM_LP_F_SCREEN)) {
            snprintf(why, whyn, "part %d record at %lu has unknown flags %02X",
                     part, (unsigned long)off, flags);
            return 1;
        }
        if ((flags & TERM_LP_F_SCREEN) && cls != (unsigned)TERM_LP_CLASS_TERM) {
            snprintf(why, whyn,
                     "part %d record at %lu is F_SCREEN but class %u, not TERM",
                     part, (unsigned long)off, cls);
            return 1;
        }
        size = TERM_LP_REC_HDR + ((uint32_t)rec.len + TERM_LP_REC_ALIGN - 1u) /
                                 TERM_LP_REC_ALIGN * TERM_LP_REC_ALIGN;
        if (size > left || off + size > p->cap) {
            snprintf(why, whyn, "part %d record at %lu (%lu bytes) overruns "
                     "used=%lu / cap=%lu", part, (unsigned long)off,
                     (unsigned long)size, (unsigned long)left,
                     (unsigned long)p->cap);
            return 1;
        }
        if (cls == (unsigned)TERM_LP_CLASS_PAD) {
            pads++;
            if (off + size != p->cap ||
                (uint32_t)rec.len != p->cap - off - TERM_LP_REC_HDR) {
                snprintf(why, whyn, "part %d PAD at %lu does not fill to the end",
                         part, (unsigned long)off);
                return 1;
            }
        } else if (cls == (unsigned)lp_part_class(part) ||
                   (cls == (unsigned)TERM_LP_CLASS_TERM &&
                    part == TERM_LP_PART_APP)) {
            content++;
            if (rec.len == 0) {
                snprintf(why, whyn, "part %d content record at %lu has len 0",
                         part, (unsigned long)off);
                return 1;
            }
        } else {
            snprintf(why, whyn, "part %d holds a record of class %u at %lu", part,
                     cls, (unsigned long)off);
            return 1;
        }
        off += size;
        if (off == p->cap) off = 0;
        left -= size;
    }
    if (off != p->head) {
        snprintf(why, whyn, "part %d chain ended at %lu, header says head %lu",
                 part, (unsigned long)off, (unsigned long)p->head);
        return 1;
    }
    if (content != p->records) {
        snprintf(why, whyn, "part %d chain holds %u content records, header says %lu",
                 part, content, (unsigned long)p->records);
        return 1;
    }
    if (out_content) *out_content = content;
    if (out_pads) *out_pads = pads;
    return 0;
}

/*
 * Walk both partitions of the LIVE region as a reader would: pick the
 * validating header slot with the greatest hseq, then check the chains. This
 * is the "TERM_LP_CLASS_TERM survives a reader's structural walk" assertion
 * (term_lp_ring.h COMPATIBILITY: a build that does not know class 2 rejects
 * the whole image, so a build that does must accept it).
 */
static inline void p5_check_live_layout(void)
{
    const term_lp_ring_t *r = p5_live();
    term_lp_hdr_t h[TERM_LP_HDR_SLOTS];
    int best = -1;
    unsigned i;
    char why[256];
    int part;

    t_checks++;
    if (!r || !r->base) {
        t_head(__FILE__, __LINE__);
        printf("     no live LP region to walk\n");
        return;
    }
    for (i = 0; i < TERM_LP_HDR_SLOTS; i++) {
        memcpy(&h[i], r->base + (size_t)i * TERM_LP_HDR_BYTES, sizeof h[i]);
        if (h[i].magic != TERM_LP_MAGIC) continue;
        if (h[i].crc != lp_crc32(&h[i], LP_CRC_COVER)) continue;
        if (best < 0 || h[i].hseq > h[best].hseq) best = (int)i;
    }
    if (best < 0) {
        t_head(__FILE__, __LINE__);
        printf("     no valid header slot in the live region\n");
        return;
    }
    for (part = 0; part < TERM_LP_PART_COUNT; part++) {
        t_checks++;
        if (p5_walk(r->base, &h[best], part, NULL, NULL, why, sizeof why) != 0) {
            t_head(__FILE__, __LINE__);
            printf("     live layout invariant broken: %s\n", why);
        }
    }
}

/* ===================================================================== */
/* Driving a term                                                        */
/* ===================================================================== */

/* Recording state as the header's ONE introspection point reports it. */
static inline int p5_recording(term_id_t id, const char *owner)
{
    return term_registry_recording(id, owner);
}

/* ...and as term_slot_info_t reports it, which is the only reader that still
 * works once the slot is DYING. term_registry.h documents both; a UI reads
 * the first, a lifecycle test needs the second. */
static inline int p5_info_recording(term_id_t id)
{
    term_slot_info_t inf;
    memset(&inf, 0, sizeof inf);
    if (term_registry_info(id, &inf) != TERM_OK) return -1;
    return inf.recording ? 1 : 0;
}

/* Feed and drain to completion. `frames` frames of drain with no clock
 * movement, so nothing can settle underneath the feed (§5's budget means one
 * frame may not be enough for a screen's worth of bytes). */
static inline void p5_feed_frames(term_id_t id, const char *owner,
                                 const char *s, int frames)
{
    int i;
    (void)term_registry_feed(id, owner, (const uint8_t *)s, strlen(s), NULL);
    for (i = 0; i < frames; i++) reg_frame();
}

static inline void p5_feed(term_id_t id, const char *owner, const char *s)
{
    p5_feed_frames(id, owner, s, 8);
}

/* `n` numbered lines, each one scrolling the previous ones up: the shape that
 * makes the line tee of R2 fire. */
static inline void p5_feed_lines(term_id_t id, const char *owner,
                                 const char *tag, int from, int n)
{
    char buf[4096];
    int i;
    size_t o = 0;
    buf[0] = 0;
    for (i = from; i < from + n; i++) {
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%03d\r\n", tag, i);
        if (o > sizeof buf - 64) { p5_feed(id, owner, buf); o = 0; buf[0] = 0; }
    }
    if (o) p5_feed(id, owner, buf);
}

/*
 * One `top`-shaped in-place repaint: home, then every row addressed
 * explicitly, painted and erased to end of line, with NO newline anywhere. A
 * stream of these changes every cell on the screen and must archive not one
 * row (R2 / PHASE5_MANIFEST decision 1: "in-place painting archives nothing").
 */
static inline void p5_repaint(term_id_t id, const char *owner, int rows,
                              int gen, const char *tag)
{
    char buf[8192];
    size_t o = 0;
    int r;
    o += (size_t)snprintf(buf + o, sizeof buf - o, "\x1b[H");
    for (r = 1; r <= rows && o < sizeof buf - 128; r++)
        o += (size_t)snprintf(buf + o, sizeof buf - o,
                              "\x1b[%d;1H%s-r%02d-g%04d\x1b[K", r, tag, r, gen);
    p5_feed_frames(id, owner, buf, 4);
}

/* Advance the fake clock and give the UI task the frames it needs to notice:
 * the settle-debounced capture of R3(b) is evaluated by the drain, so time
 * passing without a frame cannot produce one. */
static inline void p5_quiet(int64_t ms, int frames)
{
    int i;
    fp_advance(ms);
    for (i = 0; i < frames; i++) reg_frame();
}

/* Long enough for the documented settle interval, plus a margin, in one
 * jump — the debounce is "nothing fed for TERM_REC_SETTLE_MS". */
static inline void p5_settle(void)
{
    p5_quiet((int64_t)TERM_REC_SETTLE_MS + 50, 2);
}

/* Enough quiet for the rate limit of R3(b) to open again as well. */
static inline void p5_settle_after_rate_limit(void)
{
    p5_quiet((int64_t)TERM_REC_SNAP_MIN_MS + (int64_t)TERM_REC_SETTLE_MS + 50, 2);
}

/* A VT term big enough to matter but small enough that a capture is a
 * handful of records. */
static inline term_id_t p5_new_vt(const char *name, const char *owner,
                                  int cols, int rows)
{
    return reg_new(name, owner, TERM_VT, false, cols, rows);
}

/* ===================================================================== */
/* What the glass shows, for comparison against a capture                */
/* ===================================================================== */

#define P5_MAX_LINES 64

typedef struct {
    int  n;
    char line[P5_MAX_LINES][TERM_LP_REC_MAX + 1];
} p5_lines_t;

/*
 * The rows of the visible screen as a capture must hold them: R3 says "one
 * record per row", so the list runs from row 0 to the last row with anything on
 * it, and trailing blank rows are not rows anybody displayed.
 *
 * A BLANK ROW IN THE MIDDLE is rendered as a single space. That is not in the
 * header; it is the only way the header's two rules can both hold, since
 * term_lp_ring_append refuses an empty payload and dropping the row instead
 * would shift every row below it in a transcript. Observed, and pinned here
 * because a change to it changes what a pulled transcript looks like.
 */
static inline void p5_screen_lines(p5_lines_t *out, term_id_t id, const char *owner)
{
    const char *s = reg_snap(id, owner);
    size_t i = 0, o = 0;
    int rows = 0, last_nonempty = 0, r;
    memset(out, 0, sizeof *out);
    for (;;) {
        if (s[i] == '\n' || s[i] == 0) {
            if (rows < P5_MAX_LINES) {
                if (o > 0) last_nonempty = rows + 1;
                rows++;
            }
            o = 0;
            if (s[i] == 0) break;
            i++;
            continue;
        }
        if (rows < P5_MAX_LINES && o < TERM_LP_REC_MAX)
            out->line[rows][o++] = s[i];
        i++;
    }
    out->n = last_nonempty;
    for (r = 0; r < out->n; r++)
        if (out->line[r][0] == 0) strcpy(out->line[r], " ");
}

/* Is `core`'s recording tee installed? R5 says recording on/off IS that
 * pointer, so this is the structural half of the claim — read through the one
 * legitimate route a test has to a live core, the ui_visit callback. */
static int  p5_visit_recording;   /* -2 = the callback never ran */
static int  p5_visit_count;

static inline void p5_visit_cb(term_id_t id, term_core_t *core,
                               const term_view_t *view, void *user)
{
    (void)id; (void)view; (void)user;
    p5_visit_count++;
    if (core) {
        p5_visit_recording = (int)term_core_recording(core);
        term_core_dirty_clear(core);
    }
}

/* The tee state of the ONE visible term, or -2 when nothing was painted. */
static inline int p5_core_recording(void)
{
    p5_visit_recording = -2;
    p5_visit_count = 0;
    (void)term_registry_ui_visit(p5_visit_cb, NULL);
    return p5_visit_recording;
}

/* Make a term paintable, so p5_core_recording() can reach its core. */
static inline void p5_show(term_id_t id, const char *owner)
{
    term_view_t v;
    memset(&v, 0, sizeof v);
    v.x = 0; v.y = 0; v.w = 400; v.h = 300; v.visible = true;
    (void)term_registry_show(id, owner, &v);
    reg_frame();
}

/* The writer id a recorded term's records must carry: PHASE5_MANIFEST
 * decision 6, "<owner>:<name>", because a transcript of three simultaneously
 * recorded tabs is useless if every line says the app's name.
 *
 * INTERNED, ONE STABLE BUFFER PER DISTINCT owner:name — not one shared static
 * buffer. A caller holds TWO of these results at once and compares them
 * against each other (rec_panic holds the visible and the hidden term's writer
 * to assert the ordering), and with a single buffer the second call silently
 * rewrote the first result: both pointers read the same string, so an ordering
 * assertion between them could not fail and could not pass either. The pointer
 * stays valid for the rest of the process.
 */
static inline const char *p5_writer_of(const char *owner, const char *name)
{
    static char pool[8][TERM_LP_WRITER_MAX * 2];
    static unsigned used, next;
    char buf[TERM_LP_WRITER_MAX * 2];
    unsigned k;

    snprintf(buf, sizeof buf, "%s:%s", owner, name);
    for (k = 0; k < used; k++)
        if (strcmp(pool[k], buf) == 0) return pool[k];
    if (used < 8u) {
        k = used++;
    } else {
        k = next; next = (next + 1u) % 8u;   /* >8 distinct: fall back to LRU-ish
                                              * rotation, which is still never
                                              * two live results in one buffer */
    }
    memcpy(pool[k], buf, sizeof buf);
    return pool[k];
}

#endif /* TERM_REC_UTIL_H */
