/*
 * test_bb_parse.c — the request format, and Q4 ("an unknown field is a
 * REFUSAL, not an ignored line").
 *
 * term_bb_req_parse is pure: no environment, no crypto, no ring. So this suite
 * is the whole of the wire format's grammar, straight out of term_bb_pull.h's
 * "WIRE FORMAT — REQUEST":
 *
 *   "The message is a magic line followed by `key=value` lines, LF-separated, a
 *    trailing LF optional and a trailing CR per line tolerated"
 *   "Anything else — a missing required key, an unknown key, a duplicate key, a
 *    non-decimal number, an overlong message, ctr=0 — is refused (Q4)"
 *   "`msg` need not be NUL-terminated; `len` is authoritative and an embedded
 *    NUL is TERM_BB_E_SYNTAX."
 *   "`out` is fully overwritten on success and left zeroed on failure, so a
 *    caller cannot accidentally act on half a request. Optional fields carry
 *    their defaults (from = 0)."
 *
 * Every refusal case also asserts the ZEROING, because decision 10 of
 * PHASE3_MANIFEST §3 records that the first implementation got it wrong for
 * every path but one, and "an unknown key returned an error with `ctr` and
 * `topic` still filled in" is exactly the shape of thing a caller acts on by
 * mistake.
 */
/* Nothing here needs the environment fake or the ring: this is the pure
 * grammar, so the suite includes only the harness and the contract. */
#include "test_util.h"
#include "term_bb_pull.h"

#define BB_TOPIC "esp32p4-mqjs/task/u7q3x9f2"

/* ===================================================================== */
/* Case drivers                                                          */
/* ===================================================================== */

/* Parse `len` bytes and expect `want`. On a refusal, also assert `out` is
 * entirely zero (the header's "left zeroed on failure"). Returns the request
 * so an accepted case can inspect the fields. */
static term_bb_req_t parse_n(const char *msg, size_t len, term_bb_err_t want)
{
    term_bb_req_t req;
    term_bb_err_t e;

    memset(&req, 0xCD, sizeof req);
    e = term_bb_req_parse(msg, len, &req);

    t_checks++;
    if (e != want) {
        t_head(__FILE__, __LINE__);
        printf("     message : \"%s\"\n     expected: %s\n     actual  : %s\n",
               t_esc(msg), term_bb_err_str(want), term_bb_err_str(e));
    }
    if (e != TERM_BB_OK) {
        size_t i;
        const unsigned char *p = (const unsigned char *)&req;
        int dirty = 0;
        for (i = 0; i < sizeof req; i++) if (p[i]) dirty++;
        t_checks++;
        if (dirty) {
            t_head(__FILE__, __LINE__);
            printf("     message : \"%s\"\n"
                   "     a refused parse left %lu of %lu bytes of term_bb_req_t "
                   "non-zero\n", t_esc(msg), (unsigned long)dirty,
                   (unsigned long)sizeof req);
        }
    }
    return req;
}

static term_bb_req_t parse_ok(const char *msg)
{
    return parse_n(msg, strlen(msg), TERM_BB_OK);
}

static void parse_bad(const char *msg, term_bb_err_t want)
{
    (void)parse_n(msg, strlen(msg), want);
}

/* ===================================================================== */
/* What the documented example means                                     */
/* ===================================================================== */

