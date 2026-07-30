/*
 * test_lp_writers.c — P2: every record carries {writer_id, class}
 * structurally.
 *
 * "The writer is an index into a table of interned names in the region
 * header, so a 40-byte log line does not pay 32 bytes of provenance, and the
 * name survives the eviction of the record that first introduced it. NOTHING
 * is recovered by parsing a prefix string out of the text." (P2)
 *
 * And the table's own rules, from the TERM_LP_WRITERS block:
 *   "Names are stored whole, never truncated: two distinct apps must not be
 *    able to present the same writer_id ... When the table is full a record
 *    gets TERM_LP_WRITER_UNKNOWN rather than somebody else's identity, and
 *    term_lp_stats_t::unnamed counts how often that happened."
 *   term_lp_check_t::wt_crc_ok "writer names intact (false only loses NAMES)"
 *   term_lp_ring_t::wt_ok "writer names validated; else names read '?'"
 *   PHASE3_MANIFEST §2 decision 12: names are sanitised (controls, '"' and
 *   '\' become '_'), which is what lets TERM_OWNER_SYSTEM ("\1system") appear
 *   as "_system".
 */
#include "lp_util.h"

static lp_recs_t bagA;

/* The 32-byte entry `slot` of the writer table, straight out of the region. */
static const uint8_t *wt_entry(const lpreg_t *g, unsigned slot)
{
    return g->base + TERM_LP_WT_OFF + (size_t)slot * TERM_LP_WRITER_MAX;
}

/* Is `name` stored whole and NUL-padded in one of the first `writers`
 * entries? Returns the slot, or -1. */
static int wt_find(const lpreg_t *g, const char *name)
{
    unsigned i, k;
    size_t n = strlen(name);
    if (n >= TERM_LP_WRITER_MAX) return -1;
    for (i = 0; i < g->r.hdr.writers && i < TERM_LP_WRITERS; i++) {
        const uint8_t *e = wt_entry(g, i);
        if (memcmp(e, name, n) != 0) continue;
        for (k = (unsigned)n; k < TERM_LP_WRITER_MAX; k++)
            if (e[k] != 0) break;
        if (k == TERM_LP_WRITER_MAX) return (int)i;
    }
    return -1;
}

/* ===================================================================== */
/* The tag round-trips                                                   */
/* ===================================================================== */

static void case_writer_and_class_come_back_as_written(void)
{
    lpreg_t g;

    t_case("a record returns the writer and class it was appended with");
    REQUIRE(lp_fresh(&g, 1));
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system", "sys one", 7, 11));
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, "reading", "app one", 7, 12));
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, "circuit", "app two", 7, 13));
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "reading", "sys two", 7, 14));

    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 2);
    CHK_STR(bagA.writer[0], "system");
    CHK_STR(bagA.writer[1], "reading");
    CHK_INT(bagA.cls[0], TERM_LP_CLASS_SYS);
    CHK_INT(bagA.cls[1], TERM_LP_CLASS_SYS);
    CHK_INT(bagA.t_ms[0], 11);
    CHK_INT(bagA.t_ms[1], 14);

    lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_INT(bagA.n, 2);
    CHK_STR(bagA.writer[0], "reading");
    CHK_STR(bagA.writer[1], "circuit");
    CHK_INT(bagA.cls[0], TERM_LP_CLASS_APP);
    CHK_INT(bagA.cls[1], TERM_LP_CLASS_APP);

    t_case("the same name in both classes interns once (it is a name, not a tag)");
    CHK_INT(g.r.hdr.writers, 3);
    CHK_TRUE(wt_find(&g, "system") >= 0);
    CHK_TRUE(wt_find(&g, "reading") >= 0);
    CHK_TRUE(wt_find(&g, "circuit") >= 0);
    CHK_TRUE(wt_find(&g, "absent") < 0);

    t_case("nothing is parsed out of the text: a prefix does not become a writer");
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, "realapp",
                                 "otherapp: hello", 15, 15));
    CHK_STR(lp_text_at(&g.r, TERM_LP_PART_APP, 2), "otherapp: hello");
    lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_STR(bagA.writer[2], "realapp");
    CHK_INT(g.r.hdr.writers, 4);
    CHK_TRUE(wt_find(&g, "otherapp") < 0);

    t_case("the table lives where the layout says, NUL-padded to 32 bytes");
    {
        int slot = wt_find(&g, "realapp");
        CHK_TRUE(slot >= 0);
        if (slot >= 0) {
            const uint8_t *e = wt_entry(&g, (unsigned)slot);
            CHK_INT(e[7], 0);
            CHK_INT(e[TERM_LP_WRITER_MAX - 1u], 0);
        }
    }
    CHK_TRUE(g.r.hdr.unnamed == 0);
    lp_check_layout(g.base, &g.r.hdr);
    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* Exhaustion: WRITER_UNKNOWN, never somebody else's identity            */
