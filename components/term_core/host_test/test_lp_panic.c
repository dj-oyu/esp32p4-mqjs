/*
 * test_lp_panic.c — the panic note (§4.4's "panic 理由", PHASE3_MANIFEST §2a).
 *
 * term_lp_ring.h splits this in two on purpose — "The formatter is separate
 * from the append (term_lp_panic_fmt) so the whole of the interesting logic is
 * pure and host-testable" — and this suite is that split:
 *
 *   term_lp_panic_fmt: "Pure, allocation-free, NUL-terminated, never longer
 *     than `cap - 1`; returns the length written (0 if `cap` is hopeless).
 *     Strings are clipped field by field so a long task name cannot push the
 *     numbers out — the numbers are the part a human cannot reconstruct."
 *
 *       "panic: fault Load access fault task=js_task core=0 pc=0x4800f2a4 cause=5"
 *
 *   term_lp_panic_note: "Write the note into the live ring's SYS partition as
 *     writer "panic"." — "no allocation, no lock, no FreeRTOS call, no
 *     logging" — "it is one-shot. A second call (a panic inside the panic
 *     handler) does nothing".
 *
 * WHERE THE STRIPPING IS CHECKED. term_lp_ring.h assigns it to the append (P4:
 * "term_lp_ring_append() runs term_lp_strip() on every byte it accepts; there
 * is no API that writes raw bytes"), and says nothing about the formatter. So
 * the note is driven with an ESC-bearing reason string and the RECORD is
 * required to equal term_lp_strip() of the formatter's output — the public
 * strip function as ground truth, rather than a hand-written expectation.
 *
 * NOT REACHABLE HERE, and honestly so: the mid-update geometry refusal. The
 * header documents that term_lp_panic_note "REVALIDATES the DRAM shadow
 * header's geometry first and REFUSES to append when it is mid-update"; the
 * shadow belongs to the module's static live ring and a host test cannot get
 * between two of its appends. That path is a device concern (and the predicate
 * itself is term_lp_ring_open's, which test_lp_format covers).
 *
 * The note is ONE-SHOT per process, so this suite owns a process: the single
 * successful note is spent in case_note_lands_in_the_sys_partition().
 */
#include "lp_util.h"
#include "fake_port.h"

#define FILL_BYTE 0xAA

static const term_lp_panic_t doc_fault = {
    "fault", "Load access fault", "js_task", 0x4800f2a4u, 5, 0
};
static const term_lp_panic_t doc_abort = {
    "abort", "assert failed: foo bar", "IDLE", 0x40001234u, 2, 1
};

/* Format into a buffer pre-filled with FILL_BYTE and past-the-end canaries, and
 * hold every bound the header states. Returns the length written. */
static size_t fmt_into(char *out, size_t out_cap, size_t cap,
                       const term_lp_panic_t *p)
{
    size_t n;
    memset(out, FILL_BYTE, out_cap);
    n = term_lp_panic_fmt(out, cap, p);

    CHK_TRUE(n < cap || (n == 0u && cap <= 1u));
    if (cap) {
        size_t i;
        int past = 0;
        for (i = cap; i < out_cap; i++)
            if ((unsigned char)out[i] != FILL_BYTE) past++;
        t_checks++;
        if (past) {
            t_head(__FILE__, __LINE__);
            printf("     the formatter wrote %d byte(s) past cap %lu\n", past,
                   (unsigned long)cap);
        }
    }
    if (n) {
        CHK_INT(out[n], 0);                     /* NUL-terminated */
        CHK_INT(t_strnlen(out, cap), n);         /* ... at exactly n */
    }
    return n;
}

/* ===================================================================== */
/* The documented lines                                                  */
/* ===================================================================== */