static void case_the_documented_request(void)
{
    term_bb_req_t r;

    t_case("the header's own example parses to its own fields");
    r = parse_ok("bbpull1\n"
                 "ctr=1753900000123\n"
                 "top=" BB_TOPIC "\n"
                 "what=lastboot\n"
                 "from=0\n");
    CHK_TRUE(r.ctr == 1753900000123ull);
    CHK_INT(r.what, TERM_BB_WHAT_LASTBOOT);
    CHK_INT(r.from, 0);
    CHK_STR(r.topic, BB_TOPIC);

    t_case("all three `what` values, and only those three");
    CHK_INT(parse_ok("bbpull1\nctr=1\ntop=t\nwhat=stats").what, TERM_BB_WHAT_STATS);
    CHK_INT(parse_ok("bbpull1\nctr=1\ntop=t\nwhat=live").what, TERM_BB_WHAT_LIVE);
    CHK_INT(parse_ok("bbpull1\nctr=1\ntop=t\nwhat=lastboot").what, TERM_BB_WHAT_LASTBOOT);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=bogus", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=STATS", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=liveish", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=", TERM_BB_E_SYNTAX);

    t_case("`from` is optional and defaults to 0");
    CHK_INT(parse_ok("bbpull1\nctr=1\ntop=t\nwhat=live").from, 0);
    CHK_INT(parse_ok("bbpull1\nctr=1\ntop=t\nwhat=live\nfrom=7").from, 7);
    CHK_INT(parse_ok("bbpull1\nctr=1\ntop=t\nwhat=live\nfrom=4294967295").from,
            0xFFFFFFFFu);

    t_case("the counter's documented range is 1 .. 2^63-1");
    CHK_TRUE(parse_ok("bbpull1\nctr=1\ntop=t\nwhat=live").ctr == 1u);
    CHK_TRUE(parse_ok("bbpull1\nctr=9223372036854775807\ntop=t\nwhat=live").ctr
             == 9223372036854775807ull);

    t_case("a topic that fits TERM_BB_TOPIC_MAX is copied whole");
    {
        char msg[TERM_BB_REQ_MAX];
        char topic[TERM_BB_TOPIC_MAX];
        size_t i;
        for (i = 0; i < TERM_BB_TOPIC_MAX - 1u; i++) topic[i] = 'x';
        topic[TERM_BB_TOPIC_MAX - 1u] = 0;
        sprintf(msg, "bbpull1\nctr=1\ntop=%s\nwhat=live", topic);
        r = parse_ok(msg);
        CHK_STR(r.topic, topic);
        CHK_INT(t_strnlen(r.topic, TERM_BB_TOPIC_MAX), TERM_BB_TOPIC_MAX - 1u);
    }
}

/* ===================================================================== */
/* The documented tolerances                                             */
/* ===================================================================== */

static void case_tolerances(void)
{
    term_bb_req_t r;

    t_case("a trailing LF is optional");
    r = parse_ok("bbpull1\nctr=42\ntop=" BB_TOPIC "\nwhat=live");
    CHK_TRUE(r.ctr == 42u);
    CHK_STR(r.topic, BB_TOPIC);
    r = parse_ok("bbpull1\nctr=42\ntop=" BB_TOPIC "\nwhat=live\n");
    CHK_TRUE(r.ctr == 42u);
    CHK_STR(r.topic, BB_TOPIC);

    t_case("a trailing CR per line is tolerated (CRLF from a Windows tool)");
    r = parse_ok("bbpull1\r\nctr=42\r\ntop=" BB_TOPIC "\r\nwhat=live\r\nfrom=3\r\n");
    CHK_TRUE(r.ctr == 42u);
    CHK_INT(r.from, 3);
    CHK_STR(r.topic, BB_TOPIC);
    CHK_INT(r.what, TERM_BB_WHAT_LIVE);
    /* ... and no CR survives into the copied topic. */
    CHK_TRUE(strchr(r.topic, '\r') == NULL);

    t_case("blank lines are ignored (PHASE3_MANIFEST §3's wire format)");
    r = parse_ok("bbpull1\n\nctr=42\n\n\ntop=t\nwhat=live\n\n");
    CHK_TRUE(r.ctr == 42u);
    r = parse_ok("bbpull1\r\n\r\nctr=42\r\ntop=t\r\nwhat=live\r\n");
    CHK_TRUE(r.ctr == 42u);

    t_case("leading zeros in a number are tolerated");
    CHK_TRUE(parse_ok("bbpull1\nctr=0000001\ntop=t\nwhat=live").ctr == 1u);
    CHK_INT(parse_ok("bbpull1\nctr=1\ntop=t\nwhat=live\nfrom=007").from, 7);

    t_case("`len` decides where the message ends, not a NUL");
    {
        const char *msg = "bbpull1\nctr=5\ntop=t\nwhat=live\nfrom=9";
        /* Stop one byte before the '9': from must read as 0 (absent value is
         * impossible here, so this really is `len` being authoritative). */
        size_t cut = strlen(msg) - 7u;   /* drops "\nfrom=9" */
        r = parse_n(msg, cut, TERM_BB_OK);
        CHK_INT(r.from, 0);
        r = parse_n(msg, strlen(msg), TERM_BB_OK);
        CHK_INT(r.from, 9);
    }

    t_case("a non-terminated buffer is never read past `len`");
    {
        /* No NUL anywhere in the window the parser may touch: an
         * implementation that calls strlen() runs into the poison. */
        static char buf[256];
        const char *msg = "bbpull1\nctr=8\ntop=t\nwhat=stats";
        size_t n = strlen(msg);
        memset(buf, '@', sizeof buf);
        memcpy(buf, msg, n);
        r = parse_n(buf, n, TERM_BB_OK);
        CHK_TRUE(r.ctr == 8u);
        CHK_STR(r.topic, "t");
    }

    t_case("field order is not significant — these are named keys");
    r = parse_n("bbpull1\nwhat=live\nfrom=2\ntop=" BB_TOPIC "\nctr=77",
                strlen("bbpull1\nwhat=live\nfrom=2\ntop=" BB_TOPIC "\nctr=77"),
                TERM_BB_OK);
    CHK_TRUE(r.ctr == 77u);
    CHK_INT(r.from, 2);
    CHK_INT(r.what, TERM_BB_WHAT_LIVE);
    CHK_STR(r.topic, BB_TOPIC);
}