/* ===================================================================== */

static void case_table_exhaustion_yields_unknown(void)
{
    lpreg_t g;
    char name[32];
    unsigned i;

    t_case("eight distinct writers intern, and the ninth does not");
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < TERM_LP_WRITERS; i++) {
        sprintf(name, "app%u", i);
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_APP, name, i, 40, 100u + i));
        CHK_INT(g.r.hdr.writers, i + 1u);
    }
    CHK_INT(g.r.hdr.writers, TERM_LP_WRITERS);
    CHK_INT(g.r.hdr.unnamed, 0);

    for (i = 0; i < 3; i++) {
        sprintf(name, "late%u", i);
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_APP, name, 900u + i, 40, 200u + i));
        CHK_INT(g.r.hdr.writers, TERM_LP_WRITERS);
        CHK_INT(g.r.hdr.unnamed, i + 1u);
        CHK_TRUE(wt_find(&g, name) < 0);
    }

    t_case("the late writers read '?' — never one of the eight identities");
    lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_INT(bagA.n, TERM_LP_WRITERS + 3u);
    for (i = 0; i < TERM_LP_WRITERS && i < bagA.n; i++) {
        sprintf(name, "app%u", i);
        CHK_STR(bagA.writer[i], name);
    }
    for (i = TERM_LP_WRITERS; i < bagA.n; i++) {
        CHK_STR(bagA.writer[i], "?");
        CHK_TRUE(bagA.seq[i] >= 900);
    }

    t_case("the records themselves are complete: only the NAME was lost");
    for (i = 0; i < bagA.n; i++) {
        CHK_INT(bagA.len[i], 40);
        CHK_INT(bagA.cls[i], TERM_LP_CLASS_APP);
        CHK_INT(bagA.flags[i], 0);
    }
    lp_check_layout(g.base, &g.r.hdr);
    lp_free(&g);
}

static void case_missing_writer_is_unknown(void)
{
    lpreg_t g;

    t_case("NULL or empty writer means WRITER_UNKNOWN, counted in `unnamed`");
    REQUIRE(lp_fresh(&g, 1));
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, NULL, "no name", 7, 1));
    CHK_INT(g.r.hdr.unnamed, 1);
    CHK_INT(g.r.hdr.writers, 0);
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "", "empty name", 10, 2));
    CHK_INT(g.r.hdr.unnamed, 2);
    CHK_INT(g.r.hdr.writers, 0);

    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 2);
    CHK_INT(bagA.null_writer, 0);
    CHK_STR(bagA.writer[0], "?");
    CHK_STR(bagA.writer[1], "?");
    CHK_STR(lp_text_at(&g.r, TERM_LP_PART_SYS, 0), "no name");
    CHK_STR(lp_text_at(&g.r, TERM_LP_PART_SYS, 1), "empty name");
    lp_free(&g);
}

