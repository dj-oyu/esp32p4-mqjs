/*
 * term_registry.c — the platform's fixed table of terminals.
 *
 * docs/term-design.md §11.2 (phase 2). The contract, the invariants and
 * the slot state machine are documented in term_registry.h; this file is
 * the implementation of exactly that and nothing more. Where a decision
 * was not fully determined by the header it is called out inline with a
 * "CHOICE:" note and repeated in PHASE2_MANIFEST.md, so a reviewer can
 * check it against §3.1 rather than discover it here.
 *
 * Pure logic over term_port: no ESP-IDF, no FreeRTOS, no LVGL, no
 * allocation except the one block per term that I4 allows. It compiles
 * with the host toolchain and is linked into the host test runner along
 * with term_core.c.
 *
 * ---------------------------------------------------------------------
 * CONCURRENCY, IN ONE PARAGRAPH
 * ---------------------------------------------------------------------
 * One mutex covers the whole table AND every term's core. Every path
 * that touches a core — ingest, drain/parse, resize, snapshot, read,
 * blit visit, reap — holds it. That is a deliberate choice over the
 * finer-grained "copy under the lock, parse outside it" scheme: with the
 * lock held for the parse there is no window in which the UI task is
 * inside a core the reaper could free, so the safety argument is one
 * sentence instead of a case analysis over the ack protocol. The cost is
 * that an ingest can wait behind one drain slice; §5 already bounds that
 * — ingest waits at most ingest_timeout_ms and then drops and counts,
 * and the drain slice is bounded by drain_budget_bytes.
 *
 * The two-stage quiesce is unaffected: stage 1 never joins (§3.1), and
 * the acks are still what lets stage 2 free a block.
 */
#include "term_registry.h"

#include <string.h>

/* §4.4's black box. The dependency is one-way and shallow: the registry
 * tees into the ring, the ring knows nothing about terms. Lock order is
 * registry -> ring, and since the ring takes no lock of its own there is no
 * second order to get wrong (term_lp_ring.h, SERIALISATION). */
#include "term_lp_ring.h"

/* ===================================================================== */
/* Tunables that are implementation detail, not contract                 */
/* ===================================================================== */

/* §4.1: "入力バイトリング 8KB / PSRAM". Producer -> parser. */
#define TERM_RING_BYTES 8192u

/* §5: "drain は 1 フレームあたりのバイト予算制". Leftovers carry to the
 * next frame, so one noisy term cannot eat the frame. */
#define TERM_DRAIN_BUDGET_DEFAULT 4096

/* Staging buffer for one drain slice. Small on purpose: it is .bss, and
 * §2.7 says the SRAM the size diet won back is not to be spent here. The
 * budget is honoured by looping over this buffer, not by sizing it. */
#define TERM_DRAIN_CHUNK 512u

/* Concurrent snapshot/read jobs in flight. Four: at most one per app
 * (worker cap 4) can be waiting at a time, and a caller that is already
 * the UI task uses no job slot at all. */
#define TERM_JOB_SLOTS 4

/* §4.1 scrollback budget: 48KB arena, 1,024 logical lines. */
#define TERM_SB_BYTES_DEFAULT (48u * 1024u)
#define TERM_SB_LINES_DEFAULT 1024

/*
 * CHOICE: control-path lock wait.
 * create/close/show/resize/find/producer_* run on js_task, which is
 * shared by every worker (§3.1) — an unbounded wait there is the very
 * failure mode the asynchronous quiesce exists to avoid. They are not
 * ingest either, so the 20 ms lossy cap of §5 would be wrong (a create
 * that fails because the UI happened to be parsing is a bug, not a
 * dropped log line). One second: long enough that only a genuinely stuck
 * holder reaches it, short enough that js_task always comes back.
 */
#define TERM_CONTROL_TIMEOUT_MS 1000u

/* ===================================================================== */
/* Slot                                                                  */
/* ===================================================================== */

typedef struct {
    /* Queued geometry change (§5: cols/rows are written only on the UI
     * task). Storage lives in the slot, never on a heap — I4. */
    bool      queued;   /* a job for this slot is in the UI queue */
    int       cols, rows;
    term_id_t id;       /* id, generation included, at post time */
} term_resize_job_t;

typedef struct {
    term_slot_state_t state;
    uint32_t          generation;  /* current generation of THIS slot */

    char        name[TERM_NAME_MAX];
    char        owner[TERM_OWNER_MAX];
    bool        persist;
    term_mode_t mode;

    void        *block;       /* the single allocation (I4) */
    size_t       block_size;
    term_core_t *core;        /* carved out of block */

    uint8_t *ring;            /* carved out of block, after the core */
    size_t   ring_size, ring_head, ring_tail, ring_used;

    term_view_t view;
    int         cols, rows;   /* geometry the UI task has actually applied */
    /* 履歴を何**表示行**さかのぼって見ているか。0 = 生きている画面。
       上限はレンダラだけが知っている (折り返しは現在の幅で決まり、
       走ってみるまで何行になるか分からない) ので、要求はここに置いて
       **届いた値をレンダラが書き戻す**。 */
    int         scroll;

    int64_t detached_since_ms; /* LRU key while DETACHED, else 0 */
    int64_t dying_since_ms;    /* stage-2 deadline base */

    /* Outstanding stage-2 acks (§3.1). */
    bool ui_ack_pending;
    bool prod_ack_pending;

    /* The single producer (§5 SPSC): term.pipe's ssh channel, or a fake
     * in the host suites. `prod.user` doubles as the producer's identity
     * on term_registry_producer_space/_write. */
    bool             piped;
    term_producer_t  prod;

    /* term.onReply (§6/§8). Only consulted when no producer claims the
     * reply. */
    term_reply_sink_fn reply_sink;
    void              *reply_user;

    /* §10.2's caret, accumulated during a drain pass and flushed once,
     * after the lock is dropped — "変化時のみ通知", coalesced per frame. */
    bool caret_pending;
    int  caret_col, caret_row;
    bool caret_vis;

    term_resize_job_t rz;

    /* §4.4's recording, R1-R5. `record` is the ONE copy of the state — the
     * core's callback pointer is derived from it and term_registry_recording
     * reports it — and it lives here, in .bss, so that R1's "never
     * remembered" is a property of where it is stored and not of a promise
     * anybody has to keep. Everything else is the debounce (R3(b)). */
    bool     record;
    bool     rec_settled;      /* this quiet episode has been evaluated   */
    bool     rec_deferred;     /* ...and the rate limit turned it away,
                                * counted once per episode                */
    bool     rec_hash_valid;
    uint32_t rec_hash;         /* screen hash of the last capture         */
    int64_t  rec_fed_ms;       /* last drain pass that moved bytes        */
    int64_t  rec_snap_ms;      /* last capture that landed                */

    uint64_t bytes_in;
    uint64_t bytes_dropped;
} term_slot_t;

/* ===================================================================== */
/* Read job (§7.2: serialise on the UI task, join with a cap)            */
/* ===================================================================== */

typedef enum { JOB_SNAPSHOT = 0, JOB_READ = 1 } term_job_kind_t;

typedef struct {
    bool in_use;
    bool done;
    bool abandoned;   /* the caller's wait expired; do not touch its
                       * buffer, its signal or its result */
    void *sig;

    term_job_kind_t kind;
    term_id_t       id;
    bool            have_owner;
    char            owner[TERM_OWNER_MAX];

    char  *out;       /* the CALLER's buffer: written only while the lock
                       * is held and only when !abandoned */
    size_t out_size;

    uint32_t from;
    int      n;

    /* results, job-owned so the caller copies them out after the join */
    term_err_t         err;
    size_t             out_len;
    term_read_result_t res;
} term_job_t;

/* ===================================================================== */
/* Registry state                                                        */
/* ===================================================================== */

static const term_port_t *s_p;
static void              *s_mutex;
static bool               s_ready;

static term_slot_t s_slots[TERM_SLOT_COUNT];
static term_job_t  s_jobs[TERM_JOB_SLOTS];

static int      s_persist_max;
static uint32_t s_quiesce_ms;
static uint32_t s_ingest_ms;
static uint32_t s_read_ms;
static int      s_drain_budget;
static int      s_cell_w, s_cell_h;

/* §10.2: one caret sink for the whole registry, installed by the platform.
 * Not per term and not per app — a terminal does not opt into having a
 * cursor. Read on the UI task after the lock is dropped. */
static term_caret_sink_fn s_caret_sink;
static void              *s_caret_sink_user;

static term_id_t s_console_id;

static term_registry_stats_t s_stats;

/* UI-task-only staging buffer for a drain slice. */
static uint8_t s_drain_buf[TERM_DRAIN_CHUNK];

/* One row of a screen capture, and the writer name it is attributed to.
 * Static rather than on the stack because the UI task's stack is not the
 * place for another 512 bytes, and safe to share because every caller holds
 * the table lock — the same serialisation term_lp_ring_append already
 * requires of its own staging buffer. The panic path has its own copies
 * below, for the obvious reason. */
static char s_rec_row[TERM_LP_REC_MAX];
static char s_rec_writer[TERM_LP_WRITER_MAX];

/* Recording lives in its own section further down (it needs term_core's
 * callback type and the slot layout), but the lifecycle transitions of R1 are
 * spread across this whole file and every one of them has to reach it. */
static void record_clear_locked(term_slot_t *sl, const char *why);

/* ===================================================================== */
/* Small helpers                                                         */
/* ===================================================================== */

const char *term_err_str(term_err_t err)
{
    switch (err) {
    case TERM_OK:            return "ok";
    case TERM_ERR_INVAL:     return "inval";
    case TERM_ERR_NOT_READY: return "not-ready";
    case TERM_ERR_BAD_ID:    return "bad-id";
    case TERM_ERR_STALE:     return "stale";
    case TERM_ERR_NOT_OWNER: return "not-owner";
    case TERM_ERR_DYING:     return "dying";
    case TERM_ERR_NO_SLOT:   return "no-slot";
    case TERM_ERR_NO_MEM:    return "no-mem";
    case TERM_ERR_EXISTS:    return "exists";
    case TERM_ERR_BUSY:      return "busy";
    case TERM_ERR_MODE:      return "mode";
    case TERM_ERR_TIMEOUT:   return "timeout";
    case TERM_ERR_POST:      return "post";
    case TERM_ERR_TRUNC:     return "trunc";
    }
    return "unknown";
}

static bool lock_for(uint32_t ms)
{
    return s_p && s_mutex && s_p->mutex_lock(s_mutex, ms);
}

static void unlock(void)
{
    s_p->mutex_unlock(s_mutex);
}

static void wake_reaper(void)
{
    if (s_p && s_p->reaper_wake)
        s_p->reaper_wake();
}

