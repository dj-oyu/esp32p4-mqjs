/*
 * test_rec_off.c — recording OFF, which is the default and the guarantee.
 *
 * This is the suite that matters most, because phase 5's failure mode is a
 * privacy incident and not a glitch: everything else in the feature is a
 * convenience, while "a session nobody armed left no trace in a region a
 * signing key can pull in plaintext" is the promise the user made the feature
 * conditional on.
 *
 * Contracts under test:
 *   term_registry.h R1  "OFF BY DEFAULT, PER TERM, AND NEVER REMEMBERED
 *     ANYWHERE ... it is a GUARANTEE a test may assert, not a default a caller
 *     may change"; "there is no recording field on term_create_opts_t"
 *   term_registry.h R5  "IT COSTS NOTHING WHEN OFF. Recording on/off IS the
 *     installation and removal of term_core's record callback, not a boolean
 *     the hook tests: with recording off there is no route from a term to the
 *     ring"
 *   term_lp_ring.h P3   "with recording off no code path exists from SSH
 *     session content to this memory. `term.feed`, the byte ring, the VT
 *     parser and the ssh pipe never call append at all"
 *   term_registry.h     term_registry_record_screen: "TERM_ERR_INVAL when the
 *     term is not recording: this is not a second, unannounced way to put
 *     session content in the ring"
 *   term_registry.h     term_registry_record: "TERM_ERR_MODE ON A LOG TERM"
 *   term_registry.h I3  every entry point re-checks generation and owner —
 *     the three new calls included
 *   §3.2's table        "LP 黒箱 tee — TERM_VT: しない" by default
 *
 * WHY THE ASSERTIONS SEARCH THE REGION AND NOT THE RECORD LIST. A record-list
 * check answers "did a reader see it", which is not the question. The question
 * is "are the bytes there", and a write that lands and is then orphaned — a
 * header that never published it, a record evicted a moment later — is still a
 * leak to the holder of the key. So the primary detector here is
 * p5_region_hits(), a byte search over all 32,256 bytes of the live region.
 */
#include "rec_util.h"

/* ===================================================================== */
/* Off by default                                                        */
/* ===================================================================== */

