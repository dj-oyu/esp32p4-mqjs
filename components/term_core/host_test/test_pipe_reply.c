/*
 * test_pipe_reply.c — where a terminal's answers go (§6, phase 4).
 *
 * Contracts under test:
 *   §6 "端末応答 (DSR 6n / DA) — pipe 時は C 内でチャネルへ直接書き戻し。
 *       JS フィード時は term.onReply"
 *   term_registry.h set_reply, "ROUTING, in order":
 *       "1. a bound producer with a reply route gets it … 2. otherwise this
 *        sink, if one is installed; 3. otherwise the reply is dropped and
 *        counted (stats.replies_dropped)."
 *       "So a piped term does not call its onReply sink. Registering one
 *        anyway is legal and useful … but while a producer holds the term the
 *        sink stays silent."
 *   term_registry.h term_producer_t: "`reply` may be NULL (replies then fall
 *        through to the term's JS sink, if it has one, and are counted as
 *        dropped otherwise)."
 *   term_registry.h term_reply_sink_fn: "CALLED FROM THE UI TASK, FROM INSIDE
 *        term_registry_ui_drain, WITH THE TABLE LOCK HELD … len <=
 *        TERM_REPLY_MAX."
 *   term_core.h: DSR 5n / DSR 6n / primary DA are the replies that exist.
 *
 * The reply CONTENTS are the phase-1 contract (test_reply.c asserts them
 * against term_core directly); this suite asserts that the registry hands
 * exactly those bytes to exactly one of the three destinations.
 */
#include "pipe_util.h"

static p4_prod_t p4_pa;
static p4_js_t   p4_js, p4_js2;

/* Ask the terminal all three questions in one go. */
#define P4_ASK_ALL "\x1b[6n\x1b[5n\x1b[c"

static void p4_feed(term_id_t id, const char *s)
{
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)s, strlen(s), NULL),
            TERM_OK);
}

/* ===================================================================== */
/* Route 2: the JS sink of a term somebody feeds by hand                 */
/* ===================================================================== */

static void case_js_sink_gets_the_bytes(void)
{
    term_id_t id;
    term_registry_stats_t before, after;

    t_case("term.onReply receives the exact CPR/DSR bytes of a fed term");
    CHK_TRUE(reg_boot());
    p4_js_reset(&p4_js);
    id = reg_new("v", OWNER_A, TERM_VT, false, 80, 24);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_set_reply(id, OWNER_A, p4_js_sink, &p4_js), TERM_OK);

    p4_feed(id, "\x1b[3;5H\x1b[6n");

    t_case("...and not before the drain: the parser is what answers (§5)");
    CHK_INT(p4_js.got.n, 0);
    reg_frame();
    CHK_INT(p4_js.got.n, 1);
    CHK_STR(p4_msg(&p4_js.got, 0), "\x1b[3;5R");
    CHK_INT(p4_js.got.id[0], id);

    t_case("DSR 5n and primary DA go the same way");
    before = reg_stats();
    p4_js_reset(&p4_js);
    p4_feed(id, "\x1b[5n");
    reg_frame();
    CHK_INT(p4_js.got.n, 1);
    CHK_STR(p4_msg(&p4_js.got, 0), "\x1b[0n");

    p4_js_reset(&p4_js);
    p4_feed(id, "\x1b[c");
    reg_frame();
    CHK_INT(p4_js.got.n, 1);
    CHK_TRUE(p4_js.got.len[0] >= 4);
    CHK_INT(memcmp(p4_msg(&p4_js.got, 0), "\x1b[?", 3), 0);
    CHK_INT(p4_msg(&p4_js.got, 0)[p4_js.got.len[0] - 1], 'c');

    t_case("only replies_js moves: nothing was piped and nothing was dropped");
    after = reg_stats();
    CHK_INT((int)(after.replies_js - before.replies_js), 2);
    CHK_INT((int)(after.replies_piped - before.replies_piped), 0);
    CHK_INT((int)(after.replies_dropped - before.replies_dropped), 0);

    t_case("the sink runs on the UI task WITH the table lock held (header)");
    CHK_TRUE(p4_js.got.lock_depth_seen > 0);

    t_case("three questions in one buffer answer three times, in order");
    p4_js_reset(&p4_js);
    p4_feed(id, "\x1b[1;1H" P4_ASK_ALL);
    reg_frame();
    CHK_INT(p4_js.got.n, 3);
    CHK_STR(p4_msg(&p4_js.got, 0), "\x1b[1;1R");
    CHK_STR(p4_msg(&p4_js.got, 1), "\x1b[0n");
    CHK_INT(memcmp(p4_msg(&p4_js.got, 2), "\x1b[?", 3), 0);

    t_case("a reply never lands in the grid");
    CHK_STR(reg_snap_line0(id, OWNER_A), "");

    t_case("set_reply(NULL) removes it and the replies are dropped again");
    before = reg_stats();
    CHK_INT(term_registry_set_reply(id, OWNER_A, NULL, NULL), TERM_OK);
    p4_js_reset(&p4_js);
    p4_feed(id, "\x1b[6n");
    reg_frame();
    CHK_INT(p4_js.got.n, 0);
    after = reg_stats();
    CHK_INT((int)(after.replies_dropped - before.replies_dropped), 1);
    CHK_INT((int)(after.replies_js - before.replies_js), 0);

    t_case("a second set_reply replaces the first — one sink per term");
    p4_js_reset(&p4_js);
    p4_js_reset(&p4_js2);
    CHK_INT(term_registry_set_reply(id, OWNER_A, p4_js_sink, &p4_js), TERM_OK);
    CHK_INT(term_registry_set_reply(id, OWNER_A, p4_js_sink, &p4_js2), TERM_OK);
    p4_feed(id, "\x1b[5n");
    reg_frame();
    CHK_INT(p4_js.got.n, 0);
    CHK_INT(p4_js2.got.n, 1);
    CHK_STR(p4_msg(&p4_js2.got, 0), "\x1b[0n");

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Route 3: nobody is listening                                          */
/* ===================================================================== */

