/*
 * test_pipe_ring.c — the producer's side of the byte ring: backpressure
 * instead of loss (§5, phase 4 Decision 1).
 *
 * Contracts under test (term_registry.h, "THE RING, FROM THE PRODUCER'S
 * SIDE"):
 *   "BACKPRESSURE, NOT LOSS. §5 gives the pipe different rules from JS/print
 *    ingest … Truncating a run mid-escape would corrupt the screen with no
 *    way back, so the pipe never drops. The protocol is: 1. ask for space,
 *    2. read at most that many bytes off the wire, 3. write them — all of
 *    them fit, guaranteed."
 *   "Step 3 cannot come up short because the term has ONE producer (SPSC):
 *    only this task adds to the ring and only the UI drain removes from it,
 *    so the space observed in step 1 is a lower bound that can only grow."
 *   "Missing [the lock] is TERM_ERR_TIMEOUT with *out_space / *out_written
 *    set to 0 and NOTHING CONSUMED … That is the difference from
 *    term_registry_feed, which drops and counts on the same event."
 *   "A SHORT WRITE … is also TERM_ERR_TIMEOUT, with *out_written telling the
 *    truth … The registry does not drop it and does not count it as a drop."
 *   "A cookie mismatch is TERM_ERR_BUSY"; "A DYING/ZOMBIE term answers
 *    TERM_ERR_DYING to both".
 *   §5: "drain は 1 フレームあたりのバイト予算制".
 *
 * The ring's SIZE is not fixed by any header, so nothing here assumes one:
 * every case asks the registry what it is offering and reasons from that.
 */
#include "pipe_util.h"

static p4_prod_t p4_pa, p4_pb;

/* Big enough to hold anything the ring can offer in one go; the assertions
 * below check the offer against this cap rather than trusting it. */
#define P4_BIG (256u * 1024u)
static uint8_t p4_buf[P4_BIG];

/* ===================================================================== */
/* Space is honest                                                       */
/* ===================================================================== */

static void case_space_is_honest(void)
{
    term_id_t id;
    long s0, s1, s2;
    size_t wrote = 0x5A5A;
    term_err_t rc = TERM_ERR_INVAL;

    t_case("a fresh piped term offers a positive, stable amount of space");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 80, 24);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, false), TERM_OK);

    s0 = p4_space(id, &p4_pa);
    CHK_TRUE(s0 > 0);
    CHK_TRUE((size_t)s0 <= P4_BIG);

    t_case("asking twice does not change it — a query consumes nothing");
    s1 = p4_space(id, &p4_pa);
    CHK_INT(s1, s0);
    s1 = p4_space(id, &p4_pa);
    CHK_INT(s1, s0);

    t_case("a write of EXACTLY the offered space is taken whole (step 3)");
    memset(p4_buf, 'x', (size_t)s0);
    wrote = p4_write(id, &p4_pa, p4_buf, (size_t)s0, &rc);
    CHK_INT(rc, TERM_OK);
    CHK_INT((int)wrote, (int)s0);
    CHK_INT((int)reg_info(id).bytes_in, (int)s0);
    CHK_INT((int)reg_info(id).bytes_dropped, 0);
    CHK_INT((int)reg_stats().prod_bytes, (int)s0);

    t_case("the ring is now full: it offers 0 and counts the stall");
    {
        term_registry_stats_t before = reg_stats();
        s2 = p4_space(id, &p4_pa);
        CHK_INT(s2, 0);
        CHK_TRUE(reg_stats().prod_stalls > before.prod_stalls);
    }

    t_case("a full ring takes NOTHING — no half-written escape sequence");
    wrote = p4_write(id, &p4_pa, "\x1b[31m", 5, &rc);
    CHK_INT((int)wrote, 0);
    CHK_TRUE(rc != TERM_OK);
    CHK_INT((int)reg_info(id).bytes_in, (int)s0);

    t_case("...and the refusal is NOT counted as a drop (that is feed's rule)");
    CHK_INT((int)reg_info(id).bytes_dropped, 0);
    CHK_INT((int)reg_stats().drops_full, 0);
    CHK_INT((int)reg_stats().prod_bytes, (int)s0);

    t_case("draining frees space again — the producer is not wedged");
    reg_frame();
    CHK_TRUE(p4_space(id, &p4_pa) > 0);
    {
        int guard;
        for (guard = 0; guard < 4096 && p4_space(id, &p4_pa) < s0; guard++)
            reg_frame();
        CHK_INT(p4_space(id, &p4_pa), s0);
    }

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

