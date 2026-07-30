/*
 * test_rec_stats.c — the nine rec_* counters, one case each.
 *
 * term_registry.h documents each one in a line, and each line is a claim about
 * WHICH event moves it:
 *
 *   rec_on             "recording turned on, per term per session"
 *   rec_off            "turned off explicitly (not by lifecycle)"
 *   rec_cleared        "turned off BY a lifecycle transition, i.e. the R1
 *                       guarantee doing its work: re-attach, detach, pipe bind,
 *                       detach ack, close"
 *   rec_lines          "archived rows teed into the black box"
 *   rec_screens        "screen captures that landed"
 *   rec_screen_rows    "rows those captures wrote"
 *   rec_snaps_deferred "settle evaluations the SNAP_MIN rate limit turned away,
 *                       once per settle episode"
 *   rec_screen_trunc   "captures cut at TERM_REC_SCREEN_MAX"
 *   rec_panic_rows     "rows the panic path managed to append"
 *
 * Each case below drives exactly one of those events and states the whole
 * nine-counter delta vector, so "moved when it happened" and "and not
 * otherwise" are the same assertion. Where the header couples two counters —
 * arming necessarily captures, so rec_on cannot move alone — the coupling is
 * spelled out in the vector rather than hidden behind a P5_ANY.
 *
 * rec_panic_rows has its positive case in test_rec_panic.c, because
 * term_registry_panic_capture() is one-shot per process. Here it appears in
 * every vector as a zero, which is the other half of its claim.
 */
#include "rec_util.h"

static p5_bag_t bag;

/* Marker records in the whole region — one per capture that landed. */
static int count_markers(void)
{
    unsigned k;
    int n = 0;
    p5_collect(&bag);
    for (k = 0; k < bag.n; k++) if (p5_is_marker(&bag.r[k])) n++;
    return n;
}

/* A term with `rows` filled rows on the glass and nothing in flight. */
static term_id_t glass_term(const char *name, int cols, int rows, int fill)
{
    term_id_t id = reg_new(name, OWNER_A, TERM_VT, false, cols, rows);
    int r;
    if (id == TERM_ID_INVALID) return id;
    for (r = 1; r <= fill; r++) {
        char buf[64];
        sprintf(buf, "\x1b[%d;1HROW%02d", r, r);
        p5_feed(id, OWNER_A, buf);
    }
    return id;
}

/* ===================================================================== */
/* 1. rec_on                                                             */
/* ===================================================================== */

static void case_rec_on(void)
{
    term_id_t id;
    p5_mark_t a, b;

    t_case("rec_on: one per arming, and arming also takes its one capture");
    CHK_TRUE(reg_boot());
    id = glass_term("s1", 24, 4, 2);
    REQUIRE(id != TERM_ID_INVALID);

    p5_mark(&a);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    p5_mark(&b);
    /* on=1, and the immediate capture the header requires: screens=1 with the
     * two rows that were on the glass. Nothing else moves. */
    P5_CHK_RECD(&a, &b, "arming", { 1, 0, 0, 0, 1, 2, 0, 0, 0 });

    t_case("...and NOT again for an arming that changes nothing");
    p5_mark(&a);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "two redundant arms", P5_RECD_ZERO);

    t_case("...and NOT for a refused arming");
    {
        term_id_t lg = reg_new("s1log", OWNER_A, TERM_LOG, false, 24, 4);
        REQUIRE(lg != TERM_ID_INVALID);
        p5_mark(&a);
        CHK_INT(term_registry_record(lg, OWNER_A, true), TERM_ERR_MODE);
        CHK_INT(term_registry_record(id, OWNER_B, true), TERM_ERR_NOT_OWNER);
        /* Slot 7 is free in this suite, so this id names nothing at all
         * (BAD_ID) rather than an occupied slot at the wrong generation. */
        CHK_INT(term_registry_record(0x7ffffff7, OWNER_A, true), TERM_ERR_BAD_ID);
        p5_mark(&b);
        P5_CHK_RECD(&a, &b, "three refused arms", P5_RECD_ZERO);
    }

    t_case("...and once per SESSION, so re-arming after a clear counts again");
    p5_mark(&a);
    CHK_INT(term_registry_producer_bind(id, OWNER_A, reg_noop_detach, NULL),
            TERM_OK);          /* the bind clears it */
    CHK_INT(p5_recording(id, OWNER_A), 0);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    p5_mark(&b);
    CHK_INT(P5_DS(rec_on), 1);
    CHK_INT(P5_DS(rec_cleared), 1);
    CHK_INT(P5_DS(rec_off), 0);

    /* The bound producer has to ack or the slot cannot be collected (§3.1). */
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* 2/3. rec_off vs rec_cleared                                           */
/* ===================================================================== */

