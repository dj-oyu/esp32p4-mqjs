/*
 * test_bb_gate.c — the order of operations, and the replay counter's
 * durability ordering (Q2, Q3).
 *
 * term_bb_pull.h makes the order contract and says why: "Order of operations,
 * and it is contract because each step protects the next:
 *
 *     1. env sanity (callbacks, scratch size)          -> E_ENV
 *     2. length bounds                                 -> E_SIZE
 *     3. signature verified into `scratch`             -> E_SIG
 *     4. parse                                        -> E_MAGIC/E_SYNTAX/...
 *     5. topic bind, then the counter                  -> E_TOPIC / E_REPLAY
 *     6. hwm_store(ctr), durably                       -> E_HWM
 *     7. chunk loop: publish until `last`              -> E_PUBLISH
 *
 *  Nothing is published before step 6 succeeds, and the message is parsed only
 *  after the signature verified."
 *
 * Each step is shown to come before the next by making TWO things wrong at
 * once and asserting the EARLIER code — the only way to observe an order from
 * outside. And term_bb_req_gate is driven directly, because the header split it
 * out for exactly that: "the two checks that need device context, split out so
 * a test can drive them independently of parsing ... Order is topic first — a
 * request meant for another device must not consume this one's counter space,
 * and it is not a replay attempt."
 */
#include "bb_util.h"
#include "fake_port.h"

/* A well-formed request for this device at counter `ctr`. */
static const char *req_for(uint64_t ctr, const char *what, const char *topic)
{
    static char buf[4][TERM_BB_REQ_MAX];
    static int which = 0;
    which = (which + 1) & 3;
    sprintf(buf[which], "bbpull1\nctr=%llu\ntop=%s\nwhat=%s\n",
            (unsigned long long)ctr, topic, what);
    return buf[which];
}

static const char *req(uint64_t ctr, const char *what)
{
    return req_for(ctr, what, BB_TOPIC);
}

/* Serve, then hold the two things every refusal owes the caller: nothing was
 * published, and the mark did not move. */
static term_bb_err_t serve_expect(const char *msg, term_bb_err_t want)
{
    term_bb_result_t res;
    term_bb_err_t e;
    uint64_t before = bb.mark;
    int loads = bb.load_calls;

    memset(&res, 0xCD, sizeof res);
    e = bb_serve(msg, &res);

    t_checks++;
    if (e != want) {
        t_head(__FILE__, __LINE__);
        printf("     message : \"%s\"\n     expected: %s\n     actual  : %s\n",
               t_esc(msg), term_bb_err_str(want), term_bb_err_str(e));
    }
    /*
     * `hwm` is "the mark in force after this call". AMBIGUITY, reported rather
     * than papered over: when a request is refused BEFORE the mark is read
     * (E_ENV/E_SIZE/E_SIG and every parse error), the implementation leaves the
     * field at the 0 the struct was zeroed to, which is not the mark in force
     * on a device whose stored mark is 9999. The header's "Always written
     * (zeroed first)" makes that reading defensible, so what is pinned here is
     * the pair: once the mark HAS been read it must be reported exactly, and
     * before that it must be the documented zero and never a third value.
     */
    if (bb.load_calls > loads && !bb.load_fail) CHK_TRUE(res.hwm == bb.mark);
    else                                        CHK_TRUE(res.hwm == 0u);
    CHK_TRUE(bb_scratch_intact());
    if (e != TERM_BB_OK) {
        CHK_INT(bb.chunks, 0);
        CHK_INT(res.chunks, 0);
        CHK_INT(res.records, 0);
        CHK_TRUE(!res.accepted);
        if (e != TERM_BB_E_PUBLISH) CHK_TRUE(bb.mark == before);
    }
    return e;
}

/* ===================================================================== */
/* term_bb_req_gate, on its own                                          */
/* ===================================================================== */

static term_bb_req_t mkreq(const char *topic, uint64_t ctr)
{
    term_bb_req_t r;
    memset(&r, 0, sizeof r);
    r.ctr = ctr;
    r.what = TERM_BB_WHAT_LIVE;
    r.from = 0;
    strcpy(r.topic, topic);
    return r;
}

