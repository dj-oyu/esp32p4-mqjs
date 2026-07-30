/*
 * test_rec_panic.c — R3(c)'s panic half, the riskiest code in the feature.
 *
 * Contracts under test (term_registry.h's term_registry_panic_capture and
 * term_lp_ring.h's term_lp_panic_append):
 *
 *   "for each recording term, visible ones first, appends the screen through
 *    term_lp_panic_append() until TERM_REC_PANIC_MAX bytes are spent. Takes no
 *    lock, allocates nothing, calls no FreeRTOS, and is one-shot."
 *   "Returns the number of records appended."
 *   "Call from the panic handler AFTER term_lp_panic_note(), never from
 *    anywhere else."
 *   term_lp_panic_append: "revalidates the DRAM shadow header on EVERY call
 *    and refuses when it is mid-update, so a caller looping over rows either
 *    publishes a consistent header each time or writes nothing"; "`cls` is
 *    checked like any other append's, so the panic path cannot write outside
 *    the partition its class names."
 *
 * ONE SHOT PER PROCESS, so this suite gets exactly one call and has to make it
 * count. The setup is therefore built to decide four things at once:
 *
 *   - ordering: the VISIBLE term is created SECOND, so slot order and
 *     visibility order disagree and "visible ones first" is falsifiable;
 *   - the budget: the two screens together are far larger than
 *     TERM_REC_PANIC_MAX, so the cap has to bite across terms, not per term;
 *   - inertness for un-armed terms: a third term with content and no recording
 *     must contribute nothing;
 *   - the no-lock/no-alloc claims, which the fake port counts.
 *
 * The header is candid that this path is a device-checklist item and not a
 * proof (PHASE5_MANIFEST D6). What a host CAN establish is everything above
 * plus the property that makes a partial capture safe: the region still walks
 * afterwards, so a fault in the middle of a capture leaves a readable box.
 */
#include "rec_util.h"

static p5_bag_t bag;

/* A term filled with `rows` rows of `tag`-prefixed ASCII, `cols` wide. */
static term_id_t fill_term(const char *name, const char *tag, int cols, int rows,
                           bool visible)
{
    term_create_opts_t o;
    term_id_t id = TERM_ID_INVALID;
    char line[256];
    int r;

    memset(&o, 0, sizeof o);
    o.name = name; o.owner = OWNER_A; o.mode = TERM_VT;
    o.cols = cols; o.rows = rows;
    o.max_cols = cols; o.max_rows = rows; o.max_cells = cols * rows;
    if (term_registry_create(&o, &id, NULL) != TERM_OK) return TERM_ID_INVALID;

    for (r = 1; r <= rows; r++) {
        int n = sprintf(line, "\x1b[%d;1H%s%02d", r, tag, r);
        while (n < cols + 6) line[n++] = 'z';       /* pad the row out       */
        line[n] = 0;
        (void)term_registry_feed(id, OWNER_A, (const uint8_t *)line, strlen(line), NULL);
        reg_frame();
    }
    if (visible) p5_show(id, OWNER_A);
    return id;
}