static term_id_t slot_id(const term_slot_t *sl)
{
    int idx = (int)(sl - s_slots);
    return (term_id_t)((sl->generation << TERM_SLOT_BITS) |
                       (uint32_t)(idx & TERM_SLOT_MASK));
}

static void gen_advance(term_slot_t *sl)
{
    sl->generation++;
    if (sl->generation > TERM_GEN_MAX) {
        sl->generation = 1;
        s_stats.gen_wraps++; /* I2: expected 0 for the life of the device */
    }
}

static bool str_ok(const char *s, size_t cap)
{
    size_t i;
    if (!s || !s[0])
        return false;
    for (i = 0; i < cap; i++)
        if (!s[i])
            return true;
    return false; /* no NUL within cap: too long — rejected, never
                   * truncated (term_registry.h I3 rationale) */
}

/*
 * The gate of I3, applied identically to reads and writes. `owner` NULL
 * means a platform-side caller that is exempt (force-close, the
 * introspection calls); it never means "skip the check for an app".
 */
static term_err_t slot_lookup(term_id_t id, const char *owner,
                              bool allow_dying, term_slot_t **out)
{
    term_slot_t *sl;

    if (id <= 0)
        return TERM_ERR_BAD_ID;
    sl = &s_slots[term_id_slot(id)];
    if (sl->state == TERM_SLOT_FREE)
        return TERM_ERR_BAD_ID;
    if (sl->generation != term_id_generation(id)) {
        s_stats.denied_stale++;
        return TERM_ERR_STALE;
    }
    if (owner && strncmp(sl->owner, owner, TERM_OWNER_MAX) != 0) {
        s_stats.denied_owner++;
        return TERM_ERR_NOT_OWNER;
    }
    if (!allow_dying &&
        (sl->state == TERM_SLOT_DYING || sl->state == TERM_SLOT_ZOMBIE))
        return TERM_ERR_DYING;
    *out = sl;
    return TERM_OK;
}

/* ===================================================================== */
/* Byte ring (§5)                                                        */
/* ===================================================================== */

static size_t ring_space(const term_slot_t *sl)
{
    return sl->ring_size - sl->ring_used;
}

static size_t ring_write(term_slot_t *sl, const uint8_t *p, size_t n)
{
    size_t first;

    if (n > ring_space(sl))
        n = ring_space(sl);
    if (!n)
        return 0;
    first = sl->ring_size - sl->ring_head;
    if (first > n)
        first = n;
    memcpy(sl->ring + sl->ring_head, p, first);
    if (n > first)
        memcpy(sl->ring, p + first, n - first);
    sl->ring_head = (sl->ring_head + n) % sl->ring_size;
    sl->ring_used += n;
    sl->bytes_in += n;
    return n;
}

static size_t ring_read(term_slot_t *sl, uint8_t *out, size_t n)
{
    size_t first;

    if (n > sl->ring_used)
        n = sl->ring_used;
    if (!n)
        return 0;
    first = sl->ring_size - sl->ring_tail;
    if (first > n)
        first = n;
    memcpy(out, sl->ring + sl->ring_tail, first);
    if (n > first)
        memcpy(out + first, sl->ring, n - first);
    sl->ring_tail = (sl->ring_tail + n) % sl->ring_size;
    sl->ring_used -= n;
    return n;
}

/* ===================================================================== */
/* Stage 1 of the quiesce (§3.1) — never joins                           */
/* ===================================================================== */

/*
 * Mark DYING and collect what the caller must do after it has dropped
 * the lock: asking a producer to detach may block briefly (the ssh
 * implementation shuts the socket down) and must not happen under the
 * table lock.
 */
static void stage1_locked(term_slot_t *sl,
                          term_producer_detach_fn *out_fn, void **out_user,
                          term_id_t *out_id)
{
    *out_fn = NULL;
    *out_user = NULL;
    *out_id = slot_id(sl);

    if (sl->state == TERM_SLOT_DYING || sl->state == TERM_SLOT_ZOMBIE)
        return; /* idempotent */

    /* R1 + R3(c): the term is going away, so this is the last chance to keep
     * the screen, and recording must not survive into the next occupant of
     * this slot. Before the state change, while the core is still ours. */
    record_clear_locked(sl, "close");

    sl->state = TERM_SLOT_DYING;
    sl->dying_since_ms = s_p->now_ms();
    sl->detached_since_ms = 0;
    sl->view.visible = false;
    sl->ui_ack_pending = true;
    sl->prod_ack_pending = sl->piped;
    if (sl->piped) {
        *out_fn = sl->prod.detach;
        *out_user = sl->prod.user;
    }
    s_stats.closes++;
}

/* Release a slot's memory and advance its generation (stage 2's tail,
 * and deinit's). Lock held. */
static void slot_release_locked(term_slot_t *sl)
{
    if (sl->state == TERM_SLOT_ZOMBIE && s_stats.zombie_bytes >= sl->block_size)
        s_stats.zombie_bytes -= sl->block_size;
    if (sl->block)
        s_p->mem_free(sl->block);
    sl->block = NULL;
    sl->block_size = 0;
    sl->core = NULL;
    sl->ring = NULL;
    sl->ring_size = sl->ring_head = sl->ring_tail = sl->ring_used = 0;
    sl->piped = false;
    memset(&sl->prod, 0, sizeof sl->prod);
    sl->reply_sink = NULL;
    sl->reply_user = NULL;
    sl->caret_pending = false;
    sl->ui_ack_pending = false;
    sl->prod_ack_pending = false;
    sl->persist = false;
    sl->view.visible = false;
    sl->detached_since_ms = 0;
    sl->dying_since_ms = 0;
    sl->name[0] = '\0';
    sl->owner[0] = '\0';
    sl->rz.queued = false;
    gen_advance(sl);
    sl->state = TERM_SLOT_FREE;
    s_stats.freed++;
}

/* ===================================================================== */
/* Registry lifecycle                                                    */
/* ===================================================================== */

term_err_t term_registry_init(const term_registry_config_t *cfg)
{
    int i;

    if (s_ready)
        return TERM_ERR_NOT_READY; /* deliberately idempotent-hostile */
    s_p = term_port_get();
    if (!s_p)
        return TERM_ERR_NOT_READY;

    /* §4.4's black box comes up before the first line can be logged.
     * Idempotent, and on the device app_main has already called it as its
     * first statement — this call is what gives the host suites and run_pc a
     * real ring instead of a stub. */
    term_lp_ring_boot();

    memset(s_slots, 0, sizeof s_slots);
    memset(s_jobs, 0, sizeof s_jobs);
    memset(&s_stats, 0, sizeof s_stats);
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        s_slots[i].state = TERM_SLOT_FREE;
        s_slots[i].generation = 1; /* I2: ids are always >= TERM_SLOT_COUNT */
    }
    s_console_id = TERM_ID_INVALID;

    s_persist_max  = (cfg && cfg->persist_max > 0)
                         ? cfg->persist_max : TERM_PERSIST_MAX_DEFAULT;
    s_quiesce_ms   = (cfg && cfg->quiesce_timeout_ms)
                         ? cfg->quiesce_timeout_ms
                         : TERM_QUIESCE_TIMEOUT_MS_DEFAULT;
    s_ingest_ms    = (cfg && cfg->ingest_timeout_ms)
                         ? cfg->ingest_timeout_ms
                         : TERM_INGEST_TIMEOUT_MS_DEFAULT;
    s_read_ms      = (cfg && cfg->read_timeout_ms)
                         ? cfg->read_timeout_ms : TERM_READ_TIMEOUT_MS_DEFAULT;
    s_drain_budget = (cfg && cfg->drain_budget_bytes > 0)
                         ? cfg->drain_budget_bytes : TERM_DRAIN_BUDGET_DEFAULT;
    s_cell_w = (cfg && cfg->cell_w > 0) ? cfg->cell_w : TERM_CELL_W_DEFAULT;
    s_cell_h = (cfg && cfg->cell_h > 0) ? cfg->cell_h : TERM_CELL_H_DEFAULT;

    s_mutex = s_p->mutex_create();
    if (!s_mutex)
        return TERM_ERR_NO_MEM;
    s_ready = true;

    if (cfg && cfg->console) {
        term_create_opts_t o = *cfg->console;
        term_err_t e;
        o.owner = TERM_OWNER_SYSTEM; /* forced, per term_registry.h */
        e = term_registry_create(&o, &s_console_id, NULL);
        if (e != TERM_OK) {
            s_console_id = TERM_ID_INVALID;
            s_ready = false;
            s_p->mutex_destroy(s_mutex);
            s_mutex = NULL;
            return e;
        }
    }
    return TERM_OK;
}

int term_registry_deinit(void)
{
    term_producer_detach_fn fns[TERM_SLOT_COUNT];
    void *users[TERM_SLOT_COUNT];
    term_id_t ids[TERM_SLOT_COUNT];
    int i, nfn = 0, uncollected = 0;

    if (!s_ready)
        return 0;

    /*
     * "Force every slot through stage 1, run the reaper to completion"
     * (term_registry.h). Three steps, in that order:
     *
     *   1. stage 1 on every occupied slot. The acks that deinit itself
     *      creates are satisfied here: there is no UI task and no
     *      producer task left to send them, and waiting on acks nobody
     *      can produce would report every slot as uncollectable.
     *   2. ask each bound producer to detach, with the lock down.
     *   3. reap. What is left over is exactly the set of slots that were
     *      ALREADY mid-quiesce when deinit ran — a close whose producer
     *      never acked. Those keep their memory: §3.1 chooses a leak
     *      over a use-after-free, and that choice does not change
     *      because the registry is going away. They are the number this
     *      function returns.
     */
    if (!lock_for(TERM_WAIT_FOREVER))
        return -1;
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        term_slot_t *sl = &s_slots[i];
        term_producer_detach_fn fn;
        void *user;
        term_id_t id;
        if (sl->state == TERM_SLOT_FREE)
            continue;
        stage1_locked(sl, &fn, &user, &id);
        sl->ui_ack_pending = false;
        if (fn) {
            fns[nfn] = fn;
            users[nfn] = user;
            ids[nfn] = id;
            nfn++;
        }
    }
    unlock();

    for (i = 0; i < nfn; i++)
        fns[i](users[i], ids[i]);

    if (!lock_for(TERM_WAIT_FOREVER))
        return -1;
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        term_slot_t *sl = &s_slots[i];
        if (sl->state == TERM_SLOT_FREE)
            continue;
        if (sl->prod_ack_pending) {
            uncollected++; /* memory deliberately retained */
            continue;
        }
        slot_release_locked(sl);
    }
    s_console_id = TERM_ID_INVALID;
    for (i = 0; i < TERM_JOB_SLOTS; i++)
        memset(&s_jobs[i], 0, sizeof s_jobs[i]);
    /* The caret sink goes with the table (§10.2): a host suite that builds
     * and destroys a registry per case would otherwise keep pointing at
     * the previous case's storage. */
    s_caret_sink = NULL;
    s_caret_sink_user = NULL;
    s_ready = false;
    unlock();
    s_p->mutex_destroy(s_mutex);
    s_mutex = NULL;
    return uncollected;
}

