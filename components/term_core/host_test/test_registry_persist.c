/*
 * test_registry_persist.c — the tmux model: detach, re-attach, quota, LRU.
 *
 * Contracts under test (§3.1, term_registry.h create/owner_stopped):
 *   "非 persist の term は app-manager の teardown フックで自動解放。
 *    persist は生存し、同一 owner のみ 同名 create で再アタッチできる
 *    (別アプリが同名 create で他人のセッションを読む穴を塞ぐ)。
 *    persist スロット上限は初期値 4"
 *   "(c) persist 枠が満杯で新規 create が来たら 最も長くデタッチされて
 *    いる persist を evict(LRU)"
 *   create doc: re-attach "Its id — generation included — is returned
 *    unchanged"; "mode is NOT changed and a mismatch is TERM_ERR_MODE";
 *    "If every persist term is LIVE, nothing is evicted and the create
 *    fails."
 */
#include "reg_util.h"

static term_registry_config_t cfg_persist(int persist_max)
{
    term_registry_config_t c;
    memset(&c, 0, sizeof c);
    c.persist_max = persist_max;
    return c;
}

/* ===================================================================== */
/* teardown hook                                                         */
/* ===================================================================== */

static void case_owner_stopped_splits_by_persist(void)
{
    term_id_t vol, keep;
    term_slot_info_t inf;

    t_case("teardown: non-persist -> DYING, persist -> DETACHED (§3.1)");
    CHK_TRUE(reg_boot());
    vol = reg_new("scratch", OWNER_A, TERM_LOG, false, 20, 4);
    keep = reg_new("sess", OWNER_A, TERM_VT, true, 20, 4);
    CHK_TRUE(vol != TERM_ID_INVALID && keep != TERM_ID_INVALID);

    fp_advance(5000);
    CHK_INT(term_registry_owner_stopped(OWNER_A), 2);
    CHK_INT(reg_state(vol), TERM_SLOT_DYING);
    CHK_INT(reg_state(keep), TERM_SLOT_DETACHED);

    t_case("the detach time is stamped from the port clock (LRU key)");
    inf = reg_info(keep);
    CHK_INT((int)inf.detached_since_ms, (int)fp.now_ms);

    t_case("the non-persist slot comes back after a reaper pass");
    reg_settle();
    CHK_INT(reg_state(vol), -1);
    CHK_INT(reg_state(keep), TERM_SLOT_DETACHED);
    CHK_INT(fp.frees, 1);
    CHK_TRUE(fp.live_bytes > 0);          /* the persist term still holds */

    /* §3.1 calls a DETACHED term "alive, still ingesting from platform
     * producers"; whether an app-side feed is accepted before the re-attach
     * is not fixed by the design, so only the invariants are asserted:
     * the call answers (I5), it is not mistaken for a DYING refusal, and
     * the slot stays DETACHED either way. */
    t_case("a DETACHED term is alive: a feed is answered, the slot is kept");
    CHK_TRUE(term_registry_feed(keep, OWNER_A, (const uint8_t *)"x", 1, NULL)
             != TERM_ERR_DYING);
    CHK_INT(reg_state(keep), TERM_SLOT_DETACHED);
    CHK_INT(fp.allocs, 2);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* re-attach                                                             */
/* ===================================================================== */

static void case_reattach_same_owner(void)
{
    term_id_t id, again;
    term_create_opts_t o;
    bool re = false;
    term_registry_stats_t st;
    uint32_t i;

    t_case("a persist term is picked back up by the same owner+name (§3.1)");
    CHK_TRUE(reg_boot());
    id = reg_new("sess", OWNER_A, TERM_LOG, true, 20, 3);
    CHK_TRUE(id != TERM_ID_INVALID);
    for (i = 0; i < 12; i++) {
        char line[32];
        int n = sprintf(line, "L%u", i);
        CHK_INT(term_registry_log(id, OWNER_A, line, (size_t)n), TERM_OK);
    }
    reg_frame();
    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);
    CHK_INT(fp.allocs, 1);

    o = reg_opts("sess", OWNER_A, TERM_LOG, true, 20, 3);
    again = TERM_ID_INVALID;
    CHK_INT(term_registry_create(&o, &again, &re), TERM_OK);

    t_case("the id — generation included — comes back unchanged");
    CHK_INT(again, id);
    CHK_TRUE(re);
    CHK_INT(reg_state(id), TERM_SLOT_LIVE);
    CHK_INT((int)reg_info(id).detached_since_ms, 0);

    t_case("re-attach allocates nothing: it is the same block (I4)");
    CHK_INT(fp.allocs, 1);
    CHK_INT(fp.frees, 0);

    t_case("the session continued: the scrollback survived");
    st = reg_stats();
    CHK_INT(st.reattaches, 1);
    CHK_INT(st.creates, 1);
    {
        char out[1024];
        term_read_result_t res;
        memset(&res, 0, sizeof res);
        memset(out, 0, sizeof out);
        CHK_INT(term_registry_read(id, OWNER_A, 0, 3, out, sizeof out, &res), TERM_OK);
        CHK_INT(res.lines, 3);
        CHK_STR(out, "L0\nL1\nL2");
    }

    CHK_INT(reg_shutdown(), 0);
}