static void case_long_names_never_collide(void)
{
    lpreg_t g;
    /* 40 characters each, sharing the first 33 — a truncating table would
     * hand both the same identity, which the header forbids outright. */
    static const char *a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaAAAAAAA";
    static const char *b = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaBBBBBBB";

    /*
     * The header is absolute about this: "Names are stored whole, never
     * truncated: two distinct apps must not be able to present the same
     * writer_id", and the table-full rule is "a record gets
     * TERM_LP_WRITER_UNKNOWN rather than somebody else's identity". So a name
     * that does not fit whole has exactly one legal outcome: "?".
     *
     * Not reachable from an app today (TERM_LP_WRITER_MAX == MQJS_APP_NAME_MAX
     * bounds every real writer at 31 bytes), which is why this is a
     * defence-in-depth check rather than a live hole — but the header states
     * the property without that qualifier.
     */
    t_case("a name that cannot be stored whole never becomes another identity");
    REQUIRE(lp_fresh(&g, 1));
    CHK_INT(strlen(a), 40);
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, a, "from a", 6, 1));
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, b, "from b", 6, 2));
    lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_INT(bagA.n, 2);
    t_checks++;
    if (strcmp(bagA.writer[0], "?") != 0 || strcmp(bagA.writer[1], "?") != 0) {
        t_head(__FILE__, __LINE__);
        printf("     two 40-byte names, distinct after byte 33, resolved to:\n"
               "       \"%s\"\n       \"%s\"\n"
               "     the header says a name is stored whole or the record gets\n"
               "     WRITER_UNKNOWN (\"?\"), never a truncation and never\n"
               "     somebody else's identity. writers=%u unnamed=%u\n",
               bagA.writer[0], bagA.writer[1],
               g.r.hdr.writers, g.r.hdr.unnamed);
    }
    /* Both resolved to the same sentinel, not to two truncated identities.
     * (An earlier draft asserted `!= 0` — "must not collide" — which the
     * stronger both-are-"?" rule above supersedes and contradicts.) */
    CHK_TRUE(strcmp(bagA.writer[0], bagA.writer[1]) == 0);
    CHK_TRUE(strlen(bagA.writer[0]) < TERM_LP_WRITER_MAX);
    CHK_TRUE(strlen(bagA.writer[1]) < TERM_LP_WRITER_MAX);

    t_case("a 31-byte name (the longest that fits) is stored whole");
    {
        char just_fits[TERM_LP_WRITER_MAX];
        memset(just_fits, 'z', sizeof just_fits);
        just_fits[TERM_LP_WRITER_MAX - 1u] = 0;
        CHK_INT(strlen(just_fits), TERM_LP_WRITER_MAX - 1u);
        CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, just_fits, "z", 1, 3));
        lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
        CHK_INT(bagA.n, 3);
        CHK_STR(bagA.writer[2], just_fits);
        CHK_TRUE(wt_find(&g, just_fits) >= 0);
    }
    lp_free(&g);
}

static void case_names_are_sanitised(void)
{
    lpreg_t g;

    t_case("TERM_OWNER_SYSTEM's control byte becomes '_' (manifest §2 d12)");
    REQUIRE(lp_fresh(&g, 1));
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "\1system", "x", 1, 1));
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 1);
    CHK_STR(bagA.writer[0], "_system");

    t_case("quote and backslash are sanitised too, so a JSON dump stays sane");
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "a\"b\\c\td", "y", 1, 2));
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 2);
    CHK_STR(bagA.writer[1], "a_b_c_d");

    t_case("...and the sanitised form is what the region holds");
    CHK_TRUE(wt_find(&g, "_system") >= 0);
    CHK_TRUE(wt_find(&g, "a_b_c_d") >= 0);
    CHK_INT(g.r.hdr.writers, 2);
    CHK_INT(g.r.hdr.unnamed, 0);
    lp_free(&g);
}

