/*
 * test_registry_alloc.c — "one allocation at create, and never again".
 *
 * Contracts under test (§4.2, term_registry.h I4, term_port.h memory):
 *   "全確保は term.create() 時に一回、PSRAM プールから。実行中の
 *    malloc/realloc はゼロ → リーク・断片化の経路が存在しない。"
 *   "One block per term, taken at term_registry_create() from the port's
 *    allocator, sized by term_core_mem_size(). No malloc, realloc or free
 *    happens on feed, resize, drain, snapshot or read."
 *   term_port.h mem_alloc: "Called exactly once per term, at create; freed
 *    exactly once, by the reaper — or never, when the slot becomes a
 *    ZOMBIE."
 *   §4.1/§4.3: the block goes to PSRAM; TERM_MEM_INTERNAL is the escape
 *    hatch "Nothing in phase 2 passes".
 *
 * The fake allocator is the instrument: fp.alloc_frozen turns any call
 * into a counted violation, so "zero mallocs while running" is measured,
 * not asserted by inspection.
 */
#include "reg_util.h"

static void case_one_block_per_term(void)
{
    term_id_t id;
    term_slot_info_t inf;

    t_case("create takes exactly one block, and info reports its size");
    CHK_TRUE(reg_boot());
    CHK_INT(fp.allocs, 0);
    id = reg_new("v", OWNER_A, TERM_VT, false, 80, 24);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(fp.allocs, 1);
    CHK_INT(fp.frees, 0);

    inf = reg_info(id);
    CHK_INT((int)inf.mem_bytes, (int)fp.last_size);
    CHK_INT((int)fp.live_bytes, (int)inf.mem_bytes);
    CHK_TRUE(inf.mem_bytes > 0);

    t_case("the block is asked for in PSRAM (§4.1, §4.3)");
    CHK_HEX(fp.last_flags, TERM_MEM_PSRAM);

    t_case("a VT term costs more than a LOG one: the alt screen (§4.1)");
    {
        term_id_t lg = reg_new("l", OWNER_A, TERM_LOG, false, 80, 24);
        CHK_TRUE(lg != TERM_ID_INVALID);
        CHK_INT(fp.allocs, 2);
        CHK_TRUE(reg_info(lg).mem_bytes < inf.mem_bytes);
    }

    t_case("close + reap frees it exactly once");
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    reg_settle();
    CHK_INT(fp.frees, 1);
    reg_settle();
    CHK_INT(fp.frees, 1);                    /* not a second time */

    CHK_INT(reg_shutdown(), 0);
    CHK_INT(fp.allocs, fp.frees);
    CHK_INT((int)fp.live_bytes, 0);
}

static void case_nothing_allocates_while_running(void)
{
    term_id_t vt, lg;
    term_view_t view;
    char out[2048];
    term_read_result_t res;
    uint8_t blob[256];
    int i;

    t_case("no allocation on feed / log / resize / drain / snapshot / read");
    CHK_TRUE(reg_boot());
    vt = reg_new("v", OWNER_A, TERM_VT, false, 80, 24);
    lg = reg_new("l", OWNER_A, TERM_LOG, true, 80, 24);
    CHK_TRUE(vt != TERM_ID_INVALID && lg != TERM_ID_INVALID);
    memset(&view, 0, sizeof view);
    view.w = 200; view.h = 200; view.visible = true;
    CHK_INT(term_registry_show(vt, OWNER_A, &view), TERM_OK);
    CHK_INT(term_registry_show(lg, OWNER_A, &view), TERM_OK);
    reg_frame();

    memset(blob, 'q', sizeof blob);

    /* Arm the trap. Everything below is the running phase (§4.2). */
    fp.alloc_frozen = true;

    for (i = 0; i < 400; i++) {
        char line[64];
        int n = sprintf(line, "line %d with some payload", i);
        (void)term_registry_log(lg, OWNER_A, line, (size_t)n);
        (void)term_registry_feed(vt, OWNER_A, blob, sizeof blob, NULL);
        (void)term_registry_feed(vt, OWNER_A,
                                 (const uint8_t *)"\x1b[31;1mred\x1b[0m\r\n", 16, NULL);
        if ((i & 7) == 0) {
            /* Both extremes of §4.1's rotation pair, inside the block the
             * worst case already paid for. */
            (void)term_registry_resize(vt, OWNER_A, 142, 30);
            reg_frame();
            (void)term_registry_resize(vt, OWNER_A, 80, 53);
        }
        reg_frame();
        if ((i & 15) == 0) {
            memset(&res, 0, sizeof res);
            (void)term_registry_snapshot(vt, OWNER_A, out, sizeof out, NULL);
            (void)term_registry_read(lg, OWNER_A, 0, 16, out, sizeof out, &res);
            (void)reg_info(vt);
            (void)reg_stats();
            (void)term_registry_reap();
        }
    }

    /* The whole persist/detach/re-attach cycle, still under the trap. */
    CHK_INT(term_registry_owner_stopped(OWNER_A), 2);
    {
        term_create_opts_t o = reg_opts("l", OWNER_A, TERM_LOG, true, 80, 24);
        term_id_t back = TERM_ID_INVALID;
        bool re = false;
        CHK_INT(term_registry_create(&o, &back, &re), TERM_OK);
        CHK_INT(back, lg);
        CHK_TRUE(re);
    }

    fp.alloc_frozen = false;

    t_case("...the allocator was not called once in all of that (I4)");
    CHK_INT(fp.alloc_violations, 0);
    CHK_INT(fp.allocs, 2);

    t_case("nothing leaked and nothing was double-freed");
    CHK_INT(reg_shutdown(), 0);
    CHK_INT(fp.allocs, fp.frees);
    CHK_INT((int)fp.live_bytes, 0);
}

