/*
 * pipe_util.h — shared scaffolding for the phase-4 suites (pipe, replies,
 * caret).
 *
 * Adds three fakes on top of reg_util.h's registry boilerplate, one per new
 * seam term_registry.h opened in this phase:
 *
 *   p4_prod_t   a producer (term_producer_t): counts detach requests, keeps
 *               every reply its channel was handed, and records the table
 *               lock depth it was called at — term_registry.h fixes both
 *               (detach "with the table lock NOT held", reply "FROM INSIDE
 *               term_registry_ui_drain, WITH THE TABLE LOCK HELD").
 *   p4_js_t     a term.onReply sink (term_reply_sink_fn), same recording.
 *   p4_caret_t  the registry-wide caret sink (§10.2), which additionally
 *               calls back INTO the registry, because the header promises
 *               "the sink runs UNLOCKED and may call back into the
 *               registry" and a fake that never tries would not notice.
 *
 * Everything is `static inline` in a header, like fake_port.h: run_tests.sh
 * compiles one test_*.c against the component sources and nothing else, so a
 * pipe_util.c would never be built. The p4_ prefix keeps the fakes clear of
 * every real symbol (term_*, ui_*, sshc_*, fp_*, reg_*).
 *
 * Written against term_registry.h, term_pipe.h, term_core.h and
 * docs/term-design.md (§5, §6, §10.2) only.
 */
#ifndef TERM_PIPE_UTIL_H
#define TERM_PIPE_UTIL_H

#include "reg_util.h"
#include "term_pipe.h"

/* ===================================================================== */
/* Recorded byte strings                                                 */
/* ===================================================================== */

#define P4_MAX_MSGS 32
/* Deliberately wider than TERM_REPLY_MAX: a sink that is handed more than
 * the header's cap must be able to record the fact rather than overflow. */
#define P4_MSG_CAP  (TERM_REPLY_MAX * 4)

typedef struct {
    int       n;                          /* messages seen                */
    char      msg[P4_MAX_MSGS][P4_MSG_CAP];
    size_t    len[P4_MAX_MSGS];
    term_id_t id[P4_MAX_MSGS];
    size_t    max_len;                    /* longest len ever offered     */
    int       over_cap;                   /* len > TERM_REPLY_MAX seen     */
    int       lock_depth_seen;            /* fp.depth in the last call     */
    int       not_nul_terminated;         /* bytes[len] readable? no claim */
} p4_msgs_t;

static inline void p4_msgs_reset(p4_msgs_t *m) { memset(m, 0, sizeof *m); }

static inline void p4_msgs_add(p4_msgs_t *m, term_id_t id,
                               const char *bytes, size_t len)
{
    size_t k = len;
    m->lock_depth_seen = fp.depth;
    if (len > m->max_len) m->max_len = len;
    if (len > (size_t)TERM_REPLY_MAX) m->over_cap++;
    if (m->n < P4_MAX_MSGS) {
        if (k > P4_MSG_CAP - 1) k = P4_MSG_CAP - 1;
        if (bytes && k) memcpy(m->msg[m->n], bytes, k);
        m->msg[m->n][k] = 0;
        m->len[m->n] = len;
        m->id[m->n] = id;
    }
    m->n++;
}

/* The i'th recorded message, or an empty FULL-WIDTH buffer — so a CHK_STR
 * reports a readable mismatch and a memcmp of a few bytes against a message
 * that never arrived reads inside an object instead of off the end of a
 * string literal. */
static inline const char *p4_msg(const p4_msgs_t *m, int i)
{
    static char none[P4_MSG_CAP];
    if (i < 0 || i >= m->n || i >= P4_MAX_MSGS) {
        memset(none, 0, sizeof none);
        return none;
    }
    return m->msg[i];
}

/* Concatenation of everything recorded, for "the stream arrived intact"
 * assertions that do not care where the call boundaries fell. */
