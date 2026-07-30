/*
 * bb_util.h — shared scaffolding for the phase-3 §3 black-box pull suites.
 *
 * term_bb_pull.h's whole testing strategy is stated in its own file header:
 * "term_bb_serve() is the complete responder, including the replay gate and
 * the chunk loop, and a host test drives it with four function pointers." This
 * header is those four function pointers, plus what is needed to hold the
 * responder to its contract rather than merely to run it:
 *
 *   - a COUNTING, FAILABLE environment. Every refusal in the enum has to be
 *     shown to publish nothing and to leave the high-water mark where it was,
 *     so the fake keeps a mark cell, a store log and a full copy of every
 *     published chunk;
 *   - a verify callback that records EXACTLY what it was handed (Q1: "nothing
 *     outside the signature is trusted" is only true if the parse consumes the
 *     verified message, so the fake can be told to emit a message that is NOT
 *     the payload's tail and the reply must follow the emitted one);
 *   - a reply reader that checks the frame the way tools/bb_pull.py must:
 *     `seq` dense from 0, exactly one `last:1` and it last, the per-chunk
 *     `records` summing to the whole;
 *   - scratch with a red zone, because "caller-owned" plus "performs no
 *     allocation" means the responder must stay inside the buffer it was given.
 *
 * Written against term_bb_pull.h, term_lp_ring.h, PHASE3_MANIFEST.md §3 and
 * docs/term-design.md §7 only.
 */
#ifndef TERM_BB_UTIL_H
#define TERM_BB_UTIL_H

#include "lp_util.h"
#include "term_bb_pull.h"

/* The shipped topic shape: 27 bytes, a random suffix (Q3). */
#define BB_TOPIC "esp32p4-mqjs/task/u7q3x9f2"

/* TERM_BB_CHUNKS_MAX is the responder's bound; the bag is bigger so a
 * violation of the bound is visible instead of silently clipped. */
#define BB_MAX_CHUNKS (TERM_BB_CHUNKS_MAX + 8u)
#define BB_CHUNK_CAP  (TERM_BB_SCRATCH_WANT + 64u)

#define BB_SCRATCH_GUARD      32u
#define BB_SCRATCH_GUARD_BYTE 0x6Du

/* ===================================================================== */
/* The environment fake                                                  */
/* ===================================================================== */

typedef struct {
    /* -- verify ---------------------------------------------------------- */
    int      verify_calls;
    bool     verify_fail;      /* return -1, i.e. Ed25519 refused           */
    /* What the callback saw. crypto_sign_open's contract is `sm`/`smlen` is
     * signature||message and `m` receives the message, so a test can assert
     * the responder passed the RAW PAYLOAD and a buffer of its own. */
    unsigned char seen_sm[TERM_BB_ENVELOPE_MAX + 256u];
    size_t        seen_smlen;
    int           seen_too_big;
    unsigned char *seen_m;
    /* When set, the callback ignores `sm` entirely and emits this message —
     * the only way to prove the responder parses the VERIFIED bytes. */
    const char *emit;
    size_t      emit_len;

    /* -- the replay mark ------------------------------------------------- */
    int      load_calls, store_calls;
    bool     load_fail, store_fail;
    uint64_t mark;             /* the NVS cell the device would keep         */
    uint64_t last_stored;

    /* -- publish --------------------------------------------------------- */
    int      publish_calls;    /* including the one that failed             */
    int      publish_fail_at;  /* 1-based call index to refuse, 0 = never   */
    unsigned chunks;           /* successful publishes                      */
    int      bad_len;          /* len disagreed with strlen(json)           */
    int      null_json;
    int      overflow;         /* chunks the bag could not hold             */
    size_t   max_chunk;        /* longest chunk published                   */
    char     chunk[BB_MAX_CHUNKS][BB_CHUNK_CAP];
    size_t   chunk_len[BB_MAX_CHUNKS];

    /* -- the caller-owned scratch, with red zones ------------------------ */
    char     raw[BB_SCRATCH_GUARD * 2u + TERM_BB_SCRATCH_WANT + 512u];
    char    *scratch;
    size_t   scratch_size;
} bb_state_t;

