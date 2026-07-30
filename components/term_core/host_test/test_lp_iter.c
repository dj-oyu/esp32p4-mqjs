/*
 * test_lp_iter.c — the read API the MQTT responder of §7.2 is a pure consumer
 * of.
 *
 * Contracts under test (term_lp_ring.h):
 *   term_lp_iter_begin: "Begin an iteration over `part` (TERM_LP_PART_SYS /
 *     _APP), or over both in SYS-then-APP order when `part` is negative.
 *     Records come out oldest first; PAD records are skipped and never seen by
 *     the caller."
 *   term_lp_iter_t: "Oldest first. Treat as opaque."
 *   term_lp_record_t::text: "points into the region/snapshot and is NOT
 *     NUL-terminated; it stays valid as long as nobody appends (always, for a
 *     LASTBOOT snapshot)."
 *   term_lp_record_t::index: "ordinal within its partition this boot".
 *   "Read calls (term_lp_iter_*, ...) do not mutate" — §7.2's "読み出しは
 *     無変異(probe が測定対象を変えない)".
 *   The frozen image is "bit-identical in layout to the live one — which is
 *     why one iterator serves both sources".
 */
#include "lp_util.h"

static lp_recs_t bagA, bagB;

/* ===================================================================== */
/* Order, selection, emptiness                                           */
/* ===================================================================== */

static void case_oldest_first_and_partition_selection(void)
{
    lpreg_t g;
    unsigned i;

    t_case("an empty ring iterates to nothing, three ways");
    REQUIRE(lp_fresh(&g, 1));
    CHK_INT(lp_count(&g.r, -1), 0);
    CHK_INT(lp_count(&g.r, TERM_LP_PART_SYS), 0);
    CHK_INT(lp_count(&g.r, TERM_LP_PART_APP), 0);
    {   /* and a second next() after a false one stays false */
        term_lp_iter_t it;
        term_lp_record_t rec;
        term_lp_iter_begin(&it, &g.r, -1);
        CHK_TRUE(!term_lp_iter_next(&it, &rec));
        CHK_TRUE(!term_lp_iter_next(&it, &rec));
    }

    t_case("records come out oldest first, within one partition");
    for (i = 0; i < 30u; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, 48, 100u + i));
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 30);
    for (i = 0; i < bagA.n; i++) {
        CHK_INT(bagA.seq[i], (long)i);
        CHK_INT(bagA.t_ms[i], 100u + i);
    }

    t_case("only that partition: APP records are invisible to a SYS walk");
    for (i = 0; i < 9u; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_APP, "app", 700u + i, 48, 300u + i));
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 30);
    for (i = 0; i < bagA.n; i++) CHK_INT(bagA.cls[i], TERM_LP_CLASS_SYS);
    lp_collect(&bagB, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_INT(bagB.n, 9);
    for (i = 0; i < bagB.n; i++) {
        CHK_INT(bagB.cls[i], TERM_LP_CLASS_APP);
        CHK_INT(bagB.seq[i], (long)(700u + i));
    }

    t_case("part < 0 walks both, SYS first then APP, each oldest first");
    lp_collect(&bagA, &g.r, -1, g.base, g.bytes);
    CHK_INT(bagA.n, 39);
    for (i = 0; i < 30u && i < bagA.n; i++) {
        CHK_INT(bagA.cls[i], TERM_LP_CLASS_SYS);
        CHK_INT(bagA.seq[i], (long)i);
    }
    for (i = 30u; i < bagA.n; i++) {
        CHK_INT(bagA.cls[i], TERM_LP_CLASS_APP);
        CHK_INT(bagA.seq[i], (long)(700u + i - 30u));
    }

    /* The header pins negative = both and 0/1 = the two partitions; anything
     * else is not in the contract, and the only answer that cannot mislead a
     * reader is "no records". */
    t_case("any negative part means both; an out-of-range index yields nothing");
    {
        term_lp_iter_t it;
        term_lp_record_t rec;
        unsigned c = 0;
        term_lp_iter_begin(&it, &g.r, -7);
        while (term_lp_iter_next(&it, &rec)) c++;
        CHK_INT(c, 39);
        CHK_INT(lp_count(&g.r, TERM_LP_PART_COUNT), 0);
        CHK_INT(lp_count(&g.r, 99), 0);
    }

    t_case("`index` is the ordinal within the partition, 0-based, gapless");
    CHK_INT(bagA.index[0], 0);
    CHK_INT(bagA.index[29], 29);
    CHK_INT(bagA.index[30], 0);
    CHK_INT(bagA.index[38], 8);
    for (i = 1; i < 30u; i++) CHK_INT(bagA.index[i], bagA.index[i - 1u] + 1u);

    lp_free(&g);
}