static term_bb_err_t gate(const char *req_topic, uint64_t ctr,
                          const char *my_topic, uint64_t hwm)
{
    term_bb_req_t r = mkreq(req_topic, ctr);
    return term_bb_req_gate(&r, my_topic, hwm);
}

static void case_gate_alone(void)
{
    term_bb_req_t r;

    t_case("strictly greater than the mark passes; equal does not (Q2)");
    CHK_INT(gate(BB_TOPIC, 1001, BB_TOPIC, 1000), TERM_BB_OK);
    CHK_INT(gate(BB_TOPIC, 1000, BB_TOPIC, 1000), TERM_BB_E_REPLAY);
    CHK_INT(gate(BB_TOPIC, 999, BB_TOPIC, 1000), TERM_BB_E_REPLAY);
    CHK_INT(gate(BB_TOPIC, 1, BB_TOPIC, 0), TERM_BB_OK);
    CHK_INT(gate(BB_TOPIC, 0xFFFFFFFFFFFFFFFFull, BB_TOPIC,
                 0xFFFFFFFFFFFFFFFEull), TERM_BB_OK);
    CHK_INT(gate(BB_TOPIC, 0xFFFFFFFFFFFFFFFEull, BB_TOPIC,
                 0xFFFFFFFFFFFFFFFFull), TERM_BB_E_REPLAY);

    t_case("a device never pulled has mark 0, so any ctr >= 1 is fresh");
    CHK_INT(gate(BB_TOPIC, 1, BB_TOPIC, 0), TERM_BB_OK);
    CHK_INT(gate(BB_TOPIC, 1753900000123ull, BB_TOPIC, 0), TERM_BB_OK);

    t_case("the topic is compared byte for byte: no prefix, no extension (Q3)");
    CHK_INT(gate(BB_TOPIC, 5, BB_TOPIC, 0), TERM_BB_OK);
    CHK_INT(gate("esp32p4-mqjs/task/u7q3x9f", 5, BB_TOPIC, 0), TERM_BB_E_TOPIC);
    CHK_INT(gate("esp32p4-mqjs/task/u7q3x9f22", 5, BB_TOPIC, 0), TERM_BB_E_TOPIC);
    CHK_INT(gate("esp32p4-mqjs/task/U7Q3X9F2", 5, BB_TOPIC, 0), TERM_BB_E_TOPIC);
    CHK_INT(gate("", 5, BB_TOPIC, 0), TERM_BB_E_TOPIC);
    CHK_INT(gate(BB_TOPIC, 5, "esp32p4-mqjs/task/other", 0), TERM_BB_E_TOPIC);

    t_case("topic outranks the counter: another device's request is not a replay");
    /* Both wrong. The header pins which one is reported, and why: a request
     * meant for another device must not consume this one's counter space. */
    CHK_INT(gate("someone/else", 1, BB_TOPIC, 9999), TERM_BB_E_TOPIC);
    CHK_INT(gate("someone/else", 9999, BB_TOPIC, 9999), TERM_BB_E_TOPIC);

    t_case("the gate does not touch the request it was handed");
    r = mkreq("someone/else", 7);
    {
        term_bb_req_t copy = r;
        (void)term_bb_req_gate(&r, BB_TOPIC, 9999);
        CHK_INT(memcmp(&copy, &r, sizeof r), 0);
        (void)term_bb_req_gate(&r, "someone/else", 0);
        CHK_INT(memcmp(&copy, &r, sizeof r), 0);
    }
}

/* ===================================================================== */
/* The order of operations, observed from outside                        */
/* ===================================================================== */

