/*
 * tools/tests/test_perf_hist.c -- host unit test for main/perf_meter.c
 * (native-editor-spec.md section C.0 / C.1 / C.4, "perf_meter の算術"
 * assignment). Pure C99, no ESP-IDF: perf_meter.c compiles unmodified on
 * a host because ESP-specific bits are behind #ifdef ESP_PLATFORM, which
 * is never defined here.
 *
 * Each section names the defect it exists to catch, per
 * docs/native-editor-plan.md section 7's rule: a test that does not say
 * what it is for is not trusted to have caught anything.
 *
 * Build/run: tools/tests/run_perf_tests.sh (same gcc/wsl-gcc fallback as
 * components/fs_core/pc_test/run_tests.sh).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../main/perf_meter.h"

#if !PERF_METER_REAL
#error "this test must run with the real arithmetic (host build implies PERF_METER_REAL=1); check perf_meter.h's ESP_PLATFORM/CONFIG_MQJS_PERF_METER guard"
#endif

/* ================================================================== */
/* Harness (same shape as tools/tests/test_skk_dict.c) */

static int g_pass, g_fail;
static const char *g_ctx = "";

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (cond) {                                                        \
            g_pass++;                                                      \
        } else {                                                           \
            g_fail++;                                                      \
            printf("  FAIL [%s] %s:%d: ", g_ctx, __FILE__, __LINE__);       \
            printf(__VA_ARGS__);                                           \
            putchar('\n');                                                 \
        }                                                                  \
    } while (0)

static void section(const char *name)
{
    printf("\n== %s\n", name);
    g_ctx = name;
}

/* ================================================================== */
/* 1. Bucket boundaries (perf_hist_add / perf_hist_pct via a 1-sample
 *    histogram, which pins down exactly which bucket a value landed in).
 *
 *    Defect this catches: an off-by-one in the (bit - 9) shift or the
 *    clamp would put a boundary value in the neighboring bucket, and
 *    because it only shows up ±1 unit around a power of two, a test that
 *    only tries "round" numbers (1000, 5000, ...) would never see it.
 */

/* Returns the bucket a single value lands in, read back out through
   perf_hist_pct(..., 100) on a histogram containing only that one value
   (p100 of one sample is exactly that sample's bucket's upper edge). */
static uint32_t bucket_upper_of(uint32_t us)
{
    perf_hist_t h;
    memset(&h, 0, sizeof(h));
    perf_hist_add(&h, us);
    return perf_hist_pct(&h, 100);
}

static void test_bucket_boundaries(void)
{
    section("bucket boundaries");

    /* bucket 0 covers [0, 1024): 0, 1, and everything up to 1023 must
       share its upper edge (1024). us == 0 is the documented special
       case (clz(0) is UB in C, must not be computed). */
    CHECK(bucket_upper_of(0) == 1024, "us=0 must land in bucket 0 (got upper=%u)",
          bucket_upper_of(0));
    CHECK(bucket_upper_of(1) == 1024, "us=1 must land in bucket 0");
    CHECK(bucket_upper_of(1023) == 1024, "us=1023 is the last value in bucket 0");

    /* The 2^k / 2^k - 1 / 2^k + 1 triad around every finite edge from
       1024 (2^10) through 524288 (2^19) -- bucket i's edge is 1024<<i,
       i.e. 2^(10+i). This is the part an off-by-one in `bit - 9` would
       break: 2^k - 1 must stay in the bucket BELOW the edge, 2^k must be
       the first value IN the bucket whose edge is 2^k, and there must be
       no gap or overlap for 2^k + 1. */
    {
        int i;
        for (i = 0; i <= 9; i++) {
            uint32_t edge = 1024u << i;          /* 2^(10+i) */
            uint32_t edge_next = 1024u << (i + 1);
            char ctx[64];
            snprintf(ctx, sizeof ctx, "edge 2^%d (=%u)", 10 + i, edge);
            g_ctx = ctx;

            CHECK(bucket_upper_of(edge - 1) == edge,
                  "%u (edge-1) must be the top of the bucket below the edge", edge - 1);
            CHECK(bucket_upper_of(edge) == edge_next,
                  "%u (the edge itself) must be the FIRST value of the next bucket", edge);
            CHECK(bucket_upper_of(edge + 1) == edge_next,
                  "%u (edge+1) must share the same bucket as the edge", edge + 1);
        }
    }
    g_ctx = "bucket boundaries";

    /* Saturation into the open-ended top bucket (index 11, upper edge
       has no finite value -- must be exactly the documented sentinel,
       UINT32_MAX, and every value from the last finite edge upward must
       land there, including UINT32_MAX itself, without wrapping the
       bucket index off the end of the array (that would be a buffer
       overrun the test would only catch via a crash, not a wrong
       number -- so this is checked explicitly). */
    CHECK(bucket_upper_of(1048576u) == UINT32_MAX,   /* 2^20: first value in the top bucket */
          "2^20 must saturate into the top open-ended bucket");
    CHECK(bucket_upper_of(1048576u + 1) == UINT32_MAX,
          "2^20 + 1 must stay in the top bucket");
    CHECK(bucket_upper_of(UINT32_MAX) == UINT32_MAX,
          "UINT32_MAX itself must saturate cleanly, not overflow the bucket index");
}

