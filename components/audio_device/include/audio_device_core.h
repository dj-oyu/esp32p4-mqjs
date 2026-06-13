/*
 * audio_device core state machine (P1 of docs/audio-device-design.md).
 *
 * Pure C: no FreeRTOS, no I2S, no locks. A single owner (the manager task
 * on device, or the test harness on the host) serializes every call. The
 * core never performs a side effect itself — each mutating call appends an
 * ordered list of actions (start/stop/flush the backend, fire a producer
 * callback) that the caller executes AFTER releasing its lock. This keeps
 * the whole token/queue/session policy testable on the host without any
 * hardware (see tools/audio_device_test.c).
 *
 * Scope here is P1: a single EXCLUSIVE stream at a time, QUEUE conflict
 * policy. PREEMPT and OVERDUB are reserved in the type model and fall back
 * to QUEUE until P2/P3 (see the design doc §4, §11).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- shared scalar types (also used by the device API) -------------- */

typedef struct {
    uint32_t sample_rate; /* 8/12/16/24/44.1/48 kHz */
    uint8_t channels;     /* 1 | 2 (producer-side) */
} audio_format_t;

typedef enum {
    AUDIO_CONFLICT_QUEUE = 0,
    AUDIO_CONFLICT_PREEMPT, /* reserved (P2) -> QUEUE in P1 */
    AUDIO_CONFLICT_OVERDUB, /* reserved (P3) -> QUEUE in P1 */
} audio_conflict_policy_t;

typedef struct {
    audio_conflict_policy_t conflict;
    bool cancelable;
    uint8_t priority; /* higher dispatches first; FIFO within a priority */
} audio_request_policy_t;

/* A capability for a granted stream: slot identifies the stream record,
 * generation invalidates stale references. generation 0 is never valid. */
typedef struct {
    uint32_t slot;
    uint32_t generation;
} audio_token_t;

typedef uint32_t audio_request_id_t; /* 0 = invalid */

/* Callbacks are invoked by the caller while executing the action list, off
 * the lock and off the producer's stack. */
typedef void (*audio_granted_cb_t)(audio_request_id_t req, audio_token_t token,
                                   void *arg);
typedef void (*audio_cancelled_cb_t)(audio_request_id_t req, void *arg);

/* ---- core sizing ----------------------------------------------------- */

/* P1 uses one active stream (EXCLUSIVE). The array is sized >1 so a future
 * MIXING session can hold several without touching the token model. */
#define AUDIO_DEV_MAX_STREAMS 4
#define AUDIO_DEV_MAX_QUEUED  8
/* Worst case for one core call: STOP + START + GRANTED = 3; leave slack. */
#define AUDIO_DEV_MAX_ACTIONS 8

/* ---- actions the caller must execute, in order ----------------------- */

typedef enum {
    AUDIO_ACT_BACKEND_START,   /* configure/resume backend to u.fmt */
    AUDIO_ACT_BACKEND_STOP,    /* drain the backend (graceful finish) */
    AUDIO_ACT_BACKEND_FLUSH,   /* discard backend audio (abort/cancel);
                                  maps to STOP on device until a real flush
                                  API exists — semantics are "stop now" */
    AUDIO_ACT_MIXER_START,     /* open the overdub mixer at u.fmt (first
                                  OVERDUB stream of a MIXING session) */
    AUDIO_ACT_MIXER_REMOVE,    /* a MIXING stream ended; drain+drop u.slot's
                                  source while the session continues */
    AUDIO_ACT_MIXER_STOP,      /* last MIXING stream ended; drain+stop the
                                  mixer (precedes BACKEND_STOP/FLUSH) */
    AUDIO_ACT_FIRE_GRANTED,    /* call u.grant.cb(req, token, arg) */
    AUDIO_ACT_FIRE_CANCELLED,  /* call u.cancel.cb(req, arg) (cancel/reject) */
} audio_action_kind_t;

typedef struct {
    audio_action_kind_t kind;
    union {
        audio_format_t fmt;  /* BACKEND_START, MIXER_START */
        uint32_t slot;       /* MIXER_REMOVE */
        struct {
            audio_request_id_t req;
            audio_token_t token;
            audio_granted_cb_t cb;
            void *arg;
            bool mixing; /* true: a MIXING stream — set up its source ring */
        } grant;
        struct {
            audio_request_id_t req;
            audio_cancelled_cb_t cb;
            void *arg;
        } cancel;
    } u;
} audio_action_t;

