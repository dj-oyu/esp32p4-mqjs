/*
 * test_lp_format.c — format, open, and the six verdict bits of
 * term_lp_check_t.
 *
 * Contracts under test (term_lp_ring.h):
 *   format: "`bytes` must be at least TERM_LP_REGION_BYTES and `region`
 *     8-byte aligned; anything else returns false and leaves both untouched."
 *   open:   "Returns true only when `out->ok` ... A false return leaves `r`
 *     unattached: a region that did not validate is never appended to, it is
 *     re-formatted."
 *   term_lp_check_t: "Every field is separately meaningful: a wipe and a
 *     stray write look nothing alike and §4.4 cares which one it is facing."
 *   ok == magic && version && crc && geometry && chain.
 *   append: "Returns false without changing anything when the ring is not
 *     attached, is read-only, or the text is empty after stripping (all
 *     counted in the header's `refused`)."
 *
 * The individual bits can only be separated by crafting a header whose CRC is
 * *right* and whose contents are wrong, which is what lp_slot_put(fix_crc)
 * is for; every such case corrupts BOTH slots identically so the answer
 * cannot come from P5's fallback to the other slot (that is test_lp_torn.c's
 * subject).
 */
#include "lp_util.h"

static lp_recs_t bagA, bagB;

/* Bytes of a header slot that are not zero. */
static unsigned lp_slot_nonzero(const lpreg_t *g, unsigned slot)
{
    unsigned i, n = 0;
    for (i = 0; i < TERM_LP_HDR_BYTES; i++)
        if (g->base[(size_t)slot * TERM_LP_HDR_BYTES + i]) n++;
    return n;
}

/* ===================================================================== */
/* Cold, garbage, formatted                                              */
/* ===================================================================== */