static void case_rec_off_and_cleared_are_disjoint(void)
{
    term_id_t id;
    p5_mark_t a, b;

    t_case("rec_off: only an explicit record(id, false) moves it");
    CHK_TRUE(reg_boot());
    id = glass_term("s2", 24, 4, 1);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);

    p5_mark(&a);
    CHK_INT(term_registry_record(id, OWNER_A, false), TERM_OK);
    p5_mark(&b);
    /* off=1 plus the final capture the header promises, and cleared stays put:
     * this was not a lifecycle event. */
    P5_CHK_RECD(&a, &b, "explicit off", { 0, 1, 0, 0, 1, 1, 0, 0, 0 });

    t_case("...and a redundant off moves nothing at all");
    p5_mark(&a);
    CHK_INT(term_registry_record(id, OWNER_A, false), TERM_OK);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "redundant off", P5_RECD_ZERO);

    t_case("rec_cleared: only a lifecycle transition moves it, never rec_off");
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    p5_mark(&a);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "close", { 0, 0, 1, 0, 1, 1, 0, 0, 0 });

    reg_settle();
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* 4. rec_lines                                                          */
/* ===================================================================== */

static void case_rec_lines(void)
{
    term_id_t id;
    p5_mark_t a, b;

    t_case("rec_lines: one per archived row, and nothing else moves it");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("s3", OWNER_A, 24, 4);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);

    /*
     * Eight '\r\n'-terminated lines on a 4-row screen: the last newline puts
     * the cursor on a ninth row, so five rows have been pushed off the top and
     * the glass holds rows 6, 7, 8 and the empty cursor row.
     */
    p5_mark(&a);
    p5_feed_lines(id, OWNER_A, "SL", 0, 8);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "8 lines on a 4-row screen",
                { 0, 0, 0, 5, 0, 0, 0, 0, 0 });
    CHK_INT(P5_D(ses), 5);

    t_case("...a capture does not move it, however many rows it writes");
    p5_mark(&a);
    CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_OK);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "a capture", { 0, 0, 0, 0, 1, 3, 0, 0, 0 });

    t_case("...and an in-place repaint does not move it either");
    p5_mark(&a);
    p5_repaint(id, OWNER_A, 4, 1, "SR");
    p5_repaint(id, OWNER_A, 4, 2, "SR");
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "two repaints", P5_RECD_ZERO);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* 5/6. rec_screens and rec_screen_rows                                  */
/* ===================================================================== */

static void case_rec_screens_and_rows(void)
{
    term_id_t id;
    p5_mark_t a, b;
    int r, markers_before;

    t_case("rec_screens counts LANDINGS and rec_screen_rows counts ROWS");
    CHK_TRUE(reg_boot());
    id = glass_term("s4", 24, 6, 5);
    REQUIRE(id != TERM_ID_INVALID);

    markers_before = count_markers();
    p5_mark(&a);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "a 5-row screen", { 1, 0, 0, 0, 1, 5, 0, 0, 0 });

    t_case("...the marker is not a row: the records are rows + 1");
    CHK_INT(P5_D(scr), P5_DS(rec_screen_rows) + 1);
    CHK_INT(count_markers() - markers_before, 1);

    t_case("...a blank screen is a landing with no rows");
    {
        term_id_t blank = p5_new_vt("s4b", OWNER_A, 24, 6);
        REQUIRE(blank != TERM_ID_INVALID);
        p5_mark(&a);
        CHK_INT(term_registry_record(blank, OWNER_A, true), TERM_OK);
        p5_mark(&b);
        P5_CHK_RECD(&a, &b, "arming a blank term", { 1, 0, 0, 0, 1, 0, 0, 0, 0 });
        CHK_INT(P5_D(scr), 1);
    }

    t_case("...and each row on the glass counts once, blank rows never");
    for (r = 1; r <= 6; r++) {
        char buf[64];
        sprintf(buf, "\x1b[%d;1HFULL%02d", r, r);
        p5_feed(id, OWNER_A, buf);
    }
    p5_mark(&a);
    CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_OK);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "a full 6-row screen", { 0, 0, 0, 0, 1, 6, 0, 0, 0 });

    t_case("...and a blank row in the MIDDLE keeps its place in the transcript");
    /*
     * Not from the header: a blank interior row is written as a single space.
     * It has to be something — an empty payload is refused by the ring — and
     * dropping the row would move every row below it, which is exactly what a
     * reader of a transcript cannot detect. So the row count does not fall.
     */
    p5_feed(id, OWNER_A, "\x1b[3;1H\x1b[K");    /* blank one row in the middle */
    p5_mark(&a);
    CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_OK);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "a screen with one blank row",
                { 0, 0, 0, 0, 1, 6, 0, 0, 0 });
    {
        p5_lines_t glass;
        p5_screen_lines(&glass, id, OWNER_A);
        CHK_INT(glass.n, 6);
        CHK_STR(glass.line[2], " ");
    }

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* 7. rec_snaps_deferred                                                 */
/* ===================================================================== */

