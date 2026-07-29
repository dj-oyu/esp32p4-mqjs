/*
 * test_registry_quiesce.c — the asynchronous two-stage teardown (§3.1).
 *
 * Contracts under test:
 *   Stage 1 (js_task): "スロットを DYING にマークし新規 ingest を拒否、
 *     pipe 接続があれば producer へ detach を通知 … ここで一切待たずに
 *     即 return する。"
 *   Stage 2 (reaper, off js_task): "producer の detach ack と UI タスクの
 *     drain 完了を タイムアウトキャップ付き(3s)で join し、揃ったら
 *     generation++ してスロットを解放。タイムアウトした場合は ZOMBIE と
 *     して放置する — メモリは保持したまま再利用しない(132KB のリークを
 *     選び、UAF は選ばない)。ack が遅れて届けば reaper が後から回収する。
 *     ZOMBIE 数は カウンタで可視化。"
 *   §5 SPSC: "VT term の producer は pipe か term.feed のどちらか一方。
 *     pipe 中の feed はエラー、再 pipe は 旧接続の detach を済ませてから"
 *   term_registry.h producer_detach_fn: "Called from the registry with the
 *     table lock NOT held".
 */
#include "reg_util.h"

/* ===================================================================== */
/* A fake producer                                                       */
/* ===================================================================== */

typedef struct {
    int       detaches;
    term_id_t last_id;
    int       lock_depth_seen;   /* fp.depth inside the callback          */
    int       reentrancy_ok;
} prod_t;

static prod_t prod;

static void prod_detach(void *user, term_id_t id)
{
    prod_t *p = (prod_t *)user;
    p->detaches++;
    p->last_id = id;
    p->lock_depth_seen = fp.depth;
}

static void prod_reset(void) { memset(&prod, 0, sizeof prod); }

static term_registry_config_t cfg_quiesce(uint32_t ms)
{
    term_registry_config_t c;
    memset(&c, 0, sizeof c);
    c.quiesce_timeout_ms = ms;
    return c;
}

/* ===================================================================== */
/* Stage 1                                                               */
/* ===================================================================== */

static void case_stage1_does_not_free_and_does_not_join(void)
{
    term_id_t id;
    size_t bytes_before;

    t_case("close marks DYING, frees nothing, and joins nothing (§3.1)");
    CHK_TRUE(reg_boot());
    prod_reset();
    id = reg_new("t", OWNER_A, TERM_VT, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    bytes_before = fp.live_bytes;
    CHK_TRUE(bytes_before > 0);

    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    CHK_INT(reg_state(id), TERM_SLOT_DYING);
    CHK_INT(fp.frees, 0);
    CHK_INT((int)fp.live_bytes, (int)bytes_before);

    t_case("stage 1 never waits on a latch — no signal is created (§3.1)");
    CHK_INT(fp.signal_waits, 0);

    t_case("new ingest is refused with TERM_ERR_DYING");
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"x", 1, NULL),
            TERM_ERR_DYING);
    CHK_INT(term_registry_log(id, OWNER_A, "x", 1), TERM_ERR_DYING);

    t_case("and the reads/writes that are not ingest are not TERM_OK either");
    {
        char out[64];
        term_read_result_t res;
        memset(&res, 0, sizeof res);
        CHK_TRUE(term_registry_resize(id, OWNER_A, 30, 5) != TERM_OK);
        CHK_TRUE(term_registry_snapshot(id, OWNER_A, out, sizeof out, NULL) != TERM_OK);
        CHK_TRUE(term_registry_read(id, OWNER_A, 0, 2, out, sizeof out, &res) != TERM_OK);
    }

    t_case("the reaper completes it and the block comes back exactly once");
    reg_settle();
    CHK_INT(reg_state(id), -1);
    CHK_INT(fp.frees, 1);
    CHK_INT((int)fp.live_bytes, 0);
    CHK_INT(reg_stats().freed, 1);
    CHK_INT(reg_stats().zombies, 0);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Producers                                                             */
/* ===================================================================== */