static inline const char *p4_msg_all(const p4_msgs_t *m)
{
    static char out[P4_MAX_MSGS * P4_MSG_CAP];
    int i;
    size_t o = 0;
    out[0] = 0;
    for (i = 0; i < m->n && i < P4_MAX_MSGS; i++) {
        size_t l = strlen(m->msg[i]);
        if (o + l + 1 >= sizeof out) break;
        memcpy(out + o, m->msg[i], l);
        o += l;
        out[o] = 0;
    }
    return out;
}

/* ===================================================================== */
/* A fake producer (term_producer_t)                                     */
/* ===================================================================== */

typedef struct {
    const char *tag;              /* which producer, for failure output   */
    int         detaches;
    term_id_t   last_detach_id;
    int         detach_lock_depth;
    int         detach_reentrancy_rc;  /* what it saw calling back in     */
    bool        ack_on_detach;         /* ack inside detach (not the ssh
                                        * shape; used to show that the
                                        * window is what makes re-pipe
                                        * observable)                     */
    p4_msgs_t   replies;
} p4_prod_t;

/* Where a detach for a producer bound with a NULL cookie is recorded — the
 * header allows `user` to be NULL, so the fake must survive it. */
static p4_prod_t p4_nobody;

static inline void p4_prod_reset(p4_prod_t *p, const char *tag)
{
    memset(p, 0, sizeof *p);
    p->tag = tag;
}

static inline void p4_detach(void *user, term_id_t id)
{
    p4_prod_t *p = user ? (p4_prod_t *)user : &p4_nobody;
    p->detaches++;
    p->last_detach_id = id;
    p->detach_lock_depth = fp.depth;
    if (p->ack_on_detach) (void)term_registry_producer_ack(id);
}

static inline void p4_prod_reply(void *user, term_id_t id,
                                 const char *bytes, size_t len)
{
    p4_prod_t *p = user ? (p4_prod_t *)user : &p4_nobody;
    p4_msgs_add(&p->replies, id, bytes, len);
}

/* {detach, reply, cookie} — the cookie is the producer struct itself, which
 * is also its identity on term_registry_producer_space/_write. */
static inline term_producer_t p4_producer(p4_prod_t *p, bool with_reply)
{
    term_producer_t t;
    memset(&t, 0, sizeof t);
    t.detach = p4_detach;
    t.reply  = with_reply ? p4_prod_reply : NULL;
    t.user   = p;
    return t;
}

/* ===================================================================== */
/* A fake term.onReply sink (term_reply_sink_fn)                         */
/* ===================================================================== */

typedef struct {
    p4_msgs_t got;
} p4_js_t;

/* Fallback for a sink installed with a NULL cookie, same reason as
 * p4_nobody above. */
static p4_js_t p4_js_nobody;

static inline void p4_js_reset(p4_js_t *j) { memset(j, 0, sizeof *j); }

static inline void p4_js_sink(void *user, term_id_t id,
                              const char *bytes, size_t len)
{
    p4_js_t *j = user ? (p4_js_t *)user : &p4_js_nobody;
    p4_msgs_add(&j->got, id, bytes, len);
}

/* ===================================================================== */
/* The caret sink (§10.2)                                                */
/* ===================================================================== */

#define P4_CARET_MAX 64

typedef struct {
    int             n;
    term_caret_ev_t ev[P4_CARET_MAX];
    int             lock_depth_seen;   /* must be 0: the flush is unlocked */
    int             callback_rc;       /* term_registry_info() from inside */
    int             callback_tries;
    bool            call_back_in;      /* do the re-entrant call at all    */
} p4_caret_t;

static inline void p4_caret_reset(p4_caret_t *c)
{
    memset(c, 0, sizeof *c);
    c->callback_rc = TERM_OK;
}