static bb_state_t bb;

static inline int bb_verify(void *ctx, unsigned char *m, unsigned long long *mlen,
                     const unsigned char *sm, unsigned long long smlen)
{
    (void)ctx;
    bb.verify_calls++;
    bb.seen_m = m;
    bb.seen_smlen = (size_t)smlen;
    if (smlen <= (unsigned long long)sizeof bb.seen_sm) {
        if (sm) memcpy(bb.seen_sm, sm, (size_t)smlen);
    } else {
        bb.seen_too_big++;
    }
    if (bb.verify_fail) return -1;
    if (bb.emit) {
        memcpy(m, bb.emit, bb.emit_len);
        *mlen = bb.emit_len;
        return 0;
    }
    /* The honest shape of a verified envelope: strip the signature. */
    if (smlen < TERM_BB_SIG_LEN) return -1;
    *mlen = smlen - TERM_BB_SIG_LEN;
    if (*mlen) memcpy(m, sm + TERM_BB_SIG_LEN, (size_t)*mlen);
    return 0;
}

static inline bool bb_hwm_load(void *ctx, uint64_t *out)
{
    (void)ctx;
    bb.load_calls++;
    if (bb.load_fail) return false;
    *out = bb.mark;
    return true;
}

static inline bool bb_hwm_store(void *ctx, uint64_t v)
{
    (void)ctx;
    bb.store_calls++;
    bb.last_stored = v;
    if (bb.store_fail) return false;
    bb.mark = v;
    return true;
}

static inline bool bb_publish(void *ctx, const char *json, size_t len)
{
    (void)ctx;
    bb.publish_calls++;
    if (bb.publish_fail_at && bb.publish_calls == bb.publish_fail_at) return false;
    if (!json) { bb.null_json++; return false; }
    if (strlen(json) != len) bb.bad_len++;
    if (len > bb.max_chunk) bb.max_chunk = len;
    if (bb.chunks < BB_MAX_CHUNKS && len < BB_CHUNK_CAP) {
        memcpy(bb.chunk[bb.chunks], json, len);
        bb.chunk[bb.chunks][len] = 0;
        bb.chunk_len[bb.chunks] = len;
    } else {
        bb.overflow++;
    }
    bb.chunks++;
    return true;
}

/* Fresh fake, a mark of 0 (a device that has never been pulled) and the
 * scratch the device passes. */
static inline void bb_reset(void)
{
    memset(&bb, 0, sizeof bb);
    memset(bb.raw, BB_SCRATCH_GUARD_BYTE, sizeof bb.raw);
    bb.scratch = bb.raw + BB_SCRATCH_GUARD;
    bb.scratch_size = TERM_BB_SCRATCH_WANT;
}

/* Move the scratch window; the red zones follow it. */
static inline void bb_scratch(size_t n)
{
    memset(bb.raw, BB_SCRATCH_GUARD_BYTE, sizeof bb.raw);
    if (n > sizeof bb.raw - 2u * BB_SCRATCH_GUARD) n = sizeof bb.raw - 2u * BB_SCRATCH_GUARD;
    bb.scratch = bb.raw + BB_SCRATCH_GUARD;
    bb.scratch_size = n;
}

static inline int bb_scratch_intact(void)
{
    size_t i;
    for (i = 0; i < BB_SCRATCH_GUARD; i++) {
        if ((unsigned char)bb.scratch[-(ptrdiff_t)BB_SCRATCH_GUARD + (ptrdiff_t)i]
            != BB_SCRATCH_GUARD_BYTE) return 0;
        if ((unsigned char)bb.scratch[bb.scratch_size + i] != BB_SCRATCH_GUARD_BYTE)
            return 0;
    }
    return 1;
}

static inline term_bb_env_t bb_env(void)
{
    term_bb_env_t e;
    memset(&e, 0, sizeof e);
    e.verify = bb_verify;
    e.hwm_load = bb_hwm_load;
    e.hwm_store = bb_hwm_store;
    e.publish = bb_publish;
    e.ctx = &bb;
    e.topic = BB_TOPIC;
    e.scratch = bb.scratch;
    e.scratch_size = bb.scratch_size;
    return e;
}

