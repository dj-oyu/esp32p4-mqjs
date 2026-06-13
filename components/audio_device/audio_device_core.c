/*
 * audio_device core state machine. See audio_device_core.h for the model.
 * Pure C, no platform dependencies — the same object file links into the
 * device wrapper and the host test.
 */
#include "audio_device_core.h"

#include <string.h>

/* ---- action list helpers -------------------------------------------- */

static void act_push(audio_action_list_t *out, const audio_action_t *act)
{
    if (!out || out->n >= AUDIO_DEV_MAX_ACTIONS)
        return; /* sized for the worst case; a drop here is a core bug */
    out->a[out->n++] = *act;
}

static void emit_backend(audio_action_list_t *out, audio_action_kind_t kind,
                         const audio_format_t *fmt)
{
    audio_action_t act = { .kind = kind };
    if (fmt)
        act.u.fmt = *fmt;
    act_push(out, &act);
}

static void emit_granted(audio_action_list_t *out, audio_request_id_t req,
                         audio_token_t token, audio_granted_cb_t cb, void *arg,
                         bool mixing)
{
    audio_action_t act = { .kind = AUDIO_ACT_FIRE_GRANTED };
    act.u.grant.req = req;
    act.u.grant.token = token;
    act.u.grant.cb = cb;
    act.u.grant.arg = arg;
    act.u.grant.mixing = mixing;
    act_push(out, &act);
}

static void emit_mixer_remove(audio_action_list_t *out, uint32_t slot)
{
    audio_action_t act = { .kind = AUDIO_ACT_MIXER_REMOVE };
    act.u.slot = slot;
    act_push(out, &act);
}

static bool format_compatible(const audio_format_t *a, const audio_format_t *b)
{
    return a->sample_rate == b->sample_rate && a->channels == b->channels;
}

static int count_active_streams(const audio_dev_core_t *c)
{
    int n = 0;
    for (int i = 0; i < AUDIO_DEV_MAX_STREAMS; i++)
        if (c->streams[i].state == AUDIO_STREAM_PLAYING)
            n++;
    return n;
}

static void emit_cancelled(audio_action_list_t *out, audio_request_id_t req,
                           audio_cancelled_cb_t cb, void *arg)
{
    audio_action_t act = { .kind = AUDIO_ACT_FIRE_CANCELLED };
    act.u.cancel.req = req;
    act.u.cancel.cb = cb;
    act.u.cancel.arg = arg;
    act_push(out, &act);
}

/* ---- lifecycle ------------------------------------------------------- */

void audio_dev_core_init(audio_dev_core_t *c)
{
    memset(c, 0, sizeof *c);
    c->session = AUDIO_SESSION_IDLE;
    c->active_slot = -1;
    c->gen_counter = 0; /* first grant bumps to 1; 0 stays the invalid gen */
    c->seq_counter = 0;
}

static int find_free_slot(const audio_dev_core_t *c)
{
    for (int i = 0; i < AUDIO_DEV_MAX_STREAMS; i++)
        if (c->streams[i].state == AUDIO_STREAM_FREE)
            return i;
    return -1;
}

/* Populate a free slot from `pr` and return its live token (gen != 0). */
static audio_token_t claim_slot(audio_dev_core_t *c, int slot,
                                const audio_pending_req_t *pr, bool mixing)
{
    uint32_t gen = ++c->gen_counter;
    if (gen == 0)
        gen = ++c->gen_counter; /* never hand out the invalid generation */

    audio_stream_slot_t *s = &c->streams[slot];
    s->state = AUDIO_STREAM_PLAYING;
    s->generation = gen;
    s->format = pr->format;
    s->policy = pr->policy;
    s->req = pr->id;
    s->on_cancelled = pr->on_cancelled;
    s->arg = pr->arg;
    s->mixing = mixing;

    audio_token_t token = { .slot = (uint32_t)slot, .generation = gen };
    return token;
}

