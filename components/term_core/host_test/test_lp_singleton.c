/*
 * test_lp_singleton.c — the device singleton and the two JSON shapes.
 *
 * Everything the MQTT responder of §7.2/§3 will consume lives here, so the
 * shapes are contract:
 *
 *   term_lp_ring_boot: "IDEMPOTENT AND SELF-STARTING: a second call does
 *     nothing, and every other entry point in this section calls it first."
 *     "Off the device the region is ordinary .bss: everything works, nothing
 *     is retained, and term_lp_stats_t::retained is false."
 *   term_lp_log: "Returns false when there is no ring or the text was empty
 *     after stripping."
 *   term_lp_source: "The two readable images. LASTBOOT is NULL when this boot
 *     captured none."
 *   term_lp_report: "Returns bytes written excluding the NUL, or 0 when `out`
 *     is too small for even the shortest report (never a half-written
 *     document)."
 *   term_lp_dump_json: "`from` skips that many records of the dump (0 for the
 *     whole thing) and `next` is the value to pass for the following chunk;
 *     `more` says whether the buffer filled before the records ran out."
 *     "`ok` is 0 with an empty `lines` when the source does not exist."
 *     "A buffer too small for even one rendered line answers `records: 0,
 *     more: 1`, so a chunking loop must advance on `records > 0` rather than on
 *     `more` alone."
 *
 * The singleton's region is process-wide .bss, so this suite is written to run
 * exactly once, in order, and never re-formats it behind the singleton's back.
 */
#include "lp_util.h"
#include "fake_port.h"

#define JBUF 8192u

static char jbuf[JBUF + 64u];

static int has(const char *s, const char *needle)
{
    return s && needle && strstr(s, needle) != NULL;
}

/* Report/dump into jbuf with a canary past `cap`, checking the three things
 * every builder here promises: it stops inside the buffer, NUL-terminates, and
 * never leaves half a document. */
static size_t build_report(size_t cap)
{
    size_t n;
    memset(jbuf, 0x7e, sizeof jbuf);
    n = term_lp_report(jbuf, cap);
    t_checks++;
    if (n >= cap && !(n == 0 && cap == 0)) {
        t_head(__FILE__, __LINE__);
        printf("     report wrote %lu into a %lu-byte buffer\n",
               (unsigned long)n, (unsigned long)cap);
        return 0;
    }
    if (cap && jbuf[cap] != 0x7e) {
        t_head(__FILE__, __LINE__);
        printf("     report overran its buffer (cap %lu)\n", (unsigned long)cap);
    }
    if (n) {
        CHK_INT(t_strnlen(jbuf, cap), n);
        CHK_INT(jbuf[0], '{');
        CHK_INT(jbuf[n - 1u], '}');
    }
    return n;
}

static size_t build_dump(term_lp_src_t src, uint32_t from, size_t cap)
{
    size_t n;
    memset(jbuf, 0x7e, sizeof jbuf);
    n = term_lp_dump_json(src, from, jbuf, cap);
    t_checks++;
    if (n >= cap && !(n == 0 && cap == 0)) {
        t_head(__FILE__, __LINE__);
        printf("     dump wrote %lu into a %lu-byte buffer\n",
               (unsigned long)n, (unsigned long)cap);
        return 0;
    }
    if (cap && jbuf[cap] != 0x7e) {
        t_head(__FILE__, __LINE__);
        printf("     dump overran its buffer (cap %lu)\n", (unsigned long)cap);
    }
    if (n) {
        CHK_INT(t_strnlen(jbuf, cap), n);
        CHK_INT(jbuf[0], '{');
        CHK_INT(jbuf[n - 1u], '}');
    }
    return n;
}

/* The integer after "<key>":  — -1 when the key is absent. */
static long jint(const char *key)
{
    char pat[64];
    const char *p;
    sprintf(pat, "\"%s\":", key);
    p = strstr(jbuf, pat);
    if (!p) return -1;
    p += strlen(pat);
    if (*p == '"') return -2;
    return strtol(p, NULL, 10);
}

/* ===================================================================== */
/* Boot                                                                  */
/* ===================================================================== */

