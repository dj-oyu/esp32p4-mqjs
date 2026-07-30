/*
 * test_lp_strip.c — P4: the region only ever holds stripped text.
 *
 * "term_lp_ring_append() runs term_lp_strip() on every byte it accepts; there
 * is no API that writes raw bytes." And the rule itself, quoted from the
 * term_lp_strip() comment, is contract rather than detail:
 *
 *   - "it drops every ESC-introduced sequence (CSI/OSC/DCS/SS2/SS3/
 *     single-character), drops the remaining C0 controls except '\n' and
 *     '\t', and passes everything >= 0x20 through byte for byte (so UTF-8 is
 *     preserved untouched)";
 *   - CSI is "ESC [ ... final byte 0x40-0x7E"; OSC/DCS/APC/PM are "terminated
 *     by BEL or ESC \ or the end of the input"; nF is "ESC + an intermediate
 *     0x20-0x2F, then bytes up to a final 0x30-0x7E — this is what makes
 *     `ESC ( B` vanish completely instead of leaving a stray 'B'";
 *   - "An unterminated escape at the end of the input consumes the rest of the
 *     input, which is the safe direction";
 *   - "When the result would exceed `dst_cap` the copy stops at the last
 *     complete UTF-8 sequence that fits and *out_trunc ... is set true — never
 *     mid-codepoint, so a pulled log is always valid UTF-8."
 *
 * "Dropping ALL escapes rather than only SGR is deliberate and is a security
 * property, not tidiness" — so the suite finishes with a fuzz whose alphabet
 * is escape-heavy and asserts the structural outcome: no record in the region
 * ever holds an ESC, a C0 other than \n/\t, or a DEL.
 */
#include "lp_util.h"

static lp_recs_t bagA;

#define STRIP_CAP 2048u

/* Strip `len` bytes into a rotating NUL-terminated buffer. */
static const char *strip_n(const char *src, size_t len, size_t cap, bool *trunc)
{
    static char buf[4][STRIP_CAP];
    static int which = 0;
    size_t n;
    bool tr = false;
    which = (which + 1) & 3;
    if (cap > STRIP_CAP - 1u) cap = STRIP_CAP - 1u;
    memset(buf[which], 0x7e, STRIP_CAP);
    n = term_lp_strip(buf[which], cap, src, len, &tr);
    if (n > cap) {
        printf("     strip wrote %lu bytes into a %lu-byte buffer!\n",
               (unsigned long)n, (unsigned long)cap);
        n = cap;
    }
    buf[which][n] = 0;
    if (trunc) *trunc = tr;
    return buf[which];
}

static const char *strip(const char *src)
{
    return strip_n(src, strlen(src), STRIP_CAP - 1u, NULL);
}

/* ===================================================================== */
/* The rule                                                              */
/* ===================================================================== */

