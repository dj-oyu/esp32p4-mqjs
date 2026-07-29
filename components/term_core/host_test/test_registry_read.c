/*
 * test_registry_read.c — snapshot / read: serialised on the UI task,
 * non-mutating, re-validated inside the job.
 *
 * Contracts under test (§7.2, term_registry.h reads section):
 *   "スナップショット要求は MQTT タスクから grid を直接読まず、UI タスクの
 *    キューに post しフレーム境界でシリアライズして返信"
 *   "post に載せた id は 実行時点で generation を再検証し、post〜実行の間に
 *    teardown/再利用が挟まった場合はエラー返信(古い要求が別アプリの新 term
 *    を読む誤配信を塞ぐ)"
 *   "フォーマット: 行ごと UTF-8(CONT セルはスキップ、既定で属性なし)…
 *    読み出しは 無変異(probe が測定対象を変えない)"
 *   header: "a caller that is already the UI task runs it inline";
 *   "On timeout the result is TERM_ERR_TIMEOUT and the output buffer is
 *    untouched"; TERM_ERR_TRUNC "means out_size was too small and *out_len
 *    holds what would have been needed".
 */
#include "reg_util.h"

#define GUARD_BYTE 0x7e

/* Static so its lifetime cannot be confused with a stack-lifetime bug: the
 * question this buffer answers is "did anything write here", not "when". */
static char slow_out[512];

static bool guard_intact(const char *buf, size_t from, size_t to)
{
    size_t i;
    for (i = from; i < to; i++)
        if (buf[i] != (char)GUARD_BYTE) return false;
    return true;
}

/* ===================================================================== */
/* Format                                                                */
/* ===================================================================== */

static void case_snapshot_format(void)
{
    term_id_t id;
    char out[2048];
    size_t len = 0;
    int lines = 0;
    size_t i;

    t_case("snapshot: one line per row, UTF-8, no trailing blanks (§7.2)");
    CHK_TRUE(reg_boot());
    id = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_feed(id, OWNER_A,
                               (const uint8_t *)"hello\r\nworld  \r\n", 16, NULL),
            TERM_OK);
    reg_frame();

    memset(out, 0, sizeof out);
    CHK_INT(term_registry_snapshot(id, OWNER_A, out, sizeof out, &len), TERM_OK);
    CHK_INT((int)len, (int)strlen(out));
    CHK_TRUE(strncmp(out, "hello\n", 6) == 0);
    CHK_TRUE(strstr(out, "world") != NULL);

    for (i = 0; i < len; i++) {
        if (out[i] == '\n') lines++;
        /* "trailing blanks trimmed" — no row may end in a space */
        if (out[i] == ' ' && (out[i + 1] == '\n' || out[i + 1] == 0)) {
            t_head(__FILE__, __LINE__);
            printf("     snapshot row ends in a blank at byte %u\n", (unsigned)i);
        }
        t_checks++;
    }
    t_case("a snapshot never describes more rows than the term has");
    CHK_TRUE(lines <= 4);

    t_case("wide characters: the CONT half is skipped, not doubled");
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"\x1b[2J\x1b[H" "\xe6\xbc\xa2\xe5\xad\x97", 13, NULL),
            TERM_OK);
    reg_frame();
    CHK_TRUE(strncmp(reg_snap_line0(id, OWNER_A), "\xe6\xbc\xa2\xe5\xad\x97", 6) == 0);

    CHK_INT(reg_shutdown(), 0);
}