static void case_producer_spsc(void)
{
    term_id_t id;

    t_case("binding a producer makes feed() TERM_ERR_BUSY (§5 SPSC)");
    CHK_TRUE(reg_boot());
    prod_reset();
    id = reg_new("p", OWNER_A, TERM_VT, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"a", 1, NULL), TERM_OK);

    CHK_INT(term_registry_producer_bind(id, OWNER_A, prod_detach, &prod), TERM_OK);
    CHK_TRUE(reg_info(id).piped);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"b", 1, NULL),
            TERM_ERR_BUSY);

    t_case("a second producer is refused: there is exactly one writer");
    CHK_INT(term_registry_producer_bind(id, OWNER_A, prod_detach, &prod),
            TERM_ERR_BUSY);
    CHK_INT(prod.detaches, 0);

    t_case("bind is owner-gated like everything else (I3)");
    CHK_INT(term_registry_producer_bind(id, OWNER_B, prod_detach, &prod),
            TERM_ERR_NOT_OWNER);

    t_case("unbind asks the producer to detach and leaves an ack outstanding");
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(prod.detaches, 1);
    CHK_INT(prod.last_id, id);
    CHK_TRUE(reg_info(id).pending_acks > 0);
    CHK_INT(reg_state(id), TERM_SLOT_LIVE);   /* unbind is not a close */

    t_case("the detach callback runs with the table lock NOT held (header)");
    CHK_INT(prod.lock_depth_seen, 0);

    t_case("re-binding before the ack is refused (§5: ack join first)");
    CHK_INT(term_registry_producer_bind(id, OWNER_A, prod_detach, &prod),
            TERM_ERR_BUSY);

    t_case("after the ack the term takes a producer, or feed, again");
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT((int)reg_info(id).pending_acks, 0);
    CHK_TRUE(!reg_info(id).piped);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"c", 1, NULL), TERM_OK);
    CHK_INT(term_registry_producer_bind(id, OWNER_A, prod_detach, &prod), TERM_OK);

    t_case("a producer that acks its teardown leaves nothing behind");
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    CHK_INT(prod.detaches, 2);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    reg_settle();
    CHK_INT(reg_shutdown(), 0);
    CHK_INT(fp.allocs, fp.frees);
}

/* ===================================================================== */
/* Stage 2: the deadline and the zombie                                  */
/* ===================================================================== */

static void case_deadline_makes_a_zombie(void)
{
    term_registry_config_t c = cfg_quiesce(1000);
    term_id_t id;
    size_t held;
    term_registry_stats_t st;
    char name[TERM_NAME_MAX];
    int i;

    t_case("a producer that never acks turns DYING into ZOMBIE at the cap");
    CHK_TRUE(reg_boot_cfg(&c));
    prod_reset();
    id = reg_new("hung", OWNER_A, TERM_VT, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_producer_bind(id, OWNER_A, prod_detach, &prod), TERM_OK);
    held = fp.live_bytes;

    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    CHK_INT(prod.detaches, 1);
    CHK_INT(prod.lock_depth_seen, 0);
    CHK_INT(reg_state(id), TERM_SLOT_DYING);

    t_case("before the deadline the reaper leaves it alone");
    reg_frame();
    term_registry_reap();
    CHK_INT(reg_state(id), TERM_SLOT_DYING);
    fp_advance(999);
    reg_frame();
    CHK_INT(term_registry_reap(), 0);
    CHK_INT(reg_state(id), TERM_SLOT_DYING);
    CHK_INT(fp.frees, 0);

    t_case("past the deadline it becomes a ZOMBIE — and keeps its memory");
    fp_advance(2);
    reg_frame();
    term_registry_reap();
    CHK_INT(reg_state(id), TERM_SLOT_ZOMBIE);
    CHK_INT(fp.frees, 0);
    CHK_INT((int)fp.live_bytes, (int)held);
    st = reg_stats();
    CHK_TRUE(st.zombies >= 1);
    CHK_INT((int)st.zombie_bytes, (int)held);
    CHK_INT(st.freed, 0);

    t_case("the zombie slot is never reused (§3.1: a leak, not a UAF)");
    for (i = 0; i < TERM_SLOT_COUNT - 1; i++) {
        sprintf(name, "z%d", i);
        CHK_TRUE(reg_new(name, OWNER_B, TERM_LOG, false, 20, 4) != TERM_ID_INVALID);
    }
    CHK_TRUE(reg_new("one_too_many", OWNER_B, TERM_LOG, false, 20, 4)
             == TERM_ID_INVALID);
    reg_settle();
    CHK_INT(reg_state(id), TERM_SLOT_ZOMBIE);

    t_case("a stale write to the zombie's id is still refused");
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"x", 1, NULL),
            TERM_ERR_DYING);

    t_case("a late ack collects the zombie (\"reaper が後から回収する\")");
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    reg_settle();
    CHK_INT(reg_state(id), -1);
    CHK_INT(fp.frees, 1);
    st = reg_stats();
    CHK_INT((int)st.zombie_bytes, 0);
    CHK_TRUE(st.freed >= 1);

    t_case("...and only then does the slot come back into service");
    CHK_TRUE(reg_new("one_too_many", OWNER_B, TERM_LOG, false, 20, 4)
             != TERM_ID_INVALID);

    CHK_INT(reg_shutdown(), 0);
}

