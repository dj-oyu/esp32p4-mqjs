/*
 * test_bb_serve.c — the reply: the chunk loop, the cursor, the three sources
 * and the two honesty flags.
 *
 * term_bb_pull.h's "WIRE FORMAT — REPLY" is the contract:
 *
 *   "seq   0-based chunk index. A gap means a lost QoS 0 chunk"
 *   "last  1 on the final chunk. The reader reassembles by concatenating
 *          `body.lines` in `seq` order until it sees it."
 *   "body  for what=stats, term_lp_report()'s object; otherwise
 *          term_lp_dump_json()'s object, unchanged"
 *   "Two honesty flags appear only when they apply, and both also force
 *    "last":1 because the responder stops: "stall":1 ... "cut":1 ..."
 *   Q5: "THE RESPONDER ADDS NO STATE TO THE RING AND NEVER WRITES TO IT."
 *
 * The reassembly is checked for COMPLETENESS, not plausibility: the per-chunk
 * `records` must sum to the whole, and every record the iterator can see must
 * appear exactly once in the concatenated `lines`. A chunker that silently
 * dropped a record between two chunks would pass a "seq is dense" test and
 * fail this one.
 *
 * The ring is process-global (term_lp_ring.h: "Off the device the region is
 * ordinary .bss"), so the cases run in one fixed order, from an almost-empty
 * ring to a full one. What is NOT reachable here is a populated LASTBOOT: the
 * snapshot is taken by term_lp_ring_boot() from a retained region, and a host
 * process starts with a zeroed .bss one. So `lastboot` is covered in its
 * absent form, and its populated form stays a device check (PHASE3_MANIFEST
 * §3's device plan, step 4).
 */
#include "bb_util.h"
#include "fake_port.h"

#define BIG_PAYLOAD  (TERM_LP_REC_MAX)
#define BIG_WRITER   "abcdefghijklmnopqrstuvwxyz01234"   /* 31 bytes */

static char cat[512u * 1024u];   /* every chunk, concatenated */
static char refbuf[TERM_LP_DUMP_MAX];

static const char *request(uint64_t ctr, const char *what, uint32_t from)
{
    static char buf[TERM_BB_REQ_MAX];
    sprintf(buf, "bbpull1\nctr=%llu\ntop=%s\nwhat=%s\nfrom=%lu\n",
            (unsigned long long)ctr, BB_TOPIC, what, (unsigned long)from);
    return buf;
}

/* The whole reply, chunk after chunk, so a record can be looked for once
 * across the lot. */
static void concat_chunks(void)
{
    unsigned i;
    size_t o = 0;
    cat[0] = 0;
    for (i = 0; i < bb.chunks && i < BB_MAX_CHUNKS; i++) {
        size_t n = bb.chunk_len[i];
        if (o + n + 1u >= sizeof cat) break;
        memcpy(cat + o, bb.chunk[i], n);
        o += n;
    }
    cat[o] = 0;
}

static unsigned count_of(const char *hay, const char *needle)
{
    unsigned n = 0;
    size_t k = strlen(needle);
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { n++; p += k; }
    return n;
}

/* A pull, checked end to end. Returns the record total the reply carried. */
static unsigned pull(uint64_t ctr, const char *what, uint32_t from,
                     term_bb_result_t *out)
{
    term_bb_result_t res;
    term_bb_err_t e;
    unsigned sum;

    bb.chunks = 0;
    bb.publish_calls = 0;
    bb.max_chunk = 0;
    bb.overflow = bb.bad_len = bb.null_json = 0;
    bb.mark = ctr - 1u;

    e = bb_serve(request(ctr, what, from), &res);
    t_checks++;
    if (e != TERM_BB_OK) {
        t_head(__FILE__, __LINE__);
        printf("     pull(%s, from=%lu) answered %s\n", what,
               (unsigned long)from, term_bb_err_str(e));
    }
    sum = bb_check_reply(&res);
    CHK_STR(bb_jstr(bb.chunk[0], "what"), what);
    CHK_TRUE(bb.chunks >= 1u);
    concat_chunks();
    if (out) *out = res;
    return sum;
}

