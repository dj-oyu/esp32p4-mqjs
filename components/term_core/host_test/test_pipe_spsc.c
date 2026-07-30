/*
 * test_pipe_spsc.c — the single-producer rule and the re-pipe choreography
 * (§5, phase 4).
 *
 * Contracts under test:
 *   §5  "バイトリングは SPSC を強制する: VT term の producer は pipe か
 *        term.feed のどちらか一方。pipe 中の feed はエラー、再 pipe は
 *        旧接続の detach(ack join、§3.1 の quiesce と同じ手順)を済ませて
 *        から新接続を張る" — and its phase-4 settlement (a): "pipe 中は
 *        feed だけでなく term.log もエラー"; (b) "再 pipe の「ack join」は
 *        呼び出し側の再試行にした".
 *   term_registry.h feed:  "If a pipe is bound, feed is TERM_ERR_BUSY."
 *   term_registry.h log:   "a piped term has exactly one producer, so log is
 *                           TERM_ERR_BUSY while a pipe is bound."
 *   term_registry.h pipe:  "TERM_ERR_MODE unless the term is TERM_VT";
 *                          "WHEN A PRODUCER IS ALREADY BOUND this call
 *                           requests its detach … and returns TERM_ERR_BUSY
 *                           WITHOUT binding the new one. It does NOT wait for
 *                           the ack."
 *   term_registry.h detach_fn: "Called from the registry with the table lock
 *                           NOT held".
 *   term_registry.h bind:  "term_registry_producer_bind(id, o, d, u) is
 *                           exactly [bind_ex] with {d, NULL, u}."
 *   I3 (§3.1): every entry point re-checks generation and owner.
 *
 * The suite drives the ack through the fake producer, which is the only way
 * the §3.1 window between "detach asked" and "detach acked" is observable at
 * all: an ack that happened inside the detach call would hide it.
 */
#include "pipe_util.h"

/* This suite's fakes. Instances live in the suite, not in pipe_util.h, so a
 * suite that does not need one does not carry an unused object. */
static p4_prod_t p4_pa, p4_pb;
static p4_js_t   p4_js;

/* The gate and the pipe state are one thing, so assert them together: a
 * term is piped exactly while its own ingest paths are refused. */
static void p4_check_gate(term_id_t id, const char *owner, const char *where)
{
    bool piped = reg_info(id).piped;
    term_err_t f = term_registry_feed(id, owner, (const uint8_t *)"g", 1, NULL);
    term_err_t l = term_registry_log(id, owner, "g", 1);
    t_case(where);
    if (piped) {
        CHK_INT(f, TERM_ERR_BUSY);
        CHK_INT(l, TERM_ERR_BUSY);
    } else {
        CHK_INT(f, TERM_OK);
        CHK_INT(l, TERM_OK);
    }
}

/* ===================================================================== */
/* The gate                                                              */
/* ===================================================================== */

static void case_feed_and_log_are_both_refused(void)
{
    term_id_t id;
    size_t wrote = 0x5A5A;

    t_case("an unpiped VT term takes feed and log (feed IS the producer)");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"pre", 3, &wrote),
            TERM_OK);
    CHK_INT((int)wrote, 3);
    CHK_INT(term_registry_log(id, OWNER_A, "pre", 3), TERM_OK);
    CHK_TRUE(!reg_info(id).piped);

    t_case("pipe makes it piped, and feed is TERM_ERR_BUSY (§5)");
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);
    CHK_TRUE(reg_info(id).piped);
    wrote = 0x5A5A;
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"no", 2, &wrote),
            TERM_ERR_BUSY);

    t_case("term.log is refused too — it advances the same ring head (§5 (a))");
    CHK_INT(term_registry_log(id, OWNER_A, "no", 2), TERM_ERR_BUSY);

    t_case("a refused ingest is not a drop: nothing was taken and nothing lost");
    CHK_INT((int)reg_info(id).bytes_dropped, 0);
    CHK_INT((int)reg_stats().drops_full, 0);

    t_case("...and none of the refused bytes are in the term");
    reg_frame();
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "pre") != NULL);
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "no") == NULL);

    t_case("the detach callback ran with the table lock NOT held");
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(p4_pa.detaches, 1);
    CHK_INT(p4_pa.detach_lock_depth, 0);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);

    p4_check_gate(id, OWNER_A, "after the ack the term is its own producer again");
    CHK_INT(reg_shutdown(), 0);
}

