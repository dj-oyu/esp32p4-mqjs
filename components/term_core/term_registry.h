/*
 * term_registry — the platform's fixed table of terminals.
 *
 * docs/term-design.md phase 2 (§11.2). A term is NOT an app's object: it
 * is a slot object owned by the C side, and an app holds only an id
 * (§3.1). Everything an app can do to a term goes through this header,
 * and every entry point re-checks that the caller still owns the thing it
 * is naming.
 *
 * Layering: term_core (phase 1, pure VT logic) sits underneath and knows
 * nothing about ownership; term_port (this phase) supplies the four
 * platform services; term_registry is the rules. The JS bindings
 * (term.*, §8) are a thin argument-marshalling layer above this file and
 * hold no state of their own.
 *
 * =====================================================================
 * THE FIVE INVARIANTS (§3.1, §4.2, §7.2)
 * =====================================================================
 *
 * I1. FIXED TABLE. TERM_SLOT_COUNT (8) slots, statically present for the
 *     lifetime of the process. Slot count rationale in §4.1: worker cap 4,
 *     ssh_vt has shipped 3 concurrent sessions, plus the system console
 *     plus headroom.
 *
 * I2. IDS ARE (generation << 3) | slot, generation 28 bits (§3.1). Raw
 *     slot numbers never leave this file. An 8-bit generation would wrap
 *     after 256 create/close cycles and the ABA guard would quietly stop
 *     guarding; 28 bits is ~7,000 years at 100 reconnects a day. Ids stay
 *     positive int32 so a JS binding can return one as a plain number.
 *
 * I3. EVERY API CHECKS GENERATION AND OWNER — writes included (feed, log,
 *     pipe, resize, close), not just reads (§3.1). That closes both the
 *     "another app brute-forces an id and writes into it" path and the
 *     "an id from before a free/reuse now addresses somebody else's term"
 *     ABA path. `name` lives in a per-owner namespace (the key is
 *     owner+name), so one app cannot squat a name another app wants.
 *
 * I4. NOTHING IS ALLOCATED AFTER CREATE (§4.2). One block per term, taken
 *     at term_registry_create() from the port's allocator, sized by
 *     term_core_mem_size(). No malloc, realloc or free happens on feed,
 *     resize, drain, snapshot or read. Resize works inside the block that
 *     the worst-case geometry already paid for (§4.1). A leak or a
 *     fragmentation path therefore has nowhere to come from.
 *
 * I5. ERRORS ARE RETURN VALUES, NEVER EXCEPTIONS (§8). A closed, reused or
 *     foreign id is a term_err_t, and the binding turns it into a
 *     falsy/negative result. A terminal error must not kill an app.
 *
 * =====================================================================
 * SLOT STATE MACHINE (§3.1, the asynchronous two-stage quiesce)
 * =====================================================================
 *
 *                          create
 *            FREE ─────────────────────────► LIVE
 *              ▲                            │  ▲
 *              │                     owner  │  │ create(same owner+name)
 *              │                    stopped │  │  = re-attach
 *              │                   persist  ▼  │
 *              │                        DETACHED
 *              │                            │
 *              │   close / close_forced /   │  close / close_forced /
 *              │   owner stopped (non-      │  LRU evict when the persist
 *              │   persist)                 │  quota is full
 *              │            ┌───────────────┘
 *              │            ▼
 *              │         DYING ──── acks complete (reaper) ────┐
 *              │            │                                  │
 *              │            │ deadline exceeded (reaper)       │
 *              │            ▼                                  │
 *              │        ZOMBIE ──── late acks (reaper) ────────┤
 *              │                                               │
 *              └───────────── generation++, block freed ───────┘
 *
 * Stage 1 (term_registry_close / owner_stopped, runs on js_task): mark the
 * slot DYING, refuse further ingest, and ask the bound producer to detach
 * — for an ssh pipe that means shutdown() on the socket to break a
 * blocking recv, the same move that fixed the rapid-reopen UAF in
 * e1928c7. THEN RETURN IMMEDIATELY, joining nothing. js_task is shared by
 * every worker; a join here would make "close a terminal" an operation
 * that can stall all JS on the device.
 *
 * Stage 2 (term_registry_reap, runs on any task that is not js_task):
 * collect the producer's detach ack and the UI task's drain ack. When
 * both are in, bump the generation and release the block. If the deadline
 * (§3.1: 3s, the same cap microlink uses) passes first, the slot becomes a
 * ZOMBIE: memory retained, slot never reused, counter incremented. That is
 * a deliberate choice of a 132KB leak over a use-after-free. A late ack
 * still collects the zombie on a subsequent pass.
 *
 * No blind vTaskDelay anywhere in this protocol — the microlink lesson.
 *
 * =====================================================================
 * WHO RUNS WHAT
 * =====================================================================
 *
 *   js_task        create, close, feed, log, resize, show, snapshot, read
 *                  (the read pair posts to the UI task and joins with a
 *                  cap; everything else is non-blocking or bounded by the
 *                  ingest timeout)
 *   UI frame task  term_registry_ui_drain (parse, §5), term_registry_ui_visit
 *                  (blit, §9), and the serialisation half of snapshot/read
 *                  (§7.2 — always at a frame boundary, always consistent)
 *   reaper task    term_registry_reap
 *   any task       term_registry_producer_ack, term_registry_system_log,
 *                  the introspection calls
 *
 * The grid stays single-writer because only the UI task parses (§5), and
 * that is why term_registry_resize posts instead of writing cols/rows in
 * place: a torn dimension read inside the parser is not a bug this design
 * is willing to have.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "term_core.h"
#include "term_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/* Sizes and identities (§3.1)                                           */
/* ===================================================================== */