/* ================================================================== */
/* 2. p50 / p99
 *
 *    Defect this catches: reading the WRONG edge (lower instead of the
 *    documented upper), or an off-by-one in the ceiling-division target
 *    that under-counts how many samples must be covered.
 */
static void test_percentiles(void)
{
    perf_hist_t h;

    section("percentiles: empty histogram");
    memset(&h, 0, sizeof(h));
    CHECK(perf_hist_pct(&h, 50) == 0, "empty histogram p50 must be 0 (documented sentinel)");
    CHECK(perf_hist_pct(&h, 99) == 0, "empty histogram p99 must be 0 (documented sentinel)");

    section("percentiles: single sample");
    memset(&h, 0, sizeof(h));
    perf_hist_add(&h, 1500);              /* bucket 1: [1024, 2048), upper edge 2048 */
    CHECK(perf_hist_pct(&h, 1) == 2048, "1 sample: p1 must be that sample's bucket edge");
    CHECK(perf_hist_pct(&h, 50) == 2048, "1 sample: p50 must be that sample's bucket edge");
    CHECK(perf_hist_pct(&h, 99) == 2048, "1 sample: p99 must be that sample's bucket edge");
    CHECK(perf_hist_pct(&h, 100) == 2048, "1 sample: p100 must be that sample's bucket edge");

    section("percentiles: all in one bucket");
    memset(&h, 0, sizeof(h));
    {
        int i;
        for (i = 0; i < 40; i++)
            perf_hist_add(&h, 100 + i);   /* all inside bucket 0, edge 1024 */
    }
    CHECK(perf_hist_pct(&h, 50) == 1024, "all-same-bucket p50 must be that bucket's edge");
    CHECK(perf_hist_pct(&h, 99) == 1024, "all-same-bucket p99 must be that bucket's edge");

    section("percentiles: straddling a boundary");
    memset(&h, 0, sizeof(h));
    /* 9 samples in bucket 0 (edge 1024), 1 sample far into bucket 5 (edge
       32768) -- 10 total, 90% light. p50 must stay in the light bucket;
       p99 must reach the one heavy sample. This is the case an off-by-one
       in the ceiling-division target gets wrong: with the documented
       ceiling, target = ceil(10*99/100) = 10, so the cumulative count
       must include ALL 10 samples (light bucket alone only reaches 9) and
       the walk continues into the heavy bucket. A WRONG floor-rounded
       target (total*pct/100 = 9) would stop at the light bucket's
       cumulative of exactly 9 and silently hide the outlier -- p99 would
       read 1024 instead of 32768. */
    {
        int i;
        for (i = 0; i < 9; i++)
            perf_hist_add(&h, 500);       /* bucket 0 */
        perf_hist_add(&h, 20000);         /* bucket 5: [16384,32768), edge 32768 */
    }
    CHECK(perf_hist_pct(&h, 50) == 1024, "p50 of 10 samples (9 light + 1 heavy) stays light");
    CHECK(perf_hist_pct(&h, 99) == 32768,
          "p99 of 10 samples (ceil(10*0.99)=10, needs every sample) must reach the heavy outlier");
    CHECK(perf_hist_pct(&h, 100) == 32768, "p100 must reach the single heaviest sample");

    section("percentiles: mass at the top (overflow) bucket");
    memset(&h, 0, sizeof(h));
    {
        int i;
        for (i = 0; i < 5; i++)
            perf_hist_add(&h, 100);       /* bucket 0 */
        for (i = 0; i < 95; i++)
            perf_hist_add(&h, 5000000u);  /* saturates into the top bucket */
    }
    CHECK(perf_hist_pct(&h, 50) == UINT32_MAX,
          "p50 dominated by the overflow bucket must return the open-ended sentinel");
    CHECK(perf_hist_pct(&h, 99) == UINT32_MAX, "p99 likewise");
    /* But p1 (well within the 5% light mass) must NOT be swallowed by the
       overflow bucket -- this is the check that the walk starts from
       bucket 0, not from the tail. */
    CHECK(perf_hist_pct(&h, 1) == 1024, "p1 must still resolve to the light bucket");
}

