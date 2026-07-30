/*
 * test_lp_torn.c — P5: a crash during an append costs at most the record in
 * flight.
 *
 * "The region header is double-buffered and every content byte is written
 * into space the currently-valid header calls free. A reader takes the valid
 * header slot with the highest `hseq`, and the content is always consistent
 * with it. This matters more here than anywhere else in the project: the
 * append most likely to be interrupted is the one immediately before the
 * crash, i.e. exactly the line the operator wants to read." (P5)
 *
 * A host cannot crash mid-store, but the header slots are public structs at a
 * documented offset, so the two things a crash can leave behind are directly
 * constructible: the newest 128-byte header store never landed (torn slot),
 * and/or the record bytes it was about to publish are half written. Both are
 * applied to a *real* append over a real region, and the demand is the
 * header's: still valid, at most one record short, everything older intact.
 *
 * Also here, because they are the same mechanism:
 *   - "a writer always writes the OTHER slot", and a reader takes the greater
 *     hseq — so a stale slot never wins;
 *   - format "Writes BOTH header slots (the stale slot is zeroed first) so a
 *     leftover header from an earlier session — whose hseq may be far higher
 *     than the new one — can never win"; i.e. re-formatting over a previous
 *     session must not resurrect its records, while a byte copy of the image
 *     taken before the format keeps every one of them (the `lastboot` freeze).
 */
#include "lp_util.h"

static lp_recs_t bagA, bagB;
static uint8_t pristine[TERM_LP_REGION_BYTES];

#define TORN_LINE 96u

static void save(const lpreg_t *g) { memcpy(pristine, g->base, sizeof pristine); }
static void restore(lpreg_t *g) { memcpy(g->base, pristine, sizeof pristine); }

/* Every collected record is one of ours and the set is contiguous and
 * increasing — no record from the middle went missing. */
static void check_contiguous(const lp_recs_t *g, unsigned want_last_lo,
                             unsigned want_last_hi)
{
    unsigned i;
    CHK_TRUE(g->n > 0);
    if (!g->n) return;
    for (i = 0; i < g->n; i++) CHK_TRUE(g->seq[i] >= 0);
    for (i = 1; i < g->n; i++) CHK_INT(g->seq[i], g->seq[i - 1u] + 1);
    CHK_RANGE(g->seq[g->n - 1u], (long)want_last_lo, (long)want_last_hi);
}

/* ===================================================================== */
/* A torn final publish                                                  */
/* ===================================================================== */

