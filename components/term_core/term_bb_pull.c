/*
 * term_bb_pull — the signed black-box pull. See term_bb_pull.h for the wire
 * format, the five properties and the reasoning; this file is the mechanism.
 *
 * Pure C99: no ESP-IDF, no FreeRTOS, no TweetNaCl, no NVS, no MQTT, no
 * allocation, no printf. Everything device-shaped arrives through the four
 * callbacks of term_bb_env_t, which is what makes the whole responder —
 * including the replay gate and the chunk loop — runnable on the host.
 *
 * Numbers are formatted by hand rather than with snprintf. Two reasons: the
 * ROM's nano formatting is a moving target for 64-bit conversions on this
 * chip, and term_lp_ring.c already builds its JSON this way, so the two
 * serialisers behave identically at a buffer edge.
 */
#include "term_bb_pull.h"

#include <string.h>

#include "term_lp_ring.h"

/* ===================================================================== */
/* Bounded output buffer (same shape as term_lp_ring.c's)                 */
/* ===================================================================== */

typedef struct {
    char  *p;
    size_t cap;   /* usable bytes, excluding the NUL                       */
    size_t n;
    bool   ovf;
} bb_buf_t;

static void bb_putc(bb_buf_t *b, char c)
{
    if (b->n + 1u > b->cap) {
        b->ovf = true;
        return;
    }
    b->p[b->n++] = c;
}

static void bb_puts(bb_buf_t *b, const char *s)
{
    while (*s)
        bb_putc(b, *s++);
}

