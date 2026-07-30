/*
 * test_bb_refuse.c — what a refusal costs: nothing.
 *
 * term_bb_pull.h: "Nothing is published before step 6 succeeds", "Failing
 * closed is the rule", and Q2's mark is "stored BEFORE the first reply byte is
 * published". Together those mean every refusal owes the caller three things,
 * and this suite asserts all three for EVERY code in term_bb_err_t:
 *
 *   1. the return code names the reason (one code per reason, "so an operator
 *      debugging a failed pull should not have to guess");
 *   2. no reply chunk was published;
 *   3. the high-water mark did not move — a refused request must not consume
 *      counter space, or a wrong-topic broadcast would be a denial of service
 *      against every device that heard it.
 *
 * TERM_BB_E_PUBLISH is the one exception the header states, and it is checked
 * as such: the mark IS burned, deliberately, so "a crash mid-reply therefore
 * burns the counter rather than leaving it reusable".
 *
 * The suite ends with a mutation sweep over a valid request: whatever comes
 * out, the invariant is the same — a non-OK answer publishes nothing and moves
 * nothing, an OK answer moved the mark to exactly the request's counter, and
 * the responder never leaves its scratch.
 */
#include "bb_util.h"
#include "fake_port.h"

/* ===================================================================== */
/* One row per refusal class                                             */
/* ===================================================================== */

typedef enum {
    F_MSG = 0,      /* an envelope built from `msg`                       */
    F_RAW,          /* a raw payload of `raw_len` bytes                   */
    F_ENV,          /* a deliberately broken environment                  */
    F_SIGFAIL,      /* the verifier refuses                               */
    F_STOREFAIL,    /* hwm_store() refuses                                */
    F_LOADFAIL,     /* hwm_load() refuses                                 */
    F_PUBFAIL       /* the first publish refuses                          */
} how_t;

typedef struct {
    const char   *what;
    how_t         how;
    const char   *msg;
    size_t        raw_len;
    uint64_t      mark;
    term_bb_err_t want;
} refusal_t;

#define REQ_OK   "bbpull1\nctr=5000\ntop=" BB_TOPIC "\nwhat=live\n"

static const refusal_t rows[] = {
    /* -- step 1: the environment ------------------------------------- */
    { "a NULL environment",            F_ENV, NULL, 0, 0, TERM_BB_E_ENV },

    /* -- step 2: length bounds -------------------------------------- */
    { "an empty payload",              F_RAW, NULL, 0, 0, TERM_BB_E_SIZE },
    { "shorter than a signature",      F_RAW, NULL, TERM_BB_SIG_LEN - 1u, 0,
                                                          TERM_BB_E_SIZE },
    { "a signature and no message",    F_RAW, NULL, TERM_BB_SIG_LEN, 0,
                                                          TERM_BB_E_SIZE },
    { "an envelope one byte too long", F_RAW, NULL, TERM_BB_ENVELOPE_MAX + 1u, 0,
                                                          TERM_BB_E_SIZE },

    /* -- step 3: the signature -------------------------------------- */
    { "a signature that does not verify", F_SIGFAIL, REQ_OK, 0, 0,
                                                          TERM_BB_E_SIG },

    /* -- step 4: the message ---------------------------------------- */
    { "a wrong magic line",            F_MSG,
      "bbpull2\nctr=5000\ntop=" BB_TOPIC "\nwhat=live\n", 0, 0,
                                                          TERM_BB_E_MAGIC },
    { "a line that is not key=value",  F_MSG,
      "bbpull1\nctr=5000\ntop=" BB_TOPIC "\nwhat=live\nnonsense\n", 0, 0,
                                                          TERM_BB_E_SYNTAX },
    { "an unknown field (Q4)",         F_MSG,
      "bbpull1\nctr=5000\ntop=" BB_TOPIC "\nwhat=live\nredact=1\n", 0, 0,
                                                          TERM_BB_E_UNKNOWN },
    { "a duplicate field",             F_MSG,
      "bbpull1\nctr=5000\nctr=5001\ntop=" BB_TOPIC "\nwhat=live\n", 0, 0,
                                                          TERM_BB_E_DUP },
    { "a missing required field",      F_MSG,
      "bbpull1\nctr=5000\ntop=" BB_TOPIC "\n", 0, 0,      TERM_BB_E_MISSING },
    { "ctr=0",                         F_MSG,
      "bbpull1\nctr=0\ntop=" BB_TOPIC "\nwhat=live\n", 0, 0,
                                                          TERM_BB_E_RANGE },
    { "a counter that does not fit",   F_MSG,
      "bbpull1\nctr=99999999999999999999\ntop=" BB_TOPIC "\nwhat=live\n", 0, 0,
                                                          TERM_BB_E_RANGE },

    /* -- step 5: the gate ------------------------------------------- */
    { "another device's topic (Q3)",   F_MSG,
      "bbpull1\nctr=5000\ntop=someone/else\nwhat=live\n", 0, 0,
                                                          TERM_BB_E_TOPIC },
    { "a counter equal to the mark",   F_MSG, REQ_OK, 0, 5000,
                                                          TERM_BB_E_REPLAY },
    { "a counter below the mark",      F_MSG, REQ_OK, 0, 9999,
                                                          TERM_BB_E_REPLAY },

    /* -- step 6: the mark ------------------------------------------- */
    { "a mark that cannot be stored",  F_STOREFAIL, REQ_OK, 0, 100,
                                                          TERM_BB_E_HWM },
    { "a mark that cannot be read",    F_LOADFAIL, REQ_OK, 0, 100,
                                                          TERM_BB_E_HWM },
};