static void case_snapshot_truncation(void)
{
    term_id_t id;
    static char buf[256];
    size_t need = 0, full = 0;
    term_err_t rc;

    t_case("a short output buffer is TERM_ERR_TRUNC with the needed size");
    CHK_TRUE(reg_boot());
    id = reg_new("v", OWNER_A, TERM_VT, false, 40, 6);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_feed(id, OWNER_A,
                               (const uint8_t *)"aaaa\r\nbbbb\r\ncccc\r\ndddd", 21, NULL),
            TERM_OK);
    reg_frame();

    memset(buf, 0, sizeof buf);
    CHK_INT(term_registry_snapshot(id, OWNER_A, buf, sizeof buf, &full), TERM_OK);
    CHK_TRUE(full > 8);

    memset(buf, GUARD_BYTE, sizeof buf);
    rc = term_registry_snapshot(id, OWNER_A, buf, 8, &need);
    CHK_INT(rc, TERM_ERR_TRUNC);
    CHK_INT((int)need, (int)full);
    t_case("...what fits was written, NUL-terminated, and nothing beyond");
    CHK_INT(buf[7], 0);
    CHK_TRUE(strlen(buf) <= 7);
    CHK_TRUE(guard_intact(buf, 8, sizeof buf));

    t_case("out_size 0 / NULL out are errors, not writes");
    CHK_TRUE(term_registry_snapshot(id, OWNER_A, NULL, 100, &need) != TERM_OK);
    CHK_TRUE(term_registry_snapshot(id, OWNER_A, buf, 0, &need) != TERM_OK);
    CHK_TRUE(guard_intact(buf, 8, sizeof buf));

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Non-mutation (§7.2: "probe が測定対象を変えない")                      */
/* ===================================================================== */

typedef struct {
    int  visits;
    int  first_dirty;
    bool full_repaint;
    int  cols, rows;
    term_view_t view;
    term_id_t id;
} peek_t;

static void peek_cb(term_id_t id, term_core_t *core, const term_view_t *view,
                    void *user)
{
    peek_t *p = (peek_t *)user;
    p->visits++;
    p->id = id;
    if (core) {
        p->first_dirty = term_core_dirty_next(core, 0);
        p->full_repaint = term_core_full_repaint(core);
        p->cols = term_core_cols(core);
        p->rows = term_core_rows(core);
    }
    if (view) p->view = *view;
}