static void case_pipe_needs_a_vt_term(void)
{
    term_id_t lg, vt;

    t_case("pipe on a TERM_LOG term is TERM_ERR_MODE (multi-writer by design)");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    lg = reg_new("l", OWNER_A, TERM_LOG, false, 20, 4);
    vt = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
    REQUIRE(lg != TERM_ID_INVALID && vt != TERM_ID_INVALID);

    CHK_INT(p4_pipe(lg, OWNER_A, &p4_pa, true), TERM_ERR_MODE);
    CHK_TRUE(!reg_info(lg).piped);
    CHK_INT(p4_pa.detaches, 0);

    t_case("a refused pipe leaves the log term's writers working");
    CHK_INT(term_registry_log(lg, OWNER_A, "still", 5), TERM_OK);
    CHK_INT(term_registry_feed(lg, OWNER_A, (const uint8_t *)"here", 4, NULL),
            TERM_OK);
    CHK_INT((int)reg_stats().pipes, 0);

    t_case("and it did not disturb the VT term next door");
    CHK_INT(p4_pipe(vt, OWNER_A, &p4_pa, true), TERM_OK);
    CHK_TRUE(reg_info(vt).piped);
    CHK_INT((int)reg_stats().pipes, 1);

    CHK_INT(term_registry_producer_unbind(vt, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(vt), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

static void case_pipe_is_owner_gated(void)
{
    term_id_t id;
    term_producer_t prod;

    t_case("pipe/unpipe/set_reply are owner-gated like everything else (I3)");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    p4_js_reset(&p4_js);
    id = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);

    CHK_INT(p4_pipe(id, OWNER_B, &p4_pa, true), TERM_ERR_NOT_OWNER);
    CHK_INT(term_registry_producer_unbind(id, OWNER_B), TERM_ERR_NOT_OWNER);
    CHK_INT(term_registry_set_reply(id, OWNER_B, p4_js_sink, &p4_js),
            TERM_ERR_NOT_OWNER);
    CHK_TRUE(!reg_info(id).piped);
    CHK_INT(p4_pa.detaches, 0);
    CHK_TRUE(reg_stats().denied_owner >= 3);

    t_case("a foreign caller cannot even provoke the old producer's detach");
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);
    CHK_INT(p4_pipe(id, OWNER_B, &p4_pb, true), TERM_ERR_NOT_OWNER);
    CHK_INT(p4_pa.detaches, 0);

    t_case("a stale id is TERM_ERR_STALE / BAD_ID, never a silent bind");
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    reg_settle();
    CHK_TRUE(p4_pipe(id, OWNER_A, &p4_pa, true) != TERM_OK);
    CHK_TRUE(term_registry_producer_unbind(id, OWNER_A) != TERM_OK);
    CHK_TRUE(term_registry_set_reply(id, OWNER_A, p4_js_sink, &p4_js) != TERM_OK);

    t_case("NULL arguments are errors, not crashes (I5)");
    {
        term_id_t v = reg_new("v2", OWNER_A, TERM_VT, false, 20, 4);
        REQUIRE(v != TERM_ID_INVALID);
        CHK_INT(term_registry_pipe(v, OWNER_A, NULL), TERM_ERR_INVAL);
        memset(&prod, 0, sizeof prod);
        prod.user = &p4_pa;               /* no detach: the required hook */
        CHK_INT(term_registry_pipe(v, OWNER_A, &prod), TERM_ERR_INVAL);
        CHK_TRUE(!reg_info(v).piped);
        CHK_INT(term_registry_producer_bind(v, OWNER_A, NULL, &p4_pa),
                TERM_ERR_INVAL);
        CHK_INT(term_registry_producer_bind_ex(v, OWNER_A, NULL), TERM_ERR_INVAL);
        CHK_TRUE(!reg_info(v).piped);
    }

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The re-pipe choreography (§5 (b) / §3.1)                              */
/* ===================================================================== */

static void case_repipe_asks_then_refuses_then_succeeds(void)
{
    term_id_t id;
    term_registry_stats_t before, after;

    t_case("pipe on an already piped term is BUSY and does not bind");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    p4_prod_reset(&p4_pb, "B");
    id = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);
    CHK_INT((int)reg_stats().pipes, 1);

    before = reg_stats();
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pb, true), TERM_ERR_BUSY);
    after = reg_stats();

    t_case("...but it DOES ask the old producer to detach (§5)");
    CHK_INT(p4_pa.detaches, 1);
    CHK_INT(p4_pa.last_detach_id, id);
    CHK_INT(p4_pa.detach_lock_depth, 0);
    CHK_INT(p4_pb.detaches, 0);
    CHK_INT((int)(after.pipe_busy - before.pipe_busy), 1);
    CHK_INT((int)(after.pipes - before.pipes), 0);

    t_case("the term is still the OLD producer's until the ack lands");
    CHK_TRUE(reg_info(id).piped);
    CHK_TRUE(reg_info(id).pending_acks > 0);
    CHK_INT(p4_space(id, &p4_pb), -1);          /* B is nobody yet */
    CHK_TRUE(p4_space(id, &p4_pa) >= 0);        /* A still owns the ring */

    t_case("it does NOT join: a retry before the ack is refused again");
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pb, true), TERM_ERR_BUSY);
    CHK_INT(p4_pa.detaches, 2);                 /* asked on each attempt */
    CHK_TRUE(reg_info(id).piped);
    CHK_INT(fp.signal_waits, 0);                /* nothing waited on     */

    t_case("re-pipe succeeds only after the old producer acks (§3.1)");
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_TRUE(!reg_info(id).piped);
    CHK_INT((int)reg_info(id).pending_acks, 0);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pb, true), TERM_OK);
    CHK_TRUE(reg_info(id).piped);
    CHK_INT((int)reg_stats().pipes, 2);

    t_case("the new producer owns the ring and the old one is a stranger");
    CHK_TRUE(p4_space(id, &p4_pb) >= 0);
    CHK_INT(p4_space(id, &p4_pa), -1);
    {
        term_err_t rc = TERM_OK;
        CHK_INT((int)p4_write(id, &p4_pa, "x", 1, &rc), 0);
        CHK_INT(rc, TERM_ERR_BUSY);
    }

    t_case("unpipe + ack returns the term to feed, with nothing outstanding");
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(p4_pb.detaches, 1);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    p4_check_gate(id, OWNER_A, "unpiped: feed and log work again");
    CHK_INT((int)reg_info(id).pending_acks, 0);

    t_case("no recursive locking anywhere in the choreography (term_port.h)");
    CHK_INT(fp.recursive_attempts, 0);
    CHK_INT(fp.depth, 0);
    CHK_INT(fp.lock_ok, fp.unlocks);

    CHK_INT(reg_shutdown(), 0);
}