static void case_space_only_grows_for_the_sole_producer(void)
{
    term_id_t id;
    long s, prev;
    int i;
    term_err_t rc;

    t_case("space never shrinks by more than what this producer wrote (SPSC)");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 80, 24);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, false), TERM_OK);

    memset(p4_buf, 'y', 64);
    prev = p4_space(id, &p4_pa);
    CHK_TRUE(prev > 0);
    for (i = 0; i < 16 && prev >= 64; i++) {
        size_t w = p4_write(id, &p4_pa, p4_buf, 64, &rc);
        CHK_INT(rc, TERM_OK);
        CHK_INT((int)w, 64);
        s = p4_space(id, &p4_pa);
        /* No drain in between: the only thing that moved the tail is this
         * write, so the new space is exactly the old one minus 64. Nothing
         * else may consume from the ring while the producer holds it. */
        CHK_INT(s, prev - 64);
        prev = s;
    }
    CHK_TRUE(i > 0);

    t_case("a drain can only give space back, never take it away");
    prev = p4_space(id, &p4_pa);
    for (i = 0; i < 8; i++) {
        reg_frame();
        s = p4_space(id, &p4_pa);
        CHK_TRUE(s >= prev);
        prev = s;
    }

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* A short write leaves the remainder with the caller                     */
/* ===================================================================== */

static void case_short_write_keeps_the_remainder(void)
{
    term_id_t id;
    long s0;
    size_t wrote;
    term_err_t rc = TERM_ERR_INVAL;
    term_registry_stats_t before, after;

    t_case("offering more than the space takes what fits and says how much");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 80, 24);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, false), TERM_OK);

    s0 = p4_space(id, &p4_pa);
    REQUIRE(s0 > 16);
    CHK_TRUE((size_t)s0 + 32u <= P4_BIG);
    memset(p4_buf, 'z', (size_t)s0 + 32u);

    before = reg_stats();
    wrote = p4_write(id, &p4_pa, p4_buf, (size_t)s0 + 32u, &rc);
    after = reg_stats();
    CHK_INT(rc, TERM_ERR_TIMEOUT);
    CHK_INT((int)wrote, (int)s0);

    t_case("the remainder stays the producer's: not dropped, not counted");
    CHK_INT((int)reg_info(id).bytes_dropped, 0);
    CHK_INT((int)(after.drops_full - before.drops_full), 0);
    CHK_INT((int)(after.drops_lock - before.drops_lock), 0);
    CHK_INT((int)(after.prod_bytes - before.prod_bytes), (int)s0);

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

