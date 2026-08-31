#pragma once
/*
 * perf_meter -- the arithmetic core of打鍵計測 (native-editor-spec.md
 * section C.0, C.1, C.4). This header and perf_meter.c hold ONLY what can
 * be proven correct on a host: fixed-bucket histograms, a worst-N ring,
 * and a same-core-in/same-core-out timing guard. Everything ESP-specific
 * is confined to a thin #ifdef ESP_PLATFORM layer (reading the core id and
 * the cycle-count CSR) so the rest compiles and runs under plain host gcc
 * or clang for tools/tests/test_perf_hist.c.
 *
 * Explicitly NOT here (native-editor-plan.md section 2 keeps device wiring
 * with the supervisor for M0b): --wrap linker tricks, the sys.perf() JS
 * binding, ROM header regeneration, and any call-site wiring into
 * edit_task / flash_stall_meter / lvgl_port_lock. This module only proves
 * the numbers are computed correctly once someone hands it timestamps.
 *
 * Kconfig gate: CONFIG_MQJS_PERF_METER, default y (main/Kconfig.projbuild).
 * When it is off on an ESP_PLATFORM build, every function below compiles
 * to a no-op (static inline, optimized away) so call sites never need
 * their own #ifdef. On a host build the real implementation is always
 * available -- there is no Kconfig on a host, and the tests need the real
 * arithmetic regardless of what a device build would choose.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* A stale sdkconfig cache (no CONFIG_MQJS_PERF_METER symbol at all) is
   treated as "on", matching the Kconfig default of y -- silently losing
   the meter because a build directory is out of date is worse than
   carrying a few unused counters. */
#if defined(ESP_PLATFORM) && !defined(CONFIG_MQJS_PERF_METER)
#define CONFIG_MQJS_PERF_METER 1
#endif

/* 1 when the real arithmetic must exist: always on a host (tests need it),
   only when the Kconfig symbol is truthy on a device. */
#if !defined(ESP_PLATFORM) || CONFIG_MQJS_PERF_METER
#define PERF_METER_REAL 1
#else
#define PERF_METER_REAL 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Histogram: fixed 12 log2 buckets, exactly 48 bytes (native-editor-spec.md
 * section C.0 rule 3: "分布 1 本 = 48 B"). 12 * sizeof(uint32_t) == 48;
 * a compile-time check in perf_meter.c holds this fact down.
 *
 * Bucket edges. Insertion uses bit = 31 - clz(us) (one instruction, plus
 * the clamp below), so the edges fall on powers of two rather than clean
 * decimal ms marks -- that is what the spec's "相当" ("-ish") is about:
 *
 *   bucket  0: us in [0, 1024)                        "<1 ms"
 *   bucket  i (1..10): us in [1024<<(i-1), 1024<<i)    "<(2^i) ms"
 *   bucket 11: us >= 1024<<10 == 1,048,576              "≥1024 ms"  (open-ended)
 *
 * us == 0 is defined to land in bucket 0 (clz(0) is undefined behaviour in
 * plain C, so it is special-cased rather than computed).
 *
 * Types are defined unconditionally (whether or not the real arithmetic
 * is compiled in) so a struct embedding a perf_hist_t never changes shape
 * under the Kconfig toggle.
 * ------------------------------------------------------------------- */
#define PERF_HIST_BUCKETS 12

typedef struct {
    uint32_t bucket[PERF_HIST_BUCKETS];
} perf_hist_t;

/* ---------------------------------------------------------------------
 * Guard: validity ceiling + discard count wrapped around a perf_hist_t
 * (native-editor-spec.md section C.0 rule 7). Silently dropping outliers
 * makes the meter look more trustworthy than it is -- rule 7 exists
 * because that happened once already, right after the rule-6 fix, and the
 * source of that one discarded sample was never identified.
 * ------------------------------------------------------------------- */
typedef struct {
    perf_hist_t hist;    /* accepted samples only */
    uint32_t limit_us;   /* set at init; samples strictly greater are discarded */
    uint32_t discarded;  /* rule 7: count, never silent */
    uint32_t max_us;     /* max ACCEPTED sample (<= limit_us) */
} perf_guard_t;

