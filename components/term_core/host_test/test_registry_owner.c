/*
 * test_registry_owner.c — the owner gate and the per-owner name namespace.
 *
 * Contracts under test (§3.1, §7.2, term_registry.h I3):
 *   "全 API が … owner 一致 を検証し、不一致は即エラー。これで 他アプリが
 *    id を総当たりして書き込む経路 … が塞がる"
 *   "name は owner ごとの名前空間(レジストリのキーは owner+name)なので、
 *    他アプリの同名 create が先取りで枠を塞ぐ squatting は構造的に起きない"
 *   §7.2 access table: "他アプリ — API 自体が無い"
 *   term_registry.h: names and owners longer than their field are
 *   "rejected, never truncated"; close_forced has "No owner check".
 */
#include "reg_util.h"

static void case_gate_rejects_a_foreign_owner(void)
{
    term_id_t id;
    term_registry_stats_t before, after;

    t_case("every entry point refuses a foreign owner (I3)");
    CHK_TRUE(reg_boot());
    id = reg_new("mine", OWNER_A, TERM_VT, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"own", 3, NULL), TERM_OK);
    reg_frame();

    before = reg_stats();
    reg_expect_all_gated(id, OWNER_B, TERM_ERR_NOT_OWNER);
    after = reg_stats();

    t_case("the refusals are counted (stats.denied_owner)");
    CHK_TRUE(after.denied_owner - before.denied_owner >= 9);

    t_case("the term survived the attack untouched");
    CHK_INT(reg_state(id), TERM_SLOT_LIVE);
    CHK_INT((int)reg_info(id).bytes_in, 3);
    CHK_INT(reg_info(id).cols, 20);
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "own") != NULL);

    t_case("a foreign owner gets no content out of snapshot/read (§7.2)");
    {
        char out[512];
        term_read_result_t res;
        memset(out, 0x7e, sizeof out);
        memset(&res, 0, sizeof res);
        CHK_INT(term_registry_snapshot(id, OWNER_B, out, sizeof out, NULL),
                TERM_ERR_NOT_OWNER);
        CHK_INT(term_registry_read(id, OWNER_B, 0, 4, out, sizeof out, &res),
                TERM_ERR_NOT_OWNER);
        CHK_TRUE(strstr(out, "own") == NULL);
    }

    CHK_INT(reg_shutdown(), 0);
}