static void case_rec_snaps_deferred(void)
{
    term_id_t id;
    p5_mark_t a, b;
    int i;

    t_case("rec_snaps_deferred: one per settle episode the rate limit refused");
    CHK_TRUE(reg_boot());
    id = glass_term("s5", 24, 4, 1);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);  /* anchor */

    /* Inside TERM_REC_SNAP_MIN_MS of the arming capture, with a changed
     * screen: the settle is evaluated, refused and counted once. */
    p5_mark(&a);
    p5_feed(id, OWNER_A, "\x1b[2J\x1b[1;1HDEF1");
    p5_settle();
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "one refused settle", { 0, 0, 0, 0, 0, 0, 1, 0, 0 });

    t_case("...and not once per frame while the episode lasts");
    p5_mark(&a);
    for (i = 0; i < 12; i++) p5_quiet(50, 4);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "48 more frames in the same episode", P5_RECD_ZERO);

    t_case("...a new episode needs new bytes, and is counted again");
    p5_mark(&a);
    p5_feed(id, OWNER_A, "\x1b[2J\x1b[1;1HDEF2");
    p5_settle();
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "a second refused settle", { 0, 0, 0, 0, 0, 0, 1, 0, 0 });

    t_case("...and once the limit opens the settle lands instead of deferring");
    p5_mark(&a);
    p5_quiet((int64_t)TERM_REC_SNAP_MIN_MS, 3);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "the settle after the limit opened",
                { 0, 0, 0, 0, 1, 1, 0, 0, 0 });

    t_case("...an unchanged screen is not a deferral, it is not an episode");
    p5_mark(&a);
    for (i = 0; i < 5; i++) p5_settle_after_rate_limit();
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "five quiet windows over an unchanged screen",
                P5_RECD_ZERO);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* 8. rec_screen_trunc                                                   */
/* ===================================================================== */

static void case_rec_screen_trunc(void)
{
    term_create_opts_t o;
    term_id_t big = TERM_ID_INVALID, small;
    p5_mark_t a, b;
    char row[142 * 3 + 8];
    size_t rowlen = 0;
    int i, r;

    t_case("rec_screen_trunc: only a capture cut at TERM_REC_SCREEN_MAX");
    CHK_TRUE(reg_boot());

    small = glass_term("s6", 24, 4, 3);
    REQUIRE(small != TERM_ID_INVALID);
    p5_mark(&a);
    CHK_INT(term_registry_record(small, OWNER_A, true), TERM_OK);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "a capture that fits", { 1, 0, 0, 0, 1, 3, 0, 0, 0 });

    memset(&o, 0, sizeof o);
    o.name = "s6big"; o.owner = OWNER_A; o.mode = TERM_VT;
    o.cols = 142; o.rows = 53;
    o.max_cols = 142; o.max_rows = 53; o.max_cells = 142 * 53;
    CHK_INT(term_registry_create(&o, &big, NULL), TERM_OK);
    REQUIRE(big != TERM_ID_INVALID);
    for (i = 0; i < 142; i++) { memcpy(row + rowlen, "\xe2\x94\x80", 3); rowlen += 3; }
    row[rowlen] = 0;
    for (r = 1; r <= 53; r++) {
        char cu[32];
        sprintf(cu, "\x1b[%d;1H", r);
        (void)term_registry_feed(big, OWNER_A, (const uint8_t *)cu, strlen(cu), NULL);
        (void)term_registry_feed(big, OWNER_A, (const uint8_t *)row, rowlen, NULL);
        reg_frame(); reg_frame(); reg_frame(); reg_frame();
    }

    p5_mark(&a);
    CHK_INT(term_registry_record(big, OWNER_A, true), TERM_OK);
    p5_mark(&b);
    CHK_INT(P5_DS(rec_screen_trunc), 1);
    CHK_INT(P5_DS(rec_screens), 1);
    CHK_TRUE(P5_DS(rec_screen_rows) > 0);
    CHK_TRUE(P5_DS(rec_screen_rows) < 53);
    CHK_RANGE(P5_D(scr_bytes), 1, (long)TERM_REC_SCREEN_MAX);
    CHK_INT(P5_DS(rec_lines), 0);
    CHK_INT(P5_DS(rec_panic_rows), 0);

    t_case("...and a capture of the same screen after a shrink is not truncated");
    CHK_INT(term_registry_resize(big, OWNER_A, 40, 6), TERM_OK);
    reg_frame();
    p5_mark(&a);
    CHK_INT(term_registry_record_screen(big, OWNER_A), TERM_OK);
    p5_mark(&b);
    CHK_INT(P5_DS(rec_screen_trunc), 0);
    CHK_INT(P5_DS(rec_screens), 1);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* 9. rec_panic_rows stays zero for everything that is not the panic path */
