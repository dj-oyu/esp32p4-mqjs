/*
 * test_rec_lines.c — R2: the tee is at the line level, and what that buys.
 *
 * Contracts under test:
 *   term_registry.h R2   "THE TEE IS AT THE LINE LEVEL, NOT THE BYTE STREAM.
 *     term_core's term_record_fn fires once per grid row archived to
 *     scrollback ... a redraw storm therefore costs nothing (in-place painting
 *     archives no rows, and the alt screen never reaches scrollback)"
 *   term_core.h          term_record_fn: "with that row's text — no
 *     attributes, no escapes, CONT halves already resolved, exactly the bytes
 *     term_core_line_utf8 would give back"; "ONE ROW PER CALL, so a
 *     soft-wrapped logical line arrives as consecutive calls"; "Runs inside
 *     term_core_feed()/term_core_resize()"
 *   term_lp_ring.h       TERM_LP_CLASS_TERM lands in the APP partition;
 *     TERM_LP_F_SCREEN marks capture rows, so a scrolled line must NOT carry
 *     it; P4 (no escape byte survives an append); TERM_LP_REC_MAX truncation
 *     backs off to a UTF-8 boundary and sets TERM_LP_F_TRUNC
 *   PHASE5_MANIFEST §6   the writer id is "<owner>:<name>", degrading to the
 *     bare owner and then to WRITER_UNKNOWN — "attribution only, never
 *     content"
 *
 * The cost claim is the load-bearing one: it is why the feature was allowed to
 * exist at the line seam at all, and a regression that started teeing raw
 * bytes would burn the 23.5 KiB partition in seconds without failing any
 * functional test. Hence case_redraw_storm_archives_nothing, which asserts a
 * zero over a screen that provably changed under it.
 */
#include "rec_util.h"

static p5_bag_t bag;

/* Arm and fail the case if it did not take. */
#define ARM(id, owner)                                                        \
    do {                                                                      \
        CHK_INT(term_registry_record((id), (owner), true), TERM_OK);          \
        REQUIRE(p5_recording((id), (owner)) == 1);                            \
    } while (0)

/* ===================================================================== */
/* The happy path: an archived row becomes exactly one record            */
/* ===================================================================== */

static void case_archived_rows_are_teed(void)
{
    term_id_t id;
    p5_mark_t a, b;
    unsigned k, ses = 0, checked = 0;
    char out[1024];
    term_read_result_t res;

    t_case("every row archived to scrollback becomes one class-TERM record");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("t0", OWNER_A, 24, 4);
    REQUIRE(id != TERM_ID_INVALID);
    ARM(id, OWNER_A);

    p5_mark(&a);
    p5_feed_lines(id, OWNER_A, "TEE", 0, 12);   /* 4 rows on screen, 8 archived */
    p5_mark(&b);

    /* The archive count is arithmetic the design does not fix (how many rows a
     * 4-row screen has pushed off after 12 lines), so the assertion ties the
     * counter to the records rather than to a number of my own. */
    CHK_TRUE(P5_D(ses) > 0);
    CHK_INT(P5_DS(rec_lines), P5_D(ses));
    CHK_INT(P5_D(scr), 0);                 /* no capture was triggered      */
    CHK_INT(P5_D(term_in_sys), 0);         /* session content never in SYS  */

    t_case("...as class TERM, without F_SCREEN, in the APP partition");
    p5_collect(&bag);
    for (k = 0; k < bag.n; k++) {
        const p5_rec_t *d = &bag.r[k];
        if (!p5_is_ses(d)) continue;
        ses++;
        CHK_INT(d->part, TERM_LP_PART_APP);
        CHK_INT((int)d->cls, (int)TERM_LP_CLASS_TERM);
        CHK_INT((int)(d->flags & TERM_LP_F_SCREEN), 0);
        CHK_STR(d->writer, p5_writer_of(OWNER_A, "t0"));
    }
    CHK_INT((int)ses, (int)P5_D(ses));

    t_case("...with the archived text, in order, matching the scrollback");
    memset(out, 0, sizeof out);
    memset(&res, 0, sizeof res);
    CHK_INT(term_registry_read(id, OWNER_A, 0, 8, out, sizeof out, &res), TERM_OK);
    CHK_TRUE(res.lines > 0);
    {
        /* Walk the '\n'-joined scrollback and the ses records together. This
         * is the first case of the suite, so the i-th class-TERM record of the
         * region really is the i-th record this term produced. */
        char *p = out;
        unsigned i = 0;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            if (*p) {
                CHK_STR(p5_nth_text(&bag, p5_is_ses, i), p);
                i++;
                checked++;
            }
            p = nl ? nl + 1 : NULL;
        }
    }
    CHK_TRUE(checked >= 4);
    CHK_TRUE(p5_region_hits("TEE000") >= 1);

    t_case("the rows still on the glass have NOT been teed (they never left)");
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "TEE011") != NULL);
    CHK_INT((int)p5_region_hits("TEE011"), 0);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* A piped session is teed the same way                                  */
