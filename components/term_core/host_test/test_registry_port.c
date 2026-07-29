/*
 * test_registry_port.c — the platform seam and the registry's own lifecycle.
 *
 * Contracts under test:
 *   term_port.h  "Every non-optional hook must be non-NULL; term_port_install()
 *                 refuses a table that is not complete", reaper_wake/log "may
 *                 be NULL", term_port_valid "same completeness check, no side
 *                 effects".
 *   term_registry.h  init "Requires an installed port", "Idempotent-hostile:
 *                 a second call without deinit returns TERM_ERR_NOT_READY",
 *                 deinit "Returns the number of slots that could NOT be
 *                 collected", the console config field, TERM_OWNER_SYSTEM.
 *   design §3.1 (fixed table of 8), §11.2 (the shared console is the point
 *                 of this phase).
 */
#include "reg_util.h"

/* ===================================================================== */
/* Completeness of the port table                                        */
/* ===================================================================== */

/* Offsets of the mandatory hooks, so the "one NULL at a time" sweep does
 * not have to be written out thirteen times. */
static const size_t mandatory_off[] = {
    offsetof(term_port_t, mutex_create),
    offsetof(term_port_t, mutex_destroy),
    offsetof(term_port_t, mutex_lock),
    offsetof(term_port_t, mutex_unlock),
    offsetof(term_port_t, now_ms),
    offsetof(term_port_t, signal_create),
    offsetof(term_port_t, signal_destroy),
    offsetof(term_port_t, signal_set),
    offsetof(term_port_t, signal_wait),
    offsetof(term_port_t, ui_post),
    offsetof(term_port_t, ui_is_current),
    offsetof(term_port_t, mem_alloc),
    offsetof(term_port_t, mem_free),
};

static void case_port_valid(void)
{
    size_t i;
    static term_port_t probe;

    t_case("term_port_valid: a complete table passes, NULL fails");
    fp_reset();
    CHK_TRUE(term_port_valid(fp_port()));
    CHK_TRUE(!term_port_valid(NULL));

    t_case("term_port_valid: optional hooks may be NULL (term_port.h)");
    CHK_TRUE(term_port_valid(fp_port_minimal()));

    t_case("term_port_valid: every mandatory hook is actually required");
    for (i = 0; i < sizeof mandatory_off / sizeof mandatory_off[0]; i++) {
        void **slot;
        probe = *fp_port();
        slot = (void **)((char *)&probe + mandatory_off[i]);
        *slot = NULL;
        if (term_port_valid(&probe)) {
            t_head(__FILE__, __LINE__);
            printf("     mandatory hook at offset %u accepted as NULL\n",
                   (unsigned)mandatory_off[i]);
        }
        t_checks++;
    }

    t_case("term_port_valid has no side effects (term_port.h)");
    CHK_TRUE(term_port_valid(fp_port()));
    CHK_INT(fp.mutex_creates, 0);
    CHK_INT(fp.allocs, 0);
}

static void case_install(void)
{
    static term_port_t broken;
    const term_port_t *before;

    t_case("install refuses an incomplete table and changes nothing");
    fp_reset();
    CHK_TRUE(term_port_install(fp_port()));
    before = term_port_get();
    CHK_TRUE(before == fp_port());
    CHK_TRUE(term_port_installed());

    broken = *fp_port();
    broken.mem_alloc = NULL;
    CHK_TRUE(!term_port_install(&broken));
    CHK_TRUE(term_port_get() == before);

    CHK_TRUE(!term_port_install(NULL));
    CHK_TRUE(term_port_get() == before);

    t_case("the installed pointer is retained, not the contents copied");
    CHK_TRUE(term_port_get() == fp_port());
}

/* ===================================================================== */
/* Registry init / deinit                                                */
/* ===================================================================== */

static void case_init_needs_a_port(void)
{
    t_case("everything answers TERM_ERR_NOT_READY before init");
    fp_reset();
    /* A port is installed by the previous case; the registry is not up. */
    CHK_TRUE(!term_registry_ready());
    CHK_INT(term_registry_console_id(), TERM_ID_INVALID);
    reg_expect_all_gated((term_id_t)(1 << TERM_SLOT_BITS), OWNER_A,
                         TERM_ERR_NOT_READY);
    CHK_INT(term_registry_system_log("w", "x", 1), TERM_ERR_NOT_READY);
    CHK_INT(term_registry_find(OWNER_A, "n", NULL), TERM_ERR_NOT_READY);
    {
        term_create_opts_t o = reg_opts("n", OWNER_A, TERM_LOG, false, 20, 4);
        term_id_t id = TERM_ID_INVALID;
        CHK_INT(term_registry_create(&o, &id, NULL), TERM_ERR_NOT_READY);
        CHK_INT(id, TERM_ID_INVALID);
    }
    CHK_INT(term_registry_producer_ack((term_id_t)8), TERM_ERR_NOT_READY);
}

