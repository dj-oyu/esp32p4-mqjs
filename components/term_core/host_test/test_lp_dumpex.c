/*
 * test_lp_dumpex.c — term_lp_dump_json_ex, the one header change §3 needed.
 *
 * term_lp_ring.h states exactly what it is for and what it promises:
 *
 *   "`records`, `next` and `more` are inside the JSON document, so a C caller
 *    that chunks a 31 KB black box over MQTT could only learn whether to ask
 *    again by re-parsing its own output."
 *   "`info` may be NULL, in which case this IS term_lp_dump_json (which is now a
 *    one-line wrapper: the rendering has one implementation, not two). On a 0
 *    return — `out` too small for even the framing — `*info` is zeroed, so a
 *    caller that only reads `info->more` still terminates."
 *   term_lp_dump_info_t: "ok (mirrors "ok" in the JSON)", "records: content
 *    records rendered into THIS chunk", "next: pass as `from` for the next
 *    chunk (from+records)", "more: the buffer filled before the records ran
 *    out".
 *
 * So there are three things to hold together and this suite compares all three
 * against each other at every buffer size: the C struct, the JSON body it
 * returned, and the iterator's own count of the records. Two of them agreeing
 * would not be enough — the point of the side door is that a C chunker can
 * trust it WITHOUT reading the JSON, so the struct is checked against the
 * document it accompanies AND against ground truth.
 */
#include "lp_util.h"
#include "fake_port.h"

#define DBUF 65536u   /* room for a whole dense ring in one document */

static char jbuf[DBUF + 64u];
static char kbuf[DBUF + 64u];

static long jint(const char *s, const char *key)
{
    char pat[64];
    const char *p;
    if (!s) return -1;
    sprintf(pat, "\"%s\":", key);
    p = strstr(s, pat);
    if (!p) return -1;
    p += strlen(pat);
    if (*p == '"' || *p == '{' || *p == '[') return -2;
    return strtol(p, NULL, 10);
}

static unsigned count_of(const char *hay, const char *needle)
{
    unsigned n = 0;
    size_t k = strlen(needle);
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { n++; p += k; }
    return n;
}

/*
 * One dump_json_ex call, with the struct held against the document and against
 * the wrapper. Returns the byte count.
 */
static size_t dumpx(term_lp_src_t src, uint32_t from, size_t cap,
                    term_lp_dump_info_t *info)
{
    size_t n, m;
    term_lp_dump_info_t got;

    memset(&got, 0xCD, sizeof got);
    memset(jbuf, 0x7e, sizeof jbuf);
    n = term_lp_dump_json_ex(src, from, jbuf, cap, &got);

    /* Inside the buffer, whole document or nothing. */
    CHK_TRUE(n < cap || (n == 0u && cap == 0u));
    if (cap && cap < sizeof jbuf) CHK_INT(jbuf[cap], 0x7e);
    if (n) {
        CHK_INT(t_strnlen(jbuf, cap), n);
        CHK_INT(jbuf[0], '{');
        CHK_INT(jbuf[n - 1u], '}');
    }

    if (n == 0u) {
        /* "On a 0 return ... *info is zeroed, so a caller that only reads
         * info->more still terminates." */
        t_checks++;
        if (got.ok || got.records || got.next || got.more) {
            t_head(__FILE__, __LINE__);
            printf("     a 0 return left info = {ok %d, records %lu, next %lu, "
                   "more %d}\n", (int)got.ok, (unsigned long)got.records,
                   (unsigned long)got.next, (int)got.more);
        }
    } else {
        /* The struct mirrors the document. */
        CHK_INT(got.ok, jint(jbuf, "ok") == 1);
        CHK_INT((long)got.records, jint(jbuf, "records"));
        CHK_INT((long)got.next, jint(jbuf, "next"));
        CHK_INT(got.more, jint(jbuf, "more") == 1);
        CHK_INT(jint(jbuf, "from"), (long)from);
        /* "next: pass as `from` for the next chunk (from+records)" */
        CHK_INT(got.next, from + got.records);
        /* One rendered line per counted record. */
        CHK_INT(count_of(jbuf, "] sys/") + count_of(jbuf, "] app/"),
                got.records);
    }

    /* The wrapper is the same call: same bytes, same length. */
    memset(kbuf, 0x7e, sizeof kbuf);
    m = term_lp_dump_json(src, from, kbuf, cap);
    CHK_INT(m, n);
    CHK_INT(memcmp(jbuf, kbuf, cap < sizeof jbuf ? cap : sizeof jbuf), 0);

    /* ... and so is dump_json_ex with a NULL info. */
    memset(kbuf, 0x7e, sizeof kbuf);
    m = term_lp_dump_json_ex(src, from, kbuf, cap, NULL);
    CHK_INT(m, n);
    CHK_INT(memcmp(jbuf, kbuf, cap < sizeof jbuf ? cap : sizeof jbuf), 0);

    if (info) *info = got;
    return n;
}