/* ===================================================================== */
/* stats: one chunk, no content, and the buffer floor the header pins      */
/* ===================================================================== */

static void case_stats(void)
{
    term_bb_result_t res;
    static char before[TERM_LP_REPORT_MAX], after[TERM_LP_REPORT_MAX];

    t_case("what=stats is one chunk of term_lp_report()'s object, no content");
    bb_reset();
    CHK_TRUE(term_lp_report(before, sizeof before) > 0);
    (void)pull(100, "stats", 0, &res);
    CHK_INT(res.chunks, 1);
    CHK_INT(res.records, 0);
    CHK_INT(bb_jint(bb.chunk[0], "seq"), 0);
    CHK_INT(bb_jlast(bb.chunk[0]), 1);
    CHK_INT(bb_jint(bb.chunk[0], "from"), 0);
    CHK_TRUE(bb_has(bb.chunk[0], "\"bb\":1"));
    CHK_TRUE(bb_has(bb.chunk[0], "\"region\":32256"));
    /* "no content": the report has no `lines` array and no record text. */
    CHK_TRUE(!bb_has(bb.chunk[0], "\"lines\""));
    CHK_TRUE(!bb_has(bb.chunk[0], "app/"));

    t_case("...and the body is term_lp_report()'s bytes, not a second builder");
    CHK_TRUE(bb_has(bb.chunk[0], before));
    CHK_TRUE(term_lp_report(after, sizeof after) > 0);
    CHK_STR(after, before);            /* Q5: the pull changed nothing */

    t_case("a stats pull at exactly TERM_BB_SCRATCH_MIN succeeds and is whole");
    /* The header pins this relationship at the constant: "This floor also has
     * to leave room for a WHOLE `stats` body, because term_lp_report() degrades
     * to {"ready":1,"error":"truncated"} rather than emitting half a document
     * ... a test pins the relationship". */
    bb_reset();
    bb_scratch(TERM_BB_SCRATCH_MIN);
    (void)pull(200, "stats", 0, &res);
    CHK_INT(res.chunks, 1);
    CHK_TRUE(!bb_has(bb.chunk[0], "truncated"));
    CHK_TRUE(bb_has(bb.chunk[0], "\"region\":32256"));
    CHK_TRUE(bb_has(bb.chunk[0], "\"hseq\":"));
    CHK_TRUE(bb.max_chunk < TERM_BB_SCRATCH_MIN);
    bb_reset();

    t_case("`from` is echoed but means nothing to a stats pull");
    (void)pull(300, "stats", 9, &res);
    CHK_INT(res.chunks, 1);
    CHK_INT(res.records, 0);
    CHK_INT(bb_jint(bb.chunk[0], "from"), 9);
}

/* ===================================================================== */
/* lastboot, absent                                                      */
/* ===================================================================== */

static void case_lastboot_absent(void)
{
    term_bb_result_t res;

    t_case("an absent lastboot answers cleanly: ok:0, no lines, one chunk");
    bb_reset();
    CHK_TRUE(term_lp_source(TERM_LP_SRC_LASTBOOT) == NULL);
    (void)pull(400, "lastboot", 0, &res);
    CHK_INT(res.chunks, 1);
    CHK_INT(res.records, 0);
    CHK_TRUE(bb_has(bb.chunk[0], "\"src\":\"lastboot\""));
    CHK_TRUE(bb_has(bb.chunk[0], "\"ok\":0"));
    CHK_TRUE(bb_has(bb.chunk[0], "\"lines\":[]"));
    CHK_INT(bb_jlast(bb.chunk[0]), 1);
    CHK_INT(bb_jbody(bb.chunk[0], "more"), 0);
    CHK_STR(bb_jstr(bb.chunk[0], "what"), "lastboot");
}

/* ===================================================================== */
/* live: the chunk loop and the cursor                                    */
/* ===================================================================== */

