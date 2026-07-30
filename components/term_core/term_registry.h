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
 *   js_task        create, close, feed, log, resize, show, snapshot, read,
 *                  pipe/producer_bind/_unbind, set_reply
 *                  (the read pair posts to the UI task and joins with a
 *                  cap; everything else is non-blocking or bounded by the
 *                  ingest timeout)
 *   UI frame task  term_registry_ui_drain (parse, §5), term_registry_ui_visit
 *                  (blit, §9), and the serialisation half of snapshot/read
 *                  (§7.2 — always at a frame boundary, always consistent).
 *                  The reply and caret callbacks fire from here (§6, §10.2)
 *   producer task  term_registry_producer_space / _write (the ssh pipe, §5)
 *   reaper task    term_registry_reap
 *   any task       term_registry_producer_ack, term_registry_system_log,
 *                  the introspection calls
 *
 * The grid stays single-writer because only the UI task parses (§5), and
 * that is why term_registry_resize posts instead of writing cols/rows in
 * place: a torn dimension read inside the parser is not a bug this design
 * is willing to have.
 *
 * =====================================================================
 * RECORDING (§4.4's exception of 2026-07-30) — READ THIS WHOLE BLOCK
 * =====================================================================
 *
 * WHAT IT IS FOR. §7.2 gives no cross-app read API, so what a remote host
 * printed on an operator's terminal could not be got off the device at all —
 * an nvim Lua traceback on the glass was unquotable. Recording mode puts a
 * recorded session's DISPLAY CONTENT into the LP black box, where the
 * existing signed pull already reaches it: `tools/bb_pull.py --session`
 * prints it back as a transcript on the PC. Making a session readable from
 * the PC is the GOAL of the feature, not a side effect of where the bytes
 * happened to land.
 *
 * R1. OFF BY DEFAULT, PER TERM, AND NEVER REMEMBERED ANYWHERE. This is the
 *     user's decision of 2026-07-30 and it is a GUARANTEE a test may assert,
 *     not a default a caller may change:
 *
 *       - there is no recording field on term_create_opts_t, and there will
 *         not be one. A create-time flag is a thing an app stores in its
 *         saved-tab record and replays months later;
 *       - nothing here writes the flag to NVS, `store`, or any other
 *         persistence, and nothing reads it from any;
 *       - RE-ATTACHING a persist term clears it, even though the term's
 *         scrollback survived. Picking a tmux session back up is a new
 *         session for this purpose;
 *       - DETACHING (the owner app stopping) clears it;
 *       - BINDING A PRODUCER clears it, which is what makes "a tab reused
 *         for a new ssh login after a disconnect" start off — including the
 *         app-level RIS path, which always re-pipes;
 *       - a producer's detach ack clears it (the session ended);
 *       - a reboot obviously clears it: the flag lives in .bss, not in the
 *         LP region.
 *
 *     The failure this forecloses is a forgotten opt-in: a flag set once,
 *     persisted, and still quietly recording sessions months later into a
 *     region that a pull returns in plaintext. Because it cannot outlive the
 *     session, "am I being recorded?" is answerable from the current screen
 *     and nothing else — which is why ssh_vt2's tab bar must show it.
 *
 * R2. THE TEE IS AT THE LINE LEVEL, NOT THE BYTE STREAM. term_core's
 *     term_record_fn fires once per grid row archived to scrollback; see the
 *     long comment there for why the raw ssh stream is the wrong seam (the
 *     black box strips every escape, so a stripped TUI redraw is soup) and
 *     why a redraw storm therefore costs nothing (in-place painting archives
 *     no rows, and the alt screen never reaches scrollback).
 *
 * R3. AND BECAUSE OF R2, WHOLE SCREENS ARE CAPTURED TOO. A line tee cannot
 *     see content that is displayed and never scrolls off — precisely the
 *     motivating case, an error in nvim's message area on the alt screen,
 *     read and then discarded. Three capture triggers close that hole:
 *
 *       (a) MANUAL: term_registry_record_screen(), the "I am looking at an
 *           error, save it" button. Cheapest and most predictable.
 *       (b) SETTLE-DEBOUNCED: while recording, when nothing has been fed for
 *           TERM_REC_SETTLE_MS and term_core_screen_hash() differs from the
 *           last captured screen, capture it. This records the screen states
 *           a human actually reads, and still costs nothing during a `top`
 *           storm — a storm never settles. Rate-limited to one landing per
 *           TERM_REC_SNAP_MIN_MS so the debounce cannot starve the ring;
 *           skipped evaluations are counted (rec_snaps_deferred).
 *       (c) END OF SESSION and THE PANIC PATH, so a hang during `top` still
 *           leaves the last screen.
 *
 *     A capture is a marker record plus one record per row, all tagged
 *     TERM_LP_CLASS_TERM | TERM_LP_F_SCREEN and bounded by
 *     TERM_REC_SCREEN_MAX bytes.
 *
 *     TRAILING blank rows are dropped (an empty bottom half of the screen is
 *     the screen being taller than the output). An INTERIOR blank row is kept
 *     and written as ONE SPACE: the ring refuses an empty payload, and
 *     dropping the row would shift every row below it in a pulled transcript,
 *     so a space is the only rendering under which "one record per row" and
 *     "no empty record" both hold. A reader of a transcript may rely on it.
 *
 *     A BLANK SCREEN still captures: the marker lands with zero rows, because
 *     "recording started here and the glass was empty" is a fact a transcript
 *     needs and is not the same as no capture at all.
 *
 * R4. WHAT THE OPERATOR ACCEPTED, and what an app must therefore tell the
 *     user: a recorded session is pullable IN PLAINTEXT by the holder of the
 *     signing key, and survives resets until the power is cut. With R3 that
 *     covers a secret merely DISPLAYED, not only one that scrolled. "Do not
 *     record a session that will display secrets" is a user-level rule; the
 *     platform's part of the bargain is R1 plus an indicator that cannot be
 *     missed.
 *
 * R5. IT COSTS NOTHING WHEN OFF. Recording on/off IS the installation and
 *     removal of term_core's record callback, not a boolean the hook tests:
 *     with recording off there is no route from a term to the ring, in the
 *     same sense that class B was unreachable before this feature existed
 *     (term_lp_ring.h, P3).
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

/* The ui.cells grid (ui_tab5.cpp UI_CELL_W/UI_CELL_H, HackGen 9x24). Used
 * only to turn a caret in cells into a caret in canvas pixels (§10.2);
 * term_registry_config_t::cell_w/cell_h override them. */
#define TERM_CELL_W_DEFAULT 9
#define TERM_CELL_H_DEFAULT 24

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

/* ---- recording (R1-R5 above) --------------------------------------------
 *
 * All three are tuning, not contract; the contract is that they exist and
 * that the ledger says what they were when the feature was verified.
 *
 * SETTLE 1,200 ms: long enough that a human has stopped reading a frame
 * mid-redraw, short enough to catch an error message before it is dismissed.
 * A `top` refreshing every second never reaches it, which is the point.
 *
 * SNAP_MIN 5,000 ms: the rate cap of R3(b). A pathological screen that
 * alternates every 1.2 s therefore lands ~3 KB every 5 s rather than every
 * 1.2 s — the APP partition turns over in ~40 s instead of ~10 s. Recording
 * IS eviction pressure; this bounds it without pretending to remove it.
 *
 * SCREEN_MAX 8,192 B: about a third of the APP partition, which a worst-case
 * 53x142 screen of 4-byte codepoints would otherwise exceed. A capture that
 * hits the cap stops on a row boundary and counts itself truncated; it must
 * not be able to evict the scrolled lines it is supposed to complement.
 */
#define TERM_REC_SETTLE_MS    1200u
#define TERM_REC_SNAP_MIN_MS  5000u
#define TERM_REC_SCREEN_MAX   8192u

/* The panic path's share, smaller because that capture is the least certain
 * code in the feature (it reads PSRAM with no lock, in panic context) and
 * because the note and the already-recorded lines matter more than it does. */
#define TERM_REC_PANIC_MAX    4096u

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

    /* Cell metrics of the display grid, in canvas pixels. The core
     * reports caret movement in CELLS (§10.2) but the platform's caret
     * state — the one ui.caret writes and ime_core reads — is in canvas
     * pixels, and this is the only place that conversion has both
     * numbers. 0 selects TERM_CELL_W_DEFAULT / TERM_CELL_H_DEFAULT. */
    int cell_w, cell_h;

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
 * SPSC (§5), same rule as feed: a piped term has exactly one producer, so
 * log is TERM_ERR_BUSY while a pipe is bound. Line-atomicity is what makes
 * several LOG writers safe; it does not make a second writer safe against
 * a producer that owns the ring's head.
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
/* Recording (§4.4's 2026-07-30 exception; R1-R5 at the top of this file)  */
/* ===================================================================== */

/*
 * Turn recording on or off for ONE term. Owner-gated and generation-checked
 * like every other write (I3): an app can only record its own terminal, and
 * there is no cross-app entry point, so this widens no read path that §7.2
 * closed.
 *
 * `on` = true installs the line tee and arms the settle-debounced screen
 * capture, and takes an IMMEDIATE screen capture: the human pressed the
 * button because of what is on the glass right now, and waiting for the
 * settle timer would lose it if the remote redraws first.
 *
 * `on` = false takes a final capture and then removes the tee, so stopping
 * recording keeps the frame the user was looking at when they stopped.
 *
 * Idempotent: setting the state it already has does nothing at all (no
 * capture, no counter).
 *
 * TERM_ERR_MODE ON A LOG TERM. Recording is the class-B exception and nothing
 * else: a TERM_LOG term's content is ALREADY in the black box as class A,
 * because term.log and the print sink tee there unconditionally (§4.4). Arming
 * a log term would write every archived row a SECOND time as
 * TERM_LP_CLASS_TERM, halving the retained history to say nothing new. So the
 * feature is refused where it has no work to do, rather than allowed and
 * documented as pointless.
 *
 * DOES NOT PERSIST ANYTHING — R1. Every lifecycle transition listed in R1
 * clears it, and nothing restores it.
 *
 * ARMING A DETACHED TERM IS ACCEPTED (TERM_OK), and this is deliberate rather
 * than an oversight the caller has to know about. It behaves like any other
 * arming — the tee goes on and the immediate capture of the retained screen
 * lands — and RE-ATTACHING CLEARS IT AGAIN (R1), so it cannot carry into the
 * session the app picks up. What it cannot do is answer R4's question: there is
 * no app showing the term, so there is no indicator, which is why nothing in
 * the platform arms a detached term on its own. It is refused nowhere because
 * the alternative — an error whose meaning is "this id is yours but not right
 * now" — is a worse contract than a flag the next transition throws away.
 */
term_err_t term_registry_record(term_id_t id, const char *owner, bool on);

/*
 * THE introspection point for recording state (there is deliberately only
 * one, so a UI cannot read a stale second copy): 1 when this term is
 * recording, 0 when it is not, or a negative term_err_t. Owner-gated.
 *
 * The aggregate counters are in term_registry_stats_t (rec_* fields), which
 * is platform-only like the rest of that struct.
 */
int term_registry_recording(term_id_t id, const char *owner);

/*
 * Capture the visible screen NOW into the black box — R3(a), the "I am
 * looking at an error, save it" action. TERM_ERR_INVAL when the term is not
 * recording: this is not a second, unannounced way to put session content in
 * the ring, it is a button that only exists while the indicator is lit.
 *
 * Runs inline under the table lock (the lock covers every core, so the grid
 * is not being parsed while this reads it) and appends up to
 * TERM_REC_SCREEN_MAX bytes. It does NOT reset the debounce rate limit — a
 * human pressing a button is not the case that limit exists for.
 */
term_err_t term_registry_record_screen(term_id_t id, const char *owner);

/*
 * The panic path's half of R3(c). Call from the panic handler AFTER
 * term_lp_panic_note(), never from anywhere else.
 *
 * WHAT IT DOES: for each recording term, visible ones first, appends the
 * screen through term_lp_panic_append() until TERM_REC_PANIC_MAX bytes are
 * spent. Takes no lock, allocates nothing, calls no FreeRTOS, and is
 * one-shot.
 *
 * WHY IT IS THE RISKIEST CODE HERE, stated plainly so a reviewer can weigh
 * it: it reads a grid in PSRAM with no lock, from panic context. The table
 * itself is .bss and safe to walk, but the core it points at is not ours
 * alone at that moment, and a cache-disabled panic could fault the read. The
 * design contains the damage rather than claiming it cannot happen:
 *
 *   - the panic NOTE is already published before this runs, and P5 publishes
 *     a header per record, so everything appended before a fault reads back;
 *   - therefore the worst case is that a panic becomes a double panic — the
 *     UART dump is cut short and the reset reason changes — WITHOUT losing
 *     black box content;
 *   - and the debounced capture (R3(b)) has usually already stored the last
 *     screen a human could read, so this path only adds the case where the
 *     screen was still changing when the device died.
 *
 * That is a device-checklist item, not a proof. Returns the number of
 * records appended.
 */
int term_registry_panic_capture(void);

/* ===================================================================== */
/* Producers and the SPSC rule (§5)                                      */
/* ===================================================================== */

/*
 * A producer is a C-side source that writes into the byte ring directly,
 * without the bytes ever entering the JS heap (§5: the ssh pipe). Binding
 * one makes it the term's single writer and turns term_registry_feed AND
 * term_registry_log into TERM_ERR_BUSY.
 *
 * The three phase-2 calls (bind/unbind/ack) exist because the quiesce state
 * machine is DEFINED in terms of a producer's detach ack (§3.1 stage 1/2),
 * and a state machine whose main timeout path cannot be exercised is a
 * state machine nobody has tested. Phase 4 adds what an actual pipe needs:
 * a reply route back to the channel (§6), and the two ring calls the
 * producer's own task uses (space + write).
 */

/*
 * Asked to detach. Called from the registry with the table lock NOT held,
 * possibly from js_task, so it must return promptly: the ssh
 * implementation does shutdown() on the socket to break a blocking recv
 * (e1928c7) and returns. The producer acknowledges later, from its own
 * task, once it is genuinely done touching the term.
 */
typedef void (*term_producer_detach_fn)(void *user, term_id_t id);

/*
 * A terminal reply (DSR 5n/6n, primary DA — §6). One sink type serves both
 * destinations of §6's "端末応答": the bound producer's channel (pipe: the
 * answer goes back over ssh entirely in C) and the per-term JS sink
 * (term.onReply, for a term somebody feeds by hand).
 *
 * CALLED FROM THE UI TASK, FROM INSIDE term_registry_ui_drain, WITH THE
 * TABLE LOCK HELD. Therefore:
 *   - it must not call any term_registry_* function (the mutex is not
 *     recursive: that is a deadlock, not a warning),
 *   - it must not block — post with a zero timeout and count the refusal,
 *   - `bytes` is valid only for the duration of the call (a stack buffer
 *     inside the parser); copy what you keep. len <= TERM_REPLY_MAX.
 */
#define TERM_REPLY_MAX 16   /* "\x1b[" + 5 + ';' + 5 + 'R' = 14 worst case */

typedef void (*term_reply_sink_fn)(void *user, term_id_t id,
                                   const char *bytes, size_t len);

/* Everything a producer supplies. `reply` may be NULL (replies then fall
 * through to the term's JS sink, if it has one, and are counted as dropped
 * otherwise). `user` is the producer's own cookie and doubles as its
 * identity on the ring calls below. */
typedef struct {
    term_producer_detach_fn detach;   /* required */
    term_reply_sink_fn      reply;    /* optional */
    void                   *user;
} term_producer_t;

/* Bind the single producer. TERM_ERR_BUSY if one is already bound;
 * re-piping requires unbinding first and waiting for its ack (§5). */
term_err_t term_registry_producer_bind(term_id_t id, const char *owner,
                                       term_producer_detach_fn detach,
                                       void *user);

/* Same, with a reply route. term_registry_producer_bind(id, o, d, u) is
 * exactly this with {d, NULL, u}. */
term_err_t term_registry_producer_bind_ex(term_id_t id, const char *owner,
                                          const term_producer_t *prod);

/*
 * term.pipe (§8) — bind a producer to a VT term, asking whatever was bound
 * before to detach first (§5: "再 pipe は旧接続の detach(ack join)を
 * 済ませてから新接続を張る").
 *
 * TERM_ERR_MODE unless the term is TERM_VT: a LOG term is multi-writer by
 * construction (the print sink, term.log) and a ring-owning producer cannot
 * coexist with that.
 *
 * WHEN A PRODUCER IS ALREADY BOUND this call requests its detach — exactly
 * as stage 1 does, outside the lock — and returns TERM_ERR_BUSY WITHOUT
 * binding the new one. It does NOT wait for the ack. §3.1's rule is not
 * negotiable here: the caller is js_task, shared by every worker, and a
 * join would make "switch this terminal to a new session" an operation that
 * can stall all JS on the device. The ack join is therefore the caller's
 * retry — a second term_registry_pipe() succeeds once the old producer has
 * acked from its own task, which for the ssh producer is one recv-timeout
 * away. Callers that would rather not retry call
 * term_registry_producer_unbind() when they close the old session and pipe
 * the new one when it comes up.
 */
term_err_t term_registry_pipe(term_id_t id, const char *owner,
                              const term_producer_t *prod);

/* Voluntary unbind by the owner (not a close). Requests detach and marks
 * the ack outstanding, exactly as stage 1 does. This is term.unpipe. */
term_err_t term_registry_producer_unbind(term_id_t id, const char *owner);

/*
 * "I have stopped touching this term." Called from the producer's own
 * task, with no owner argument — the producer is C-side platform code,
 * not an app. Safe to call on an id whose slot has already moved on: a
 * stale ack is dropped, counted, and is never mistaken for the ack of a
 * later term in the same slot (that is what the generation in the id is
 * for, I2).
 *
 * A DUPLICATE ACK on a term that is still alive is NOT stale and is not
 * counted as such: "stale" means the slot moved on (FREE, or a different
 * generation), and a second ack for the term that is still there is processed
 * as a session end again — recording is cleared and a final capture taken
 * (R1 + R3(c)), even though there is no longer a producer bound. That is the
 * fail-safe direction and it is why the check is written this way: the cost of
 * the duplicate is one more screen in the black box, while the cost of
 * ignoring an ack that is real — because the caller acked twice, or acked after
 * an unbind it did not observe — would be recording that outlived its session,
 * the one thing R1 exists to prevent.
 */
term_err_t term_registry_producer_ack(term_id_t id);

/*
 * ---------------------------------------------------------------------
 * THE RING, FROM THE PRODUCER'S SIDE (§5) — call these from the
 * producer's own task, and identify yourself with the `user` cookie the
 * bind was given. There is no owner argument: a producer is platform C
 * code, not an app. A cookie mismatch is TERM_ERR_BUSY, which is what a
 * producer that was unbound-but-has-not-acked-yet sees once someone else
 * has taken its place.
 * ---------------------------------------------------------------------
 *
 * BACKPRESSURE, NOT LOSS. §5 gives the pipe different rules from
 * JS/print ingest: "ssh パイプは満杯なら consume を止め SSH ウィンドウで
 * 背圧(エスケープ列を千切らない)". Truncating a run mid-escape would
 * corrupt the screen with no way back, so the pipe never drops. The
 * protocol is:
 *
 *   1. ask for space,
 *   2. read at most that many bytes off the wire,
 *   3. write them — all of them fit, guaranteed.
 *
 * Step 3 cannot come up short because the term has ONE producer (SPSC):
 * only this task adds to the ring and only the UI drain removes from it,
 * so the space observed in step 1 is a lower bound that can only grow.
 * When space is 0 the producer must stop reading its socket; the TCP
 * window closes, the peer's SSH window stops advancing, and the sender
 * throttles itself. Do not spin: sleep for a bounded interval (the ssh
 * session task uses its recv timeout) and ask again.
 *
 * Both calls take the table lock with the ingest bound (config
 * ingest_timeout_ms). Missing it is TERM_ERR_TIMEOUT with *out_space /
 * *out_written set to 0 and NOTHING CONSUMED — the producer still owns
 * its bytes and simply tries again. That is the difference from
 * term_registry_feed, which drops and counts on the same event.
 *
 * A SHORT WRITE — a producer that offered more than the space it asked
 * for — is also TERM_ERR_TIMEOUT, with *out_written telling the truth
 * about how much the ring took. Same code as feed's overflow because
 * "the ring would not take all of it" is the same event; the difference
 * is what happens to the remainder, and here it stays the caller's. The
 * registry does not drop it and does not count it as a drop.
 *
 * A DYING/ZOMBIE term answers TERM_ERR_DYING to both (its detach is
 * already on its way); the producer's job then is to stop and ack.
 */
term_err_t term_registry_producer_space(term_id_t id, void *user,
                                        size_t *out_space);

term_err_t term_registry_producer_write(term_id_t id, void *user,
                                        const uint8_t *bytes, size_t len,
                                        size_t *out_written);

/* ===================================================================== */
/* Terminal replies to JS (term.onReply, §6/§8)                          */
/* ===================================================================== */

/*
 * term.onReply(id, cb) — where a fed term's DSR/DA answers go. Owner-gated
 * like every other entry point; `fn` NULL removes the sink.
 *
 * ROUTING, in order (§6: "pipe 時は C 内でチャネルへ直接書き戻し。JS
 * フィード時は term.onReply"):
 *   1. a bound producer with a reply route gets it — the answer never
 *      enters the JS heap, which is the entire point of the pipe;
 *   2. otherwise this sink, if one is installed;
 *   3. otherwise the reply is dropped and counted (stats.replies_dropped).
 *
 * So a piped term does not call its onReply sink. Registering one anyway is
 * legal and useful — it becomes live the moment the pipe detaches — but
 * while a producer holds the term the sink stays silent. That is not a
 * degradation to work around: a terminal reply belongs to the stream that
 * asked the question.
 *
 * The sink runs on the UI task under the table lock — see
 * term_reply_sink_fn for what that forbids.
 */
term_err_t term_registry_set_reply(term_id_t id, const char *owner,
                                   term_reply_sink_fn fn, void *user);

/* ===================================================================== */
/* The caret sink (§10.2)                                                */
/* ===================================================================== */

/*
 * §10.2 settles this: when a VT term's cursor moves, the term PUSHES the
 * new caret rectangle into the same platform-side caret state that a JS
 * app fills by calling ui.caret(x, y, h). ime_core reads that state and
 * never learns that term exists; term never learns that ime exists. The
 * alternative the design rejects by name — ime polling a
 * term_cursor_pos() — is not implemented and should not be added.
 *
 * ONE sink for the whole registry, installed by the platform at bring-up,
 * not per term and not by an app: an app does not opt into having a
 * cursor. `fn` NULL removes it, and term_registry_deinit() clears it —
 * it belongs to the table's lifetime, so a suite that builds one registry
 * per case installs its sink per case too.
 *
 * WHEN IT FIRES. Only on change (term_core only notifies when
 * col/row/visible actually differ), only for a term whose view is
 * visible, and AT MOST ONCE PER TERM PER term_registry_ui_drain() PASS —
 * i.e. once per frame, not once per byte and not once per drain chunk.
 * The drain accumulates the latest position per slot and flushes after it
 * has released the table lock, so the sink runs UNLOCKED and may call
 * back into the registry (it still should not block: it shares the frame
 * with LVGL).
 *
 * Coordinates are canvas pixels: the term's view rect plus col/row times
 * the configured cell size. `h` is the cell height, i.e. what ui.caret's
 * third argument means. `visible` is false when the cursor is hidden
 * (DECTCEM off) — the anchor is then stale and the IME float should fall
 * back to whatever it uses for an app that never called ui.caret.
 */
typedef struct {
    term_id_t id;
    char      owner[TERM_OWNER_MAX]; /* who to attribute the caret to */
    int       x, y, h;               /* canvas pixels */
    bool      visible;
} term_caret_ev_t;

typedef void (*term_caret_sink_fn)(void *user, const term_caret_ev_t *ev);

void term_registry_set_caret_sink(term_caret_sink_fn fn, void *user);

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
    bool              recording;         /* R1; never persisted anywhere */
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

    /* -- phase 4 (§5 pipe, §6 replies, §10.2 caret) ------------------- */
    uint32_t pipes;          /* producers bound                          */
    uint32_t pipe_busy;      /* pipes refused because one was bound; the
                              * old producer was asked to detach         */
    uint64_t prod_bytes;     /* bytes taken from producers (never lossy) */
    uint32_t prod_stalls;    /* space queries answered 0 = the ring is
                              * full and the ssh window is doing the work*/
    uint32_t replies_piped;  /* replies routed into a producer's channel */
    uint32_t replies_js;     /* replies handed to a term.onReply sink    */
    uint32_t replies_dropped;/* replies nobody was listening for         */
    uint32_t caret_pushes;   /* caret events handed to the sink          */

    /* -- phase 5 (§4.4's recording exception; R1-R5) ------------------ */
    /*
     * WHAT A CAPTURE ADDS TO WHICH COUNTER, because a reader of these numbers
     * has to be able to reconstruct the records: rec_screens counts MARKERS
     * (one per landing) and rec_screen_rows counts ROWS, so a capture that
     * landed appends rec_screen_rows + 1 records and a blank-screen capture is
     * a landing with zero rows. The PANIC path is the exception and moves
     * rec_panic_rows ONLY: its marker is not a rec_screens landing, because the
     * counter is the one thing a post-mortem reader cannot re-derive and
     * "screens the running system captured" is the fact worth keeping there.
     */
    uint32_t rec_on;         /* recording turned on, per term per session */
    uint32_t rec_off;        /* turned off explicitly (not by lifecycle)  */
    uint32_t rec_cleared;    /* turned off BY a lifecycle transition, i.e.
                              * the R1 guarantee doing its work: re-attach,
                              * detach, pipe bind, detach ack, close       */
    uint32_t rec_lines;      /* archived rows teed into the black box     */
    uint32_t rec_screens;    /* screen captures that landed               */
    uint32_t rec_screen_rows;/* rows those captures wrote                 */
    uint32_t rec_snaps_deferred; /* settle evaluations the SNAP_MIN rate
                              * limit turned away, once per settle episode */
    uint32_t rec_screen_trunc;/* captures cut at TERM_REC_SCREEN_MAX      */
    uint32_t rec_panic_rows; /* rows the panic path managed to append     */
} term_registry_stats_t;

void term_registry_stats(term_registry_stats_t *out);

#ifdef __cplusplus
}
#endif