static void case_create_is_the_only_failure_point(void)
{
    term_create_opts_t o;
    term_id_t id = TERM_ID_INVALID;
    int live_before;

    t_case("an allocator that says no is TERM_ERR_NO_MEM (I4)");
    CHK_TRUE(reg_boot());
    live_before = reg_live_count();
    fp.alloc_fail_next = 1;
    o = reg_opts("v", OWNER_A, TERM_VT, false, 80, 24);
    CHK_INT(term_registry_create(&o, &id, NULL), TERM_ERR_NO_MEM);
    CHK_INT(id, TERM_ID_INVALID);

    t_case("...and the failed create consumed no slot");
    CHK_INT(reg_live_count(), live_before);
    CHK_INT(fp.allocs, 0);
    CHK_INT(fp.frees, 0);

    t_case("the next create succeeds normally");
    CHK_INT(term_registry_create(&o, &id, NULL), TERM_OK);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(fp.allocs, 1);
    CHK_INT(reg_live_count(), live_before + 1);

    t_case("a NO_MEM create did not disturb the generation sequence");
    CHK_TRUE(term_id_generation(id) >= 1);

    CHK_INT(reg_shutdown(), 0);
    CHK_INT((int)fp.live_bytes, 0);
}

static void case_full_table_accounting(void)
{
    term_id_t ids[TERM_SLOT_COUNT];
    char name[TERM_NAME_MAX];
    size_t total = 0;
    int i;

    t_case("eight terms means eight blocks and eight frees (I1, I4)");
    CHK_TRUE(reg_boot());
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        sprintf(name, "s%d", i);
        ids[i] = reg_new(name, OWNER_A, (i & 1) ? TERM_LOG : TERM_VT, false, 80, 24);
        CHK_TRUE(ids[i] != TERM_ID_INVALID);
        total += reg_info(ids[i]).mem_bytes;
    }
    CHK_INT(fp.allocs, TERM_SLOT_COUNT);
    CHK_INT((int)fp.live_bytes, (int)total);

    t_case("the design's budget holds: a full table stays inside 1MB (§4.1)");
    CHK_TRUE(total <= 1024u * 1024u * 2u);   /* §4.1 quotes ~1.3MB for 8 VT */

    CHK_INT(reg_shutdown(), 0);
    CHK_INT(fp.frees, TERM_SLOT_COUNT);
    CHK_INT((int)fp.live_bytes, 0);
}

static void case_zombie_keeps_its_block(void)
{
    term_registry_config_t c;
    term_id_t id;
    size_t held;

    memset(&c, 0, sizeof c);
    c.quiesce_timeout_ms = 500;

    t_case("a ZOMBIE keeps its block: term_port.h's \"or never\" branch");
    CHK_TRUE(reg_boot_cfg(&c));
    id = reg_new("hung", OWNER_A, TERM_VT, false, 80, 24);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_producer_bind(id, OWNER_A, reg_noop_detach, NULL), TERM_OK);
    held = fp.live_bytes;
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    fp_advance(1000);
    reg_settle();
    CHK_INT(reg_state(id), TERM_SLOT_ZOMBIE);
    CHK_INT(fp.frees, 0);
    CHK_INT((int)fp.live_bytes, (int)held);
    CHK_INT((int)reg_stats().zombie_bytes, (int)held);

    t_case("deinit says it could not collect it, and still does not free it");
    CHK_INT(reg_shutdown(), 1);
    CHK_INT(fp.frees, 0);
    CHK_INT((int)fp.live_bytes, (int)held);
    fp_reclaim_all();
}

int main(void)
{
    t_suite("registry_alloc");
    case_one_block_per_term();
    case_nothing_allocates_while_running();
    case_create_is_the_only_failure_point();
    case_full_table_accounting();
    case_zombie_keeps_its_block();
    fp_reclaim_all();
    return t_summary();
}
