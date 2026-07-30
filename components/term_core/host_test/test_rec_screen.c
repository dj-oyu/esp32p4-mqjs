/*
 * test_rec_screen.c — R3: whole screens, and the four moments they are taken.
 *
 * Contracts under test (term_registry.h unless noted):
 *   R3        "A line tee cannot see content that is displayed and never
 *     scrolls off — precisely the motivating case ... Three capture triggers
 *     close that hole: (a) MANUAL ... (b) SETTLE-DEBOUNCED ... (c) END OF
 *     SESSION and THE PANIC PATH"; "A capture is a marker record plus one
 *     record per row, all tagged TERM_LP_CLASS_TERM | TERM_LP_F_SCREEN and
 *     bounded by TERM_REC_SCREEN_MAX bytes"
 *   record()  "`on` = true installs the line tee and arms the settle-debounced
 *     screen capture, and takes an IMMEDIATE screen capture: the human pressed
 *     the button because of what is on the glass right now"; "`on` = false
 *     takes a final capture and then removes the tee"; "Idempotent: setting the
 *     state it already has does nothing at all (no capture, no counter)"
 *   R3(b)     "when nothing has been fed for TERM_REC_SETTLE_MS and
 *     term_core_screen_hash() differs from the last captured screen, capture
 *     it ... Rate-limited to one landing per TERM_REC_SNAP_MIN_MS ... skipped
 *     evaluations are counted (rec_snaps_deferred)"
 *   record_screen()  "It does NOT reset the debounce rate limit — a human
 *     pressing a button is not the case that limit exists for."
 *   term_core.h  term_core_screen_hash: "NOT included: the cursor position and
 *     visibility ... two calls with no intervening grid mutation agree"
 *   docs §4.4 記録モード, the counterintuitive clause: 記録を入れる前に表示され
 *     た内容も、その時点で画面に残っていれば箱に入る … オフ期間にスクロール
 *     し去った行は入らない
 */
#include "rec_util.h"

static p5_bag_t bag;

#define ARM(id, owner)                                                        \
    do {                                                                      \
        CHK_INT(term_registry_record((id), (owner), true), TERM_OK);          \
        REQUIRE(p5_recording((id), (owner)) == 1);                            \
    } while (0)

/*
 * The records of the LAST capture in the region.
 *
 * R3 defines a capture as "a marker record plus one record per row", and
 * consecutive captures sit directly against each other in the ring, so the
 * group boundary is the marker and not a gap: walk back to the last marker and
 * take everything after it. `out_marker` receives the marker's text.
 */
static void last_capture(p5_lines_t *rows, char *marker, size_t markern)
{
    unsigned k, first = 0;
    int found = 0;
    memset(rows, 0, sizeof *rows);
    if (markern) marker[0] = 0;
    p5_collect(&bag);
    for (k = bag.n; k > 0; k--) {
        if (p5_is_marker(&bag.r[k - 1])) { first = k - 1; found = 1; break; }
    }
    if (!found) return;
    if (markern) {
        strncpy(marker, bag.r[first].text, markern - 1);
        marker[markern - 1] = 0;
    }
    for (k = first + 1; k < bag.n; k++) {
        const p5_rec_t *d = &bag.r[k];
        if (!p5_is_scr(d) || p5_is_marker(d)) break;
        if (rows->n < P5_MAX_LINES) {
            memcpy(rows->line[rows->n], d->text, d->len);
            rows->line[rows->n][d->len] = 0;
            rows->n++;
        }
    }
}

/* Does the capture that just landed hold exactly the rows on the glass? */
static void check_capture_matches_glass(term_id_t id, const char *owner,
                                        long screens_delta, long rows_delta,
                                        long scr_delta)
{
    p5_lines_t glass, got;
    char marker[TERM_LP_REC_MAX + 1];
    int i;

    p5_screen_lines(&glass, id, owner);
    last_capture(&got, marker, sizeof marker);

    /* R3's structure: one marker plus one record per row. */
    CHK_INT(screens_delta, 1);
    CHK_INT(scr_delta, rows_delta + 1);
    CHK_INT(got.n, (int)rows_delta);
    CHK_INT(got.n, glass.n);
    for (i = 0; i < got.n && i < glass.n; i++)
        CHK_STR(got.line[i], glass.line[i]);

    /* The marker's wording is PHASE5_MANIFEST's, not the header's — it is what
     * tools/bb_pull.py --session splits a transcript on, so it is checked, but
     * only for its documented shape. */
    CHK_TRUE(strncmp(marker, P5_MARKER_PREFIX, strlen(P5_MARKER_PREFIX)) == 0);
    CHK_TRUE(strstr(marker, "rows=") != NULL);
}