static void case_live_one_chunk(void)
{
    term_bb_result_t res;
    unsigned total;

    t_case("a ring that fits in one chunk is one chunk, last:1, more:0");
    bb_reset();
    total = bb_live_records();
    CHK_TRUE(total >= 1u);            /* the boot marker, at least */
    (void)pull(500, "live", 0, &res);
    CHK_INT(res.chunks, 1);
    CHK_INT(res.records, total);
    CHK_TRUE(bb_has(bb.chunk[0], "\"src\":\"live\""));
    CHK_TRUE(bb_has(bb.chunk[0], "\"ok\":1"));
    CHK_INT(bb_jbody(bb.chunk[0], "more"), 0);
    CHK_INT(bb_jbody(bb.chunk[0], "next"), (long long)total);
    CHK_TRUE(bb_has(bb.chunk[0], "sys/system: boot "));

    t_case("a cursor past the end answers records:0, more:0, one chunk");
    (void)pull(600, "live", 999999u, &res);
    CHK_INT(res.chunks, 1);
    CHK_INT(res.records, 0);
    CHK_TRUE(bb_has(bb.chunk[0], "\"lines\":[]"));
    CHK_INT(bb_jbody(bb.chunk[0], "more"), 0);
    CHK_INT(bb_jlast(bb.chunk[0]), 1);
    CHK_TRUE(!bb_has(bb.chunk[0], "\"stall\":1"));
    CHK_TRUE(!bb_has(bb.chunk[0], "\"cut\":1"));
}

static void case_live_many_chunks(void)
{
    term_bb_result_t res;
    unsigned total, i, sum;

    t_case("a ring that needs many chunks: seq dense, one last, records whole");
    bb_reset();
    CHK_INT(bb_fill(172u, 100u, "chunker"), 172u);
    total = bb_live_records();
    CHK_TRUE(total >= 173u);
    sum = pull(700, "live", 0, &res);
    CHK_TRUE(res.chunks > 4u);        /* really is chunked */
    CHK_INT(sum, total);
    CHK_INT(res.records, total);
    CHK_INT(bb_jbody(bb_last_chunk(), "more"), 0);
    CHK_INT(bb_jbody(bb_last_chunk(), "next"), (long long)total);

    t_case("...and every record appears exactly once in the reassembly");
    /* One rendered line per record: "[<t_ms>] <sys|app>/<writer>: <text>". */
    CHK_INT(count_of(cat, "] app/chunker: A"), 172u);
    CHK_INT(count_of(cat, "] sys/system: boot "), 1u);
    for (i = 0; i < 172u; i++) {
        char needle[32];
        sprintf(needle, "chunker: A%06u:", i);
        t_checks++;
        if (count_of(cat, needle) != 1u) {
            t_head(__FILE__, __LINE__);
            printf("     record %u appears %u times in the reassembled reply\n",
                   i, count_of(cat, needle));
        }
    }

    t_case("Q5: reading the ring did not change it");
    {
        term_lp_stats_t a, b;
        unsigned n1 = bb_live_records();
        memset(&a, 0, sizeof a);
        term_lp_stats(&a);
        (void)pull(800, "live", 0, &res);
        memset(&b, 0, sizeof b);
        term_lp_stats(&b);
        CHK_INT(memcmp(&a, &b, sizeof a), 0);
        CHK_INT(bb_live_records(), n1);
    }

    t_case("`from` resumes exactly where the previous chunk's `next` pointed");
    {
        uint32_t mid = (uint32_t)(total / 2u);
        unsigned tail = pull(900, "live", mid, &res);
        CHK_INT(tail, total - mid);
        CHK_INT(bb_jint(bb.chunk[0], "from"), (long long)mid);
        CHK_INT(bb_jbody(bb_last_chunk(), "next"), (long long)total);
        CHK_INT(bb_jbody(bb_last_chunk(), "more"), 0);
        /* The first line of the resumed pull is the mid-th line of the whole
         * dump — ground truth straight out of term_lp_ring.h's own reader. */
        CHK_TRUE(term_lp_dump_json_ex(TERM_LP_SRC_LIVE, mid, refbuf,
                                      sizeof refbuf, NULL) > 0);
        {
            const char *ref = strstr(refbuf, "\"lines\":[\"");
            const char *got = strstr(bb.chunk[0], "\"lines\":[\"");
            REQUIRE(ref && got);
            CHK_INT(strncmp(ref, got, 48), 0);
        }
    }

    t_case("resuming chunk by chunk reassembles the whole log and terminates");
    {
        uint32_t from = 0;
        unsigned rounds = 0, seen = 0;
        uint64_t ctr = 1000;
        for (;;) {
            long long more, recs;
            bb.chunks = 0;
            bb.publish_calls = 0;
            bb.mark = ctr - 1u;
            CHK_INT(bb_serve(request(ctr++, "live", from), &res), TERM_BB_OK);
            REQUIRE(bb.chunks >= 1u);
            /* Read only the FIRST chunk of each reply and re-pull from its
             * cursor: the cursor chain has to be usable in isolation, which is
             * how a reader recovers from a lost QoS 0 chunk. */
            recs = bb_jbody(bb.chunk[0], "records");
            more = bb_jbody(bb.chunk[0], "more");
            CHK_INT(bb_jint(bb.chunk[0], "from"), (long long)from);
            if (recs <= 0) { CHK_TRUE(more == 1 || bb.chunks == 1u); break; }
            seen += (unsigned)recs;
            from = (uint32_t)bb_jbody(bb.chunk[0], "next");
            CHK_INT(from, (long long)(seen));
            if (more == 0) break;
            if (++rounds > 300u) break;
        }
        CHK_TRUE(rounds <= 300u);
        CHK_INT(seen, total);
    }
}