static void case_nobody_listening_is_counted(void)
{
    term_id_t id;
    term_registry_stats_t before, after;

    t_case("with neither a pipe nor a sink the reply is dropped and counted");
    CHK_TRUE(reg_boot());
    id = reg_new("v", OWNER_A, TERM_VT, false, 80, 24);
    REQUIRE(id != TERM_ID_INVALID);

    before = reg_stats();
    p4_feed(id, P4_ASK_ALL);
    reg_frame();
    after = reg_stats();
    CHK_INT((int)(after.replies_dropped - before.replies_dropped), 3);
    CHK_INT((int)(after.replies_js - before.replies_js), 0);
    CHK_INT((int)(after.replies_piped - before.replies_piped), 0);

    t_case("dropping a reply does not disturb the screen or the term");
    p4_feed(id, "text");
    reg_frame();
    CHK_STR(reg_snap_line0(id, OWNER_A), "text");
    CHK_INT(reg_state(id), TERM_SLOT_LIVE);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Route 1: the producer's channel wins                                  */
/* ===================================================================== */

static void case_a_pipe_answers_in_c(void)
{
    term_id_t id;
    long sp;
    term_err_t rc = TERM_ERR_INVAL;
    term_registry_stats_t before, after;

    t_case("a piped term's answers go to the producer's channel (§6)");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    p4_js_reset(&p4_js);
    id = reg_new("v", OWNER_A, TERM_VT, false, 80, 24);
    REQUIRE(id != TERM_ID_INVALID);

    /* A JS sink is installed as well, deliberately: the header says
     * registering one on a piped term is legal and must stay silent. */
    CHK_INT(term_registry_set_reply(id, OWNER_A, p4_js_sink, &p4_js), TERM_OK);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, true), TERM_OK);

    sp = p4_space(id, &p4_pa);
    CHK_TRUE(sp > 32);
    {
        static const char ask[] = "\x1b[9;7H" P4_ASK_ALL;
        CHK_INT((int)p4_write(id, &p4_pa, ask, sizeof ask - 1, &rc),
                (int)(sizeof ask - 1));
        CHK_INT(rc, TERM_OK);
    }

    before = reg_stats();
    reg_frame();
    after = reg_stats();

    CHK_INT(p4_pa.replies.n, 3);
    CHK_STR(p4_msg(&p4_pa.replies, 0), "\x1b[9;7R");
    CHK_STR(p4_msg(&p4_pa.replies, 1), "\x1b[0n");
    CHK_INT(memcmp(p4_msg(&p4_pa.replies, 2), "\x1b[?", 3), 0);
    CHK_INT(p4_pa.replies.id[0], id);

    t_case("...and the JS sink stays silent while the producer holds the term");
    CHK_INT(p4_js.got.n, 0);
    CHK_INT((int)(after.replies_piped - before.replies_piped), 3);
    CHK_INT((int)(after.replies_js - before.replies_js), 0);
    CHK_INT((int)(after.replies_dropped - before.replies_dropped), 0);

    t_case("the producer's reply fn also runs under the lock, on the UI task");
    CHK_TRUE(p4_pa.replies.lock_depth_seen > 0);

    t_case("the JS sink becomes live the moment the pipe detaches");
    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    p4_pa.replies.n = 0;
    p4_js_reset(&p4_js);
    before = reg_stats();
    p4_feed(id, "\x1b[5n");
    reg_frame();
    after = reg_stats();
    CHK_INT(p4_pa.replies.n, 0);
    CHK_INT(p4_js.got.n, 1);
    CHK_STR(p4_msg(&p4_js.got, 0), "\x1b[0n");
    CHK_INT((int)(after.replies_js - before.replies_js), 1);

    CHK_INT(reg_shutdown(), 0);
}