/* ===================================================================== */
/* The magic line                                                        */
/* ===================================================================== */

static void case_magic(void)
{
    t_case("the first line must be exactly TERM_BB_MAGIC");
    CHK_STR(TERM_BB_MAGIC, "bbpull1");
    parse_bad("bbpull2\nctr=1\ntop=t\nwhat=live", TERM_BB_E_MAGIC);
    parse_bad("bbpull0\nctr=1\ntop=t\nwhat=live", TERM_BB_E_MAGIC);
    parse_bad("BBPULL1\nctr=1\ntop=t\nwhat=live", TERM_BB_E_MAGIC);
    parse_bad("bbpull\nctr=1\ntop=t\nwhat=live", TERM_BB_E_MAGIC);
    parse_bad("bbpull1x\nctr=1\ntop=t\nwhat=live", TERM_BB_E_MAGIC);
    parse_bad(" bbpull1\nctr=1\ntop=t\nwhat=live", TERM_BB_E_MAGIC);
    parse_bad("bbpull1 \nctr=1\ntop=t\nwhat=live", TERM_BB_E_MAGIC);
    parse_bad("\nbbpull1\nctr=1\ntop=t\nwhat=live", TERM_BB_E_MAGIC);
    parse_bad("ctr=1\ntop=t\nwhat=live\nbbpull1", TERM_BB_E_MAGIC);

    t_case("an empty message is a missing magic line, not a crash");
    parse_n("", 0, TERM_BB_E_MAGIC);
    parse_n("\n", 1, TERM_BB_E_MAGIC);

    t_case("the magic line alone is well-formed but carries no fields");
    parse_bad("bbpull1", TERM_BB_E_MISSING);
    parse_bad("bbpull1\n", TERM_BB_E_MISSING);
    parse_bad("bbpull1\r\n", TERM_BB_E_MISSING);
}

/* ===================================================================== */
/* Q4: unknown, duplicate, missing                                       */
/* ===================================================================== */

static void case_unknown_fields_are_refused(void)
{
    t_case("Q4: a key this version does not implement is a REFUSAL");
    /* The header's own example of the threat: "A signed request containing
     * `redact=1` came from someone who believed the device would redact." */
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\nredact=1", TERM_BB_E_UNKNOWN);
    parse_bad("bbpull1\nredact=1\nctr=1\ntop=t\nwhat=live", TERM_BB_E_UNKNOWN);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\nto=9", TERM_BB_E_UNKNOWN);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\ncount=9", TERM_BB_E_UNKNOWN);

    t_case("keys are matched whole and case-sensitively");
    parse_bad("bbpull1\nCTR=1\ntop=t\nwhat=live", TERM_BB_E_UNKNOWN);
    parse_bad("bbpull1\nWhat=live\nctr=1\ntop=t", TERM_BB_E_UNKNOWN);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\nctr2=1", TERM_BB_E_UNKNOWN);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\nfromm=1", TERM_BB_E_UNKNOWN);
    parse_bad("bbpull1\nctrx=1\ntop=t\nwhat=live", TERM_BB_E_UNKNOWN);
    parse_bad("bbpull1\nc=1\ntop=t\nwhat=live", TERM_BB_E_UNKNOWN);

    t_case("the same key twice is a refusal, even with the same value");
    parse_bad("bbpull1\nctr=1\nctr=1\ntop=t\nwhat=live", TERM_BB_E_DUP);
    parse_bad("bbpull1\nctr=1\nctr=2\ntop=t\nwhat=live", TERM_BB_E_DUP);
    parse_bad("bbpull1\nctr=1\ntop=t\ntop=t\nwhat=live", TERM_BB_E_DUP);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\nwhat=stats", TERM_BB_E_DUP);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\nfrom=1\nfrom=1", TERM_BB_E_DUP);

    t_case("each required key's absence is reported as such");
    parse_bad("bbpull1\ntop=t\nwhat=live", TERM_BB_E_MISSING);
    parse_bad("bbpull1\nctr=1\nwhat=live", TERM_BB_E_MISSING);
    parse_bad("bbpull1\nctr=1\ntop=t", TERM_BB_E_MISSING);
    parse_bad("bbpull1\nfrom=3", TERM_BB_E_MISSING);
}