static void case_reads_do_not_mutate(void)
{
    term_id_t id;
    term_view_t view;
    peek_t before, after;
    term_registry_stats_t s0, s1;
    const char *snap_a;
    static char snap_copy[REG_SNAP_CAP];
    char out[512];
    term_read_result_t res;

    t_case("neither snapshot nor read disturbs the damage set or the core");
    CHK_TRUE(reg_boot());
    id = reg_new("v", OWNER_A, TERM_LOG, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    memset(&view, 0, sizeof view);
    view.w = 64; view.h = 64; view.visible = true;
    CHK_INT(term_registry_show(id, OWNER_A, &view), TERM_OK);
    CHK_INT(term_registry_log(id, OWNER_A, "one", 3), TERM_OK);
    CHK_INT(term_registry_log(id, OWNER_A, "two", 3), TERM_OK);
    reg_frame();
    /* Make some damage and deliberately do NOT paint it away. */
    CHK_INT(term_registry_log(id, OWNER_A, "three", 5), TERM_OK);
    fp_pump();
    term_registry_ui_drain();

    memset(&before, 0, sizeof before);
    CHK_INT(term_registry_ui_visit(peek_cb, &before), 1);
    CHK_INT(before.visits, 1);
    CHK_TRUE(before.first_dirty >= 0 || before.full_repaint);
    s0 = reg_stats();

    snap_a = reg_snap(id, OWNER_A);
    memcpy(snap_copy, snap_a, strlen(snap_a) + 1);
    memset(&res, 0, sizeof res);
    CHK_INT(term_registry_read(id, OWNER_A, 0, 4, out, sizeof out, &res), TERM_OK);

    memset(&after, 0, sizeof after);
    CHK_INT(term_registry_ui_visit(peek_cb, &after), 1);
    CHK_INT(after.first_dirty, before.first_dirty);
    CHK_INT(after.full_repaint, before.full_repaint);
    CHK_INT(after.cols, before.cols);
    CHK_INT(after.rows, before.rows);

    t_case("...and the same snapshot comes back the second time");
    CHK_STR(reg_snap(id, OWNER_A), snap_copy);

    t_case("reads do not move the ingest counters either");
    s1 = reg_stats();
    CHK_INT(s1.drops_lock, s0.drops_lock);
    CHK_INT(s1.drops_full, s0.drops_full);
    CHK_INT((int)reg_info(id).bytes_dropped, 0);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* Where the job runs                                                    */
/* ===================================================================== */

static void case_inline_when_already_on_the_ui_task(void)
{
    term_id_t id;
    char out[256];
    int posts_before;

    t_case("a caller that IS the UI task runs the job inline (header)");
    CHK_TRUE(reg_boot());
    id = reg_new("v", OWNER_A, TERM_LOG, false, 20, 4);
    CHK_INT(term_registry_log(id, OWNER_A, "inline", 6), TERM_OK);
    reg_frame();

    fp.ui_current = true;
    posts_before = fp.posts;
    memset(out, 0, sizeof out);
    CHK_INT(term_registry_snapshot(id, OWNER_A, out, sizeof out, NULL), TERM_OK);
    CHK_TRUE(strstr(out, "inline") != NULL);
    CHK_INT(fp.posts, posts_before);        /* no post to itself           */
    CHK_INT(fp.signal_waits, 0);            /* and no self-join            */
    fp.ui_current = false;

    CHK_INT(reg_shutdown(), 0);
}

static void case_post_refused(void)
{
    term_id_t id;
    static char out[256];
    term_read_result_t res;
    term_registry_stats_t s0, s1;

    t_case("a UI queue that refuses the job is TERM_ERR_POST, counted");
    CHK_TRUE(reg_boot());
    id = reg_new("v", OWNER_A, TERM_LOG, false, 20, 4);
    CHK_INT(term_registry_log(id, OWNER_A, "hidden", 6), TERM_OK);
    reg_frame();

    memset(out, GUARD_BYTE, sizeof out);
    memset(&res, 0, sizeof res);
    s0 = reg_stats();
    fp.ui_mode = FP_UI_REFUSE;
    CHK_INT(term_registry_snapshot(id, OWNER_A, out, sizeof out, NULL),
            TERM_ERR_POST);
    CHK_INT(term_registry_read(id, OWNER_A, 0, 2, out, sizeof out, &res),
            TERM_ERR_POST);
    CHK_TRUE(term_registry_resize(id, OWNER_A, 30, 5) == TERM_ERR_POST);
    fp.ui_mode = FP_UI_INLINE;
    s1 = reg_stats();
    CHK_TRUE(s1.post_fails - s0.post_fails >= 3);

    t_case("...and the output buffer was not touched");
    CHK_TRUE(guard_intact(out, 0, sizeof out));

    CHK_INT(reg_shutdown(), 0);
}

static void case_timed_out_job_never_writes_back(void)
{
    term_id_t a, b;
    term_err_t rc;
    int i;

    t_case("a read whose job never ran is TERM_ERR_TIMEOUT, buffer untouched");
    CHK_TRUE(reg_boot());
    fp.ui_mode = FP_UI_DEFER;              /* the UI task is not running */
    a = reg_new("A", OWNER_A, TERM_LOG, false, 20, 4);
    CHK_TRUE(a != TERM_ID_INVALID);
    CHK_INT(term_registry_log(a, OWNER_A, "AAAA", 4), TERM_OK);
    fp_pump();
    term_registry_ui_drain();

    memset(slow_out, GUARD_BYTE, sizeof slow_out);
    rc = term_registry_snapshot(a, OWNER_A, slow_out, sizeof slow_out, NULL);
    CHK_INT(rc, TERM_ERR_TIMEOUT);
    CHK_TRUE(guard_intact(slow_out, 0, sizeof slow_out));
    CHK_TRUE(fp_jobs_pending() >= 1);

    t_case("the slot is torn down and reused while the job is still queued");
    CHK_INT(term_registry_close(a, OWNER_A), TERM_OK);
    for (i = 0; i < 4; i++) {
        term_registry_ui_drain();          /* no fp_pump: the job waits   */
        reg_paint();
        term_registry_reap();
    }
    b = reg_new("B", OWNER_B, TERM_LOG, false, 20, 4);
    CHK_TRUE(b != TERM_ID_INVALID);
    CHK_INT(term_registry_log(b, OWNER_B, "BBBB", 4), TERM_OK);
    term_registry_ui_drain();

    /* Every byte still reads 0x7e, so neither the closed term's screen nor
     * the new tenant's was serialised into a buffer whose caller gave up —
     * that is §7.2's "post〜実行の間に teardown/再利用が挟まった場合は
     * エラー返信" seen from the caller's side. */
    t_case("running it now must not deliver anything to the old caller");
    fp_pump();
    CHK_TRUE(guard_intact(slow_out, 0, sizeof slow_out));

    fp.ui_mode = FP_UI_INLINE;
    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* read() edges                                                          */
/* ===================================================================== */

static void case_read_edges(void)
{
    term_id_t id;
    char out[1024];
    term_read_result_t res;
    int i;

    t_case("read of an empty scrollback is an empty answer, not an error");
    CHK_TRUE(reg_boot());
    id = reg_new("v", OWNER_A, TERM_LOG, false, 20, 6);
    CHK_TRUE(id != TERM_ID_INVALID);
    memset(&res, 0xff, sizeof res);
    memset(out, 0xff, sizeof out);
    CHK_INT(term_registry_read(id, OWNER_A, 0, 4, out, sizeof out, &res), TERM_OK);
    CHK_INT(res.lines, 0);
    CHK_INT((int)res.first, (int)res.next);
    CHK_INT((int)res.bytes, 0);

    t_case("n <= 0 is TERM_ERR_INVAL (header)");
    CHK_INT(term_registry_read(id, OWNER_A, 0, 0, out, sizeof out, &res),
            TERM_ERR_INVAL);
    CHK_INT(term_registry_read(id, OWNER_A, 0, -1, out, sizeof out, &res),
            TERM_ERR_INVAL);

    for (i = 0; i < 30; i++) {
        char line[32];
        int n = sprintf(line, "n%d", i);
        CHK_INT(term_registry_log(id, OWNER_A, line, (size_t)n), TERM_OK);
    }
    reg_frame();

    t_case("`from` below the surviving range starts at the oldest (header)");
    memset(&res, 0, sizeof res);
    CHK_INT(term_registry_read(id, OWNER_A, 0, 2, out, sizeof out, &res), TERM_OK);
    CHK_INT(res.lines, 2);
    CHK_STR(out, "n0\nn1");
    CHK_INT((int)res.next, (int)res.first + 2);

    t_case("`from` past the end yields nothing and still reports the range");
    memset(&res, 0, sizeof res);
    CHK_INT(term_registry_read(id, OWNER_A, 0x7ffffff0u, 4, out, sizeof out, &res),
            TERM_OK);
    CHK_INT(res.lines, 0);

    t_case("a small output buffer truncates rather than overruns");
    memset(out, GUARD_BYTE, sizeof out);
    memset(&res, 0, sizeof res);
    CHK_INT(term_registry_read(id, OWNER_A, 0, 20, out, 12, &res), TERM_OK);
    CHK_TRUE(res.truncated);
    CHK_TRUE(res.lines < 20);
    CHK_TRUE(strlen(out) < 12);
    CHK_TRUE(guard_intact(out, 12, sizeof out));

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* show / visit                                                          */
/* ===================================================================== */

static void case_show_and_visit(void)
{
    term_id_t a, b, c;
    term_view_t va, vb;
    peek_t seen;
    int n;

    t_case("visit iterates the terms that should be painted, in slot order");
    CHK_TRUE(reg_boot());
    a = reg_new("a", OWNER_A, TERM_LOG, false, 20, 4);
    b = reg_new("b", OWNER_A, TERM_LOG, false, 20, 4);
    c = reg_new("c", OWNER_A, TERM_LOG, false, 20, 4);
    CHK_TRUE(a != TERM_ID_INVALID && b != TERM_ID_INVALID && c != TERM_ID_INVALID);

    memset(&va, 0, sizeof va);
    va.x = 4; va.y = 8; va.w = 100; va.h = 60; va.visible = true;
    memset(&vb, 0, sizeof vb);
    vb.x = 0; vb.y = 0; vb.w = 50; vb.h = 50; vb.visible = false;

    CHK_INT(term_registry_show(a, OWNER_A, &va), TERM_OK);
    CHK_INT(term_registry_show(b, OWNER_A, &vb), TERM_OK);
    reg_frame();

    memset(&seen, 0, sizeof seen);
    n = term_registry_ui_visit(peek_cb, &seen);
    CHK_INT(n, 1);
    CHK_INT(seen.id, a);
    CHK_INT(seen.view.x, 4);
    CHK_INT(seen.view.y, 8);
    CHK_INT(seen.view.w, 100);
    CHK_INT(seen.view.h, 60);
    CHK_TRUE(seen.view.visible);

    t_case("show(NULL) hides (tab switching is show/hide, §8)");
    CHK_INT(term_registry_show(a, OWNER_A, NULL), TERM_OK);
    reg_frame();
    memset(&seen, 0, sizeof seen);
    CHK_INT(term_registry_ui_visit(peek_cb, &seen), 0);
    CHK_INT(seen.visits, 0);

    t_case("re-showing brings it back with the rect it was given");
    CHK_INT(term_registry_show(a, OWNER_A, &va), TERM_OK);
    reg_frame();
    memset(&seen, 0, sizeof seen);
    CHK_INT(term_registry_ui_visit(peek_cb, &seen), 1);
    CHK_INT(seen.view.w, 100);

    t_case("visit tolerates a NULL callback");
    CHK_INT(term_registry_ui_visit(NULL, NULL), 0);

    CHK_INT(reg_shutdown(), 0);
}

/* ===================================================================== */
/* resize is posted, not written in place (§5)                           */
/* ===================================================================== */

static void case_resize_is_posted(void)
{
    term_id_t id;

    t_case("resize does not write cols/rows on the calling task (§5)");
    CHK_TRUE(reg_boot());
    fp.ui_mode = FP_UI_DEFER;
    id = reg_new("v", OWNER_A, TERM_VT, false, 20, 4);
    CHK_TRUE(id != TERM_ID_INVALID);
    CHK_INT(term_registry_resize(id, OWNER_A, 40, 8), TERM_OK);
    CHK_INT(reg_info(id).cols, 20);        /* still the old geometry */
    CHK_INT(reg_info(id).rows, 4);
    CHK_TRUE(fp_jobs_pending() >= 1);

    t_case("...it lands at the frame boundary");
    fp_pump();
    CHK_INT(reg_info(id).cols, 40);
    CHK_INT(reg_info(id).rows, 8);

    t_case("content survives a resize (term_core_resize carries it over)");
    fp.ui_mode = FP_UI_INLINE;
    CHK_INT(term_registry_feed(id, OWNER_A, (const uint8_t *)"survive", 7, NULL),
            TERM_OK);
    reg_frame();
    CHK_INT(term_registry_resize(id, OWNER_A, 30, 6), TERM_OK);
    reg_frame();
    CHK_TRUE(strstr(reg_snap(id, OWNER_A), "survive") != NULL);

    /* term_core B5: "A resize never allocates, never fails for want of
     * memory, and can only be refused for exceeding the maxima fixed at
     * init." Whether the refusal is reported at post time or swallowed in
     * the job the design does not say; that the geometry does not move,
     * and that nothing is allocated to try, it does. */
    t_case("a geometry past the maxima never takes effect (B5, I4)");
    (void)term_registry_resize(id, OWNER_A, TERM_MAX_COLS_DEFAULT + 1, 6);
    (void)term_registry_resize(id, OWNER_A, 0, 6);
    (void)term_registry_resize(id, OWNER_A, 20, 0);
    (void)term_registry_resize(id, OWNER_A, -5, -5);
    reg_frame();
    CHK_INT(reg_info(id).cols, 30);
    CHK_INT(reg_info(id).rows, 6);
    CHK_INT(fp.allocs, 1);

    CHK_INT(reg_shutdown(), 0);
}

int main(void)
{
    t_suite("registry_read");
    case_snapshot_format();
    case_snapshot_truncation();
    case_reads_do_not_mutate();
    case_inline_when_already_on_the_ui_task();
    case_post_refused();
    case_timed_out_job_never_writes_back();
    case_read_edges();
    case_show_and_visit();
    case_resize_is_posted();
    fp_reclaim_all();
    return t_summary();
}
