/*
 * term_port — the thin platform seam under term_registry.
 *
 * docs/term-design.md phase 2 (§11.2). The registry's rules — the slot
 * table, the generation/owner gate, the persist namespace, the LRU evict,
 * the asynchronous two-stage quiesce (§3.1) — are pure logic and must be
 * testable on the host, exactly like the parser was in phase 1. The only
 * things standing between that logic and a host compiler are four
 * platform services:
 *
 *   1. a mutex with a BOUNDED wait (§5: "producer 側は bounded wait のみ",
 *      JS/print ingest gives up after 20ms and counts a drop),
 *   2. a monotonic clock (§3.1: the 3s quiesce cap, the detached-LRU age),
 *   3. a way to run work on the UI frame task (§5: parse, resize and
 *      snapshot all happen there so the grid stays single-writer; §7.2:
 *      snapshots are serialised at a frame boundary), and
 *   4. a coarse allocator for the one per-term block (§4.2: everything is
 *      taken in a single allocation at term.create() and never again).
 *
 * So those four are function pointers, installed once at boot. On the
 * device term_port_freertos() supplies FreeRTOS mutexes, esp_timer, the
 * ui_tab5 command path and heap_caps_malloc(MALLOC_CAP_SPIRAM). On the
 * host the tests install a fake: a counting mutex, a settable clock, an
 * inline "UI task" and plain malloc. The registry itself never learns
 * which one it got.
 *
 * NOT a general abstraction layer. There is deliberately no task
 * creation, no queue, no timer and no file I/O here: the registry starts
 * no tasks of its own. Its two off-thread bodies of work are the reaper
 * pass (term_registry_reap) and the frame drain (term_registry_ui_drain),
 * and both are plain functions that the platform's existing tasks call.
 * Adding a "spawn a task" hook would be the first step towards a second
 * scheduler, which this project does not need.
 *
 * This header is C99 and pulls in nothing but <stdint.h>/<stddef.h>/
 * <stdbool.h> — no ESP-IDF, no FreeRTOS, no LVGL. Same rule as
 * term_core.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/* Waits                                                                 */
/* ===================================================================== */

/* Block indefinitely. Legal for platform-internal callers that are known
 * not to be on js_task; ingest paths must NOT use it (§5). */
#define TERM_WAIT_FOREVER 0xFFFFFFFFu

/* Do not block at all: acquire or fail immediately. */
#define TERM_WAIT_NONE    0u

/* Defaults the registry uses when its config leaves them at 0. Here
 * rather than in term_registry.h because they are timing policy, i.e.
 * properties of the port's world. */
#define TERM_INGEST_TIMEOUT_MS_DEFAULT   20u   /* §5: JS/print drop cap   */
#define TERM_QUIESCE_TIMEOUT_MS_DEFAULT  3000u /* §3.1: ZOMBIE after this */
#define TERM_READ_TIMEOUT_MS_DEFAULT     200u  /* §7.2: post+join cap     */

/* ===================================================================== */
/* Memory (§4.1 placement, §4.2 "one allocation, then never again")      */
/* ===================================================================== */

enum {
    /* Default. grid + scrollback + byte ring go to PSRAM: §4.3 measures
     * the grid at under 3% of a full-repaint frame's traffic, and the L2
     * SRAM reclaimed by the size diet is not to be spent here (§2.7). */
    TERM_MEM_PSRAM    = 0,
    /* Escape hatch for a future profile-driven move of one allocation
     * (§4.3: "確保は 1 箇所にし、実機プロファイルで grid が見えたら動かす").
     * Nothing in phase 2 passes it. */
    TERM_MEM_INTERNAL = 1u << 0,
};

/* ===================================================================== */
/* UI task                                                               */
/* ===================================================================== */

/* A unit of work for the UI frame task. `arg` points at storage the
 * poster keeps alive until the job has run (the registry uses per-slot
 * storage, never the heap — §4.2). */
typedef void (*term_ui_job_fn)(void *arg);

/* ===================================================================== */
/* The port                                                              */
/* ===================================================================== */

/*
 * Every non-optional hook must be non-NULL; term_port_install() refuses a
 * table that is not complete. Hooks are called from several tasks at once
 * and must be thread-safe. None of them may call back into the registry.
 */