#define TERM_SLOT_BITS   3
#define TERM_SLOT_COUNT  (1 << TERM_SLOT_BITS)   /* 8, I1 / §4.1        */
#define TERM_SLOT_MASK   (TERM_SLOT_COUNT - 1)
#define TERM_GEN_BITS    28
#define TERM_GEN_MAX     ((1u << TERM_GEN_BITS) - 1u)

/* Persist slots, initial value from §3.1. §12 lists the final number as
 * open pending on-device measurement, so it is a config field too. */
#define TERM_PERSIST_MAX_DEFAULT 4

/* Name is the per-owner re-attach key (§3.1). 16 including the NUL, as in
 * the §3.1 struct sketch. */
#define TERM_NAME_MAX  16

/*
 * Owner is the app name from a signed push, i.e. the trusted identity the
 * gate of I3 rests on. DELIBERATE DEVIATION from the §3.1 sketch's
 * char owner[16]: app names are MQJS_APP_NAME_MAX (32) bytes, and
 * truncating a 32-byte name into a 16-byte field would let two distinct
 * apps present the same owner identity — silently merging their
 * namespaces and defeating the very check the field exists for. The
 * registry sizes the field to hold a whole app name, and rejects (never
 * truncates) anything longer. The implementation static-asserts
 * TERM_OWNER_MAX >= MQJS_APP_NAME_MAX in the IDF build.
 */
#define TERM_OWNER_MAX 32

/* A term id: (generation << TERM_SLOT_BITS) | slot. Generations start at
 * 1, so a valid id is always >= TERM_SLOT_COUNT and 0 is free to mean
 * "none" (I2). Signed because it crosses into JS as a number. */
typedef int32_t term_id_t;

#define TERM_ID_INVALID ((term_id_t)0)

static inline int term_id_slot(term_id_t id)
{
    return (int)((uint32_t)id & TERM_SLOT_MASK);
}
static inline uint32_t term_id_generation(term_id_t id)
{
    return ((uint32_t)id >> TERM_SLOT_BITS) & TERM_GEN_MAX;
}

/* ===================================================================== */
/* Errors (§8: return values, never exceptions — I5)                     */
/* ===================================================================== */

