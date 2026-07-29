/*
 * test_registry_id.c — ids, generations and the ABA guard.
 *
 * Contracts under test (§3.1, term_registry.h I2/I3/I5):
 *   "id は生スロット番号ではなく (generation << 3) | slot"
 *   "全 API が呼び出しごとに generation 一致 + owner 一致 を検証し、
 *    不一致は即エラー … 解放→再利用後に古い id で別アプリの term に
 *    書き込む ABA を塞ぐ"
 *   "close 済み/再利用済み/他アプリの id は例外ではなく エラー戻り値"
 */
#include "reg_util.h"

/* Fill every slot but one, so the next create is forced into a known slot
 * and the ABA case is deterministic rather than allocator-dependent. */
static void fill_all_but(term_id_t *out, int count, const char *owner)
{
    char name[TERM_NAME_MAX];
    int i;
    for (i = 0; i < count; i++) {
        sprintf(name, "f%d", i);
        out[i] = reg_new(name, owner, TERM_LOG, false, 20, 4);
        CHK_TRUE(out[i] != TERM_ID_INVALID);
    }
}

static void case_id_shape(void)
{
    term_id_t ids[TERM_SLOT_COUNT];
    int seen[TERM_SLOT_COUNT];
    int i, j;

    t_case("ids are positive, pack (generation << 3) | slot, generation >= 1");
    CHK_TRUE(reg_boot());
    memset(seen, 0, sizeof seen);
    fill_all_but(ids, TERM_SLOT_COUNT, OWNER_A);

    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        int slot = term_id_slot(ids[i]);
        uint32_t gen = term_id_generation(ids[i]);
        CHK_TRUE(ids[i] > 0);
        CHK_RANGE(slot, 0, TERM_SLOT_COUNT - 1);
        CHK_TRUE(gen >= 1);
        CHK_TRUE(gen <= TERM_GEN_MAX);
        CHK_INT(ids[i], (term_id_t)((gen << TERM_SLOT_BITS) | (uint32_t)slot));
        /* "a valid id is always >= TERM_SLOT_COUNT and 0 is free to mean
         * none" (term_registry.h I2) */
        CHK_TRUE(ids[i] >= TERM_SLOT_COUNT);
        CHK_TRUE(!seen[slot]);
        seen[slot] = 1;
        for (j = 0; j < i; j++) CHK_TRUE(ids[i] != ids[j]);
    }

    t_case("the ninth create has nowhere to go (fixed table of 8, I1)");
    CHK_TRUE(reg_new("f8", OWNER_A, TERM_LOG, false, 20, 4) == TERM_ID_INVALID);
    {
        term_create_opts_t o = reg_opts("f8", OWNER_A, TERM_LOG, false, 20, 4);
        term_id_t id = (term_id_t)0x5A5A;
        CHK_INT(term_registry_create(&o, &id, NULL), TERM_ERR_NO_SLOT);
        t_case("out_id is written only on TERM_OK (term_registry.h)");
        CHK_INT(id, (term_id_t)0x5A5A);
    }

    CHK_INT(reg_shutdown(), 0);
}

static void case_generation_advances(void)
{
    term_id_t ids[TERM_SLOT_COUNT];
    term_id_t old_id, new_id;
    int slot;
    uint32_t g0, g1;
    term_registry_stats_t st;

    t_case("a reused slot comes back with a strictly greater generation");
    CHK_TRUE(reg_boot());
    fill_all_but(ids, TERM_SLOT_COUNT, OWNER_A);

    old_id = ids[3];
    slot = term_id_slot(old_id);
    g0 = term_id_generation(old_id);
    CHK_INT(term_registry_close(old_id, OWNER_A), TERM_OK);
    reg_settle();
    CHK_INT(reg_state(old_id), -1);          /* the slot is FREE now       */

    /* Only one slot is free, so the next create must land in it. */
    new_id = reg_new("again", OWNER_A, TERM_LOG, false, 20, 4);
    CHK_TRUE(new_id != TERM_ID_INVALID);
    CHK_INT(term_id_slot(new_id), slot);
    g1 = term_id_generation(new_id);
    CHK_TRUE(g1 > g0);
    CHK_TRUE(new_id != old_id);

    t_case("the 28-bit generation does not wrap in a test's lifetime (I2)");
    st = reg_stats();
    CHK_INT(st.gen_wraps, 0);

    t_case("ABA: every API refuses the old id with TERM_ERR_STALE (§3.1)");
    reg_expect_all_gated(old_id, OWNER_A, TERM_ERR_STALE);
    CHK_TRUE(reg_stats().denied_stale >= 9);

    t_case("...and the new term in that slot was not touched");
    CHK_INT(reg_state(new_id), TERM_SLOT_LIVE);
    CHK_INT((int)reg_info(new_id).bytes_in, 0);
    CHK_INT(reg_info(new_id).cols, 20);

    CHK_INT(reg_shutdown(), 0);
}