static void case_escape_sequences_vanish_whole(void)
{
    t_case("plain text and UTF-8 pass through byte for byte");
    CHK_STR(strip("plain"), "plain");
    CHK_STR(strip(""), "");
    CHK_STR(strip("日本語 ok"), "日本語 ok");
    CHK_INT(term_lp_strip(NULL, STRIP_CAP, "日本語", 9, NULL), 9);

    /* "Returns the number of bytes written (or that would be)" — with dst NULL
     * the count is still what WOULD be written, i.e. still capped by dst_cap.
     * A caller that wants the true stripped length must pass a cap that can
     * hold it; dst_cap 0 measures 0. */
    t_case("measure-only (dst NULL) is capped exactly like the copy");
    CHK_INT(term_lp_strip(NULL, 99, "hello", 5, NULL), 5);
    CHK_INT(term_lp_strip(NULL, 3, "hello", 5, NULL), 3);
    CHK_INT(term_lp_strip(NULL, 0, "hello", 5, NULL), 0);
    {
        bool tr = false;
        CHK_INT(term_lp_strip(NULL, 3, "hello", 5, &tr), 3);
        CHK_TRUE(tr);
        CHK_INT(term_lp_strip(NULL, 99, "hello", 5, &tr), 5);
        CHK_TRUE(!tr);
    }

    t_case("SGR: the §12 case the black box was born from");
    CHK_STR(strip("a\x1b[31mb\x1b[0mc"), "abc");
    CHK_STR(strip("\x1b[1;2;3;4;5m"), "");
    CHK_STR(strip("\x1b[38;2;255;0;0mred\x1b[m"), "red");

    t_case("CSI with private/intermediate bytes, and a non-'m' final");
    CHK_STR(strip("x\x1b[?25hy"), "xy");
    CHK_STR(strip("\x1b[>4;2m"), "");
    CHK_STR(strip("\x1b[2J\x1b[H pwned"), " pwned");
    CHK_STR(strip("a\x1b[10;20Hb"), "ab");
    CHK_STR(strip("a\x1b[!p" "b"), "ab");

    t_case("OSC, terminated by BEL, by ST, or by the end of the input");
    CHK_STR(strip("\x1b]0;title\x07x"), "x");
    CHK_STR(strip("\x1b]0;title\x1b\\x"), "x");
    CHK_STR(strip("a\x1b]0;never-terminated"), "a");
    CHK_STR(strip("a\x1b]8;;http://example.com\x07link\x1b]8;;\x07z"), "alinkz");

    t_case("DCS, APC and PM go the same way");
    CHK_STR(strip("\x1bP0;1|foo\x1b\\bar"), "bar");
    CHK_STR(strip("a\x1b_app payload\x1b\\z"), "az");
    CHK_STR(strip("a\x1b^privacy\x1b\\z"), "az");

    t_case("nF sequences vanish whole: no stray 'B' from ESC ( B");
    CHK_STR(strip("\x1b(B"), "");
    CHK_STR(strip("A\x1b(Bz"), "Az");
    CHK_STR(strip("\x1b)0abc"), "abc");
    CHK_STR(strip("A\x1b#8z"), "Az");

    t_case("ESC + any other single byte, including SS2/SS3");
    CHK_STR(strip("\x1b" "7x"), "x");
    CHK_STR(strip("\x1bMx"), "x");
    CHK_STR(strip("\x1b=z"), "z");
    CHK_STR(strip("a\x1bNXz"), "aXz");   /* SS2: only the ESC N is consumed */
    CHK_STR(strip("a\x1bOXz"), "aXz");   /* SS3                            */

    t_case("an unterminated escape at the end eats the rest, never leaks it");
    CHK_STR(strip("abc\x1b["), "abc");
    CHK_STR(strip("abc\x1b[3"), "abc");
    CHK_STR(strip("abc\x1b[31;4"), "abc");
    CHK_STR(strip("x\x1b"), "x");
    CHK_STR(strip("x\x1bP incomplete dcs"), "x");
}

static void case_c0_controls_except_nl_and_tab(void)
{
    static const char raw[] = "a\rb\nc\td\x07" "e\x00" "f\x1fg\x7fh";

    t_case("\\r, BEL, NUL, other C0s and DEL are dropped; \\n and \\t are kept");
    CHK_STR(strip_n(raw, sizeof raw - 1u, STRIP_CAP - 1u, NULL), "ab\nc\tdefgh");
    CHK_STR(strip("\r\r\r"), "");
    CHK_STR(strip("line1\r\nline2\r\n"), "line1\nline2\n");
    CHK_INT(term_lp_strip(NULL, STRIP_CAP, "a\rb", 3, NULL), 2);

    t_case("0x20 is not a control: the space survives");
    CHK_STR(strip(" a b "), " a b ");
}

/* ===================================================================== */
/* Truncation: at a codepoint boundary, or not at all                    */
/* ===================================================================== */