typedef struct {
    audio_action_t a[AUDIO_DEV_MAX_ACTIONS];
    int n;
} audio_action_list_t;

/* ---- core state (opaque-ish; the wrapper/test owns one instance) ------ */

typedef enum {
    AUDIO_STREAM_FREE = 0,
    AUDIO_STREAM_PLAYING, /* granted + writable (STARTING/PLAYING collapsed) */
} audio_stream_state_t;

typedef struct {
    audio_stream_state_t state;
    uint32_t generation; /* 0 when free; matches the live token otherwise */
    audio_format_t format;
    audio_request_policy_t policy;
    audio_request_id_t req;
    audio_cancelled_cb_t on_cancelled;
    void *arg;
    bool mixing; /* member of the current MIXING session */
} audio_stream_slot_t;

typedef struct {
    bool used;
    audio_request_id_t id;
    audio_format_t format;
    audio_request_policy_t policy;
    audio_granted_cb_t on_granted;
    audio_cancelled_cb_t on_cancelled;
    void *arg;
    uint32_t seq; /* FIFO tiebreaker within a priority */
} audio_pending_req_t;

typedef enum {
    AUDIO_SESSION_IDLE = 0,
    AUDIO_SESSION_EXCLUSIVE,
    AUDIO_SESSION_MIXING, /* reserved (P3) */
} audio_session_state_t;

typedef struct {
    audio_stream_slot_t streams[AUDIO_DEV_MAX_STREAMS];
    audio_pending_req_t queue[AUDIO_DEV_MAX_QUEUED];
    audio_session_state_t session;
    int active_slot;             /* EXCLUSIVE: the live stream; -1 otherwise */
    audio_format_t session_fmt;  /* MIXING: the fixed session format */
    uint32_t gen_counter; /* monotonic generation source (skips 0) */
    uint32_t seq_counter; /* monotonic FIFO source */
} audio_dev_core_t;

/* ---- core API (single-threaded; caller serializes) ------------------- */

void audio_dev_core_init(audio_dev_core_t *c);

/* Submit a request with a caller-reserved id (so the producer can learn
 * its id synchronously and cancel it). If the device is idle the request
 * is granted immediately (START + GRANTED actions). If busy it is queued
 * by (priority desc, FIFO). A full queue rejects the request via a
 * CANCELLED action — there is no silent drop.
 *
 * OVERDUB opens or joins a MIXING session: the first OVERDUB stream on an
 * idle device opens one (BACKEND_START + MIXER_START + GRANTED), and a
 * later OVERDUB request with a matching format joins the running session
 * immediately (GRANTED, no backend churn). OVERDUB against an EXCLUSIVE
 * session, or a MIXING session of a different format, falls back to QUEUE.
 * PREEMPT is still treated as QUEUE (P2). */
void audio_dev_core_submit(audio_dev_core_t *c, audio_request_id_t id,
                           const audio_format_t *fmt,
                           const audio_request_policy_t *pol,
                           audio_granted_cb_t on_granted,
                           audio_cancelled_cb_t on_cancelled, void *arg,
                           audio_action_list_t *out);

/* Owner withdraws a still-queued request. Allowed regardless of cancelable
 * (it is the owner's own withdrawal). Returns true and emits CANCELLED if
 * the request was queued; false if it was unknown or already granted. */
bool audio_dev_core_cancel_request(audio_dev_core_t *c, audio_request_id_t id,
                                   audio_action_list_t *out);

/* Producer signals normal completion. Revokes the token, drains the
 * backend (STOP), then dispatches the next queued request if any. Returns
 * false for a stale/unknown token (no side effect). */
bool audio_dev_core_finish(audio_dev_core_t *c, audio_token_t token,
                           audio_action_list_t *out);

/* Producer's own immediate stop. Like finish but FLUSH (discard) instead
 * of drain. Returns false for a stale/unknown token. */
bool audio_dev_core_abort(audio_dev_core_t *c, audio_token_t token,
                          audio_action_list_t *out);

/* Hot path: is this token the live, writable stream? Pure, no actions. */
bool audio_dev_core_token_active(const audio_dev_core_t *c,
                                 audio_token_t token);

/* True if the token's live stream belongs to a MIXING session — i.e. its
 * writes must be routed to the per-stream source ring, not straight to the
 * backend. False for a stale token or an EXCLUSIVE stream. */
bool audio_dev_core_token_is_mixing(const audio_dev_core_t *c,
                                    audio_token_t token);
