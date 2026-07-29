/*
 * test_reply.c — terminal replies (DSR 6n / DSR 5n / primary DA).
 *
 * Design §6 row "端末応答 (DSR 6n / DA) — pipe 時は C 内でチャネルへ直接
 * 書き戻し。JS フィード時は term.onReply"; header term_reply_fn (emitted from
 * inside term_core_feed, bytes valid only for the call, NULL means generate
 * and drop).
 *
 * The DA identity string is not fixed by the design, so only its shape is
 * asserted. CPR's format is: it is the reply the remote parses.
 */
#include "test_util.h"

typedef struct {
    int calls;
    char buf[256];
    size_t len;
} reply_log_t;

static void reply_sink(void *user, const char *bytes, size_t len)
{
    reply_log_t *l = (reply_log_t *)user;
    l->calls++;
    if (len > sizeof l->buf - 1) len = sizeof l->buf - 1;
    memcpy(l->buf, bytes, len);
    l->buf[len] = 0;
    l->len = len;
}

static void case_cpr(void)
{
    term_config_t cfg = tc_cfg(TERM_VT, 20, 6);
    reply_log_t log;
    tcore_t t;
    term_stats_t st;

    memset(&log, 0, sizeof log);
    cfg.reply_cb = reply_sink;
    cfg.reply_user = &log;
    t = tc_make_cfg(&cfg);

    t_case("DSR 6n answers CPR with the 1-based cursor position");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[3;5H");
    log.calls = 0;
    feed(t.c, "\x1b[6n");
    CHK_INT(log.calls, 1);
    CHK_STR(log.buf, "\x1b[3;5R");

    t_case("CPR follows the cursor");
    feed(t.c, "\x1b[1;1H");
    log.calls = 0;
    feed(t.c, "\x1b[6n");
    CHK_INT(log.calls, 1);
    CHK_STR(log.buf, "\x1b[1;1R");

    t_case("DSR 5n answers the device-status OK report");
    log.calls = 0;
    feed(t.c, "\x1b[5n");
    CHK_INT(log.calls, 1);
    CHK_STR(log.buf, "\x1b[0n");

    t_case("a reply does not disturb the grid or the cursor");
    feed(t.c, "\x1b[2;2Habc");
    log.calls = 0;
    feed(t.c, "\x1b[6n");
    CHK_STR(row_text(t.c, 1), " abc");
    CHK_INT(cursor_row(t.c), 1);
    CHK_INT(cursor_col(t.c), 4);
    CHK_STR(log.buf, "\x1b[2;5R");

    t_case("the replies counter counts them");
    term_core_stats(t.c, &st);
    CHK_INT(st.replies, 4);
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

static void case_da(void)
{
    term_config_t cfg = tc_cfg(TERM_VT, 20, 6);
    reply_log_t log;
    tcore_t t;

    memset(&log, 0, sizeof log);
    cfg.reply_cb = reply_sink;
    cfg.reply_user = &log;
    t = tc_make_cfg(&cfg);

    t_case("primary DA (CSI c) answers a CSI ? ... c device attributes string");
    REQUIRE(t.c != NULL);
    log.calls = 0;
    feed(t.c, "\x1b[c");
    CHK_INT(log.calls, 1);
    CHK_TRUE(log.len >= 4);
    CHK_INT(memcmp(log.buf, "\x1b[?", 3), 0);
    CHK_INT(log.buf[log.len - 1], 'c');

    t_case("CSI 0c is the same request");
    log.calls = 0;
    log.buf[0] = 0;
    feed(t.c, "\x1b[0c");
    CHK_INT(log.calls, 1);
    CHK_INT(memcmp(log.buf, "\x1b[?", 3), 0);

    t_case("a DA reply never lands in the grid");
    CHK_STR(row_text(t.c, 0), "");
    tc_free(&t);
}

static void case_callback_binding(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 6);   /* no reply_cb in the config */
    reply_log_t log;
    term_stats_t st;

    t_case("with no reply callback the reply is generated and dropped");
    REQUIRE(t.c != NULL);
    feed(t.c, "\x1b[6n\x1b[5n\x1b[c");
    CHK_STR(row_text(t.c, 0), "");
    term_core_stats(t.c, &st);
    CHK_INT(st.replies, 3);

    t_case("term_core_set_reply_cb() binds late (registry re-attach path)");
    memset(&log, 0, sizeof log);
    term_core_set_reply_cb(t.c, reply_sink, &log);
    feed(t.c, "\x1b[1;1H\x1b[6n");
    CHK_INT(log.calls, 1);
    CHK_STR(log.buf, "\x1b[1;1R");

    t_case("term_core_set_reply_cb(NULL) detaches it again");
    term_core_set_reply_cb(t.c, NULL, NULL);
    log.calls = 0;
    feed(t.c, "\x1b[6n");
    CHK_INT(log.calls, 0);
    CHK_TRUE(tc_guards_intact(&t));
    tc_free(&t);
}

int main(void)
{
    t_suite("reply");
    case_cpr();
    case_da();
    case_callback_binding();
    return t_summary();
}