/* ================================================================== */
/* 3. Outlier discarding (perf_guard_t), spec rule 7.
 *
 *    Defect this catches: silently dropping outliers without counting
 *    them (rule 7 exists specifically because that happened once,
 *    unnoticed, right after the rule-6 fix) or discarding a value that
 *    is exactly AT the limit (the spec says "超" -- strictly over).
 */
static void test_guard_outliers(void)
{
    perf_guard_t g;

    section("guard: exactly-at-limit is accepted, limit+1 is discarded");
    perf_guard_init(&g, 100000u); /* e.g. the 100 ms cache-stall ceiling */

    perf_guard_add(&g, 100000u);           /* == limit: must be accepted */
    CHECK(g.discarded == 0, "value == limit must not be discarded");
    CHECK(g.max_us == 100000u, "accepted value must update max_us");
    {
        uint32_t total = 0;
        int i;
        for (i = 0; i < PERF_HIST_BUCKETS; i++)
            total += g.hist.bucket[i];
        CHECK(total == 1, "exactly-at-limit sample must be counted in the histogram");
    }

    perf_guard_add(&g, 100001u);           /* limit + 1: must be discarded */
    CHECK(g.discarded == 1, "value == limit+1 must increment discarded exactly once");
    CHECK(g.max_us == 100000u, "a discarded value must not move max_us");
    {
        uint32_t total = 0;
        int i;
        for (i = 0; i < PERF_HIST_BUCKETS; i++)
            total += g.hist.bucket[i];
        CHECK(total == 1, "discarded sample must NOT increase the histogram's total count");
    }

    section("guard: many discards keep accumulating, never silently capped");
    {
        int i;
        for (i = 0; i < 50; i++)
            perf_guard_add(&g, 500000u);   /* way over limit */
    }
    CHECK(g.discarded == 51, "51 out-of-range samples (1 earlier + 50 here) must all be counted");
}

/* ================================================================== */
/* 4. Worst-16 ring: <16 entries, exactly 16, and the eviction/ordering
 *    rule after it wraps.
 *
 *    Defect this catches: an eviction rule that drops the WORST sample
 *    instead of the lightest once the ring is full (silently turning
 *    "worst 16" into "some arbitrary 16"), or that gets the tie rule
 *    backwards (keeping the older sample instead of the documented
 *    "newer wins on a tie").
 */