/* ===================================================================== */

static void case_piped_session_is_teed(void)
{
    term_id_t id;
    static p4_prod_t prod;
    term_producer_t pt;
    p5_mark_t a, b;
    size_t wrote = 0;

    t_case("a producer's bytes are teed too — the pipe is the real case");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("t0", OWNER_A, 24, 4);
    REQUIRE(id != TERM_ID_INVALID);
    p4_prod_reset(&prod, "pp");
    pt = p4_producer(&prod, false);
    CHK_INT(term_registry_pipe(id, OWNER_A, &pt), TERM_OK);
    /* Armed AFTER the bind: a bind clears, so this is the order a human on
     * the device necessarily uses. */
    ARM(id, OWNER_A);

    p5_mark(&a);
    CHK_INT(term_registry_producer_write(id, &prod,
                (const uint8_t *)"PIPE1\r\nPIPE2\r\nPIPE3\r\nPIPE4\r\nPIPE5\r\nPIPE6\r\n",
                42, &wrote), TERM_OK);
    CHK_INT((int)wrote, 42);
    p5_feed_frames(id, OWNER_A, "", 6);
    p5_mark(&b);
    CHK_TRUE(P5_D(ses) >= 2);
    CHK_INT(P5_DS(rec_lines), P5_D(ses));
    p5_collect(&bag);
    CHK_TRUE(p5_bag_with(&bag, "PIPE1") >= 1);
    {
        unsigned k, seen = 0;
        for (k = 0; k < bag.n; k++) {
            const p5_rec_t *d = &bag.r[k];
            if (!p5_is_ses(d) || strncmp(d->text, "PIPE", 4) != 0) continue;
            seen++;
            CHK_STR(d->writer, p5_writer_of(OWNER_A, "t0"));
        }
        CHK_TRUE(seen >= 2);
    }

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The cost claim                                                        */
/* ===================================================================== */

static void case_redraw_storm_archives_nothing(void)
{
    term_id_t id;
    p5_mark_t a, b;
    int i;
    const char *before;
    char snap_before[1024];

    t_case("200 in-place repaints of the whole screen archive NOT ONE row (R2)");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("t0", OWNER_A, 40, 8);
    REQUIRE(id != TERM_ID_INVALID);
    ARM(id, OWNER_A);
    p5_repaint(id, OWNER_A, 8, 0, "STORMTEXT");
    before = reg_snap(id, OWNER_A);
    strncpy(snap_before, before, sizeof snap_before - 1);
    snap_before[sizeof snap_before - 1] = 0;

    p5_mark(&a);
    for (i = 1; i <= 200; i++) p5_repaint(id, OWNER_A, 8, i, "STORMTEXT");
    p5_mark(&b);

    t_case("...and the screen really did change under the assertion");
    CHK_TRUE(strcmp(reg_snap(id, OWNER_A), snap_before) != 0);
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "g0200") != NULL);

    CHK_INT(P5_D(ses), 0);
    CHK_INT(P5_D(scr), 0);
    CHK_INT(P5_DS(rec_lines), 0);
    CHK_INT(P5_D(appended[TERM_LP_PART_APP]), 0);

    t_case("...a storm never settles, so it produces no capture either (R3b)");
    /* The clock has not moved: every one of those 200 frames is inside the
     * settle interval, which is the whole argument for the debounce. */
    CHK_INT(P5_DS(rec_screens), 0);
    CHK_INT(P5_DS(rec_snaps_deferred), 0);

    t_case("...but the moment it stops, the screen it left IS captured");
    p5_mark(&a);
    p5_settle_after_rate_limit();
    p5_mark(&b);
    CHK_INT(P5_DS(rec_screens), 1);
    CHK_TRUE(P5_D(scr) > 1);
    CHK_TRUE(p5_region_hits("g0200") >= 1);

    CHK_INT(reg_shutdown(), 0);
}