/* ===================================================================== */
/* Arming: exactly one screen's worth of the past                         */
/* ===================================================================== */

/*
 * The counterintuitive clause of the privacy contract, both halves. §4.4 says
 * so in as many words, and the manifest records that the orchestrator
 * cross-checked it on the JS side: what is ON THE GLASS when you arm lands in
 * the box; what scrolled away while recording was off does not.
 *
 * If this ever drifts, the app's confirmation page becomes a lie in one
 * direction or the feature stops solving the case it was built for in the
 * other, so both halves are pinned here.
 */
static void case_arming_captures_one_screen_of_the_past(void)
{
    term_id_t id;
    p5_mark_t a, b;
    p5_lines_t glass;
    int i;

    t_case("arming captures what is on the glass — including pre-arm content");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("t0", OWNER_A, 24, 4);
    REQUIRE(id != TERM_ID_INVALID);

    /* 20 lines with recording OFF on a 4-row screen: 16 have scrolled away
     * and 4 are still visible. */
    p5_feed_lines(id, OWNER_A, "GONE", 0, 16);
    p5_feed(id, OWNER_A, "VISIBLE1\r\nVISIBLE2\r\nVISIBLE3\r\n");
    p5_screen_lines(&glass, id, OWNER_A);
    CHK_TRUE(glass.n >= 3);

    p5_mark(&a);
    ARM(id, OWNER_A);
    p5_mark(&b);

    t_case("...the visible rows landed");
    check_capture_matches_glass(id, OWNER_A, P5_DS(rec_screens),
                                P5_DS(rec_screen_rows), P5_D(scr));
    CHK_TRUE(p5_region_hits("VISIBLE1") >= 1);
    CHK_TRUE(p5_region_hits("VISIBLE3") >= 1);

    t_case("...the rows that scrolled away while off did NOT (the line tee "
           "was inert, so the reach is exactly one screen)");
    for (i = 0; i < 16; i++) {
        char needle[32];
        sprintf(needle, "GONE%03d", i);
        CHK_INT((int)p5_region_hits(needle), 0);
    }
    CHK_INT(P5_D(ses), 0);      /* no archived line came in with the arming */
    P5_CHK_RECD(&a, &b, "arming", { 1, 0, 0, 0, 1, (long)glass.n, 0, 0, 0 });

    t_case("arming is idempotent: doing it again captures nothing (header)");
    p5_mark(&a);
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    CHK_INT(p5_recording(id, OWNER_A), 1);
    p5_mark(&b);
    CHK_INT(P5_D(scr), 0);
    P5_CHK_RECD(&a, &b, "arming an armed term", P5_RECD_ZERO);

    t_case("arming a blank screen writes the marker and no rows");
    {
        term_id_t blank = p5_new_vt("bl", OWNER_A, 24, 4);
        REQUIRE(blank != TERM_ID_INVALID);
        p5_mark(&a);
        ARM(blank, OWNER_A);
        p5_mark(&b);
        /* The marker alone still says "recording started here", which is what
         * a transcript needs; an empty row cannot become a record at all
         * because term_lp_ring_append refuses an empty payload. */
        CHK_INT(P5_DS(rec_screens), 1);
        CHK_INT(P5_DS(rec_screen_rows), 0);
        CHK_INT(P5_D(scr), 1);
    }

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Manual and end-of-recording                                           */
/* ===================================================================== */

static void case_manual_capture(void)
{
    term_id_t id;
    p5_mark_t a, b;

    t_case("record_screen captures the screen now (R3a)");
    CHK_TRUE(reg_boot());
    /* 40 columns, so the error message below occupies one row and the case is
     * about the capture rather than about soft wrapping. */
    id = p5_new_vt("t0", OWNER_A, 40, 4);
    REQUIRE(id != TERM_ID_INVALID);
    ARM(id, OWNER_A);
    /* An error message painted in place, exactly the motivating case: nothing
     * scrolls, so the line tee will never see it. */
    p5_feed(id, OWNER_A, "\x1b[2J\x1b[1;1HE5108: Error executing lua\r\n"
                         "stack traceback:");
    p5_mark(&a);
    CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_OK);
    p5_mark(&b);
    check_capture_matches_glass(id, OWNER_A, P5_DS(rec_screens),
                                P5_DS(rec_screen_rows), P5_D(scr));
    CHK_INT(P5_D(ses), 0);            /* nothing scrolled                   */
    CHK_TRUE(p5_region_hits("E5108: Error executing lua") >= 1);
    P5_CHK_RECD(&a, &b, "manual capture", { 0, 0, 0, 0, 1, 2, 0, 0, 0 });

    t_case("...and it can be pressed twice in a row (no rate limit on a human)");
    p5_mark(&a);
    CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_OK);
    p5_mark(&b);
    CHK_INT(P5_DS(rec_screens), 2);

    t_case("turning recording OFF keeps the frame the user was looking at");
    p5_feed(id, OWNER_A, "\x1b[2J\x1b[1;1HFINALFRAME");
    p5_mark(&a);
    CHK_INT(term_registry_record(id, OWNER_A, false), TERM_OK);
    CHK_INT(p5_recording(id, OWNER_A), 0);
    p5_mark(&b);
    check_capture_matches_glass(id, OWNER_A, P5_DS(rec_screens),
                                P5_DS(rec_screen_rows), P5_D(scr));
    CHK_TRUE(p5_region_hits("FINALFRAME") >= 1);
    P5_CHK_RECD(&a, &b, "explicit off", { 0, 1, 0, 0, 1, 1, 0, 0, 0 });

    t_case("...and after that the tee is really gone");
    p5_show(id, OWNER_A);
    CHK_INT(p5_core_recording(), 0);
    p5_mark(&a);
    p5_feed_lines(id, OWNER_A, "AFTEROFF", 0, 20);
    p5_settle_after_rate_limit();
    p5_mark(&b);
    CHK_INT(P5_D(ses), 0);
    CHK_INT(P5_D(scr), 0);
    CHK_INT((int)p5_region_hits("AFTEROFF"), 0);
    P5_CHK_RECD(&a, &b, "traffic after an explicit off", P5_RECD_ZERO);

    t_case("record(false) twice is idempotent: no second capture");
    p5_mark(&a);
    CHK_INT(term_registry_record(id, OWNER_A, false), TERM_OK);
    p5_mark(&b);
    P5_CHK_RECD(&a, &b, "off on an off term", P5_RECD_ZERO);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The settle debounce                                                   */