static void case_unpipe_without_a_producer(void)
{
    term_id_t id;

    t_case("unpipe on a term with no producer changes nothing");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);

    /* The header does not fix a code for this, so only the invariant is
     * asserted: whatever it answers, the term must not end up owing an ack
     * nobody will send, and feed must keep working. */
    (void)term_registry_producer_unbind(id, OWNER_A);
    CHK_INT((int)reg_info(id).pending_acks, 0);
    CHK_TRUE(!reg_info(id).piped);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"ok", 2, NULL),
            TERM_OK);

    t_case("an unmatched ack is dropped and counted, not applied to the term");
    CHK_TRUE(term_registry_producer_ack(id) == TERM_OK ||
             reg_stats().stale_acks > 0);
    CHK_INT((int)reg_info(id).pending_acks, 0);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"ok", 2, NULL),
            TERM_OK);

    t_case("...and the term still binds a producer afterwards");
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);

    CHK_INT(reg_shutdown(), 0);
}

static void case_bind_is_bind_ex_without_a_reply(void)
{
    term_id_t a, b;
    term_producer_t prod;

    t_case("producer_bind == bind_ex with {detach, NULL, user} (header)");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    p4_prod_reset(&p4_pb, "B");
    a = reg_new("a", OWNER_A, TERM_VT, false, 20, 4);
    b = reg_new("b", OWNER_A, TERM_VT, false, 20, 4);
    REQUIRE(a != TERM_ID_INVALID && b != TERM_ID_INVALID);

    CHK_INT(term_registry_producer_bind(a, OWNER_A, p4_detach, &p4_pa), TERM_OK);
    memset(&prod, 0, sizeof prod);
    prod.detach = p4_detach;
    prod.reply = NULL;
    prod.user = &p4_pb;
    CHK_INT(term_registry_producer_bind_ex(b, OWNER_A, &prod), TERM_OK);

    CHK_TRUE(reg_info(a).piped);
    CHK_TRUE(reg_info(b).piped);
    CHK_INT(term_registry_feed(a, OWNER_A, (const uint8_t *)"x", 1, NULL),
            TERM_ERR_BUSY);
    CHK_INT(term_registry_feed(b, OWNER_A, (const uint8_t *)"x", 1, NULL),
            TERM_ERR_BUSY);

    t_case("both count as pipes, and both cookies address their own ring");
    CHK_INT((int)reg_stats().pipes, 2);
    CHK_TRUE(p4_space(a, &p4_pa) >= 0);
    CHK_TRUE(p4_space(b, &p4_pb) >= 0);
    CHK_INT(p4_space(a, &p4_pb), -1);
    CHK_INT(p4_space(b, &p4_pa), -1);

    t_case("a producer bound with a NULL cookie is addressed by NULL");
    {
        term_id_t c = reg_new("c", OWNER_A, TERM_VT, false, 20, 4);
        size_t sp = 0x5A5A;
        REQUIRE(c != TERM_ID_INVALID);
        CHK_INT(term_registry_producer_bind(c, OWNER_A, p4_detach, NULL), TERM_OK);
        CHK_INT(term_registry_producer_space(c, NULL, &sp), TERM_OK);
        CHK_TRUE(sp > 0);
        CHK_INT(term_registry_producer_space(c, &p4_pa, &sp), TERM_ERR_BUSY);
        CHK_INT(term_registry_producer_unbind(c, OWNER_A), TERM_OK);
        CHK_INT(term_registry_producer_ack(c), TERM_OK);
    }

    CHK_INT(term_registry_producer_unbind(a, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(a), TERM_OK);
    CHK_INT(term_registry_producer_unbind(b, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(b), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The pipe against the quiesce (§3.1)                                   */
/* ===================================================================== */

static void case_close_asks_the_pipe_to_detach(void)
{
    term_registry_config_t c = p4_cfg_quiesce(1000);
    term_id_t id;

    t_case("close asks the bound producer to detach, and joins nothing");
    CHK_TRUE(reg_boot_cfg(&c));
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);

    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    CHK_INT(p4_pa.detaches, 1);
    CHK_INT(p4_pa.detach_lock_depth, 0);
    CHK_INT(reg_state(id), TERM_SLOT_DYING);
    CHK_INT(fp.frees, 0);
    CHK_INT(fp.signal_waits, 0);

    t_case("a DYING term answers the producer's ring calls with DYING");
    {
        size_t sp = 0x5A5A, wrote = 0x5A5A;
        CHK_INT(term_registry_producer_space(id, &p4_pa, &sp), TERM_ERR_DYING);
        CHK_INT(term_registry_producer_write(id, &p4_pa,
                                             (const uint8_t *)"x", 1, &wrote),
                TERM_ERR_DYING);
    }

    t_case("a pipe that acks lets the slot free on the next reaper pass");
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    reg_settle();
    CHK_INT(reg_state(id), -1);
    CHK_INT(fp.frees, 1);
    CHK_INT(reg_stats().zombies, 0);

    t_case("a pipe that never acks becomes the deliberate ZOMBIE instead");
    p4_prod_reset(&p4_pb, "B");
    id = reg_new("v2", OWNER_A, TERM_VT, false, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pb, true), TERM_OK);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    fp_advance(1001);
    reg_frame();
    term_registry_reap();
    CHK_INT(reg_state(id), TERM_SLOT_ZOMBIE);
    CHK_TRUE(reg_stats().zombies >= 1);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    reg_settle();
    CHK_INT(reg_state(id), -1);

    CHK_INT(reg_shutdown(), 0);
}

static void case_owner_stopped_detaches_the_pipe(void)
{
    term_id_t id;

    t_case("an owner that stops takes its non-persist piped term with it");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);

    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);
    CHK_INT(reg_state(id), TERM_SLOT_DYING);
    CHK_INT(p4_pa.detaches, 1);
    CHK_INT(p4_pa.detach_lock_depth, 0);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    reg_settle();
    CHK_INT(reg_state(id), -1);

    t_case("a persist term detaches instead, and keeps its producer");
    p4_prod_reset(&p4_pb, "B");
    id = reg_new("p", OWNER_A, TERM_VT, true, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pb, true), TERM_OK);
    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);
    CHK_INT(reg_state(id), TERM_SLOT_DETACHED);

    /* §3.1: a DETACHED term is "alive, still ingesting from platform
     * producers". So the platform producer must still be able to write. */
    CHK_INT(p4_pb.detaches, 0);
    CHK_TRUE(reg_info(id).piped);
    CHK_TRUE(p4_space(id, &p4_pb) > 0);
    {
        term_err_t rc = TERM_ERR_INVAL;
        CHK_INT((int)p4_write(id, &p4_pb, "detached\r\n", 10, &rc), 10);
        CHK_INT(rc, TERM_OK);
    }
    reg_frame();

    t_case("...and the re-attach finds it with the pipe still bound");
    {
        term_create_opts_t o = reg_opts("p", OWNER_A, TERM_VT, true, 20, 4);
        term_id_t again = TERM_ID_INVALID;
        bool re = false;
        CHK_INT(term_registry_create(&o, &again, &re), TERM_OK);
        CHK_INT(again, id);
        CHK_TRUE(re);
        CHK_TRUE(reg_info(id).piped);
        CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"x", 1, NULL),
                TERM_ERR_BUSY);
        CHK_TRUE(strstr(reg_snap(id, OWNER_A), "detached") != NULL);
    }

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("pipe_spsc");
    case_feed_and_log_are_both_refused();
    case_pipe_needs_a_vt_term();
    case_pipe_is_owner_gated();
    case_repipe_asks_then_refuses_then_succeeds();
    case_unpipe_without_a_producer();
    case_bind_is_bind_ex_without_a_reply();
    case_close_asks_the_pipe_to_detach();
    case_owner_stopped_detaches_the_pipe();
    fp_reclaim_all();
    return t_summary();
}