bool term_registry_ready(void)
{
    return s_ready;
}

/* ===================================================================== */
/* create / re-attach (§3.1, §8)                                         */
/* ===================================================================== */

static int persist_count_locked(void)
{
    int i, n = 0;
    for (i = 0; i < TERM_SLOT_COUNT; i++)
        if (s_slots[i].persist &&
            (s_slots[i].state == TERM_SLOT_LIVE ||
             s_slots[i].state == TERM_SLOT_DETACHED))
            n++;
    return n;
}

static term_slot_t *find_key_locked(const char *owner, const char *name)
{
    int i;
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        term_slot_t *sl = &s_slots[i];
        if (sl->state != TERM_SLOT_LIVE && sl->state != TERM_SLOT_DETACHED)
            continue;
        if (strncmp(sl->owner, owner, TERM_OWNER_MAX) == 0 &&
            strncmp(sl->name, name, TERM_NAME_MAX) == 0)
            return sl;
    }
    return NULL;
}

/* ===================================================================== */
/* What the core calls back into (§6 replies, §10.2 caret)               */
/* ===================================================================== */

/*
 * Both of these run INSIDE term_core_feed(), i.e. on the UI task with the
 * table lock held. Neither may take the lock again or block; see
 * term_reply_sink_fn in the header for the rule the reply route inherits.
 */

static void reg_reply_cb(void *user, const char *bytes, size_t len)
{
    term_slot_t *sl = (term_slot_t *)user;

    /* §6: a piped term answers into its own channel, in C. The JS sink is
     * for terms somebody feeds by hand — a reply belongs to the stream
     * that asked the question. */
    if (sl->piped && sl->prod.reply) {
        s_stats.replies_piped++;
        sl->prod.reply(sl->prod.user, slot_id(sl), bytes, len);
        return;
    }
    if (sl->reply_sink) {
        s_stats.replies_js++;
        sl->reply_sink(sl->reply_user, slot_id(sl), bytes, len);
        return;
    }
    s_stats.replies_dropped++;
}

/*
 * §10.2's push. Nothing is delivered from here: the position is parked in
 * the slot and the drain flushes it once, after the lock is dropped. The
 * core already fires only on change, so "coalesced per frame" costs three
 * ints and a flag rather than a comparison against a remembered value.
 */
static void reg_caret_cb(void *user, int col, int row, bool visible)
{
    term_slot_t *sl = (term_slot_t *)user;

    sl->caret_col = col;
    sl->caret_row = row;
    sl->caret_vis = visible;
    sl->caret_pending = true;
}

/* ===================================================================== */
/* Recording (§4.4's 2026-07-30 exception; R1-R5 in term_registry.h)      */
/* ===================================================================== */

/* The class+flag every screen-capture record carries (term_lp_ring.h). Named
 * once so the two append sites and the panic twin cannot drift apart. */
#define REC_SCREEN_CLS \
    ((term_lp_class_t)((unsigned)TERM_LP_CLASS_TERM | TERM_LP_F_SCREEN))

/* Bounded appenders for the one line this feature formats (the capture
 * marker). No snprintf: this code is one caller away from panic context and
 * the file is allocation- and libc-format-free by I4. Both return the new
 * length and never write past `cap - 1`. */
static size_t rb_put(char *dst, size_t cap, size_t at, const char *s)
{
    while (*s && at + 1u < cap)
        dst[at++] = *s++;
    dst[at] = '\0';
    return at;
}

static size_t rb_putu(char *dst, size_t cap, size_t at, unsigned v)
{
    char tmp[12];
    int n = 0;

    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 12);
    while (n-- > 0 && at + 1u < cap)
        dst[at++] = tmp[n];
    dst[at] = '\0';
    return at;
}

/*
 * The writer_id a recorded record carries: "<owner>:<name>", e.g.
 * "ssh_vt2:t0".
 *
 * WHY THE TERM NAME IS IN IT. A transcript of three simultaneously recorded
 * tabs is useless if every line says "ssh_vt2": the writer field is the only
 * per-record provenance the format has (term_lp_ring.h, P2), so it is where
 * the tab has to go. Both halves are as trustworthy as the owner alone —
 * `owner` came from the signed push and `name` is inside that owner's own
 * namespace (I3) — so this does not weaken what a writer_id means.
 *
 * WHAT IT COSTS. The writer table holds 8 interned names, shared with every
 * app's print(). Three recorded tabs are three of them. When the table fills,
 * or when owner+':'+name will not fit in 31 bytes, the honest fallback is the
 * bare owner, and after that lp_intern's own WRITER_UNKNOWN. Both degrade
 * ATTRIBUTION only; content is never affected.
 */
static const char *rec_writer(const term_slot_t *sl)
{
    size_t o = strlen(sl->owner), n = strlen(sl->name);

    if (!o)
        return NULL;
    if (n && o + 1u + n < sizeof s_rec_writer) {
        memcpy(s_rec_writer, sl->owner, o);
        s_rec_writer[o] = ':';
        memcpy(s_rec_writer + o + 1u, sl->name, n);
        s_rec_writer[o + 1u + n] = '\0';
        return s_rec_writer;
    }
    return sl->owner;
}

/*
 * The panic path's own copies of the two buffers. Separate rather than shared
 * because the panic handler cannot take the lock, and the lock is what makes
 * the shared pair safe: reusing s_rec_row would mean a crash landing in the
 * middle of a capture publishes half of one row and half of another as one
 * record. 288 bytes of .bss buys "the note is either right or absent".
 */
static char s_panic_row[TERM_LP_REC_MAX];
static char s_panic_writer[TERM_LP_WRITER_MAX];

static const char *rec_writer_panic(const term_slot_t *sl)
{
    size_t o = strlen(sl->owner), n = strlen(sl->name);

    if (!o)
        return NULL;
    if (n && o + 1u + n < sizeof s_panic_writer) {
        memcpy(s_panic_writer, sl->owner, o);
        s_panic_writer[o] = ':';
        memcpy(s_panic_writer + o + 1u, sl->name, n);
        s_panic_writer[o + 1u + n] = '\0';
        return s_panic_writer;
    }
    return sl->owner;
}

/*
 * R2's tee. Runs inside term_core_feed() — UI task, table lock held — which
 * is exactly the context term_lp_ring_append wants (it takes no lock of its
 * own and requires the caller's serialisation, and the registry's mutex is
 * the serialisation every other writer uses). So this appends INLINE and
 * needs none of the caret sink's park-and-flush: unlike the caret, the black
 * box is not another subsystem's state, it is one level DOWN from here, and
 * the lock order registry -> ring is the one term_lp_ring.h already names.
 */
static void reg_record_cb(void *user, const char *text, size_t len)
{
    term_slot_t *sl = (term_slot_t *)user;

    if (term_lp_log(TERM_LP_CLASS_TERM, rec_writer(sl), text, len))
        s_stats.rec_lines++;
}

/*
 * Capture the active screen. Lock held; `why` names the trigger and goes into
 * the marker record, which is what lets a reader see frame boundaries in a
 * transcript instead of one undifferentiated wall of rows.
 *
 * TRAILING BLANK ROWS ARE DROPPED, interior ones are not: an empty line in
 * the middle of an error message is content, an empty bottom half of the
 * screen is just the screen being taller than the output.
 *
 * A BLANK ROW INSIDE the kept range is written as ONE SPACE. The ring refuses
 * an empty payload, and dropping the row instead would shift every row below
 * it in a pulled transcript, so a space is the only rendering under which R3's
 * "one record per row" and the ring's "no empty record" both hold.
 *
 * `charge_limit` says whether this landing anchors R3(b)'s rate limit. Only
 * the AUTOMATIC triggers do: see record_screen's own comment for why a human
 * pressing a button must not spend the debounce's budget.
 */
static void capture_screen_locked(term_slot_t *sl, const char *why,
                                  bool charge_limit)
{
    const char *writer;
    int rows, r, last = -1;
    size_t spent = 0, n;
    bool trunc = false;

    if (!sl->core)
        return;
    rows = term_core_rows(sl->core);
    for (r = 0; r < rows; r++)
        if (term_core_row_utf8(sl->core, r, NULL, 0) > 0)
            last = r;

    writer = rec_writer(sl);
    n = rb_put(s_rec_row, sizeof s_rec_row, 0, "--- screen ");
    n = rb_put(s_rec_row, sizeof s_rec_row, n, why ? why : "?");
    n = rb_put(s_rec_row, sizeof s_rec_row, n, " ");
    n = rb_putu(s_rec_row, sizeof s_rec_row, n,
                (unsigned)term_core_cols(sl->core));
    n = rb_put(s_rec_row, sizeof s_rec_row, n, "x");
    n = rb_putu(s_rec_row, sizeof s_rec_row, n, (unsigned)rows);
    n = rb_put(s_rec_row, sizeof s_rec_row, n, " rows=");
    n = rb_putu(s_rec_row, sizeof s_rec_row, n, (unsigned)(last + 1));
    n = rb_put(s_rec_row, sizeof s_rec_row, n, " ---");
    if (!term_lp_log(REC_SCREEN_CLS, writer, s_rec_row, n))
        return;                 /* no ring: do not walk 51KB for nothing */
    spent += n;

    for (r = 0; r <= last; r++) {
        int need = term_core_row_utf8(sl->core, r, s_rec_row,
                                      sizeof s_rec_row);
        size_t take;
        if (need < 0)
            need = 0;
        if ((size_t)need >= sizeof s_rec_row)
            need = (int)sizeof s_rec_row - 1;   /* row_utf8 NUL-terminates */
        take = (size_t)need;
        if (spent + take > TERM_REC_SCREEN_MAX) {
            trunc = true;
            break;
        }
        /* An empty row still gets a record so the transcript keeps the
         * screen's line structure — the ring refuses an empty payload, so
         * one space is what an empty line has to be. */
        if (!take) {
            s_rec_row[0] = ' ';
            take = 1;
        }
        if (term_lp_log(REC_SCREEN_CLS, writer, s_rec_row, take))
            s_stats.rec_screen_rows++;
        spent += take;
    }
    s_stats.rec_screens++;
    if (trunc)
        s_stats.rec_screen_trunc++;
    /* The rate limit's anchor moves for an automatic landing only. The HASH
     * moves for every landing, manual included: it answers "is this screen
     * already in the box", and the answer is yes however it got there. */
    if (charge_limit)
        sl->rec_snap_ms = s_p->now_ms();
    sl->rec_hash = term_core_screen_hash(sl->core);
    sl->rec_hash_valid = true;
}