static void case_reattach_is_owner_scoped(void)
{
    term_id_t id, other;
    term_create_opts_t o;
    bool re = true;

    t_case("another app asking for the same name gets its OWN term (§3.1)");
    CHK_TRUE(reg_boot());
    id = reg_new("sess", OWNER_A, TERM_LOG, true, 20, 3);
    CHK_INT(term_registry_log(id, OWNER_A, "SECRET", 6), TERM_OK);
    reg_frame();
    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);

    o = reg_opts("sess", OWNER_B, TERM_LOG, true, 20, 3);
    other = TERM_ID_INVALID;
    CHK_INT(term_registry_create(&o, &other, &re), TERM_OK);
    CHK_TRUE(other != id);
    CHK_TRUE(!re);
    CHK_INT(fp.allocs, 2);

    t_case("...and sees none of the first owner's history");
    CHK_TRUE(strstr(reg_snap(other, OWNER_B), "SECRET") == NULL);
    {
        char out[512];
        term_read_result_t res;
        memset(out, 0, sizeof out);
        memset(&res, 0, sizeof res);
        CHK_INT(term_registry_read(other, OWNER_B, 0, 8, out, sizeof out, &res), TERM_OK);
        CHK_TRUE(strstr(out, "SECRET") == NULL);
    }

    t_case("the original is still DETACHED and still its owner's");
    CHK_INT(reg_state(id), TERM_SLOT_DETACHED);

    CHK_INT(reg_shutdown(), 0);
}