static void case_boot_is_idempotent_and_self_starting(void)
{
    term_lp_stats_t s, s2;

    t_case("boot brings a live ring up off-device, retaining nothing");
    fp_reset();
    REQUIRE(term_port_install(fp_port()));
    term_lp_ring_boot();
    CHK_TRUE(term_lp_ring_ready());
    memset(&s, 0, sizeof s);
    term_lp_stats(&s);
    CHK_TRUE(s.ready);
    CHK_TRUE(!s.retained);
    CHK_INT(s.region_bytes, TERM_LP_REGION_BYTES);
    CHK_INT(s.cap[TERM_LP_PART_SYS], TERM_LP_SYS_BYTES);
    CHK_INT(s.cap[TERM_LP_PART_APP], TERM_LP_APP_BYTES);
    CHK_TRUE(s.boot_seq >= 1u);

    t_case("a virgin .bss region reads as COLD with no previous session");
    CHK_INT(s.prev, TERM_LP_PREV_NONE);
    CHK_INT(s.retention, TERM_LP_RET_COLD);
    CHK_TRUE(!s.lastboot);
    CHK_INT(s.prev_boot_seq, 0);
    CHK_INT(s.last_records[TERM_LP_PART_SYS], 0);
    CHK_INT(s.last_records[TERM_LP_PART_APP], 0);
    CHK_INT(s.last_used[TERM_LP_PART_SYS], 0);
    CHK_INT(s.last_used[TERM_LP_PART_APP], 0);
    CHK_TRUE(term_lp_source(TERM_LP_SRC_LIVE) != NULL);
    CHK_TRUE(term_lp_source(TERM_LP_SRC_LASTBOOT) == NULL);

    t_case("the boot marker is already in the SYS partition (§4.4)");
    CHK_INT(s.records[TERM_LP_PART_SYS], 1);
    CHK_INT(s.appended[TERM_LP_PART_SYS], 1);
    CHK_INT(s.records[TERM_LP_PART_APP], 0);
    {
        const term_lp_ring_t *live = term_lp_source(TERM_LP_SRC_LIVE);
        term_lp_iter_t it;
        term_lp_record_t rec;
        REQUIRE(live != NULL);
        term_lp_iter_begin(&it, live, TERM_LP_PART_SYS);
        REQUIRE(term_lp_iter_next(&it, &rec));
        CHK_STR(rec.writer, TERM_LP_WRITER_SYSTEM);
        CHK_INT(rec.cls, TERM_LP_CLASS_SYS);
        CHK_TRUE(rec.len > 4u);
        CHK_INT(memcmp(rec.text, "boot ", 5), 0);
        CHK_TRUE(!term_lp_iter_next(&it, &rec));
    }

    t_case("a second boot() does nothing at all");
    term_lp_ring_boot();
    term_lp_ring_boot();
    memset(&s2, 0, sizeof s2);
    term_lp_stats(&s2);
    CHK_INT(s2.boot_seq, s.boot_seq);
    CHK_INT(s2.records[TERM_LP_PART_SYS], 1);
    CHK_INT(s2.appended[TERM_LP_PART_SYS], 1);
    CHK_INT(s2.hseq, s.hseq);
    CHK_TRUE(term_lp_source(TERM_LP_SRC_LIVE) == term_lp_source(TERM_LP_SRC_LIVE));
}

static void case_the_tee_routes_by_class(void)
{
    term_lp_stats_t s;

    t_case("term_lp_log routes by class and strips on the way in");
    CHK_TRUE(term_lp_log(TERM_LP_CLASS_APP, "reading", "page 42", 7));
    CHK_TRUE(term_lp_log(TERM_LP_CLASS_SYS, "system", "\x1b[32mwifi: up\x1b[0m", 17));
    CHK_TRUE(term_lp_log(TERM_LP_CLASS_APP, "reading", "quote \" and \\ and \n", 19));
    memset(&s, 0, sizeof s);
    term_lp_stats(&s);
    CHK_INT(s.records[TERM_LP_PART_SYS], 2);
    CHK_INT(s.records[TERM_LP_PART_APP], 2);
    CHK_INT(s.writers, 2);

    t_case("an empty or all-escape line is refused and counted");
    CHK_TRUE(!term_lp_log(TERM_LP_CLASS_APP, "reading", "", 0));
    CHK_TRUE(!term_lp_log(TERM_LP_CLASS_APP, "reading", "\x1b[0m", 4));
    term_lp_stats(&s);
    CHK_INT(s.refused, 2);
    CHK_INT(s.records[TERM_LP_PART_APP], 2);

    t_case("the live source shows exactly what was logged");
    {
        const term_lp_ring_t *live = term_lp_source(TERM_LP_SRC_LIVE);
        REQUIRE(live != NULL);
        CHK_STR(lp_text_at(live, TERM_LP_PART_SYS, 1), "wifi: up");
        CHK_STR(lp_text_at(live, TERM_LP_PART_APP, 0), "page 42");
        CHK_INT(lp_count(live, -1), 4);
    }
}