static void case_the_documented_lines(void)
{
    char b[512];
    size_t n;

    t_case("the header's own fault line, byte for byte");
    n = fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &doc_fault);
    CHK_STR(b, "panic: fault Load access fault task=js_task core=0 "
               "pc=0x4800f2a4 cause=5");
    CHK_INT(n, strlen("panic: fault Load access fault task=js_task core=0 "
                      "pc=0x4800f2a4 cause=5"));

    t_case("the manifest's abort line, byte for byte");
    n = fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &doc_abort);
    CHK_STR(b, "panic: abort assert failed: foo bar task=IDLE core=1 "
               "pc=0x40001234 cause=2");
    CHK_TRUE(n > 0);

    t_case("pc is eight lowercase hex digits and cause is decimal");
    {
        term_lp_panic_t p = doc_fault;
        p.pc = 1;
        p.cause = 0;
        p.core = 0;
        (void)fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &p);
        CHK_TRUE(strstr(b, "pc=0x00000001") != NULL);
        CHK_TRUE(strstr(b, "cause=0") != NULL);
        p.pc = 0xFFFFFFFFu;
        p.cause = 0xFFFFFFFFu;
        p.core = 1;
        (void)fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &p);
        CHK_TRUE(strstr(b, "pc=0xffffffff") != NULL);
        CHK_TRUE(strstr(b, "cause=4294967295") != NULL);
        CHK_TRUE(strstr(b, "core=1") != NULL);
    }

    t_case("every string may be NULL, and the numbers still come through");
    {
        term_lp_panic_t p;
        memset(&p, 0, sizeof p);
        p.pc = 0x4001abcdu;
        p.cause = 7;
        p.core = 1;
        n = fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &p);
        CHK_TRUE(n > 0);
        CHK_INT(memcmp(b, "panic: ", 7), 0);
        CHK_TRUE(strstr(b, "task=") != NULL);
        CHK_TRUE(strstr(b, "core=1") != NULL);
        CHK_TRUE(strstr(b, "pc=0x4001abcd") != NULL);
        CHK_TRUE(strstr(b, "cause=7") != NULL);
        /* No "(null)" and no stray punctuation from a missing field. */
        CHK_TRUE(strstr(b, "(null)") == NULL);
        CHK_TRUE(strstr(b, "nil") == NULL);
    }

    t_case("a NULL out or a NULL info writes nothing and says so");
    CHK_INT(term_lp_panic_fmt(NULL, 100, &doc_fault), 0);
    CHK_INT(term_lp_panic_fmt(b, sizeof b, NULL), 0);
    CHK_INT(term_lp_panic_fmt(NULL, 0, NULL), 0);
    CHK_INT(term_lp_panic_fmt(b, 0, &doc_fault), 0);

    t_case("the formatter is pure: the same input, the same bytes, no side "
           "effects");
    {
        char c[512];
        term_lp_stats_t s1, s2;
        int allocs;
        memset(&s1, 0, sizeof s1);
        term_lp_stats(&s1);
        allocs = fp.allocs;
        fp.alloc_frozen = true;
        (void)fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &doc_fault);
        (void)fmt_into(c, sizeof c, TERM_LP_PANIC_MAX, &doc_fault);
        fp.alloc_frozen = false;
        CHK_STR(c, b);
        CHK_INT(fp.allocs, allocs);
        CHK_INT(fp.alloc_violations, 0);
        memset(&s2, 0, sizeof s2);
        term_lp_stats(&s2);
        CHK_INT(memcmp(&s1, &s2, sizeof s1), 0);
    }
}

/* ===================================================================== */
/* Clipping: the numbers are the part a human cannot reconstruct           */
/* ===================================================================== */

