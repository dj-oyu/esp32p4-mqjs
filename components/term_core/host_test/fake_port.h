/*
 * fake_port.h — the term_port_t stand-in for the phase-2 registry tests.
 *
 * term_port.h says the four platform services exist precisely so the
 * registry's rules can be exercised on the host: "On the host the tests
 * install a fake: a counting mutex, a settable clock, an inline 'UI task'
 * and plain malloc." This is that fake, plus the knobs the design's own
 * failure paths need:
 *
 *   - a mutex that can refuse a BOUNDED wait, so §5's "producer 側は
 *     bounded wait のみ … 20ms タイムアウトで drop+カウンタ" can be driven
 *     without a scheduler, and that counts recursive acquisition, which
 *     term_port.h says never happens;
 *   - a settable clock, so the 3s quiesce deadline (§3.1) and the
 *     detached-LRU age are reached in zero real time;
 *   - a UI post that is inline, deferred or refusing, so "the job runs at a
 *     frame boundary" (§7.2), "resize is posted, never written in place"
 *     (§5) and the TERM_ERR_POST path all become reachable;
 *   - an allocator that counts, can be made to fail, and can be FROZEN, so
 *     I4 ("nothing is allocated after create", §4.2) is a measurement
 *     rather than a hope.
 *
 * Header-only on purpose: run_tests.sh compiles one test_*.c against the
 * component sources one directory up and nothing else, so a fake_port.c
 * would never be built.
 *
 * Written against term_port.h and docs/term-design.md only.
 */
#ifndef TERM_FAKE_PORT_H
#define TERM_FAKE_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "term_port.h"

#define FP_MAX_JOBS     64
#define FP_MAX_BLOCKS   64
#define FP_MAX_SIGNALS  16

enum {
    FP_UI_INLINE = 0, /* ui_post runs the job immediately and returns true */
    FP_UI_DEFER,      /* ui_post queues; fp_pump() is the "frame boundary" */
    FP_UI_REFUSE      /* ui_post returns false (the TERM_ERR_POST path)    */
};

typedef struct {
    term_ui_job_fn fn;
    void          *arg;
} fp_job_t;

typedef struct {
    void  *p;
    size_t n;
} fp_block_t;

typedef struct {
    /* -- clock (§3.1 deadlines and LRU age) ------------------------- */
    int64_t now_ms;

    /* -- mutex ------------------------------------------------------ */
    int mutex_creates, mutex_destroys;
    int lock_calls, lock_ok, lock_fails, unlocks;
    int depth, max_depth, recursive_attempts;
    int forever_locks;          /* acquisitions asking TERM_WAIT_FOREVER */
    int bounded_locks;          /* acquisitions with a real timeout      */
    uint32_t last_timeout_ms;
    bool refuse_bounded;        /* every bounded wait fails (contention) */

    /* -- signal ----------------------------------------------------- */
    int signal_creates, signal_destroys, signal_sets;
    int signal_waits, signal_timeouts;

    /* -- UI task ---------------------------------------------------- */
    int  ui_mode;               /* FP_UI_*                              */
    bool ui_current;            /* what ui_is_current() answers         */
    int  posts, post_refusals, jobs_run;
    fp_job_t q[FP_MAX_JOBS];
    int qh, qt;

    /* -- memory (§4.2 / I4) ----------------------------------------- */
    int      allocs, frees, alloc_failures;
    size_t   live_bytes, last_size;
    unsigned last_flags;
    int      alloc_fail_next;   /* >0: fail this many upcoming allocs   */
    bool     alloc_frozen;      /* any alloc/free while set is a fault  */
    int      alloc_violations;
    fp_block_t blocks[FP_MAX_BLOCKS];
    int        nblocks;

    /* -- optional hooks --------------------------------------------- */
    int reaper_wakes, log_calls;
} fp_state_t;

static fp_state_t fp;

/* ===================================================================== */
/* Mutex                                                                 */
/* ===================================================================== */

static int fp_mutex_body;   /* one table lock is all the registry needs */

static void *fp_mutex_create(void)
{
    fp.mutex_creates++;
    return &fp_mutex_body;
}

static void fp_mutex_destroy(void *m)
{
    (void)m;
    fp.mutex_destroys++;
}

