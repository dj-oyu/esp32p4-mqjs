/*
 * test_pipe_stub.c — term_pipe.h's lifecycle against the host stub producer.
 *
 * Contracts under test (term_pipe.h):
 *   "Off the device there is no ssh, so the handle is ignored and the bind
 *    installs a STUB producer: it never produces a byte, but it is a real
 *    producer as far as the registry is concerned — it makes the term piped,
 *    so feed/log answer TERM_ERR_BUSY, and it acks its detach one
 *    term_pipe_pump() later rather than instantly, which is what makes the
 *    re-pipe rule (§5) observable from a host test."
 *   term_pipe_bind: "Returns what term_registry_pipe() returns, so all of its
 *    rules apply unchanged — owner gate, TERM_ERR_MODE for a non-VT term, and
 *    TERM_ERR_BUSY when a producer was already bound (in which case that
 *    producer has just been asked to detach and a retry after its ack
 *    succeeds)"; "TERM_ERR_NO_SLOT no free pipe context (one per term slot)".
 *   term_pipe_unbind: "stage-1 semantics, no join. The term stops being piped
 *    when the ack lands".
 *   term_pipe_pump: "this is where a requested detach turns into
 *    term_registry_producer_ack()".
 *   term_pipe_count: "How many pipe contexts are in use. Introspection for
 *    tests and probes; never a gate on anything."
 *
 * term_pipe's contexts outlive a single registry, so every case here starts
 * and ends with term_pipe_count() == 0: a leaked context would otherwise show
 * up as an unrelated failure three cases later.
 */
#include "pipe_util.h"

/* The handle is ignored by the stub; a recognisable non-zero value makes it
 * obvious in a failure that nothing was derived from it. */
#define P4_HANDLE 7

static void p4_expect_clean(void)
{
    CHK_INT(term_pipe_count(), 0);
}

/* ===================================================================== */
/* Bind                                                                  */
/* ===================================================================== */

static void case_bind_makes_a_real_producer(void)
{
    term_id_t id;
    size_t sp = 0x5A5A;

    t_case("term_pipe_bind installs a producer the registry believes in");
    CHK_TRUE(reg_boot());
    p4_expect_clean();
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);

    CHK_INT(term_pipe_bind(id, OWNER_A, P4_HANDLE), TERM_OK);
    CHK_INT(term_pipe_count(), 1);
    CHK_TRUE(reg_info(id).piped);
    CHK_INT((int)reg_stats().pipes, 1);

    t_case("...so feed and log are both TERM_ERR_BUSY (§5)");
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"x", 1, NULL),
            TERM_ERR_BUSY);
    CHK_INT(term_registry_log(id, OWNER_A, "x", 1), TERM_ERR_BUSY);

    t_case("the ring belongs to the stub, and nobody else can address it");
    CHK_INT(term_registry_producer_space(id, NULL, &sp), TERM_ERR_BUSY);

    t_case("the stub produces nothing: the term stays empty however long");
    p4_drain_frames(8);
    term_pipe_pump();
    p4_drain_frames(8);
    CHK_INT((int)reg_info(id).bytes_in, 0);
    CHK_STR(reg_snap_line0(id, OWNER_A), "");
    CHK_INT((int)reg_stats().prod_bytes, 0);

    t_case("pump with no detach outstanding changes nothing");
    term_pipe_pump();
    term_pipe_pump();
    CHK_INT(term_pipe_count(), 1);
    CHK_TRUE(reg_info(id).piped);

    t_case("unbind + pump releases the context and the term");
    CHK_INT(term_pipe_unbind(id, OWNER_A), TERM_OK);
    CHK_TRUE(reg_info(id).piped);                /* deferred: no join     */
    CHK_TRUE(reg_info(id).pending_acks > 0);
    CHK_INT(term_pipe_count(), 1);
    term_pipe_pump();
    CHK_TRUE(!reg_info(id).piped);
    CHK_INT((int)reg_info(id).pending_acks, 0);
    CHK_INT(term_pipe_count(), 0);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"back", 4, NULL),
            TERM_OK);

    CHK_INT(reg_shutdown(), 0);
    p4_expect_clean();
}

static void case_a_refused_bind_frees_its_context(void)
{
    term_id_t lg, vt;
    int i;

    t_case("a bind refused for TERM_ERR_MODE leaks no pipe context");
    CHK_TRUE(reg_boot());
    p4_expect_clean();
    lg = reg_new("l", OWNER_A, TERM_LOG, false, 40, 6);
    vt = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(lg != TERM_ID_INVALID && vt != TERM_ID_INVALID);

    for (i = 0; i < 12; i++) {
        CHK_INT(term_pipe_bind(lg, OWNER_A, P4_HANDLE), TERM_ERR_MODE);
        CHK_INT(term_pipe_count(), 0);
    }

    t_case("nor does one refused by the owner gate (I3)");
    for (i = 0; i < 12; i++) {
        CHK_INT(term_pipe_bind(vt, OWNER_B, P4_HANDLE), TERM_ERR_NOT_OWNER);
        CHK_INT(term_pipe_count(), 0);
    }

    t_case("nor one refused for a bad or stale id");
    CHK_TRUE(term_pipe_bind(TERM_ID_INVALID, OWNER_A, P4_HANDLE) != TERM_OK);
    CHK_TRUE(term_pipe_bind(0x7FFFFFF0, OWNER_A, P4_HANDLE) != TERM_OK);
    CHK_INT(term_pipe_count(), 0);

    t_case("...and the term is still bindable afterwards");
    CHK_INT(term_pipe_bind(vt, OWNER_A, P4_HANDLE), TERM_OK);
    CHK_INT(term_pipe_count(), 1);
    CHK_INT(term_pipe_unbind(vt, OWNER_A), TERM_OK);
    term_pipe_pump();
    CHK_INT(term_pipe_count(), 0);

    CHK_INT(reg_shutdown(), 0);
    p4_expect_clean();
}