typedef enum {
    TERM_OK             =  0,
    TERM_ERR_INVAL      = -1,  /* malformed argument: NULL, bad geometry,
                                * name/owner too long or empty            */
    TERM_ERR_NOT_READY  = -2,  /* no port installed, or registry not init */
    TERM_ERR_BAD_ID     = -3,  /* slot out of range, or slot is FREE      */
    TERM_ERR_STALE      = -4,  /* generation mismatch: the id names a term
                                * that was closed and the slot reused (the
                                * ABA case of I3)                         */
    TERM_ERR_NOT_OWNER  = -5,  /* live term, wrong owner (I3)             */
    TERM_ERR_DYING      = -6,  /* DYING or ZOMBIE: ingest refused (§3.1
                                * stage 1)                                */
    TERM_ERR_NO_SLOT    = -7,  /* all 8 slots taken, or the persist quota
                                * is full and nothing is evictable (§3.1) */
    TERM_ERR_NO_MEM     = -8,  /* the one create-time allocation failed
                                * (I4: the only place this can occur)     */
    TERM_ERR_EXISTS     = -9,  /* owner+name is live and attached, so the
                                * create is not a re-attach               */
    TERM_ERR_BUSY       = -10, /* SPSC violation: feed() on a piped term,
                                * or a second producer (§5)               */
    TERM_ERR_MODE       = -11, /* operation not defined for this mode
                                * (§3.2), e.g. scrollback read on a term
                                * created with scrollback disabled        */
    TERM_ERR_TIMEOUT    = -12, /* bounded wait expired: ingest gave up
                                * after the ingest timeout and dropped
                                * (§5), or a read's UI join hit its cap
                                * (§7.2)                                  */
    TERM_ERR_POST       = -13, /* the UI queue would not take the job     */
    TERM_ERR_TRUNC      = -14, /* output buffer too small; what fits was
                                * written and the result reports the
                                * needed size                             */
} term_err_t;

/* Stable short string for logs and probe replies; never NULL. */
const char *term_err_str(term_err_t err);

/* ===================================================================== */
/* Slot state (§3.1 quiesce)                                             */
/* ===================================================================== */

typedef enum {
    TERM_SLOT_FREE = 0, /* no term; block released, generation already
                         * advanced past whatever last lived here        */
    TERM_SLOT_LIVE,     /* attached to a running owner                   */
    TERM_SLOT_DETACHED, /* persist term whose owner stopped; alive, still
                         * ingesting from platform producers, waiting for
                         * a same-owner+same-name create to re-attach.
                         * detached_since_ms drives the LRU evict         */
    TERM_SLOT_DYING,    /* stage 1 done; ingest refused; waiting on acks  */
    TERM_SLOT_ZOMBIE,   /* stage 2 timed out; memory intentionally
                         * retained and the slot never reused (§3.1)      */
} term_slot_state_t;

/* ===================================================================== */
/* Configuration                                                         */
/* ===================================================================== */

/* Display rectangle for term_registry_show (§8). Canvas pixels, the same
 * coordinate space ui_tab5 uses. §12 leaves the composition order against
 * ui.* canvas drawing open; this struct is the part that is settled. */
typedef struct {
    int16_t x, y, w, h;
    bool    visible;    /* false hides without forgetting the rect (tab
                         * switching is show/hide, §8)                    */
} term_view_t;

typedef struct {
    /* Per-owner re-attach key (§3.1). Required, 1..TERM_NAME_MAX-1 bytes. */
    const char *name;
    /* Trusted app identity (I3). Required. Platform-side callers pass
     * TERM_OWNER_SYSTEM. */
    const char *owner;

    term_mode_t mode;     /* TERM_VT or TERM_LOG (§3.2)                   */
    bool        persist;  /* survive the owner stopping (§3.1)            */

    int cols, rows;       /* initial geometry                             */

    /* Allocation maxima; 0 selects the term_core defaults, i.e. the
     * worst case over both orientations (§4.1) so a rotation never
     * reallocates. */
    int max_cols, max_rows, max_cells;

    /* Scrollback budget (§4.1: 48KB arena / 1,024 lines). 0 selects the
     * mode's default; both explicitly 0 is only meaningful for a VT term
     * that genuinely wants no history. */
    size_t scrollback_bytes;
    int    scrollback_lines;
} term_create_opts_t;