static void case_torn_publish_costs_one_record(void)
{
    lpreg_t g;
    term_lp_ring_t r2;
    term_lp_check_t chk;
    int newest, oldest;
    unsigned i;

    t_case("baseline: 20 records, and the last one is there");
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 20u; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, TORN_LINE, 10u + i));
    save(&g);
    memset(&r2, 0, sizeof r2);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_INT(chk.records[TERM_LP_PART_SYS], 20);
    lp_collect(&bagA, &r2, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 20);
    check_contiguous(&bagA, 19, 19);

    newest = lp_slot_newest(&g);
    oldest = lp_slot_oldest(&g);
    REQUIRE(newest >= 0 && oldest >= 0 && newest != oldest);

    t_case("the newest header store never landed: still valid, one record short");
    lp_tear_slot(&g, (unsigned)newest, 0xFF);
    memset(&r2, 0, sizeof r2);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_TRUE(chk.crc_ok);
    CHK_TRUE(chk.geometry_ok);
    CHK_TRUE(chk.chain_ok);
    CHK_INT(chk.slot, oldest);
    CHK_RANGE(chk.records[TERM_LP_PART_SYS], 19, 20);
    lp_collect(&bagB, &r2, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagB.n, chk.records[TERM_LP_PART_SYS]);
    check_contiguous(&bagB, 18, 19);
    for (i = 0; i < bagB.n; i++) {
        CHK_INT(bagB.seq[i], bagA.seq[i]);      /* the older ones are intact */
        CHK_INT(bagB.len[i], TORN_LINE);
        CHK_STR(bagB.writer[i], "system");
        CHK_INT(bagB.t_ms[i], 10u + i);
    }
    lp_check_layout(g.base, &r2.hdr);

    t_case("...and torn a second way: the in-flight record's bytes are garbage too");
    restore(&g);
    lp_tear_slot(&g, (unsigned)newest, 0x5A);
    memset(&r2, 0, sizeof r2);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, NULL));
    /* Everything from the surviving header's head onwards is free space by
     * that header's own account, so smashing it must change nothing. */
    memset(g.base + TERM_LP_SYS_OFF + r2.hdr.part[TERM_LP_PART_SYS].head, 0xA5,
           2u * (TERM_LP_REC_HDR + TORN_LINE));
    memset(&r2, 0, sizeof r2);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_TRUE(chk.chain_ok);
    CHK_INT(chk.records[TERM_LP_PART_SYS], bagB.n);
    lp_collect(&bagA, &r2, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, bagB.n);
    for (i = 0; i < bagA.n && i < bagB.n; i++) CHK_INT(bagA.seq[i], bagB.seq[i]);
    CHK_INT(bagA.with_esc, 0);
    lp_check_layout(g.base, &r2.hdr);

    t_case("a torn STALE slot changes nothing at all: the live one still wins");
    restore(&g);
    lp_tear_slot(&g, (unsigned)oldest, 0xFF);
    memset(&r2, 0, sizeof r2);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_INT(chk.slot, newest);
    CHK_INT(chk.records[TERM_LP_PART_SYS], 20);
    CHK_INT(lp_count(&r2, TERM_LP_PART_SYS), 20);

    t_case("both slots torn: no ring at all, and the handle stays unattached");
    restore(&g);
    lp_tear_slot(&g, 0, 0xFF);
    lp_tear_slot(&g, 1, 0xFF);
    memset(&r2, 0, sizeof r2);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(!term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(!chk.ok);
    CHK_TRUE(!chk.crc_ok);
    CHK_TRUE(!chk.zeroed);
    CHK_TRUE(!r2.attached);
    CHK_INT(lp_count(&r2, -1), 0);

    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* The same, but the interrupted append was also evicting                */
/* ===================================================================== */

static void case_torn_publish_while_evicting(void)
{
    lpreg_t g;
    term_lp_ring_t r2;
    term_lp_check_t chk;
    unsigned i, live_before, newest;

    /*
     * Manifest §2 decision 5: "Without this, a crash during the append that
     * evicts the oldest record would overwrite the record the live header
     * still calls `tail`, and the chain walk would then reject the *whole*
     * region — losing the entire black box at the one moment it matters."
     */
    t_case("a full partition, then an evicting append that is cut off");
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 300u; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, TORN_LINE, i));
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_SYS].evicted > 0);
    live_before = g.r.hdr.part[TERM_LP_PART_SYS].records;
    CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", 300, TORN_LINE, 300));
    save(&g);

    newest = (unsigned)lp_slot_newest(&g);
    lp_tear_slot(&g, newest, 0xFF);
    memset(&r2, 0, sizeof r2);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_TRUE(chk.chain_ok);
    CHK_TRUE(chk.geometry_ok);
    CHK_RANGE(chk.records[TERM_LP_PART_SYS], live_before - 1u, live_before);
    lp_collect(&bagA, &r2, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, chk.records[TERM_LP_PART_SYS]);
    check_contiguous(&bagA, 299, 300);
    lp_check_layout(g.base, &r2.hdr);

    t_case("...and the record the old header called `tail` was not overwritten");
    for (i = 0; i < bagA.n; i++) {
        CHK_INT(bagA.len[i], TORN_LINE);
        CHK_STR(bagA.writer[i], "system");
    }

    t_case("the untouched region shows the completed record");
    restore(&g);
    memset(&r2, 0, sizeof r2);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    lp_collect(&bagB, &r2, TERM_LP_PART_SYS, g.base, g.bytes);
    check_contiguous(&bagB, 300, 300);
    CHK_TRUE(bagB.n >= bagA.n);

    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* Stale slots lose                                                      */
/* ===================================================================== */

static void case_the_greater_hseq_wins_and_slots_alternate(void)
{
    lpreg_t g;
    term_lp_hdr_t a, b, newer, older;
    term_lp_check_t chk;
    term_lp_ring_t r2;
    unsigned i;
    int prev_newest = -1;

    t_case("every append publishes the OTHER slot, so hseq alternates");
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 12u; i++) {
        int newest;
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_APP, "app", i, 40, i));
        newest = lp_slot_newest(&g);
        CHK_TRUE(newest >= 0);
        if (prev_newest >= 0) CHK_TRUE(newest != prev_newest);
        CHK_INT(g.r.slot, newest);
        prev_newest = newest;
    }

    t_case("the two slots describe consecutive states; the reader takes the newer");
    lp_slot_get(&g, 0, &a);
    lp_slot_get(&g, 1, &b);
    CHK_HEX(a.magic, TERM_LP_MAGIC);
    CHK_HEX(b.magic, TERM_LP_MAGIC);
    newer = a.hseq > b.hseq ? a : b;
    older = a.hseq > b.hseq ? b : a;
    CHK_INT(newer.hseq, older.hseq + 1u);
    CHK_INT(newer.part[TERM_LP_PART_APP].records, 12);
    CHK_INT(older.part[TERM_LP_PART_APP].records, 11);
    memset(&r2, 0, sizeof r2);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_INT(chk.records[TERM_LP_PART_APP], 12);
    CHK_INT(r2.hdr.hseq, newer.hseq);
    CHK_INT(lp_count(&r2, TERM_LP_PART_APP), 12);

    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* Freeze, re-format, and no resurrection                                */
/* ===================================================================== */