/* ===================================================================== */
/* The re-pipe window the deferred ack exists to expose                  */
/* ===================================================================== */

static void case_the_deferred_ack_is_the_whole_point(void)
{
    term_id_t id;

    t_case("re-binding before the ack is BUSY, and asks the old stub to go");
    CHK_TRUE(reg_boot());
    p4_expect_clean();
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_pipe_bind(id, OWNER_A, P4_HANDLE), TERM_OK);

    CHK_INT(term_pipe_bind(id, OWNER_A, P4_HANDLE + 1), TERM_ERR_BUSY);
    CHK_TRUE(reg_info(id).piped);
    CHK_TRUE(reg_info(id).pending_acks > 0);
    CHK_TRUE(reg_stats().pipe_busy >= 1);

    t_case("...the ack does NOT arrive inside the call (that would hide it)");
    CHK_TRUE(reg_info(id).piped);
    CHK_INT(term_pipe_bind(id, OWNER_A, P4_HANDLE + 1), TERM_ERR_BUSY);

    t_case("one pump later the retry succeeds (§5's ack join as a retry)");
    term_pipe_pump();
    CHK_TRUE(!reg_info(id).piped);
    CHK_INT(term_pipe_count(), 0);
    CHK_INT(term_pipe_bind(id, OWNER_A, P4_HANDLE + 1), TERM_OK);
    CHK_INT(term_pipe_count(), 1);
    CHK_TRUE(reg_info(id).piped);

    t_case("the new context is the term's, and the pump does not undo it");
    term_pipe_pump();
    term_pipe_pump();
    CHK_TRUE(reg_info(id).piped);
    CHK_INT(term_pipe_count(), 1);

    CHK_INT(term_pipe_unbind(id, OWNER_A), TERM_OK);
    term_pipe_pump();
    CHK_INT(reg_shutdown(), 0);
    p4_expect_clean();
}

static void case_unbind_of_an_unpiped_term(void)
{
    term_id_t id;

    t_case("unbind on a term with no pipe leaves nothing outstanding");
    CHK_TRUE(reg_boot());
    p4_expect_clean();
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);

    (void)term_pipe_unbind(id, OWNER_A);       /* code not fixed by header */
    CHK_INT(term_pipe_count(), 0);
    CHK_INT((int)reg_info(id).pending_acks, 0);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"x", 1, NULL),
            TERM_OK);

    t_case("unbind is owner-gated, and a refused one changes nothing");
    CHK_INT(term_pipe_bind(id, OWNER_A, P4_HANDLE), TERM_OK);
    CHK_INT(term_pipe_unbind(id, OWNER_B), TERM_ERR_NOT_OWNER);
    CHK_TRUE(reg_info(id).piped);
    CHK_INT((int)reg_info(id).pending_acks, 0);
    CHK_INT(term_pipe_count(), 1);
    term_pipe_pump();
    CHK_TRUE(reg_info(id).piped);               /* nothing to ack          */

    CHK_INT(term_pipe_unbind(id, OWNER_A), TERM_OK);
    term_pipe_pump();
    CHK_INT(reg_shutdown(), 0);
    p4_expect_clean();
}

/* ===================================================================== */
/* One context per term slot                                             */
/* ===================================================================== */

static void case_one_context_per_slot(void)
{
    term_id_t ids[TERM_SLOT_COUNT];
    char name[TERM_NAME_MAX];
    int i;

    t_case("every slot can hold a pipe at once (one context per term slot)");
    CHK_TRUE(reg_boot());
    p4_expect_clean();
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        sprintf(name, "v%d", i);
        ids[i] = reg_new(name, OWNER_A, TERM_VT, false, 20, 4);
        REQUIRE(ids[i] != TERM_ID_INVALID);
    }
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        CHK_INT(term_pipe_bind(ids[i], OWNER_A, P4_HANDLE + i), TERM_OK);
        CHK_INT(term_pipe_count(), i + 1);
    }
    CHK_INT(term_pipe_count(), TERM_SLOT_COUNT);
    CHK_INT((int)reg_stats().pipes, TERM_SLOT_COUNT);

    t_case("...and the count never exceeds the table it mirrors");
    CHK_TRUE(term_pipe_count() <= TERM_SLOT_COUNT);

    t_case("releasing them one at a time releases exactly one context each");
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        CHK_INT(term_pipe_unbind(ids[i], OWNER_A), TERM_OK);
        term_pipe_pump();
        CHK_INT(term_pipe_count(), TERM_SLOT_COUNT - i - 1);
        CHK_TRUE(!reg_info(ids[i]).piped);
    }

    CHK_INT(reg_shutdown(), 0);
    p4_expect_clean();
}