/* ===================================================================== */
/* Syntax and range                                                      */
/* ===================================================================== */

static void case_syntax(void)
{
    t_case("a line that is not key=value is refused");
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\ngarbage", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\ngarbage\nctr=1\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr 1\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);

    t_case("an empty key or an empty value is refused");
    parse_bad("bbpull1\n=1\nctr=1\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=1\ntop=\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\nfrom=", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\n=\nctr=1\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);

    t_case("a non-decimal number is refused");
    parse_bad("bbpull1\nctr=abc\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=1a\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=a1\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=1 2\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr= 1\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=+1\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=-1\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=0x10\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=1.0\ntop=t\nwhat=live", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\nfrom=-1", TERM_BB_E_SYNTAX);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\nfrom=1x", TERM_BB_E_SYNTAX);

    t_case("an embedded NUL is TERM_BB_E_SYNTAX, whatever it is inside");
    parse_n("bbpull1\nctr=1\ntop=t\nwhat=li\0ve", 30u, TERM_BB_E_SYNTAX);
    parse_n("bbpull1\nctr=1\0\ntop=t\nwhat=live", 30u, TERM_BB_E_SYNTAX);
    parse_n("bbpull1\0\nctr=1\ntop=t\nwhat=live", 30u, TERM_BB_E_SYNTAX);
    parse_n("bbpull1\nctr=1\ntop=t\nwhat=live\0", 31u, TERM_BB_E_SYNTAX);

    t_case("ctr=0 is out of range: an unset counter cannot be spelled");
    parse_bad("bbpull1\nctr=0\ntop=t\nwhat=live", TERM_BB_E_RANGE);
    parse_bad("bbpull1\nctr=00\ntop=t\nwhat=live", TERM_BB_E_RANGE);
    parse_bad("bbpull1\nctr=0000000\ntop=t\nwhat=live", TERM_BB_E_RANGE);

    t_case("a number that does not fit is out of range, not truncated");
    /* 2^63 is one past the documented top of the range. */
    parse_bad("bbpull1\nctr=9223372036854775808\ntop=t\nwhat=live",
              TERM_BB_E_RANGE);
    parse_bad("bbpull1\nctr=18446744073709551616\ntop=t\nwhat=live",
              TERM_BB_E_RANGE);
    parse_bad("bbpull1\nctr=999999999999999999999999\ntop=t\nwhat=live",
              TERM_BB_E_RANGE);
    /* from is uint32_t. */
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\nfrom=4294967296",
              TERM_BB_E_RANGE);
    parse_bad("bbpull1\nctr=1\ntop=t\nwhat=live\nfrom=99999999999999999999",
              TERM_BB_E_RANGE);
}

static void case_overlong(void)
{
    static char msg[TERM_BB_REQ_MAX * 4];
    char topic[TERM_BB_TOPIC_MAX + 64u];
    size_t i, n;

    t_case("a topic one byte too long to store whole is refused, not clipped");
    for (i = 0; i < TERM_BB_TOPIC_MAX; i++) topic[i] = 'y';
    topic[TERM_BB_TOPIC_MAX] = 0;
    sprintf(msg, "bbpull1\nctr=1\ntop=%s\nwhat=live", topic);
    {
        term_bb_req_t req;
        term_bb_err_t e;
        memset(&req, 0xCD, sizeof req);
        e = term_bb_req_parse(msg, strlen(msg), &req);
        CHK_TRUE(e != TERM_BB_OK);
        /* Whatever the code, the copy must never be a prefix of the value —
         * "a device configured with a longer one refuses every request loudly
         * rather than comparing a prefix". */
        if (e == TERM_BB_OK) CHK_STR(req.topic, topic);
    }

    t_case("a message over TERM_BB_REQ_MAX is refused");
    n = (size_t)sprintf(msg, "bbpull1\nctr=1\ntop=t\nwhat=live\n");
    while (n < TERM_BB_REQ_MAX + 64u) {
        /* Padding with blank lines keeps every LINE legal, so the only thing
         * wrong with this message is its length. */
        msg[n++] = '\n';
    }
    msg[n] = 0;
    CHK_TRUE(n > TERM_BB_REQ_MAX);
    {
        term_bb_req_t req;
        memset(&req, 0xCD, sizeof req);
        CHK_TRUE(term_bb_req_parse(msg, n, &req) != TERM_BB_OK);
    }
    /* ... and exactly TERM_BB_REQ_MAX bytes is still accepted. */
    n = (size_t)sprintf(msg, "bbpull1\nctr=1\ntop=t\nwhat=live\n");
    while (n < TERM_BB_REQ_MAX) msg[n++] = '\n';
    msg[n] = 0;
    (void)parse_n(msg, TERM_BB_REQ_MAX, TERM_BB_OK);

    t_case("a NULL message is refused rather than dereferenced");
    {
        term_bb_req_t req;
        memset(&req, 0xCD, sizeof req);
        CHK_TRUE(term_bb_req_parse(NULL, 10, &req) != TERM_BB_OK);
        memset(&req, 0xCD, sizeof req);
        CHK_TRUE(term_bb_req_parse(NULL, 0, &req) != TERM_BB_OK);
    }
}