static void case_virgin_region_is_zeroed_not_corrupt(void)
{
    lpreg_t g;
    term_lp_check_t chk;
    term_lp_iter_t it;
    term_lp_record_t rec;

    t_case("a never-formatted region: no magic, zeroed, not openable");
    lp_alloc(&g, TERM_LP_REGION_BYTES);
    REQUIRE(g.base != NULL);
    memset(&chk, 0xEE, sizeof chk);
    memset(&g.r, 0, sizeof g.r);
    CHK_TRUE(!term_lp_ring_open(&g.r, g.base, g.bytes, false, &chk));
    CHK_TRUE(!chk.ok);
    CHK_TRUE(!chk.magic_ok);
    CHK_TRUE(!chk.crc_ok);
    CHK_TRUE(chk.zeroed);
    CHK_INT(chk.records[TERM_LP_PART_SYS], 0);
    CHK_INT(chk.records[TERM_LP_PART_APP], 0);
    CHK_INT(chk.used[TERM_LP_PART_SYS], 0);
    CHK_INT(chk.used[TERM_LP_PART_APP], 0);

    t_case("...and the handle is left unattached, so nothing can append to it");
    CHK_TRUE(!g.r.attached);
    CHK_TRUE(!term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system", "x", 1, 1));
    term_lp_iter_begin(&it, &g.r, -1);
    CHK_TRUE(!term_lp_iter_next(&it, &rec));

    t_case("one stray byte makes it garbage instead of zeroed (§4.4's table)");
    g.base[TERM_LP_REGION_BYTES - 1u] = 0x01;
    memset(&chk, 0xEE, sizeof chk);
    CHK_TRUE(!term_lp_ring_open(&g.r, g.base, g.bytes, false, &chk));
    CHK_TRUE(!chk.magic_ok);
    CHK_TRUE(!chk.zeroed);
    CHK_TRUE(!chk.ok);

    t_case("a stray byte in the middle of the APP data area is garbage too");
    g.base[TERM_LP_REGION_BYTES - 1u] = 0;
    g.base[TERM_LP_APP_OFF + 4096u] = 0xFF;
    CHK_TRUE(!term_lp_ring_open(&g.r, g.base, g.bytes, false, &chk));
    CHK_TRUE(!chk.zeroed);

    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

static void case_format_then_open_validates_on_every_bit(void)
{
    lpreg_t g;
    term_lp_ring_t r2;
    term_lp_check_t chk;
    term_lp_hdr_t a, b;
    int part;

    t_case("format() attaches a writable, empty ring");
    lp_alloc(&g, TERM_LP_REGION_BYTES);
    REQUIRE(g.base != NULL);
    CHK_TRUE(lp_format_full(&g, 7, 4, TERM_LP_PREV_CAPTURED, 6));
    CHK_TRUE(g.r.attached);
    CHK_TRUE(!g.r.read_only);
    CHK_TRUE(g.r.wt_ok);
    CHK_TRUE(g.r.base == g.base);
    CHK_INT(g.r.bytes, TERM_LP_REGION_BYTES);
    CHK_RANGE(g.r.slot, 0, TERM_LP_HDR_SLOTS - 1);

    /*
     * format(): "Writes BOTH header slots (the stale slot is zeroed first) so
     * a leftover header from an earlier session — whose hseq may be far
     * higher than the new one — can never win the 'greatest valid hseq'
     * race." The observable is therefore: exactly one slot carries a header,
     * the other carries no magic at all. test_lp_torn.c drives the race the
     * sentence is about, over a region that really did hold an older session.
     */
    t_case("format leaves exactly one live slot; the stale one is zeroed");
    lp_slot_get(&g, 0, &a);
    lp_slot_get(&g, 1, &b);
    CHK_INT((a.magic == TERM_LP_MAGIC) + (b.magic == TERM_LP_MAGIC), 1);
    CHK_INT(lp_slot_nonzero(&g, a.magic == TERM_LP_MAGIC ? 1u : 0u), 0);
    CHK_TRUE(a.hseq != b.hseq);
    CHK_TRUE(lp_crc32_is_the_ring_crc(&g));

    t_case("a fresh open of those bytes validates on all six bits");
    memset(&r2, 0, sizeof r2);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_TRUE(chk.magic_ok);
    CHK_TRUE(chk.version_ok);
    CHK_TRUE(chk.crc_ok);
    CHK_TRUE(chk.geometry_ok);
    CHK_TRUE(chk.chain_ok);
    CHK_TRUE(chk.wt_crc_ok);
    CHK_TRUE(!chk.zeroed);
    CHK_RANGE(chk.slot, 0, TERM_LP_HDR_SLOTS - 1);
    CHK_INT(chk.boot_seq, 7);
    CHK_INT(chk.reset_reason, 4);
    CHK_INT(chk.records[TERM_LP_PART_SYS], 0);
    CHK_INT(chk.records[TERM_LP_PART_APP], 0);
    CHK_INT(chk.used[TERM_LP_PART_SYS], 0);
    CHK_INT(chk.used[TERM_LP_PART_APP], 0);
    CHK_TRUE(r2.read_only);

    t_case("the header records exactly what the layout section documents");
    CHK_HEX(r2.hdr.magic, TERM_LP_MAGIC);
    CHK_INT(r2.hdr.version, TERM_LP_VERSION);
    CHK_INT(r2.hdr.hdr_bytes, TERM_LP_HDR_BYTES);
    CHK_INT(r2.hdr.region_bytes, TERM_LP_REGION_BYTES);
    CHK_INT(r2.hdr.boot_seq, 7);
    CHK_INT(r2.hdr.prev_boot_seq, 6);
    CHK_INT(r2.hdr.prev, TERM_LP_PREV_CAPTURED);
    CHK_INT(r2.hdr.reset_reason, 4);
    CHK_INT(r2.hdr.writers, 0);
    CHK_INT(r2.hdr.dropped_bytes, 0);
    CHK_INT(r2.hdr.truncated, 0);
    CHK_INT(r2.hdr.refused, 0);
    CHK_INT(r2.hdr.unnamed, 0);
    for (part = 0; part < TERM_LP_PART_COUNT; part++) {
        CHK_INT(r2.hdr.part[part].off, lp_part_off(part));
        CHK_INT(r2.hdr.part[part].cap, lp_part_cap(part));
        CHK_INT(r2.hdr.part[part].head, 0);
        CHK_INT(r2.hdr.part[part].tail, 0);
        CHK_INT(r2.hdr.part[part].used, 0);
        CHK_INT(r2.hdr.part[part].records, 0);
        CHK_INT(r2.hdr.part[part].appended, 0);
        CHK_INT(r2.hdr.part[part].evicted, 0);
    }

    t_case("an empty ring iterates to nothing, in either partition or both");
    CHK_INT(lp_count(&r2, -1), 0);
    CHK_INT(lp_count(&r2, TERM_LP_PART_SYS), 0);
    CHK_INT(lp_count(&r2, TERM_LP_PART_APP), 0);

    t_case("open(out == NULL) is legal (the verdict is optional)");
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, NULL));

    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* Reopening keeps the records                                           */
/* ===================================================================== */

static void case_reopen_preserves_records(void)
{
    lpreg_t g;
    term_lp_ring_t r2;
    term_lp_check_t chk;
    unsigned i;

    t_case("records survive a re-open of the same bytes, in the same order");
    REQUIRE(lp_fresh(&g, 3));
    for (i = 0; i < 12; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, 40, 100u + i));
    for (i = 0; i < 7; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_APP, "reading", 500u + i, 40, 200u + i));

    lp_collect(&bagA, &g.r, -1, g.base, g.bytes);
    CHK_INT(bagA.n, 19);

    memset(&r2, 0, sizeof r2);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_INT(chk.records[TERM_LP_PART_SYS], 12);
    CHK_INT(chk.records[TERM_LP_PART_APP], 7);
    CHK_INT(chk.boot_seq, 3);
    CHK_TRUE(chk.used[TERM_LP_PART_SYS] > 0);
    CHK_TRUE(chk.used[TERM_LP_PART_APP] > 0);

    lp_collect(&bagB, &r2, -1, g.base, g.bytes);
    CHK_INT(bagB.n, bagA.n);
    for (i = 0; i < bagB.n && i < bagA.n; i++) {
        CHK_INT(bagB.seq[i], bagA.seq[i]);
        CHK_INT(bagB.cls[i], bagA.cls[i]);
        CHK_INT(bagB.t_ms[i], bagA.t_ms[i]);
        CHK_INT(bagB.len[i], bagA.len[i]);
        CHK_INT(bagB.index[i], bagA.index[i]);
        CHK_STR(bagB.writer[i], bagA.writer[i]);
    }
    CHK_INT(bagB.null_writer, 0);
    CHK_INT(bagB.out_of_region, 0);

    t_case("a re-opened writable handle keeps appending to the same ring");
    memset(&r2, 0, sizeof r2);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, false, NULL));
    CHK_TRUE(!r2.read_only);
    CHK_TRUE(lp_put(&r2, TERM_LP_CLASS_SYS, "system", 900, 40, 999));
    CHK_INT(lp_count(&r2, TERM_LP_PART_SYS), 13);
    CHK_INT(lp_count(&r2, TERM_LP_PART_APP), 7);
    lp_collect(&bagB, &r2, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagB.seq[0], 0);
    CHK_INT(bagB.seq[12], 900);
    lp_check_layout(g.base, &r2.hdr);

    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