static void case_default_is_off(void)
{
    term_id_t id, id2;
    term_create_opts_t o;

    t_case("a term is born with recording off (R1) — every mode and shape");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("v", OWNER_A, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p5_recording(id, OWNER_A), 0);
    CHK_INT(p5_info_recording(id), 0);

    /* A persist term is the one an app is most likely to have saved a flag
     * for, so it is called out separately. */
    id2 = reg_new("p", OWNER_A, TERM_VT, true, 20, 4);
    CHK_TRUE(id2 != TERM_ID_INVALID);
    CHK_INT(p5_recording(id2, OWNER_A), 0);

    t_case("the create options cannot ask for it: zeroed opts, no field (R1)");
    /* There is no recording field to set — R1 says there is not and never
     * will be. The nearest a test can come to asserting the absence of a
     * struct member is to build the options from all-zeroes and from a fully
     * populated struct and get an unarmed term either way. */
    memset(&o, 0, sizeof o);
    o.name = "z"; o.owner = OWNER_A; o.mode = TERM_VT; o.cols = 20; o.rows = 4;
    o.persist = true;
    o.max_cols = 80; o.max_rows = 24; o.max_cells = 80 * 24;
    o.scrollback_bytes = 8192; o.scrollback_lines = 64;
    {
        term_id_t id3 = TERM_ID_INVALID;
        CHK_INT(term_registry_create(&o, &id3, NULL), TERM_OK);
        CHK_INT(p5_recording(id3, OWNER_A), 0);
    }

    t_case("R5: the term_core tee is NOT installed, which is what 'off' means");
    p5_show(id, OWNER_A);
    CHK_INT(p5_core_recording(), 0);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The inertness measurement                                             */
/* ===================================================================== */

/*
 * A whole session's worth of traffic through a term nobody armed: lines that
 * scroll off, a full-screen redraw storm, alt-screen output, a resize that
 * pushes rows into scrollback, a term.log line, a producer writing through the
 * pipe, and finally the lifecycle events that DO capture when armed.
 *
 * Every needle is unique to this case, because the region is process-wide and
 * a needle another case legitimately recorded would still be in it.
 */
static void case_off_leaves_no_byte_anywhere(void)
{
    term_id_t id;
    static p4_prod_t prod;
    term_producer_t pt;
    p5_mark_t a, b;
    int i;

    t_case("recording off: a full session leaves nothing in the region (P3/R5)");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("off", OWNER_A, 24, 5);
    REQUIRE(id != TERM_ID_INVALID);
    p5_show(id, OWNER_A);
    p5_mark(&a);

    /* 40 lines: the screen is 5 rows, so 35 of them are archived to
     * scrollback — every one of them a call the line tee would have made. */
    p5_feed_lines(id, OWNER_A, "SECRETLINE", 0, 40);
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "SECRETLINE039") != NULL);

    /* A redraw storm, an alt-screen session, and a resize that archives. */
    for (i = 0; i < 20; i++) p5_repaint(id, OWNER_A, 5, i, "SECRETPAINT");
    p5_feed(id, OWNER_A, "\x1b[?1049h");
    p5_feed_lines(id, OWNER_A, "SECRETALT", 0, 15);
    p5_feed(id, OWNER_A, "\x1b[?1049l");
    CHK_INT(term_registry_resize(id, OWNER_A, 24, 2), TERM_OK);
    reg_frame();
    p5_feed_lines(id, OWNER_A, "SECRETAFTER", 0, 6);

    /* Time passing cannot produce a capture for a term that is not armed —
     * the settle timer of R3(b) is only reachable "while recording". */
    for (i = 0; i < 8; i++) p5_quiet((int64_t)TERM_REC_SNAP_MIN_MS + 100, 2);

    /* And the producer path, which term_lp_ring.h P3 names explicitly. */
    p4_prod_reset(&prod, "off");
    pt = p4_producer(&prod, false);
    CHK_INT(term_registry_pipe(id, OWNER_A, &pt), TERM_OK);
    {
        size_t wrote = 0;
        CHK_INT(term_registry_producer_write(id, &prod,
                    (const uint8_t *)"SECRETPIPE1\r\nSECRETPIPE2\r\n", 26, &wrote),
                TERM_OK);
        CHK_INT((int)wrote, 26);
    }
    for (i = 0; i < 6; i++) reg_frame();
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "SECRETPIPE2") != NULL);

    t_case("not one class-TERM record was appended");
    p5_mark(&b);
    CHK_INT(P5_D(ses), 0);
    CHK_INT(P5_D(scr), 0);
    CHK_INT(P5_D(ses_bytes), 0);
    CHK_INT(P5_D(scr_bytes), 0);

    t_case("...and the APP partition took no append at all (not even evicted)");
    /* The stronger statement: `appended` is monotonic, so a record that was
     * written and then evicted still shows here. Nothing in this case writes
     * to the APP partition through any other route either. */
    CHK_INT(P5_D(appended[TERM_LP_PART_APP]), 0);
    CHK_INT(P5_D(evicted[TERM_LP_PART_APP]), 0);

    t_case("...and no byte of the session's text is anywhere in the region");
    CHK_INT((int)p5_region_hits("SECRETLINE"), 0);
    CHK_INT((int)p5_region_hits("SECRETPAINT"), 0);
    CHK_INT((int)p5_region_hits("SECRETALT"), 0);
    CHK_INT((int)p5_region_hits("SECRETAFTER"), 0);
    CHK_INT((int)p5_region_hits("SECRETPIPE"), 0);
    /* The writer id a recorded term would have interned is not there either:
     * the writer table is part of the region and part of the leak. */
    CHK_INT((int)p5_region_hits(p5_writer_of(OWNER_A, "off")), 0);

    t_case("...and every one of the nine rec_* counters stayed put");
    P5_CHK_RECD(&a, &b, "a whole session with recording off", P5_RECD_ZERO);

    t_case("the lifecycle events that capture when armed also write nothing");
    p5_mark(&a);
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    reg_settle();
    p5_mark(&b);
    CHK_INT(P5_D(ses), 0);
    CHK_INT(P5_D(scr), 0);
    CHK_INT(P5_D(appended[TERM_LP_PART_APP]), 0);
    P5_CHK_RECD(&a, &b, "unbind + ack + close, never armed", P5_RECD_ZERO);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The two doors that must stay shut while off                           */