static void test_ring(void)
{
    perf_ring_t r;

    section("ring: fewer than 16 entries");
    perf_ring_init(&r);
    CHECK(perf_ring_len(&r) == 0, "freshly initialized ring must be empty");
    {
        uint32_t vals[5] = { 30, 10, 50, 20, 40 };
        int i;
        for (i = 0; i < 5; i++)
            perf_ring_push(&r, vals[i], NULL, 0);
    }
    CHECK(perf_ring_len(&r) == 5, "5 pushes into an unfilled ring must all be kept");
    /* Ascending by total_us: 10, 20, 30, 40, 50 */
    {
        static const uint32_t expect[5] = { 10, 20, 30, 40, 50 };
        int i;
        for (i = 0; i < 5; i++) {
            const perf_ring_entry_t *e = perf_ring_at(&r, (uint32_t)i);
            CHECK(e != NULL && e->total_us == expect[i],
                  "index %d must be %u ascending, got %u", i, expect[i],
                  e ? e->total_us : 0xffffffffu);
        }
    }
    CHECK(perf_ring_at(&r, 5) == NULL, "reading past len must return NULL, not garbage");

    section("ring: exactly 16 entries");
    perf_ring_init(&r);
    {
        int i;
        /* Push 1..16 in a scrambled order so a correctness bug that only
           happens to work for already-sorted input would not hide. */
        static const uint32_t vals[16] = {
            8, 3, 15, 1, 11, 6, 16, 2, 9, 14, 4, 12, 7, 10, 5, 13
        };
        for (i = 0; i < 16; i++)
            perf_ring_push(&r, vals[i], NULL, 0);
    }
    CHECK(perf_ring_len(&r) == 16, "16 pushes into an empty ring must fill it exactly");
    {
        int i;
        for (i = 0; i < 16; i++) {
            const perf_ring_entry_t *e = perf_ring_at(&r, (uint32_t)i);
            CHECK(e != NULL && e->total_us == (uint32_t)(i + 1),
                  "index %d must hold value %d ascending", i, i + 1);
        }
    }

    section("ring: eviction after it is full (worst-16 rule, not FIFO)");
    /* Ring currently holds 1..16 (full). Push a value LIGHTER than the
       current lightest (1): must be dropped, ring unchanged. This is the
       case that distinguishes "worst 16" from a plain circular buffer --
       a FIFO ring would happily evict the OLDEST (value 8, pushed first)
       and admit this new light sample, which is exactly the bug this
       check exists to catch. */
    perf_ring_push(&r, 0, NULL, 0);
    CHECK(perf_ring_len(&r) == 16, "ring must stay full");
    {
        const perf_ring_entry_t *e0 = perf_ring_at(&r, 0);
        CHECK(e0 != NULL && e0->total_us == 1,
              "a sample lighter than everything held must be dropped, not admitted");
    }

    /* Push a value heavier than everything held (100): must evict the
       CURRENT lightest (1), not the oldest push, not the current
       heaviest. */
    perf_ring_push(&r, 100, NULL, 0);
    CHECK(perf_ring_len(&r) == 16, "ring must stay full");
    {
        const perf_ring_entry_t *e0 = perf_ring_at(&r, 0);
        const perf_ring_entry_t *e15 = perf_ring_at(&r, 15);
        CHECK(e0 != NULL && e0->total_us == 2,
              "evicting the lightest (1) must leave 2 as the new lightest");
        CHECK(e15 != NULL && e15->total_us == 100,
              "the new heaviest sample must land at the top of the ring");
    }

    section("ring: tie at the eviction boundary keeps the NEWER sample");
    /* Ring's lightest is currently 2 (seq from the very first fill).
       Push another sample whose total_us is also 2, tagged via its
       stage_us[0] so we can tell them apart after the fact. The
       documented rule says a tie is NOT strictly lighter, so it must
       evict the old '2' and the ring must hold the NEW one. */
    {
        uint32_t stage[PERF_RING_STAGES];
        memset(stage, 0, sizeof stage);
        stage[0] = 0xABCDu; /* tag: identifies "the new push" */
        perf_ring_push(&r, 2, stage, 1);
    }
    {
        const perf_ring_entry_t *e0 = perf_ring_at(&r, 0);
        CHECK(e0 != NULL && e0->total_us == 2, "the lightest slot must still read 2");
        CHECK(e0 != NULL && e0->stage_us[0] == 0xABCDu,
              "a tie at the boundary must keep the NEWER sample (tagged), not the older one");
    }
}