static void case_owner_string_validation(void)
{
    char long_owner[TERM_OWNER_MAX + 8];
    char max_owner[TERM_OWNER_MAX];
    char long_name[TERM_NAME_MAX + 8];
    char max_name[TERM_NAME_MAX];
    term_create_opts_t o;
    term_id_t id = TERM_ID_INVALID;

    t_case("NULL / empty name and owner are TERM_ERR_INVAL");
    CHK_TRUE(reg_boot());
    o = reg_opts(NULL, OWNER_A, TERM_LOG, false, 20, 4);
    CHK_INT(term_registry_create(&o, &id, NULL), TERM_ERR_INVAL);
    o = reg_opts("n", NULL, TERM_LOG, false, 20, 4);
    CHK_INT(term_registry_create(&o, &id, NULL), TERM_ERR_INVAL);
    o = reg_opts("", OWNER_A, TERM_LOG, false, 20, 4);
    CHK_INT(term_registry_create(&o, &id, NULL), TERM_ERR_INVAL);
    o = reg_opts("n", "", TERM_LOG, false, 20, 4);
    CHK_INT(term_registry_create(&o, &id, NULL), TERM_ERR_INVAL);
    CHK_INT(term_registry_create(NULL, &id, NULL), TERM_ERR_INVAL);
    CHK_INT(fp.allocs, 0);

    t_case("a name/owner that exactly fills its field (minus NUL) is legal");
    memset(max_name, 'n', sizeof max_name); max_name[TERM_NAME_MAX - 1] = 0;
    memset(max_owner, 'o', sizeof max_owner); max_owner[TERM_OWNER_MAX - 1] = 0;
    o = reg_opts(max_name, max_owner, TERM_LOG, false, 20, 4);
    CHK_INT(term_registry_create(&o, &id, NULL), TERM_OK);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_STR(reg_info(id).name, max_name);
    CHK_STR(reg_info(id).owner, max_owner);

    t_case("longer than the field is REJECTED, never truncated (manifest 1)");
    memset(long_owner, 'o', sizeof long_owner);
    long_owner[TERM_OWNER_MAX] = 0;            /* one over the usable length */
    memset(long_name, 'n', sizeof long_name);
    long_name[TERM_NAME_MAX] = 0;

    o = reg_opts("ok", long_owner, TERM_LOG, false, 20, 4);
    CHK_INT(term_registry_create(&o, &id, NULL), TERM_ERR_INVAL);
    o = reg_opts(long_name, OWNER_A, TERM_LOG, false, 20, 4);
    CHK_INT(term_registry_create(&o, &id, NULL), TERM_ERR_INVAL);

    t_case("...so a truncating registry cannot make two owners into one");
    /* If owners were truncated to the field, long_owner and max_owner would
     * collide and this would find the first term instead of failing. Which
     * error it is (INVAL for the over-long string, BAD_ID for "no such
     * key") the design does not fix; that it is not a hit, it does. */
    CHK_TRUE(term_registry_find(long_owner, max_name, &id) != TERM_OK);

    t_case("a foreign owner longer than the field is still refused");
    {
        term_id_t victim = TERM_ID_INVALID;
        CHK_INT(term_registry_find(max_owner, max_name, &victim), TERM_OK);
        CHK_TRUE(term_registry_feed(victim, long_owner, (const uint8_t *)"x", 1, NULL)
                 != TERM_OK);
        CHK_INT(term_registry_feed(victim, NULL, (const uint8_t *)"x", 1, NULL),
                TERM_ERR_INVAL);
        CHK_INT((int)reg_info(victim).bytes_in, 0);
    }

    CHK_INT(reg_shutdown(), 0);
}

static void case_namespace_is_per_owner(void)
{
    term_id_t a, b, got;

    t_case("two apps may both hold a term called \"ssh0\" (§3.1 squatting)");
    CHK_TRUE(reg_boot());
    a = reg_new("ssh0", OWNER_A, TERM_VT, true, 20, 4);
    b = reg_new("ssh0", OWNER_B, TERM_VT, true, 20, 4);
    CHK_TRUE(a != TERM_ID_INVALID);
    CHK_TRUE(b != TERM_ID_INVALID);
    CHK_TRUE(a != b);
    CHK_TRUE(term_id_slot(a) != term_id_slot(b));

    t_case("each app finds only its own");
    got = TERM_ID_INVALID;
    CHK_INT(term_registry_find(OWNER_A, "ssh0", &got), TERM_OK);
    CHK_INT(got, a);
    CHK_INT(term_registry_find(OWNER_B, "ssh0", &got), TERM_OK);
    CHK_INT(got, b);

    t_case("writes stay in their own namespace");
    CHK_INT(term_registry_log(a, OWNER_A, "AAA", 3), TERM_OK);
    CHK_INT(term_registry_log(b, OWNER_B, "BBB", 3), TERM_OK);
    reg_frame();
    CHK_TRUE(strstr(reg_snap(a, OWNER_A), "AAA") != NULL);
    CHK_TRUE(strstr(reg_snap(a, OWNER_A), "BBB") == NULL);
    CHK_TRUE(strstr(reg_snap(b, OWNER_B), "BBB") != NULL);
    CHK_TRUE(strstr(reg_snap(b, OWNER_B), "AAA") == NULL);

    t_case("a same-key create by the same owner while LIVE is TERM_ERR_EXISTS");
    {
        term_create_opts_t o = reg_opts("ssh0", OWNER_A, TERM_VT, true, 20, 4);
        term_id_t id = TERM_ID_INVALID;
        bool re = true;
        CHK_INT(term_registry_create(&o, &id, &re), TERM_ERR_EXISTS);
        CHK_INT(id, TERM_ID_INVALID);
    }

    t_case("owner_stopped only touches that owner's terms");
    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);
    CHK_INT(reg_state(a), TERM_SLOT_DETACHED);
    CHK_INT(reg_state(b), TERM_SLOT_LIVE);
    CHK_INT(term_registry_owner_stopped("nobody_here"), 0);

    CHK_INT(reg_shutdown(), 0);
}