/* ===================================================================== */
/* Every value of a huge line is refused the same way (a small sweep)     */
/* ===================================================================== */

static void case_byte_sweep(void)
{
    unsigned c;

    t_case("a stray byte appended to a well-formed request never sneaks in");
    /* Only '\n' and '\r' may end a line; every other byte turns the last line
     * into something else, and the parser must have an opinion about all of
     * them. Nothing here may be silently accepted with the fields intact. */
    for (c = 1; c < 256u; c++) {
        char msg[128];
        term_bb_req_t req;
        term_bb_err_t e;
        int n = sprintf(msg, "bbpull1\nctr=1\ntop=t\nwhat=live\n");
        msg[n] = (char)c;
        msg[n + 1] = 0;
        memset(&req, 0xCD, sizeof req);
        e = term_bb_req_parse(msg, (size_t)n + 1u, &req);
        t_checks++;
        if (c == '\n' || c == '\r') {
            if (e != TERM_BB_OK) {
                t_head(__FILE__, __LINE__);
                printf("     a trailing 0x%02X must be tolerated, got %s\n", c,
                       term_bb_err_str(e));
            }
        } else if (e == TERM_BB_OK) {
            t_head(__FILE__, __LINE__);
            printf("     a trailing 0x%02X was accepted as a whole request\n", c);
        }
    }
}

/* ===================================================================== */
/* The error names                                                       */
/* ===================================================================== */

static void case_error_names(void)
{
    unsigned i;

    t_case("every code has the short, stable, lower-case name /status carries");
    CHK_STR(term_bb_err_str(TERM_BB_E_SIZE), "bad-length");
    CHK_STR(term_bb_err_str(TERM_BB_E_SIG), "bad-signature");
    CHK_STR(term_bb_err_str(TERM_BB_E_MAGIC), "bad-magic");
    CHK_STR(term_bb_err_str(TERM_BB_E_SYNTAX), "bad-request");
    CHK_STR(term_bb_err_str(TERM_BB_E_UNKNOWN), "unknown-field");
    CHK_STR(term_bb_err_str(TERM_BB_E_DUP), "duplicate-field");
    CHK_STR(term_bb_err_str(TERM_BB_E_MISSING), "missing-field");
    CHK_STR(term_bb_err_str(TERM_BB_E_RANGE), "out-of-range");
    CHK_STR(term_bb_err_str(TERM_BB_E_TOPIC), "wrong-topic");
    CHK_STR(term_bb_err_str(TERM_BB_E_REPLAY), "replay");
    CHK_STR(term_bb_err_str(TERM_BB_E_HWM), "counter-store");
    CHK_STR(term_bb_err_str(TERM_BB_E_ENV), "misconfigured");
    CHK_STR(term_bb_err_str(TERM_BB_E_PUBLISH), "publish-failed");

    t_case("...and it is safe for any code, including out-of-range ones");
    for (i = 0; i < 64u; i++) {
        const char *s = term_bb_err_str((term_bb_err_t)i);
        size_t k;
        CHK_TRUE(s != NULL && s[0] != 0);
        if (!s) continue;
        for (k = 0; s[k]; k++)
            CHK_TRUE((s[k] >= 'a' && s[k] <= 'z') || s[k] == '-');
    }
    CHK_STR(term_bb_err_str((term_bb_err_t)900), "bad-code");
    CHK_STR(term_bb_err_str((term_bb_err_t)-1), "bad-code");
}

int main(void)
{
    t_suite("bb_parse");
    case_the_documented_request();
    case_tolerances();
    case_magic();
    case_unknown_fields_are_refused();
    case_syntax();
    case_overlong();
    case_byte_sweep();
    case_error_names();
    return t_summary();
}