static bool fp_mutex_lock(void *m, uint32_t timeout_ms)
{
    (void)m;
    fp.lock_calls++;
    fp.last_timeout_ms = timeout_ms;
    if (timeout_ms == TERM_WAIT_FOREVER) fp.forever_locks++;
    else                                 fp.bounded_locks++;

    /* term_port.h: "Recursive acquisition never happens and need not be
     * supported." Record the attempt; a bounded one fails the way a real
     * non-recursive mutex would, an unbounded one is granted so the test
     * process cannot deadlock on somebody else's bug. */
    if (fp.depth > 0) {
        fp.recursive_attempts++;
        if (timeout_ms != TERM_WAIT_FOREVER) { fp.lock_fails++; return false; }
    }
    if (fp.refuse_bounded && timeout_ms != TERM_WAIT_FOREVER) {
        fp.lock_fails++;
        return false;
    }
    fp.depth++;
    if (fp.depth > fp.max_depth) fp.max_depth = fp.depth;
    fp.lock_ok++;
    return true;
}

static void fp_mutex_unlock(void *m)
{
    (void)m;
    fp.unlocks++;
    if (fp.depth > 0) fp.depth--;
}

/* ===================================================================== */
/* Clock                                                                 */
/* ===================================================================== */

static int64_t fp_now_ms(void) { return fp.now_ms; }

static inline void fp_advance(int64_t ms) { if (ms > 0) fp.now_ms += ms; }

/* ===================================================================== */
/* Signal                                                                */
/* ===================================================================== */

static int fp_sig_pool[FP_MAX_SIGNALS];
static int fp_sig_used[FP_MAX_SIGNALS];

static void *fp_signal_create(void)
{
    int i;
    fp.signal_creates++;
    for (i = 0; i < FP_MAX_SIGNALS; i++) {
        if (!fp_sig_used[i]) {
            fp_sig_used[i] = 1;
            fp_sig_pool[i] = 0;
            return &fp_sig_pool[i];
        }
    }
    return NULL;   /* the registry must cope; more than 16 concurrent
                    * waiters is not a thing in these tests */
}

static void fp_signal_destroy(void *s)
{
    int i;
    fp.signal_destroys++;
    for (i = 0; i < FP_MAX_SIGNALS; i++)
        if (s == &fp_sig_pool[i]) fp_sig_used[i] = 0;
}

static void fp_signal_set(void *s)
{
    fp.signal_sets++;
    if (s) *(int *)s = 1;
}

static bool fp_signal_wait(void *s, uint32_t timeout_ms)
{
    (void)timeout_ms;
    fp.signal_waits++;
    /* No scheduler here: a job that has already run (FP_UI_INLINE) has set
     * the latch, and a job still sitting in the queue (FP_UI_DEFER) has
     * not — which is exactly the "the UI never got to it" timeout of
     * §7.2. Tests choose the outcome by choosing the ui_mode. */
    if (s && *(int *)s) return true;
    fp.signal_timeouts++;
    return false;
}

/* ===================================================================== */
/* UI frame task                                                         */
/* ===================================================================== */

static bool fp_ui_post(term_ui_job_fn fn, void *arg, uint32_t timeout_ms)
{
    int next;
    (void)timeout_ms;
    fp.posts++;
    if (!fn) return false;
    if (fp.ui_mode == FP_UI_REFUSE) { fp.post_refusals++; return false; }
    if (fp.ui_mode == FP_UI_INLINE) { fp.jobs_run++; fn(arg); return true; }

    next = (fp.qt + 1) % FP_MAX_JOBS;
    if (next == fp.qh) { fp.post_refusals++; return false; }
    fp.q[fp.qt].fn = fn;
    fp.q[fp.qt].arg = arg;
    fp.qt = next;
    return true;
}

static bool fp_ui_is_current(void) { return fp.ui_current; }

/* Run every queued job — the host stand-in for one frame boundary. */
static inline int fp_pump(void)
{
    int n = 0;
    while (fp.qh != fp.qt) {
        fp_job_t j = fp.q[fp.qh];
        fp.qh = (fp.qh + 1) % FP_MAX_JOBS;
        fp.jobs_run++;
        n++;
        j.fn(j.arg);
    }
    return n;
}

static inline int fp_jobs_pending(void)
{
    return (fp.qt - fp.qh + FP_MAX_JOBS) % FP_MAX_JOBS;
}

/* ===================================================================== */
/* Memory (§4.2)                                                         */
/* ===================================================================== */