static void case_env_sanity_is_first(void)
{
    term_bb_result_t res;
    term_bb_env_t e;

    t_case("a misconfigured environment is refused before anything is read");
    bb_reset();
    CHK_INT(term_bb_serve(NULL, bb_env_buf, 200, &res), TERM_BB_E_ENV);
    CHK_INT(bb.verify_calls, 0);

    memset(&e, 0, sizeof e);
    CHK_INT(term_bb_serve(&e, bb_env_buf, 200, &res), TERM_BB_E_ENV);

    /* Each callback in turn: all four are required. */
    e = bb_env(); e.verify = NULL;
    CHK_INT(term_bb_serve(&e, bb_env_buf, 200, &res), TERM_BB_E_ENV);
    e = bb_env(); e.hwm_load = NULL;
    CHK_INT(term_bb_serve(&e, bb_env_buf, 200, &res), TERM_BB_E_ENV);
    e = bb_env(); e.hwm_store = NULL;
    CHK_INT(term_bb_serve(&e, bb_env_buf, 200, &res), TERM_BB_E_ENV);
    e = bb_env(); e.publish = NULL;
    CHK_INT(term_bb_serve(&e, bb_env_buf, 200, &res), TERM_BB_E_ENV);
    e = bb_env(); e.topic = NULL;
    CHK_INT(term_bb_serve(&e, bb_env_buf, 200, &res), TERM_BB_E_ENV);
    e = bb_env(); e.scratch = NULL;
    CHK_INT(term_bb_serve(&e, bb_env_buf, 200, &res), TERM_BB_E_ENV);
    CHK_INT(bb.verify_calls, 0);
    CHK_INT(bb.store_calls, 0);
    CHK_INT(bb.chunks, 0);

    t_case("a scratch below TERM_BB_SCRATCH_MIN is refused, not worked around");
    {
        const char *m = req(1, "stats");
        size_t n = bb_wrap(m, strlen(m));
        bb_reset();
        bb_scratch(TERM_BB_SCRATCH_MIN - 1u);
        e = bb_env();
        CHK_INT(term_bb_serve(&e, bb_env_buf, n, &res), TERM_BB_E_ENV);
        CHK_INT(bb.verify_calls, 0);
        CHK_INT(bb.chunks, 0);
        /* ... and exactly TERM_BB_SCRATCH_MIN is accepted. */
        bb_reset();
        bb_scratch(TERM_BB_SCRATCH_MIN);
        e = bb_env();
        CHK_INT(term_bb_serve(&e, bb_env_buf, n, &res), TERM_BB_OK);
        CHK_TRUE(bb.chunks >= 1u);
        CHK_TRUE(bb_scratch_intact());
    }
}

static void case_length_before_signature(void)
{
    term_bb_result_t res;
    term_bb_env_t e;
    static unsigned char big[TERM_BB_ENVELOPE_MAX + 64u];

    t_case("an envelope shorter than a signature never reaches the verifier");
    bb_reset();
    e = bb_env();
    CHK_INT(term_bb_serve(&e, bb_env_buf, 0, &res), TERM_BB_E_SIZE);
    CHK_INT(term_bb_serve(&e, bb_env_buf, 1, &res), TERM_BB_E_SIZE);
    CHK_INT(term_bb_serve(&e, bb_env_buf, TERM_BB_SIG_LEN - 1u, &res),
            TERM_BB_E_SIZE);
    CHK_INT(bb.verify_calls, 0);
    CHK_INT(bb.load_calls, 0);
    CHK_INT(bb.store_calls, 0);

    t_case("a signature with no message at all is a length refusal too");
    CHK_INT(term_bb_serve(&e, bb_env_buf, TERM_BB_SIG_LEN, &res), TERM_BB_E_SIZE);

    t_case("an envelope over TERM_BB_ENVELOPE_MAX never reaches the verifier");
    memset(big, 'x', sizeof big);
    bb_reset();
    e = bb_env();
    CHK_INT(term_bb_serve(&e, big, TERM_BB_ENVELOPE_MAX + 1u, &res),
            TERM_BB_E_SIZE);
    CHK_INT(term_bb_serve(&e, big, sizeof big, &res), TERM_BB_E_SIZE);
    CHK_INT(bb.verify_calls, 0);
    CHK_INT(bb.chunks, 0);

    t_case("a NULL payload is refused");
    CHK_TRUE(term_bb_serve(&e, NULL, 200, &res) != TERM_BB_OK);
    CHK_INT(bb.chunks, 0);
}