/* ===================================================================== */
/* Envelopes                                                             */
/* ===================================================================== */

/*
 * signature(64) || message, with a signature the fake verifier does not read.
 * Suites that need a REAL signature use bb_vectors.h instead; this is for the
 * ~40 cases whose subject is the parser, the gate or the chunk loop, where
 * carrying a real signature per case would mean regenerating vectors every
 * time a field changes.
 */
static unsigned char bb_env_buf[TERM_BB_ENVELOPE_MAX + 4096u];

static inline size_t bb_wrap(const char *msg, size_t len)
{
    size_t i;
    if (TERM_BB_SIG_LEN + len > sizeof bb_env_buf) return 0;
    for (i = 0; i < TERM_BB_SIG_LEN; i++)
        bb_env_buf[i] = (unsigned char)(0xA0u + (i & 0x3Fu));
    if (len) memcpy(bb_env_buf + TERM_BB_SIG_LEN, msg, len);
    return TERM_BB_SIG_LEN + len;
}

/* Serve one message through the fake environment. */
static inline term_bb_err_t bb_serve_bytes(const char *msg, size_t len,
                                           term_bb_result_t *res)
{
    term_bb_env_t e = bb_env();
    size_t n = bb_wrap(msg, len);
    return term_bb_serve(&e, bb_env_buf, n, res);
}

static inline term_bb_err_t bb_serve(const char *msg, term_bb_result_t *res)
{
    return bb_serve_bytes(msg, strlen(msg), res);
}

/* ===================================================================== */
/* Reading the reply                                                     */
/* ===================================================================== */

/* The integer after the FIRST "<key>": in `s`, or -1 when the key is absent,
 * -2 when the value is not a number. Unsigned 64-bit so `ctr` fits. */
static inline long long bb_jint(const char *s, const char *key)
{
    char pat[64];
    const char *p;
    if (!s) return -1;
    sprintf(pat, "\"%s\":", key);
    p = strstr(s, pat);
    if (!p) return -1;
    p += strlen(pat);
    if (*p == '"' || *p == '{' || *p == '[') return -2;
    return (long long)strtoull(p, NULL, 10);
}

/* The same, but only inside the "body":{...} part. -1 when the body has no
 * such key — a `stats` body is term_lp_report()'s object and shares none of
 * the dump's cursor keys. */
static inline long long bb_jbody(const char *s, const char *key)
{
    const char *p = s ? strstr(s, "\"body\":") : NULL;
    return p ? bb_jint(p, key) : -1;
}

/*
 * The frame's own "last", which is the LAST such key in the chunk: a `stats`
 * body carries a nested "last":{...} object of its own (term_lp_report's
 * lastboot counters), so a first-match reader would answer 0 for every stats
 * reply. The frame closes with "last":<0|1>}, so the final occurrence is it.
 */
static inline long long bb_jlast(const char *s)
{
    const char *p = s, *hit = NULL;
    if (!s) return -1;
    while ((p = strstr(p, "\"last\":")) != NULL) { hit = p; p += 7; }
    if (!hit) return -1;
    hit += 7;
    if (*hit == '"' || *hit == '{' || *hit == '[') return -2;
    return (long long)strtoull(hit, NULL, 10);
}

/* The string value after "<key>":" — into a rotating buffer. */
static inline const char *bb_jstr(const char *s, const char *key)
{
    static char buf[4][128];
    static int which = 0;
    char pat[64];
    const char *p;
    size_t n = 0;
    which = (which + 1) & 3;
    buf[which][0] = 0;
    if (!s) return buf[which];
    sprintf(pat, "\"%s\":\"", key);
    p = strstr(s, pat);
    if (!p) return buf[which];
    p += strlen(pat);
    while (*p && *p != '"' && n < sizeof buf[0] - 1u) buf[which][n++] = *p++;
    buf[which][n] = 0;
    return buf[which];
}

static inline int bb_has(const char *s, const char *needle)
{
    return s && needle && strstr(s, needle) != NULL;
}

