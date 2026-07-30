/*
 * test_lp_appendpath.c — P6, as far as a host can see it.
 *
 * "P6. NO ALLOCATION ON THE APPEND PATH, AND NO LP READS ON THE HOT PATH
 *  BEYOND EVICTION. The header lives in a DRAM shadow; appends are
 *  sequential, 8-byte-aligned writes. The one LP read is the 8-byte header of
 *  a record being evicted from the tail."
 *
 * Three of those clauses are host-observable and are what this suite drives:
 *
 *   1. no allocation — the component's only allocator is the port's
 *      mem_alloc/mem_free (term_port.h: "a coarse allocator for the one
 *      per-term block"), so the fake port's FROZEN mode turns "nothing is
 *      allocated" into a measurement, exactly as the phase-2 suites do for I4.
 *      LeakSanitizer is the second half: a per-append allocation that leaked
 *      would be reported at exit after 6,000 appends.
 *   2. the header lives in a DRAM shadow — so an append does not consult the
 *      region's header at all. Smash both region header slots and keep
 *      appending through the same handle: the ring must carry on and the next
 *      publishes must leave the region valid again.
 *   3. appends are sequential writes into free space, never reads of it — fill
 *      every free byte of both partitions with garbage and keep appending; the
 *      chain must stay exactly as walkable as before.
 *
 * Not host-observable, and deliberately left to the device: that the eviction
 * read really is 8 bytes and that no other LP read happens on the hot path
 * (that is a bus-traffic statement, not an API one), and whether a malloc/free
 * *pair* on the append path would go unnoticed by both counters above.
 */
#include "lp_util.h"
#include "fake_port.h"

static lp_recs_t bagA, bagB;

/* ===================================================================== */
/* No allocation                                                         */
/* ===================================================================== */

static void case_append_allocates_nothing(void)
{
    lpreg_t g;
    unsigned i;
    uint32_t appended;

    t_case("6000 appends through a FROZEN allocator: not one call");
    fp_reset();
    REQUIRE(term_port_install(fp_port()));
    fp.alloc_frozen = true;

    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 6000u; i++) {
        term_lp_class_t cls = (i % 3u) ? TERM_LP_CLASS_APP : TERM_LP_CLASS_SYS;
        CHK_TRUE(lp_put(&g.r, cls, cls == TERM_LP_CLASS_SYS ? "system" : "app",
                        i, 24u + (i % 61u) * 8u, i));
    }
    CHK_INT(fp.allocs, 0);
    CHK_INT(fp.frees, 0);
    CHK_INT(fp.alloc_violations, 0);
    CHK_INT(fp.live_bytes, 0);

    t_case("...and neither does reading the result back");
    lp_collect(&bagA, &g.r, -1, g.base, g.bytes);
    CHK_TRUE(bagA.n > 0);
    CHK_INT(fp.allocs, 0);
    CHK_INT(fp.alloc_violations, 0);
    fp.alloc_frozen = false;

    t_case("the run really did wrap both partitions many times over");
    appended = g.r.hdr.part[TERM_LP_PART_SYS].appended +
               g.r.hdr.part[TERM_LP_PART_APP].appended;
    CHK_INT(appended, 6000);
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_SYS].evicted > 1500u);
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_APP].evicted > 3000u);
    lp_check_layout(g.base, &g.r.hdr);
    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* The header shadow is authoritative                                    */
/* ===================================================================== */

static void case_header_shadow_is_the_authority(void)
{
    lpreg_t g;
    term_lp_ring_t r2;
    term_lp_check_t chk;
    unsigned i;

    t_case("both region header slots smashed: the ring keeps appending");
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 5u; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, 64, i));
    memset(g.base, 0xFF, TERM_LP_HDR_BYTES * TERM_LP_HDR_SLOTS);
    for (i = 5u; i < 10u; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, 64, i));
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 10);
    CHK_INT(lp_count(&g.r, TERM_LP_PART_SYS), 10);

    t_case("...and the publishes that followed left the region valid again");
    memset(&r2, 0, sizeof r2);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_TRUE(chk.chain_ok);
    CHK_INT(chk.records[TERM_LP_PART_SYS], 10);
    lp_collect(&bagA, &r2, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 10);
    for (i = 0; i < bagA.n; i++) {
        CHK_INT(bagA.seq[i], (long)i);
        CHK_INT(bagA.len[i], 64);
    }
    lp_check_layout(g.base, &r2.hdr);
    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* Free space is written, never read                                     */
/* ===================================================================== */

static void case_appends_do_not_read_free_space(void)
{
    lpreg_t g;
    term_lp_ring_t r2;
    term_lp_check_t chk;
    unsigned i;
    uint32_t head;

    t_case("every free byte of both partitions filled with garbage");
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 10u; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, 64, i));
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 10);

    head = g.r.hdr.part[TERM_LP_PART_SYS].head;
    memset(g.base + TERM_LP_SYS_OFF + head, 0xA5, TERM_LP_SYS_BYTES - head);
    memset(g.base + TERM_LP_APP_OFF, 0xA5, TERM_LP_APP_BYTES);

    t_case("appending over it works, and the chain is still exactly walkable");
    for (i = 10u; i < 40u; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, 64, i));
    for (i = 0; i < 10u; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_APP, "app", 500u + i, 64, i));
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 40);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].records, 10);
    lp_check_layout(g.base, &g.r.hdr);

    memset(&r2, 0, sizeof r2);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_INT(chk.records[TERM_LP_PART_SYS], 40);
    CHK_INT(chk.records[TERM_LP_PART_APP], 10);
    lp_collect(&bagB, &r2, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagB.n, 40);
    for (i = 0; i < bagB.n; i++) {
        CHK_INT(bagB.seq[i], (long)i);
        CHK_INT(bagB.len[i], 64);
        CHK_STR(bagB.writer[i], "system");
    }
    CHK_INT(bagB.with_esc, 0);
    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* Nothing outside the region, ever                                      */
/* ===================================================================== */

static void case_writes_stay_inside_the_region(void)
{
    lpreg_t g;
    unsigned i;
    size_t dirty = 0;

    t_case("a heavy mixed run touches no byte outside the region");
    lp_alloc(&g, TERM_LP_REGION_BYTES + 64u);
    REQUIRE(g.base != NULL);
    CHK_TRUE(lp_format(&g, 1));
    memset(g.base + TERM_LP_REGION_BYTES, 0xA7, 64u);
    for (i = 0; i < 3000u; i++) {
        term_lp_class_t cls = (i & 1u) ? TERM_LP_CLASS_APP : TERM_LP_CLASS_SYS;
        CHK_TRUE(lp_put(&g.r, cls, "system", i, 8u + (i % 63u) * 8u, i));
    }
    for (i = 0; i < 64u; i++)
        if (g.base[TERM_LP_REGION_BYTES + i] != 0xA7) dirty++;
    CHK_INT(dirty, 0);
    CHK_TRUE(lp_guards_intact(&g));
    lp_check_layout(g.base, &g.r.hdr);
    lp_free(&g);
}

int main(void)
{
    t_suite("lp_appendpath");
    case_append_allocates_nothing();
    case_header_shadow_is_the_authority();
    case_appends_do_not_read_free_space();
    case_writes_stay_inside_the_region();
    fp_reclaim_all();
    return t_summary();
}
