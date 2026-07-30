/*
 * test_pipe_stats.c — the eight phase-4 counters move on their own event and
 * on nothing else.
 *
 * term_registry.h names each one:
 *   pipes            "producers bound"
 *   pipe_busy        "pipes refused because one was bound; the old producer
 *                     was asked to detach"
 *   prod_bytes       "bytes taken from producers (never lossy)"
 *   prod_stalls      "space queries answered 0 = the ring is full and the ssh
 *                     window is doing the work"
 *   replies_piped    "replies routed into a producer's channel"
 *   replies_js       "replies handed to a term.onReply sink"
 *   replies_dropped  "replies nobody was listening for"
 *   caret_pushes     "caret events handed to the sink"
 *
 * A counter that moves on the wrong event is worse than one that never moves:
 * it is a probe that lies. So every case here brackets ONE action and asserts
 * the whole eight-wide delta vector, not just the counter it is about.
 */
#include "pipe_util.h"

static p4_prod_t  p4_pa, p4_pb;
static p4_js_t    p4_js;
static p4_caret_t p4_car;

/* ===================================================================== */
/* The delta vector                                                      */
/* ===================================================================== */

enum {
    D_PIPES = 0, D_BUSY, D_BYTES, D_STALLS,
    D_R_PIPED, D_R_JS, D_R_DROP, D_CARET, D_N
};

static const char *p4_dname[D_N] = {
    "pipes", "pipe_busy", "prod_bytes", "prod_stalls",
    "replies_piped", "replies_js", "replies_dropped", "caret_pushes"
};

typedef struct { long long v[D_N]; } p4_vec_t;

static p4_vec_t p4_snap(void)
{
    term_registry_stats_t s = reg_stats();
    p4_vec_t o;
    o.v[D_PIPES]   = s.pipes;
    o.v[D_BUSY]    = s.pipe_busy;
    o.v[D_BYTES]   = (long long)s.prod_bytes;
    o.v[D_STALLS]  = s.prod_stalls;
    o.v[D_R_PIPED] = s.replies_piped;
    o.v[D_R_JS]    = s.replies_js;
    o.v[D_R_DROP]  = s.replies_dropped;
    o.v[D_CARET]   = s.caret_pushes;
    return o;
}

/* -1 in `want` means "not asserted here". Everything else is exact. */
static void p4_expect(const p4_vec_t *before, const long long *want)
{
    p4_vec_t after = p4_snap();
    int i;
    for (i = 0; i < D_N; i++) {
        long long got = after.v[i] - before->v[i];
        if (want[i] < 0) continue;
        t_checks++;
        if (got != want[i]) {
            t_head(__FILE__, __LINE__);
            printf("     counter : %s\n     expected: %+lld\n     actual  : %+lld\n",
                   p4_dname[i], want[i], got);
        }
    }
}

#define P4_NONE  { 0, 0, 0, 0, 0, 0, 0, 0 }

static void p4_feed(term_id_t id, const char *s)
{
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)s, strlen(s), NULL),
            TERM_OK);
}

/* ===================================================================== */
/* A fresh registry starts at zero                                       */
/* ===================================================================== */

static void case_a_fresh_table_is_at_zero(void)
{
    int i;
    p4_vec_t z;

    t_case("all eight counters start at 0 and belong to this table's lifetime");
    CHK_TRUE(reg_boot());
    z = p4_snap();
    for (i = 0; i < D_N; i++) CHK_INT((int)z.v[i], 0);

    /* Move a few, then rebuild the table: the new one must be at zero again,
     * or a probe would report the previous boot's traffic. */
    {
        term_id_t id = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
        REQUIRE(id != TERM_ID_INVALID);
        CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);
        CHK_TRUE(reg_stats().pipes > 0);
        CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
        CHK_INT(term_registry_producer_ack(id), TERM_OK);
        CHK_INT(reg_shutdown(), 0);
    }
    CHK_TRUE(reg_boot());
    z = p4_snap();
    for (i = 0; i < D_N; i++) CHK_INT((int)z.v[i], 0);
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* One action at a time                                                  */
/* ===================================================================== */