/* ===================================================================== */
/* The stats report                                                      */
/* ===================================================================== */

static void case_report_shape(void)
{
    term_lp_stats_t s;
    size_t n, cap;

    memset(&s, 0, sizeof s);
    term_lp_stats(&s);

    t_case("the report carries every documented key, and no content");
    n = build_report(TERM_LP_REPORT_MAX);
    CHK_TRUE(n > 0);
    CHK_TRUE(has(jbuf, "\"ready\":1"));
    CHK_TRUE(has(jbuf, "\"retained\":0"));
    CHK_TRUE(has(jbuf, "\"region\":32256"));
    CHK_TRUE(has(jbuf, "\"rec_max\":512"));
    CHK_TRUE(has(jbuf, "\"retention\":\"cold\""));
    CHK_TRUE(has(jbuf, "\"prev\":\"none\""));
    CHK_TRUE(has(jbuf, "\"live\":{"));
    CHK_TRUE(has(jbuf, "\"sys\":{"));
    CHK_TRUE(has(jbuf, "\"app\":{"));
    CHK_TRUE(has(jbuf, "\"last\":{"));
    CHK_INT(jint("boot_seq"), (long)s.boot_seq);
    CHK_INT(jint("writers"), (long)s.writers);
    CHK_INT(jint("refused"), (long)s.refused);
    CHK_INT(jint("trunc"), (long)s.truncated);
    CHK_INT(jint("unnamed"), (long)s.unnamed);
    CHK_INT(jint("dropped"), (long)s.dropped_bytes);
    CHK_INT(jint("hseq"), (long)s.hseq);
    CHK_INT(jint("lastboot"), 0);
    CHK_INT(jint("prev_boot_seq"), (long)s.prev_boot_seq);

    t_case("...and no writer NAME and no log text leak into it (§7.2)");
    CHK_TRUE(!has(jbuf, "page 42"));
    CHK_TRUE(!has(jbuf, "wifi: up"));
    CHK_TRUE(!has(jbuf, "reading"));
    CHK_TRUE(!has(jbuf, "boot "));

    t_case("every buffer size answers with a whole document or with nothing");
    for (cap = 0; cap <= 200u; cap++) (void)build_report(cap);
    n = build_report(TERM_LP_REPORT_MAX);
    CHK_TRUE(n > 0);
    CHK_INT(term_lp_report(NULL, TERM_LP_REPORT_MAX), 0);
    CHK_INT(term_lp_report(jbuf, 0), 0);
}

/* ===================================================================== */
/* The content dump                                                      */
/* ===================================================================== */