/* Grant `pr` on an idle device: OVERDUB opens a MIXING session, anything
 * else opens an EXCLUSIVE one. Used by submit (idle path) and dispatch. */
static void grant_request(audio_dev_core_t *c, const audio_pending_req_t *pr,
                          audio_action_list_t *out)
{
    int slot = find_free_slot(c);
    if (slot < 0) {
        /* Impossible on an idle device (all slots free); guard anyway. */
        emit_cancelled(out, pr->id, pr->on_cancelled, pr->arg);
        return;
    }

    bool mixing = pr->policy.conflict == AUDIO_CONFLICT_OVERDUB;
    audio_token_t token = claim_slot(c, slot, pr, mixing);

    emit_backend(out, AUDIO_ACT_BACKEND_START, &pr->format);
    if (mixing) {
        c->session = AUDIO_SESSION_MIXING;
        c->active_slot = -1;
        c->session_fmt = pr->format;
        emit_backend(out, AUDIO_ACT_MIXER_START, &pr->format);
    } else {
        c->session = AUDIO_SESSION_EXCLUSIVE;
        c->active_slot = slot;
    }
    emit_granted(out, pr->id, token, pr->on_granted, pr->arg, mixing);
}

/* Add an OVERDUB stream to the already-running MIXING session. Returns
 * false (caller should queue) if no slot is free. */
static bool join_mixing(audio_dev_core_t *c, const audio_pending_req_t *pr,
                        audio_action_list_t *out)
{
    int slot = find_free_slot(c);
    if (slot < 0)
        return false;
    audio_token_t token = claim_slot(c, slot, pr, true);
    emit_granted(out, pr->id, token, pr->on_granted, pr->arg, true);
    return true;
}

/* Pick the best queued request: highest priority, then lowest seq (FIFO).
 * Returns the queue index or -1 if empty. */
static int pick_next(const audio_dev_core_t *c)
{
    int best = -1;
    for (int i = 0; i < AUDIO_DEV_MAX_QUEUED; i++) {
        if (!c->queue[i].used)
            continue;
        if (best < 0) {
            best = i;
            continue;
        }
        const audio_pending_req_t *b = &c->queue[best];
        const audio_pending_req_t *q = &c->queue[i];
        if (q->policy.priority > b->policy.priority ||
            (q->policy.priority == b->policy.priority && q->seq < b->seq))
            best = i;
    }
    return best;
}

/* If the device is idle and a request is waiting, grant it. */
static void dispatch_next(audio_dev_core_t *c, audio_action_list_t *out)
{
    if (c->session != AUDIO_SESSION_IDLE)
        return;
    int idx = pick_next(c);
    if (idx < 0)
        return;
    audio_pending_req_t pr = c->queue[idx];
    c->queue[idx].used = false;
    grant_request(c, &pr, out);
}

/* ---- submit ---------------------------------------------------------- */

void audio_dev_core_submit(audio_dev_core_t *c, audio_request_id_t id,
                           const audio_format_t *fmt,
                           const audio_request_policy_t *pol,
                           audio_granted_cb_t on_granted,
                           audio_cancelled_cb_t on_cancelled, void *arg,
                           audio_action_list_t *out)
{
    audio_pending_req_t pr = {
        .used = true,
        .id = id,
        .format = *fmt,
        .policy = *pol,
        .on_granted = on_granted,
        .on_cancelled = on_cancelled,
        .arg = arg,
        .seq = c->seq_counter++,
    };

    /* Idle: open a session straight away (EXCLUSIVE, or MIXING for OVERDUB). */
    if (c->session == AUDIO_SESSION_IDLE) {
        grant_request(c, &pr, out);
        return;
    }

    /* OVERDUB into a running, format-compatible MIXING session joins it now
     * instead of queueing. Mismatched format or an EXCLUSIVE session falls
     * through to the queue. */
    if (pol->conflict == AUDIO_CONFLICT_OVERDUB &&
        c->session == AUDIO_SESSION_MIXING &&
        format_compatible(fmt, &c->session_fmt)) {
        if (join_mixing(c, &pr, out))
            return;
        /* no free slot — fall through and queue */
    }

    for (int i = 0; i < AUDIO_DEV_MAX_QUEUED; i++) {
        if (!c->queue[i].used) {
            c->queue[i] = pr;
            return;
        }
    }
    /* Queue full: reject (no silent drop). */
    emit_cancelled(out, id, on_cancelled, arg);
}