/* ===================================================================== */

static void case_settle_timing(void)
{
    term_id_t id;
    p5_mark_t a, b;

    t_case("a settled screen is captured, but not before TERM_REC_SETTLE_MS");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("t0", OWNER_A, 24, 4);
    REQUIRE(id != TERM_ID_INVALID);
    ARM(id, OWNER_A);                       /* capture 1, and the rate anchor */
    p5_settle_after_rate_limit();            /* let the rate limit open       */

    p5_feed(id, OWNER_A, "\x1b[2J\x1b[1;1HSETTLING");
    p5_mark(&a);
    p5_quiet((int64_t)TERM_REC_SETTLE_MS - 100, 4);
    p5_mark(&b);
    CHK_INT(P5_DS(rec_screens), 0);
    CHK_INT(P5_D(scr), 0);
    CHK_INT((int)p5_region_hits("SETTLING"), 0);

    t_case("...and it is captured once the interval has passed");
    p5_mark(&a);
    p5_quiet(200, 2);
    p5_mark(&b);
    check_capture_matches_glass(id, OWNER_A, P5_DS(rec_screens),
                                P5_DS(rec_screen_rows), P5_D(scr));
    CHK_TRUE(p5_region_hits("SETTLING") >= 1);
    P5_CHK_RECD(&a, &b, "the settle capture", { 0, 0, 0, 0, 1, 1, 0, 0, 0 });

    t_case("an unchanged screen never settles again, however long you wait");
    p5_mark(&a);
    {
        int i;
        for (i = 0; i < 20; i++) p5_settle_after_rate_limit();
    }
    p5_mark(&b);
    CHK_INT(P5_D(scr), 0);
    P5_CHK_RECD(&a, &b, "20 settle windows over an unchanged screen",
                P5_RECD_ZERO);

    t_case("...and a cursor move is not a change (term_core_screen_hash)");
    p5_mark(&a);
    p5_feed(id, OWNER_A, "\x1b[4;20H");     /* the caret moves, no cell does */
    p5_settle_after_rate_limit();
    p5_mark(&b);
    CHK_INT(P5_D(scr), 0);
    CHK_INT(P5_DS(rec_screens), 0);

    t_case("...but one changed cell is");
    p5_mark(&a);
    p5_feed(id, OWNER_A, "\x1b[1;1HZ");
    p5_settle_after_rate_limit();
    p5_mark(&b);
    CHK_INT(P5_DS(rec_screens), 1);

    CHK_INT(reg_shutdown(), 0);
}