/* The owner string platform code uses for terms it creates itself (the
 * system console). No app can be named this: app names come from signed
 * pushes and the name is reserved at the manager level. */
#define TERM_OWNER_SYSTEM "\1system"

typedef struct {
    /* Cap on persist terms (§3.1, initial 4; §12 open). 0 selects
     * TERM_PERSIST_MAX_DEFAULT. */
    int persist_max;

    /* Stage-2 deadline before a DYING slot becomes a ZOMBIE (§3.1).
     * 0 selects TERM_QUIESCE_TIMEOUT_MS_DEFAULT (3000). */
    uint32_t quiesce_timeout_ms;

    /* How long an ingest call will wait for the table lock before it
     * drops and counts (§5). 0 selects TERM_INGEST_TIMEOUT_MS_DEFAULT
     * (20). */
    uint32_t ingest_timeout_ms;

    /* Cap on the UI join in snapshot/read (§7.2). 0 selects
     * TERM_READ_TIMEOUT_MS_DEFAULT. */
    uint32_t read_timeout_ms;

    /* Per-term bytes parsed per frame (§5: "drain は 1 フレームあたりの
     * バイト予算制"). Leftovers carry to the next frame so one noisy term
     * cannot eat the frame or starve LVGL. 0 selects the built-in
     * default. */
    int drain_budget_bytes;

    /* Create the shared system console at init (TERM_LOG, owner
     * TERM_OWNER_SYSTEM) — the "汎用 console 画面" that motivates this
     * phase (§11.2). NULL creates none, and term_registry_console_id()
     * then returns TERM_ID_INVALID. `owner` in the pointed-to opts is
     * ignored and forced to TERM_OWNER_SYSTEM. */
    const term_create_opts_t *console;
} term_registry_config_t;

/* ===================================================================== */
/* Lifecycle of the registry itself                                      */
/* ===================================================================== */

/*
 * Initialise the table. Requires an installed port (term_port_install).
 * `cfg` may be NULL for all defaults. Idempotent-hostile: a second call
 * without deinit returns TERM_ERR_NOT_READY rather than silently
 * reinitialising over live terms.
 */
term_err_t term_registry_init(const term_registry_config_t *cfg);

/*
 * Tear the whole table down: force every slot through stage 1, run the
 * reaper to completion, release the mutex. Intended for host tests, which
 * build and destroy a registry per case; on the device nothing calls it.
 * Returns the number of slots that could NOT be collected (they had
 * outstanding acks and were left as zombies) — a test that expects a
 * clean teardown asserts 0.
 */
int term_registry_deinit(void);

bool term_registry_ready(void);

/* ===================================================================== */
/* Create, re-attach, close (§3.1, §8)                                   */
/* ===================================================================== */

/*
 * term.create (§8). Two outcomes share this entry point:
 *
 *   NEW: no slot holds (owner, name). A FREE slot is taken, one block is
 *     allocated (I4) and a fresh generation is stamped. Fails with
 *     TERM_ERR_NO_SLOT when all 8 are occupied — DYING and ZOMBIE slots
 *     count as occupied.
 *
 *   RE-ATTACH: a DETACHED slot holds exactly (owner, name) and is
 *     persist. Its id — generation included — is returned unchanged and
 *     the session continues with its scrollback intact (§3.1, the tmux
 *     model). *out_reattached is set true. The requested geometry is
 *     applied as a resize; mode is NOT changed and a mismatch is
 *     TERM_ERR_MODE. A LIVE slot with the same key is TERM_ERR_EXISTS,
 *     not a second handle to the same term.
 *
 * The key is (owner, name) — never name alone. Another app asking for
 * "ssh0" gets its own term, and can neither read nor squat this one
 * (§3.1).
 *
 * PERSIST QUOTA. If persist is requested and the quota is already full,
 * the registry evicts the persist term that has been DETACHED longest
 * (§3.1 (c) — LRU by detach time, so an app that never runs again cannot
 * hold a slot forever). Eviction takes that slot through stage 1; its
 * memory comes back on a later reaper pass, so the create may still
 * return TERM_ERR_NO_SLOT if nothing else is free right now. A caller
 * that gets TERM_ERR_NO_SLOT may retry after a reaper pass. If every
 * persist term is LIVE, nothing is evicted and the create fails.
 *
 * out_id is written only on TERM_OK. out_reattached may be NULL.
 */