static void case_reformat_over_a_session_resurrects_nothing(void)
{
    lpreg_t live, snap;
    term_lp_ring_t frozen;
    term_lp_check_t chk;
    unsigned i;
    uint32_t old_hseq;

    t_case("a session with 40 records, then a byte copy of the whole image");
    REQUIRE(lp_fresh(&live, 1));
    for (i = 0; i < 20u; i++) {
        CHK_TRUE(lp_put(&live.r, TERM_LP_CLASS_SYS, "system", i, 64, 100u + i));
        CHK_TRUE(lp_put(&live.r, TERM_LP_CLASS_APP, "reading", 500u + i, 64,
                        200u + i));
    }
    old_hseq = live.r.hdr.hseq;
    CHK_INT(live.r.hdr.part[TERM_LP_PART_SYS].records, 20);
    CHK_INT(live.r.hdr.part[TERM_LP_PART_APP].records, 20);
    lp_collect(&bagA, &live.r, -1, live.base, live.bytes);
    CHK_INT(bagA.n, 40);

    lp_alloc(&snap, TERM_LP_REGION_BYTES);
    REQUIRE(snap.base != NULL);
    memcpy(snap.base, live.base, TERM_LP_REGION_BYTES);

    t_case("re-formatting the live region for boot 2 leaves nothing readable");
    CHK_TRUE(lp_format_full(&live, 2, 3, TERM_LP_PREV_CAPTURED, 1));
    CHK_INT(live.r.hdr.boot_seq, 2);
    CHK_INT(live.r.hdr.prev_boot_seq, 1);
    CHK_INT(live.r.hdr.prev, TERM_LP_PREV_CAPTURED);
    CHK_INT(live.r.hdr.part[TERM_LP_PART_SYS].records, 0);
    CHK_INT(live.r.hdr.part[TERM_LP_PART_APP].records, 0);
    CHK_INT(lp_count(&live.r, -1), 0);
    CHK_TRUE(old_hseq > 8u);

    t_case("...not even through a cold open, whatever hseq the old slot had");
    {
        term_lp_ring_t r2;
        memset(&r2, 0, sizeof r2);
        memset(&chk, 0, sizeof chk);
        CHK_TRUE(term_lp_ring_open(&r2, live.base, live.bytes, true, &chk));
        CHK_TRUE(chk.ok);
        CHK_INT(chk.boot_seq, 2);
        CHK_INT(chk.records[TERM_LP_PART_SYS], 0);
        CHK_INT(chk.records[TERM_LP_PART_APP], 0);
        CHK_INT(chk.used[TERM_LP_PART_SYS], 0);
        CHK_INT(chk.used[TERM_LP_PART_APP], 0);
        CHK_INT(lp_count(&r2, -1), 0);
    }

    t_case("the new session logs into the same region, from empty");
    for (i = 0; i < 5u; i++)
        CHK_TRUE(lp_put(&live.r, TERM_LP_CLASS_SYS, "system", 900u + i, 64,
                        i));
    CHK_INT(lp_count(&live.r, TERM_LP_PART_SYS), 5);
    lp_collect(&bagB, &live.r, TERM_LP_PART_SYS, live.base, live.bytes);
    CHK_INT(bagB.n, 5);
    CHK_INT(bagB.seq[0], 900);
    CHK_INT(bagB.seq[4], 904);
    lp_check_layout(live.base, &live.r.hdr);

    t_case("and the frozen copy still holds all 40, read-only, unchanged");
    memset(&frozen, 0, sizeof frozen);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(term_lp_ring_open(&frozen, snap.base, snap.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_TRUE(chk.chain_ok);
    CHK_INT(chk.boot_seq, 1);
    CHK_INT(chk.records[TERM_LP_PART_SYS], 20);
    CHK_INT(chk.records[TERM_LP_PART_APP], 20);
    lp_collect(&bagB, &frozen, -1, snap.base, snap.bytes);
    CHK_INT(bagB.n, 40);
    for (i = 0; i < 40u && i < bagB.n && i < bagA.n; i++) {
        CHK_INT(bagB.seq[i], bagA.seq[i]);
        CHK_INT(bagB.cls[i], bagA.cls[i]);
        CHK_INT(bagB.t_ms[i], bagA.t_ms[i]);
        CHK_STR(bagB.writer[i], bagA.writer[i]);
    }
    CHK_TRUE(frozen.read_only);
    CHK_TRUE(!term_lp_ring_append(&frozen, TERM_LP_CLASS_SYS, "system", "x", 1, 1));
    CHK_INT(lp_count(&frozen, -1), 40);
    lp_check_layout(snap.base, &frozen.hdr);

    t_case("the two images are independent: appending live does not touch it");
    CHK_TRUE(lp_put(&live.r, TERM_LP_CLASS_APP, "reading", 42, 64, 42));
    CHK_INT(lp_count(&frozen, -1), 40);
    CHK_INT(lp_count(&live.r, -1), 6);
    CHK_TRUE(lp_guards_intact(&live));
    CHK_TRUE(lp_guards_intact(&snap));

    lp_free(&snap);
    lp_free(&live);
}

int main(void)
{
    t_suite("lp_torn");
    case_torn_publish_costs_one_record();
    case_torn_publish_while_evicting();
    case_the_greater_hseq_wins_and_slots_alternate();
    case_reformat_over_a_session_resurrects_nothing();
    return t_summary();
}