/* ===================================================================== */
/* Against the quiesce                                                   */
/* ===================================================================== */

static void case_close_then_pump_collects_the_slot(void)
{
    term_registry_config_t c = p4_cfg_quiesce(1000);
    term_id_t id;

    t_case("closing a stub-piped term needs one pump to complete stage 2");
    CHK_TRUE(reg_boot_cfg(&c));
    p4_expect_clean();
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_pipe_bind(id, OWNER_A, P4_HANDLE), TERM_OK);

    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    CHK_INT(reg_state(id), TERM_SLOT_DYING);
    reg_frame();
    term_registry_reap();
    CHK_INT(reg_state(id), TERM_SLOT_DYING);      /* still owed the ack   */
    CHK_INT(term_pipe_count(), 1);

    term_pipe_pump();
    CHK_INT(term_pipe_count(), 0);
    reg_settle();
    CHK_INT(reg_state(id), -1);
    CHK_INT(fp.frees, 1);
    CHK_INT(reg_stats().zombies, 0);

    t_case("a term nobody pumps becomes the deliberate ZOMBIE, then collects");
    id = reg_new("v2", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_pipe_bind(id, OWNER_A, P4_HANDLE), TERM_OK);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    fp_advance(1001);
    reg_frame();
    term_registry_reap();
    CHK_INT(reg_state(id), TERM_SLOT_ZOMBIE);
    CHK_INT(term_pipe_count(), 1);

    term_pipe_pump();
    reg_settle();
    CHK_INT(reg_state(id), -1);
    CHK_INT(term_pipe_count(), 0);

    t_case("owner_stopped takes a stub-piped non-persist term the same way");
    id = reg_new("v3", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_pipe_bind(id, OWNER_A, P4_HANDLE), TERM_OK);
    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);
    CHK_INT(reg_state(id), TERM_SLOT_DYING);
    term_pipe_pump();
    reg_settle();
    CHK_INT(reg_state(id), -1);
    CHK_INT(term_pipe_count(), 0);

    CHK_INT(reg_shutdown(), 0);
    p4_expect_clean();
}

/* term_registry_deinit() forces every slot through stage 1 and runs the
 * reaper to completion. A stub that has not pumped therefore cannot have
 * acked, so the header's "number of slots that could NOT be collected" must
 * be 1 — and pumping afterwards, into a registry that no longer exists, must
 * be safe rather than a use-after-free. */
static void case_deinit_with_a_live_pipe(void)
{
    term_id_t id;
    int left;

    t_case("deinit reports the stub-piped slot as uncollectable");
    CHK_TRUE(reg_boot());
    p4_expect_clean();
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_pipe_bind(id, OWNER_A, P4_HANDLE), TERM_OK);

    left = reg_shutdown();
    CHK_INT(left, 1);

    t_case("pumping after the registry is gone is safe, and frees the context");
    term_pipe_pump();
    term_pipe_pump();
    CHK_INT(term_pipe_count(), 0);

    /* The retention is deliberate (§3.1 prefers a leak to a UAF); release it
     * so the run ends leak-clean. */
    fp_reclaim_all();

    t_case("and the next registry starts from a clean pipe table");
    CHK_TRUE(reg_boot());
    p4_expect_clean();
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_pipe_bind(id, OWNER_A, P4_HANDLE), TERM_OK);
    CHK_INT(term_pipe_count(), 1);
    CHK_INT(term_pipe_unbind(id, OWNER_A), TERM_OK);
    term_pipe_pump();
    CHK_INT(reg_shutdown(), 0);
    p4_expect_clean();
}

static void case_pipe_before_the_registry_exists(void)
{
    t_case("term_pipe_bind with no registry is an error, not a crash (I5)");
    fp_reset();
    CHK_TRUE(term_port_install(fp_port()));
    CHK_TRUE(term_pipe_bind(TERM_SLOT_COUNT, OWNER_A, P4_HANDLE) != TERM_OK);
    CHK_TRUE(term_pipe_unbind(TERM_SLOT_COUNT, OWNER_A) != TERM_OK);
    term_pipe_pump();
    CHK_INT(term_pipe_count(), 0);
}

int main(void)
{
    t_suite("pipe_stub");
    case_bind_makes_a_real_producer();
    case_a_refused_bind_frees_its_context();
    case_the_deferred_ack_is_the_whole_point();
    case_unbind_of_an_unpiped_term();
    case_one_context_per_slot();
    case_close_then_pump_collects_the_slot();
    case_deinit_with_a_live_pipe();
    case_pipe_before_the_registry_exists();
    fp_reclaim_all();
    return t_summary();
}