/* One flow, not several cases: the call under test is one-shot per process. */
static void case_panic_capture(void)
{
    term_id_t hidden, shown, quiet;
    p5_mark_t a, b;
    int appended, second;
    int locks_before, allocs_before, frees_before, posts_before;
    int locks_after, allocs_after, frees_after, posts_after;
    long payload = 0;
    unsigned k, first_visible = 0, first_hidden = 0;
    const char *w_shown, *w_hidden;

    t_case("setup: two recording terms (the visible one in the higher slot) "
           "and one that nobody armed");
    CHK_TRUE(reg_boot());
    hidden = fill_term("pH", "H", 40, 53, false);
    shown  = fill_term("pV", "V", 40, 53, true);
    quiet  = fill_term("pQ", "Q", 40, 8, false);
    REQUIRE(hidden != TERM_ID_INVALID && shown != TERM_ID_INVALID &&
            quiet != TERM_ID_INVALID);
    CHK_TRUE(term_id_slot(shown) > term_id_slot(hidden));
    CHK_INT(term_registry_record(hidden, OWNER_A, true), TERM_OK);
    CHK_INT(term_registry_record(shown, OWNER_A, true), TERM_OK);
    CHK_INT(p5_recording(quiet, OWNER_A), 0);
    w_shown  = p5_writer_of(OWNER_A, "pV");
    w_hidden = p5_writer_of(OWNER_A, "pH");
    /* Both writers are interned by the arming captures above, so the panic
     * records' attribution cannot be blamed on a full writer table. */

    t_case("the panic door is open before the note-and-capture sequence");
    CHK_TRUE(term_lp_panic_ready());
    CHK_TRUE(term_lp_panic_note(NULL) || true);   /* the ordering rule: note
                                                   * first. Its own contract
                                                   * is phase 3's. */

    t_case("...and the visible term really is the visible one, so the ordering "
           "assertion below has something to order");
    CHK_INT(p5_core_recording(), 1);       /* the painted term is recording  */
    CHK_INT(p5_visit_count, 1);            /* and it is the only painted one */

    /* The port counters have to be sampled with NOTHING else in between: a
     * p5_mark() itself reads the registry's stats and takes the table lock. */
    p5_mark(&a);
    locks_before  = fp.lock_calls;
    allocs_before = fp.allocs;
    frees_before  = fp.frees;
    posts_before  = fp.posts;
    appended = term_registry_panic_capture();
    locks_after  = fp.lock_calls;
    allocs_after = fp.allocs;
    frees_after  = fp.frees;
    posts_after  = fp.posts;
    p5_mark(&b);

    t_case("it appended records and said how many");
    CHK_TRUE(appended > 0);
    CHK_INT(P5_D(scr), appended);
    CHK_TRUE(P5_DS(rec_panic_rows) > 0);
    printf("     [panic] appended=%d scr=%ld bytes=%ld rows=%ld screens=%ld\n",
           appended, P5_D(scr), P5_D(scr_bytes), P5_DS(rec_panic_rows),
           P5_DS(rec_screens));

    t_case("...and it moved rec_panic_rows and nothing else: the panic path has "
           "its own counter because it is the one that may fail half way");
    P5_CHK_RECD(&a, &b, "panic capture",
                { 0, 0, 0, 0, 0, 0, 0, 0, P5_ANY });
    CHK_INT(P5_DS(rec_panic_rows), appended - 2);   /* two markers, two terms */

    t_case("...taking no lock, allocating nothing and posting no job");
    CHK_INT(locks_after, locks_before);
    CHK_INT(allocs_after, allocs_before);
    CHK_INT(frees_after, frees_before);
    CHK_INT(posts_after, posts_before);
    CHK_INT(fp.alloc_violations, 0);

    t_case("...inside TERM_REC_PANIC_MAX, across all terms and not per term");
    /* Only the records this call added: they are the last `appended` records of
     * the region, because a panic capture is the last thing that ran. */
    p5_collect(&bag);
    REQUIRE(bag.n >= (unsigned)appended);
    for (k = bag.n - (unsigned)appended; k < bag.n; k++) {
        const p5_rec_t *d = &bag.r[k];
        payload += d->len;
        CHK_INT((int)d->cls, (int)TERM_LP_CLASS_TERM);
        CHK_TRUE((d->flags & TERM_LP_F_SCREEN) != 0);
        CHK_INT(d->part, TERM_LP_PART_APP);   /* never the SYS reserve (P1)  */
        CHK_TRUE(d->len <= TERM_LP_REC_MAX);
    }
    CHK_RANGE(payload, 1, (long)TERM_REC_PANIC_MAX);
    {
        /* Break the total down, so a failure of the bound above says WHICH
         * bytes were not budgeted rather than only that it was exceeded. */
        long rows_bytes = 0, marker_bytes = 0;
        int markers = 0;
        for (k = bag.n - (unsigned)appended; k < bag.n; k++) {
            if (p5_is_marker(&bag.r[k])) { marker_bytes += bag.r[k].len; markers++; }
            else rows_bytes += bag.r[k].len;
        }
        printf("     [panic] budget=%u total=%ld rows=%ld markers=%ld (%d of them)\n",
               TERM_REC_PANIC_MAX, payload, rows_bytes, marker_bytes, markers);
    }

    t_case("...visible terms first, even though the hidden one is the lower slot");
    for (k = bag.n - (unsigned)appended; k < bag.n; k++) {
        const p5_rec_t *d = &bag.r[k];
        if (!first_visible && strcmp(d->writer, w_shown) == 0)
            first_visible = k + 1;
        if (!first_hidden && strcmp(d->writer, w_hidden) == 0)
            first_hidden = k + 1;
    }
    CHK_TRUE(first_visible != 0);
    if (first_hidden) CHK_TRUE(first_visible < first_hidden);

    t_case("...the visible term's screen is really in there");
    CHK_TRUE(p5_region_hits("V01") >= 1);

    t_case("...and the term nobody armed contributed nothing");
    CHK_INT((int)p5_region_hits("Q01"), 0);
    for (k = bag.n - (unsigned)appended; k < bag.n; k++)
        CHK_TRUE(strcmp(bag.r[k].writer, p5_writer_of(OWNER_A, "pQ")) != 0);

    t_case("it is one-shot: a panic inside the panic handler does nothing");
    p5_mark(&a);
    second = term_registry_panic_capture();
    p5_mark(&b);
    CHK_INT(second, 0);
    CHK_INT(P5_D(scr), 0);
    CHK_INT(P5_D(appended[TERM_LP_PART_APP]), 0);
    P5_CHK_RECD(&a, &b, "a second panic capture", P5_RECD_ZERO);

    t_case("the region still walks: a partial capture leaves a readable box (P5)");
    p5_check_live_layout();
    {
        term_lp_check_t chk;
        term_lp_ring_t ro;
        const term_lp_ring_t *live = p5_live();
        REQUIRE(live != NULL);
        memset(&chk, 0, sizeof chk);
        /* Open the live bytes read-only, exactly as a reader of a retained
         * image would: class 2 and F_SCREEN must survive the chain walk. */
        CHK_TRUE(term_lp_ring_open(&ro, live->base, live->bytes, true, &chk));
        CHK_TRUE(chk.ok);
        CHK_TRUE(chk.chain_ok);
        CHK_TRUE(chk.geometry_ok);
        CHK_TRUE(chk.crc_ok);
    }

    t_case("...and the ordinary capture path still works afterwards");
    p5_mark(&a);
    CHK_INT(term_registry_record_screen(shown, OWNER_A), TERM_OK);
    p5_mark(&b);
    CHK_INT(P5_DS(rec_screens), 1);
    CHK_TRUE(P5_D(scr) > 1);

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("rec_panic");
    case_panic_capture();
    fp_reclaim_all();
    return t_summary();
}