/*
 * Arm or disarm. THE function that implements R1 and R5: recording on/off is
 * the presence or absence of the core's callback, so "off" leaves no route
 * from this term to the ring at all.
 *
 * `lifecycle` distinguishes "the user turned it off" from "a transition in R1
 * turned it off", because those are different facts about the device and the
 * second one is the guarantee doing its job. A lifecycle clear still takes
 * the final capture (that is the end-of-session snapshot of R3(c)) unless the
 * core is already gone.
 */
static void record_set_locked(term_slot_t *sl, bool on, bool lifecycle,
                              const char *why)
{
    if (on == sl->record)
        return;
    if (on) {
        sl->record = true;
        sl->rec_hash_valid = false;
        sl->rec_settled = false;
        sl->rec_deferred = false;
        sl->rec_fed_ms = s_p->now_ms();
        sl->rec_snap_ms = 0;
        term_core_set_record_cb(sl->core, reg_record_cb, sl);
        s_stats.rec_on++;
        /* R3(a) at the moment of arming: the reason a human pressed the
         * button is on the glass now, and the settle timer would lose it to
         * the next redraw. */
        capture_screen_locked(sl, why ? why : "start", true);
        return;
    }
    if (sl->core)
        capture_screen_locked(sl, why ? why : "stop", true);
    sl->record = false;
    sl->rec_hash_valid = false;
    sl->rec_settled = false;
    sl->rec_deferred = false;
    term_core_set_record_cb(sl->core, NULL, NULL);
    if (lifecycle)
        s_stats.rec_cleared++;
    else
        s_stats.rec_off++;
}

/* R1's guarantee at a lifecycle transition. Cheap enough to call
 * unconditionally: it returns immediately unless recording was on. */
static void record_clear_locked(term_slot_t *sl, const char *why)
{
    record_set_locked(sl, false, true, why);
}

/*
 * R3(b), once per drain pass per term. `moved` says whether this pass took
 * bytes out of the ring.
 *
 * THE COST DISCIPLINE, in the order the conditions are written, because the
 * order IS the design:
 *
 *   1. not recording -> zero work. Not even a clock read.
 *   2. bytes moved -> the screen is changing: reset the quiet timer and
 *      return. This is the `top` storm, and it costs two stores.
 *   3. already evaluated this quiet episode -> return. An unchanged screen
 *      with no new bytes cannot become a different screen, so one hash per
 *      episode is all the information there is.
 *   4. not quiet long enough -> return, still no hash.
 *   5. rate limit still closed -> count once and return, still no hash. The
 *      episode stays unevaluated so the capture happens when it opens.
 *   6. only now: hash the screen (51 KiB of PSRAM), and capture only if it
 *      differs from the last one captured.
 *
 * So the hash runs at most once per settle interval per recording term, and
 * never at all for a term nobody is recording or a screen nobody stopped
 * writing to.
 */
static void record_settle_locked(term_slot_t *sl, bool moved, int64_t now)
{
    uint32_t h;

    if (!sl->record)
        return;
    if (moved) {
        sl->rec_fed_ms = now;
        sl->rec_settled = false;
        sl->rec_deferred = false;
        return;
    }
    if (sl->rec_settled)
        return;
    if (now - sl->rec_fed_ms < (int64_t)TERM_REC_SETTLE_MS)
        return;
    if (now - sl->rec_snap_ms < (int64_t)TERM_REC_SNAP_MIN_MS) {
        if (!sl->rec_deferred) {
            sl->rec_deferred = true;
            s_stats.rec_snaps_deferred++;
        }
        return;
    }
    h = term_core_screen_hash(sl->core);
    if (sl->rec_hash_valid && h == sl->rec_hash) {
        sl->rec_settled = true;   /* nothing new to keep, and nothing can
                                   * change it until bytes arrive */
        return;
    }
    capture_screen_locked(sl, "settled", true);
    sl->rec_settled = true;
    sl->rec_deferred = false;
}

/* Carve the one block: core first (term_core_init wants 8-byte
 * alignment and term_core_mem_size is already a multiple of 8), byte
 * ring after it. One allocation, one free — I4. */
static term_err_t slot_alloc_locked(term_slot_t *sl,
                                    const term_create_opts_t *opts)
{
    term_config_t cc;
    size_t core_size, total;
    void *block;

    memset(&cc, 0, sizeof cc);
    cc.mode      = opts->mode;
    cc.cols      = opts->cols;
    cc.rows      = opts->rows;
    cc.max_cols  = opts->max_cols;
    cc.max_rows  = opts->max_rows;
    cc.max_cells = opts->max_cells;
    /* term_registry.h: "0 selects the mode's default". §4.1 gives one
     * pair of numbers for both modes. */
    cc.scrollback_bytes = opts->scrollback_bytes ? opts->scrollback_bytes
                                                 : TERM_SB_BYTES_DEFAULT;
    cc.scrollback_lines = opts->scrollback_lines ? opts->scrollback_lines
                                                 : TERM_SB_LINES_DEFAULT;
    /* Wired at construction, not later: a term that could emit a reply or
     * move its caret before somebody remembered to attach the trampolines
     * would lose exactly the first ones (the DA/DSR handshake a shell sends
     * on connect is the first thing that happens). */
    cc.reply_cb   = reg_reply_cb;
    cc.reply_user = sl;
    cc.caret_cb   = reg_caret_cb;
    cc.caret_user = sl;

    core_size = term_core_mem_size(&cc);
    if (!core_size)
        return TERM_ERR_INVAL; /* geometry / scrollback combination refused */
    total = core_size + TERM_RING_BYTES;

    block = s_p->mem_alloc(total, TERM_MEM_PSRAM);
    if (!block)
        return TERM_ERR_NO_MEM;

    sl->core = term_core_init(block, core_size, &cc);
    if (!sl->core) {
        s_p->mem_free(block);
        return TERM_ERR_INVAL;
    }
    sl->block = block;
    sl->block_size = total;
    sl->ring = (uint8_t *)block + core_size;
    sl->ring_size = TERM_RING_BYTES;
    sl->ring_head = sl->ring_tail = sl->ring_used = 0;
    return TERM_OK;
}

/* Queue a geometry change for the UI task. Lock held; the caller posts
 * after unlocking (never inline under the lock — the job takes it). */
static bool queue_resize_locked(term_slot_t *sl, int cols, int rows)
{
    bool need_post = !sl->rz.queued;
    sl->rz.cols = cols;
    sl->rz.rows = rows;
    sl->rz.id = slot_id(sl);
    sl->rz.queued = true;
    return need_post;
}

static void resize_job(void *arg);

static term_err_t post_or_run(term_ui_job_fn fn, void *arg)
{
    if (s_p->ui_is_current()) {
        fn(arg);
        return TERM_OK;
    }
    if (!s_p->ui_post(fn, arg, 0)) {
        s_stats.post_fails++;
        return TERM_ERR_POST;
    }
    return TERM_OK;
}

term_err_t term_registry_create(const term_create_opts_t *opts,
                                term_id_t *out_id, bool *out_reattached)
{
    term_slot_t *sl, *victim = NULL;
    term_producer_detach_fn vfn = NULL;
    void *vuser = NULL;
    term_id_t vid = TERM_ID_INVALID;
    term_err_t e;
    bool need_post = false;
    int i;

    if (out_reattached)
        *out_reattached = false;
    /* Readiness is checked BEFORE the arguments, here and in every other
     * entry point: with no registry there is nothing for an argument to
     * be valid against, and a caller that gets TERM_ERR_INVAL from a
     * table that does not exist yet goes looking for the wrong bug. */
    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!opts || !out_id)
        return TERM_ERR_INVAL;
    if (!str_ok(opts->name, TERM_NAME_MAX) ||
        !str_ok(opts->owner, TERM_OWNER_MAX))
        return TERM_ERR_INVAL;
    if (opts->mode != TERM_VT && opts->mode != TERM_LOG)
        return TERM_ERR_INVAL;
    if (opts->cols < 1 || opts->rows < 1)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;

    /* --- re-attach (§3.1, the tmux model) ------------------------- */
    sl = find_key_locked(opts->owner, opts->name);
    if (sl) {
        if (sl->state == TERM_SLOT_LIVE) {
            unlock();
            return TERM_ERR_EXISTS; /* not a second handle to one term */
        }
        if (sl->mode != opts->mode) {
            unlock();
            return TERM_ERR_MODE;
        }
        sl->state = TERM_SLOT_LIVE;
        sl->detached_since_ms = 0;
        /* R1: a re-attach is a NEW session even though the scrollback
         * survived. Belt and braces — the DETACHED transition already
         * cleared it — because this is the path a restarted app takes to
         * pick a tab back up, and it is precisely where a persisted flag
         * would have come back to life. */
        record_clear_locked(sl, "reattach");
        if (opts->cols != sl->cols || opts->rows != sl->rows)
            need_post = queue_resize_locked(sl, opts->cols, opts->rows);
        *out_id = slot_id(sl);
        s_stats.reattaches++;
        unlock();
        if (need_post)
            post_or_run(resize_job, sl);
        if (out_reattached)
            *out_reattached = true;
        return TERM_OK;
    }

    /* --- persist quota, LRU evict (§3.1 (c)) ---------------------- */
    if (opts->persist && persist_count_locked() >= s_persist_max) {
        for (i = 0; i < TERM_SLOT_COUNT; i++) {
            term_slot_t *c = &s_slots[i];
            if (!c->persist || c->state != TERM_SLOT_DETACHED)
                continue;
            if (!victim || c->detached_since_ms < victim->detached_since_ms)
                victim = c;
        }
        if (!victim) {
            unlock(); /* every persist term is LIVE: nothing to evict */
            return TERM_ERR_NO_SLOT;
        }
        stage1_locked(victim, &vfn, &vuser, &vid);
        s_stats.evicted++;
    }

    /* --- new term ------------------------------------------------- */
    sl = NULL;
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        if (s_slots[i].state == TERM_SLOT_FREE) {
            sl = &s_slots[i];
            break;
        }
    }
    if (!sl) {
        unlock();
        /* The evicted slot's memory comes back on a reaper pass; §3.1
         * says the caller may retry then. */
        if (vfn)
            vfn(vuser, vid);
        wake_reaper();
        return TERM_ERR_NO_SLOT;
    }

    e = slot_alloc_locked(sl, opts);
    if (e != TERM_OK) {
        unlock();
        if (vfn)
            vfn(vuser, vid);
        wake_reaper();
        return e;
    }

    memset(sl->name, 0, sizeof sl->name);
    memset(sl->owner, 0, sizeof sl->owner);
    strncpy(sl->name, opts->name, sizeof sl->name - 1);
    strncpy(sl->owner, opts->owner, sizeof sl->owner - 1);
    sl->persist = opts->persist;
    sl->mode = opts->mode;
    sl->cols = opts->cols;
    sl->rows = opts->rows;
    sl->view.x = sl->view.y = sl->view.w = sl->view.h = 0;
    sl->view.visible = false;
    sl->detached_since_ms = 0;
    sl->dying_since_ms = 0;
    sl->ui_ack_pending = false;
    sl->prod_ack_pending = false;
    sl->piped = false;
    memset(&sl->prod, 0, sizeof sl->prod);
    sl->reply_sink = NULL;
    sl->reply_user = NULL;
    sl->caret_pending = false;
    sl->caret_col = sl->caret_row = 0;
    sl->caret_vis = true;
    sl->rz.queued = false;
    /* R1 in its simplest form: a brand new term is never recording. Written
     * out rather than left to the slot's previous contents, because a slot is
     * reused and "whatever the last tenant left" is not a guarantee. */
    sl->record = false;
    sl->rec_settled = false;
    sl->rec_deferred = false;
    sl->rec_hash_valid = false;
    sl->rec_hash = 0;
    sl->rec_fed_ms = 0;
    sl->rec_snap_ms = 0;
    sl->bytes_in = 0;
    sl->bytes_dropped = 0;
    sl->state = TERM_SLOT_LIVE;
    s_stats.creates++;
    *out_id = slot_id(sl);
    unlock();

    if (vfn)
        vfn(vuser, vid);
    if (victim)
        wake_reaper();
    return TERM_OK;
}