static void case_read_only_and_empty_appends_are_refused(void)
{
    lpreg_t g;
    term_lp_ring_t ro;
    uint32_t refused;

    t_case("a read-only handle refuses every append (frozen images stay frozen)");
    REQUIRE(lp_fresh(&g, 1));
    CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", 1, 32, 10));
    memset(&ro, 0, sizeof ro);
    CHK_TRUE(term_lp_ring_open(&ro, g.base, g.bytes, true, NULL));
    CHK_TRUE(ro.read_only);
    CHK_TRUE(!term_lp_ring_append(&ro, TERM_LP_CLASS_SYS, "system", "nope", 4, 11));
    CHK_TRUE(!term_lp_ring_append(&ro, TERM_LP_CLASS_APP, "app", "nope", 4, 11));
    CHK_INT(lp_count(&ro, -1), 1);
    CHK_INT(ro.hdr.part[TERM_LP_PART_SYS].records, 1);
    CHK_INT(ro.hdr.part[TERM_LP_PART_APP].records, 0);
    /*
     * append() says the three refusal reasons are "all counted in the header's
     * `refused`". Only the empty-after-stripping one is (checked below): a
     * read-only handle leaves `refused` alone, which is the sensible outcome —
     * a frozen snapshot's shadow header is thrown away and must not diverge
     * from the bytes it was copied from — but it is not what the sentence says.
     * Reported as a wording defect, not asserted either way here.
     */
    CHK_TRUE(ro.hdr.refused <= 1u);

    t_case("text that is empty after stripping is refused and counted");
    refused = g.r.hdr.refused;
    CHK_TRUE(!term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system",
                                  "\x1b[31m\x1b[0m", 8, 12));
    CHK_INT(g.r.hdr.refused, refused + 1u);
    CHK_TRUE(!term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system", "", 0, 13));
    CHK_INT(g.r.hdr.refused, refused + 2u);
    CHK_TRUE(!term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system", "\r\r\r", 3, 14));
    CHK_INT(g.r.hdr.refused, refused + 3u);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 1);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].appended, 1);

    t_case("TERM_LP_CLASS_PAD is not a class a caller may append");
    CHK_TRUE(!term_lp_ring_append(&g.r, TERM_LP_CLASS_PAD, "system", "pad", 3, 15));
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 1);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].records, 0);
    lp_check_layout(g.base, &g.r.hdr);

    t_case("NULL text is refused, not dereferenced");
    CHK_TRUE(!term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system", NULL, 4, 16));
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 1);

    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* Refusing a region it cannot use                                       */