static void case_free_slot_and_garbage_ids(void)
{
    term_id_t id;
    char out[128];
    term_id_t junk[6];
    size_t i;

    t_case("an id whose slot is FREE is TERM_ERR_BAD_ID, not STALE");
    CHK_TRUE(reg_boot());
    id = reg_new("t", OWNER_A, TERM_LOG, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    reg_settle();
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"x", 1, NULL),
            TERM_ERR_BAD_ID);
    CHK_INT(term_registry_info(id, NULL), TERM_ERR_INVAL);

    t_case("garbage ids are errors, never a crash and never TERM_OK (I5)");
    junk[0] = TERM_ID_INVALID;
    junk[1] = -1;
    junk[2] = (term_id_t)0x7FFFFFFF;
    junk[3] = 1;                      /* slot 1, generation 0 — never issued */
    junk[4] = (term_id_t)(TERM_SLOT_COUNT - 1);
    junk[5] = (term_id_t)0x40000000;
    for (i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        term_slot_info_t inf;
        CHK_TRUE(term_registry_feed(junk[i], OWNER_A, (const uint8_t *)"x", 1, NULL) != TERM_OK);
        CHK_TRUE(term_registry_log(junk[i], OWNER_A, "x", 1) != TERM_OK);
        CHK_TRUE(term_registry_close(junk[i], OWNER_A) != TERM_OK);
        CHK_TRUE(term_registry_close_forced(junk[i]) != TERM_OK);
        CHK_TRUE(term_registry_snapshot(junk[i], OWNER_A, out, sizeof out, NULL) != TERM_OK);
        CHK_TRUE(term_registry_info(junk[i], &inf) != TERM_OK);
        CHK_TRUE(term_registry_producer_ack(junk[i]) != TERM_OK ||
                 reg_stats().stale_acks > 0);
    }

    t_case("no allocation was made or lost on any of those false paths (I4)");
    CHK_INT(fp.allocs, fp.frees);

    CHK_INT(reg_shutdown(), 0);
}

static void case_close_is_terminal_for_the_id(void)
{
    term_id_t id;

    t_case("the id is dead to the caller the moment close returns TERM_OK");
    CHK_TRUE(reg_boot());
    id = reg_new("t", OWNER_A, TERM_VT, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"hi", 2, NULL), TERM_OK);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);

    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"x", 1, NULL),
            TERM_ERR_DYING);
    CHK_INT(term_registry_log(id, OWNER_A, "x", 1), TERM_ERR_DYING);

    t_case("closing an already-DYING term is TERM_OK and a no-op");
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    CHK_INT(reg_state(id), TERM_SLOT_DYING);
    CHK_INT(reg_stats().closes, 1);

    CHK_INT(reg_shutdown(), 0);
}

static void case_find_by_key(void)
{
    term_id_t id, got;

    t_case("find() answers for LIVE and DETACHED, not for DYING (header)");
    CHK_TRUE(reg_boot());
    id = reg_new("sess", OWNER_A, TERM_VT, true, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);

    got = TERM_ID_INVALID;
    CHK_INT(term_registry_find(OWNER_A, "sess", &got), TERM_OK);
    CHK_INT(got, id);

    CHK_INT(term_registry_find(OWNER_B, "sess", &got), TERM_ERR_BAD_ID);
    CHK_INT(term_registry_find(OWNER_A, "nope", &got), TERM_ERR_BAD_ID);
    CHK_INT(term_registry_find(NULL, "sess", &got), TERM_ERR_INVAL);
    CHK_INT(term_registry_find(OWNER_A, NULL, &got), TERM_ERR_INVAL);

    CHK_INT(term_registry_owner_stopped(OWNER_A), 1);
    CHK_INT(reg_state(id), TERM_SLOT_DETACHED);
    got = TERM_ID_INVALID;
    CHK_INT(term_registry_find(OWNER_A, "sess", &got), TERM_OK);
    CHK_INT(got, id);

    CHK_INT(term_registry_close_forced(id), TERM_OK);
    CHK_INT(term_registry_find(OWNER_A, "sess", &got), TERM_ERR_BAD_ID);

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("registry_id");
    case_id_shape();
    case_generation_advances();
    case_free_slot_and_garbage_ids();
    case_close_is_terminal_for_the_id();
    case_find_by_key();
    fp_reclaim_all();
    return t_summary();
}