/*
 * Hold the published reply to the documented frame, the way the reader has to
 * read it (term_bb_pull.h, "WIRE FORMAT — REPLY"):
 *
 *   - every chunk is a self-contained JSON object, NUL-terminated, `len`
 *     agreeing with it, and inside the scratch;
 *   - "bb":1 and the request's `ctr` and `what` echoed on every chunk;
 *   - `seq` is 0-based and dense (a gap is a LOST chunk, so the responder
 *     itself must never leave one);
 *   - exactly one chunk carries "last":1 and it is the final one;
 *   - each chunk's `from` is the previous chunk's body.next (the cursor
 *     chain), and body.from equals the frame's from;
 *   - the per-chunk body.records sum to `res->records`.
 *
 * Returns the summed record count.
 */
static inline unsigned bb_check_reply(const term_bb_result_t *res)
{
    unsigned i, sum = 0, lasts = 0;
    long long expect_from = -1;

    t_checks++;
    if (bb.overflow || bb.bad_len || bb.null_json) {
        t_head(__FILE__, __LINE__);
        printf("     reply plumbing: overflow %d, bad len %d, null json %d\n",
               bb.overflow, bb.bad_len, bb.null_json);
    }
    CHK_INT(bb.chunks, res->chunks);
    CHK_TRUE(bb.chunks <= TERM_BB_CHUNKS_MAX);
    CHK_TRUE(bb.max_chunk < bb.scratch_size);
    CHK_TRUE(bb_scratch_intact());

    for (i = 0; i < bb.chunks && i < BB_MAX_CHUNKS; i++) {
        const char *c = bb.chunk[i];
        long long recs, from, next;
        size_t n = bb.chunk_len[i];

        CHK_INT(strlen(c), n);
        CHK_TRUE(n > 0 && c[0] == '{' && c[n - 1u] == '}');
        CHK_INT(bb_jint(c, "bb"), 1);
        CHK_TRUE((uint64_t)bb_jint(c, "ctr") == res->req.ctr);
        CHK_INT(bb_jint(c, "seq"), (long long)i);
        recs = bb_jbody(c, "records");
        from = bb_jint(c, "from");
        next = bb_jbody(c, "next");
        if (expect_from >= 0) CHK_INT(from, expect_from);
        /* A dump body echoes the frame's cursor; a `stats` body has no cursor
         * keys at all, and then there is nothing to cross-check. */
        if (bb_jbody(c, "from") >= 0) CHK_INT(bb_jbody(c, "from"), from);
        if (recs >= 0) {
            sum += (unsigned)recs;
            CHK_INT(next, from + recs);
            expect_from = next;
        }
        if (bb_jlast(c) == 1) {
            lasts++;
            CHK_INT(i, bb.chunks - 1u);
        }
        /* Both honesty flags force "last":1 (term_bb_pull.h). */
        if (bb_has(c, "\"stall\":1") || bb_has(c, "\"cut\":1"))
            CHK_INT(bb_jlast(c), 1);
    }
    CHK_INT(lasts, 1);
    CHK_INT(sum, res->records);
    return sum;
}

/* The final chunk, or "" when nothing was published. */
static inline const char *bb_last_chunk(void)
{
    if (bb.chunks == 0 || bb.chunks > BB_MAX_CHUNKS) return "";
    return bb.chunk[bb.chunks - 1u];
}

/* ===================================================================== */
/* Filling the live ring                                                 */
/* ===================================================================== */

/* `n` records of `payload` bytes into the APP partition, through the tee that
 * term_lp_ring.h says is the only writer. Returns how many were accepted. */
static inline unsigned bb_fill(unsigned n, size_t payload, const char *writer)
{
    char line[TERM_LP_REC_MAX + 8u];
    unsigned i, ok = 0;
    for (i = 0; i < n; i++) {
        size_t k = lp_mkline(line, sizeof line, 'A', i, payload);
        if (term_lp_log(TERM_LP_CLASS_APP, writer, line, k)) ok++;
    }
    return ok;
}

/* Ground truth: what the iterator says the live image holds. */
static inline unsigned bb_live_records(void)
{
    const term_lp_ring_t *live = term_lp_source(TERM_LP_SRC_LIVE);
    return live ? lp_count(live, -1) : 0u;
}

#endif /* TERM_BB_UTIL_H */