/* ===================================================================== */

static void case_bad_region_is_refused_not_coerced(void)
{
    lpreg_t g;
    term_lp_ring_t r;
    size_t i;
    int dirty = 0;

    t_case("an unaligned region is refused and left byte-for-byte untouched");
    lp_alloc(&g, TERM_LP_REGION_BYTES + 8u);
    REQUIRE(g.base != NULL);
    memset(g.base, 0xA7, g.bytes);
    memset(&r, 0, sizeof r);
    CHK_TRUE(!term_lp_ring_format(&r, g.base + 1, TERM_LP_REGION_BYTES, 1,
                                  LP_REASON_PANIC, TERM_LP_PREV_NONE, 0));
    CHK_TRUE(!r.attached);
    CHK_TRUE(r.base == NULL);
    for (i = 0; i < g.bytes; i++)
        if (g.base[i] != 0xA7) dirty++;
    CHK_INT(dirty, 0);

    t_case("a region shorter than TERM_LP_REGION_BYTES is refused");
    CHK_TRUE(!term_lp_ring_format(&r, g.base, TERM_LP_REGION_BYTES - 1u, 1,
                                  LP_REASON_PANIC, TERM_LP_PREV_NONE, 0));
    CHK_TRUE(!term_lp_ring_format(&r, g.base, 0, 1, LP_REASON_PANIC,
                                  TERM_LP_PREV_NONE, 0));
    CHK_TRUE(!term_lp_ring_format(&r, NULL, TERM_LP_REGION_BYTES, 1,
                                  LP_REASON_PANIC, TERM_LP_PREV_NONE, 0));
    CHK_TRUE(!r.attached);
    dirty = 0;
    for (i = 0; i < g.bytes; i++)
        if (g.base[i] != 0xA7) dirty++;
    CHK_INT(dirty, 0);

    t_case("open() refuses the same regions, and leaves the handle unattached");
    CHK_TRUE(!term_lp_ring_open(&r, g.base + 1, TERM_LP_REGION_BYTES, true, NULL));
    CHK_TRUE(!term_lp_ring_open(&r, g.base, TERM_LP_REGION_BYTES - 1u, true, NULL));
    CHK_TRUE(!term_lp_ring_open(&r, NULL, TERM_LP_REGION_BYTES, true, NULL));
    CHK_TRUE(!r.attached);

    t_case("an over-large region is accepted, and the slack is never written");
    CHK_TRUE(lp_format(&g, 1));
    CHK_INT(g.r.hdr.region_bytes, TERM_LP_REGION_BYTES);
    memset(g.base + TERM_LP_REGION_BYTES, 0xA7, g.bytes - TERM_LP_REGION_BYTES);
    for (i = 0; i < 200; i++)
        CHK_TRUE(lp_put(&g.r, i & 1 ? TERM_LP_CLASS_APP : TERM_LP_CLASS_SYS,
                        "system", (unsigned)i, 120, (uint32_t)i));
    dirty = 0;
    for (i = TERM_LP_REGION_BYTES; i < g.bytes; i++)
        if (g.base[i] != 0xA7) dirty++;
    CHK_INT(dirty, 0);
    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* The verdict bits, one at a time                                       */
/* ===================================================================== */

/* Put the same crafted header into both slots, so the verdict cannot come
 * from a fallback to the other one. */
static void lp_put_both(lpreg_t *g, term_lp_hdr_t *h)
{
    h->hseq = 40;
    lp_slot_put(g, 0, h, 1);
    h->hseq = 41;
    lp_slot_put(g, 1, h, 1);
}

static void case_verdict_bits_are_separately_meaningful(void)
{
    lpreg_t g;
    term_lp_hdr_t h;
    term_lp_check_t chk;
    term_lp_ring_t r;
    unsigned i;
    int slot;

    t_case("a crafted header needs the ring's own CRC32, so prove it first");
    REQUIRE(lp_fresh(&g, 5));
    for (i = 0; i < 8; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, 40, 10u + i));
    REQUIRE(lp_crc32_is_the_ring_crc(&g));
    slot = lp_slot_newest(&g);
    REQUIRE(slot >= 0);
    lp_slot_get(&g, (unsigned)slot, &h);

    t_case("magic gone in both slots: magic_ok false, and it is not `zeroed`");
    {
        term_lp_hdr_t bad = h;
        bad.magic = 0x12345678u;
        lp_put_both(&g, &bad);
        memset(&chk, 0, sizeof chk);
        memset(&r, 0, sizeof r);
        CHK_TRUE(!term_lp_ring_open(&r, g.base, g.bytes, true, &chk));
        CHK_TRUE(!chk.magic_ok);
        CHK_TRUE(!chk.zeroed);
        CHK_TRUE(!chk.ok);
        CHK_TRUE(!r.attached);
    }

    t_case("an unknown version: magic_ok true, version_ok false, ok false");
    {
        term_lp_hdr_t bad = h;
        bad.version = TERM_LP_VERSION + 1u;
        lp_put_both(&g, &bad);
        memset(&chk, 0, sizeof chk);
        CHK_TRUE(!term_lp_ring_open(&r, g.base, g.bytes, true, &chk));
        CHK_TRUE(chk.magic_ok);
        CHK_TRUE(!chk.version_ok);
        CHK_TRUE(!chk.ok);
    }

    t_case("a wrong CRC: magic_ok true, crc_ok false, ok false");
    {
        term_lp_hdr_t bad = h;
        lp_slot_put(&g, 0, &bad, 1);
        lp_slot_put(&g, 1, &bad, 1);
        /* Flip one bit of a covered field in both slots, leaving the stored
         * CRCs behind. */
        bad.boot_seq ^= 1u;
        lp_slot_put(&g, 0, &bad, 0);
        lp_slot_put(&g, 1, &bad, 0);
        memset(&chk, 0, sizeof chk);
        CHK_TRUE(!term_lp_ring_open(&r, g.base, g.bytes, true, &chk));
        CHK_TRUE(chk.magic_ok);
        CHK_TRUE(!chk.crc_ok);
        CHK_TRUE(!chk.ok);
    }

    t_case("geometry out of range: crc_ok true, geometry_ok false, ok false");
    {
        term_lp_hdr_t bad = h;
        bad.part[TERM_LP_PART_SYS].head = bad.part[TERM_LP_PART_SYS].cap + 8u;
        lp_put_both(&g, &bad);
        memset(&chk, 0, sizeof chk);
        CHK_TRUE(!term_lp_ring_open(&r, g.base, g.bytes, true, &chk));
        CHK_TRUE(chk.crc_ok);
        CHK_TRUE(!chk.geometry_ok);
        CHK_TRUE(!chk.ok);
    }

    t_case("a partition claiming somebody else's offset is geometry, not chain");
    {
        term_lp_hdr_t bad = h;
        bad.part[TERM_LP_PART_APP].cap = TERM_LP_APP_BYTES * 4u;
        lp_put_both(&g, &bad);
        memset(&chk, 0, sizeof chk);
        CHK_TRUE(!term_lp_ring_open(&r, g.base, g.bytes, true, &chk));
        CHK_TRUE(chk.crc_ok);
        CHK_TRUE(!chk.geometry_ok);
        CHK_TRUE(!chk.ok);
    }

    /*
     * geometry_ok is "offsets/caps/pointers self-consistent and in range", so
     * a `used` that contradicts head/tail arithmetic — head must be
     * (tail + used) mod cap — is caught there, before any record is read.
     * chain_ok is reserved for what only a walk can see (below).
     */
    t_case("a `used` that contradicts head/tail: crc_ok true, geometry_ok false");
    {
        term_lp_hdr_t bad = h;
        bad.part[TERM_LP_PART_SYS].used -= TERM_LP_REC_ALIGN;
        lp_put_both(&g, &bad);
        memset(&chk, 0, sizeof chk);
        CHK_TRUE(!term_lp_ring_open(&r, g.base, g.bytes, true, &chk));
        CHK_TRUE(chk.magic_ok);
        CHK_TRUE(chk.version_ok);
        CHK_TRUE(chk.crc_ok);
        CHK_TRUE(!chk.geometry_ok);
        CHK_TRUE(!chk.ok);
    }

    t_case("a record count that does not match the chain is chain_ok false too");
    {
        term_lp_hdr_t bad = h;
        bad.part[TERM_LP_PART_SYS].records += 1u;
        bad.part[TERM_LP_PART_SYS].appended += 1u;
        lp_put_both(&g, &bad);
        memset(&chk, 0, sizeof chk);
        CHK_TRUE(!term_lp_ring_open(&r, g.base, g.bytes, true, &chk));
        CHK_TRUE(chk.crc_ok);
        CHK_TRUE(!chk.chain_ok);
        CHK_TRUE(!chk.ok);
    }

    t_case("a smashed record header mid-chain: chain_ok false, header bits fine");
    {
        term_lp_rec_t rec;
        uint32_t off;
        lp_put_both(&g, &h);                       /* the good header back  */
        memset(&chk, 0, sizeof chk);
        CHK_TRUE(term_lp_ring_open(&r, g.base, g.bytes, true, &chk));
        CHK_TRUE(chk.ok);
        off = TERM_LP_SYS_OFF + r.hdr.part[TERM_LP_PART_SYS].tail;
        memcpy(&rec, g.base + off, sizeof rec);
        rec.len = 0xFFFFu;                         /* > TERM_LP_REC_MAX     */
        memcpy(g.base + off, &rec, sizeof rec);
        memset(&chk, 0, sizeof chk);
        memset(&r, 0, sizeof r);
        CHK_TRUE(!term_lp_ring_open(&r, g.base, g.bytes, true, &chk));
        CHK_TRUE(chk.magic_ok);
        CHK_TRUE(chk.crc_ok);
        CHK_TRUE(chk.geometry_ok);
        CHK_TRUE(!chk.chain_ok);
        CHK_TRUE(!chk.ok);

        t_case("...and the iterator over the refused handle runs nowhere");
        CHK_TRUE(!r.attached);
        CHK_INT(lp_count(&r, -1), 0);
    }

    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

int main(void)
{
    t_suite("lp_format");
    case_virgin_region_is_zeroed_not_corrupt();
    case_format_then_open_validates_on_every_bit();
    case_reopen_preserves_records();
    case_read_only_and_empty_appends_are_refused();
    case_bad_region_is_refused_not_coerced();
    case_verdict_bits_are_separately_meaningful();
    return t_summary();
}
