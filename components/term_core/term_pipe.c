/*
 * term_pipe.c — term.pipe's producer (docs/term-design.md §5, §8).
 *
 * Contract in term_pipe.h. The device half is thin on purpose: everything
 * that could be got wrong (SPSC, the detach ack, the reply route, the
 * backpressure arithmetic) lives in term_registry.c and sshc.c, both of
 * which are testable without an ssh server. What is left here is the
 * plumbing that names one to the other.
 *
 * Lifetime of a pipe context:
 *
 *   bind      js_task     allocate, term_registry_pipe(), install the sink
 *   run       ssh task    space/write into the ring, replies back to tx
 *   detach    js_task     ask sshc to drop the sink; return, join nothing
 *   ack       ssh task    gone() -> term_registry_producer_ack(), free
 *
 * The context is freed by whichever task sends the ack, and `used` is
 * cleared LAST — after the ack — so a js_task that is scanning for a free
 * context can never adopt one the ssh task is still inside (the same
 * publish-last rule sshc uses for its session slots).
 */
#include "term_pipe.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "sshc.h"
#endif

/* One per term slot: a term has at most one producer, so there is nothing
 * to size this by other than the table itself (§3.1 I1). */
typedef struct {
    volatile bool used;
    term_id_t     term;
    int           handle;
#ifndef ESP_PLATFORM
    volatile bool detach_req; /* the host stub's deferred ack */
#endif
} term_pipe_t;

static term_pipe_t s_pipes[TERM_SLOT_COUNT];

static term_pipe_t *pipe_alloc(term_id_t id, int handle)
{
    int i;
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        if (s_pipes[i].used)
            continue;
        s_pipes[i].term = id;
        s_pipes[i].handle = handle;
#ifndef ESP_PLATFORM
        s_pipes[i].detach_req = false;
#endif
        s_pipes[i].used = true;
        return &s_pipes[i];
    }
    return NULL;
}

int term_pipe_count(void)
{
    int i, n = 0;
    for (i = 0; i < TERM_SLOT_COUNT; i++)
        if (s_pipes[i].used)
            n++;
    return n;
}

/* The ack, and the end of this context. Order matters: the registry must
 * be told before the slot is offered to anybody else. */
static void pipe_ack_and_free(term_pipe_t *p)
{
    term_registry_producer_ack(p->term);
    p->used = false;
}

#ifdef ESP_PLATFORM

/* ------------------------------------------------------------------ */
/* device: an ssh channel                                              */
/* ------------------------------------------------------------------ */

/* --- what the registry calls (js_task for detach, UI task for reply) --- */

static void pipe_detach(void *user, term_id_t id)
{
    term_pipe_t *p = (term_pipe_t *)user;
    (void)id;
    /* Returns at once. sshc turns this into a flag its session task reads,
     * and the ack comes back through pipe_gone() from that task — never
     * from here, which may be js_task (§3.1: stage 1 joins nothing). */
    mqjs_ssh_drop_sink(p->handle);
}

static void pipe_reply(void *user, term_id_t id, const char *bytes, size_t len)
{
    term_pipe_t *p = (term_pipe_t *)user;
    (void)id;
    /* §6: "pipe 時は C 内でチャネルへ直接書き戻し". Runs on the UI task
     * under the registry's lock, so it must not block — mqjs_ssh_write is
     * a zero-timeout stream-buffer send and returns false rather than
     * waiting. A refused reply is a reply the remote will ask for again;
     * a blocked UI task is a frozen device. */
    mqjs_ssh_write(p->handle, bytes, len);
}

/* --- what sshc calls, all on the session task --- */

static size_t pipe_space(void *user)
{
    term_pipe_t *p = (term_pipe_t *)user;
    size_t space = 0;
    /* Any error — DYING, stale, lock timeout — answers 0, which sshc reads
     * as "stop consuming". Backpressure is the correct response to every
     * one of them: the bytes stay on the wire and nothing is truncated. */
    term_registry_producer_space(p->term, p, &space);
    return space;
}

static size_t pipe_write(void *user, const void *data, size_t len)
{
    term_pipe_t *p = (term_pipe_t *)user;
    size_t took = 0;
    term_registry_producer_write(p->term, p, (const uint8_t *)data, len, &took);
    return took;
}

static void pipe_gone(void *user)
{
    pipe_ack_and_free((term_pipe_t *)user);
}

term_err_t term_pipe_bind(term_id_t id, const char *owner, int handle)
{
    term_producer_t prod;
    sshc_sink_t sink;
    term_pipe_t *p;
    term_err_t e;

    if (!mqjs_ssh_up(handle))
        return TERM_ERR_BAD_ID;
    p = pipe_alloc(id, handle);
    if (!p)
        return TERM_ERR_NO_SLOT;

    prod.detach = pipe_detach;
    prod.reply = pipe_reply;
    prod.user = p;
    e = term_registry_pipe(id, owner, &prod);
    if (e != TERM_OK) {
        p->used = false;
        return e;
    }

    sink.space = pipe_space;
    sink.write = pipe_write;
    sink.gone = pipe_gone;
    sink.user = p;
    if (!mqjs_ssh_set_sink(handle, &sink)) {
        /* The session went away (or already has a sink) between the check
         * above and here. Unwind by hand rather than through the detach
         * path: the detach would ask sshc to drop a sink that was never
         * installed, and nobody would ever send the ack. We are the
         * producer and we have provably never touched the term, so acking
         * here is exactly true. */
        term_registry_producer_unbind(id, owner);
        pipe_ack_and_free(p);
        return TERM_ERR_BAD_ID;
    }
    return TERM_OK;
}

void term_pipe_pump(void)
{
    /* The device's acks come from the ssh session task, where they belong.
     * Nothing to do on js_task. */
}

#else /* !ESP_PLATFORM */

/* ------------------------------------------------------------------ */
/* host / PC: a stub producer with a deferred ack                      */
/* ------------------------------------------------------------------ */

static void pipe_detach(void *user, term_id_t id)
{
    term_pipe_t *p = (term_pipe_t *)user;
    (void)id;
    /* NOT an ack. The device's producer needs a trip through its own task
     * before it can honestly say it has stopped touching the term, and a
     * host stub that shortcut that would make every re-pipe succeed on the
     * first try — hiding the one window §5's rule is about. */
    p->detach_req = true;
}

term_err_t term_pipe_bind(term_id_t id, const char *owner, int handle)
{
    term_producer_t prod;
    term_pipe_t *p = pipe_alloc(id, handle);
    term_err_t e;

    if (!p)
        return TERM_ERR_NO_SLOT;
    prod.detach = pipe_detach;
    prod.reply = NULL; /* no channel to answer into; replies fall through
                        * to term.onReply, and are counted if nobody is
                        * listening (term_registry.h routing rules) */
    prod.user = p;
    e = term_registry_pipe(id, owner, &prod);
    if (e != TERM_OK)
        p->used = false;
    return e;
}

void term_pipe_pump(void)
{
    int i;
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        term_pipe_t *p = &s_pipes[i];
        if (p->used && p->detach_req) {
            p->detach_req = false;
            pipe_ack_and_free(p);
        }
    }
}

#endif /* ESP_PLATFORM */

term_err_t term_pipe_unbind(term_id_t id, const char *owner)
{
    return term_registry_producer_unbind(id, owner);
}