static void case_a_pipe_without_a_reply_route_falls_through(void)
{
    term_id_t id;
    term_err_t rc = TERM_ERR_INVAL;
    term_registry_stats_t before, after;

    t_case("a producer with reply == NULL falls through to the JS sink");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    p4_js_reset(&p4_js);
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 8);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_set_reply(id, OWNER_A, p4_js_sink, &p4_js), TERM_OK);
    CHK_INT(p4_pipe(id, OWNER_A, &p4_pa, false), TERM_OK);   /* no reply fn */

    before = reg_stats();
    CHK_INT((int)p4_write(id, &p4_pa, "\x1b[5n", 4, &rc), 4);
    reg_frame();
    after = reg_stats();
    CHK_INT(p4_pa.replies.n, 0);
    CHK_INT(p4_js.got.n, 1);
    CHK_STR(p4_msg(&p4_js.got, 0), "\x1b[0n");
    CHK_INT((int)(after.replies_js - before.replies_js), 1);
    CHK_INT((int)(after.replies_piped - before.replies_piped), 0);

    t_case("...and with no sink either it is dropped and counted");
    CHK_INT(term_registry_set_reply(id, OWNER_A, NULL, NULL), TERM_OK);
    before = reg_stats();
    CHK_INT((int)p4_write(id, &p4_pa, "\x1b[5n", 4, &rc), 4);
    reg_frame();
    after = reg_stats();
    CHK_INT(p4_js.got.n, 1);                        /* unchanged */
    CHK_INT((int)(after.replies_dropped - before.replies_dropped), 1);

    CHK_INT(term_registry_producer_unbind(id, OWNER_A), TERM_OK);
    CHK_INT(term_registry_producer_ack(id), TERM_OK);
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Addressing: the id carries the generation                             */
/* ===================================================================== */