static void case_truncation_lands_on_a_codepoint_boundary(void)
{
    char src[900];
    const char *out;
    bool trunc;
    size_t cap, full, i;

    t_case("three-byte codepoints: a 512-byte cap stops at 510");
    for (i = 0; i < 200; i++) memcpy(src + i * 3u, "\xE3\x81\x82", 3);  /* あ */
    src[600] = 0;
    full = term_lp_strip(NULL, 4096, src, 600, &trunc);
    CHK_INT(full, 600);
    CHK_TRUE(!trunc);
    out = strip_n(src, 600, TERM_LP_REC_MAX, &trunc);
    CHK_INT(strlen(out), 510);
    CHK_TRUE(trunc);
    CHK_TRUE(lp_utf8_valid(out, strlen(out)));
    CHK_TRUE(memcmp(out, src, 510) == 0);

    t_case("a cap of 9, 10 or 11 all yield the same three codepoints");
    for (cap = 9; cap <= 11; cap++) {
        out = strip_n(src, 600, cap, &trunc);
        CHK_INT(strlen(out), 9);
        CHK_TRUE(trunc);
        CHK_TRUE(lp_utf8_valid(out, strlen(out)));
    }
    out = strip_n(src, 600, 12, &trunc);
    CHK_INT(strlen(out), 12);
    CHK_TRUE(trunc);

    t_case("four-byte codepoints back off the same way");
    memcpy(src, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80", 8);   /* two emoji */
    out = strip_n(src, 8, 6, &trunc);
    CHK_INT(strlen(out), 4);
    CHK_TRUE(trunc);
    CHK_TRUE(lp_utf8_valid(out, 4));
    out = strip_n(src, 8, 8, &trunc);
    CHK_INT(strlen(out), 8);
    CHK_TRUE(!trunc);
    out = strip_n(src, 8, 3, &trunc);
    CHK_INT(strlen(out), 0);
    CHK_TRUE(trunc);

    t_case("every cap from 0 up gives a valid-UTF-8 prefix of the whole answer");
    strcpy(src, "ab\xE3\x81\x82" "c\xF0\x9F\x98\x80" "de\xC3\xA9" "f");
    full = strlen(src);
    for (cap = 0; cap <= full + 2u; cap++) {
        size_t n;
        out = strip_n(src, full, cap, &trunc);
        n = strlen(out);
        t_checks++;
        if (n > cap || memcmp(out, src, n) != 0 || !lp_utf8_valid(out, n) ||
            trunc != (n < full)) {
            t_head(__FILE__, __LINE__);
            printf("     cap %lu: got %lu bytes (trunc=%d) of %lu, valid=%d\n",
                   (unsigned long)cap, (unsigned long)n, (int)trunc,
                   (unsigned long)full, lp_utf8_valid(out, n));
        }
    }

    t_case("the cap applies to the STRIPPED length, not the raw one");
    /* 300 payload bytes wrapped in 300 bytes of SGR: fits, untruncated. */
    {
        char big[1024];
        size_t o = 0;
        for (i = 0; i < 60; i++) {
            memcpy(big + o, "\x1b[31m", 5); o += 5;
            memcpy(big + o, "abcde", 5); o += 5;
        }
        CHK_INT(o, 600);
        out = strip_n(big, o, TERM_LP_REC_MAX, &trunc);
        CHK_INT(strlen(out), 300);
        CHK_TRUE(!trunc);
    }
}

/* ===================================================================== */
/* The same rule, on the way into the region                             */
/* ===================================================================== */

static void case_append_stores_only_stripped_text(void)
{
    lpreg_t g;
    static const char raw[] = "\x1b[1;32m[boot]\x1b[0m wifi: got ip\r";

    t_case("append strips: the region never holds the escapes it was handed");
    REQUIRE(lp_fresh(&g, 1));
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system", raw,
                                 sizeof raw - 1u, 42));
    CHK_STR(lp_text_at(&g.r, TERM_LP_PART_SYS, 0), "[boot] wifi: got ip");
    lp_collect(&bagA, &g.r, TERM_LP_PART_SYS, g.base, g.bytes);
    CHK_INT(bagA.n, 1);
    CHK_INT(bagA.len[0], 19);
    CHK_INT(bagA.with_esc, 0);
    CHK_INT(bagA.flags[0], 0);
    CHK_INT(g.r.hdr.truncated, 0);

    t_case("an embedded newline stays inside the one record (manifest d11)");
    CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system",
                                 "two\nlines\r\n", 11, 43));
    CHK_STR(lp_text_at(&g.r, TERM_LP_PART_SYS, 1), "two\nlines\n");
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 2);

    t_case("a line of nothing but escapes is refused, not stored empty");
    CHK_TRUE(!term_lp_ring_append(&g.r, TERM_LP_CLASS_SYS, "system",
                                  "\x1b[2J\x1b[H\x1b]0;t\x07", 13, 44));
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].records, 2);
    CHK_INT(g.r.hdr.refused, 1);

    t_case("an over-long ASCII line is stored at the cap with F_TRUNC");
    {
        char big[1024];
        memset(big, 'x', sizeof big);
        CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, "chatty", big,
                                     sizeof big, 45));
        lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
        CHK_INT(bagA.n, 1);
        CHK_INT(bagA.len[0], TERM_LP_REC_MAX);
        CHK_INT(bagA.flags[0], TERM_LP_F_TRUNC);
        CHK_INT(TERM_LP_CLASS_OF(bagA.cls[0]), TERM_LP_CLASS_APP);
        CHK_INT(g.r.hdr.truncated, 1);
    }

    t_case("an over-long UTF-8 line backs off to 510 bytes, still valid UTF-8");
    {
        char big[900];
        size_t i;
        for (i = 0; i < 200; i++) memcpy(big + i * 3u, "\xE3\x81\x82", 3);
        CHK_TRUE(term_lp_ring_append(&g.r, TERM_LP_CLASS_APP, "chatty", big,
                                     600, 46));
        lp_collect(&bagA, &g.r, TERM_LP_PART_APP, g.base, g.bytes);
        CHK_INT(bagA.n, 2);
        CHK_INT(bagA.len[1], 510);
        CHK_INT(bagA.flags[1], TERM_LP_F_TRUNC);
        CHK_INT(g.r.hdr.truncated, 2);
        {
            const char *txt = lp_text_at(&g.r, TERM_LP_PART_APP, 1);
            CHK_INT(strlen(txt), 510);
            CHK_TRUE(lp_utf8_valid(txt, 510));
            CHK_TRUE(memcmp(txt, big, 510) == 0);
        }
    }
    lp_check_layout(g.base, &g.r.hdr);
    CHK_TRUE(lp_guards_intact(&g));
    lp_free(&g);
}