static void case_clipping_keeps_the_numbers(void)
{
    static char big[300];
    char b[512];
    term_lp_panic_t p;
    size_t n;

    memset(big, 'L', sizeof big - 1u);
    big[sizeof big - 1u] = 0;

    t_case("a 299-byte kind, reason and task still leave pc= and cause= intact");
    p.kind = big;
    p.reason = big;
    p.task = big;
    p.pc = 0xdeadbeefu;
    p.cause = 0xFFFFFFFFu;
    p.core = 7;
    n = fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &p);
    CHK_TRUE(n > 0 && n < TERM_LP_PANIC_MAX);
    CHK_TRUE(strstr(b, "pc=0xdeadbeef") != NULL);
    CHK_TRUE(strstr(b, "cause=4294967295") != NULL);
    CHK_TRUE(strstr(b, "core=7") != NULL);
    CHK_TRUE(strstr(b, "task=") != NULL);

    t_case("...and the clipping really is field by field, not one field eating "
           "the line");
    /* Each of the three strings appears, and none of them got all the room:
     * a formatter that appended kind unbounded would leave no `task=` at all,
     * which the check above already catches; this pins that all three are
     * present and each is shorter than the whole. */
    {
        const char *task = strstr(b, "task=");
        size_t kind_run = 0, i;
        REQUIRE(task != NULL);
        for (i = 7; b[i] == 'L'; i++) kind_run++;   /* after "panic: " */
        CHK_TRUE(kind_run > 0);
        CHK_TRUE(kind_run < strlen(big));
        CHK_TRUE(strlen(task) > 5u);
    }

    t_case("one long field does not starve the others");
    p.kind = "fault";
    p.reason = big;
    p.task = "js_task";
    n = fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &p);
    CHK_TRUE(n > 0 && n < TERM_LP_PANIC_MAX);
    CHK_TRUE(strstr(b, "panic: fault ") == b);
    CHK_TRUE(strstr(b, "task=js_task") != NULL);
    CHK_TRUE(strstr(b, "pc=0xdeadbeef") != NULL);

    p.kind = big;
    p.reason = "short";
    p.task = "js_task";
    n = fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &p);
    CHK_TRUE(n > 0 && n < TERM_LP_PANIC_MAX);
    CHK_TRUE(strstr(b, "task=js_task") != NULL);
    CHK_TRUE(strstr(b, "cause=") != NULL);

    t_case("empty strings are legal and produce a line, not a crash");
    p.kind = "";
    p.reason = "";
    p.task = "";
    n = fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &p);
    CHK_TRUE(n > 0);
    CHK_INT(memcmp(b, "panic:", 6), 0);
    CHK_TRUE(strstr(b, "pc=0xdeadbeef") != NULL);

    t_case("a negative core is rendered, whatever it is, within the bound");
    p.kind = "fault";
    p.reason = "r";
    p.task = "t";
    p.core = -1;
    n = fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &p);
    CHK_TRUE(n > 0 && n < TERM_LP_PANIC_MAX);
    CHK_TRUE(strstr(b, "core=") != NULL);
    CHK_TRUE(strstr(b, "pc=0xdeadbeef") != NULL);
}

/* ===================================================================== */
/* The buffer bound at every size                                        */
/* ===================================================================== */