/* ===================================================================== */
/* close / owner teardown (§3.1 stage 1)                                 */
/* ===================================================================== */

static term_err_t close_common(term_id_t id, const char *owner)
{
    term_slot_t *sl;
    term_err_t e;
    term_producer_detach_fn fn;
    void *user;
    term_id_t did;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, owner, true /* closing a DYING term is a no-op */, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    stage1_locked(sl, &fn, &user, &did);
    unlock();

    /* Outside the lock on purpose: the ssh producer's detach does a
     * shutdown() on its socket to break a blocking recv (e1928c7). */
    if (fn)
        fn(user, did);
    wake_reaper();
    return TERM_OK;
}

term_err_t term_registry_close(term_id_t id, const char *owner)
{
    if (!owner)
        return TERM_ERR_INVAL; /* apps must identify themselves (I3) */
    return close_common(id, owner);
}

term_err_t term_registry_close_forced(term_id_t id)
{
    return close_common(id, NULL);
}

int term_registry_owner_stopped(const char *owner)
{
    int i, n = 0;
    term_producer_detach_fn fns[TERM_SLOT_COUNT];
    void *users[TERM_SLOT_COUNT];
    term_id_t ids[TERM_SLOT_COUNT];
    int nfn = 0;

    if (!s_ready || !owner || !owner[0])
        return 0;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return 0;
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        term_slot_t *sl = &s_slots[i];
        if (sl->state != TERM_SLOT_LIVE)
            continue;
        if (strncmp(sl->owner, owner, TERM_OWNER_MAX) != 0)
            continue;
        n++;
        if (sl->persist) {
            /* §3.1: kept alive, waiting for a same-owner+same-name
             * create to pick it back up, or for the LRU to evict it.
             * R1 + R3(c): the app that was showing this session is gone, so
             * take the last screen and disarm. */
            record_clear_locked(sl, "detach");
            sl->state = TERM_SLOT_DETACHED;
            sl->detached_since_ms = s_p->now_ms();
            sl->view.visible = false;
        } else {
            term_producer_detach_fn fn;
            void *user;
            term_id_t did;
            stage1_locked(sl, &fn, &user, &did);
            if (fn) {
                fns[nfn] = fn;
                users[nfn] = user;
                ids[nfn] = did;
                nfn++;
            }
        }
    }
    unlock();
    for (i = 0; i < nfn; i++)
        fns[i](users[i], ids[i]);
    if (n)
        wake_reaper();
    return n;
}

term_err_t term_registry_find(const char *owner, const char *name,
                              term_id_t *out_id)
{
    term_slot_t *sl;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!owner || !name || !out_id)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    sl = find_key_locked(owner, name);
    if (!sl) {
        unlock();
        return TERM_ERR_BAD_ID;
    }
    *out_id = slot_id(sl);
    unlock();
    return TERM_OK;
}

/* ===================================================================== */
/* Writes (§5, §8) — all owner-gated                                     */
/* ===================================================================== */

term_err_t term_registry_feed(term_id_t id, const char *owner,
                              const uint8_t *bytes, size_t len,
                              size_t *out_written)
{
    term_slot_t *sl;
    term_err_t e;
    size_t took;

    if (out_written)
        *out_written = 0;
    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!bytes && len)
        return TERM_ERR_INVAL;
    if (!owner)
        return TERM_ERR_INVAL;
    if (!len)
        return TERM_OK;

    if (!lock_for(s_ingest_ms)) {
        s_stats.drops_lock++;
        return TERM_ERR_TIMEOUT; /* §5: logging never blocks an app */
    }
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    if (sl->piped) {
        unlock();
        return TERM_ERR_BUSY; /* §5 SPSC: a piped term has one producer */
    }
    took = ring_write(sl, bytes, len);
    if (took < len) {
        sl->bytes_dropped += len - took;
        s_stats.drops_full++;
    }
    unlock();
    if (out_written)
        *out_written = took;
    return took == len ? TERM_OK : TERM_ERR_TIMEOUT;
}

/* The byte-ring half of a line append. Lock held; slot already validated. */
static term_err_t log_locked(term_slot_t *sl, const char *text, size_t len)
{
    static const uint8_t crlf[2] = { '\r', '\n' };

    /* Line-atomic: taken whole or dropped whole, so two writers never
     * interleave halves of two messages (§3.2). The lock is what makes
     * multiple writers safe here — this path is not the SPSC ring
     * fast-path, and does not care whether a producer is bound. */
    if (ring_space(sl) < len + sizeof crlf) {
        sl->bytes_dropped += len + sizeof crlf;
        s_stats.drops_full++;
        return TERM_ERR_TIMEOUT;
    }
    ring_write(sl, (const uint8_t *)text, len);
    ring_write(sl, crlf, sizeof crlf);
    return TERM_OK;
}

term_err_t term_registry_log(term_id_t id, const char *owner,
                             const char *text, size_t len)
{
    term_slot_t *sl;
    term_err_t e;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!text && len)
        return TERM_ERR_INVAL;

    if (!lock_for(s_ingest_ms)) {
        s_stats.drops_lock++;
        return TERM_ERR_TIMEOUT;
    }
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    if (sl->piped) {
        /* §5 SPSC. Line-atomicity is what makes several LOG writers safe
         * against each other; it does nothing about a producer that owns
         * the ring's head, so the same gate feed has applies here. */
        unlock();
        return TERM_ERR_BUSY;
    }
    /*
     * §4.4's tee, at the line-oriented ingest and under the table lock (the
     * ring's serialisation, term_lp_ring.h). TERM_LOG only: a VT term is
     * class B by §3.2 and its mode cannot change, so SSH session content has
     * no path here at all — structure, not a filter. `owner` is the tag's
     * writer_id; a NULL owner means a platform caller reached us through
     * term_registry_platform_log, which has already teed with its own writer
     * and class, so this path must not tee again.
     */
    if (sl->mode == TERM_LOG && owner)
        term_lp_log(TERM_LP_CLASS_APP, owner, text, len);
    e = log_locked(sl, text, len);
    unlock();
    return e;
}

static void resize_job(void *arg)
{
    term_slot_t *sl = (term_slot_t *)arg;
    int cols, rows;

    if (!s_ready)
        return;
    if (!lock_for(TERM_WAIT_FOREVER))
        return;
    /* §7.2's rule applied to the write path: re-validate at RUN time,
     * not just at post time. A slot that was freed and reused between
     * the post and now must not have its successor resized. */
    if (!sl->rz.queued || sl->rz.id != slot_id(sl) ||
        (sl->state != TERM_SLOT_LIVE && sl->state != TERM_SLOT_DETACHED)) {
        unlock();
        return;
    }
    cols = sl->rz.cols;
    rows = sl->rz.rows;
    sl->rz.queued = false;
    if (sl->core && term_core_resize(sl->core, cols, rows) == 0) {
        sl->cols = cols;
        sl->rows = rows;
    }
    unlock();
}

term_err_t term_registry_resize(term_id_t id, const char *owner,
                                int cols, int rows)
{
    term_slot_t *sl;
    term_err_t e;
    bool need_post;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (cols < 1 || rows < 1)
        return TERM_ERR_INVAL;
    if (!owner)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    need_post = queue_resize_locked(sl, cols, rows);
    unlock();
    if (!need_post)
        return TERM_OK; /* coalesced into the job already queued */
    return post_or_run(resize_job, sl);
}

term_err_t term_registry_show(term_id_t id, const char *owner,
                              const term_view_t *view)
{
    term_slot_t *sl;
    term_err_t e;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!owner)
        return TERM_ERR_INVAL;
    if (view && (view->w < 0 || view->h < 0))
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    if (view) {
        /* The caret sink only speaks for a visible term (§10.2), so a
         * term that becomes visible — or moves — owes the platform its
         * anchor even though its cursor did not move. Tab switching is
         * show/hide, and the IME float must not keep pointing at the tab
         * that just went away.
         *
         * The same transition owes it a full repaint, and for the same
         * kind of reason: the damage set describes cell changes, and
         * nothing changed — what changed is which pixels are on the
         * glass. Without this a switched-to tab stays blank until its
         * remote writes something (term_core_repaint_all). */
        if (view->visible &&
            (!sl->view.visible || view->x != sl->view.x ||
             view->y != sl->view.y || view->w != sl->view.w ||
             view->h != sl->view.h)) {
            sl->caret_pending = true;
            term_core_repaint_all(sl->core);
        }
        sl->view = *view;
    } else {
        sl->view.visible = false; /* hide without forgetting the rect */
    }
    unlock();
    return TERM_OK;
}

/* ---- 履歴スクロール ------------------------------------------------- */

/* delta 表示行ぶんさかのぼる (負で戻る)。新しい位置を *now に返す。
   0 は「動かさずに現在値を読む」。位置が変わったら全面再描画を予約する
   —— damage はセルの変化を語るもので、**変わったのはどの画素が硝子の
   上に居るか**だから (term_registry_show の可視化遷移と同じ理由)。 */
term_err_t term_registry_scroll(term_id_t id, const char *owner, int delta,
                                int *now)
{
    term_slot_t *sl;
    term_err_t e;
    int before;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!owner)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    before = sl->scroll;
    sl->scroll += delta;
    if (sl->scroll < 0)
        sl->scroll = 0;
    if (sl->scroll != before)
        term_core_repaint_all(sl->core);
    if (now)
        *now = sl->scroll;
    unlock();
    return TERM_OK;
}