term_err_t term_registry_create(const term_create_opts_t *opts,
                                term_id_t *out_id,
                                bool *out_reattached);

/*
 * term.close (§8) — stage 1 of the quiesce, and nothing more. Marks the
 * slot DYING, refuses subsequent ingest with TERM_ERR_DYING, and asks any
 * bound producer to detach. RETURNS IMMEDIATELY: it does not wait for the
 * producer, the UI drain, or the free (§3.1). The id is dead to the
 * caller the moment this returns TERM_OK.
 *
 * Owner-gated like everything else (I3): closing somebody else's term is
 * TERM_ERR_NOT_OWNER. Closing an already-DYING term is TERM_OK and a
 * no-op.
 */
term_err_t term_registry_close(term_id_t id, const char *owner);

/*
 * Administrative force-close (§3.1 (b)): reclaim the slot of an app that
 * no longer runs. No owner check — the gate is elsewhere, on the signed
 * management request that reaches this call (§7.2, phase 3). Not reachable
 * from JS. Same stage-1 semantics as term_registry_close.
 */
term_err_t term_registry_close_forced(term_id_t id);

/*
 * App-manager teardown hook (§3.1: "非 persist の term は teardown フック
 * で自動解放", the same place the widget retain-stack is swept). For every
 * term owned by `owner`:
 *   non-persist -> stage 1 (DYING)
 *   persist     -> DETACHED, detached_since_ms = now, ready to be
 *                  re-attached by a future create or evicted by LRU
 * Returns the number of slots touched. Safe to call for an owner with no
 * terms.
 */
int term_registry_owner_stopped(const char *owner);

/* Look up (owner, name). TERM_ERR_BAD_ID when the pair names nothing.
 * A DETACHED term is found; a DYING/ZOMBIE one is not. */
term_err_t term_registry_find(const char *owner, const char *name,
                              term_id_t *out_id);

/* ===================================================================== */
/* Writes — all owner-gated (I3)                                         */
/* ===================================================================== */

/*
 * term.feed (§8): raw bytes with full VT interpretation. The bytes are
 * copied into the term's byte ring and parsed later, on the UI task
 * (§5) — this call never runs the parser and never blocks on it.
 *
 * SPSC (§5): a VT term has exactly one producer. If a pipe is bound, feed
 * is TERM_ERR_BUSY. Without a pipe, feed is the producer and js_task is
 * the only writer.
 *
 * Bounded, lossy, counted: if the ring is full or the lock is not
 * acquired within the ingest timeout, as much as fits is taken, the
 * remainder is dropped, the drop counter advances and the call returns
 * TERM_ERR_TIMEOUT. Logging never blocks an app (§5). *out_written, when
 * non-NULL, receives the byte count actually accepted.
 */
term_err_t term_registry_feed(term_id_t id, const char *owner,
                              const uint8_t *bytes, size_t len,
                              size_t *out_written);

/*
 * term.log (§8): the line-oriented convenience. Appends a newline,
 * AND IS THE TEE POINT FOR THE LP BLACK BOX (§4.4) — see the note above
 * term_registry_platform_log below for what that means and why it is here
 * rather than in the parser.
 *
 * accepts no escape interpretation beyond what the core already does, and
 * is ATOMIC PER LINE — a line is taken whole or dropped whole, so
 * concurrent writers never interleave halves of two messages (§3.2's
 * "行単位アトミック drop + カウンタ"). Multiple writers are safe here
 * precisely because this path goes through the lock rather than straight
 * into the ring.
 *
 * Legal on both modes. On a VT term it is equivalent to feeding the text
 * plus CRLF — and a VT term is NOT teed to the black box (§3.2's table:
 * "LP 黒箱 tee — TERM_VT: しない"), which the mode makes structural since a
 * term's mode is fixed at create and a re-attach with a different one is
 * TERM_ERR_MODE.
 */