static void case_init_lifecycle(void)
{
    t_case("init(NULL) takes the defaults and comes up ready");
    CHK_TRUE(reg_boot());
    CHK_TRUE(term_registry_ready());
    CHK_INT(fp.mutex_creates, 1);          /* one table lock (term_port.h)  */

    t_case("a second init without deinit is refused (idempotent-hostile)");
    CHK_INT(term_registry_init(NULL), TERM_ERR_NOT_READY);
    CHK_TRUE(term_registry_ready());

    t_case("deinit of an empty table collects everything");
    CHK_INT(reg_shutdown(), 0);
    CHK_TRUE(!term_registry_ready());
    CHK_INT(fp.mutex_destroys, fp.mutex_creates);
    CHK_INT(fp.allocs, fp.frees);
    CHK_INT((int)fp.live_bytes, 0);

    t_case("after deinit the API is NOT_READY again, and init works again");
    CHK_INT(term_registry_find(OWNER_A, "n", NULL), TERM_ERR_NOT_READY);
    CHK_INT(term_registry_init(NULL), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

static void case_deinit_closes_live_terms(void)
{
    term_id_t a, b;

    t_case("deinit forces live terms through stage 1 and frees them");
    CHK_TRUE(reg_boot());
    a = reg_new("t0", OWNER_A, TERM_LOG, false, 20, 4);
    b = reg_new("t1", OWNER_B, TERM_VT, true, 20, 4);
    CHK_TRUE(a != TERM_ID_INVALID);
    CHK_TRUE(b != TERM_ID_INVALID);
    CHK_INT(fp.allocs, 2);

    CHK_INT(reg_shutdown(), 0);
    CHK_INT(fp.frees, 2);
    CHK_INT((int)fp.live_bytes, 0);
    CHK_INT(fp.mutex_destroys, fp.mutex_creates);
}

/* ===================================================================== */
/* The shared console (§11.2)                                            */
/* ===================================================================== */

static void case_no_console(void)
{
    term_registry_config_t cfg;
    memset(&cfg, 0, sizeof cfg);

    t_case("config.console == NULL creates no console (term_registry.h)");
    fp_reset();
    CHK_TRUE(term_port_install(fp_port()));
    CHK_INT(term_registry_init(&cfg), TERM_OK);
    CHK_INT(term_registry_console_id(), TERM_ID_INVALID);
    CHK_INT(fp.allocs, 0);

    t_case("system_log with no console is an error, not a crash (I5)");
    CHK_TRUE(term_registry_system_log("someapp", "hello", 5) != TERM_OK);

    CHK_INT(reg_shutdown(), 0);
}

static void case_console(void)
{
    term_registry_config_t cfg;
    term_create_opts_t copts;
    term_id_t cid;
    term_slot_info_t info;

    t_case("config.console creates the shared TERM_LOG console");
    copts = reg_opts("console", "pretend_app", TERM_LOG, true, 40, 6);
    memset(&cfg, 0, sizeof cfg);
    cfg.console = &copts;

    fp_reset();
    CHK_TRUE(term_port_install(fp_port()));
    CHK_INT(term_registry_init(&cfg), TERM_OK);

    cid = term_registry_console_id();
    CHK_TRUE(cid != TERM_ID_INVALID);
    CHK_INT(fp.allocs, 1);

    t_case("the console's owner is forced to TERM_OWNER_SYSTEM");
    CHK_INT(term_registry_info(cid, &info), TERM_OK);
    CHK_STR(info.owner, TERM_OWNER_SYSTEM);
    CHK_INT(info.mode, TERM_LOG);
    CHK_INT(info.state, TERM_SLOT_LIVE);

    t_case("an app cannot reach the console: the owner gate holds (§7.2)");
    reg_expect_all_gated(cid, OWNER_A, TERM_ERR_NOT_OWNER);
    CHK_INT(reg_state(cid), TERM_SLOT_LIVE);   /* the close was refused too */

    t_case("system_log lands in the console (§3.1 print sink -> registry)");
    CHK_INT(term_registry_system_log("someapp", "hello console", 13), TERM_OK);
    reg_frame();
    CHK_TRUE(strstr(reg_snap(cid, TERM_OWNER_SYSTEM), "hello console") != NULL);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Constants the design fixes (§3.1)                                     */
/* ===================================================================== */

static void case_constants(void)
{
    t_case("fixed table of 8 slots, 28-bit generation (§3.1, I1/I2)");
    CHK_INT(TERM_SLOT_BITS, 3);
    CHK_INT(TERM_SLOT_COUNT, 8);
    CHK_INT(TERM_SLOT_MASK, 7);
    CHK_INT(TERM_GEN_BITS, 28);
    CHK_TRUE(TERM_SLOT_BITS + TERM_GEN_BITS <= 31);   /* stays positive int32 */
    CHK_INT(TERM_PERSIST_MAX_DEFAULT, 4);             /* §3.1 initial value  */
    CHK_INT(TERM_ID_INVALID, 0);

    t_case("the port's timing policy matches the design's numbers");
    CHK_INT(TERM_INGEST_TIMEOUT_MS_DEFAULT, 20);      /* §5   */
    CHK_INT(TERM_QUIESCE_TIMEOUT_MS_DEFAULT, 3000);   /* §3.1 */

    t_case("term_err_str is never NULL for any documented code (I5)");
    {
        int e;
        for (e = 0; e >= -14; e--) {
            const char *s = term_err_str((term_err_t)e);
            t_checks++;
            if (!s || !s[0]) {
                t_head(__FILE__, __LINE__);
                printf("     term_err_str(%d) returned %s\n", e, s ? "\"\"" : "NULL");
            }
        }
        CHK_TRUE(term_err_str((term_err_t)-999) != NULL);   /* unknown code  */
    }
}

int main(void)
{
    t_suite("registry_port");
    case_port_valid();
    case_install();
    case_init_needs_a_port();
    case_init_lifecycle();
    case_deinit_closes_live_terms();
    case_no_console();
    case_console();
    case_constants();
    fp_reclaim_all();
    return t_summary();
}