static void case_each_counter_has_exactly_one_cause(void)
{
    term_id_t id, lg;
    p4_vec_t b;
    long sp;
    term_err_t rc = TERM_ERR_INVAL;

    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    p4_prod_reset(&p4_pb, "B");
    p4_js_reset(&p4_js);
    p4_caret_install(&p4_car);
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 8);
    lg = reg_new("l", OWNER_A, TERM_LOG, false, 40, 8);
    REQUIRE(id != TERM_ID_INVALID && lg != TERM_ID_INVALID);

    t_case("creating and showing a term moves none of the eight");
    {
        static const long long want[D_N] = P4_NONE;
        b = p4_snap();
        CHK_INT(p4_show(id, OWNER_A, 0, 0, false), TERM_OK);
        reg_frame();
        p4_expect(&b, want);
    }

    t_case("feeding an unpiped, hidden term moves none of the eight");
    {
        static const long long want[D_N] = P4_NONE;
        b = p4_snap();
        p4_feed(id, "hello");
        reg_frame();
        p4_expect(&b, want);
    }

    t_case("a pipe refused for MODE moves nothing — not even pipe_busy");
    {
        static const long long want[D_N] = P4_NONE;
        b = p4_snap();
        CHK_INT(p4_pipe(lg, OWNER_A, &p4_pa, true), TERM_ERR_MODE);
        p4_expect(&b, want);
    }

    t_case("a pipe refused by the owner gate moves nothing either");
    {
        static const long long want[D_N] = P4_NONE;
        b = p4_snap();
        CHK_INT(p4_pipe(id, OWNER_B, &p4_pa, true), TERM_ERR_NOT_OWNER);
        p4_expect(&b, want);
    }

    t_case("binding a producer moves pipes, and only pipes");
    {
        static const long long want[D_N] = { 1, 0, 0, 0, 0, 0, 0, 0 };
        b = p4_snap();
        CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);
        p4_expect(&b, want);
    }

    t_case("a space query that answers >0 moves nothing");
    {
        static const long long want[D_N] = P4_NONE;
        b = p4_snap();
        sp = p4_space(id, &p4_pa);
        CHK_TRUE(sp > 0);
        p4_expect(&b, want);
    }

    t_case("a producer write moves prod_bytes by exactly what it took");
    {
        static const long long want[D_N] = { 0, 0, 5, 0, 0, 0, 0, 0 };
        b = p4_snap();
        CHK_INT((int)p4_write(id, &p4_pa, "abcde", 5, &rc), 5);
        CHK_INT(rc, TERM_OK);
        p4_expect(&b, want);
    }

    t_case("a rejected write by a foreign cookie moves nothing");
    {
        static const long long want[D_N] = P4_NONE;
        b = p4_snap();
        CHK_INT((int)p4_write(id, &p4_pb, "zzz", 3, &rc), 0);
        CHK_INT(rc, TERM_ERR_BUSY);
        p4_expect(&b, want);
    }

    t_case("a space query answering 0 moves prod_stalls, and only that");
    {
        static const long long want[D_N] = { 0, 0, -1, 1, 0, 0, 0, 0 };
        long remaining;
        /* Fill the ring first — that part is a write, so it is bracketed
         * separately from the query being measured. */
        remaining = p4_space(id, &p4_pa);
        CHK_TRUE(remaining > 0);
        {
            static uint8_t fill[256 * 1024];
            memset(fill, 'f', (size_t)remaining);
            CHK_INT((int)p4_write(id, &p4_pa, fill, (size_t)remaining, &rc),
                    (int)remaining);
        }
        b = p4_snap();
        CHK_INT(p4_space(id, &p4_pa), 0);
        p4_expect(&b, want);
    }

    t_case("...and a second 0-answer counts a second stall");
    {
        static const long long want[D_N] = { 0, 0, 0, 2, 0, 0, 0, 0 };
        b = p4_snap();
        CHK_INT(p4_space(id, &p4_pa), 0);
        CHK_INT(p4_space(id, &p4_pa), 0);
        p4_expect(&b, want);
    }

    t_case("draining a full ring of plain text moves none of the eight");
    {
        static const long long want[D_N] = P4_NONE;
        int i;
        b = p4_snap();
        for (i = 0; i < 64; i++) reg_frame();
        p4_expect(&b, want);
    }

    t_case("a re-pipe attempt moves pipe_busy and NOT pipes");
    {
        static const long long want[D_N] = { 0, 1, 0, 0, 0, 0, 0, 0 };
        b = p4_snap();
        CHK_INT(p4_pipe(id, OWNER_A, &p4_pb, true), TERM_ERR_BUSY);
        p4_expect(&b, want);
    }

    t_case("a reply into a producer's channel moves replies_piped only");
    {
        static const long long want[D_N] = { 0, 0, -1, -1, 1, 0, 0, 0 };
        b = p4_snap();
        CHK_INT((int)p4_write(id, &p4_pa, "\x1b[5n", 4, &rc), 4);
        reg_frame();
        CHK_INT(p4_pa.replies.n, 1);
        p4_expect(&b, want);
    }

    t_case("unbind and ack move none of the eight");
    {
        static const long long want[D_N] = P4_NONE;
        b = p4_snap();
        CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
        CHK_INT(term_registry_producer_ack(id), TERM_OK);
        reg_frame();
        p4_expect(&b, want);
    }

    t_case("a reply to a JS sink moves replies_js only");
    {
        static const long long want[D_N] = { 0, 0, 0, 0, 0, 1, 0, 0 };
        CHK_INT(term_registry_set_reply(id, OWNER_A, p4_js_sink, &p4_js),
                TERM_OK);
        b = p4_snap();
        p4_feed(id, "\x1b[5n");
        reg_frame();
        CHK_INT(p4_js.got.n, 1);
        p4_expect(&b, want);
    }

    t_case("installing or removing a sink moves nothing by itself");
    {
        static const long long want[D_N] = P4_NONE;
        b = p4_snap();
        CHK_INT(term_registry_set_reply(id, OWNER_A, NULL, NULL), TERM_OK);
        CHK_INT(term_registry_set_reply(id, OWNER_A, p4_js_sink, &p4_js),
                TERM_OK);
        CHK_INT(term_registry_set_reply(id, OWNER_A, NULL, NULL), TERM_OK);
        reg_frame();
        p4_expect(&b, want);
    }

    t_case("a reply nobody wants moves replies_dropped only");
    {
        static const long long want[D_N] = { 0, 0, 0, 0, 0, 0, 3, 0 };
        b = p4_snap();
        p4_feed(id, "\x1b[6n\x1b[5n\x1b[c");
        reg_frame();
        p4_expect(&b, want);
    }

    t_case("a caret push on a visible term moves caret_pushes only");
    {
        static const long long want[D_N] = { 0, 0, 0, 0, 0, 0, 0, 1 };
        CHK_INT(p4_show(id, OWNER_A, 0, 0, true), TERM_OK);
        reg_frame();                       /* the show's own re-arm        */
        p4_caret_reset(&p4_car);
        b = p4_snap();
        p4_feed(id, "\x1b[8;9H");
        reg_frame();
        CHK_INT(p4_car.n, 1);
        p4_expect(&b, want);
    }

    t_case("a frame with nothing to say moves none of the eight");
    {
        static const long long want[D_N] = P4_NONE;
        b = p4_snap();
        reg_frame();
        reg_frame();
        p4_expect(&b, want);
    }

    t_case("closing and reaping the term moves none of the eight");
    {
        static const long long want[D_N] = P4_NONE;
        b = p4_snap();
        CHK_INT(term_registry_close(id, OWNER_A), TERM_OK);
        reg_settle();
        p4_expect(&b, want);
    }

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The old counters and the new ones do not cross-talk                    */
/* ===================================================================== */