term_err_t term_registry_log(term_id_t id, const char *owner,
                             const char *text, size_t len);

/*
 * term.resize (§8, §5). Posted to the UI task: cols/rows are written only
 * there, so the parser can never observe a torn geometry mid-sequence.
 * Returns as soon as the job is queued (TERM_ERR_POST if the queue
 * refuses it); the geometry visible to a subsequent read may still be the
 * old one for up to a frame. Content survives — term_core_resize carries
 * it over and re-wraps scrollback at the new width (§4.1, §3.1).
 *
 * Notifying the pty of the new size is the caller's business (§8).
 */
term_err_t term_registry_resize(term_id_t id, const char *owner,
                                int cols, int rows);

/*
 * term.show (§8): where this term paints inside the owner's screen.
 * Tab switching is show/hide of several terms, not create/destroy.
 * `view` NULL hides. Takes effect on the next frame.
 */
term_err_t term_registry_show(term_id_t id, const char *owner,
                              const term_view_t *view);

/* ===================================================================== */
/* Reads — owner-gated, and non-mutating (§7.2)                          */
/* ===================================================================== */

/*
 * There is no cross-app read API, on purpose: §7.2's access table gives
 * other apps no gate at all because the entry point does not exist. The
 * only unowned reader is the MQTT responder behind an Ed25519 signature,
 * which is phase 3 and will enter through its own privileged calls.
 *
 * Both reads are serialised ON THE UI TASK at a frame boundary (§7.2), so
 * a snapshot is always internally consistent and never races the parser.
 * The caller posts a job and joins with a cap (config read_timeout_ms);
 * a caller that is already the UI task runs it inline. On timeout the
 * result is TERM_ERR_TIMEOUT and the output buffer is untouched — the
 * registry retains the job's storage until the job has run, so a timed-out
 * caller's stack is never written to afterwards.
 *
 * The id is re-validated (generation and owner) INSIDE the job, not just
 * at post time (§7.2): if the term was closed and its slot reused in
 * between, the answer is an error, not another app's screen.
 *
 * Neither call mutates anything — a probe must not perturb what it
 * measures (§7.2).
 */

/* term.snapshot: the visible screen as UTF-8, one line per row, CONT
 * cells skipped, trailing blanks trimmed, no attributes (§7.2). ~14KB
 * worst case. *out_len (may be NULL) receives the byte length excluding
 * the NUL; TERM_ERR_TRUNC means out_size was too small and *out_len holds
 * what would have been needed. */
term_err_t term_registry_snapshot(term_id_t id, const char *owner,
                                  char *out, size_t out_size,
                                  size_t *out_len);

typedef struct {
    uint32_t first;      /* oldest surviving scrollback id at read time */
    uint32_t next;       /* id to pass as `from` for the following chunk */
    int      lines;      /* logical lines written into `out`            */
    size_t   bytes;      /* bytes written, excluding the NUL            */
    bool     truncated;  /* stopped early because `out` filled up       */
} term_read_result_t;

/*
 * term.read(id, from, n): a chunk of scrollback as logical lines joined by
 * '\n' (§7.2, §8). Ids are the monotonic line ids of term_core; `from`
 * below the surviving range starts at the oldest line instead of failing,
 * and `res->first` tells the caller what it missed. n <= 0 is
 * TERM_ERR_INVAL. `res` may be NULL.
 */
term_err_t term_registry_read(term_id_t id, const char *owner,
                              uint32_t from, int n,
                              char *out, size_t out_size,
                              term_read_result_t *res);

/* ===================================================================== */
/* Producers and the SPSC rule (§5)                                      */
/* ===================================================================== */

/*
 * A producer is a C-side source that writes into the byte ring directly,
 * without the bytes ever entering the JS heap (§5: the ssh pipe). Binding
 * one makes it the term's single writer and turns term_registry_feed into
 * TERM_ERR_BUSY.
 *
 * term.pipe itself is phase 4. These three calls exist now because the
 * quiesce state machine is DEFINED in terms of a producer's detach ack
 * (§3.1 stage 1/2), and a state machine whose main timeout path cannot be
 * exercised is a state machine nobody has tested — the phase 2 host tests
 * bind a fake producer, ack it late, and check that DYING becomes ZOMBIE
 * at the deadline and that a late ack still collects it.
 */