static void case_a_stale_generation_does_not_deliver(void)
{
    term_id_t ids[TERM_SLOT_COUNT];
    term_id_t old_id, reborn;
    char name[TERM_NAME_MAX];
    int i, slot;
    term_registry_stats_t before, after;

    t_case("a sink does not survive its term: the slot's next tenant is not it");
    CHK_TRUE(reg_boot());
    p4_js_reset(&p4_js);

    /* Fill the table so the rebirth is forced back into the same slot — the
     * ABA shape I2's generation exists for. */
    for (i = 0; i < TERM_SLOT_COUNT; i++) {
        sprintf(name, "s%d", i);
        ids[i] = reg_new(name, OWNER_A, TERM_VT, false, 20, 4);
        REQUIRE(ids[i] != TERM_ID_INVALID);
    }
    old_id = ids[3];
    slot = term_id_slot(old_id);
    CHK_INT(term_registry_set_reply(old_id, OWNER_A, p4_js_sink, &p4_js),
            TERM_OK);
    p4_feed(old_id, "\x1b[5n");
    reg_frame();
    CHK_INT(p4_js.got.n, 1);

    CHK_INT(term_registry_close(old_id, OWNER_A), TERM_OK);
    reg_settle();

    reborn = reg_new("reborn", OWNER_B, TERM_VT, false, 20, 4);
    REQUIRE(reborn != TERM_ID_INVALID);
    CHK_INT(term_id_slot(reborn), slot);
    CHK_TRUE(reborn != old_id);
    CHK_TRUE(term_id_generation(reborn) != term_id_generation(old_id));

    t_case("the new tenant's replies reach nobody, not the old sink");
    p4_js_reset(&p4_js);
    before = reg_stats();
    CHK_INT(term_registry_feed(reborn, OWNER_B, (const uint8_t *)"\x1b[5n", 4,
                               NULL), TERM_OK);
    reg_frame();
    after = reg_stats();
    CHK_INT(p4_js.got.n, 0);
    CHK_INT((int)(after.replies_dropped - before.replies_dropped), 1);
    CHK_INT((int)(after.replies_js - before.replies_js), 0);

    t_case("and a sink installed with the stale id is refused (I3)");
    CHK_TRUE(term_registry_set_reply(old_id, OWNER_A, p4_js_sink, &p4_js)
             != TERM_OK);
    CHK_TRUE(reg_stats().denied_stale >= 1);

    t_case("the id a sink is handed is the FULL id, generation included");
    p4_js_reset(&p4_js);
    CHK_INT(term_registry_set_reply(reborn, OWNER_B, p4_js_sink, &p4_js),
            TERM_OK);
    CHK_INT(term_registry_feed(reborn, OWNER_B, (const uint8_t *)"\x1b[5n", 4,
                               NULL), TERM_OK);
    reg_frame();
    CHK_INT(p4_js.got.n, 1);
    CHK_INT(p4_js.got.id[0], reborn);
    CHK_TRUE(p4_js.got.id[0] != old_id);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* TERM_REPLY_MAX                                                        */
/* ===================================================================== */

static void case_reply_max_is_never_exceeded(void)
{
    term_create_opts_t o;
    term_id_t id = TERM_ID_INVALID;
    int i;

    t_case("the longest reply the biggest grid can produce fits in the cap");
    CHK_TRUE(reg_boot());
    p4_js_reset(&p4_js);

    /* CPR is the long one: "\x1b[" + row + ';' + col + 'R'. The biggest
     * numbers a term can report are its own dimensions, so ask for the
     * largest geometry term_core.h allows. */
    o = reg_opts("big", OWNER_A, TERM_VT, false,
                 TERM_MAX_COLS_DEFAULT, TERM_MAX_ROWS_DEFAULT);
    o.max_cols = TERM_MAX_COLS_DEFAULT;
    o.max_rows = TERM_MAX_ROWS_DEFAULT;
    o.max_cells = TERM_MAX_COLS_DEFAULT * TERM_MAX_ROWS_DEFAULT;
    REQUIRE(term_registry_create(&o, &id, NULL) == TERM_OK);
    CHK_INT(term_registry_set_reply(id, OWNER_A, p4_js_sink, &p4_js), TERM_OK);

    {
        char seq[64];
        sprintf(seq, "\x1b[%d;%dH\x1b[6n", TERM_MAX_ROWS_DEFAULT,
                TERM_MAX_COLS_DEFAULT);
        p4_feed(id, seq);
    }
    reg_frame();
    CHK_INT(p4_js.got.n, 1);
    {
        char expect[32];
        sprintf(expect, "\x1b[%d;%dR", TERM_MAX_ROWS_DEFAULT,
                TERM_MAX_COLS_DEFAULT);
        CHK_STR(p4_msg(&p4_js.got, 0), expect);
    }
    CHK_TRUE(p4_js.got.max_len <= (size_t)TERM_REPLY_MAX);
    CHK_INT(p4_js.got.over_cap, 0);

    t_case("no sequence of requests ever hands the sink more than the cap");
    p4_js_reset(&p4_js);
    for (i = 0; i < 40; i++) {
        char seq[80];
        /* Absurd parameters as well as legal ones: B2 clamps them into the
         * grid, so the reply stays short whatever the remote asks. */
        sprintf(seq, "\x1b[%d;%dH" P4_ASK_ALL "\x1b[999999;999999H\x1b[6n",
                (i * 7) % 60 + 1, (i * 13) % 140 + 1);
        p4_feed(id, seq);
        reg_frame();
    }
    CHK_TRUE(p4_js.got.n > 40);
    CHK_TRUE(p4_js.got.max_len <= (size_t)TERM_REPLY_MAX);
    CHK_INT(p4_js.got.over_cap, 0);
    CHK_TRUE(p4_js.got.max_len > 0);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* The sink's world: no recursion, no allocation                          */
/* ===================================================================== */

static void case_reply_delivery_is_clean(void)
{
    term_id_t id;
    int i;

    t_case("delivering replies allocates nothing and never re-locks");
    CHK_TRUE(reg_boot());
    p4_prod_reset(&p4_pa, "A");
    p4_js_reset(&p4_js);
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 8);
    REQUIRE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_set_reply(id, OWNER_A, p4_js_sink, &p4_js), TERM_OK);

    fp.alloc_frozen = true;
    for (i = 0; i < 20; i++) {
        p4_feed(id, P4_ASK_ALL);
        reg_frame();
    }
    fp.alloc_frozen = false;
    CHK_INT(fp.alloc_violations, 0);
    CHK_INT(fp.recursive_attempts, 0);
    CHK_INT(fp.depth, 0);
    CHK_INT(fp.lock_ok, fp.unlocks);
    CHK_INT(p4_js.got.n, 60);

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("pipe_reply");
    case_js_sink_gets_the_bytes();
    case_nobody_listening_is_counted();
    case_a_pipe_answers_in_c();
    case_a_pipe_without_a_reply_route_falls_through();
    case_a_stale_generation_does_not_deliver();
    case_reply_max_is_never_exceeded();
    case_reply_delivery_is_clean();
    fp_reclaim_all();
    return t_summary();
}