static void case_signature_before_everything_after_it(void)
{
    t_case("a bad signature outranks a broken message (step 3 before step 4)");
    bb_reset();
    bb.verify_fail = true;
    serve_expect("this is not a request at all", TERM_BB_E_SIG);
    serve_expect("bbpull1\nredact=1\n", TERM_BB_E_SIG);
    serve_expect(req(1, "live"), TERM_BB_E_SIG);
    CHK_INT(bb.verify_calls, 3);

    t_case("a bad signature outranks a stale counter (step 3 before step 5)");
    bb_reset();
    bb.mark = 5000;
    bb.verify_fail = true;
    serve_expect(req(1, "live"), TERM_BB_E_SIG);
    CHK_INT(bb.store_calls, 0);
    CHK_TRUE(bb.mark == 5000u);

    t_case("a bad signature outranks a wrong topic");
    bb_reset();
    bb.verify_fail = true;
    serve_expect(req_for(9, "live", "someone/else"), TERM_BB_E_SIG);

    t_case("Q1: the message that is parsed is the VERIFIED one, not the payload");
    /* The verifier is handed the payload and hands back a message. If the
     * responder parsed the payload's tail instead, this would answer `live`. */
    bb_reset();
    bb.emit = req(11, "stats");
    bb.emit_len = strlen(bb.emit);
    {
        term_bb_result_t res;
        CHK_INT(bb_serve(req(22, "live"), &res), TERM_BB_OK);
        CHK_TRUE(res.req.ctr == 11u);
        CHK_INT(res.req.what, TERM_BB_WHAT_STATS);
        CHK_TRUE(bb.mark == 11u);
        CHK_INT(bb.chunks, 1);
        CHK_STR(bb_jstr(bb.chunk[0], "what"), "stats");
    }

    t_case("...and the verifier saw the raw payload, byte for byte");
    bb_reset();
    {
        const char *m = req(31, "stats");
        size_t n = bb_wrap(m, strlen(m));
        term_bb_env_t e = bb_env();
        term_bb_result_t res;
        CHK_INT(term_bb_serve(&e, bb_env_buf, n, &res), TERM_BB_OK);
        CHK_INT(bb.verify_calls, 1);
        CHK_INT(bb.seen_smlen, n);
        CHK_INT(memcmp(bb.seen_sm, bb_env_buf, n), 0);
        CHK_INT(bb.seen_too_big, 0);
        /* "signature verified into `scratch`" — the buffer handed to the
         * verifier is the caller's scratch, which is also why scratch_size
         * has to cover the whole envelope. */
        CHK_TRUE((char *)bb.seen_m >= bb.scratch);
        CHK_TRUE((char *)bb.seen_m + n <= bb.scratch + bb.scratch_size);
    }
}

static void case_parse_before_the_gate(void)
{
    t_case("a broken message outranks a stale counter (step 4 before step 5)");
    bb_reset();
    bb.mark = 9999;
    serve_expect("bbpull1\nctr=1\ntop=" BB_TOPIC "\nwhat=live\nredact=1\n",
                 TERM_BB_E_UNKNOWN);
    serve_expect("bbpull2\nctr=1\ntop=" BB_TOPIC "\nwhat=live\n", TERM_BB_E_MAGIC);
    serve_expect("bbpull1\nctr=0\ntop=" BB_TOPIC "\nwhat=live\n", TERM_BB_E_RANGE);
    CHK_INT(bb.store_calls, 0);
    CHK_TRUE(bb.mark == 9999u);

    t_case("a broken message outranks a wrong topic");
    bb_reset();
    serve_expect("bbpull1\nctr=1\ntop=someone/else\nwhat=live\nredact=1\n",
                 TERM_BB_E_UNKNOWN);
    serve_expect("bbpull1\nctr=1\ntop=someone/else\nwhat=bogus\n",
                 TERM_BB_E_SYNTAX);
}