static void case_a_refused_lock_consumes_nothing(void)
{
    term_id_t id;
    size_t sp = 0x5A5A, wrote = 0x5A5A;
    term_registry_stats_t before, after;

    t_case("a lock it cannot get is TERM_ERR_TIMEOUT with space 0");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, false), TERM_OK);

    before = reg_stats();
    fp.refuse_bounded = true;
    CHK_INT(term_registry_producer_space(id, &p4_pa, &sp), TERM_ERR_TIMEOUT);
    CHK_INT((int)sp, 0);

    t_case("...and a write in the same state consumes NOTHING (not a drop)");
    CHK_INT(term_registry_producer_write(id, &p4_pa,
                                         (const uint8_t *)"AB", 2, &wrote),
            TERM_ERR_TIMEOUT);
    CHK_INT((int)wrote, 0);
    fp.refuse_bounded = false;
    after = reg_stats();
    CHK_INT((int)(after.drops_full - before.drops_full), 0);
    CHK_INT((int)reg_info(id).bytes_dropped, 0);
    CHK_INT((int)(after.prod_bytes - before.prod_bytes), 0);

    t_case("the producer re-offers its bytes and they appear exactly once");
    {
        term_err_t rc = TERM_ERR_INVAL;
        CHK_INT((int)p4_write(id, &p4_pa, "AB", 2, &rc), 2);
        CHK_INT(rc, TERM_OK);
    }
    reg_frame();
    CHK_STR(reg_snap_line0(id, OWNER_A), "AB");

    t_case("ingest never asks for an unbounded wait (§5, term_port.h)");
    {
        int forever = fp.forever_locks;
        (void)p4_space(id, &p4_pa);
        {
            term_err_t rc = TERM_ERR_INVAL;
            (void)p4_write(id, &p4_pa, "C", 1, &rc);
            CHK_INT(rc, TERM_OK);
        }
        CHK_INT(fp.forever_locks, forever);
        CHK_TRUE(fp.last_timeout_ms <= TERM_INGEST_TIMEOUT_MS_DEFAULT);
    }

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Who may touch the ring                                                */
/* ===================================================================== */

static void case_only_the_bound_cookie(void)
{
    term_id_t id;
    size_t sp = 0x5A5A, wrote = 0x5A5A;

    t_case("a foreign cookie can neither ask nor write (TERM_ERR_BUSY)");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    p4_prod_reset(&p4_pb, "B");
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, false), TERM_OK);

    CHK_INT(term_registry_producer_space(id, &p4_pb, &sp), TERM_ERR_BUSY);
    CHK_INT(term_registry_producer_write(id, &p4_pb,
                                         (const uint8_t *)"bad", 3, &wrote),
            TERM_ERR_BUSY);
    CHK_INT((int)reg_info(id).bytes_in, 0);

    t_case("an unpiped term has no producer to be, either");
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_TRUE(term_registry_producer_space(id, &p4_pa, &sp) != TERM_OK);
    CHK_TRUE(term_registry_producer_write(id, &p4_pa,
                                          (const uint8_t *)"bad", 3, &wrote)
             != TERM_OK);
    CHK_INT((int)reg_info(id).bytes_in, 0);

    t_case("a stale id is refused after the slot has moved on (I2/I3)");
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, false), TERM_OK);
    CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    reg_settle();
    CHK_TRUE(term_registry_producer_space(id, &p4_pa, &sp) != TERM_OK);
    CHK_TRUE(term_registry_producer_write(id, &p4_pa,
                                          (const uint8_t *)"bad", 3, &wrote)
             != TERM_OK);

    t_case("bad arguments are errors, not crashes (I5)");
    {
        term_id_t v = reg_new("v2", OWNER_A, TERM_VT, false, 40, 6);
        REQUIRE(v != TERM_ID_INVALID);
        CHK_INT(p4_pipe(v, OWNER_A, &p4_pa, false), TERM_OK);
        CHK_INT(term_registry_producer_space(v, &p4_pa, NULL), TERM_ERR_INVAL);
        CHK_INT(term_registry_producer_write(v, &p4_pa, NULL, 4, &wrote),
                TERM_ERR_INVAL);
        CHK_INT((int)reg_info(v).bytes_in, 0);
        CHK_INT(term_registry_producer_unbind(v, OWNER_A), TERM_OK);
        CHK_INT(term_registry_producer_ack(v), TERM_OK);
    }

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Bytes survive the trip                                                */
/* ===================================================================== */