/* ===================================================================== */

static void case_record_screen_refused_while_off(void)
{
    term_id_t id;
    p5_mark_t a, b;

    t_case("record_screen while off is TERM_ERR_INVAL, not a quiet capture");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("rs", OWNER_A, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);
    p5_feed(id, OWNER_A, "NOBUTTONCONTENT\r\n");
    p5_mark(&a);
    CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_ERR_INVAL);
    CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_ERR_INVAL);
    p5_mark(&b);
    CHK_INT(P5_D(scr), 0);
    CHK_INT((int)p5_region_hits("NOBUTTONCONTENT"), 0);
    P5_CHK_RECD(&a, &b, "record_screen refused", P5_RECD_ZERO);

    CHK_INT(reg_shutdown(), 0);
}

static void case_panic_capture_with_nothing_armed(void)
{
    term_id_t id;
    p5_mark_t a, b;

    /* term_registry_panic_capture is one-shot per process, so the "nothing is
     * recording" half of its contract has to be the only call in some suite.
     * It belongs here: with nothing armed, the panic path is inert too. */
    t_case("the panic path appends nothing when no term is recording (R3c)");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("pc", OWNER_A, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);
    p5_feed(id, OWNER_A, "PANICNOTARMED\r\n");
    CHK_TRUE(term_lp_panic_ready());
    p5_mark(&a);
    CHK_INT(term_registry_panic_capture(), 0);
    p5_mark(&b);
    CHK_INT(P5_D(scr), 0);
    CHK_INT(P5_D(appended[TERM_LP_PART_APP]), 0);
    CHK_INT((int)p5_region_hits("PANICNOTARMED"), 0);
    P5_CHK_RECD(&a, &b, "panic capture, nothing armed", P5_RECD_ZERO);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* A LOG term refuses to be armed                                        */
/* ===================================================================== */

static void case_log_term_refuses(void)
{
    term_id_t lg;
    p5_mark_t a, b;

    t_case("record on a TERM_LOG term is TERM_ERR_MODE (R1's class-B scope)");
    CHK_TRUE(reg_boot());
    lg = reg_new("lg", OWNER_A, TERM_LOG, false, 24, 4);
    REQUIRE(lg != TERM_ID_INVALID);
    p5_mark(&a);
    CHK_INT(term_registry_record(lg, OWNER_A, true), TERM_ERR_MODE);
    CHK_INT(p5_recording(lg, OWNER_A), 0);
    CHK_INT(p5_info_recording(lg), 0);

    t_case("...and turning it OFF is refused the same way, not silently OK");
    /* The header attaches TERM_ERR_MODE to the operation, not to the `on`
     * argument: "the feature is refused where it has no work to do". */
    CHK_INT(term_registry_record(lg, OWNER_A, false), TERM_ERR_MODE);

    t_case("...and record_screen on a log term is refused too");
    CHK_TRUE(term_registry_record_screen(lg, OWNER_A) != TERM_OK);

    t_case("the reason it is refused: a log term is ALREADY in the box, as APP");
    CHK_INT(term_registry_log(lg, OWNER_A, "LOGCLASSA", 9), TERM_OK);
    reg_frame();
    p5_mark(&b);
    CHK_TRUE(P5_D(app) >= 1);        /* class A landed          */
    CHK_INT(P5_D(ses), 0);           /* class TERM did not      */
    CHK_INT(P5_D(scr), 0);
    CHK_TRUE(p5_region_hits("LOGCLASSA") >= 1);
    /* Exactly the duplication the refusal exists to prevent: had arming
     * worked, this text would be in the region twice. */
    CHK_INT((int)p5_region_hits("LOGCLASSA"), 1);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* I3: the three new entry points are gated like every other             */
/* ===================================================================== */

static void case_owner_and_generation_gates(void)
{
    term_id_t id, reused;
    p5_mark_t a, b;

    t_case("a foreign owner cannot arm, query or capture another app's term");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("g", OWNER_A, 20, 4);
    REQUIRE(id != TERM_ID_INVALID);
    p5_mark(&a);
    CHK_INT(term_registry_record(id, OWNER_B, true), TERM_ERR_NOT_OWNER);
    CHK_INT(term_registry_record(id, OWNER_B, false), TERM_ERR_NOT_OWNER);
    CHK_INT(term_registry_recording(id, OWNER_B), TERM_ERR_NOT_OWNER);
    CHK_INT(term_registry_record_screen(id, OWNER_B), TERM_ERR_NOT_OWNER);
    CHK_INT(p5_recording(id, OWNER_A), 0);
    p5_mark(&b);
    CHK_TRUE(reg_stats().denied_owner > 0);
    P5_CHK_RECD(&a, &b, "four refusals by owner", P5_RECD_ZERO);

    t_case("a malformed id or owner is an error, never a crash (I5)");
    CHK_INT(term_registry_record(TERM_ID_INVALID, OWNER_A, true), TERM_ERR_BAD_ID);
    CHK_INT(term_registry_record(0x7fffffff, OWNER_A, true), TERM_ERR_BAD_ID);
    CHK_INT(term_registry_recording(0x7fffffff, OWNER_A), TERM_ERR_BAD_ID);
    CHK_INT(term_registry_record_screen(0x7fffffff, OWNER_A), TERM_ERR_BAD_ID);
    CHK_INT(term_registry_record(id, NULL, true), TERM_ERR_INVAL);
    CHK_INT(term_registry_recording(id, NULL), TERM_ERR_INVAL);
    CHK_INT(term_registry_record_screen(id, NULL), TERM_ERR_INVAL);

    t_case("an id whose slot was reused answers STALE, not the new tenant (I3)");
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    reg_settle();
    reused = p5_new_vt("g2", OWNER_A, 20, 4);
    CHK_TRUE(reused != TERM_ID_INVALID);
    CHK_INT(term_id_slot(reused), term_id_slot(id));   /* same slot, new gen */
    CHK_TRUE(term_id_generation(reused) > term_id_generation(id));
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_ERR_STALE);
    CHK_INT(term_registry_recording(id, OWNER_A), TERM_ERR_STALE);
    CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_ERR_STALE);
    CHK_INT(p5_recording(reused, OWNER_A), 0);

    t_case("a DYING term refuses to be armed (ingest is already refused)");
    CHK_INT(term_registry_close(reused, OWNER_A), TERM_OK);
    CHK_INT(reg_state(reused), TERM_SLOT_DYING);
    CHK_INT(term_registry_record(reused, OWNER_A, true), TERM_ERR_DYING);
    CHK_INT(term_registry_recording(reused, OWNER_A), TERM_ERR_DYING);
    CHK_INT(term_registry_record_screen(reused, OWNER_A), TERM_ERR_DYING);
    CHK_INT(p5_info_recording(reused), 0);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The whole-suite statement                                             */
/* ===================================================================== */

static p5_bag_t final_bag;

static void case_suite_wide_zero(void)
{
    unsigned k, term_records = 0;
    term_lp_stats_t ls;

    t_case("nothing in this whole suite ever put a class-TERM record in the ring");
    p5_collect(&final_bag);
    for (k = 0; k < final_bag.n; k++)
        if (final_bag.r[k].cls == TERM_LP_CLASS_TERM) term_records++;
    CHK_INT((int)term_records, 0);

    t_case("...and the region still walks as a reader would walk it");
    p5_check_live_layout();

    memset(&ls, 0, sizeof ls);
    term_lp_stats(&ls);
    CHK_TRUE(ls.ready);
    /* Only the boot marker and this suite's one class-A log line ever went in:
     * whatever the number, the APP partition holds no session content. */
    CHK_TRUE(ls.appended[TERM_LP_PART_APP] <= 2);
}

int main(void)
{
    t_suite("rec_off");
    case_default_is_off();
    case_off_leaves_no_byte_anywhere();
    case_record_screen_refused_while_off();
    case_panic_capture_with_nothing_armed();
    case_log_term_refuses();
    case_owner_and_generation_gates();
    case_suite_wide_zero();
    fp_reclaim_all();
    return t_summary();
}