/* ---------------------------------------------------------------------
 * Worst-N ring: the N=16 heaviest samples ever pushed, each with a
 * per-stage breakdown (native-editor-spec.md section C.0 rule 3: "最悪の
 * ときに何が起きていたか"). PERF_RING_STAGES=6 matches the known largest
 * user (edit's T_isr..T5 pipeline, section C.1); a caller with fewer
 * stages just leaves the trailing slots at 0.
 *
 * Eviction rule (must be explicit -- spec: "巻き込みの規則を明示"):
 *   - While count < PERF_RING_LEN: every push is kept, inserted so the
 *     array stays ascending by total_us (index 0 = lightest kept so far).
 *   - Once full: a push lighter than index 0 (the lightest of the current
 *     worst 16) is dropped -- the 16 already held are all at least as bad,
 *     so it adds no diagnostic value. A push >= index 0 evicts index 0 and
 *     is inserted keeping ascending order.
 *   - Ties (total_us equal to the current lightest) are KEPT, i.e. the
 *     newer sample replaces the older one at that value -- the more
 *     recent occurrence is more useful for "what was happening just now".
 *
 * Reading order: perf_ring_at(r, 0) is the LIGHTEST of the kept worst-16;
 * perf_ring_at(r, len-1) is the single worst sample ever seen.
 * ------------------------------------------------------------------- */
#define PERF_RING_LEN    16
#define PERF_RING_STAGES 6

typedef struct {
    uint32_t total_us;                    /* the ranking key; caller decides what it means */
    uint32_t stage_us[PERF_RING_STAGES];  /* per-stage breakdown, 0-filled past n_stages */
    uint32_t seq;                         /* insertion order, for reasoning about ties */
} perf_ring_entry_t;

typedef struct {
    perf_ring_entry_t e[PERF_RING_LEN];
    uint32_t count;     /* valid entries, 0..PERF_RING_LEN */
    uint32_t next_seq;
} perf_ring_t;

/* ---------------------------------------------------------------------
 * Span: the structural fix for the M0a core-mismatch bug (native-editor-
 * spec.md section C.0 rule 6). M0a's first cut wrote the entry timestamp
 * with one core's cycle-count CSR and read the exit timestamp with the
 * other's -- the two CSRs are not synchronized, and the naive subtraction
 * produced a reported stall of 10,177,430 us (10.18 s) for what was
 * really a normal flash op. flash_stall_meter.c's fix keeps the entry
 * timestamp in a per-core array indexed by esp_cpu_get_core_id(), which
 * works there because the enable/disable pair is tied to the core doing
 * the flash op. This module needs something that holds even when entry
 * and exit are NOT guaranteed to run on the fixed pair of call sites a
 * flash wrap gets for free.
 *
 * The fix: perf_span_t is a plain VALUE returned by perf_span_enter() and
 * threaded by the caller into perf_span_exit_us(). It carries the core id
 * it was entered on. perf_span_exit_us() re-reads the CURRENT core id and
 * compares before doing any arithmetic on the cycle counts -- so a
 * cross-core call can never produce a subtracted number, correct or not.
 * It can only ever produce the correct duration (same core both ends) or
 * the unmistakable PERF_SPAN_CORE_MISMATCH sentinel. There is no third
 * outcome, which is the whole point: "取り違えようがない形" (a form that
 * cannot be gotten wrong), not "the caller remembers to check". The
 * struct fields are public only because C has no encapsulation -- callers
 * must not read t_enter_cycles/core_id directly and subtract them by
 * hand; perf_span_exit_us() is the only function allowed to.
 *
 * tools/tests/test_perf_hist.c names this case directly: it enters a span
 * pretending to be core 0, switches the (host-only) fake "current core"
 * to 1, and asserts perf_span_exit_us() returns PERF_SPAN_CORE_MISMATCH
 * rather than some cycle-count difference -- i.e. the M0a bug's exact
 * shape is provably unreachable through this API.
 * ------------------------------------------------------------------- */
typedef struct {
    uint32_t t_enter_cycles;
    uint32_t core_id;
} perf_span_t;

/* Distinct from perf_hist_pct's UINT32_MAX ("bucket has no finite edge")
   -- this one means "the caller misused the API", not "the value is
   large". Both happen to be UINT32_MAX because there is no smaller value
   that is equally impossible to confuse with a real microsecond count on
   a 32-bit counter running at any plausible clock. */
#define PERF_SPAN_CORE_MISMATCH ((uint32_t)0xFFFFFFFFu)

#ifdef ESP_PLATFORM
/* P4 runs the CPU at a fixed 360 MHz (user decision: no dynamic
   frequency scaling here -- see the size-diet and ppa-bench notes).
   Device callers should use this rather than re-deriving 360 themselves;
   host tests pass their own divisor (typically 1) since the host's
   "cycle count" is not a real CPU cycle count. */
#define PERF_CPU_MHZ 360
#endif

#if PERF_METER_REAL

/* Insert one sample. Never fails, never discards -- validity limits and
   the "how many did we throw away" count (spec rule 7) live one level up,
   in perf_guard_t, because different call sites need different ceilings
   (100 ms for a cache stall, 1 s for an edit_task stage). A value past the
   last finite edge just saturates into bucket 11; it does not wrap or
   overflow. */
