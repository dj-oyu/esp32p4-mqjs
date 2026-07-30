/*
 * test_lp_partition.c — P1: two static partitions, no dynamic quotas.
 *
 * "The threat is eviction DoS: with one shared ring a chatty or malicious
 * app's spam flushes the platform's last words. A static split makes that
 * structurally impossible" (term_lp_ring.h, P1; docs/term-design.md §4.4
 * "主脅威は追い出し DoS ... 2 区画に静的分割").
 *
 * So the suite spends most of its checks on the two directions of isolation:
 * wrap SYS many times over and the APP data area must still be zero *bytes*
 * (not merely "accounted as empty"), and vice versa; then the case the split
 * exists for — a 17-lap app flood while six system lines sit in the SYS
 * partition, which must come back byte-exact.
 *
 * Also here, because eviction is what wrapping means:
 *   append: "Lossy at the tail, never at the head: records are evicted from
 *     the oldest end until the new one fits, which is what a flight recorder
 *     is."
 *   term_lp_part_t: `used` includes PAD, `records` does not, and
 *     records + evicted == appended.
 *   hdr.dropped_bytes: "payload bytes lost to eviction, both parts".
 */
#include "lp_util.h"

static lp_recs_t bagA, bagB;

#define LINE_BYTES 96u          /* record = 8 + 96 = 104, no padding needed */

/* Every survivor is a well-formed line of exactly LINE_BYTES, and the set is
 * a contiguous, increasing run ending at `last_seq` — i.e. the newest records
 * in order, with nothing evicted out of turn. */
static void check_survivors(const lp_recs_t *g, char tag, unsigned last_seq,
                            uint16_t want_len)
{
    unsigned i;
    CHK_TRUE(g->n > 0);
    CHK_INT(g->overflow, 0);
    CHK_INT(g->out_of_region, 0);
    CHK_INT(g->with_esc, 0);
    if (g->n == 0) return;
    CHK_INT(g->seq[g->n - 1u], (long)last_seq);
    CHK_INT(g->seq[0], (long)(last_seq + 1u - g->n));
    for (i = 0; i < g->n; i++) {
        t_checks++;
        if (g->seq[i] != (long)(last_seq + 1u - g->n + i) ||
            g->len[i] != want_len) {
            t_head(__FILE__, __LINE__);
            printf("     record %u of %u: seq %ld (want %ld), len %u (want %u)\n",
                   i, g->n, g->seq[i], (long)(last_seq + 1u - g->n + i),
                   g->len[i], want_len);
            break;
        }
    }
    (void)tag;
}

/* ===================================================================== */
/* One partition wraps; the other is not merely empty, it is untouched    */
/* ===================================================================== */

static void case_sys_laps_leave_app_untouched(void)
{
    lpreg_t g;
    term_lp_check_t chk;
    term_lp_ring_t r2;
    unsigned i, content = 0, pads = 0;
    char why[256];

    t_case("400 SYS lines (~5 laps of 8 KiB) evict only from SYS");
    REQUIRE(lp_fresh(&g, 2));
    for (i = 0; i < 400; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", i, LINE_BYTES,
                        1000u + i));

    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].appended, 400);
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_SYS].evicted > 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records +
            g.r.hdr.part[TERM_LP_PART_SYS].evicted, 400);
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_SYS].used <= TERM_LP_SYS_BYTES);
    CHK_RANGE(g.r.hdr.part[TERM_LP_PART_SYS].records, 60, 78);

    t_case("the APP partition never moved: counters AND bytes");
    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].records, 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].appended, 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].evicted, 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].used, 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].head, 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].tail, 0);
    CHK_TRUE(lp_part_untouched(&g, TERM_LP_PART_APP));
    CHK_TRUE(lp_guards_intact(&g));

    t_case("dropped_bytes accounts for exactly the evicted payloads");
    CHK_INT(g.r.hdr.dropped_bytes,
            g.r.hdr.part[TERM_LP_PART_SYS].evicted * LINE_BYTES);
    CHK_INT(g.r.hdr.truncated, 0);
    CHK_INT(g.r.hdr.refused, 0);

    t_case("the wrapped chain still walks, with a PAD at every lap boundary");
    CHK_INT(lp_walk(g.base, &g.r.hdr, TERM_LP_PART_SYS, &content, &pads,
                    why, sizeof why), 0);
    if (why[0]) printf("     %s\n", why);
    CHK_INT(content, g.r.hdr.part[TERM_LP_PART_SYS].records);
    CHK_RANGE(pads, 0, 1);   /* at most one PAD is live at any moment */
    lp_check_layout(g.base, &g.r.hdr);

    t_case("the survivors are the newest records, in order, byte-exact");
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, g.r.hdr.part[TERM_LP_PART_SYS].records);
    check_survivors(&bagA, 'S', 399, (uint16_t)LINE_BYTES);
    for (i = 0; i < bagA.n; i++) {
        CHK_INT(bagA.cls[i], TERM_LP_CLASS_SYS);
        CHK_INT(bagA.flags[i], 0);
    }
    {
        char want[256];
        size_t n = lp_mkline(want, sizeof want, 'S', 399, LINE_BYTES);
        CHK_INT(n, LINE_BYTES);
        CHK_STR(lp_text_at(&g.r, TERM_LP_PART_SYS, bagA.n - 1u), want);
    }

    t_case("iterating both partitions sees exactly the SYS survivors");
    CHK_INT(lp_count(&g.r, -1), bagA.n);
    CHK_INT(lp_count(&g.r, TERM_LP_PART_APP), 0);

    t_case("a fresh open of the wrapped region validates and reads the same");
    memset(&r2, 0, sizeof r2);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_TRUE(chk.chain_ok);
    CHK_INT(chk.records[TERM_LP_PART_SYS], bagA.n);
    CHK_INT(chk.records[TERM_LP_PART_APP], 0);
    lp_collect(&bagB, &r2, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagB.n, bagA.n);
    for (i = 0; i < bagB.n && i < bagA.n; i++)
        CHK_INT(bagB.seq[i], bagA.seq[i]);

    lp_free(&g);
}