/* ===================================================================== */
/* Ground truth                                                          */
/* ===================================================================== */

static unsigned live_records(void)
{
    const term_lp_ring_t *live = term_lp_source(TERM_LP_SRC_LIVE);
    return live ? lp_count(live, -1) : 0u;
}

static void case_a_whole_dump(void)
{
    term_lp_dump_info_t info;
    unsigned total;

    t_case("a buffer that fits everything reports the iterator's own count");
    total = live_records();
    CHK_TRUE(total >= 1u);
    CHK_TRUE(dumpx(TERM_LP_SRC_LIVE, 0, DBUF, &info) > 0);
    CHK_TRUE(info.ok);
    CHK_INT(info.records, total);
    CHK_INT(info.next, total);
    CHK_TRUE(!info.more);

    t_case("...and with content in it too");
    {
        unsigned i;
        for (i = 0; i < 24u; i++) {
            char line[64];
            int k = sprintf(line, "dumpex line %u", i);
            CHK_TRUE(term_lp_log(TERM_LP_CLASS_APP, "dumpex", line, (size_t)k));
        }
        total = live_records();
        CHK_TRUE(dumpx(TERM_LP_SRC_LIVE, 0, DBUF, &info) > 0);
        CHK_INT(info.records, total);
        CHK_INT(info.next, total);
        CHK_TRUE(!info.more);
        CHK_INT(count_of(jbuf, "app/dumpex: dumpex line "), 24u);
    }

    t_case("a cursor at, and past, the end is not an error");
    CHK_TRUE(dumpx(TERM_LP_SRC_LIVE, total, DBUF, &info) > 0);
    CHK_TRUE(info.ok);
    CHK_INT(info.records, 0);
    CHK_INT(info.next, total);
    CHK_TRUE(!info.more);
    CHK_TRUE(dumpx(TERM_LP_SRC_LIVE, 0xFFFFFFFFu, DBUF, &info) > 0);
    CHK_INT(info.records, 0);
    CHK_TRUE(!info.more);

    t_case("an absent source mirrors ok:0 into the struct");
    CHK_TRUE(term_lp_source(TERM_LP_SRC_LASTBOOT) == NULL);
    CHK_TRUE(dumpx(TERM_LP_SRC_LASTBOOT, 0, DBUF, &info) > 0);
    CHK_TRUE(!info.ok);
    CHK_INT(info.records, 0);
    CHK_TRUE(!info.more);
    CHK_TRUE(strstr(jbuf, "\"lines\":[]") != NULL);

    t_case("reading does not mutate (term_lp_ring.h: read calls do not mutate)");
    {
        term_lp_stats_t a, b;
        memset(&a, 0, sizeof a);
        term_lp_stats(&a);
        (void)dumpx(TERM_LP_SRC_LIVE, 0, DBUF, &info);
        (void)dumpx(TERM_LP_SRC_LIVE, 3, 256u, &info);
        memset(&b, 0, sizeof b);
        term_lp_stats(&b);
        CHK_INT(memcmp(&a, &b, sizeof a), 0);
        CHK_INT(live_records(), total);
    }
}

/* ===================================================================== */
/* Every buffer size                                                     */
/* ===================================================================== */