static void case_settle_rate_limit(void)
{
    term_id_t id;
    p5_mark_t a, b;
    int i;
    long landed, deferred;

    t_case("a second settle inside TERM_REC_SNAP_MIN_MS is deferred and counted");
    CHK_TRUE(reg_boot());
    id = p5_new_vt("t0", OWNER_A, 24, 4);
    REQUIRE(id != TERM_ID_INVALID);
    ARM(id, OWNER_A);
    p5_settle_after_rate_limit();
    p5_feed(id, OWNER_A, "\x1b[2J\x1b[1;1HFIRST");
    p5_settle();                                  /* lands: the limit is open */

    p5_mark(&a);
    p5_feed(id, OWNER_A, "\x1b[2J\x1b[1;1HSECOND");
    p5_settle();                                  /* 1.25s later: too soon    */
    p5_mark(&b);
    CHK_INT(P5_DS(rec_screens), 0);
    CHK_INT(P5_D(scr), 0);
    CHK_INT(P5_DS(rec_snaps_deferred), 1);
    CHK_INT((int)p5_region_hits("SECOND"), 0);

    t_case("...counted ONCE per settle episode, not once per frame (manifest)");
    p5_mark(&a);
    for (i = 0; i < 10; i++) p5_quiet(100, 3);    /* still inside the limit  */
    p5_mark(&b);
    CHK_INT(P5_DS(rec_snaps_deferred), 0);
    CHK_INT(P5_DS(rec_screens), 0);

    t_case("...and the screen lands once the limit opens, without new bytes");
    p5_mark(&a);
    p5_quiet((int64_t)TERM_REC_SNAP_MIN_MS, 3);
    p5_mark(&b);
    CHK_INT(P5_DS(rec_screens), 1);
    CHK_TRUE(p5_region_hits("SECOND") >= 1);

    t_case("over a long alternating run, captures are bounded by the rate cap");
    /* This is the claim that the debounce "cannot starve the ring": 20
     * changed-then-settled screens over 20 * SETTLE ms may land at most
     * elapsed / SNAP_MIN + 1 times. */
    p5_mark(&a);
    for (i = 0; i < 20; i++) {
        char buf[64];
        sprintf(buf, "\x1b[2J\x1b[1;1HALT%02d", i);
        p5_feed(id, OWNER_A, buf);
        p5_settle();
    }
    p5_mark(&b);
    landed = P5_DS(rec_screens);
    deferred = P5_DS(rec_snaps_deferred);
    CHK_TRUE(landed >= 1);
    {
        /* elapsed / SNAP_MIN + 1 landings, at most. The guard on the divisor is
         * not paranoia: the constant is tuning (the header says so), and a
         * suite that divides by it crashes instead of failing when somebody
         * tries 0. */
        long span = (long)(20 * (TERM_REC_SETTLE_MS + 50));
        long cap = TERM_REC_SNAP_MIN_MS ? (long)TERM_REC_SNAP_MIN_MS : 1;
        CHK_RANGE(landed, 1, span / cap + 1);
    }
    CHK_INT(landed + deferred, 20);   /* every episode either landed or was
                                       * turned away, exactly once */

    CHK_INT(reg_shutdown(), 0);
}

/*
 * The header is explicit that a human's button press is not what the rate
 * limit is for: "It does NOT reset the debounce rate limit". So a manual
 * capture must not be able to suppress the settle capture that follows it.
 *
 * The A/B is the assertion: the same sequence with and without the manual
 * capture in the middle must produce the same settle capture.
 */