static void case_app_laps_leave_sys_untouched(void)
{
    lpreg_t g;
    unsigned i;

    t_case("1200 APP lines (~5 laps of 23 KiB) evict only from APP");
    REQUIRE(lp_fresh(&g, 2));
    for (i = 0; i < 1200; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_APP, "reading", i, LINE_BYTES,
                        2000u + i));

    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].appended, 1200);
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_APP].evicted > 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].records +
            g.r.hdr.part[TERM_LP_PART_APP].evicted, 1200);
    CHK_RANGE(g.r.hdr.part[TERM_LP_PART_APP].records, 200, 226);

    t_case("the SYS partition never moved: counters AND bytes");
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].appended, 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].evicted, 0);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].used, 0);
    CHK_TRUE(lp_part_untouched(&g, TERM_LP_PART_SYS));
    CHK_TRUE(lp_guards_intact(&g));

    t_case("the survivors are the newest APP records, in order");
    lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_INT(bagA.n, g.r.hdr.part[TERM_LP_PART_APP].records);
    check_survivors(&bagA, 'A', 1199, (uint16_t)LINE_BYTES);
    for (i = 0; i < bagA.n; i++) {
        CHK_INT(bagA.cls[i], TERM_LP_CLASS_APP);
        CHK_STR(bagA.writer[i], "reading");
    }
    lp_check_layout(g.base, &g.r.hdr);

    lp_free(&g);
}

/* ===================================================================== */
/* The threat model, driven                                              */
/* ===================================================================== */

static void case_chatty_app_cannot_flush_the_platform(void)
{
    lpreg_t g;
    unsigned i;
    static const char *last_words[6] = {
        "boot 9 reset=panic(4) prev=captured(8)",
        "wifi: got ip 100.83.214.77",
        "mqtt: connected",
        "brownout detector armed",
        "task hung: js_task",
        "panic: LoadProhibited at 0x00000000",
    };

    t_case("six platform lines, then a 17-lap app flood");
    REQUIRE(lp_fresh(&g, 9));
    for (i = 0; i < 6; i++)
        CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS,
                                     TERM_LP_WRITER_SYSTEM, last_words[i],
                                     strlen(last_words[i]), 10u + i));
    for (i = 0; i < 2000; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_APP, "spammer", i, 200, 100u + i));

    t_case("the app partition really was flooded (many laps, many evictions)");
    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].appended, 2000);
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_APP].evicted > 1800);
    CHK_TRUE(g.r.hdr.dropped_bytes > 350000u);

    t_case("...and the platform's last words are all six, byte-exact, in order");
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 6);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].evicted, 0);
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 6);
    for (i = 0; i < 6 && i < bagA.n; i++) {
        CHK_STR(lp_text_at(&g.r, TERM_LP_PART_SYS, i), last_words[i]);
        CHK_INT(bagA.len[i], (int)strlen(last_words[i]));
        CHK_INT(bagA.cls[i], TERM_LP_CLASS_SYS);
        CHK_INT(bagA.t_ms[i], 10u + i);
        CHK_STR(bagA.writer[i], TERM_LP_WRITER_SYSTEM);
    }

    t_case("both chains are still walkable after the flood");
    lp_check_layout(g.base, &g.r.hdr);
    CHK_TRUE(lp_guards_intact(&g));

    t_case("and the reverse: a flooded SYS partition does not touch APP's tail");
    for (i = 0; i < 400; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, "system", 5000u + i, 200,
                        9000u + i));
    lp_collect(&bagB, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_INT(bagB.n, g.r.hdr.part[TERM_LP_PART_APP].records);
    check_survivors(&bagB, 'A', 1999, 200);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_APP].appended, 2000);
    lp_check_layout(g.base, &g.r.hdr);

    lp_free(&g);
}