static void *fp_mem_alloc(size_t size, unsigned flags)
{
    void *p;
    fp.last_size = size;
    fp.last_flags = flags;
    if (fp.alloc_frozen) fp.alloc_violations++;
    if (fp.alloc_fail_next > 0) {
        fp.alloc_fail_next--;
        fp.alloc_failures++;
        return NULL;
    }
    if (fp.nblocks >= FP_MAX_BLOCKS) { fp.alloc_failures++; return NULL; }
    p = malloc(size ? size : 1);
    if (!p) { fp.alloc_failures++; return NULL; }
    /* heap_caps_malloc does not zero, so neither do we: a core or a
     * registry that leans on zeroed memory should fail here, not on the
     * device. */
    memset(p, 0xCD, size);
    fp.blocks[fp.nblocks].p = p;
    fp.blocks[fp.nblocks].n = size;
    fp.nblocks++;
    fp.allocs++;
    fp.live_bytes += size;
    return p;
}

static void fp_mem_free(void *ptr)
{
    int i;
    if (fp.alloc_frozen) fp.alloc_violations++;
    if (!ptr) return;
    for (i = 0; i < fp.nblocks; i++) {
        if (fp.blocks[i].p == ptr) {
            fp.live_bytes -= fp.blocks[i].n;
            fp.blocks[i] = fp.blocks[fp.nblocks - 1];
            fp.nblocks--;
            fp.frees++;
            free(ptr);
            return;
        }
    }
    /* A free of something we never handed out is a fault, not a no-op. */
    fp.alloc_violations++;
}

/* Release whatever the registry still holds (zombies, §3.1). Call at the
 * very end of main so a deliberate retention is not reported as a leak. */
static inline void fp_reclaim_all(void)
{
    while (fp.nblocks > 0) {
        fp.nblocks--;
        free(fp.blocks[fp.nblocks].p);
        fp.blocks[fp.nblocks].p = NULL;
    }
    fp.live_bytes = 0;
}

/* ===================================================================== */
/* Optional hooks                                                        */
/* ===================================================================== */

static void fp_reaper_wake(void) { fp.reaper_wakes++; }

static void fp_log(int level, const char *tag, const char *msg)
{
    (void)level; (void)tag; (void)msg;
    fp.log_calls++;
}

/* ===================================================================== */
/* The table                                                             */
/* ===================================================================== */

static const term_port_t fp_table = {
    fp_mutex_create,
    fp_mutex_destroy,
    fp_mutex_lock,
    fp_mutex_unlock,
    fp_now_ms,
    fp_signal_create,
    fp_signal_destroy,
    fp_signal_set,
    fp_signal_wait,
    fp_ui_post,
    fp_ui_is_current,
    fp_mem_alloc,
    fp_mem_free,
    fp_reaper_wake,
    fp_log,
};

static inline const term_port_t *fp_port(void) { return &fp_table; }

/* A complete table with both optional hooks left out — term_port.h says
 * reaper_wake and log "may be NULL". */
static const term_port_t fp_table_minimal = {
    fp_mutex_create,
    fp_mutex_destroy,
    fp_mutex_lock,
    fp_mutex_unlock,
    fp_now_ms,
    fp_signal_create,
    fp_signal_destroy,
    fp_signal_set,
    fp_signal_wait,
    fp_ui_post,
    fp_ui_is_current,
    fp_mem_alloc,
    fp_mem_free,
    NULL,
    NULL,
};

static inline const term_port_t *fp_port_minimal(void) { return &fp_table_minimal; }

/* Fresh counters, clock at a non-zero value (term_port.h: the clock "need
 * not start at 0", and a registry that treats 0 as "never detached" must
 * not be accidentally satisfied by a zero clock). */
static inline void fp_reset(void)
{
    int i;
    /* Anything still outstanding belongs to a registry that has already
     * been deinited — a deliberately retained ZOMBIE block, say. Release
     * it here rather than losing the pointer in the memset, so a real leak
     * elsewhere is not masked by bookkeeping noise. */
    fp_reclaim_all();
    memset(&fp, 0, sizeof fp);
    for (i = 0; i < FP_MAX_SIGNALS; i++) { fp_sig_used[i] = 0; fp_sig_pool[i] = 0; }
    fp.now_ms = 100000;
    fp.ui_mode = FP_UI_INLINE;
    fp.ui_current = false;
}

#endif /* TERM_FAKE_PORT_H */
