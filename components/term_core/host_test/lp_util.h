/*
 * lp_util.h — shared scaffolding for the phase-3 LP black-box ring suites.
 *
 * The ring's pure core is contract-testable against "a plain malloc'd region"
 * (term_lp_ring.h, last paragraph of the file header), so every suite here
 * builds its own region rather than going through the device singleton. What
 * this header adds on top of the phase-1 CHK_* harness:
 *
 *   - a region with red zones, because P6's "appends are sequential,
 *     8-byte-aligned writes" into the region is only half a property: the
 *     other half is that nothing outside the region is ever touched;
 *   - CRC32 over a header slot, so a suite can *craft* a header that is
 *     internally valid but geometrically or structurally wrong. Without this
 *     the individual bits of term_lp_check_t (version_ok / geometry_ok /
 *     chain_ok) cannot be told apart from crc_ok — every hand corruption
 *     would just break the CRC first. lp_crc32_is_the_ring_crc() proves the
 *     polynomial against a freshly formatted region before any suite leans
 *     on it;
 *   - sequence-numbered payloads plus a collector, so "the survivors are the
 *     newest records, in order" is decided by decoding the payloads rather
 *     than by counting;
 *   - lp_walk(): a raw walk of a partition out of the region bytes, checking
 *     the layout invariants the header documents (8-byte alignment, no
 *     straddling record, a PAD to the partition end, `used`/`records`/`head`
 *     agreeing with the chain).
 *
 * Written against term_lp_ring.h, PHASE3_MANIFEST.md §2 and
 * docs/term-design.md §4.4/§7 only.
 */
#ifndef TERM_LP_UTIL_H
#define TERM_LP_UTIL_H

#include "test_util.h"
#include "term_lp_ring.h"

/* ===================================================================== */
/* A region, with red zones around it                                    */
/* ===================================================================== */

#define LP_GUARD      64u
#define LP_GUARD_BYTE 0x5Au

/* An arbitrary but fixed reset reason for formats that do not care; 4 is
 * ESP_RST_PANIC, the cause the black box exists for. */
#define LP_REASON_PANIC 4u

typedef struct {
    uint8_t       *raw;
    uint8_t       *base;      /* 8-byte aligned, `bytes` usable */
    size_t         bytes;
    size_t         raw_bytes;
    term_lp_ring_t r;
} lpreg_t;

static inline void lp_alloc(lpreg_t *g, size_t bytes)
{
    uintptr_t a;
    memset(g, 0, sizeof *g);
    g->raw_bytes = bytes + 2u * LP_GUARD + 8u;
    g->raw = (uint8_t *)malloc(g->raw_bytes);
    if (!g->raw) return;
    memset(g->raw, LP_GUARD_BYTE, g->raw_bytes);
    a = ((uintptr_t)g->raw + LP_GUARD + 7u) & ~(uintptr_t)7u;
    g->base = (uint8_t *)a;
    g->bytes = bytes;
    memset(g->base, 0, bytes);
}

static inline void lp_free(lpreg_t *g)
{
    free(g->raw);
    g->raw = NULL;
    g->base = NULL;
}

/* 1 when both red zones are intact. */
static inline int lp_guards_intact(const lpreg_t *g)
{
    size_t i;
    if (!g->raw) return 1;
    for (i = 0; i < LP_GUARD; i++) {
        if (g->base[-(ptrdiff_t)LP_GUARD + (ptrdiff_t)i] != LP_GUARD_BYTE) return 0;
        if (g->base[g->bytes + i] != LP_GUARD_BYTE) return 0;
    }
    return 1;
}

static inline bool lp_format_full(lpreg_t *g, uint32_t boot_seq, uint8_t reason,
                                  term_lp_prev_t prev, uint32_t prev_boot_seq)
{
    return term_lp_ring_format(&g->r, g->base, g->bytes, boot_seq, reason,
                               prev, prev_boot_seq);
}

static inline bool lp_format(lpreg_t *g, uint32_t boot_seq)
{
    return lp_format_full(g, boot_seq, LP_REASON_PANIC, TERM_LP_PREV_NONE, 0);
}