/* ================================================================== */
/* 5. Core mismatch guard (perf_span_t / perf_span_enter / perf_span_exit_us).
 *
 *    THE defect this catches: native-editor-spec.md section C.0 rule 6 --
 *    M0a subtracted two different cores' cycle-count CSRs and reported a
 *    stall of 10,177,430 us. This section names that failure mode
 *    directly: entering a span "on core 0" and reading it back "on core
 *    1" must be structurally incapable of producing a subtracted number.
 */
static void test_span_core_guard(void)
{
    perf_span_t span;
    uint32_t us;

    section("span: normal same-core usage computes the real duration");
    perf_host_set_core_id(0);
    perf_host_set_cycle_count(1000);
    span = perf_span_enter();
    perf_host_set_cycle_count(4600);           /* +3600 cycles, same core */
    us = perf_span_exit_us(span, 1 /* mhz=1: host cycles map 1:1 to "us" */);
    CHECK(us == 3600, "same-core span must compute the plain cycle difference, got %u", us);
    CHECK(us != PERF_SPAN_CORE_MISMATCH, "a valid same-core measurement must not read as the mismatch sentinel");

    section("span: THE M0a bug, reproduced and proven caught");
    /* Enter as if this were core 0 (e.g. the flash-stall wrap's disable
       hook, or an edit_task stage that happened to run there)... */
    perf_host_set_core_id(0);
    perf_host_set_cycle_count(1000);
    span = perf_span_enter();
    /* ...then read the exit as if a DIFFERENT core's CSR were being
       consulted, with a huge, wrapped-looking cycle value -- this is
       exactly the shape that produced 10,177,430 us in the M0a
       postmortem: two unrelated CSRs, naively subtracted. */
    perf_host_set_core_id(1);
    perf_host_set_cycle_count(50);             /* smaller than entry: a raw
                                                   subtraction would even
                                                   underflow to a huge
                                                   unsigned number */
    us = perf_span_exit_us(span, 1);
    CHECK(us == PERF_SPAN_CORE_MISMATCH,
          "cross-core exit must return the mismatch sentinel, not a subtracted (and here, "
          "underflowed) cycle count -- got %u", us);
    /* This is the "assert で落ちる" the assignment asks the test to name:
       a caller that forgets to handle the sentinel and instead treats it
       as a plain duration is expected to assert on it (this is exactly
       what the CHECK above does -- it is the assertion, and it fires
       precisely when the API returns the wrong thing). The API itself
       cannot be coaxed into calling __real subtraction across cores: the
       core check happens before any cycle arithmetic runs, so there is no
       code path that both (a) compiles and (b) computes a cross-core
       delta -- confirmed by inspection of perf_span_exit_us in
       perf_meter.c, and pinned here so a future refactor cannot
       reintroduce the unchecked subtraction without this test catching
       it. */
    CHECK(us != 3600 - 1000, "must never coincidentally equal a plausible-looking duration");

    section("span: mismatch is symmetric (core 1 -> core 0 also caught)");
    perf_host_set_core_id(1);
    perf_host_set_cycle_count(9000);
    span = perf_span_enter();
    perf_host_set_core_id(0);
    perf_host_set_cycle_count(9500);
    us = perf_span_exit_us(span, 1);
    CHECK(us == PERF_SPAN_CORE_MISMATCH, "the guard must not be one-directional, got %u", us);

    section("span: divide-by-zero cpu_mhz is defused, not crashed on");
    perf_host_set_core_id(0);
    perf_host_set_cycle_count(0);
    span = perf_span_enter();
    perf_host_set_cycle_count(500);
    us = perf_span_exit_us(span, 0);
    CHECK(us == 500, "cpu_mhz=0 must be treated as 1, not divide by zero");
}

/* ================================================================== */

int main(void)
{
    test_bucket_boundaries();
    test_percentiles();
    test_guard_outliers();
    test_ring();
    test_span_core_guard();

    printf("\n%s: %d checks passed, %d failed\n",
           g_fail ? "FAILURE" : "SUCCESS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