static void case_ack_order_does_not_matter(void)
{
    term_registry_config_t c = cfg_quiesce(1000);
    term_id_t id;

    t_case("ack before the drain: the slot still frees cleanly");
    CHK_TRUE(reg_boot_cfg(&c));
    prod_reset();
    id = reg_new("a", OWNER_A, TERM_VT, false, 20, 4);
    CHK_INT(term_registry_producer_bind(id, OWNER_A, prod_detach, &prod), TERM_OK);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    reg_settle();
    CHK_INT(reg_state(id), -1);
    CHK_INT(fp.frees, 1);
    CHK_INT(reg_stats().zombies, 0);

    t_case("drain before the ack: same outcome, no zombie");
    prod_reset();
    id = reg_new("b", OWNER_A, TERM_VT, false, 20, 4);
    CHK_INT(term_registry_producer_bind(id, OWNER_A, prod_detach, &prod), TERM_OK);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    reg_frame();
    term_registry_reap();
    CHK_INT(reg_state(id), TERM_SLOT_DYING);      /* still owed the ack */
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    reg_settle();
    CHK_INT(reg_state(id), -1);
    CHK_INT(fp.frees, 2);
    CHK_INT(reg_stats().zombies, 0);

    CHK_INT(reg_shutdown(), 0);
}

static void case_stale_ack_is_dropped_and_counted(void)
{
    term_registry_config_t c = cfg_quiesce(1000);
    term_id_t old_id, other, reborn;
    term_id_t ids[TERM_SLOT_COUNT];
    char name[TERM_NAME_MAX];
    int i, slot;

    t_case("an ack for an id that has moved on is dropped and counted");
    CHK_TRUE(reg_boot_cfg(&c));
    prod_reset();

    /* Occupy every slot so the rebirth is forced back into the same one. */
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        sprintf(name, "s%d", i);
        ids[i] = reg_new(name, OWNER_A, TERM_VT, false, 20, 4);
        CHK_TRUE(ids[i] != TERM_ID_INVALID);
    }
    old_id = ids[5];
    slot = term_id_slot(old_id);
    other = ids[1];

    CHK_INT(term_registry_producer_bind(old_id, OWNER_A, prod_detach, &prod), TERM_OK);
    CHK_INT(term_registry_close(old_id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(old_id), TERM_OK);   /* honest ack */
    reg_settle();

    reborn = reg_new("reborn", OWNER_B, TERM_LOG, false, 20, 4);
    CHK_TRUE(reborn != TERM_ID_INVALID);
    CHK_INT(term_id_slot(reborn), slot);
    CHK_TRUE(reborn != old_id);

    t_case("the duplicate ack does not disturb the new tenant (I2)");
    CHK_TRUE(term_registry_producer_ack(old_id) != TERM_OK ||
             reg_stats().stale_acks > 0);
    CHK_TRUE(reg_stats().stale_acks >= 1);
    CHK_INT(reg_state(reborn), TERM_SLOT_LIVE);
    CHK_INT((int)reg_info(reborn).pending_acks, 0);
    CHK_TRUE(!reg_info(reborn).piped);
    CHK_INT(reg_state(other), TERM_SLOT_LIVE);

    t_case("nothing was freed by the stale ack");
    reg_settle();
    CHK_INT(reg_state(reborn), TERM_SLOT_LIVE);

    CHK_INT(reg_shutdown(), 0);
}

static void case_deinit_reports_uncollectable(void)
{
    term_registry_config_t c = cfg_quiesce(1000);
    term_id_t id;
    int left;

    t_case("deinit reports the slots it could not collect (header)");
    CHK_TRUE(reg_boot_cfg(&c));
    prod_reset();
    id = reg_new("hung", OWNER_A, TERM_VT, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_producer_bind(id, OWNER_A, prod_detach, &prod), TERM_OK);
    CHK_TRUE(reg_new("clean", OWNER_B, TERM_LOG, false, 20, 4) != TERM_ID_INVALID);

    left = reg_shutdown();
    CHK_INT(left, 1);
    CHK_INT(fp.frees, 1);                 /* the clean one came back      */
    CHK_TRUE(fp.live_bytes > 0);          /* the hung one is retained     */
    CHK_INT(prod.detaches, 1);

    /* The retention is deliberate; release it so the run ends leak-clean. */
    fp_reclaim_all();
}

static void case_no_recursive_locking(void)
{
    term_id_t id;

    t_case("nothing in the protocol takes the table lock twice (term_port.h)");
    CHK_TRUE(reg_boot());
    prod_reset();
    id = reg_new("t", OWNER_A, TERM_VT, false, 20, 4);
    CHK_INT(term_registry_producer_bind(id, OWNER_A, prod_detach, &prod), TERM_OK);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"x", 1, NULL),
            TERM_ERR_BUSY);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    reg_settle();
    CHK_INT(fp.recursive_attempts, 0);
    CHK_INT(fp.depth, 0);
    CHK_INT(fp.lock_ok, fp.unlocks);

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("registry_quiesce");
    case_stage1_does_not_free_and_does_not_join();
    case_producer_spsc();
    case_deadline_makes_a_zombie();
    case_ack_order_does_not_matter();
    case_stale_ack_is_dropped_and_counted();
    case_no_recursive_locking();
    case_deinit_reports_uncollectable();
    fp_reclaim_all();
    return t_summary();
}
