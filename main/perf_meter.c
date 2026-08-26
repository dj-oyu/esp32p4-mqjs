/*
 * perf_meter -- implementation. See perf_meter.h for the contract and the
 * reasoning; this file only has the arithmetic and the two tiny
 * platform-specific accessors (real core id / cycle count on ESP_PLATFORM,
 * settable fakes on a host so tools/tests/test_perf_hist.c can drive the
 * core-mismatch guard without hardware).
 *
 * Compiled on both sides: ESP-IDF component build (main/CMakeLists.txt)
 * and the host test (tools/tests/run_perf_tests.sh, same toolchain
 * fallback as components/fs_core/pc_test/run_tests.sh).
 */
#include "perf_meter.h"

#if PERF_METER_REAL

/* perf_hist_t must stay exactly 12 x uint32_t == 48 bytes (native-editor-
   spec.md section C.0 rule 3). A portable compile-time check (works under
   -std=c99, unlike _Static_assert which is C11): an array with a negative
   size is a compile error. */
typedef char perf_hist_size_check[(sizeof(perf_hist_t) == 48) ? 1 : -1];

#ifdef ESP_PLATFORM

#include "esp_cpu.h"

static inline uint32_t perf_core_id_now(void)
{
    return (uint32_t)esp_cpu_get_core_id();
}
static inline uint32_t perf_cycle_count_now(void)
{
    return (uint32_t)esp_cpu_get_cycle_count();
}

#else /* host */

/* Plain globals, not volatile/atomic: this is a single-threaded host test
   process, not a device with real concurrent cores. Production never
   compiles this branch. */
static uint32_t s_host_core_id = 0;
static uint32_t s_host_cycles = 0;

uint32_t perf_host_core_id(void) { return s_host_core_id; }
uint32_t perf_host_cycle_count(void) { return s_host_cycles; }
void perf_host_set_core_id(uint32_t core) { s_host_core_id = core; }
void perf_host_set_cycle_count(uint32_t cycles) { s_host_cycles = cycles; }

static inline uint32_t perf_core_id_now(void) { return s_host_core_id; }
static inline uint32_t perf_cycle_count_now(void) { return s_host_cycles; }

#endif /* ESP_PLATFORM */

/* -------------------------------------------------------------- histogram */

/* us == 0 -> bucket 0 (see header: clz(0) is UB, special-cased instead of
   computed). Otherwise: bit = floor(log2(us)) via 31 - clz(us) (one
   instruction on both riscv32 lzcnt and gcc's generic clz lowering), then
   shift down by 9 so bucket 0 covers everything below 1024 us and clamp
   into [0, PERF_HIST_BUCKETS - 1] so anything at or past the last finite
   edge saturates into the open-ended top bucket instead of indexing past
   the array. */
static int perf_hist_bucket_of(uint32_t us)
{
    int b;
    if (us == 0) {
        b = 0;
    } else {
        int bit = 31 - __builtin_clz(us);
        b = bit - 9;
        if (b < 0)
            b = 0;
    }
    if (b > PERF_HIST_BUCKETS - 1)
        b = PERF_HIST_BUCKETS - 1;
    return b;
}

/* Upper (exclusive) edge of bucket i, in microseconds. The top bucket has
   no finite edge (see header doc on perf_hist_pct) and returns UINT32_MAX. */
static uint32_t perf_hist_bucket_upper_us(int i)
{
    if (i >= PERF_HIST_BUCKETS - 1)
        return UINT32_MAX;
    return 1024u << i;
}

void perf_hist_add(perf_hist_t *h, uint32_t us)
{
    h->bucket[perf_hist_bucket_of(us)]++;
}

uint32_t perf_hist_pct(const perf_hist_t *h, int pct)
{
    uint32_t total = 0;
    int i;
    for (i = 0; i < PERF_HIST_BUCKETS; i++)
        total += h->bucket[i];
    if (total == 0)
        return 0; /* documented: empty histogram has no percentile */

    if (pct < 1)
        pct = 1;
    if (pct > 100)
        pct = 100;

    /* Ceiling division: guarantees the returned edge covers AT LEAST
       pct% of samples, never fewer (e.g. total=3, pct=50 must reach the
       2nd sample, not round down to the 1st). */
    {
        /* 64-bit intermediate: total is a free-running sample count and
           total*100 overflows uint32 at total = 42,949,672 (reachable on a
           device that runs for days). Found by injecting nothing — the
           adversarial pass probed the pristine code and bisected the edge. */
        uint32_t target = (uint32_t)(((uint64_t)total * (uint64_t)pct + 99u) / 100u);
        uint32_t cum = 0;
        for (i = 0; i < PERF_HIST_BUCKETS; i++) {
            cum += h->bucket[i];
            if (cum >= target)
                return perf_hist_bucket_upper_us(i);
        }
    }
    /* Unreachable: cum reaches total (== target's ceiling source) by the
       last bucket at the latest. Kept as a safe fallback rather than
       trusting that argument silently. */
    return perf_hist_bucket_upper_us(PERF_HIST_BUCKETS - 1);
}