static void case_alt_screen_never_reaches_the_line_path(void)
{
    term_id_t id;
    p5_mark_t a, b;

    t_case("50 lines written on the alt screen archive nothing (R2/§3.2)");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("t0", OWNER_A, 24, 4);
    REQUIRE(id != TERM_ID_INVALID);
    ARM(id, OWNER_A);
    p5_feed(id, OWNER_A, "\x1b[?1049h");
    CHK_TRUE(reg_info(id).mode == TERM_VT);

    p5_mark(&a);
    p5_feed_lines(id, OWNER_A, "ALTLINE", 0, 50);
    p5_mark(&b);
    CHK_INT(P5_D(ses), 0);
    CHK_INT(P5_DS(rec_lines), 0);
    /* No capture trigger has fired, so nothing else can have carried it in
     * either: the alt screen's content is not in the region at all yet. */
    CHK_INT(P5_D(scr), 0);
    CHK_INT((int)p5_region_hits("ALTLINE000"), 0);

    t_case("...which is EXACTLY why R3 exists: a capture does reach it");
    p5_mark(&a);
    CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_OK);
    p5_mark(&b);
    CHK_INT(P5_DS(rec_screens), 1);
    CHK_TRUE(P5_D(scr) > 1);
    CHK_TRUE(p5_region_hits("ALTLINE049") >= 1);

    t_case("...and leaving the alt screen archives nothing either");
    p5_mark(&a);
    p5_feed(id, OWNER_A, "\x1b[?1049l");
    p5_mark(&b);
    CHK_INT(P5_D(ses), 0);
    CHK_INT(P5_DS(rec_lines), 0);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The other place the tee fires                                         */
/* ===================================================================== */