static void case_name_outlives_its_first_record(void)
{
    lpreg_t g;
    unsigned i;

    t_case("the interned name survives eviction of the record that made it");
    REQUIRE(lp_fresh(&g, 1));
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, "early",
                                 "the first line", 14, 1));
    for (i = 0; i < 600; i++)
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_APP, "spammer", i, 200, 10u + i));
    CHK_TRUE(g.r.hdr.part[TERM_LP_PART_APP].evicted > 0);
    CHK_INT(g.r.hdr.writers, 2);

    /* "early"'s only record is long gone... */
    lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    for (i = 0; i < bagA.n; i++) CHK_STR(bagA.writer[i], "spammer");

    /* ...but its identity is still in the table, so the name costs nothing
     * the second time and still resolves. */
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, "early",
                                 "and now the last", 16, 999));
    CHK_INT(g.r.hdr.writers, 2);
    CHK_INT(g.r.hdr.unnamed, 0);
    lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
    CHK_TRUE(bagA.n > 0);
    CHK_STR(bagA.writer[bagA.n - 1u], "early");
    CHK_STR(lp_text_at(&g.r, TERM_LP_PART_APP, bagA.n - 1u), "and now the last");
    CHK_TRUE(wt_find(&g, "early") >= 0);
    lp_check_layout(g.base, &g.r.hdr);
    lp_free(&g);
}

/* ===================================================================== */
/* wt_crc: losing the names must not lose the records                    */
/* ===================================================================== */

static void case_wt_crc_failure_loses_names_only(void)
{
    lpreg_t g;
    term_lp_ring_t r2;
    term_lp_check_t chk;
    unsigned i;
    int slot;

    t_case("corrupting an interned name: ok stays true, wt_crc_ok goes false");
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 3; i++) {
        char name[16];
        sprintf(name, "w%u", i);
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, name, i, 48, 10u + i));
    }
    CHK_INT(g.r.hdr.writers, 3);
    slot = wt_find(&g, "w1");
    REQUIRE(slot >= 0);
    g.base[TERM_LP_WT_OFF + (size_t)slot * TERM_LP_WRITER_MAX] = 'X';

    memset(&r2, 0, sizeof r2);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_TRUE(chk.crc_ok);
    CHK_TRUE(chk.chain_ok);
    CHK_TRUE(!chk.wt_crc_ok);
    CHK_TRUE(!r2.wt_ok);
    CHK_INT(chk.records[TERM_LP_PART_SYS], 3);

    t_case("...and every record still reads, with '?' where the name was");
    lp_collect(&bagA, &r2, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 3);
    for (i = 0; i < bagA.n; i++) {
        CHK_STR(bagA.writer[i], "?");
        CHK_INT(bagA.len[i], 48);
        CHK_INT(bagA.seq[i], (long)i);
        CHK_INT(bagA.t_ms[i], 10u + i);
    }

    t_case("an unused table entry is outside the CRC (it covers `writers` names)");
    lp_free(&g);
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 3; i++) {
        char name[16];
        sprintf(name, "w%u", i);
        CHK_TRUE(lp_put(&g.r, TERM_LP_CLASS_SYS, name, i, 48, 10u + i));
    }
    g.base[TERM_LP_WT_OFF + (size_t)(TERM_LP_WRITERS - 1u) * TERM_LP_WRITER_MAX] = 'X';
    memset(&r2, 0, sizeof r2);
    memset(&chk, 0, sizeof chk);
    CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
    CHK_TRUE(chk.ok);
    CHK_TRUE(chk.wt_crc_ok);
    CHK_TRUE(r2.wt_ok);
    lp_collect(&bagA, &r2, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 3);
    CHK_STR(bagA.writer[0], "w0");
    CHK_STR(bagA.writer[2], "w2");
    lp_free(&g);
}

int main(void)
{
    t_suite("lp_writers");
    case_writer_and_class_come_back_as_written();
    case_table_exhaustion_yields_unknown();
    case_missing_writer_is_unknown();
    case_long_names_never_collide();
    case_names_are_sanitised();
    case_name_outlives_its_first_record();
    case_wt_crc_failure_loses_names_only();
    return t_summary();
}