static void case_bytes_round_trip_into_the_grid(void)
{
    term_id_t id;
    static const char payload[] =
        "\x1b[2J\x1b[1;1H" "hello\r\n" "\x1b[32m" "green\x1b[0m\r\n" "third";
    term_err_t rc = TERM_ERR_INVAL;

    t_case("a producer's bytes are parsed by the drain, VT rules and all");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, false), TERM_OK);

    CHK_INT((int)p4_write(id, &p4_pa, payload, sizeof payload - 1, &rc),
            (int)(sizeof payload - 1));
    CHK_INT(rc, TERM_OK);

    t_case("nothing is visible before the drain — the parser runs there (§5)");
    CHK_STR(reg_snap_line0(id, OWNER_A), "");

    t_case("after the drain the whole payload is on screen, in order");
    p4_drain_frames(8);
    /* Trailing blank rows are part of a snapshot (one line per row, §7.2), so
     * the comparison is against the head of it. */
    CHK_INT(strncmp(reg_snap(id, OWNER_A), "hello\ngreen\nthird\n", 18), 0);
    CHK_STR(reg_snap_line0(id, OWNER_A), "hello");

    t_case("a payload larger than one frame's budget arrives whole, in order");
    {
        term_registry_config_t c;
        term_id_t v;
        char expect[512];
        int i, guard;

        memset(&c, 0, sizeof c);
        c.drain_budget_bytes = 8;      /* §5: a byte budget per frame     */
        CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
        CHK_INT(term_registry_producer_ack(id), TERM_OK);
        CHK_INT(reg_shutdown(), 0);
        CHK_TRUE(reg_boot_cfg(&c));
        p4_prod_reset(&p4_pa, "A");
        v = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
        REQUIRE(v != TERM_ID_INVALID);
        CHK_INT(p4_pipe(v, OWNER_A, &p4_pa, false), TERM_OK);

        for (i = 0; i < 30; i++) expect[i] = (char)('a' + i % 26);
        expect[30] = 0;
        CHK_INT((int)p4_write(v, &p4_pa, expect, 30, &rc), 30);
        CHK_INT(rc, TERM_OK);
        for (guard = 0; guard < 64 &&
             strlen(reg_snap_line0(v, OWNER_A)) < 30; guard++)
            reg_frame();
        CHK_STR(reg_snap_line0(v, OWNER_A), expect);
        CHK_TRUE(guard > 1);           /* it really took several frames   */

        CHK_INT(term_registry_producer_unbind(v, OWNER_A), TERM_OK);
        CHK_INT(term_registry_producer_ack(v), TERM_OK);
        CHK_INT(reg_shutdown(), 0);
    }
}

static void case_bytes_round_trip_into_scrollback(void)
{
    term_id_t id;
    char line[64];
    char out[4096];
    term_read_result_t res;
    term_err_t rc = TERM_ERR_INVAL;
    int i, sent = 0;

    t_case("lines pushed off the top reach scrollback with their text intact");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 4);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, false), TERM_OK);

    for (i = 0; i < 12; i++) {
        int n = sprintf(line, "<%02d>\r\n", i);
        long sp = p4_space(id, &p4_pa);
        if (sp < n) { p4_drain_frames(4); sp = p4_space(id, &p4_pa); }
        CHK_TRUE(sp >= n);
        CHK_INT((int)p4_write(id, &p4_pa, line, (size_t)n, &rc), n);
        CHK_INT(rc, TERM_OK);
        sent++;
    }
    CHK_INT(sent, 12);
    p4_drain_frames(16);

    memset(out, 0, sizeof out);
    memset(&res, 0, sizeof res);
    CHK_INT(term_registry_read(id, OWNER_A, 0, 4, out, sizeof out, &res),
            TERM_OK);
    CHK_INT(res.lines, 4);
    CHK_STR(out, "<00>\n<01>\n<02>\n<03>");
    CHK_TRUE(!res.truncated);

    t_case("the newest lines are on screen and the counters agree");
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "<11>") != NULL);
    CHK_INT((int)reg_info(id).bytes_in, (int)reg_stats().prod_bytes);
    CHK_INT((int)reg_info(id).bytes_dropped, 0);

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