/*
 * Asked to detach. Called from the registry with the table lock NOT held,
 * possibly from js_task, so it must return promptly: the ssh
 * implementation does shutdown() on the socket to break a blocking recv
 * (e1928c7) and returns. The producer acknowledges later, from its own
 * task, once it is genuinely done touching the term.
 */
typedef void (*term_producer_detach_fn)(void *user, term_id_t id);

/* Bind the single producer. TERM_ERR_BUSY if one is already bound;
 * re-piping requires unbinding first and waiting for its ack (§5). */
term_err_t term_registry_producer_bind(term_id_t id, const char *owner,
                                       term_producer_detach_fn detach,
                                       void *user);

/* Voluntary unbind by the owner (not a close). Requests detach and marks
 * the ack outstanding, exactly as stage 1 does. */
term_err_t term_registry_producer_unbind(term_id_t id, const char *owner);

/*
 * "I have stopped touching this term." Called from the producer's own
 * task, with no owner argument — the producer is C-side platform code,
 * not an app. Safe to call on an id whose slot has already moved on: a
 * stale ack is dropped, counted, and is never mistaken for the ack of a
 * later term in the same slot (that is what the generation in the id is
 * for, I2).
 */
term_err_t term_registry_producer_ack(term_id_t id);

/* ===================================================================== */
/* Platform-side entry points                                            */
/* ===================================================================== */

/*
 * Run one reaper pass (§3.1 stage 2). Called periodically by a platform
 * task — never js_task, never the UI task's critical section — and
 * additionally whenever the port's reaper_wake fires. Frees every DYING
 * slot whose acks are all in (generation++ then release), promotes to
 * ZOMBIE every DYING slot past the quiesce deadline, and collects zombies
 * whose acks finally arrived. Returns the number of slots freed by this
 * pass.
 */
int term_registry_reap(void);

/*
 * UI frame task: drain each term's byte ring into its core and parse
 * (§5). Honours the per-term byte budget and carries leftovers to the
 * next frame, so a redraw storm from one term cannot starve the others or
 * LVGL. This is also where a DYING term's drain ack is produced. Returns
 * true if any visible term changed and a blit pass is worth running.
 */
bool term_registry_ui_drain(void);

/*
 * UI frame task: iterate the terms that should be painted, in slot order.
 * The callback receives the live core (read it with the term_core_* row
 * and dirty accessors) and the term's view rect. Do not call any other
 * registry function from inside the callback. Returns the number of terms
 * visited.
 *
 * The callback is where the ui_tab5 glue lives: blit dirty rows with the
 * existing ui.cells blitter and call term_core_dirty_clear() when done
 * (§9 — the renderer, and only the renderer, clears damage).
 */
typedef void (*term_ui_visit_fn)(term_id_t id, term_core_t *core,
                                 const term_view_t *view, void *user);
int term_registry_ui_visit(term_ui_visit_fn fn, void *user);

/*
 * The shared system console (TERM_LOG), or TERM_ID_INVALID when the
 * config asked for none. This is the "console 画面 any app can use"
 * (§2.1, §11.2).
 */
term_id_t term_registry_console_id(void);

/*
 * Which black-box partition a platform-side line belongs in (§4.4's static
 * 8KB/23KB split, and the `class` half of its {writer_id, class} record
 * tag). Both are class A data (§7.1); the split exists so that a chatty or
 * hostile app cannot flush the platform's last words, which is why the
 * caller — the only party that knows whether the writer is a trusted system
 * app — decides, and the registry does not guess from the name.
 */
typedef enum {
    TERM_WCLASS_APP    = 0,  /* user app print()/term.log -> 23KB partition */
    TERM_WCLASS_SYSTEM = 1,  /* platform events, system apps -> 8KB         */
} term_wclass_t;