static void case_every_cap(void)
{
    char b[512];
    size_t cap;
    unsigned unwritten = 0, worst_cap = 0;

    t_case("no cap makes it overrun, half-terminate or exceed cap-1");
    for (cap = 0; cap <= 200u; cap++) {
        size_t n = fmt_into(b, sizeof b, cap, &doc_fault);
        if (!n) continue;
        /* "returns the length written": every byte in [0,n) has to BE output,
         * so none of them may still hold the caller's fill byte. */
        if (memchr(b, (char)FILL_BYTE, n) != NULL) {
            unwritten++;
            if (!worst_cap) worst_cap = (unsigned)cap;
        }
    }

    t_case("...and the length it returns is the length it actually wrote");
    t_checks++;
    if (unwritten) {
        t_head(__FILE__, __LINE__);
        printf("     %u cap value(s) (from cap=%u) returned a length longer than\n"
               "     the text written: term_lp_panic_fmt returns cap-1 when the\n"
               "     line does not fit, but leaves the bytes between the end of\n"
               "     the text and cap-1 UNWRITTEN, so the caller's own buffer\n"
               "     contents end up inside the returned string. The header says\n"
               "     \"returns the length written\".\n", unwritten, worst_cap);
    }

    t_case("a full-size buffer always holds the whole line, for any input");
    {
        term_lp_panic_t p;
        unsigned i;
        static char pad[400];
        for (i = 0; i < 64u; i++) {
            size_t k = (size_t)i * 6u;
            if (k >= sizeof pad) k = sizeof pad - 1u;
            memset(pad, 'x', k);
            pad[k] = 0;
            p.kind = pad;
            p.reason = pad;
            p.task = pad;
            p.pc = 0x12345678u + i;
            p.cause = i;
            p.core = (int)(i & 1u);
            {
                size_t n = fmt_into(b, sizeof b, TERM_LP_PANIC_MAX, &p);
                char want[64];
                sprintf(want, "pc=0x%08x", 0x12345678u + i);
                CHK_TRUE(n > 0 && n < TERM_LP_PANIC_MAX);
                CHK_TRUE(strstr(b, want) != NULL);
                CHK_TRUE(strstr(b, "cause=") != NULL);
                CHK_INT(memchr(b, (char)FILL_BYTE, n) != NULL, 0);
            }
        }
    }
}

/* ===================================================================== */
/* The note in the ring                                                  */
/* ===================================================================== */

/* The text of the first SYS record whose writer is "panic", NUL-terminated. */
static const char *panic_record(unsigned *out_n, unsigned *out_flags)
{
    static char buf[TERM_LP_REC_MAX + 1];
    const term_lp_ring_t *live = term_lp_source(TERM_LP_SRC_LIVE);
    term_lp_iter_t it;
    term_lp_record_t rec;
    unsigned n = 0;
    buf[0] = 0;
    if (out_n) *out_n = 0;
    if (out_flags) *out_flags = 0;
    if (!live) return buf;
    term_lp_iter_begin(&it, live, TERM_LP_PART_SYS);
    while (term_lp_iter_next(&it, &rec)) {
        if (!rec.writer || strcmp(rec.writer, "panic") != 0) continue;
        n++;
        if (n == 1u) {
            memcpy(buf, rec.text, rec.len);
            buf[rec.len] = 0;
            if (out_flags) *out_flags = rec.flags;
        }
    }
    if (out_n) *out_n = n;
    return buf;
}