static void case_feed_drops_are_not_producer_stalls(void)
{
    term_id_t id;
    p4_vec_t b;
    uint8_t chunk[512];
    term_registry_stats_t s0, s1;
    int i;

    t_case("a full ring under feed() counts drops_full, not prod_stalls");
    CHK_TRUE(reg_boot());
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 8);
    REQUIRE(id != TERM_ID_INVALID);
    memset(chunk, 'x', sizeof chunk);

    b = p4_snap();
    s0 = reg_stats();
    for (i = 0; i < 256; i++) {
        size_t wrote = 0;
        if (term_registry_feed(id, OWNER_A, chunk, sizeof chunk, &wrote)
            != TERM_OK) break;
    }
    s1 = reg_stats();
    {
        static const long long want[D_N] = P4_NONE;
        p4_expect(&b, want);
    }
    CHK_TRUE(s1.drops_full > s0.drops_full);

    t_case("and a lock timeout on the producer's side is not a drop either");
    {
        static const long long want[D_N] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        size_t sp = 0;
        p4_prod_reset(&p4_pa, "A");
        for (i = 0; i < 64; i++) reg_frame();      /* empty the ring       */
        CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);
        b = p4_snap();
        s0 = reg_stats();
        fp.refuse_bounded = true;
        CHK_INT(term_registry_producer_space(id, &p4_pa, &sp), TERM_ERR_TIMEOUT);
        CHK_INT((int)sp, 0);
        fp.refuse_bounded = false;
        /* A query that never reached the ring is not a "space answered 0":
         * the ring may be empty. Nothing moves. */
        p4_expect(&b, want);
        s1 = reg_stats();
        CHK_INT((int)(s1.drops_full - s0.drops_full), 0);
    }

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