static void run_row(const refusal_t *r)
{
    term_bb_result_t res;
    term_bb_env_t e;
    term_bb_err_t got;
    static unsigned char raw[TERM_BB_ENVELOPE_MAX + 64u];
    unsigned records_before = bb_live_records();

    bb_reset();
    bb.mark = r->mark;
    e = bb_env();

    switch (r->how) {
    case F_ENV:
        got = term_bb_serve(NULL, bb_env_buf, 200, &res);
        break;
    case F_RAW:
        memset(raw, 'z', sizeof raw);
        got = term_bb_serve(&e, raw, r->raw_len, &res);
        break;
    case F_SIGFAIL:
        bb.verify_fail = true;
        got = bb_serve(r->msg, &res);
        break;
    case F_STOREFAIL:
        bb.store_fail = true;
        got = bb_serve(r->msg, &res);
        break;
    case F_LOADFAIL:
        bb.load_fail = true;
        got = bb_serve(r->msg, &res);
        break;
    case F_PUBFAIL:
        bb.publish_fail_at = 1;
        got = bb_serve(r->msg, &res);
        break;
    case F_MSG:
    default:
        got = bb_serve(r->msg, &res);
        break;
    }

    t_checks++;
    if (got != r->want) {
        t_head(__FILE__, __LINE__);
        printf("     case    : %s\n     expected: %s\n     actual  : %s\n",
               r->what, term_bb_err_str(r->want), term_bb_err_str(got));
    }

    /* 2. nothing was published. */
    t_checks++;
    if (bb.chunks != 0u || bb.publish_calls != 0) {
        t_head(__FILE__, __LINE__);
        printf("     case    : %s\n     a refusal published %u chunk(s) "
               "(%d publish calls)\n", r->what, bb.chunks, bb.publish_calls);
    }
    CHK_INT(res.chunks, 0);
    CHK_INT(res.records, 0);
    CHK_TRUE(!res.accepted);

    /* 3. the mark did not move, and was not even offered a new value. */
    t_checks++;
    if (bb.mark != r->mark) {
        t_head(__FILE__, __LINE__);
        printf("     case    : %s\n     the mark moved %llu -> %llu\n", r->what,
               (unsigned long long)r->mark, (unsigned long long)bb.mark);
    }
    if (r->want != TERM_BB_E_HWM) CHK_INT(bb.store_calls, 0);

    /* And the ring is untouched: a refusal is not an append (Q5, P3). */
    CHK_INT(bb_live_records(), records_before);
    CHK_TRUE(bb_scratch_intact());
}