static void case_note_lands_in_the_sys_partition(void)
{
    /* An ESC in the reason: the formatter is not documented to strip and the
     * append is (P4), so this is where the two meet. */
    static const term_lp_panic_t p = {
        "fault", "Load \x1b[31maccess\x1b[0m fault", "js_task", 0x4800f2a4u, 5, 0
    };
    char raw[TERM_LP_PANIC_MAX + 16u];
    char want[TERM_LP_PANIC_MAX + 16u];
    size_t rawn, wantn;
    unsigned found = 0, flags = 0;
    term_lp_stats_t before, after;
    int locks, logs, allocs;

    t_case("a NULL info is refused and does not spend the one shot");
    CHK_TRUE(!term_lp_panic_note(NULL));

    t_case("the note is appended to SYS as writer \"panic\"");
    memset(&before, 0, sizeof before);
    term_lp_stats(&before);
    locks = fp.lock_calls;
    logs = fp.log_calls;
    allocs = fp.allocs;
    fp.alloc_frozen = true;
    CHK_TRUE(term_lp_panic_note(&p));
    fp.alloc_frozen = false;
    memset(&after, 0, sizeof after);
    term_lp_stats(&after);
    CHK_INT(after.records[TERM_LP_PART_SYS],
            before.records[TERM_LP_PART_SYS] + 1u);
    CHK_INT(after.appended[TERM_LP_PART_SYS],
            before.appended[TERM_LP_PART_SYS] + 1u);
    CHK_INT(after.records[TERM_LP_PART_APP], before.records[TERM_LP_PART_APP]);
    CHK_INT(after.refused, before.refused);

    t_case("...from panic context: no allocation, no lock, no logging");
    CHK_INT(fp.allocs, allocs);
    CHK_INT(fp.alloc_violations, 0);
    CHK_INT(fp.lock_calls, locks);
    CHK_INT(fp.log_calls, logs);

    t_case("the record is exactly term_lp_strip() of the formatted line (P4)");
    rawn = term_lp_panic_fmt(raw, sizeof raw, &p);
    CHK_TRUE(rawn > 0);
    wantn = term_lp_strip(want, sizeof want, raw, rawn, NULL);
    want[wantn] = 0;
    CHK_STR(panic_record(&found, &flags), want);
    CHK_INT(found, 1);
    CHK_INT(flags & TERM_LP_F_TRUNC, 0);

    t_case("...so no escape byte reached the region");
    {
        const char *rec = panic_record(NULL, NULL);
        size_t i;
        CHK_TRUE(strchr(rec, 0x1b) == NULL);
        CHK_TRUE(strstr(rec, "[31m") == NULL);
        for (i = 0; rec[i]; i++)
            CHK_TRUE((unsigned char)rec[i] >= 0x20u ||
                     rec[i] == '\n' || rec[i] == '\t');
        /* The facts an operator needs survived the strip. */
        CHK_TRUE(strstr(rec, "panic: fault ") == rec);
        CHK_TRUE(strstr(rec, "task=js_task") != NULL);
        CHK_TRUE(strstr(rec, "core=0") != NULL);
        CHK_TRUE(strstr(rec, "pc=0x4800f2a4") != NULL);
        CHK_TRUE(strstr(rec, "cause=5") != NULL);
    }

    t_case("and it reads back through the pullable dump as sys/panic");
    {
        static char dump[TERM_LP_DUMP_MAX];
        CHK_TRUE(term_lp_dump_json(TERM_LP_SRC_LIVE, 0, dump, sizeof dump) > 0);
        CHK_TRUE(strstr(dump, "sys/panic: panic: fault ") != NULL);
        CHK_TRUE(strstr(dump, "pc=0x4800f2a4 cause=5") != NULL);
        CHK_TRUE(strchr(dump, 0x1b) == NULL);
    }
}

static void case_note_is_one_shot(void)
{
    static const term_lp_panic_t q = {
        "abort", "a panic inside the panic handler", "IDLE", 0x40001234u, 2, 1
    };
    term_lp_stats_t before, after;
    unsigned found = 0;

    t_case("a second note does nothing at all");
    memset(&before, 0, sizeof before);
    term_lp_stats(&before);
    CHK_TRUE(!term_lp_panic_note(&q));
    CHK_TRUE(!term_lp_panic_note(&doc_fault));
    CHK_TRUE(!term_lp_panic_note(NULL));
    memset(&after, 0, sizeof after);
    term_lp_stats(&after);
    CHK_INT(memcmp(&before, &after, sizeof before), 0);
    (void)panic_record(&found, NULL);
    CHK_INT(found, 1);

    t_case("...and the formatter still works after the shot is spent");
    {
        char b[256];
        CHK_TRUE(term_lp_panic_fmt(b, sizeof b, &q) > 0);
        CHK_TRUE(strstr(b, "task=IDLE") != NULL);
    }
}

int main(void)
{
    t_suite("lp_panic");
    t_case("the port installs and the ring boots");
    fp_reset();
    CHK_TRUE(term_port_install(fp_port()));
    term_lp_ring_boot();
    CHK_TRUE(term_lp_ring_ready());

    case_the_documented_lines();
    case_clipping_keeps_the_numbers();
    case_every_cap();
    case_note_lands_in_the_sys_partition();
    case_note_is_one_shot();

    fp_reclaim_all();
    return t_summary();
}