static void case_dump_shape_and_cursor(void)
{
    size_t n;
    long total;
    unsigned i;

    t_case("the live dump renders oldest first, SYS before APP");
    n = build_dump(TERM_LP_SRC_LIVE, 0, TERM_LP_DUMP_MAX);
    CHK_TRUE(n > 0);
    CHK_TRUE(has(jbuf, "\"src\":\"live\""));
    CHK_TRUE(has(jbuf, "\"ok\":1"));
    CHK_TRUE(has(jbuf, "\"from\":0"));
    CHK_TRUE(has(jbuf, "\"lines\":["));
    CHK_TRUE(has(jbuf, "sys/system: boot "));
    CHK_TRUE(has(jbuf, "sys/system: wifi: up"));
    CHK_TRUE(has(jbuf, "app/reading: page 42"));
    CHK_INT(jint("records"), 4);
    CHK_INT(jint("next"), 4);
    CHK_INT(jint("more"), 0);
    {   /* SYS lines before APP lines */
        const char *sys = strstr(jbuf, "app/reading: page 42");
        const char *app = strstr(jbuf, "sys/system: wifi: up");
        CHK_TRUE(sys && app && app < sys);
    }

    t_case("quote, backslash and newline are escaped, so the document is JSON");
    CHK_TRUE(has(jbuf, "quote \\\" and \\\\ and \\n"));
    CHK_TRUE(!has(jbuf, "and \n"));

    t_case("`from` skips records and `next` chases it");
    n = build_dump(TERM_LP_SRC_LIVE, 2, TERM_LP_DUMP_MAX);
    CHK_TRUE(n > 0);
    CHK_INT(jint("from"), 2);
    CHK_INT(jint("records"), 2);
    CHK_INT(jint("next"), 4);
    CHK_INT(jint("more"), 0);
    CHK_TRUE(!has(jbuf, "boot "));
    CHK_TRUE(has(jbuf, "app/reading: page 42"));

    n = build_dump(TERM_LP_SRC_LIVE, 4, TERM_LP_DUMP_MAX);
    CHK_TRUE(n > 0);
    CHK_INT(jint("records"), 0);
    CHK_INT(jint("next"), 4);
    CHK_INT(jint("more"), 0);
    n = build_dump(TERM_LP_SRC_LIVE, 9999, TERM_LP_DUMP_MAX);
    CHK_TRUE(n > 0);
    CHK_INT(jint("records"), 0);

    t_case("an absent source answers ok:0 with an empty array");
    n = build_dump(TERM_LP_SRC_LASTBOOT, 0, TERM_LP_DUMP_MAX);
    CHK_TRUE(n > 0);
    CHK_TRUE(has(jbuf, "\"src\":\"lastboot\""));
    CHK_TRUE(has(jbuf, "\"ok\":0"));
    CHK_TRUE(has(jbuf, "\"lines\":[]"));
    CHK_INT(jint("records"), 0);

    t_case("a chunking loop that advances on `records > 0` terminates");
    for (i = 0; i < 40u; i++) {
        char line[64];
        int k = sprintf(line, "chunk line %u", i);
        CHK_TRUE(term_lp_log(TERM_LP_CLASS_APP, "chunker", line, (size_t)k));
    }
    {
        uint32_t from = 0;
        unsigned rounds = 0, seen = 0;
        long more;
        do {
            long recs;
            n = build_dump(TERM_LP_SRC_LIVE, from, 256u);
            if (!n) break;
            recs = jint("records");
            more = jint("more");
            CHK_INT(jint("from"), (long)from);
            if (recs <= 0) {
                /* "A buffer too small for even one rendered line answers
                 * records: 0, more: 1" — the loop must not spin on it. */
                CHK_INT(more, 1);
                break;
            }
            seen += (unsigned)recs;
            CHK_INT(jint("next"), (long)from + recs);
            from = (uint32_t)jint("next");
        } while (more == 1 && ++rounds < 200u);
        CHK_TRUE(rounds < 200u);
        n = build_dump(TERM_LP_SRC_LIVE, 0, TERM_LP_DUMP_MAX);
        total = jint("records");
        CHK_TRUE(total >= 40);
        CHK_INT(seen, (unsigned)total);
    }

    t_case("a hopeless buffer is refused outright, not half filled");
    for (n = 0; n <= 120u; n++) (void)build_dump(TERM_LP_SRC_LIVE, 0, n);
    CHK_INT(term_lp_dump_json(TERM_LP_SRC_LIVE, 0, NULL, TERM_LP_DUMP_MAX), 0);
    CHK_INT(term_lp_dump_json(TERM_LP_SRC_LIVE, 0, jbuf, 0), 0);
}

/* ===================================================================== */
/* The enum name tables                                                  */
/* ===================================================================== */

static void case_enum_strings(void)
{
    unsigned i;

    t_case("prev/retention/reason all answer with a non-empty name");
    CHK_STR(term_lp_prev_str(TERM_LP_PREV_NONE), "none");
    CHK_STR(term_lp_prev_str(TERM_LP_PREV_CAPTURED), "captured");
    CHK_STR(term_lp_retention_str(TERM_LP_RET_COLD), "cold");
    CHK_STR(term_lp_retention_str(TERM_LP_RET_OK), "ok");
    CHK_STR(term_lp_retention_str(TERM_LP_RET_LOST), "lost");
    CHK_STR(term_lp_retention_str(TERM_LP_RET_CORRUPT), "corrupt");
    for (i = 0; i <= (unsigned)TERM_LP_PREV_GARBAGE + 2u; i++) {
        const char *s = term_lp_prev_str((term_lp_prev_t)i);
        CHK_TRUE(s != NULL && s[0] != 0);
    }
    for (i = 0; i <= (unsigned)TERM_LP_RET_UNKNOWN + 2u; i++) {
        const char *s = term_lp_retention_str((term_lp_retention_t)i);
        CHK_TRUE(s != NULL && s[0] != 0);
    }
    for (i = 0; i < 24u; i++) {
        const char *s = term_lp_reason_str(i);
        CHK_TRUE(s != NULL && s[0] != 0);
    }
    CHK_TRUE(term_lp_reason_str(255) != NULL);
    CHK_STR(term_lp_reason_str(4), "panic");
}

int main(void)
{
    t_suite("lp_singleton");
    case_boot_is_idempotent_and_self_starting();
    case_the_tee_routes_by_class();
    case_report_shape();
    case_dump_shape_and_cursor();
    case_enum_strings();
    fp_reclaim_all();
    return t_summary();
}