static void bb_putu64(bb_buf_t *b, uint64_t v)
{
    char tmp[20];
    int i = 0;
    if (!v) {
        bb_putc(b, '0');
        return;
    }
    while (v && i < (int)sizeof tmp) {
        tmp[i++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    while (i > 0)
        bb_putc(b, tmp[--i]);
}

/* ===================================================================== */
/* Names                                                                 */
/* ===================================================================== */

const char *term_bb_err_str(term_bb_err_t e)
{
    switch (e) {
    case TERM_BB_OK:        return "ok";
    case TERM_BB_E_SIZE:    return "bad-length";
    case TERM_BB_E_SIG:     return "bad-signature";
    case TERM_BB_E_MAGIC:   return "bad-magic";
    case TERM_BB_E_SYNTAX:  return "bad-request";
    case TERM_BB_E_UNKNOWN: return "unknown-field";
    case TERM_BB_E_DUP:     return "duplicate-field";
    case TERM_BB_E_MISSING: return "missing-field";
    case TERM_BB_E_RANGE:   return "out-of-range";
    case TERM_BB_E_TOPIC:   return "wrong-topic";
    case TERM_BB_E_REPLAY:  return "replay";
    case TERM_BB_E_HWM:     return "counter-store";
    case TERM_BB_E_ENV:     return "misconfigured";
    case TERM_BB_E_PUBLISH: return "publish-failed";
    }
    return "bad-code";
}

static const char *bb_what_str(term_bb_what_t w)
{
    switch (w) {
    case TERM_BB_WHAT_LASTBOOT: return "lastboot";
    case TERM_BB_WHAT_LIVE:     return "live";
    case TERM_BB_WHAT_STATS:    break;
    }
    return "stats";
}

/* ===================================================================== */
/* Request parsing                                                       */
/* ===================================================================== */

/* Decimal, no sign, no space, no leading '+'. Leading zeros are accepted
 * (they carry no ambiguity); anything else is a syntax error, and a value
 * that would exceed `limit` is out of range rather than silently wrapped. */
static bool bb_num(const char *v, size_t len, uint64_t limit, uint64_t *out,
                   term_bb_err_t *err)
{
    uint64_t x = 0;
    if (len == 0 || len > 20u) {
        *err = len ? TERM_BB_E_RANGE : TERM_BB_E_SYNTAX;
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned d;
        if (v[i] < '0' || v[i] > '9') {
            *err = TERM_BB_E_SYNTAX;
            return false;
        }
        d = (unsigned)(v[i] - '0');
        if (x > (limit - d) / 10u) {
            *err = TERM_BB_E_RANGE;
            return false;
        }
        x = x * 10u + d;
    }
    *out = x;
    return true;
}

static bool bb_eq(const char *v, size_t len, const char *lit)
{
    size_t l = strlen(lit);
    return len == l && memcmp(v, lit, l) == 0;
}

/* The parse proper. term_bb_req_parse() wraps it so that EVERY failure path
 * leaves `out` zeroed — the header promises that, and a half-filled request
 * struct is exactly the kind of thing a caller acts on by mistake (the
 * harness caught this: an unknown field left the counter and the topic
 * behind). One wrapper beats a memset before each of a dozen returns. */
static term_bb_err_t bb_parse(const char *msg, size_t len, term_bb_req_t *out)
{
    const size_t maglen = sizeof(TERM_BB_MAGIC) - 1u;
    size_t i;
    bool have_ctr = false, have_top = false, have_what = false;
    bool have_from = false;

    memset(out, 0, sizeof *out);
    if (!msg)
        return TERM_BB_E_SYNTAX;
    if (len > TERM_BB_REQ_MAX)
        return TERM_BB_E_SIZE;
    /* A NUL means the sender's idea of the message and ours differ about
     * where it ends; never guess (§ Q4). */
    if (memchr(msg, '\0', len))
        return TERM_BB_E_SYNTAX;

    /* --- line 1: the magic, exactly --- */
    {
        size_t l = 0;
        while (l < len && msg[l] != '\n')
            l++;
        i = (l < len) ? l + 1u : l;
        if (l && msg[l - 1u] == '\r')
            l--;
        if (l != maglen || memcmp(msg, TERM_BB_MAGIC, maglen) != 0)
            return TERM_BB_E_MAGIC;
    }

    /* --- key=value lines --- */
    while (i < len) {
        const char *line = msg + i;
        size_t l = 0, klen;
        const char *val;
        size_t vlen;
        uint64_t num;
        term_bb_err_t err = TERM_BB_E_SYNTAX;

        while (i + l < len && line[l] != '\n')
            l++;
        i += l + ((i + l < len) ? 1u : 0u);
        if (l && line[l - 1u] == '\r')
            l--;
        if (l == 0)
            continue;                 /* blank line: no intent, no content */

        {
            const char *eq = memchr(line, '=', l);
            if (!eq || eq == line)
                return TERM_BB_E_SYNTAX;
            klen = (size_t)(eq - line);
            val = eq + 1;
            vlen = l - klen - 1u;
        }
        if (vlen == 0)
            return TERM_BB_E_SYNTAX;

        if (klen == 3 && !memcmp(line, "ctr", 3)) {
            if (have_ctr)
                return TERM_BB_E_DUP;
            /* 1 .. 2^63-1: the top bit is left unused so that a reader or a
             * store that treats the counter as signed cannot misread it. */
            if (!bb_num(val, vlen, 0x7FFFFFFFFFFFFFFFull, &num, &err))
                return err;
            if (num == 0)
                return TERM_BB_E_RANGE;
            out->ctr = num;
            have_ctr = true;
        } else if (klen == 3 && !memcmp(line, "top", 3)) {
            if (have_top)
                return TERM_BB_E_DUP;
            if (vlen >= sizeof out->topic)
                return TERM_BB_E_RANGE;
            memcpy(out->topic, val, vlen);
            out->topic[vlen] = '\0';
            have_top = true;
        } else if (klen == 4 && !memcmp(line, "what", 4)) {
            if (have_what)
                return TERM_BB_E_DUP;
            if (bb_eq(val, vlen, "stats"))
                out->what = TERM_BB_WHAT_STATS;
            else if (bb_eq(val, vlen, "lastboot"))
                out->what = TERM_BB_WHAT_LASTBOOT;
            else if (bb_eq(val, vlen, "live"))
                out->what = TERM_BB_WHAT_LIVE;
            else
                return TERM_BB_E_SYNTAX;
            have_what = true;
        } else if (klen == 4 && !memcmp(line, "from", 4)) {
            if (have_from)
                return TERM_BB_E_DUP;
            if (!bb_num(val, vlen, 0xFFFFFFFFull, &num, &err))
                return err;
            out->from = (uint32_t)num;
            have_from = true;
        } else {
            return TERM_BB_E_UNKNOWN;
        }
    }

    if (!have_ctr || !have_top || !have_what)
        return TERM_BB_E_MISSING;
    return TERM_BB_OK;
}

term_bb_err_t term_bb_req_parse(const char *msg, size_t len,
                                term_bb_req_t *out)
{
    term_bb_err_t err;
    if (!out)
        return TERM_BB_E_ENV;
    err = bb_parse(msg, len, out);
    if (err != TERM_BB_OK)
        memset(out, 0, sizeof *out);
    return err;
}

term_bb_err_t term_bb_req_gate(const term_bb_req_t *req, const char *my_topic,
                               uint64_t hwm)
{
    if (!req || !my_topic)
        return TERM_BB_E_ENV;
    if (strcmp(req->topic, my_topic) != 0)
        return TERM_BB_E_TOPIC;
    if (req->ctr <= hwm)
        return TERM_BB_E_REPLAY;
    return TERM_BB_OK;
}

/* ===================================================================== */
/* The reply                                                             */
/* ===================================================================== */

/* Longest tail this file appends after the body:
 * ,"stall":1,"cut":1,"last":0}  = 28, plus the NUL. */
#define BB_TAIL_RESERVE 32u
/* term_lp_dump_json's own floor. */
#define BB_BODY_MIN     96u

static term_bb_err_t bb_reply(const term_bb_env_t *env,
                              const term_bb_req_t *req,
                              term_bb_result_t *res)
{
    uint32_t from = req->from;
    uint32_t seq = 0;

    for (;;) {
        bb_buf_t b;
        term_lp_dump_info_t info;
        size_t blen, bodycap;
        bool last, stall = false, cut = false;

        b.p = env->scratch;
        b.cap = env->scratch_size - 1u;    /* keep room for the NUL */
        b.n = 0;
        b.ovf = false;

        bb_puts(&b, "{\"bb\":1,\"ctr\":");
        bb_putu64(&b, req->ctr);
        bb_puts(&b, ",\"what\":\"");
        bb_puts(&b, bb_what_str(req->what));
        bb_puts(&b, "\",\"seq\":");
        bb_putu64(&b, seq);
        bb_puts(&b, ",\"from\":");
        bb_putu64(&b, from);
        bb_puts(&b, ",\"body\":");
        if (b.ovf || b.cap - b.n < BB_TAIL_RESERVE + BB_BODY_MIN)
            return TERM_BB_E_ENV;         /* scratch too small: not a request
                                             error, a misconfiguration */
        bodycap = b.cap - b.n - BB_TAIL_RESERVE;

        memset(&info, 0, sizeof info);
        if (req->what == TERM_BB_WHAT_STATS) {
            blen = term_lp_report(b.p + b.n, bodycap);
            last = true;
        } else {
            term_lp_src_t src = (req->what == TERM_BB_WHAT_LASTBOOT)
                                    ? TERM_LP_SRC_LASTBOOT
                                    : TERM_LP_SRC_LIVE;
            blen = term_lp_dump_json_ex(src, from, b.p + b.n, bodycap, &info);
            /* A buffer that cannot hold one line yields records == 0 with
             * more == 1: the pull cannot advance, so say so and stop rather
             * than publishing the same chunk forever. */
            stall = info.more && info.records == 0;
            cut = info.more && !stall && (seq + 1u >= TERM_BB_CHUNKS_MAX);
            last = !info.more || stall || cut;
        }
        if (!blen)
            return TERM_BB_E_ENV;
        b.n += blen;

        if (stall)
            bb_puts(&b, ",\"stall\":1");
        if (cut)
            bb_puts(&b, ",\"cut\":1");
        bb_puts(&b, ",\"last\":");
        bb_putc(&b, last ? '1' : '0');
        bb_putc(&b, '}');
        if (b.ovf)
            return TERM_BB_E_ENV;         /* BB_TAIL_RESERVE is wrong if so */
        b.p[b.n] = '\0';

        if (!env->publish(env->ctx, b.p, b.n))
            return TERM_BB_E_PUBLISH;
        if (res) {
            res->chunks = seq + 1u;
            res->records += info.records;
        }
        if (last)
            return TERM_BB_OK;
        seq++;
        from = info.next;
    }
}

term_bb_err_t term_bb_serve(const term_bb_env_t *env, const void *payload,
                            size_t len, term_bb_result_t *res)
{
    term_bb_req_t req;
    term_bb_err_t err;
    unsigned long long mlen = 0;
    uint64_t hwm = 0;

    if (res)
        memset(res, 0, sizeof *res);
    if (!env || !env->verify || !env->hwm_load || !env->hwm_store ||
        !env->publish || !env->topic || !env->scratch ||
        env->scratch_size < TERM_BB_SCRATCH_MIN)
        return TERM_BB_E_ENV;
    if (!payload || len <= TERM_BB_SIG_LEN || len > TERM_BB_ENVELOPE_MAX)
        return TERM_BB_E_SIZE;

    /* The envelope is opened into `scratch`, which crypto_sign_open needs to
     * be `len` bytes wide; TERM_BB_SCRATCH_MIN guarantees that. The message
     * is copied out of it (into `req`) before the buffer is reused for the
     * reply, so one buffer serves both and nothing is allocated. */
    if (env->verify(env->ctx, (unsigned char *)env->scratch, &mlen,
                    (const unsigned char *)payload,
                    (unsigned long long)len) != 0)
        return TERM_BB_E_SIG;
    if (mlen > (unsigned long long)TERM_BB_REQ_MAX)
        return TERM_BB_E_SIZE;

    err = term_bb_req_parse(env->scratch, (size_t)mlen, &req);
    if (err != TERM_BB_OK)
        return err;
    if (res) {
        res->parsed = true;
        res->req = req;
    }

    if (!env->hwm_load(env->ctx, &hwm))
        return TERM_BB_E_HWM;             /* fail closed */
    if (res)
        res->hwm = hwm;
    err = term_bb_req_gate(&req, env->topic, hwm);
    if (err != TERM_BB_OK)
        return err;

    /* Burn the counter BEFORE answering: a reset in the middle of a reply
     * must not leave this request replayable (term_bb_pull.h Q2). */
    if (!env->hwm_store(env->ctx, req.ctr))
        return TERM_BB_E_HWM;
    if (res) {
        res->accepted = true;
        res->hwm = req.ctr;
    }

    return bb_reply(env, &req, res);
}