/* ---- 以下 2 本は visit コールバックの中からだけ呼ぶ ------------------
 *
 * **ロックを取らない。** term_registry_ui_visit は fn() を**ロックを
 * 持ったまま**呼ぶので、ここで取り直すと非再帰ミューテックスに自分で
 * ぶつかる —— 実機では毎フレーム制御タイムアウトぶん待って 0 が返り、
 * 「表示が遅い・入力が遅い・スクロールしない」が同時に出た。
 * スロットが生きていることも view が可視であることも visit が保証済み。 */

int term_registry_scroll_rows(term_id_t id)
{
    term_slot_t *sl;

    if (!s_ready || slot_lookup(id, NULL, true, &sl) != TERM_OK)
        return 0;
    return sl->scroll;
}

/* レンダラが「実際にさかのぼれた行数」を書き戻す。履歴の先頭で指を
   動かし続けても、要求だけが際限なく増えていくのを防ぐ。 */
void term_registry_scroll_reached(term_id_t id, int achieved)
{
    term_slot_t *sl;

    if (!s_ready || achieved < 0)
        return;
    if (slot_lookup(id, NULL, true, &sl) == TERM_OK && sl->scroll > achieved)
        sl->scroll = achieved;
}

/* ===================================================================== */
/* Reads (§7.2) — serialised on the UI task, non-mutating                */
/* ===================================================================== */

static term_err_t snapshot_locked(term_slot_t *sl, char *out, size_t out_size,
                                  size_t *out_len)
{
    int rows = term_core_rows(sl->core);
    int r;
    size_t off = 0, total = 0;
    bool trunc = false;

    if (out && out_size)
        out[0] = '\0';
    for (r = 0; r < rows; r++) {
        size_t avail = (!trunc && out && out_size > off) ? out_size - off : 0;
        int need = term_core_row_utf8(sl->core, r, avail ? out + off : NULL,
                                      avail);
        if (need < 0)
            need = 0;
        if (avail && (size_t)need + 1 <= avail)
            off += (size_t)need;
        else
            trunc = true;
        total += (size_t)need;
        if (r + 1 < rows) {
            total += 1;
            if (!trunc && out && off + 1 < out_size)
                out[off++] = '\n';
            else
                trunc = true;
        }
    }
    if (out && out_size)
        out[off < out_size ? off : out_size - 1] = '\0';
    if (out_len)
        *out_len = total;
    return trunc ? TERM_ERR_TRUNC : TERM_OK;
}

static term_err_t read_locked(term_slot_t *sl, uint32_t from, int n,
                              char *out, size_t out_size,
                              term_read_result_t *res)
{
    uint32_t first = term_core_sb_first(sl->core);
    uint32_t end   = term_core_sb_end(sl->core);
    uint32_t id    = from;
    size_t off = 0;
    int lines = 0;
    bool trunc = false;

    if (out && out_size)
        out[0] = '\0';
    /* §8: a `from` below the surviving range starts at the oldest line
     * instead of failing; res->first tells the caller what it missed. */
    if (id < first)
        id = first;
    while (lines < n && id < end) {
        size_t avail = (out && out_size > off) ? out_size - off : 0;
        int need;
        if (!avail) {
            trunc = true;
            break;
        }
        need = term_core_line_utf8(sl->core, id, out + off, avail);
        if (need < 0) {
            id++; /* evicted underneath us: skip, do not stop */
            continue;
        }
        if ((size_t)need + 1 > avail) {
            out[off] = '\0'; /* undo the partial line */
            trunc = true;
            break;
        }
        off += (size_t)need;
        lines++;
        id++;
        if (lines < n && id < end) {
            if (off + 1 < out_size) {
                out[off++] = '\n';
            } else {
                trunc = true;
                break;
            }
        }
    }
    if (out && out_size)
        out[off < out_size ? off : out_size - 1] = '\0';
    if (res) {
        res->first = first;
        res->next = id;
        res->lines = lines;
        res->bytes = off;
        res->truncated = trunc;
    }
    return TERM_OK;
}

static void job_body_locked(term_job_t *j)
{
    term_slot_t *sl;
    term_err_t e = slot_lookup(j->id, j->have_owner ? j->owner : NULL,
                               false, &sl);
    if (e != TERM_OK) {
        j->err = e;
        return;
    }
    if (j->kind == JOB_SNAPSHOT)
        j->err = snapshot_locked(sl, j->out, j->out_size, &j->out_len);
    else
        j->err = read_locked(sl, j->from, j->n, j->out, j->out_size, &j->res);
}

static void read_job(void *arg)
{
    term_job_t *j = (term_job_t *)arg;
    void *sig;

    if (!s_ready)
        return;
    if (!lock_for(TERM_WAIT_FOREVER))
        return;
    if (j->abandoned) {
        /* The caller's wait expired: its buffer, its result struct and
         * its signal are all gone. Release the job slot and touch
         * nothing else (term_registry.h: "the registry retains the job's
         * storage until the job has run"). */
        j->in_use = false;
        unlock();
        return;
    }
    job_body_locked(j);
    j->done = true;
    sig = j->sig;
    /* Signalled under the lock on purpose: outside it the caller could
     * observe done, take its result and destroy the signal before this
     * line ran. */
    if (sig)
        s_p->signal_set(sig);
    unlock();
}

static term_err_t run_read_job(term_job_kind_t kind, term_id_t id,
                               const char *owner, char *out, size_t out_size,
                               uint32_t from, int n,
                               size_t *out_len, term_read_result_t *res)
{
    term_job_t *j = NULL;
    term_slot_t *sl = NULL;
    void *sig;
    term_err_t err;
    int i;
    bool waited;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!owner)
        return TERM_ERR_INVAL;

    /*
     * Gate first, round trip second. §7.2 requires the id to be
     * re-validated INSIDE the job, and it still is — post time and run
     * time are different moments. This check is the other half: a term
     * the caller does not own, or one that is already gone, gets its
     * answer here instead of costing a frame. It is also the only place
     * an error path may touch `out`: past this point the caller's buffer
     * is untouched unless the job actually ran (a timed-out caller's
     * stack must never be written to, term_registry.h §7.2).
     */
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    err = slot_lookup(id, owner, false, &sl);
    unlock();
    if (err != TERM_OK) {
        if (out && out_size)
            out[0] = '\0';
        return err;
    }

    /* Already the UI task: run it here. Posting to ourselves would
     * deadlock, and a frame boundary is where we already are (§7.2). */
    if (s_p->ui_is_current()) {
        term_job_t local;
        memset(&local, 0, sizeof local);
        local.kind = kind;
        local.id = id;
        local.have_owner = true;
        strncpy(local.owner, owner, sizeof local.owner - 1);
        local.out = out;
        local.out_size = out_size;
        local.from = from;
        local.n = n;
        if (!lock_for(TERM_WAIT_FOREVER))
            return TERM_ERR_TIMEOUT;
        job_body_locked(&local);
        unlock();
        if (out_len)
            *out_len = local.out_len;
        if (res)
            *res = local.res;
        return local.err;
    }

    sig = s_p->signal_create();
    if (!sig)
        return TERM_ERR_NO_MEM;

    if (!lock_for(TERM_CONTROL_TIMEOUT_MS)) {
        s_p->signal_destroy(sig);
        return TERM_ERR_TIMEOUT;
    }
    for (i = 0; i < TERM_JOB_SLOTS; i++) {
        if (!s_jobs[i].in_use) {
            j = &s_jobs[i];
            break;
        }
    }
    if (!j) {
        unlock();
        s_p->signal_destroy(sig);
        s_stats.post_fails++;
        return TERM_ERR_POST;
    }
    memset(j, 0, sizeof *j);
    j->in_use = true;
    j->sig = sig;
    j->kind = kind;
    j->id = id;
    j->have_owner = true;
    strncpy(j->owner, owner, sizeof j->owner - 1);
    j->out = out;
    j->out_size = out_size;
    j->from = from;
    j->n = n;
    unlock();

    if (!s_p->ui_post(read_job, j, 0)) {
        s_stats.post_fails++;
        if (lock_for(TERM_WAIT_FOREVER)) {
            j->in_use = false;
            unlock();
        }
        s_p->signal_destroy(sig);
        return TERM_ERR_POST;
    }

    waited = s_p->signal_wait(sig, s_read_ms);
    (void)waited;

    if (!lock_for(TERM_WAIT_FOREVER)) {
        /* Cannot even reach the table: abandon conservatively. The job
         * slot leaks for this registry's lifetime, which is strictly
         * better than letting the job write into a dead stack. */
        return TERM_ERR_TIMEOUT;
    }
    if (j->done) {
        err = j->err;
        if (out_len)
            *out_len = j->out_len;
        if (res)
            *res = j->res;
        j->sig = NULL;
        j->in_use = false;
        unlock();
        s_p->signal_destroy(sig);
        return err;
    }
    /* Timed out: the job has not run. Mark it so that when it does it
     * writes nothing and releases its own slot. */
    j->abandoned = true;
    j->sig = NULL;
    j->out = NULL;
    j->out_size = 0;
    unlock();
    s_p->signal_destroy(sig);
    return TERM_ERR_TIMEOUT;
}

term_err_t term_registry_snapshot(term_id_t id, const char *owner,
                                  char *out, size_t out_size, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!out || !out_size)
        return TERM_ERR_INVAL;
    return run_read_job(JOB_SNAPSHOT, id, owner, out, out_size, 0, 0,
                        out_len, NULL);
}

term_err_t term_registry_read(term_id_t id, const char *owner,
                              uint32_t from, int n,
                              char *out, size_t out_size,
                              term_read_result_t *res)
{
    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!out || !out_size || n <= 0)
        return TERM_ERR_INVAL;
    return run_read_job(JOB_READ, id, owner, out, out_size, from, n,
                        NULL, res);
}

/* ===================================================================== */
/* Recording, the public half (§4.4's exception; R1-R5)                   */
/* ===================================================================== */

term_err_t term_registry_record(term_id_t id, const char *owner, bool on)
{
    term_slot_t *sl;
    term_err_t e;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!owner)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    if (sl->mode != TERM_VT) {
        /* Recording is the class-B exception and nothing else — a LOG term is
         * already teed as class A, so arming it would only duplicate. See the
         * header. */
        unlock();
        return TERM_ERR_MODE;
    }
    /*
     * NOT posted to the UI task, unlike snapshot/read, even though arming
     * takes a screen capture and therefore reads the grid. The table lock is
     * what protects a core from the parser (this file's opening comment: one
     * mutex covers the whole table AND every term's core), so reading here is
     * as consistent as reading in the drain. The read pair posts because it
     * wants a FRAME boundary for the caller's benefit and because it copies
     * into the caller's buffer; neither applies to an append into the ring.
     */
    record_set_locked(sl, on, false, on ? "start" : "stop");
    unlock();
    return TERM_OK;
}