/* ===================================================================== */
/* The two honesty flags                                                 */
/* ===================================================================== */

static void case_stall(void)
{
    term_bb_result_t res;
    unsigned before;

    t_case("a buffer that cannot hold one rendered line says stall:1, last:1");
    /* term_lp_ring.h: "A buffer too small for even one rendered line answers
     * records: 0, more: 1, so a chunking loop must advance on records > 0
     * rather than on more alone." term_bb_pull.h turns that into an explicit
     * flag so the READER can see why the pull stopped. */
    bb_reset();
    before = bb_live_records();
    CHK_INT(bb_fill(4u, BIG_PAYLOAD, BIG_WRITER), 4u);
    CHK_TRUE(bb_live_records() > before);

    bb_scratch(TERM_BB_SCRATCH_MIN);
    bb.mark = 0;
    /* Start the pull AT the first oversized record, so the stall happens on
     * chunk 0 and nothing else can be confused with it. */
    CHK_INT(bb_serve(request(2000, "live", before), &res), TERM_BB_OK);
    CHK_INT(res.chunks, 1);
    CHK_INT(res.records, 0);
    CHK_TRUE(bb_has(bb.chunk[0], "\"stall\":1"));
    CHK_INT(bb_jlast(bb.chunk[0]), 1);
    CHK_INT(bb_jbody(bb.chunk[0], "records"), 0);
    CHK_INT(bb_jbody(bb.chunk[0], "more"), 1);
    CHK_INT(bb_jbody(bb.chunk[0], "next"), (long long)before);
    CHK_TRUE(!bb_has(bb.chunk[0], "\"cut\":1"));
    CHK_TRUE(bb_scratch_intact());
    (void)bb_check_reply(&res);

    t_case("...and it does not spin: the loop stopped rather than repeating");
    CHK_INT(bb.publish_calls, 1);

    t_case("the same records DO come out through a scratch that fits them");
    bb_reset();
    bb.mark = 0;
    CHK_INT(bb_serve(request(2100, "live", before), &res), TERM_BB_OK);
    CHK_TRUE(res.records >= 4u);
    concat_chunks();
    CHK_INT(count_of(cat, "] app/" BIG_WRITER ": A"), 4u);
    CHK_TRUE(!bb_has(bb_last_chunk(), "\"stall\":1"));
}