static long ab_settle_after(int with_manual)
{
    term_id_t id;
    p5_mark_t a, b;
    long screens;

    CHK_TRUE(reg_boot());
    id = p5_new_vt("t0", OWNER_A, 24, 4);
    if (id == TERM_ID_INVALID) return -1;
    CHK_INT(term_registry_record(id, OWNER_A, true), TERM_OK);
    p5_settle_after_rate_limit();
    p5_feed(id, OWNER_A, "\x1b[2J\x1b[1;1HABFIRST");
    p5_settle();                          /* a settle capture lands here     */
    p5_settle_after_rate_limit();          /* ...and the limit opens again    */

    p5_mark(&a);
    if (with_manual) CHK_INT(term_registry_record_screen(id, OWNER_A), TERM_OK);
    p5_feed(id, OWNER_A, "\x1b[2J\x1b[1;1HABSECOND");
    p5_settle();
    p5_mark(&b);
    screens = P5_DS(rec_screens) - (with_manual ? 1 : 0);
    CHK_INT(reg_shutdown(), 0);
    return screens;
}

static void case_manual_does_not_reset_the_rate_limit(void)
{
    long without, with;

    t_case("CONTROL ARM: with nothing in the middle, the settle capture lands");
    without = ab_settle_after(0);
    CHK_INT(without, 1);

    t_case("term_registry_record_screen 'does NOT reset the debounce rate "
           "limit' (term_registry.h) — pressing the button must not lose the "
           "settle capture that follows it");
    with = ab_settle_after(1);
    CHK_INT(with, without);
}

/* ===================================================================== */
/* The size bound                                                        */
/* ===================================================================== */

static void case_screen_max_truncates_on_a_row_boundary(void)
{
    term_create_opts_t o;
    term_id_t id = TERM_ID_INVALID;
    p5_mark_t a, b;
    p5_lines_t rows;
    char marker[TERM_LP_REC_MAX + 1];
    char row[142 * 3 + 8];
    size_t rowlen = 0;
    int r, i;

    t_case("a capture is bounded by TERM_REC_SCREEN_MAX and counts itself");
    CHK_TRUE(reg_boot());
    memset(&o, 0, sizeof o);
    o.name = "big"; o.owner = OWNER_A; o.mode = TERM_VT;
    o.cols = 142; o.rows = 53;
    o.max_cols = 142; o.max_rows = 53; o.max_cells = 142 * 53;
    CHK_INT(term_registry_create(&o, &id, NULL), TERM_OK);
    REQUIRE(id != TERM_ID_INVALID);

    /* The worst geometry the design sizes for, filled with 3-byte codepoints:
     * 142 * 3 = 426 bytes per row, 53 rows = 22,578 bytes of display content
     * against an 8,192-byte cap. */
    for (i = 0; i < 142; i++) { memcpy(row + rowlen, "\xe2\x94\x80", 3); rowlen += 3; }
    row[rowlen] = 0;
    for (r = 1; r <= 53; r++) {
        char cu[32];
        sprintf(cu, "\x1b[%d;1H", r);
        CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)cu, strlen(cu), NULL),
                TERM_OK);
        CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)row, rowlen, NULL),
                TERM_OK);
        reg_frame(); reg_frame(); reg_frame(); reg_frame();
    }

    p5_mark(&a);
    ARM(id, OWNER_A);
    p5_mark(&b);

    CHK_INT(P5_DS(rec_screens), 1);
    CHK_INT(P5_DS(rec_screen_trunc), 1);
    CHK_TRUE(P5_DS(rec_screen_rows) > 0);
    CHK_TRUE(P5_DS(rec_screen_rows) < 53);      /* it really was cut short   */
    CHK_INT(P5_D(scr), P5_DS(rec_screen_rows) + 1);

    t_case("...the cap counts the payload it wrote, marker included");
    CHK_TRUE(P5_D(scr_bytes) > 0);
    CHK_RANGE(P5_D(scr_bytes), 1, (long)TERM_REC_SCREEN_MAX);

    t_case("...and it stopped on a ROW boundary, never mid-row or mid-codepoint");
    last_capture(&rows, marker, sizeof marker);
    CHK_TRUE(rows.n > 0);
    for (i = 0; i < rows.n; i++) {
        CHK_INT((int)strlen(rows.line[i]), (int)rowlen);   /* a whole row     */
        CHK_TRUE(lp_utf8_valid(rows.line[i], strlen(rows.line[i])));
    }
    CHK_INT(rows.n, (int)P5_DS(rec_screen_rows));

    t_case("...and a row that is longer than one record is truncated, not split");
    /* 426 bytes fits TERM_LP_REC_MAX (512), so this geometry must NOT trip
     * F_TRUNC on a record — the cap that bit was the capture's, not the
     * record's. The two bounds are separate and a suite should say which. */
    p5_collect(&bag);
    {
        unsigned k, trunc_recs = 0;
        for (k = 0; k < bag.n; k++)
            if (p5_is_scr(&bag.r[k]) && (bag.r[k].flags & TERM_LP_F_TRUNC))
                trunc_recs++;
        CHK_INT((int)trunc_recs, 0);
    }

    t_case("...and every record of the capture is TERM|F_SCREEN in APP");
    for (i = 0; i < (int)bag.n; i++) {
        const p5_rec_t *d = &bag.r[i];
        if (!p5_is_scr(d)) continue;
        CHK_INT(d->part, TERM_LP_PART_APP);
        CHK_INT((int)d->cls, (int)TERM_LP_CLASS_TERM);
        CHK_TRUE((d->flags & TERM_LP_F_SCREEN) != 0);
        CHK_TRUE(d->len <= TERM_LP_REC_MAX);
    }

    t_case("...and the region still walks: a reader can decode class 2");
    p5_check_live_layout();

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* term_core_screen_hash's own contract                                  */
/* ===================================================================== */