int term_registry_recording(term_id_t id, const char *owner)
{
    term_slot_t *sl;
    term_err_t e;
    bool on;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!owner)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    on = sl->record;
    unlock();
    return on ? 1 : 0;
}

term_err_t term_registry_record_screen(term_id_t id, const char *owner)
{
    term_slot_t *sl;
    term_err_t e;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!owner)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    if (!sl->record) {
        /* Not a second, unannounced door into the ring: this exists only
         * while the indicator is lit. */
        unlock();
        return TERM_ERR_INVAL;
    }
    /* charge_limit = false, and this is the header's promise ("It does NOT
     * reset the debounce rate limit"), not an optimisation.
     *
     * THE LIMIT EXISTS TO STOP AN AUTOMATIC TRIGGER FROM STARVING THE RING,
     * and a button press is bounded by the human pressing it. If a manual
     * capture moved rec_snap_ms, pressing "save this screen" would turn the
     * NEXT screen state away for up to TERM_REC_SNAP_MIN_MS and count it
     * deferred — the button would cost the operator the automatic snapshot of
     * whatever came after it, which is the opposite of what it is for. So the
     * manual path shares none of the limiter's state: not the anchor here, and
     * not the two episode flags below, which it clears so that a screen which
     * changes AFTER the press is still picked up by the debounce. */
    capture_screen_locked(sl, "manual", false);
    sl->rec_settled = false;
    sl->rec_deferred = false;
    unlock();
    return TERM_OK;
}

int term_registry_panic_capture(void)
{
    static bool done;
    int pass, i, appended = 0;
    size_t spent = 0;

    if (done || !s_ready)
        return 0;
    done = true;
    if (!term_lp_panic_ready())
        return 0;

    /* Visible terms first: with a shared budget, the tab the user was looking
     * at is the one worth spending it on. Two passes over the table, pass 0
     * taking the visible terms and pass 1 the rest, because slot index and
     * visibility are unrelated — the hidden tab is as likely as not to be the
     * lower slot, and spending the budget on it is exactly the outcome this
     * ordering rule exists to prevent. No lock — see the header on why that is
     * contained rather than safe.
     *
     * TERM_REC_PANIC_MAX IS A CAP ON WHAT IS WRITTEN, not on what is written
     * before the last record: every append is checked for fit FIRST (marker
     * included), so the total payload cannot exceed the budget by a record.
     * The panic path is spending a shared LP region under a fault; "4,096 plus
     * however long the last row happened to be" is not a bound. */
    for (pass = 0; pass < 2 && spent < TERM_REC_PANIC_MAX; pass++) {
        for (i = 0; i < TERM_SLOT_COUNT && spent < TERM_REC_PANIC_MAX; i++) {
            term_slot_t *sl = &s_slots[i];
            const char *writer;
            int rows, r;

            if (!sl->record || !sl->core)
                continue;
            if (sl->state != TERM_SLOT_LIVE && sl->state != TERM_SLOT_DETACHED)
                continue;
            if ((pass == 0) != (sl->view.visible ? true : false))
                continue;

            writer = rec_writer_panic(sl);
            rows = term_core_rows(sl->core);
            /* One marker, so a reader can tell a panic screen from the
             * debounced one that may already be a few records back. */
            {
                size_t n = rb_put(s_panic_row, sizeof s_panic_row, 0,
                                  "--- screen panic ");
                n = rb_putu(s_panic_row, sizeof s_panic_row, n,
                            (unsigned)term_core_cols(sl->core));
                n = rb_put(s_panic_row, sizeof s_panic_row, n, "x");
                n = rb_putu(s_panic_row, sizeof s_panic_row, n,
                            (unsigned)rows);
                n = rb_put(s_panic_row, sizeof s_panic_row, n, " ---");
                /* The marker is charged, and a marker that does not fit means
                 * this term contributes NOTHING: rows without their own marker
                 * would read as a continuation of the previous term's frame. */
                if (spent + n > TERM_REC_PANIC_MAX)
                    continue;
                if (!term_lp_panic_append(REC_SCREEN_CLS, writer,
                                          s_panic_row, n))
                    return appended;
                spent += n;
                appended++;
            }
            for (r = 0; r < rows && spent < TERM_REC_PANIC_MAX; r++) {
                int need = term_core_row_utf8(sl->core, r, s_panic_row,
                                              sizeof s_panic_row);
                if (need <= 0)
                    continue;      /* blank rows are not worth a panic append */
                if ((size_t)need >= sizeof s_panic_row)
                    need = (int)sizeof s_panic_row - 1;
                if (spent + (size_t)need > TERM_REC_PANIC_MAX)
                    break;         /* stop on a row boundary, inside the cap */
                if (!term_lp_panic_append(REC_SCREEN_CLS, writer, s_panic_row,
                                          (size_t)need))
                    return appended;   /* the shadow header went bad: stop */
                spent += (size_t)need;
                appended++;
                s_stats.rec_panic_rows++;
            }
        }
    }
    return appended;
}

/* ===================================================================== */
/* Producers (§5)                                                        */
/* ===================================================================== */

term_err_t term_registry_producer_bind_ex(term_id_t id, const char *owner,
                                          const term_producer_t *prod)
{
    term_slot_t *sl;
    term_err_t e;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!owner || !prod || !prod->detach)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    if (sl->piped) {
        unlock();
        return TERM_ERR_BUSY;
    }
    record_clear_locked(sl, "bind");   /* R1: a new producer = a new session */
    sl->piped = true;
    sl->prod = *prod;
    s_stats.pipes++;
    unlock();
    return TERM_OK;
}

term_err_t term_registry_producer_bind(term_id_t id, const char *owner,
                                       term_producer_detach_fn detach,
                                       void *user)
{
    term_producer_t p;

    p.detach = detach;
    p.reply = NULL;
    p.user = user;
    return term_registry_producer_bind_ex(id, owner, &p);
}

/*
 * term.pipe. The whole of §5's re-pipe rule, and no more than that: the
 * old producer is asked to detach here, its ack collects it later, and
 * this call never waits for either. CHOICE (PHASE4_MANIFEST #2): the
 * "ack join" of §5 is the caller's retry rather than a bounded sleep on
 * js_task, because js_task is shared by every worker and §3.1 already
 * refuses to let a terminal's lifecycle stall all JS on the device.
 */
term_err_t term_registry_pipe(term_id_t id, const char *owner,
                              const term_producer_t *prod)
{
    term_slot_t *sl;
    term_err_t e;
    term_producer_detach_fn fn = NULL;
    void *user = NULL;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!owner || !prod || !prod->detach)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    if (sl->mode != TERM_VT) {
        /* A LOG term is multi-writer by construction (print sink,
         * term.log). A producer that owns the ring's head cannot share it
         * with them, and silently disabling the console's own writers to
         * accommodate a pipe would be the wrong half to give up. */
        unlock();
        return TERM_ERR_MODE;
    }
    if (sl->piped) {
        fn = sl->prod.detach;
        user = sl->prod.user;
        sl->prod_ack_pending = true; /* same ack discipline as stage 1 */
        s_stats.pipe_busy++;
        unlock();
        fn(user, id); /* outside the lock: it may shutdown() a socket */
        return TERM_ERR_BUSY;
    }
    /*
     * R1's structural half of "a reused tab starts recording off". Binding a
     * producer is the one event every new session passes through: a fresh
     * connect, a reconnect into a tab that was cleared with RIS, a re-pipe
     * onto a different channel. Clearing here means an app that FORGOT to
     * disarm cannot carry one session's opt-in into the next one, so the
     * guarantee does not depend on app code being careful.
     */
    record_clear_locked(sl, "bind");
    sl->piped = true;
    sl->prod = *prod;
    s_stats.pipes++;
    unlock();
    return TERM_OK;
}

term_err_t term_registry_producer_unbind(term_id_t id, const char *owner)
{
    term_slot_t *sl;
    term_err_t e;
    term_producer_detach_fn fn = NULL;
    void *user = NULL;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!owner)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    if (sl->piped) {
        fn = sl->prod.detach;
        user = sl->prod.user;
        sl->prod_ack_pending = true; /* same ack discipline as stage 1 */
    }
    unlock();
    if (fn)
        fn(user, id);
    return TERM_OK;
}

term_err_t term_registry_producer_ack(term_id_t id)
{
    term_slot_t *sl;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    if (id <= 0) {
        unlock();
        return TERM_ERR_BAD_ID;
    }
    sl = &s_slots[term_id_slot(id)];
    /* I2 in action: a late ack for a term that has already been freed
     * addresses a generation this slot no longer has, so it can never be
     * mistaken for the ack of the term that lives here now. */
    if (sl->state == TERM_SLOT_FREE ||
        sl->generation != term_id_generation(id)) {
        s_stats.stale_acks++;
        unlock();
        return TERM_OK;
    }
    /* R1 + R3(c): the producer is gone, i.e. the ssh session ended. Keep the
     * last screen (a session that died in `top` scrolled nothing, so this is
     * the only record of what it was showing) and disarm. */
    record_clear_locked(sl, "session-end");
    sl->prod_ack_pending = false;
    sl->piped = false;
    memset(&sl->prod, 0, sizeof sl->prod);
    unlock();
    wake_reaper();
    return TERM_OK;
}

/* ===================================================================== */
/* The ring, from the producer's side (§5: backpressure, never loss)     */
/* ===================================================================== */

/*
 * Both entry points share the gate. No owner argument — the producer is
 * platform C code — so the identity check is the cookie the bind was
 * given. That matters for exactly one case: a producer that has been
 * unbound but has not acked yet is still `piped` and still holds a valid
 * id, and once it HAS acked and somebody else has taken the slot's pipe
 * its writes must not land in the new producer's stream.
 */
static term_err_t producer_gate(term_id_t id, void *user, term_slot_t **out)
{
    term_slot_t *sl;
    term_err_t e;

    e = slot_lookup(id, NULL, false, &sl);
    if (e != TERM_OK)
        return e;
    if (!sl->piped || sl->prod.user != user)
        return TERM_ERR_BUSY;
    *out = sl;
    return TERM_OK;
}

term_err_t term_registry_producer_space(term_id_t id, void *user,
                                        size_t *out_space)
{
    term_slot_t *sl;
    term_err_t e;
    size_t space;

    if (out_space)
        *out_space = 0;
    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!out_space)
        return TERM_ERR_INVAL;
    if (!lock_for(s_ingest_ms))
        return TERM_ERR_TIMEOUT; /* nothing consumed: ask again */
    e = producer_gate(id, user, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    space = ring_space(sl);
    if (!space)
        s_stats.prod_stalls++;
    unlock();
    *out_space = space;
    return TERM_OK;
}