static void case_topic_before_the_counter(void)
{
    t_case("a wrong-topic request does not burn this device's counter (Q3)");
    bb_reset();
    bb.mark = 1000;
    serve_expect(req_for(2000, "live", "someone/else"), TERM_BB_E_TOPIC);
    CHK_INT(bb.store_calls, 0);
    CHK_TRUE(bb.mark == 1000u);
    /* The same counter still works for a request that IS for this device —
     * which is the whole point of not consuming it. */
    {
        term_bb_result_t res;
        CHK_INT(bb_serve(req(2000, "stats"), &res), TERM_BB_OK);
        CHK_TRUE(bb.mark == 2000u);
    }

    t_case("a wrong topic outranks a stale counter, and is reported as such");
    bb_reset();
    bb.mark = 5000;
    serve_expect(req_for(10, "live", "someone/else"), TERM_BB_E_TOPIC);
    serve_expect(req_for(5000, "live", "someone/else"), TERM_BB_E_TOPIC);
    serve_expect(req_for(10, "live", BB_TOPIC), TERM_BB_E_REPLAY);
    CHK_INT(bb.store_calls, 0);
}

static void case_equal_is_refused_greater_is_served(void)
{
    term_bb_result_t res;

    t_case("the counter must be STRICTLY greater than the stored mark");
    bb_reset();
    bb.mark = 1000;
    serve_expect(req(1000, "stats"), TERM_BB_E_REPLAY);
    serve_expect(req(999, "stats"), TERM_BB_E_REPLAY);
    serve_expect(req(1, "stats"), TERM_BB_E_REPLAY);
    CHK_INT(bb.store_calls, 0);
    CHK_INT(bb.chunks, 0);

    CHK_INT(bb_serve(req(1001, "stats"), &res), TERM_BB_OK);
    CHK_TRUE(res.accepted);
    CHK_TRUE(res.hwm == 1001u);
    CHK_TRUE(bb.mark == 1001u);
    CHK_INT(bb.store_calls, 1);
    CHK_TRUE(bb.last_stored == 1001u);

    t_case("...so a captured request is inert the second time it arrives");
    bb.chunks = 0;
    bb.publish_calls = 0;
    serve_expect(req(1001, "stats"), TERM_BB_E_REPLAY);
    CHK_INT(bb.store_calls, 1);   /* still just the one accept */
}

/* ===================================================================== */
/* Q2's durability ordering: the mark is burned BEFORE the first chunk    */
/* ===================================================================== */

static void case_mark_is_burned_before_the_reply(void)
{
    term_bb_result_t res;

    t_case("hwm_store happens before the first publish, so a crash mid-reply "
           "burns the counter");
    bb_reset();
    bb.publish_fail_at = 1;      /* the very first chunk never lands */
    CHK_INT(bb_serve(req(7000, "live"), &res), TERM_BB_E_PUBLISH);
    CHK_INT(bb.store_calls, 1);
    CHK_TRUE(bb.last_stored == 7000u);
    CHK_TRUE(bb.mark == 7000u);           /* burned anyway */
    CHK_INT(bb.chunks, 0);                /* nothing was published */
    CHK_INT(bb.publish_calls, 1);
    CHK_TRUE(res.accepted);               /* the gate passed and the mark stored */
    CHK_TRUE(res.hwm == 7000u);
    CHK_INT(res.chunks, 0);

    t_case("...and a replay of that request is refused afterwards");
    bb.publish_fail_at = 0;
    serve_expect(req(7000, "live"), TERM_BB_E_REPLAY);

    t_case("a publish that fails mid-reply still leaves the mark burned");
    bb_reset();
    (void)bb_fill(120u, 40u, "chunker");
    bb.publish_fail_at = 3;
    CHK_INT(bb_serve(req(8000, "live"), &res), TERM_BB_E_PUBLISH);
    CHK_TRUE(bb.mark == 8000u);
    CHK_INT(bb.chunks, 2);
    CHK_INT(bb.publish_calls, 3);
    CHK_TRUE(res.chunks == 2u);
    CHK_TRUE(bb_scratch_intact());

    t_case("a store that fails publishes NOTHING (fail closed)");
    bb_reset();
    bb.mark = 100;
    bb.store_fail = true;
    CHK_INT(bb_serve(req(200, "live"), &res), TERM_BB_E_HWM);
    CHK_INT(bb.store_calls, 1);          /* exactly one attempt, no retry */
    CHK_INT(bb.chunks, 0);
    CHK_INT(bb.publish_calls, 0);
    CHK_TRUE(bb.mark == 100u);           /* unmoved */
    CHK_TRUE(!res.accepted);
    CHK_TRUE(res.hwm == 100u);

    t_case("a mark that cannot be READ publishes nothing either");
    bb_reset();
    bb.load_fail = true;
    CHK_INT(bb_serve(req(200, "live"), &res), TERM_BB_E_HWM);
    CHK_INT(bb.store_calls, 0);
    CHK_INT(bb.chunks, 0);
    CHK_INT(bb.publish_calls, 0);
    CHK_TRUE(!res.accepted);

    t_case("an absent store is not a failure: mark 0 and any ctr >= 1 works");
    bb_reset();
    bb.mark = 0;
    CHK_INT(bb_serve(req(1, "stats"), &res), TERM_BB_OK);
    CHK_TRUE(bb.mark == 1u);
}