/* ---- cancel a queued request ---------------------------------------- */

bool audio_dev_core_cancel_request(audio_dev_core_t *c, audio_request_id_t id,
                                   audio_action_list_t *out)
{
    for (int i = 0; i < AUDIO_DEV_MAX_QUEUED; i++) {
        if (c->queue[i].used && c->queue[i].id == id) {
            audio_pending_req_t pr = c->queue[i];
            c->queue[i].used = false;
            emit_cancelled(out, pr.id, pr.on_cancelled, pr.arg);
            return true;
        }
    }
    return false; /* unknown, or already granted (not cancelable via this) */
}

/* ---- token lookup ---------------------------------------------------- */

/* Return the live slot for a token, or NULL if stale/unknown. */
static audio_stream_slot_t *slot_for_token(audio_dev_core_t *c,
                                           audio_token_t token)
{
    if (token.generation == 0 || token.slot >= AUDIO_DEV_MAX_STREAMS)
        return NULL;
    audio_stream_slot_t *s = &c->streams[token.slot];
    if (s->state != AUDIO_STREAM_PLAYING || s->generation != token.generation)
        return NULL;
    return s;
}

bool audio_dev_core_token_active(const audio_dev_core_t *c, audio_token_t token)
{
    if (token.generation == 0 || token.slot >= AUDIO_DEV_MAX_STREAMS)
        return false;
    const audio_stream_slot_t *s = &c->streams[token.slot];
    return s->state == AUDIO_STREAM_PLAYING &&
           s->generation == token.generation;
}

bool audio_dev_core_token_is_mixing(const audio_dev_core_t *c,
                                    audio_token_t token)
{
    if (!audio_dev_core_token_active(c, token))
        return false;
    return c->streams[token.slot].mixing;
}

/* Free a slot's record (leaves session bookkeeping to the caller). */
static void free_slot(audio_stream_slot_t *s)
{
    s->state = AUDIO_STREAM_FREE;
    s->generation = 0;
    s->on_cancelled = NULL;
    s->arg = NULL;
    s->mixing = false;
}

/* ---- finish / abort -------------------------------------------------- */

static bool end_stream(audio_dev_core_t *c, audio_token_t token,
                       audio_action_kind_t backend, audio_action_list_t *out)
{
    audio_stream_slot_t *s = slot_for_token(c, token);
    if (!s)
        return false; /* stale token: no effect on the live session */

    bool mixing = s->mixing;
    uint32_t slot = (uint32_t)(s - c->streams);
    free_slot(s);

    if (mixing && count_active_streams(c) > 0) {
        /* The MIXING session continues with the other streams; just drop
         * this source. No backend stop, no dispatch. */
        emit_mixer_remove(out, slot);
        return true;
    }

    /* Either an EXCLUSIVE stream ended, or the last MIXING stream did:
     * the session is over. */
    if (mixing)
        emit_backend(out, AUDIO_ACT_MIXER_STOP, NULL);
    c->session = AUDIO_SESSION_IDLE;
    c->active_slot = -1;
    emit_backend(out, backend, NULL);
    dispatch_next(c, out);
    return true;
}

bool audio_dev_core_finish(audio_dev_core_t *c, audio_token_t token,
                           audio_action_list_t *out)
{
    return end_stream(c, token, AUDIO_ACT_BACKEND_STOP, out);
}

bool audio_dev_core_abort(audio_dev_core_t *c, audio_token_t token,
                          audio_action_list_t *out)
{
    return end_stream(c, token, AUDIO_ACT_BACKEND_FLUSH, out);
}