static void case_screen_hash_properties(void)
{
    tcore_t t;
    uint32_t h0, h1;

    t_case("screen_hash: two calls with no mutation between them agree");
    t = tc_make(TERM_VT, 24, 6);
    REQUIRE(t.c != NULL);
    feed(t.c, "hash me\r\nsecond row");
    h0 = term_core_screen_hash(t.c);
    CHK_HEX(term_core_screen_hash(t.c), h0);
    CHK_HEX(term_core_screen_hash(t.c), h0);

    t_case("...a cursor move alone does not change it (the caret is excluded)");
    cup(t.c, 5, 10);
    CHK_HEX(term_core_screen_hash(t.c), h0);
    feed(t.c, "\x1b[?25l");                    /* hide the cursor            */
    CHK_HEX(term_core_screen_hash(t.c), h0);
    feed(t.c, "\x1b[?25h");
    CHK_HEX(term_core_screen_hash(t.c), h0);

    t_case("...a changed cell does");
    cup(t.c, 1, 1);
    feed(t.c, "X");
    h1 = term_core_screen_hash(t.c);
    CHK_TRUE(h1 != h0);

    t_case("...so does a colour change on the same character (attributes count)");
    cup(t.c, 1, 1);
    feed(t.c, "\x1b[31mX\x1b[0m");
    CHK_TRUE(term_core_screen_hash(t.c) != h1);

    t_case("...so does the geometry, even with identical content");
    h1 = term_core_screen_hash(t.c);
    CHK_INT(term_core_resize(t.c, 30, 6), 0);
    CHK_TRUE(term_core_screen_hash(t.c) != h1);

    t_case("...and the alt screen is a different screen");
    h1 = term_core_screen_hash(t.c);
    feed(t.c, "\x1b[?1049h");
    CHK_TRUE(term_core_screen_hash(t.c) != h1);
    feed(t.c, "\x1b[?1049l");
    CHK_HEX(term_core_screen_hash(t.c), h1);

    t_case("...and reading the hash never mutates the core");
    {
        char why[256];
        int dirty_before = term_core_dirty_next(t.c, 0);
        (void)term_core_screen_hash(t.c);
        CHK_INT(term_core_dirty_next(t.c, 0), dirty_before);
        CHK_INT(invariants_ok(t.c, why, sizeof why), 0);
        CHK_INT(tc_guards_intact(&t), 1);
    }
    tc_free(&t);
}

int main(void)
{
    t_suite("rec_screen");
    case_arming_captures_one_screen_of_the_past();
    case_manual_capture();
    case_settle_timing();
    case_settle_rate_limit();
    case_manual_does_not_reset_the_rate_limit();
    case_screen_max_truncates_on_a_row_boundary();
    case_screen_hash_properties();
    fp_reclaim_all();
    return t_summary();
}