/* ===================================================================== */
/* The result record                                                     */
/* ===================================================================== */

static void case_result_is_always_written(void)
{
    term_bb_result_t res;
    term_bb_env_t e;

    t_case("`res` is zeroed first and written whatever the code");
    bb_reset();
    memset(&res, 0xCD, sizeof res);
    CHK_INT(bb_serve("bbpull2\nctr=1\ntop=t\nwhat=live", &res), TERM_BB_E_MAGIC);
    CHK_TRUE(!res.parsed);
    CHK_TRUE(!res.accepted);
    CHK_INT(res.chunks, 0);
    CHK_INT(res.records, 0);
    CHK_TRUE(res.req.ctr == 0u);
    CHK_INT(res.req.from, 0);
    CHK_INT(res.req.topic[0], 0);

    t_case("`parsed` says the request below is meaningful");
    bb_reset();
    bb.mark = 9999;
    memset(&res, 0xCD, sizeof res);
    CHK_INT(bb_serve(req(5, "live"), &res), TERM_BB_E_REPLAY);
    CHK_TRUE(res.parsed);
    CHK_TRUE(!res.accepted);
    CHK_TRUE(res.req.ctr == 5u);
    CHK_STR(res.req.topic, BB_TOPIC);
    CHK_INT(res.req.what, TERM_BB_WHAT_LIVE);

    t_case("a wrong-topic refusal still reports what it parsed");
    bb_reset();
    memset(&res, 0xCD, sizeof res);
    CHK_INT(bb_serve(req_for(5, "live", "someone/else"), &res), TERM_BB_E_TOPIC);
    CHK_TRUE(res.parsed);
    CHK_STR(res.req.topic, "someone/else");

    t_case("`res` may be NULL");
    bb_reset();
    e = bb_env();
    {
        const char *m = req(1, "stats");
        size_t n = bb_wrap(m, strlen(m));
        CHK_INT(term_bb_serve(&e, bb_env_buf, n, NULL), TERM_BB_OK);
        CHK_INT(bb.chunks, 1);
        CHK_TRUE(bb.mark == 1u);
        CHK_INT(term_bb_serve(&e, bb_env_buf, n, NULL), TERM_BB_E_REPLAY);
        CHK_INT(term_bb_serve(&e, bb_env_buf, 3, NULL), TERM_BB_E_SIZE);
    }
}

int main(void)
{
    t_suite("bb_gate");
    t_case("the port installs and the ring boots");
    fp_reset();
    CHK_TRUE(term_port_install(fp_port()));
    term_lp_ring_boot();
    CHK_TRUE(term_lp_ring_ready());

    case_gate_alone();
    case_env_sanity_is_first();
    case_length_before_signature();
    case_signature_before_everything_after_it();
    case_parse_before_the_gate();
    case_topic_before_the_counter();
    case_equal_is_refused_greater_is_served();
    case_mark_is_burned_before_the_reply();
    case_result_is_always_written();

    fp_reclaim_all();
    return t_summary();
}