/* ===================================================================== */
/* Escape-heavy fuzz: the structural claim, over 3000 records            */
/* ===================================================================== */

static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static void case_no_record_ever_holds_a_control_byte(void)
{
    lpreg_t g;
    uint32_t seed = 0x5EEDBEEFu;
    unsigned i, appended = 0, refused = 0;
    /* Escape-heavy on purpose, plus UTF-8 lead/continuation bytes and the C0
     * controls the rule keeps or drops. */
    static const char alpha[] = {
        0x1b, 0x1b, '[', ']', 'P', '_', '^', '(', ')', 'm', 'H', 'J', ';',
        '0', '1', '2', '3', 'a', 'b', 'c', ' ', 0x07, '\r', '\n', '\t',
        0x00, 0x1f, 0x7f, (char)0xE3, (char)0x81, (char)0x82, (char)0xF0,
        (char)0x9F, (char)0x98, (char)0x80, (char)0xC3, (char)0xA9, '\\'
    };
    char buf[700];

    t_case("3000 escape-soup lines: nothing but text >= 0x20, \\n and \\t lands");
    REQUIRE(lp_fresh(&g, 1));
    for (i = 0; i < 3000u; i++) {
        size_t n = 1u + (xs32(&seed) % 640u), k;
        term_lp_class_t cls = (xs32(&seed) & 1u) ? TERM_LP_CLASS_APP
                                                : TERM_LP_CLASS_SYS;
        for (k = 0; k < n; k++)
            buf[k] = alpha[xs32(&seed) % (sizeof alpha)];
        if (term_lp_ring_append(&g.r, cls, "fuzz", buf, n, i)) appended++;
        else refused++;
    }
    CHK_TRUE(appended > 2000u);
    CHK_INT(appended + refused, 3000);
    CHK_INT(g.r.hdr.refused, refused);
    CHK_INT(g.r.hdr.part[TERM_LP_PART_SYS].appended +
            g.r.hdr.part[TERM_LP_PART_APP].appended, appended);

    {
        term_lp_iter_t it;
        term_lp_record_t rec;
        unsigned seen = 0, bad_byte = 0, bad_len = 0;
        term_lp_iter_begin(&it, &g.r, -1);
        while (term_lp_iter_next(&it, &rec)) {
            uint16_t j;
            seen++;
            if (rec.len == 0 || rec.len > TERM_LP_REC_MAX) bad_len++;
            for (j = 0; j < rec.len; j++) {
                unsigned char c = rec.text[j];
                if (c >= 0x20u && c != 0x7fu) continue;
                if (c == '\n' || c == '\t') continue;
                if (bad_byte++ == 0)
                    printf("     record %u byte %u is 0x%02X\n", seen, j, c);
            }
        }
        CHK_INT(bad_byte, 0);
        CHK_INT(bad_len, 0);
        CHK_TRUE(seen > 0);
        CHK_INT(seen, g.r.hdr.part[TERM_LP_PART_SYS].records +
                      g.r.hdr.part[TERM_LP_PART_APP].records);
    }

    t_case("...and the region is still structurally sound afterwards");
    lp_check_layout(g.base, &g.r.hdr);
    CHK_TRUE(lp_guards_intact(&g));
    {
        term_lp_ring_t r2;
        term_lp_check_t chk;
        memset(&r2, 0, sizeof r2);
        CHK_TRUE(term_lp_ring_open(&r2, g.base, g.bytes, true, &chk));
        CHK_TRUE(chk.ok);
    }
    lp_free(&g);
}

int main(void)
{
    t_suite("lp_strip");
    case_escape_sequences_vanish_whole();
    case_c0_controls_except_nl_and_tab();
    case_truncation_lands_on_a_codepoint_boundary();
    case_append_stores_only_stripped_text();
    case_no_record_ever_holds_a_control_byte();
    return t_summary();
}