void perf_hist_add(perf_hist_t *h, uint32_t us);

/* p50 / p99 (or any 1..100). Returns the UPPER edge (exclusive boundary,
 * in microseconds) of the bucket that first reaches the percentile target
 * -- i.e. the returned value is an upper bound: at least `pct`% of the
 * recorded samples are <= it. This is the decision the spec asked to be
 * made once, written down, and locked by a test (section C.0 rule 3):
 * NOT the lower edge, NOT an interpolated estimate.
 *
 * The one bucket that has no finite upper edge (bucket 11, "≥1024 ms")
 * returns UINT32_MAX rather than a made-up number -- that is the "off the
 * chart" sentinel, distinct from PERF_SPAN_CORE_MISMATCH above only in
 * meaning, not in bit pattern.
 *
 * An empty histogram (no samples at all) returns 0. This is the same
 * value bucket 0's lower edge would produce, which is intentional: an
 * empty distribution genuinely has no p50, and 0 reads as "nothing to
 * report" rather than as a fabricated number in the middle of the range.
 */
uint32_t perf_hist_pct(const perf_hist_t *h, int pct);

void perf_guard_init(perf_guard_t *g, uint32_t limit_us);
/* us == limit_us is accepted ("100 ms超" in the spec means strictly over);
   us > limit_us increments discarded and does not touch the histogram. */
void perf_guard_add(perf_guard_t *g, uint32_t us);

void perf_ring_init(perf_ring_t *r);
void perf_ring_push(perf_ring_t *r, uint32_t total_us, const uint32_t *stage_us, int n_stages);
uint32_t perf_ring_len(const perf_ring_t *r);
/* i must be < perf_ring_len(r); NULL otherwise. Ordering: see above. */
const perf_ring_entry_t *perf_ring_at(const perf_ring_t *r, uint32_t i);

perf_span_t perf_span_enter(void);
/* cpu_mhz: cycles-to-microseconds divisor for the calling platform. 0 is
   treated as 1 (defensive: never let a caller's bad constant produce a
   division by zero on top of a stall meter that must never crash). */
uint32_t perf_span_exit_us(perf_span_t span, uint32_t cpu_mhz);

#ifndef ESP_PLATFORM
/* Host-only test hooks. Production (ESP_PLATFORM) builds never call
   these -- perf_span_enter/exit_us read the real esp_cpu_get_core_id()
   and esp_cpu_get_cycle_count() there, wired in perf_meter.c. These let
   tools/tests/test_perf_hist.c fabricate the "entered on core 0, read on
   core 1" scenario without hardware. */
uint32_t perf_host_core_id(void);
uint32_t perf_host_cycle_count(void);
void perf_host_set_core_id(uint32_t core);
void perf_host_set_cycle_count(uint32_t cycles);
#endif

#else /* !PERF_METER_REAL: CONFIG_MQJS_PERF_METER is off on this device build */

/* Every call below is a static-inline no-op, so device code that calls
   perf_meter functions unconditionally compiles either way and the
   compiler removes the calls entirely -- "config off -> the calls
   disappear" without every call site needing its own #ifdef. */
static inline void perf_hist_add(perf_hist_t *h, uint32_t us)
{
    (void)h; (void)us;
}
static inline uint32_t perf_hist_pct(const perf_hist_t *h, int pct)
{
    (void)h; (void)pct;
    return 0;
}
static inline void perf_guard_init(perf_guard_t *g, uint32_t limit_us)
{
    (void)g; (void)limit_us;
}
static inline void perf_guard_add(perf_guard_t *g, uint32_t us)
{
    (void)g; (void)us;
}
static inline void perf_ring_init(perf_ring_t *r)
{
    (void)r;
}
static inline void perf_ring_push(perf_ring_t *r, uint32_t total_us,
                                   const uint32_t *stage_us, int n_stages)
{
    (void)r; (void)total_us; (void)stage_us; (void)n_stages;
}
static inline uint32_t perf_ring_len(const perf_ring_t *r)
{
    (void)r;
    return 0;
}
static inline const perf_ring_entry_t *perf_ring_at(const perf_ring_t *r, uint32_t i)
{
    (void)r; (void)i;
    return NULL;
}
static inline perf_span_t perf_span_enter(void)
{
    perf_span_t s;
    s.t_enter_cycles = 0;
    s.core_id = 0;
    return s;
}
static inline uint32_t perf_span_exit_us(perf_span_t span, uint32_t cpu_mhz)
{
    (void)span; (void)cpu_mhz;
    return 0;
}

#endif /* PERF_METER_REAL */

#ifdef __cplusplus
}
#endif