static void case_every_refusal_class(void)
{
    unsigned i;
    unsigned seen[64];

    memset(seen, 0, sizeof seen);
    for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        t_case(rows[i].what);
        run_row(&rows[i]);
        if ((unsigned)rows[i].want < 64u) seen[rows[i].want]++;
    }

    t_case("a scratch below the floor and the four missing callbacks");
    {
        term_bb_result_t res;
        term_bb_env_t e;
        const char *m = REQ_OK;
        size_t n = bb_wrap(m, strlen(m));
        struct { const char *what; int which; } k[] = {
            { "no verify", 0 }, { "no hwm_load", 1 },
            { "no hwm_store", 2 }, { "no publish", 3 },
            { "no topic", 4 }, { "no scratch", 5 }, { "a short scratch", 6 }
        };
        unsigned j;
        for (j = 0; j < sizeof k / sizeof k[0]; j++) {
            bb_reset();
            if (k[j].which == 6) bb_scratch(TERM_BB_SCRATCH_MIN - 1u);
            e = bb_env();
            if (k[j].which == 0) e.verify = NULL;
            if (k[j].which == 1) e.hwm_load = NULL;
            if (k[j].which == 2) e.hwm_store = NULL;
            if (k[j].which == 3) e.publish = NULL;
            if (k[j].which == 4) e.topic = NULL;
            if (k[j].which == 5) e.scratch = NULL;
            t_checks++;
            if (term_bb_serve(&e, bb_env_buf, n, &res) != TERM_BB_E_ENV) {
                t_head(__FILE__, __LINE__);
                printf("     %s was not refused as misconfigured\n", k[j].what);
            }
            CHK_INT(bb.chunks, 0);
            CHK_INT(bb.store_calls, 0);
        }
        seen[TERM_BB_E_ENV]++;
    }

    t_case("TERM_BB_E_PUBLISH is the documented exception: the mark IS burned");
    {
        term_bb_result_t res;
        bb_reset();
        bb.mark = 100;
        bb.publish_fail_at = 1;
        CHK_INT(bb_serve(REQ_OK, &res), TERM_BB_E_PUBLISH);
        CHK_INT(bb.chunks, 0);              /* still nothing landed */
        CHK_INT(bb.publish_calls, 1);
        CHK_TRUE(bb.mark == 5000u);         /* ... but it is spent */
        CHK_INT(bb.store_calls, 1);
        CHK_TRUE(res.accepted);
        seen[TERM_BB_E_PUBLISH]++;
    }

    t_case("every error code in the enum was exercised by a case above");
    {
        unsigned c;
        for (c = (unsigned)TERM_BB_E_SIZE; c <= (unsigned)TERM_BB_E_PUBLISH; c++) {
            t_checks++;
            if (!seen[c]) {
                t_head(__FILE__, __LINE__);
                printf("     no case produces %s (code %u)\n",
                       term_bb_err_str((term_bb_err_t)c), c);
            }
        }
    }
}

/* ===================================================================== */
/* A mutation sweep: the invariant holds for anything at all              */
/* ===================================================================== */

static uint32_t rnd_state = 0x1234567u;

static uint32_t rnd(void)
{
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state >> 8;
}