static inline void p4_caret_sink(void *user, const term_caret_ev_t *ev)
{
    p4_caret_t *c = (p4_caret_t *)user;
    if (!c || !ev) return;
    if (fp.depth > c->lock_depth_seen) c->lock_depth_seen = fp.depth;
    if (c->n < P4_CARET_MAX) c->ev[c->n] = *ev;
    c->n++;
    if (c->call_back_in) {
        term_slot_info_t info;
        memset(&info, 0, sizeof info);
        c->callback_tries++;
        c->callback_rc = (int)term_registry_info(ev->id, &info);
    }
}

/* The last event, or a poison record so a mismatch prints readably. */
static inline term_caret_ev_t p4_caret_last(const p4_caret_t *c)
{
    term_caret_ev_t z;
    if (c->n > 0 && c->n <= P4_CARET_MAX) return c->ev[c->n - 1];
    memset(&z, 0, sizeof z);
    z.x = z.y = z.h = -12345;
    return z;
}

static inline void p4_caret_install(p4_caret_t *c)
{
    p4_caret_reset(c);
    term_registry_set_caret_sink(p4_caret_sink, c);
}

/* ===================================================================== */
/* Registry configs the phase-4 cases need                               */
/* ===================================================================== */

/* Cell metrics deliberately NOT 9x24: the config field must be what the
 * caret geometry is computed from (term_registry.h: "0 selects
 * TERM_CELL_W_DEFAULT / TERM_CELL_H_DEFAULT"). */
#define P4_CELL_W 7
#define P4_CELL_H 17

static inline term_registry_config_t p4_cfg_cells(int cw, int ch)
{
    term_registry_config_t c;
    memset(&c, 0, sizeof c);
    c.cell_w = cw;
    c.cell_h = ch;
    return c;
}

static inline term_registry_config_t p4_cfg_quiesce(uint32_t ms)
{
    term_registry_config_t c;
    memset(&c, 0, sizeof c);
    c.quiesce_timeout_ms = ms;
    return c;
}

/* ===================================================================== */
/* Views and pipes                                                       */
/* ===================================================================== */

static inline term_view_t p4_view(int x, int y, int w, int h, bool visible)
{
    term_view_t v;
    memset(&v, 0, sizeof v);
    v.x = (int16_t)x; v.y = (int16_t)y;
    v.w = (int16_t)w; v.h = (int16_t)h;
    v.visible = visible;
    return v;
}

static inline term_err_t p4_show(term_id_t id, const char *owner,
                                 int x, int y, bool visible)
{
    term_view_t v = p4_view(x, y, 400, 300, visible);
    return term_registry_show(id, owner, &v);
}

/* Bind `p` as the term's producer through term_registry_pipe (the term.pipe
 * entry point), with or without a reply route. */
static inline term_err_t p4_pipe(term_id_t id, const char *owner,
                                 p4_prod_t *p, bool with_reply)
{
    term_producer_t prod = p4_producer(p, with_reply);
    return term_registry_pipe(id, owner, &prod);
}

/* Space the ring offers this producer right now, or -1 on any error. */
static inline long p4_space(term_id_t id, p4_prod_t *p)
{
    size_t sp = 0x5A5A;
    if (term_registry_producer_space(id, p, &sp) != TERM_OK) return -1;
    return (long)sp;
}

/* Write through the producer's side of the ring; returns bytes taken and
 * reports the status through *rc. */
static inline size_t p4_write(term_id_t id, p4_prod_t *p, const void *bytes,
                              size_t len, term_err_t *rc)
{
    size_t wrote = 0x5A5A;
    term_err_t r = term_registry_producer_write(id, p, (const uint8_t *)bytes,
                                                len, &wrote);
    if (rc) *rc = r;
    if (r != TERM_OK && wrote == 0x5A5A) return 0;  /* never told us */
    return wrote;
}

/* Drain until the ring is empty again (or `cap` frames have gone by), so a
 * payload larger than one frame's byte budget lands whole. Returns the
 * number of frames it took. */
static inline int p4_drain_frames(int cap)
{
    int i;
    for (i = 0; i < cap; i++) reg_frame();
    return i;
}

#endif /* TERM_PIPE_UTIL_H */