static void case_cut(void)
{
    term_bb_result_t res;
    unsigned total, sum;

    t_case("TERM_BB_CHUNKS_MAX chunks with records left says cut:1, last:1");
    bb_reset();
    bb_scratch(TERM_BB_SCRATCH_MIN);
    (void)bb_fill(2000u, 8u, "w");
    total = bb_live_records();
    CHK_TRUE(total > 1000u);          /* eviction leaves a dense ring */
    bb.mark = 0;
    CHK_INT(bb_serve(request(3000, "live", 0), &res), TERM_BB_OK);
    CHK_INT(res.chunks, TERM_BB_CHUNKS_MAX);
    CHK_TRUE(res.records < total);    /* records really did remain */
    CHK_TRUE(bb_has(bb_last_chunk(), "\"cut\":1"));
    CHK_INT(bb_jlast(bb_last_chunk()), 1);
    CHK_INT(bb_jbody(bb_last_chunk(), "more"), 1);
    CHK_TRUE(!bb_has(bb_last_chunk(), "\"stall\":1"));
    sum = bb_check_reply(&res);
    CHK_INT(sum, res.records);

    t_case("...and the cut reply says where to resume, so a re-pull finishes");
    {
        uint32_t next = (uint32_t)bb_jbody(bb_last_chunk(), "next");
        unsigned got = res.records;
        uint64_t ctr = 3001;
        int rounds = 0;
        CHK_INT(next, (long long)got);
        while (rounds++ < 8) {
            bb.chunks = 0;
            bb.publish_calls = 0;
            bb.mark = ctr - 1u;
            CHK_INT(bb_serve(request(ctr++, "live", next), &res), TERM_BB_OK);
            got += res.records;
            next = (uint32_t)bb_jbody(bb_last_chunk(), "next");
            if (!bb_has(bb_last_chunk(), "\"cut\":1")) break;
        }
        CHK_TRUE(rounds < 8);
        CHK_INT(got, total);
    }

    t_case("the chunk bound is never exceeded, whatever the ring holds");
    bb_reset();
    bb_scratch(TERM_BB_SCRATCH_MIN);
    (void)bb_fill(600u, 60u, "w2");
    bb.mark = 0;
    CHK_INT(bb_serve(request(4000, "live", 0), &res), TERM_BB_OK);
    CHK_TRUE(res.chunks <= TERM_BB_CHUNKS_MAX);
    CHK_TRUE(bb.chunks <= TERM_BB_CHUNKS_MAX);
    CHK_INT(bb_jlast(bb_last_chunk()), 1);
    /* Whichever way it ended, it said so. */
    CHK_TRUE(bb_jbody(bb_last_chunk(), "more") == 0 ||
             bb_has(bb_last_chunk(), "\"cut\":1") ||
             bb_has(bb_last_chunk(), "\"stall\":1"));
    (void)bb_check_reply(&res);
}

/* ===================================================================== */
/* Every chunk stays inside the buffer it was given                       */
/* ===================================================================== */

static void case_scratch_sweep(void)
{
    term_bb_result_t res;
    size_t cap;

    t_case("every legal scratch size answers with whole chunks and no overrun");
    for (cap = TERM_BB_SCRATCH_MIN; cap <= TERM_BB_SCRATCH_MIN + 96u; cap += 8u) {
        bb_reset();
        bb_scratch(cap);
        bb.mark = 0;
        CHK_INT(bb_serve(request(5000 + (uint64_t)cap, "live", 0), &res),
                TERM_BB_OK);
        CHK_TRUE(bb_scratch_intact());
        CHK_TRUE(bb.max_chunk < cap);
        CHK_TRUE(bb.chunks >= 1u);
        CHK_INT(bb_jlast(bb_last_chunk()), 1);
        CHK_INT(bb.bad_len, 0);
    }
}

int main(void)
{
    t_suite("bb_serve");
    t_case("the port installs and the ring boots");
    fp_reset();
    CHK_TRUE(term_port_install(fp_port()));
    term_lp_ring_boot();
    CHK_TRUE(term_lp_ring_ready());

    case_stats();
    case_lastboot_absent();
    case_live_one_chunk();
    case_live_many_chunks();
    case_stall();
    case_cut();
    case_scratch_sweep();

    fp_reclaim_all();
    return t_summary();
}