static void case_mutation_sweep(void)
{
    static char msg[TERM_BB_REQ_MAX * 2];
    unsigned it, accepted = 0, refused = 0;
    unsigned codes[64];

    t_case("a mutated request either serves or refuses, and never half of each");
    bb_reset();          /* no leftover failure switch from the cases above */
    memset(codes, 0, sizeof codes);
    /* One record so an accepted pull has something to say. */
    (void)term_lp_log(TERM_LP_CLASS_APP, "fuzz", "a line", 6);

    for (it = 0; it < 4000u; it++) {
        size_t n, cut;
        unsigned muts, k;
        term_bb_result_t res;
        term_bb_err_t e;
        uint64_t mark_before;
        int stores_before;

        n = (size_t)sprintf(msg, "bbpull1\nctr=%u\ntop=%s\nwhat=%s\nfrom=%u\n",
                            1u + (rnd() % 4000u), BB_TOPIC,
                            (rnd() % 3u) == 0 ? "live"
                              : ((rnd() % 2u) ? "stats" : "lastboot"),
                            rnd() % 8u);
        muts = 1u + rnd() % 4u;
        for (k = 0; k < muts; k++) {
            size_t at = rnd() % n;
            switch (rnd() % 5u) {
            case 0: msg[at] = (char)(rnd() & 0x7Fu); break;   /* ASCII noise */
            case 1: msg[at] = (char)(1u + (rnd() % 255u)); break;
            case 2: msg[at] = '\n'; break;
            case 3: msg[at] = '='; break;
            default: msg[at] = (char)('0' + (rnd() % 10u)); break;
            }
        }
        cut = (rnd() % 8u) ? n : (rnd() % (n + 1u));

        /* Reset only the counters: memsetting the chunk bag 4000 times is
         * measurement, not testing. */
        bb.chunks = 0;
        bb.publish_calls = 0;
        bb.store_calls = 0;
        bb.load_calls = 0;
        bb.verify_calls = 0;
        bb.overflow = bb.bad_len = bb.null_json = 0;
        bb.max_chunk = 0;
        bb.mark = 0;
        mark_before = bb.mark;
        stores_before = bb.store_calls;

        e = bb_serve_bytes(msg, cut, &res);
        if ((unsigned)e < 64u) codes[e]++;

        t_checks++;
        if (e == TERM_BB_OK) {
            accepted++;
            if (bb.chunks == 0u || bb.mark != res.req.ctr || !res.accepted) {
                t_head(__FILE__, __LINE__);
                printf("     an accepted pull published %u chunks and left the "
                       "mark at %llu (ctr %llu)\n", bb.chunks,
                       (unsigned long long)bb.mark,
                       (unsigned long long)res.req.ctr);
            }
        } else {
            refused++;
            if (bb.chunks != 0u || bb.mark != mark_before ||
                bb.store_calls != stores_before) {
                t_head(__FILE__, __LINE__);
                printf("     %s published %u chunks / stored %d times\n",
                       term_bb_err_str(e), bb.chunks,
                       bb.store_calls - stores_before);
            }
        }
        CHK_TRUE(bb_scratch_intact());
        CHK_INT(bb.overflow, 0);
        CHK_INT(bb.bad_len, 0);
    }

    t_case("...and the sweep really reached both outcomes");
    CHK_TRUE(accepted > 20u);
    CHK_TRUE(refused > 200u);
    /* The mutation only ever damages a well-formed request, so the codes it can
     * produce are the message-level ones; nothing here should be able to make
     * the environment or the counter fail. */
    CHK_INT(codes[TERM_BB_E_ENV], 0);
    CHK_INT(codes[TERM_BB_E_HWM], 0);
    CHK_INT(codes[TERM_BB_E_PUBLISH], 0);
    CHK_INT(codes[TERM_BB_E_SIG], 0);
}

int main(void)
{
    t_suite("bb_refuse");
    t_case("the port installs and the ring boots");
    fp_reset();
    CHK_TRUE(term_port_install(fp_port()));
    term_lp_ring_boot();
    CHK_TRUE(term_lp_ring_ready());

    case_every_refusal_class();
    case_mutation_sweep();

    fp_reclaim_all();
    return t_summary();
}