/* ------------------------------------------------------------------ guard */

void perf_guard_init(perf_guard_t *g, uint32_t limit_us)
{
    int i;
    for (i = 0; i < PERF_HIST_BUCKETS; i++)
        g->hist.bucket[i] = 0;
    g->limit_us = limit_us;
    g->discarded = 0;
    g->max_us = 0;
}

void perf_guard_add(perf_guard_t *g, uint32_t us)
{
    if (us > g->limit_us) {
        /* rule 7: count, never silently drop */
        g->discarded++;
        return;
    }
    perf_hist_add(&g->hist, us);
    if (us > g->max_us)
        g->max_us = us;
}

/* ------------------------------------------------------------------- ring */

void perf_ring_init(perf_ring_t *r)
{
    int i, s;
    r->count = 0;
    r->next_seq = 0;
    for (i = 0; i < PERF_RING_LEN; i++) {
        r->e[i].total_us = 0;
        r->e[i].seq = 0;
        for (s = 0; s < PERF_RING_STAGES; s++)
            r->e[i].stage_us[s] = 0;
    }
}

void perf_ring_push(perf_ring_t *r, uint32_t total_us, const uint32_t *stage_us, int n_stages)
{
    perf_ring_entry_t cand;
    int i;

    cand.total_us = total_us;
    cand.seq = r->next_seq++;
    for (i = 0; i < PERF_RING_STAGES; i++)
        cand.stage_us[i] = (stage_us != NULL && i < n_stages) ? stage_us[i] : 0;

    if (r->count < PERF_RING_LEN) {
        /* Not full yet: every push is kept. Insertion-sort into ascending
           position by total_us (stable-ish: strictly-greater shifts, so
           equal existing entries stay before the new one -- consistent
           with "ties keep the newer at the boundary" once the ring fills,
           see below). */
        int pos = (int)r->count;
        while (pos > 0 && r->e[pos - 1].total_us > cand.total_us) {
            r->e[pos] = r->e[pos - 1];
            pos--;
        }
        r->e[pos] = cand;
        r->count++;
        return;
    }

    /* Full: only keep it if at least as heavy as the lightest of the
       current worst-16 (index 0). Strictly lighter adds no diagnostic
       value over what is already held, so it is dropped. */
    if (cand.total_us < r->e[0].total_us)
        return;

    /* Evict index 0 (lightest) and insert keeping ascending order. A tie
       with r->e[0].total_us passes the `<` check above (it is not
       strictly lighter), so it evicts the old lightest and the newer
       sample is the one kept -- matching the documented tie rule. */
    i = 0;
    while (i < PERF_RING_LEN - 1 && r->e[i + 1].total_us <= cand.total_us) {
        r->e[i] = r->e[i + 1];
        i++;
    }
    r->e[i] = cand;
}

uint32_t perf_ring_len(const perf_ring_t *r)
{
    return r->count;
}

const perf_ring_entry_t *perf_ring_at(const perf_ring_t *r, uint32_t i)
{
    if (i >= r->count)
        return NULL;
    return &r->e[i];
}

/* ------------------------------------------------------------------- span */

perf_span_t perf_span_enter(void)
{
    perf_span_t s;
    /* Order matters only in that both reads describe "now": which comes
       first does not affect correctness of the mismatch check below. */
    s.core_id = perf_core_id_now();
    s.t_enter_cycles = perf_cycle_count_now();
    return s;
}

uint32_t perf_span_exit_us(perf_span_t span, uint32_t cpu_mhz)
{
    uint32_t now_cycles;

    /* THE guard (native-editor-spec.md section C.0 rule 6): refuse to
       subtract two cycle counts unless both were read on the same core.
       This is checked before any arithmetic touches the cycle counts, so
       a cross-core call cannot produce a number -- correct or bogus --
       only this sentinel. */
    if (perf_core_id_now() != span.core_id)
        return PERF_SPAN_CORE_MISMATCH;

    if (cpu_mhz == 0)
        cpu_mhz = 1;

    now_cycles = perf_cycle_count_now();
    return (now_cycles - span.t_enter_cycles) / cpu_mhz;
}

#endif /* PERF_METER_REAL */