static void case_counters_only_ever_go_up(void)
{
    term_id_t id;
    p4_vec_t prev, now;
    int i, k;
    term_err_t rc;

    t_case("under mixed traffic every counter is monotonic");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    p4_js_reset(&p4_js);
    p4_caret_install(&p4_car);
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 8);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(p4_show(id, OWNER_A, 0, 0, true), TERM_OK);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);

    prev = p4_snap();
    for (i = 0; i < 50; i++) {
        char seq[64];
        long sp = p4_space(id, &p4_pa);
        int n = sprintf(seq, "\x1b[%d;%dH\x1b[6n\x1b[c", (i % 7) + 1,
                        (i % 30) + 1);
        if (sp >= n) CHK_INT((int)p4_write(id, &p4_pa, seq, (size_t)n, &rc), n);
        reg_frame();
        now = p4_snap();
        for (k = 0; k < D_N; k++) {
            t_checks++;
            if (now.v[k] < prev.v[k]) {
                t_head(__FILE__, __LINE__);
                printf("     counter : %s went backwards %lld -> %lld\n",
                       p4_dname[k], prev.v[k], now.v[k]);
            }
        }
        prev = now;
    }

    t_case("...and the totals agree with what the fakes actually saw");
    CHK_INT((int)reg_stats().replies_piped, p4_pa.replies.n);
    CHK_INT((int)reg_stats().caret_pushes, p4_car.n);
    CHK_INT((int)reg_stats().replies_js, 0);
    CHK_INT((int)reg_stats().prod_bytes, (int)reg_info(id).bytes_in);

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("pipe_stats");
    case_a_fresh_table_is_at_zero();
    case_each_counter_has_exactly_one_cause();
    case_feed_drops_are_not_producer_stalls();
    case_counters_only_ever_go_up();
    fp_reclaim_all();
    return t_summary();
}