static void case_reattach_mode_and_geometry(void)
{
    term_id_t id, got;
    term_create_opts_t o;
    bool re = false;

    t_case("a mode mismatch on re-attach is TERM_ERR_MODE (header)");
    CHK_TRUE(reg_boot());
    id = reg_new("sess", OWNER_A, TERM_VT, true, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);

    o = reg_opts("sess", OWNER_A, TERM_LOG, true, 20, 4);
    got = TERM_ID_INVALID;
    CHK_INT(term_registry_create(&o, &got, &re), TERM_ERR_MODE);
    CHK_INT(got, TERM_ID_INVALID);
    CHK_INT(reg_state(id), TERM_SLOT_DETACHED);
    CHK_INT(reg_info(id).mode, TERM_VT);

    t_case("the requested geometry is applied as a resize on re-attach");
    o = reg_opts("sess", OWNER_A, TERM_VT, true, 40, 8);
    CHK_INT(term_registry_create(&o, &got, &re), TERM_OK);
    CHK_INT(got, id);
    CHK_TRUE(re);
    reg_frame();                      /* resize is posted to the UI (§5) */
    CHK_INT(reg_info(id).cols, 40);
    CHK_INT(reg_info(id).rows, 8);
    CHK_INT(fp.allocs, 1);            /* still no second allocation      */

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* quota and LRU                                                         */
/* ===================================================================== */

static void case_persist_quota(void)
{
    term_registry_config_t c = cfg_persist(0);     /* 0 -> the default 4 */
    term_id_t ids[4];
    char name[TERM_NAME_MAX];
    int i;

    t_case("the persist quota defaults to 4 (§3.1)");
    CHK_TRUE(reg_boot_cfg(&c));
    for (i = 0; i < TERM_PERSIST_MAX_DEFAULT; i++) {
        sprintf(name, "p%d", i);
        ids[i] = reg_new(name, OWNER_A, TERM_LOG, true, 20, 4);
        CHK_TRUE(ids[i] != TERM_ID_INVALID);
    }

    t_case("with every persist term LIVE nothing is evicted, create fails");
    {
        term_create_opts_t o = reg_opts("p4", OWNER_A, TERM_LOG, true, 20, 4);
        term_id_t id = TERM_ID_INVALID;
        CHK_INT(term_registry_create(&o, &id, NULL), TERM_ERR_NO_SLOT);
        CHK_INT(id, TERM_ID_INVALID);
        CHK_INT(reg_stats().evicted, 0);
    }

    t_case("non-persist terms do not consume the persist quota");
    CHK_TRUE(reg_new("v0", OWNER_A, TERM_LOG, false, 20, 4) != TERM_ID_INVALID);
    CHK_TRUE(reg_new("v1", OWNER_A, TERM_LOG, false, 20, 4) != TERM_ID_INVALID);
    CHK_INT(reg_live_count(), TERM_PERSIST_MAX_DEFAULT + 2);

    t_case("closing a persist term returns its quota after the reaper");
    CHK_INT(term_registry_close(ids[0], OWNER_A), TERM_OK);
    reg_settle();
    CHK_TRUE(reg_new("p4", OWNER_A, TERM_LOG, true, 20, 4) != TERM_ID_INVALID);

    CHK_INT(reg_shutdown(), 0);
}

static void case_persist_quota_configurable(void)
{
    term_registry_config_t c = cfg_persist(2);

    t_case("persist_max is configurable (§12 leaves the number open)");
    CHK_TRUE(reg_boot_cfg(&c));
    CHK_TRUE(reg_new("p0", OWNER_A, TERM_LOG, true, 20, 4) != TERM_ID_INVALID);
    CHK_TRUE(reg_new("p1", OWNER_A, TERM_LOG, true, 20, 4) != TERM_ID_INVALID);
    CHK_TRUE(reg_new("p2", OWNER_A, TERM_LOG, true, 20, 4) == TERM_ID_INVALID);
    CHK_TRUE(reg_new("v0", OWNER_A, TERM_LOG, false, 20, 4) != TERM_ID_INVALID);
    CHK_INT(reg_shutdown(), 0);
}

static void case_lru_evicts_the_longest_detached(void)
{
    term_registry_config_t c = cfg_persist(0);
    term_id_t p[4];
    const char *owners[4];
    term_id_t fresh;
    term_create_opts_t o;
    int i;

    t_case("LRU: the persist term detached longest ago is the one evicted");
    CHK_TRUE(reg_boot_cfg(&c));
    owners[0] = "own_one"; owners[1] = "own_two";
    owners[2] = "own_three"; owners[3] = "own_four";

    for (i = 0; i < 4; i++) {
        p[i] = reg_new("sess", owners[i], TERM_LOG, true, 20, 4);
        CHK_TRUE(p[i] != TERM_ID_INVALID);
    }
    /* Detach them in order, a second apart, so "longest detached" is p[0]
     * and is NOT the same as "lowest slot" or "created first" by accident:
     * detach 1, 0, 3, 2 and let the clock decide. */
    fp_advance(1000); CHK_INT(term_registry_owner_stopped(owners[1]), 1);
    fp_advance(1000); CHK_INT(term_registry_owner_stopped(owners[0]), 1);
    fp_advance(1000); CHK_INT(term_registry_owner_stopped(owners[3]), 1);
    fp_advance(1000); CHK_INT(term_registry_owner_stopped(owners[2]), 1);
    for (i = 0; i < 4; i++) CHK_INT(reg_state(p[i]), TERM_SLOT_DETACHED);

    o = reg_opts("newcomer", OWNER_A, TERM_LOG, true, 20, 4);
    fresh = TERM_ID_INVALID;
    CHK_INT(term_registry_create(&o, &fresh, NULL), TERM_OK);
    CHK_TRUE(fresh != TERM_ID_INVALID);

    t_case("exactly one term was evicted, and it is owners[1]'s");
    CHK_INT(reg_stats().evicted, 1);
    CHK_TRUE(reg_state(p[1]) == TERM_SLOT_DYING || reg_state(p[1]) == -1);
    CHK_INT(reg_state(p[0]), TERM_SLOT_DETACHED);
    CHK_INT(reg_state(p[2]), TERM_SLOT_DETACHED);
    CHK_INT(reg_state(p[3]), TERM_SLOT_DETACHED);

    t_case("the evicted key is gone from its owner's namespace");
    reg_settle();
    {
        term_id_t got = TERM_ID_INVALID;
        CHK_INT(term_registry_find(owners[1], "sess", &got), TERM_ERR_BAD_ID);
        CHK_INT(term_registry_find(owners[0], "sess", &got), TERM_OK);
        CHK_INT(got, p[0]);
    }

    t_case("the next eviction takes the new oldest (owners[0])");
    o = reg_opts("newcomer2", OWNER_B, TERM_LOG, true, 20, 4);
    CHK_INT(term_registry_create(&o, &fresh, NULL), TERM_OK);
    CHK_INT(reg_stats().evicted, 2);
    CHK_TRUE(reg_state(p[0]) == TERM_SLOT_DYING || reg_state(p[0]) == -1);
    CHK_INT(reg_state(p[2]), TERM_SLOT_DETACHED);

    CHK_INT(reg_shutdown(), 0);
}

static void case_lru_does_not_touch_live_persist(void)
{
    term_registry_config_t c = cfg_persist(2);
    term_id_t live, detached, fresh;
    term_create_opts_t o;

    t_case("a LIVE persist term is never the eviction victim (§3.1 (c))");
    CHK_TRUE(reg_boot_cfg(&c));
    live = reg_new("hot", OWNER_A, TERM_LOG, true, 20, 4);
    detached = reg_new("cold", OWNER_B, TERM_LOG, true, 20, 4);
    CHK_TRUE(live != TERM_ID_INVALID && detached != TERM_ID_INVALID);
    fp_advance(2000);
    CHK_INT(term_registry_owner_stopped(OWNER_B), 1);

    o = reg_opts("third", "own_three", TERM_LOG, true, 20, 4);
    fresh = TERM_ID_INVALID;
    CHK_INT(term_registry_create(&o, &fresh, NULL), TERM_OK);
    CHK_INT(reg_stats().evicted, 1);
    CHK_INT(reg_state(live), TERM_SLOT_LIVE);
    CHK_TRUE(reg_state(detached) == TERM_SLOT_DYING || reg_state(detached) == -1);

    CHK_INT(reg_shutdown(), 0);
}

static void case_dying_and_zombie_slots_stay_occupied(void)
{
    term_id_t ids[TERM_SLOT_COUNT];
    char name[TERM_NAME_MAX];
    int i;

    t_case("DYING slots count as occupied until the reaper frees them");
    CHK_TRUE(reg_boot());
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        sprintf(name, "s%d", i);
        ids[i] = reg_new(name, OWNER_A, TERM_LOG, false, 20, 4);
        CHK_TRUE(ids[i] != TERM_ID_INVALID);
    }
    CHK_INT(term_registry_close(ids[2], OWNER_A), TERM_OK);
    CHK_INT(reg_state(ids[2]), TERM_SLOT_DYING);
    CHK_TRUE(reg_new("late", OWNER_A, TERM_LOG, false, 20, 4) == TERM_ID_INVALID);
    CHK_INT(reg_live_count(), TERM_SLOT_COUNT);

    t_case("...and the slot becomes usable once it is actually free");
    reg_settle();
    CHK_INT(reg_live_count(), TERM_SLOT_COUNT - 1);
    CHK_TRUE(reg_new("late", OWNER_A, TERM_LOG, false, 20, 4) != TERM_ID_INVALID);

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("registry_persist");
    case_owner_stopped_splits_by_persist();
    case_reattach_same_owner();
    case_reattach_is_owner_scoped();
    case_reattach_mode_and_geometry();
    case_persist_quota();
    case_persist_quota_configurable();
    case_lru_evicts_the_longest_detached();
    case_lru_does_not_touch_live_persist();
    case_dying_and_zombie_slots_stay_occupied();
    fp_reclaim_all();
    return t_summary();
}