/*
 * The platform log stream (§3.1: print sink -> registry). Two things happen,
 * in this order:
 *
 *   1. THE LINE IS TEED TO THE LP BLACK BOX (§4.4) — always, whether or not
 *      a console term exists and even before term_registry_init() has run.
 *      The flight recorder is deliberately more reliable than the console:
 *      if it needed the registry to be up, the lines from a boot that failed
 *      before bring-up — the ones worth having — would be the ones missing.
 *   2. the line is mirrored into the console term, if there is one, exactly
 *      as term_registry_log would.
 *
 * `writer` is the app name that produced the line (§4.4's writer_id; it
 * comes from a signed push, so it is a trustworthy identifier) and `wc`
 * chooses the partition. No owner gate: the console belongs to the platform
 * and this is the platform writing to it. Line-atomic and lossy.
 *
 * The return value describes step 2 only — TERM_ERR_NOT_READY when there is
 * no console, TERM_ERR_TIMEOUT when the console dropped the line. Step 1 has
 * no error a caller could act on; its counters are in term_lp_stats().
 *
 * SGR/escape stripping happens inside the black box (term_lp_ring.h, P4),
 * not here: the console wants the escapes, the recorder must never keep
 * them.
 */
term_err_t term_registry_platform_log(const char *writer, term_wclass_t wc,
                                      const char *text, size_t len);

/*
 * The phase-2 spelling, kept because it is what the existing callers and
 * suites use: exactly term_registry_platform_log(writer, TERM_WCLASS_APP,
 * text, len). §4.4's contract with app developers is attached to this call —
 * anything printed may be retained across a reset and pulled off the device
 * by the owner's key, so an app must not print secrets.
 */
term_err_t term_registry_system_log(const char *writer,
                                    const char *text, size_t len);

/* ===================================================================== */
/* Introspection (platform-side; never exposed to JS)                    */
/* ===================================================================== */

typedef struct {
    term_id_t         id;
    char              name[TERM_NAME_MAX];
    char              owner[TERM_OWNER_MAX];
    term_slot_state_t state;
    term_mode_t       mode;
    bool              persist;
    bool              piped;             /* a producer is bound (§5)     */
    int               cols, rows;
    size_t            mem_bytes;         /* the single block, I4         */
    int64_t           detached_since_ms; /* LRU key, 0 when attached     */
    uint32_t          pending_acks;      /* outstanding stage-2 acks     */
    uint64_t          bytes_in;          /* accepted into the ring       */
    uint64_t          bytes_dropped;     /* refused: ring full / timeout */
} term_slot_info_t;

/* No owner gate on purpose: this exposes owner names and is therefore
 * platform-only. It must not be reachable from a JS binding — that would
 * be the cross-app read path §7.2 says does not exist. */
term_err_t term_registry_info(term_id_t id, term_slot_info_t *out);

/* Fill `out` with up to `max` non-FREE slots; returns how many were
 * written. Same platform-only rule as term_registry_info. */
int term_registry_list(term_slot_info_t *out, int max);

typedef struct {
    uint32_t creates;        /* new terms                                */
    uint32_t reattaches;     /* persist terms picked back up (§3.1)      */
    uint32_t closes;         /* stage 1 entered                          */
    uint32_t freed;          /* stage 2 completed, block released        */
    uint32_t evicted;        /* persist terms dropped by the LRU (§3.1)  */
    uint32_t zombies;        /* DYING slots that hit the deadline —
                              * the number §3.1 wants made visible       */
    size_t   zombie_bytes;   /* memory those zombies still hold          */
    uint32_t gen_wraps;      /* 28-bit generation wraps; expected 0 for
                              * the life of the device (I2)              */
    uint32_t stale_acks;     /* acks for ids that had already moved on   */
    uint32_t denied_owner;   /* I3 rejections: wrong owner               */
    uint32_t denied_stale;   /* I3 rejections: stale generation          */
    uint32_t drops_lock;     /* ingest calls that lost the bounded wait  */
    uint32_t drops_full;     /* ingest bytes refused by a full ring      */
    uint32_t post_fails;     /* UI queue refusals                        */
} term_registry_stats_t;

void term_registry_stats(term_registry_stats_t *out);

#ifdef __cplusplus
}
#endif