/* ===================================================================== */
/* Across the wrap point                                                 */
/* ===================================================================== */

static void case_iteration_crosses_the_wrap(void)
{
    lpreg_t g;
    unsigned i, content = 0, pads = 0;
    char why[256];

    /*
     * 96-byte payloads make a 104-byte record, so a lap is 78 records plus an
     * 80-byte PAD = exactly 8192: the partition ends up FULL, with
     * head == tail. That is the case term_lp_part_t::used exists for ("`used`
     * is carried explicitly because head == tail is otherwise ambiguous
     * between empty and full"), and the chain necessarily crosses the physical
     * end of the partition, which is what this case is here to walk.
     */
    t_case("a full partition: head == tail, used == cap, and the chain wraps");
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 300u; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, 96, i));
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].used, TERM_LP_SYS_BYTES);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].head,
            g.r.hdr.part[TERM_LP_PART_SYS].tail);
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_SYS].tail +
             g.r.hdr.part[TERM_LP_PART_SYS].used > TERM_LP_SYS_BYTES);
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_SYS].tail > 0);

    t_case("...and the walk still comes out oldest first, with no PAD in sight");
    CHK_INT(lp_walk(g.base, &g.r.hdr, TERM_LP_PART_SYS, &content, &pads, why,
                    sizeof why), 0);
    if (why[0]) printf("     %s\n", why);
    CHK_INT(pads, 1);              /* the lap boundary the walk crossed */
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, content);
    CHK_INT(bagA.n, g.r.hdr.part[TERM_LP_PART_SYS].records);
    for (i = 0; i < bagA.n; i++) {
        CHK_INT(bagA.cls[i], TERM_LP_CLASS_SYS);   /* never CLASS_PAD */
        CHK_INT(bagA.len[i], 96);
        if (i) CHK_INT(bagA.seq[i], bagA.seq[i - 1u] + 1);
    }
    CHK_INT(bagA.seq[bagA.n - 1u], 299);

    t_case("`index` keeps counting across the wrap: last == appended - 1");
    CHK_INT(bagA.index[bagA.n - 1u],
            g.r.hdr.part[TERM_LP_PART_SYS].appended - 1u);
    CHK_INT(bagA.index[0], g.r.hdr.part[TERM_LP_PART_SYS].evicted);
    for (i = 1; i < bagA.n; i++) CHK_INT(bagA.index[i], bagA.index[i - 1u] + 1u);

    t_case("text pointers all point inside the partition's own data area");
    CHK_INT(bagA.out_of_region, 0);
    {
        term_lp_iter_t it;
        term_lp_record_t rec;
        unsigned outside = 0;
        term_lp_iter_begin(&it, &g.r, TERM_LP_PART_SYS);
        while (term_lp_iter_next(&it, &rec)) {
            const uint8_t *lo = g.base + TERM_LP_SYS_OFF;
            const uint8_t *hi = lo + TERM_LP_SYS_BYTES;
            if (rec.text < lo || rec.text + rec.len > hi) outside++;
        }
        CHK_INT(outside, 0);
    }
    lp_free(&g);
}

/* ===================================================================== */
/* Iteration does not mutate, and iterators are independent values        */
/* ===================================================================== */