/* A ring that fills mid-escape and is drained afterwards must still parse the
 * sequence: the protocol keeps the escape whole by never taking a byte the
 * producer did not get room for. */
static void case_an_escape_never_gets_chopped(void)
{
    term_id_t id;
    long s0;
    size_t wrote;
    term_err_t rc = TERM_ERR_INVAL;
    static const char tail[] = "\x1b[7mREV\x1b[0m";

    t_case("a run that does not fit is re-offered whole and parses correctly");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, false), TERM_OK);

    /* Fill the ring to within 4 bytes of full, then offer an 11-byte escape
     * run: the write must take at most what fits and the rest stays ours. */
    s0 = p4_space(id, &p4_pa);
    REQUIRE(s0 > 8);
    memset(p4_buf, '.', (size_t)s0 - 4);
    CHK_INT((int)p4_write(id, &p4_pa, p4_buf, (size_t)s0 - 4, &rc),
            (int)(s0 - 4));
    CHK_INT(rc, TERM_OK);
    CHK_INT(p4_space(id, &p4_pa), 4);

    /* The protocol says: ask first, then offer at most that. A producer that
     * ignores the answer gets a truthful short write and keeps its tail. */
    wrote = p4_write(id, &p4_pa, tail, sizeof tail - 1, &rc);
    CHK_INT(rc, TERM_ERR_TIMEOUT);
    CHK_INT((int)wrote, 4);

    t_case("...and after a drain the remainder completes the sequence");
    {
        int guard;
        for (guard = 0; guard < 4096 &&
             (size_t)p4_space(id, &p4_pa) < (sizeof tail - 1 - wrote); guard++)
            reg_frame();
        CHK_INT((int)p4_write(id, &p4_pa, tail + wrote,
                              sizeof tail - 1 - wrote, &rc),
                (int)(sizeof tail - 1 - wrote));
        CHK_INT(rc, TERM_OK);
        p4_drain_frames(4096 / 64);
    }
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "REV") != NULL);

    t_case("...and the escape bytes themselves never became text");
    /* A torn "\x1b[7m" would have re-synchronised in GROUND and printed the
     * leftover parameter bytes. "7m" and "0m" must not be on screen; whether
     * the reverse attribute actually landed in the cells is asserted through
     * the visitor in test_cells_attr.c. */
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "7mREV") == NULL);
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "REV0m") == NULL);
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "\x1b") == NULL);

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Nothing is allocated on this path (I4)                                */
/* ===================================================================== */

static void case_the_ring_path_allocates_nothing(void)
{
    term_id_t id;
    term_err_t rc = TERM_ERR_INVAL;
    int i;

    t_case("space/write/drain allocate nothing after create (I4, §4.2)");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    id = reg_new("v", OWNER_A, TERM_VT, false, 80, 24);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, false), TERM_OK);
    CHK_INT(p4_show(id, OWNER_A, 0, 0, true), TERM_OK);

    fp.alloc_frozen = true;
    for (i = 0; i < 64; i++) {
        long sp = p4_space(id, &p4_pa);
        if (sp > 32) {
            memset(p4_buf, 'q', 32);
            CHK_INT((int)p4_write(id, &p4_pa, p4_buf, 32, &rc), 32);
        }
        reg_frame();
    }
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    fp.alloc_frozen = false;
    CHK_INT(fp.alloc_violations, 0);

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("pipe_ring");
    case_space_is_honest();
    case_space_only_grows_for_the_sole_producer();
    case_short_write_keeps_the_remainder();
    case_a_refused_lock_consumes_nothing();
    case_only_the_bound_cookie();
    case_bytes_round_trip_into_the_grid();
    case_bytes_round_trip_into_scrollback();
    case_an_escape_never_gets_chopped();
    case_the_ring_path_allocates_nothing();
    fp_reclaim_all();
    return t_summary();
}