typedef struct {
    /* -- mutex ------------------------------------------------------- */
    /* One mutex protects the whole slot table; per-term data is reached
     * only while holding it, or from the UI task under the DYING rules of
     * §3.1. Recursive acquisition never happens and need not be
     * supported. Priority inheritance is desirable on the device (the UI
     * task must not be blocked by a low-priority JS worker) but is not
     * part of the contract. */
    void *(*mutex_create)(void);
    void  (*mutex_destroy)(void *mutex);
    /* true when acquired. timeout_ms may be TERM_WAIT_NONE or
     * TERM_WAIT_FOREVER. A false return is a normal, expected outcome on
     * ingest paths and must never be treated as an error by the port. */
    bool  (*mutex_lock)(void *mutex, uint32_t timeout_ms);
    void  (*mutex_unlock)(void *mutex);

    /* -- clock ------------------------------------------------------- */
    /* Monotonic milliseconds since boot. Must not go backwards; need not
     * start at 0. Used for the quiesce deadline and the detached-LRU age
     * (§3.1) — never for wall-clock time. */
    int64_t (*now_ms)(void);

    /* -- signal (bounded join for reads) ----------------------------- */
    /* A single binary latch: set once, waited on once. Used only by
     * term_registry_snapshot/read, which post a serialiser to the UI task
     * and then wait with a cap (§7.2). The quiesce protocol deliberately
     * uses NO join at all (§3.1: stage 1 returns immediately, stage 2 is
     * the polling reaper), so a signal never blocks js_task on a
     * teardown. Created and destroyed on the waiter's stack lifetime;
     * a wait that times out must leave the latch safe to destroy after
     * the registry has retired the job (the registry guarantees the job
     * storage outlives the timeout — see term_registry.h). */
    void *(*signal_create)(void);
    void  (*signal_destroy)(void *sig);
    void  (*signal_set)(void *sig);
    bool  (*signal_wait)(void *sig, uint32_t timeout_ms); /* true if set */

    /* -- UI frame task ----------------------------------------------- */
    /* Queue `fn(arg)` for the UI frame task. Returns false if the queue
     * is full or the UI is not running; the caller then reports an error
     * rather than blocking (the registry counts post failures). */
    bool (*ui_post)(term_ui_job_fn fn, void *arg, uint32_t timeout_ms);
    /* True when the calling task IS the UI frame task. Callers that are
     * already there run the job inline instead of posting to themselves —
     * which is both faster and the only way to avoid a self-deadlock in
     * the snapshot path. */
    bool (*ui_is_current)(void);

    /* -- memory (§4.2) ----------------------------------------------- */
    /* 8-byte aligned at minimum (term_core_init requires it). Called
     * exactly once per term, at create; freed exactly once, by the reaper
     * (§3.1 stage 2) — or never, when the slot becomes a ZOMBIE. Must
     * return NULL on failure, never abort. */
    void *(*mem_alloc)(size_t size, unsigned flags);
    void  (*mem_free)(void *ptr);

    /* -- optional ---------------------------------------------------- */
    /* Nudge the reaper task so a DYING slot is collected promptly instead
     * of at the next periodic pass. May be NULL: correctness must not
     * depend on it, only latency (§3.1: "ack が遅れて届けば reaper が
     * 後から回収する"). */
    void (*reaper_wake)(void);
    /* Diagnostics. May be NULL. Not variadic on purpose — a fake in a
     * host test should be three lines. `level` follows ESP_LOG_*:
     * 1=error, 2=warn, 3=info, 4=debug. */
    void (*log)(int level, const char *tag, const char *msg);
} term_port_t;

/* ===================================================================== */
/* Installation                                                          */
/* ===================================================================== */

/*
 * Install the port. `p` must have static lifetime — the pointer is
 * retained, not the contents copied. Returns false (and changes nothing)
 * if any mandatory hook is NULL.
 *
 * Call before term_registry_init(). Installing a different port while a
 * registry is live is a programming error: the registry holds a mutex and
 * memory that came from the old one. Host tests call term_registry_deinit()
 * first, which is the only sanctioned way to swap.
 */
bool term_port_install(const term_port_t *p);

/* The installed port, or NULL. */
const term_port_t *term_port_get(void);

/* True once a complete port is installed. */
bool term_port_installed(void);

/* Validate without installing: same completeness check, no side effects.
 * Lets a port implementation assert itself at boot. */
bool term_port_valid(const term_port_t *p);

/*
 * The device port: FreeRTOS mutex, esp_timer clock, the ui_tab5 job path,
 * heap_caps_malloc(MALLOC_CAP_SPIRAM). Defined only in the IDF build
 * (term_port_freertos.c); host tests never link it and install their own
 * fake instead. Returns a pointer to a static table, so
 * term_port_install(term_port_freertos()) is the whole boot sequence.
 */
const term_port_t *term_port_freertos(void);

#ifdef __cplusplus
}
#endif