static void case_reading_changes_nothing(void)
{
    lpreg_t g;
    static uint8_t before[TERM_LP_REGION_BYTES];
    term_lp_hdr_t hdr_before;
    unsigned i;

    t_case("a full iteration leaves the region byte-identical (§7.2)");
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 200u; i++)
        CHK_TRUE(lp_put(&g.r, i & 1u ? TERM_LP_CLASS_APP : TERM_LP_CLASS_SYS,
                        "system", i, 120, i));
    memcpy(before, g.base, sizeof before);
    hdr_before = g.r.hdr;
    lp_collect(&bagA, &g.r, -1, g.base, g.bytes);
    CHK_TRUE(bagA.n > 0);
    CHK_INT(memcmp(before, g.base, sizeof before), 0);
    CHK_INT(memcmp(&hdr_before, &g.r.hdr, sizeof hdr_before), 0);

    t_case("two iterators over one ring do not interfere");
    {
        term_lp_iter_t a, b;
        term_lp_record_t ra, rb;
        unsigned n = 0;
        term_lp_iter_begin(&a, &g.r, -1);
        term_lp_iter_begin(&b, &g.r, -1);
        while (term_lp_iter_next(&a, &ra)) {
            CHK_TRUE(term_lp_iter_next(&b, &rb));
            if (ra.len != rb.len || ra.text != rb.text || ra.index != rb.index ||
                ra.cls != rb.cls) {
                t_head(__FILE__, __LINE__);
                printf("     record %u differs between two iterators\n", n);
                break;
            }
            t_checks++;
            n++;
        }
        CHK_TRUE(!term_lp_iter_next(&b, &rb));
        CHK_INT(n, bagA.n);
    }

    t_case("the same text pointers come back on a second pass");
    {
        term_lp_iter_t it;
        term_lp_record_t rec;
        unsigned k = 0, moved = 0;
        const uint8_t *first_text = NULL;
        term_lp_iter_begin(&it, &g.r, -1);
        while (term_lp_iter_next(&it, &rec)) {
            if (!k) first_text = rec.text;
            k++;
        }
        lp_collect(&bagB, &g.r, -1, g.base, g.bytes);
        CHK_INT(bagB.n, bagA.n);
        for (k = 0; k < bagB.n && k < bagA.n; k++)
            if (bagB.seq[k] != bagA.seq[k] || bagB.len[k] != bagA.len[k]) moved++;
        CHK_INT(moved, 0);
        CHK_TRUE(first_text != NULL);
    }
    lp_free(&g);
}

/* ===================================================================== */
/* One iterator, two sources                                             */
/* ===================================================================== */

static void case_one_iterator_serves_live_and_frozen(void)
{
    lpreg_t live, snap;
    term_lp_ring_t frozen;
    term_lp_check_t chk;
    unsigned i;

    t_case("a frozen byte copy iterates exactly like the live image did");
    REQUIRE(lp_fresh(&live, 6));
    for (i = 0; i < 400u; i++)
        CHK_TRUE(lp_put(&live.r, (i % 4u) ? TERM_LP_CLASS_APP : TERM_LP_CLASS_SYS,
                        (i % 4u) ? "reading" : "system", i, 64u + (i % 3u) * 8u,
                        1000u + i));
    lp_collect(&bagA, &live.r, -1, live.base, live.bytes);
    CHK_TRUE(bagA.n > 100u);

    lp_alloc(&snap, TERM_LP_REGION_BYTES);
    REQUIRE(snap.base != NULL);
    memcpy(snap.base, live.base, TERM_LP_REGION_BYTES);
    memset(&frozen, 0, sizeof frozen);
    CHK_TRUE(term_lp_ring_open(&frozen, snap.base, snap.bytes, true, &chk));
    CHK_TRUE(chk.ok);

    lp_collect(&bagB, &frozen, -1, snap.base, snap.bytes);
    CHK_INT(bagB.n, bagA.n);
    for (i = 0; i < bagB.n && i < bagA.n; i++) {
        CHK_INT(bagB.seq[i], bagA.seq[i]);
        CHK_INT(bagB.cls[i], bagA.cls[i]);
        CHK_INT(bagB.len[i], bagA.len[i]);
        CHK_INT(bagB.index[i], bagA.index[i]);
        CHK_INT(bagB.t_ms[i], bagA.t_ms[i]);
        CHK_INT(bagB.flags[i], bagA.flags[i]);
        CHK_STR(bagB.writer[i], bagA.writer[i]);
    }
    CHK_INT(bagB.out_of_region, 0);

    t_case("appending to the live image does not disturb the frozen one");
    for (i = 0; i < 50u; i++)
        CHK_TRUE(lp_put(&live.r, TERM_LP_CLASS_APP, "reading", 9000u + i, 200,
                        i));
    lp_collect(&bagB, &frozen, -1, snap.base, snap.bytes);
    CHK_INT(bagB.n, bagA.n);
    for (i = 0; i < bagB.n && i < bagA.n; i++) CHK_INT(bagB.seq[i], bagA.seq[i]);
    CHK_TRUE(lp_guards_intact(&live));
    CHK_TRUE(lp_guards_intact(&snap));

    lp_free(&snap);
    lp_free(&live);
}

int main(void)
{
    t_suite("lp_iter");
    case_oldest_first_and_partition_selection();
    case_iteration_crosses_the_wrap();
    case_reading_changes_nothing();
    case_one_iterator_serves_live_and_frozen();
    return t_summary();
}