/* A zeroed region of exactly TERM_LP_REGION_BYTES, formatted. */
static inline bool lp_fresh(lpreg_t *g, uint32_t boot_seq)
{
    lp_alloc(g, TERM_LP_REGION_BYTES);
    if (!g->base) return false;
    return lp_format(g, boot_seq);
}

/* ===================================================================== */
/* Header slots (P5: two public 128-byte structs at the region base)      */
/* ===================================================================== */

/* term_lp_ring.h: "`crc` covers bytes [0, offsetof(crc)) of this struct". */
#define LP_CRC_COVER (offsetof(term_lp_hdr_t, crc))

/* CRC-32 (reflected 0xEDB88320, the one PHASE3_MANIFEST §1 decision 3 pins
 * for this component). lp_crc32_is_the_ring_crc() checks the guess against a
 * real formatted header before any suite crafts one. */
static inline uint32_t lp_crc32(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= b[i];
        for (k = 0; k < 8; k++)
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

static inline void lp_slot_get(const lpreg_t *g, unsigned slot, term_lp_hdr_t *out)
{
    memcpy(out, g->base + (size_t)slot * TERM_LP_HDR_BYTES, sizeof *out);
}

/* Store a header into a slot. `fix_crc` recomputes the CRC so the slot is
 * internally valid — the only way to reach version_ok / geometry_ok /
 * chain_ok without tripping crc_ok first. */
static inline void lp_slot_put(lpreg_t *g, unsigned slot, term_lp_hdr_t *h, int fix_crc)
{
    if (fix_crc) h->crc = lp_crc32(h, LP_CRC_COVER);
    memcpy(g->base + (size_t)slot * TERM_LP_HDR_BYTES, h, sizeof *h);
}

/* The slot a reader must pick: greatest hseq among the slots that carry the
 * magic. -1 when neither does. */
static inline int lp_slot_newest(const lpreg_t *g)
{
    term_lp_hdr_t h;
    unsigned i;
    int best = -1;
    uint32_t best_seq = 0;
    for (i = 0; i < TERM_LP_HDR_SLOTS; i++) {
        lp_slot_get(g, i, &h);
        if (h.magic != TERM_LP_MAGIC) continue;
        if (best < 0 || h.hseq > best_seq) { best = (int)i; best_seq = h.hseq; }
    }
    return best;
}

static inline int lp_slot_oldest(const lpreg_t *g)
{
    int n = lp_slot_newest(g);
    if (n < 0) return -1;
    return n ? 0 : 1;
}

/* Does our CRC32 reproduce what format() wrote? Every crafted-header case
 * depends on this, so each suite that crafts asserts it first. */
static inline int lp_crc32_is_the_ring_crc(const lpreg_t *g)
{
    term_lp_hdr_t h;
    int slot = lp_slot_newest(g);
    if (slot < 0) return 0;
    lp_slot_get(g, (unsigned)slot, &h);
    return h.crc == lp_crc32(&h, LP_CRC_COVER);
}

/* Simulate a torn 128-byte header store: the tail of the slot never landed. */
static inline void lp_tear_slot(lpreg_t *g, unsigned slot, uint8_t fill)
{
    memset(g->base + (size_t)slot * TERM_LP_HDR_BYTES + 64u, fill, 64u);
}

/* ===================================================================== */
/* Payloads that carry their own ordinal                                 */
/* ===================================================================== */

/*
 * "<tag><6 digits>:" then '.' filler out to `want` bytes. The tag says which
 * partition the line was written for and the digits say when, so an eviction
 * order or an iteration order is *decoded*, not inferred.
 */
static inline size_t lp_mkline(char *buf, size_t cap, char tag, unsigned seq,
                               size_t want)
{
    size_t n;
    if (want < 8u) want = 8u;
    if (want > cap - 1u) want = cap - 1u;
    sprintf(buf, "%c%06u:", tag, seq % 1000000u);
    n = strlen(buf);
    while (n < want) buf[n++] = '.';
    buf[n] = 0;
    return n;
}

/* The ordinal a payload carries, or -1 when it is not one of ours. */
static inline long lp_seq_of(const uint8_t *text, size_t len)
{
    long v = 0;
    int i;
    if (len < 8u || text == NULL) return -1;
    if (text[7] != ':') return -1;
    for (i = 1; i <= 6; i++) {
        if (text[i] < '0' || text[i] > '9') return -1;
        v = v * 10 + (text[i] - '0');
    }
    return v;
}

static inline char lp_tag_for(term_lp_class_t cls)
{
    return cls == TERM_LP_CLASS_SYS ? 'S' : 'A';
}

/* One append of a sequence-numbered line of `want` bytes. */
static inline bool lp_put(term_lp_ring_t *r, term_lp_class_t cls,
                          const char *writer, unsigned seq, size_t want,
                          uint32_t t_ms)
{
    char buf[TERM_LP_REC_MAX * 3];
    size_t n = lp_mkline(buf, sizeof buf, lp_tag_for(cls), seq, want);
    return term_lp_ring_append(r, cls, writer, buf, n, t_ms);
}

/* ===================================================================== */
/* Collecting an iteration                                               */
/* ===================================================================== */

/* Smallest record is 8 + align8(1) = 16 bytes, so the APP partition can hold
 * 1472 and the SYS one 512. */
#define LP_MAX_RECS 2100

typedef struct {
    unsigned n;
    unsigned overflow;    /* records the bag could not hold                */
    int      null_writer; /* header: "resolved name, never NULL"           */
    int      out_of_region;
    int      with_esc;    /* P4: no escape byte may survive an append      */
    long     seq[LP_MAX_RECS];
    uint8_t  cls[LP_MAX_RECS];
    unsigned flags[LP_MAX_RECS];
    uint32_t t_ms[LP_MAX_RECS];
    uint32_t index[LP_MAX_RECS];
    uint16_t len[LP_MAX_RECS];
    char     writer[LP_MAX_RECS][TERM_LP_WRITER_MAX];
} lp_recs_t;

/* A bag is far too big for a stack frame: a suite that needs one declares it
 * as its own file-scope static (`static lp_recs_t bagA;`). */

/*
 * Walk `part` (negative = both) and record everything the public
 * term_lp_record_t exposes. `base`/`bytes` (NULL to skip) is the image the
 * text pointers must live inside.
 */
static inline void lp_collect(lp_recs_t *g, const term_lp_ring_t *r, int part,
                             const uint8_t *base, size_t bytes)
{
    term_lp_iter_t it;
    term_lp_record_t rec;
    memset(g, 0, sizeof *g);
    term_lp_iter_begin(&it, r, part);
    while (term_lp_iter_next(&it, &rec)) {
        size_t i;
        if (g->n >= LP_MAX_RECS) { g->overflow++; continue; }
        if (base && rec.text &&
            ((const uint8_t *)rec.text < base ||
             (const uint8_t *)rec.text + rec.len > base + bytes))
            g->out_of_region++;
        for (i = 0; i < rec.len; i++)
            if (rec.text[i] == 0x1b) { g->with_esc++; break; }
        g->seq[g->n]   = lp_seq_of(rec.text, rec.len);
        g->cls[g->n]   = (uint8_t)rec.cls;
        g->flags[g->n] = rec.flags;
        g->t_ms[g->n]  = rec.t_ms;
        g->index[g->n] = rec.index;
        g->len[g->n]   = rec.len;
        if (!rec.writer) {
            g->null_writer++;
            g->writer[g->n][0] = 0;
        } else {
            size_t wn = t_strnlen(rec.writer, TERM_LP_WRITER_MAX);
            if (wn > TERM_LP_WRITER_MAX - 1u) wn = TERM_LP_WRITER_MAX - 1u;
            memcpy(g->writer[g->n], rec.writer, wn);
            g->writer[g->n][wn] = 0;
        }
        g->n++;
    }
}

/* The payload of the i-th record of an iteration, NUL-terminated, in a
 * rotating buffer (for CHK_STR against an expected stripped text). */
static inline const char *lp_text_at(const term_lp_ring_t *r, int part, unsigned want)
{
    static char buf[4][TERM_LP_REC_MAX + 1];
    static int which = 0;
    term_lp_iter_t it;
    term_lp_record_t rec;
    unsigned i = 0;
    which = (which + 1) & 3;
    buf[which][0] = 0;
    term_lp_iter_begin(&it, r, part);
    while (term_lp_iter_next(&it, &rec)) {
        if (i++ != want) continue;
        memcpy(buf[which], rec.text, rec.len);
        buf[which][rec.len] = 0;
        return buf[which];
    }
    return buf[which];
}

static inline unsigned lp_count(const term_lp_ring_t *r, int part)
{
    term_lp_iter_t it;
    term_lp_record_t rec;
    unsigned n = 0;
    term_lp_iter_begin(&it, r, part);
    while (term_lp_iter_next(&it, &rec)) n++;
    return n;
}

/* ===================================================================== */
/* The raw layout walk (item 8: the structural invariants of the layout)  */
/* ===================================================================== */

static inline uint32_t lp_part_off(int part)
{
    return part == TERM_LP_PART_SYS ? TERM_LP_SYS_OFF : TERM_LP_APP_OFF;
}

static inline uint32_t lp_part_cap(int part)
{
    return part == TERM_LP_PART_SYS ? TERM_LP_SYS_BYTES : TERM_LP_APP_BYTES;
}

static inline term_lp_class_t lp_part_class(int part)
{
    return part == TERM_LP_PART_SYS ? TERM_LP_CLASS_SYS : TERM_LP_CLASS_APP;
}

/*
 * Walk one partition out of the raw region bytes, using `h` (the header slot
 * a reader would pick). Returns 0 when every layout invariant the header
 * documents holds, else fills `why`.
 *
 * The invariants, all from term_lp_ring.h's "REGION LAYOUT":
 *   - the descriptor's off/cap are the compile-time constants (P1: static);
 *   - head/tail inside [0,cap), used <= cap, everything 8-byte aligned;
 *   - one record is 8 bytes + `len` payload, rounded up to REC_ALIGN;
 *   - len <= REC_MAX, class one of APP/SYS/PAD, flags only F_TRUNC;
 *   - a content record's class matches its partition (P1's routing);
 *   - "Records never straddle the end of a partition: a PAD record fills the
 *     remainder and the next record starts at offset 0";
 *   - the chain from `tail` consumes exactly `used` bytes and lands on `head`;
 *   - the content record count equals `records` (PAD not counted).
 */
static inline int lp_walk(const uint8_t *base, const term_lp_hdr_t *h, int part,
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
    if (p->cap % TERM_LP_REC_ALIGN) {
        snprintf(why, whyn, "part %d cap %lu not a multiple of %u", part,
                 (unsigned long)p->cap, TERM_LP_REC_ALIGN);
        return 1;
    }
    if (p->head >= p->cap || p->tail >= p->cap || p->used > p->cap) {
        snprintf(why, whyn, "part %d head/tail/used = %lu/%lu/%lu, cap %lu", part,
                 (unsigned long)p->head, (unsigned long)p->tail,
                 (unsigned long)p->used, (unsigned long)p->cap);
        return 1;
    }
    if ((p->head % TERM_LP_REC_ALIGN) || (p->tail % TERM_LP_REC_ALIGN) ||
        (p->used % TERM_LP_REC_ALIGN)) {
        snprintf(why, whyn, "part %d head/tail/used not 8-aligned: %lu/%lu/%lu", part,
                 (unsigned long)p->head, (unsigned long)p->tail,
                 (unsigned long)p->used);
        return 1;
    }
    if (p->records > p->appended || p->evicted > p->appended ||
        p->records + p->evicted != p->appended) {
        snprintf(why, whyn,
                 "part %d records+evicted != appended: %lu+%lu vs %lu", part,
                 (unsigned long)p->records, (unsigned long)p->evicted,
                 (unsigned long)p->appended);
        return 1;
    }

    off = p->tail;
    left = p->used;
    while (left > 0) {
        term_lp_rec_t rec;
        uint32_t size;
        unsigned cls, flags;

        if (off % TERM_LP_REC_ALIGN) {
            snprintf(why, whyn, "part %d record at %lu is not %u-aligned", part,
                     (unsigned long)off, TERM_LP_REC_ALIGN);
            return 1;
        }
        if (off + TERM_LP_REC_HDR > p->cap) {
            snprintf(why, whyn, "part %d record header at %lu straddles cap %lu",
                     part, (unsigned long)off, (unsigned long)p->cap);
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
        if (flags & ~(unsigned)TERM_LP_F_TRUNC) {
            snprintf(why, whyn, "part %d record at %lu has unknown flags %02X",
                     part, (unsigned long)off, flags);
            return 1;
        }
        size = TERM_LP_REC_HDR + ((uint32_t)rec.len + TERM_LP_REC_ALIGN - 1u) /
                                 TERM_LP_REC_ALIGN * TERM_LP_REC_ALIGN;
        if (size > left) {
            snprintf(why, whyn,
                     "part %d record at %lu is %lu bytes, only %lu of `used` left",
                     part, (unsigned long)off, (unsigned long)size,
                     (unsigned long)left);
            return 1;
        }
        if (off + size > p->cap) {
            snprintf(why, whyn, "part %d record at %lu (%lu bytes) straddles cap %lu",
                     part, (unsigned long)off, (unsigned long)size,
                     (unsigned long)p->cap);
            return 1;
        }
        if (cls == (unsigned)TERM_LP_CLASS_PAD) {
            pads++;
            if (off + size != p->cap) {
                snprintf(why, whyn,
                         "part %d PAD at %lu ends at %lu, not at the partition end %lu",
                         part, (unsigned long)off, (unsigned long)(off + size),
                         (unsigned long)p->cap);
                return 1;
            }
            if ((uint32_t)rec.len != p->cap - off - TERM_LP_REC_HDR) {
                snprintf(why, whyn,
                         "part %d PAD at %lu has len %u, remainder needs %lu",
                         part, (unsigned long)off, rec.len,
                         (unsigned long)(p->cap - off - TERM_LP_REC_HDR));
                return 1;
            }
        } else if (cls == (unsigned)lp_part_class(part)) {
            content++;
            if (rec.len == 0) {
                snprintf(why, whyn,
                         "part %d content record at %lu has len 0 (append refuses "
                         "empty text)", part, (unsigned long)off);
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

/* CHK the walk of both partitions of `r`'s image, printing `why` on failure. */
static inline void lp_check_layout(const uint8_t *base, const term_lp_hdr_t *h)
{
    char why[256];
    int part;
    for (part = 0; part < TERM_LP_PART_COUNT; part++) {
        t_checks++;
        if (lp_walk(base, h, part, NULL, NULL, why, sizeof why) != 0) {
            t_head(__FILE__, __LINE__);
            printf("     layout invariant broken: %s\n", why);
        }
    }
}

/* Is every byte of a partition's data area still zero? (P1: the other
 * partition is not merely accounted as empty, it is untouched.) */
static inline int lp_part_untouched(const lpreg_t *g, int part)
{
    uint32_t i, off = lp_part_off(part), cap = lp_part_cap(part);
    for (i = 0; i < cap; i++)
        if (g->base[off + i] != 0) return 0;
    return 1;
}

/* A valid UTF-8 sequence check for the truncation tests (P4: "never
 * mid-codepoint, so a pulled log is always valid UTF-8"). */
static inline int lp_utf8_valid(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if (c < 0x80u) { i++; continue; }
        if ((c & 0xE0u) == 0xC0u) need = 1;
        else if ((c & 0xF0u) == 0xE0u) need = 2;
        else if ((c & 0xF8u) == 0xF0u) need = 3;
        else return 0;
        if (i + need >= n) return 0;   /* a continuation byte is missing */
        {
            size_t k;
            for (k = 1; k <= need; k++)
                if (((unsigned char)s[i + k] & 0xC0u) != 0x80u) return 0;
        }
        i += need + 1u;
    }
    return 1;
}

#endif /* TERM_LP_UTIL_H */