/* ===================================================================== */

static void case_rec_panic_rows_is_the_panic_path_only(void)
{
    term_id_t id;
    p5_mark_t a, b;
    static p4_prod_t prod;
    term_producer_t pt;

    t_case("rec_panic_rows: nothing but the panic path moves it");
    CHK_TRUE(reg_boot());
    id = glass_term("s7", 24, 4, 2);
    REQUIRE(id != TERM_ID_INVALID);
    p4_prod_reset(&prod, "s7");
    pt = p4_producer(&prod, false);

    p5_mark(&a);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    p5_feed_lines(id, OWNER_A, "PN", 0, 10);
    CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_OK);
    p5_settle_after_rate_limit();
    p5_repaint(id, OWNER_A, 4, 1, "PR");
    CHK_INT(term_registry_pipe(id, OWNER_A, &pt), TERM_OK);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    reg_settle();
    p5_mark(&b);
    CHK_INT(P5_DS(rec_panic_rows), 0);
    /* ...while everything else in the feature moved: the zero above is not the
     * zero of a suite that did nothing. */
    CHK_TRUE(P5_DS(rec_on) >= 3);
    CHK_TRUE(P5_DS(rec_cleared) >= 2);
    CHK_TRUE(P5_DS(rec_lines) >= 6);
    CHK_TRUE(P5_DS(rec_screens) >= 4);
    CHK_TRUE(P5_DS(rec_screen_rows) >= 4);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* And the table's own lifetime                                          */
/* ===================================================================== */

static void case_stats_belong_to_the_table(void)
{
    term_id_t id;
    term_registry_stats_t s;

    t_case("the rec_* counters are the table's, so a new table starts at zero");
    CHK_TRUE(reg_boot());
    id = glass_term("s8", 24, 4, 2);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    p5_feed_lines(id, OWNER_A, "TB", 0, 8);
    s = reg_stats();
    CHK_TRUE(s.rec_on >= 1);
    CHK_TRUE(s.rec_lines >= 4);
    CHK_INT(reg_shutdown(), 0);

    CHK_TRUE(reg_boot());
    s = reg_stats();
    CHK_INT((int)s.rec_on, 0);
    CHK_INT((int)s.rec_off, 0);
    CHK_INT((int)s.rec_cleared, 0);
    CHK_INT((int)s.rec_lines, 0);
    CHK_INT((int)s.rec_screens, 0);
    CHK_INT((int)s.rec_screen_rows, 0);
    CHK_INT((int)s.rec_snaps_deferred, 0);
    CHK_INT((int)s.rec_screen_trunc, 0);
    CHK_INT((int)s.rec_panic_rows, 0);
    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("rec_stats");
    case_rec_on();
    case_rec_off_and_cleared_are_disjoint();
    case_rec_lines();
    case_rec_screens_and_rows();
    case_rec_snaps_deferred();
    case_rec_screen_trunc();
    case_rec_panic_rows_is_the_panic_path_only();
    case_stats_belong_to_the_table();
    p5_check_live_layout();
    fp_reclaim_all();
    return t_summary();
}