term_err_t term_registry_producer_write(term_id_t id, void *user,
                                        const uint8_t *bytes, size_t len,
                                        size_t *out_written)
{
    term_slot_t *sl;
    term_err_t e;
    size_t took;

    if (out_written)
        *out_written = 0;
    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!bytes && len)
        return TERM_ERR_INVAL;
    if (!len)
        return TERM_OK;
    if (!lock_for(s_ingest_ms))
        return TERM_ERR_TIMEOUT; /* the producer still owns its bytes */
    e = producer_gate(id, user, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    took = ring_write(sl, bytes, len);
    s_stats.prod_bytes += took;
    unlock();
    if (out_written)
        *out_written = took;
    /*
     * A short write means the producer wrote more than the space it asked
     * for — a protocol error on its side, not a lost-bytes policy like
     * feed's. It is reported, never silently absorbed, so the leftover is
     * the caller's to re-offer.
     */
    return took == len ? TERM_OK : TERM_ERR_TIMEOUT;
}

/* ===================================================================== */
/* Reply sink (§6/§8) and caret sink (§10.2)                             */
/* ===================================================================== */

term_err_t term_registry_set_reply(term_id_t id, const char *owner,
                                   term_reply_sink_fn fn, void *user)
{
    term_slot_t *sl;
    term_err_t e;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!owner)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, owner, false, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    sl->reply_sink = fn;
    sl->reply_user = user;
    unlock();
    return TERM_OK;
}

void term_registry_set_caret_sink(term_caret_sink_fn fn, void *user)
{
    /* No lock: two words written once at bring-up, read on the UI task
     * after the drain has released the lock. Taking the table mutex here
     * would order this call behind a parse for no benefit. */
    s_caret_sink = fn;
    s_caret_sink_user = user;
}

/* ===================================================================== */
/* Stage 2: the reaper (§3.1)                                            */
/* ===================================================================== */

int term_registry_reap(void)
{
    int i, freed = 0;
    int64_t now;

    if (!s_ready)
        return 0;
    if (!lock_for(TERM_WAIT_FOREVER))
        return 0;
    now = s_p->now_ms();
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        term_slot_t *sl = &s_slots[i];
        if (sl->state != TERM_SLOT_DYING && sl->state != TERM_SLOT_ZOMBIE)
            continue;
        if (!sl->ui_ack_pending && !sl->prod_ack_pending) {
            /* Both acks in — including the late ack that collects a
             * zombie on a subsequent pass (§3.1). */
            slot_release_locked(sl);
            freed++;
            continue;
        }
        if (sl->state == TERM_SLOT_DYING &&
            now - sl->dying_since_ms >= (int64_t)s_quiesce_ms) {
            /* Deadline: keep the memory, never reuse the slot. A 132KB
             * leak is the chosen trade against a use-after-free. */
            sl->state = TERM_SLOT_ZOMBIE;
            s_stats.zombies++;
            s_stats.zombie_bytes += sl->block_size;
        }
    }
    unlock();
    return freed;
}

/* ===================================================================== */
/* UI frame task (§5 drain, §9 blit)                                     */
/* ===================================================================== */

bool term_registry_ui_drain(void)
{
    int i;
    bool changed = false;
    bool acked = false;
    /* §10.2's flush list. Built under the lock, delivered after it: the
     * sink writes into another subsystem's state (the platform caret the
     * IME reads) and must not do that from inside the registry's critical
     * section. Bounded by the slot count, so it is a stack array and not
     * an allocation — I4 holds on this path too. */
    term_caret_ev_t carets[TERM_SLOT_COUNT];
    int ncarets = 0;
    term_caret_sink_fn sink;
    void *sink_user;
    int64_t now;

    if (!s_ready)
        return false;
    if (!lock_for(TERM_WAIT_FOREVER))
        return false;
    /* One read of the sink for the whole pass: deciding to collect an
     * event and then delivering it must not disagree about whether there
     * is anybody to deliver to. */
    sink = s_caret_sink;
    sink_user = s_caret_sink_user;
    /* One clock read for the whole pass, like the sink: the debounce of R3(b)
     * compares terms against the same instant, and now_ms() is a port call. */
    now = s_p->now_ms();
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        term_slot_t *sl = &s_slots[i];
        int budget;
        bool moved = false;

        if (sl->state == TERM_SLOT_DYING || sl->state == TERM_SLOT_ZOMBIE) {
            /* This is the UI half of the stage-2 ack: past this point
             * the UI task does not touch the core again (drain and
             * visit both skip these states). */
            if (sl->ui_ack_pending) {
                sl->ui_ack_pending = false;
                acked = true;
            }
            continue;
        }
        if (sl->state == TERM_SLOT_FREE || !sl->core)
            continue;

        budget = s_drain_budget;
        while (budget > 0 && sl->ring_used) {
            size_t want = (size_t)budget < sizeof s_drain_buf
                              ? (size_t)budget : sizeof s_drain_buf;
            size_t got = ring_read(sl, s_drain_buf, want);
            if (!got)
                break;
            term_core_feed(sl->core, s_drain_buf, got);
            budget -= (int)got;
            moved = true;
        }
        /* R3(b). After the parse, so a screen judged "settled" is the screen
         * this pass produced and not the one before it. The recording tee
         * itself already fired inside term_core_feed, from sb_archive_row. */
        record_settle_locked(sl, moved, now);
        /* Leftovers stay in the ring for the next frame — §5's carry. */
        if (sl->view.visible &&
            (term_core_full_repaint(sl->core) ||
             term_core_dirty_next(sl->core, 0) >= 0))
            changed = true;

        /* §10.2: one caret event per term per frame, and only for a term
         * the user can actually see. An invisible term's anchor keeps its
         * pending flag until term_registry_show makes it visible again. */
        if (sl->caret_pending && sl->view.visible && sink) {
            term_caret_ev_t *ev = &carets[ncarets++];
            sl->caret_pending = false;
            ev->id = slot_id(sl);
            memcpy(ev->owner, sl->owner, sizeof ev->owner);
            ev->owner[sizeof ev->owner - 1] = '\0';
            ev->x = sl->view.x + sl->caret_col * s_cell_w;
            ev->y = sl->view.y + sl->caret_row * s_cell_h;
            ev->h = s_cell_h;
            ev->visible = sl->caret_vis;
            s_stats.caret_pushes++;
        }
    }
    unlock();
    for (i = 0; i < ncarets; i++)
        sink(sink_user, &carets[i]);
    if (acked)
        wake_reaper();
    return changed;
}

int term_registry_ui_visit(term_ui_visit_fn fn, void *user)
{
    int i, n = 0;

    if (!fn || !s_ready)
        return 0;
    if (!lock_for(TERM_WAIT_FOREVER))
        return 0;
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        term_slot_t *sl = &s_slots[i];
        if (sl->state != TERM_SLOT_LIVE && sl->state != TERM_SLOT_DETACHED)
            continue;
        if (!sl->core || !sl->view.visible)
            continue;
        fn(slot_id(sl), sl->core, &sl->view, user);
        n++;
    }
    unlock();
    return n;
}

term_id_t term_registry_console_id(void)
{
    return s_console_id;
}

term_err_t term_registry_platform_log(const char *writer, term_wclass_t wc,
                                      const char *text, size_t len)
{
    term_lp_class_t cls = (wc == TERM_WCLASS_SYSTEM) ? TERM_LP_CLASS_SYS
                                                     : TERM_LP_CLASS_APP;
    term_slot_t *sl;
    term_err_t e;

    if (!text && len)
        return TERM_ERR_INVAL;
    if (!s_ready) {
        /* No table means no mutex to serialise on. This is the pre-bring-up
         * window, where app_main and js_task are the only callers and the
         * ring's single-writer requirement holds anyway — and a line from a
         * boot that never got as far as a console is exactly the kind the
         * black box exists for. */
        term_lp_log(cls, writer, text, len);
        return TERM_ERR_NOT_READY;
    }
    if (!lock_for(s_ingest_ms)) {
        s_stats.drops_lock++;
        return TERM_ERR_TIMEOUT;   /* §5's bounded wait; the tee shares it */
    }
    term_lp_log(cls, writer, text, len);
    if (s_console_id == TERM_ID_INVALID) {
        unlock();
        return TERM_ERR_NOT_READY;
    }
    /* No owner gate: the console belongs to the platform, and this is the
     * platform writing to it. */
    e = slot_lookup(s_console_id, NULL, false, &sl);
    if (e == TERM_OK)
        e = log_locked(sl, text, len);
    unlock();
    return e;
}

term_err_t term_registry_system_log(const char *writer,
                                    const char *text, size_t len)
{
    return term_registry_platform_log(writer, TERM_WCLASS_APP, text, len);
}

/* ===================================================================== */
/* Introspection (platform-side only)                                    */
/* ===================================================================== */

static void fill_info(const term_slot_t *sl, term_slot_info_t *out)
{
    memset(out, 0, sizeof *out);
    out->id = slot_id(sl);
    memcpy(out->name, sl->name, sizeof out->name);
    memcpy(out->owner, sl->owner, sizeof out->owner);
    out->state = sl->state;
    out->mode = sl->mode;
    out->persist = sl->persist;
    out->piped = sl->piped;
    out->recording = sl->record;
    out->cols = sl->cols;
    out->rows = sl->rows;
    out->mem_bytes = sl->block_size;
    out->detached_since_ms = sl->detached_since_ms;
    out->pending_acks = (uint32_t)((sl->ui_ack_pending ? 1 : 0) +
                                   (sl->prod_ack_pending ? 1 : 0));
    out->bytes_in = sl->bytes_in;
    out->bytes_dropped = sl->bytes_dropped;
}

term_err_t term_registry_info(term_id_t id, term_slot_info_t *out)
{
    term_slot_t *sl;
    term_err_t e;

    if (!s_ready)
        return TERM_ERR_NOT_READY;
    if (!out)
        return TERM_ERR_INVAL;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return TERM_ERR_TIMEOUT;
    e = slot_lookup(id, NULL, true, &sl);
    if (e != TERM_OK) {
        unlock();
        return e;
    }
    fill_info(sl, out);
    unlock();
    return TERM_OK;
}

int term_registry_list(term_slot_info_t *out, int max)
{
    int i, n = 0;

    if (!out || max <= 0 || !s_ready)
        return 0;
    if (!lock_for(TERM_CONTROL_TIMEOUT_MS))
        return 0;
    for (i = 0; i < TERM_SLOT_COUNT && n < max; i++) {
        if (s_slots[i].state == TERM_SLOT_FREE)
            continue;
        fill_info(&s_slots[i], &out[n++]);
    }
    unlock();
    return n;
}

void term_registry_stats(term_registry_stats_t *out)
{
    if (!out)
        return;
    if (!s_ready || !lock_for(TERM_CONTROL_TIMEOUT_MS)) {
        *out = s_stats;
        return;
    }
    *out = s_stats;
    unlock();
}