static void case_every_buffer_size(void)
{
    term_lp_dump_info_t info;
    size_t cap;
    unsigned zero_returns = 0, stalls = 0;

    t_case("every buffer size agrees with itself, at cap and one past it");
    for (cap = 0; cap <= 400u; cap++) {
        size_t n = dumpx(TERM_LP_SRC_LIVE, 0, cap, &info);
        if (!n) { zero_returns++; continue; }
        if (info.records == 0u) {
            /* "A buffer too small for even one rendered line answers
             * records: 0, more: 1, so a chunking loop must advance on
             * records > 0 rather than on more alone." */
            CHK_TRUE(info.more);
            stalls++;
        }
    }
    CHK_TRUE(zero_returns > 0u);   /* the 96-byte floor really is a floor */
    CHK_TRUE(stalls > 0u);         /* and the stall shape really happens */

    t_case("a hopeless buffer is refused outright and zeroes the struct");
    {
        memset(&info, 0xCD, sizeof info);
        CHK_INT(term_lp_dump_json_ex(TERM_LP_SRC_LIVE, 0, jbuf, 0, &info), 0);
        CHK_TRUE(!info.ok && !info.more);
        CHK_INT(info.records, 0);
        CHK_INT(info.next, 0);
        memset(&info, 0xCD, sizeof info);
        CHK_INT(term_lp_dump_json_ex(TERM_LP_SRC_LIVE, 0, NULL, DBUF, &info), 0);
        CHK_TRUE(!info.ok && !info.more);
        CHK_INT(info.records, 0);
        memset(&info, 0xCD, sizeof info);
        CHK_INT(term_lp_dump_json_ex(TERM_LP_SRC_LIVE, 0, jbuf, 95u, &info), 0);
        CHK_TRUE(!info.ok && !info.more);
        /* And the documented floor works. */
        CHK_TRUE(term_lp_dump_json_ex(TERM_LP_SRC_LIVE, 0, jbuf, 96u, &info) > 0);
    }
}

/* ===================================================================== */
/* The reason it exists: a C chunker that never reads the JSON             */
/* ===================================================================== */

static void case_a_c_chunker_terminates(void)
{
    term_lp_dump_info_t info;
    unsigned total, i;

    t_case("a chunk loop driven ONLY by the struct reassembles the whole dump");
    for (i = 0; i < 300u; i++) {
        char line[80];
        int k = sprintf(line, "chunk %u ------------------------------", i);
        (void)term_lp_log(TERM_LP_CLASS_APP, "chunker", line, (size_t)k);
    }
    total = live_records();
    CHK_TRUE(total > 100u);

    {
        size_t caps[] = { 96u, 128u, 200u, 512u, 1024u, TERM_LP_DUMP_MAX };
        unsigned c;
        for (c = 0; c < sizeof caps / sizeof caps[0]; c++) {
            uint32_t from = 0;
            unsigned seen = 0, rounds = 0;
            for (;;) {
                size_t n = term_lp_dump_json_ex(TERM_LP_SRC_LIVE, from, jbuf,
                                                caps[c], &info);
                if (!n) break;                     /* zeroed info -> stop */
                if (info.records == 0u) break;     /* advance on records > 0 */
                seen += info.records;
                CHK_INT(info.next, from + info.records);
                from = info.next;
                if (!info.more) break;
                if (++rounds > 5000u) break;
            }
            t_checks++;
            if (rounds > 5000u) {
                t_head(__FILE__, __LINE__);
                printf("     the loop did not terminate at cap %lu\n",
                       (unsigned long)caps[c]);
            }
            t_checks++;
            if (caps[c] >= 200u && seen != total) {
                t_head(__FILE__, __LINE__);
                printf("     cap %lu reassembled %u of %u records\n",
                       (unsigned long)caps[c], seen, total);
            }
        }
    }

    t_case("...and the cursor is exact against the iterator at every position");
    /* `from` skips exactly that many records of the dump, so a whole-buffer
     * dump from k must carry total-k of them, for every k. Against LASTBOOT the
     * header calls this exact and against LIVE best-effort; nothing is
     * appending here, so LIVE is exact too and a drift of one would show. */
    for (i = 0; i <= total + 2u; i++) {
        size_t n = term_lp_dump_json_ex(TERM_LP_SRC_LIVE, i, jbuf, DBUF, &info);
        unsigned want = i < total ? total - i : 0u;
        t_checks++;
        if (!n || info.records != want || info.more ||
            info.next != (i < total ? total : i)) {
            t_head(__FILE__, __LINE__);
            printf("     from=%u: records %lu (want %u), next %lu, more %d\n", i,
                   (unsigned long)info.records, want,
                   (unsigned long)info.next, (int)info.more);
        }
    }
}

int main(void)
{
    t_suite("lp_dumpex");
    t_case("the port installs and the ring boots");
    fp_reset();
    CHK_TRUE(term_port_install(fp_port()));
    term_lp_ring_boot();
    CHK_TRUE(term_lp_ring_ready());

    case_a_whole_dump();
    case_every_buffer_size();
    case_a_c_chunker_terminates();

    fp_reclaim_all();
    return t_summary();
}