static void case_system_owner_is_reserved(void)
{
    term_id_t sys_id, app_id;

    t_case("TERM_OWNER_SYSTEM is not a name an app could present");
    CHK_TRUE(TERM_OWNER_SYSTEM[0] < 0x20);   /* control byte, §: reserved */

    t_case("a platform term and an app term with the same name coexist");
    CHK_TRUE(reg_boot());
    sys_id = reg_new("console", TERM_OWNER_SYSTEM, TERM_LOG, true, 20, 4);
    app_id = reg_new("console", OWNER_A, TERM_LOG, false, 20, 4);
    CHK_TRUE(sys_id != TERM_ID_INVALID);
    CHK_TRUE(app_id != TERM_ID_INVALID);
    CHK_TRUE(sys_id != app_id);

    t_case("an app cannot reach the platform's term by claiming its owner");
    CHK_INT(term_registry_feed(sys_id, OWNER_A, (const uint8_t *)"x", 1, NULL),
            TERM_ERR_NOT_OWNER);

    CHK_INT(reg_shutdown(), 0);
}

static void case_forced_close_has_no_owner_check(void)
{
    term_id_t id;

    t_case("close_forced reclaims a slot regardless of owner (§3.1 (b))");
    CHK_TRUE(reg_boot());
    id = reg_new("stuck", OWNER_A, TERM_VT, true, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_close(id, OWNER_B), TERM_ERR_NOT_OWNER);
    CHK_INT(reg_state(id), TERM_SLOT_LIVE);

    CHK_INT(term_registry_close_forced(id), TERM_OK);
    CHK_INT(reg_state(id), TERM_SLOT_DYING);
    reg_settle();
    CHK_INT(reg_state(id), -1);
    CHK_INT(fp.frees, 1);

    CHK_INT(reg_shutdown(), 0);
}

static void case_list_and_info_are_platform_only(void)
{
    term_slot_info_t v[TERM_SLOT_COUNT];
    term_id_t a, b;
    int n, i, seen_a = 0, seen_b = 0;

    t_case("list() reports non-FREE slots, capped by max (header)");
    CHK_TRUE(reg_boot());
    a = reg_new("t0", OWNER_A, TERM_LOG, false, 20, 4);
    b = reg_new("t1", OWNER_B, TERM_VT, true, 30, 5);
    CHK_TRUE(a != TERM_ID_INVALID && b != TERM_ID_INVALID);

    n = term_registry_list(v, TERM_SLOT_COUNT);
    CHK_INT(n, 2);
    for (i = 0; i < n; i++) {
        if (v[i].id == a) { seen_a = 1; CHK_STR(v[i].owner, OWNER_A); CHK_INT(v[i].mode, TERM_LOG); CHK_TRUE(!v[i].persist); }
        if (v[i].id == b) { seen_b = 1; CHK_STR(v[i].owner, OWNER_B); CHK_INT(v[i].mode, TERM_VT); CHK_TRUE(v[i].persist); }
    }
    CHK_TRUE(seen_a && seen_b);

    memset(v, 0, sizeof v);
    CHK_INT(term_registry_list(v, 1), 1);
    CHK_INT(term_registry_list(v, 0), 0);
    CHK_INT(term_registry_list(NULL, 4), 0);

    t_case("info() reports the geometry and the single block (I4)");
    {
        term_slot_info_t inf = reg_info(b);
        CHK_INT(inf.cols, 30);
        CHK_INT(inf.rows, 5);
        CHK_TRUE(inf.mem_bytes > 0);
        CHK_TRUE(!inf.piped);
        CHK_INT((int)inf.pending_acks, 0);
        CHK_INT((int)inf.detached_since_ms, 0);   /* 0 while attached */
    }

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("registry_owner");
    case_gate_rejects_a_foreign_owner();
    case_owner_string_validation();
    case_namespace_is_per_owner();
    case_system_owner_is_reserved();
    case_forced_close_has_no_owner_check();
    case_list_and_info_are_platform_only();
    fp_reclaim_all();
    return t_summary();
}