/* ===================================================================== */
/* Interleaving, and many laps of both at once                           */
/* ===================================================================== */

static void case_interleaved_laps_keep_both_chains_valid(void)
{
    lpreg_t g;
    term_lp_check_t chk;
    term_lp_ring_t r2;
    unsigned i;
    unsigned sizes[6];

    sizes[0] = 16; sizes[1] = 33; sizes[2] = 96;
    sizes[3] = 200; sizes[4] = 401; sizes[5] = 512;

    t_case("2400 interleaved records of six different sizes");
    REQUIRE(lp_fresh(&g, 4));
    for (i = 0; i < 2400; i++) {
        term_lp_class_t cls = (i % 3u) ? TERM_LP_CLASS_APP : TERM_LP_CLASS_SYS;
        CHK_TRUE(lp_put(&g.r, cls, cls == TERM_LP_CLASS_SYS ? "system" : "app",
                        i, sizes[i % 6u], 500u + i));
    }
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].appended +
            g.r.hdr.part[TERM_LP_PART_APP].appended, 2400);
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_SYS].evicted > 0);
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_APP].evicted > 0);
    CHK_INT(g.r.hdr.truncated, 0);
    CHK_INT(g.r.hdr.refused, 0);

    t_case("both partitions hold contiguous newest-first-evicted runs");
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    lp_collect(&bagB, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_INT(bagA.n, g.r.hdr.part[TERM_LP_PART_SYS].records);
    CHK_INT(bagB.n, g.r.hdr.part[TERM_LP_PART_APP].records);
    for (i = 1; i < bagA.n; i++) CHK_TRUE(bagA.seq[i] > bagA.seq[i - 1u]);
    for (i = 1; i < bagB.n; i++) CHK_TRUE(bagB.seq[i] > bagB.seq[i - 1u]);
    for (i = 0; i < bagA.n; i++) {
        CHK_INT(bagA.cls[i], TERM_LP_CLASS_SYS);
        CHK_INT(bagA.seq[i] % 3, 0);
    }
    for (i = 0; i < bagB.n; i++) {
        CHK_INT(bagB.cls[i], TERM_LP_CLASS_APP);
        CHK_TRUE(bagB.seq[i] % 3 != 0);
    }
    CHK_INT(bagA.seq[bagA.n - 1u], 2397);
    CHK_INT(bagB.seq[bagB.n - 1u], 2399);

    t_case("the region validates from cold, with the same contents");
    lp_check_layout(g.base, &g.r.hdr);
    memset(&r2, 0, sizeof r2);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_INT(chk.records[TERM_LP_PART_SYS], bagA.n);
    CHK_INT(chk.records[TERM_LP_PART_APP], bagB.n);
    CHK_INT(chk.used[TERM_LP_PART_SYS], g.r.hdr.part[TERM_LP_PART_SYS].used);
    CHK_INT(chk.used[TERM_LP_PART_APP], g.r.hdr.part[TERM_LP_PART_APP].used);
    CHK_TRUE(lp_guards_intact(&g));

    lp_free(&g);
}

int main(void)
{
    t_suite("lp_partition");
    case_sys_laps_leave_app_untouched();
    case_app_laps_leave_sys_untouched();
    case_chatty_app_cannot_flush_the_platform();
    case_interleaved_laps_keep_both_chains_valid();
    return t_summary();
}