static void case_resize_archives_are_teed(void)
{
    term_id_t id;
    p5_mark_t a, b;

    t_case("rows pushed into scrollback by a shrink are teed (term_core.h)");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("t0", OWNER_A, 24, 6);
    REQUIRE(id != TERM_ID_INVALID);
    ARM(id, OWNER_A);
    p5_feed(id, OWNER_A, "RZ1\r\nRZ2\r\nRZ3\r\nRZ4\r\nRZ5\r\nRZ6");

    p5_mark(&a);
    CHK_INT(term_registry_resize(id, OWNER_A, 24, 2), TERM_OK);
    reg_frame();
    p5_mark(&b);
    CHK_TRUE(P5_D(ses) >= 4);
    CHK_INT(P5_DS(rec_lines), P5_D(ses));
    p5_collect(&bag);
    CHK_TRUE(p5_bag_with(&bag, "RZ1") >= 1);
    CHK_TRUE(p5_bag_with(&bag, "RZ4") >= 1);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* One row per call, and what a row's payload looks like                 */
/* ===================================================================== */

static void case_one_record_per_row(void)
{
    term_id_t id;
    p5_mark_t a, b;
    unsigned k;
    char joined[256];
    size_t o = 0;

    t_case("a soft-wrapped logical line arrives as one record per ROW");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("t0", OWNER_A, 20, 3);
    REQUIRE(id != TERM_ID_INVALID);
    ARM(id, OWNER_A);

    p5_mark(&a);
    /* 50 columns of text at 20 columns wide = three rows of one logical
     * line, then enough traffic to push all of it off the screen. */
    p5_feed(id, OWNER_A, "WRAP01234567890123456789012345678901234567890123\r\n");
    p5_feed_lines(id, OWNER_A, "PUSH", 0, 6);
    p5_mark(&b);

    /*
     * Take the record that starts the wrapped line and the ones that follow
     * it, in region order, until 48 bytes have been consumed: the header's
     * "consecutive calls" is a statement about ORDER as much as about count.
     */
    p5_collect(&bag);
    joined[0] = 0;
    {
        int started = 0;
        unsigned rows = 0;
        for (k = 0; k < bag.n; k++) {
            const p5_rec_t *d = &bag.r[k];
            if (!p5_is_ses(d)) continue;
            if (!started && strncmp(d->text, "WRAP", 4) != 0) continue;
            started = 1;
            CHK_TRUE(strlen(d->text) <= 20);     /* never wider than the row */
            if (o + strlen(d->text) + 1 >= sizeof joined) break;
            strcpy(joined + o, d->text);
            o += strlen(d->text);
            rows++;
            if (o >= 48) break;
        }
        t_case("...and each of them is a separate record, not one long one");
        CHK_INT((int)rows, 3);            /* 20 + 20 + 8 columns            */
    }
    CHK_STR(joined, "WRAP01234567890123456789012345678901234567890123");
    CHK_TRUE(P5_D(ses) >= 3);

    CHK_INT(reg_shutdown(), 0);
}

static void case_payload_is_stripped_display_text(void)
{
    term_id_t id;
    p5_mark_t a, b;
    unsigned k;

    t_case("a record holds resolved text: no escapes, no attributes (P4)");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("t0", OWNER_A, 24, 3);
    REQUIRE(id != TERM_ID_INVALID);
    ARM(id, OWNER_A);

    p5_mark(&a);
    /* Colour, a cursor move that overwrites, a wide codepoint and a CR that
     * rewrites the start of the row: what lands must be what a human saw. */
    p5_feed(id, OWNER_A, "\x1b[31mRED\x1b[0m-\xe6\x97\xa5\xe6\x9c\xac-END\r\n");
    p5_feed(id, OWNER_A, "over\rOVER\r\n");
    p5_feed_lines(id, OWNER_A, "FILL", 0, 6);
    p5_mark(&b);

    p5_collect(&bag);
    for (k = 0; k < bag.n; k++) {
        const p5_rec_t *d = &bag.r[k];
        size_t i;
        if (d->cls != TERM_LP_CLASS_TERM) continue;
        for (i = 0; i < d->len; i++) {
            CHK_TRUE((unsigned char)d->text[i] != 0x1b);
            CHK_TRUE((unsigned char)d->text[i] >= 0x20 ||
                     d->text[i] == '\n' || d->text[i] == '\t');
        }
        CHK_TRUE(lp_utf8_valid(d->text, d->len));
    }
    CHK_TRUE(p5_bag_with(&bag, "RED-\xe6\x97\xa5\xe6\x9c\xac-END") >= 1);
    CHK_TRUE(p5_bag_with(&bag, "OVER") >= 1);
    CHK_INT((int)p5_region_hits("over"), 0);   /* overwritten, never displayed */

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Attribution                                                           */
/* ===================================================================== */

static void case_two_tabs_are_separable(void)
{
    term_id_t t1, t2;
    unsigned k, n1 = 0, n2 = 0;

    t_case("two recorded tabs are separable by writer (manifest §6)");
    CHK_TRUE(reg_boot());
    t1 = p5_new_vt("tabA", OWNER_A, 24, 3);
    t2 = p5_new_vt("tabB", OWNER_A, 24, 3);
    REQUIRE(t1 != TERM_ID_INVALID && t2 != TERM_ID_INVALID);
    ARM(t1, OWNER_A);
    ARM(t2, OWNER_A);
    p5_feed_lines(t1, OWNER_A, "TABAL", 0, 8);
    p5_feed_lines(t2, OWNER_A, "TABBL", 0, 8);

    p5_collect(&bag);
    for (k = 0; k < bag.n; k++) {
        const p5_rec_t *d = &bag.r[k];
        if (!p5_is_ses(d)) continue;
        if (strcmp(d->writer, p5_writer_of(OWNER_A, "tabA")) == 0) {
            n1++;
            CHK_TRUE(strncmp(d->text, "TABAL", 5) == 0);
        } else if (strcmp(d->writer, p5_writer_of(OWNER_A, "tabB")) == 0) {
            n2++;
            CHK_TRUE(strncmp(d->text, "TABBL", 5) == 0);
        }
    }
    CHK_TRUE(n1 >= 4);
    CHK_TRUE(n2 >= 4);

    t_case("...and a second app's identically named tab is a third writer");
    {
        term_id_t t3 = p5_new_vt("tabA", OWNER_B, 24, 3);
        REQUIRE(t3 != TERM_ID_INVALID);
        ARM(t3, OWNER_B);
        p5_feed_lines(t3, OWNER_B, "TABBETA", 0, 8);
        p5_collect(&bag);
        CHK_TRUE(p5_bag_with(&bag, "TABBETA000") >= 1);
        for (k = 0; k < bag.n; k++) {
            const p5_rec_t *d = &bag.r[k];
            if (!p5_is_ses(d)) continue;
            if (strncmp(d->text, "TABBETA", 7) != 0) continue;
            CHK_STR(d->writer, p5_writer_of(OWNER_B, "tabA"));
        }
    }

    CHK_INT(reg_shutdown(), 0);
}

static void case_long_writer_degrades_to_the_owner(void)
{
    /* owner 24 + ':' + name 11 = 36 bytes, past TERM_LP_WRITER_MAX-1 (31), so
     * the pair cannot be interned whole. The manifest's rule is that
     * attribution degrades and content never does. */
    static const char *own = "app_with_a_longish_name1";
    static const char *nm  = "session_012";
    term_id_t id;
    unsigned k, seen = 0;

    t_case("a writer id too long for the table degrades to the bare owner");
    CHK_TRUE(reg_boot());
    CHK_TRUE(strlen(own) + 1 + strlen(nm) > TERM_LP_WRITER_MAX - 1);
    CHK_TRUE(strlen(own) <= TERM_LP_WRITER_MAX - 1);
    id = reg_new(nm, own, TERM_VT, false, 24, 3);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_record(id, own, true), TERM_OK);
    p5_feed_lines(id, own, "LONGW", 0, 8);

    p5_collect(&bag);
    for (k = 0; k < bag.n; k++) {
        const p5_rec_t *d = &bag.r[k];
        if (!p5_is_ses(d) || strncmp(d->text, "LONGW", 5) != 0) continue;
        seen++;
        CHK_STR(d->writer, own);
    }
    CHK_TRUE(seen >= 4);

    t_case("...and the CONTENT is complete regardless of the attribution");
    CHK_TRUE(p5_bag_with(&bag, "LONGW000") >= 1);

    CHK_INT(reg_shutdown(), 0);
}

/*
 * LAST CASE OF THE SUITE, deliberately: the writer table is part of the
 * process-wide region and interning is permanent, so filling it poisons the
 * attribution of every case that runs afterwards.
 */
static void case_writer_table_full_degrades_to_unknown(void)
{
    term_id_t id;
    term_lp_stats_t ls;
    int i;
    unsigned k, seen = 0;

    t_case("with the writer table full, attribution degrades and content does not");
    CHK_TRUE(reg_boot());
    for (i = 0; i < (int)TERM_LP_WRITERS + 2; i++) {
        char w[24];
        sprintf(w, "hog%d", i);
        (void)term_registry_platform_log(w, TERM_WCLASS_APP, "hog", 3);
    }
    memset(&ls, 0, sizeof ls);
    term_lp_stats(&ls);
    CHK_INT((int)ls.writers, (int)TERM_LP_WRITERS);

    id = p5_new_vt("wt", OWNER_A, 24, 3);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    p5_feed_lines(id, OWNER_A, "NOWRITER", 0, 8);

    p5_collect(&bag);
    for (k = 0; k < bag.n; k++) {
        const p5_rec_t *d = &bag.r[k];
        if (!p5_is_ses(d) || strncmp(d->text, "NOWRITER", 8) != 0) continue;
        seen++;
        /* term_lp_ring.h: "a record gets TERM_LP_WRITER_UNKNOWN rather than
         * somebody else's identity", and the reader resolves that to "?" —
         * never a truncated or borrowed name. */
        CHK_STR(d->writer, "?");
    }
    CHK_TRUE(seen >= 4);
    CHK_TRUE(p5_bag_with(&bag, "NOWRITER000") >= 1);
    memset(&ls, 0, sizeof ls);
    term_lp_stats(&ls);
    CHK_TRUE(ls.unnamed > 0);

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    /*
     * ORDER MATTERS HERE, and only here: the ring's writer table interns 8
     * names for the life of the PROCESS (term_lp_ring.h P2), so the cases that
     * assert an attribution have to run before the table is full. The cases
     * after them all reuse one term name, and the deliberate exhaustion is
     * last.
     */
    t_suite("rec_lines");
    case_archived_rows_are_teed();                  /* interns app_alpha:t0 */
    case_two_tabs_are_separable();                  /* +3 writers           */
    case_long_writer_degrades_to_the_owner();       /* +1 writer            */
    case_piped_session_is_teed();
    case_redraw_storm_archives_nothing();
    case_alt_screen_never_reaches_the_line_path();
    case_resize_archives_are_teed();
    case_one_record_per_row();
    case_payload_is_stripped_display_text();
    case_writer_table_full_degrades_to_unknown();   /* poisons the table */
    p5_check_live_layout();
    fp_reclaim_all();
    return t_summary();
}
