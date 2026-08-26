/*
 * test_prof.cpp — 提示メータ (spec §F #13) のホストテスト。
 *
 * 何を見ているか
 *   LVGL のイベント列から 1 提示ぶんの内訳を組み立てる算術:
 *   draw = render - flush - wait の分け方、チャンク数、回転の分離、
 *   窓の集計、**外れ値を黙って捨てないこと** (C.0 規則 7)、
 *   報告のあとに窓が空になること、キャンバス由来の数え方。
 *
 * 何を見ていないか (実機でしか分からない)
 *   LVGL が本当にこの順でイベントを出すか、flush_cb の中身、
 *   PPA 回転の実時間、UART へ 1 行吐く実コスト、
 *   RENDER_START が「1 リフレッシュに 1 回」であること。
 *
 * ui_tab5.cpp からマーカーで切り出した本物のテキストを走らせる
 * (写しではない)。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- 偽の LVGL イベント層 ---- */
typedef enum {
    LV_EVENT_RENDER_START,
    LV_EVENT_RENDER_READY,
    LV_EVENT_FLUSH_START,
    LV_EVENT_FLUSH_FINISH,
    LV_EVENT_FLUSH_WAIT_START,
    LV_EVENT_FLUSH_WAIT_FINISH,
} lv_event_code_t;
typedef struct { lv_event_code_t code; } lv_event_t;
typedef struct lv_display_t lv_display_t;
static lv_event_code_t lv_event_get_code(lv_event_t *e) { return e->code; }
static int g_attached;
static void lv_display_add_event_cb(lv_display_t *, void (*)(lv_event_t *),
                                    lv_event_code_t, void *)
{
    g_attached++;
}

/* ---- 偽の時計とログ ---- */
static int64_t g_now;
static int64_t esp_timer_get_time(void) { return g_now; }

static char g_last_log[512];
static int g_logs;
#define ESP_LOGW(tag, ...)                                                 \
    do {                                                                   \
        (void)(tag);                                                       \
        snprintf(g_last_log, sizeof g_last_log, __VA_ARGS__);              \
        g_logs++;                                                          \
    } while (0)

/* ---- 切り出した本体が読む外の変数 ----
   s_prof_canvas_mark と s_pm はマーカーの内側で定義されているので、
   ここでは宣言しない (二重定義になる)。 */
static bool s_landscape;
static int64_t s_ppa_rot_us;

#include "profmeter.inc"

/* ------------------------------------------------------------------ */
static int g_fail;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

static void ev(lv_event_code_t code, int64_t at)
{
    g_now = at;
    lv_event_t e = { code };
    prof_event(&e);
}

/* 1 提示を組み立てる。t0 から始まり、chunks 個のチャンクがそれぞれ
   flush_us / wait_us / rot_us を使い、全体で total_us かかる。
   戻り値は「終わった時刻」。 */
static int64_t present(int64_t t0, int chunks, int flush_us, int wait_us,
                       int rot_us, int total_us)
{
    ev(LV_EVENT_RENDER_START, t0);
    int64_t t = t0 + 10;
    for (int i = 0; i < chunks; i++) {
        ev(LV_EVENT_FLUSH_WAIT_START, t);
        t += wait_us;
        ev(LV_EVENT_FLUSH_WAIT_FINISH, t);
        ev(LV_EVENT_FLUSH_START, t);
        s_ppa_rot_us += rot_us; /* __wrap_lvgl_port_ppa_rotate の代役 */
        t += flush_us;
        ev(LV_EVENT_FLUSH_FINISH, t);
    }
    ev(LV_EVENT_RENDER_READY, t0 + total_us);
    return t0 + total_us;
}

static void reset(void)
{
    memset(&s_pm, 0, sizeof s_pm);
    memset(&s_prof, 0, sizeof s_prof);
    s_ppa_rot_us = 0;
    s_prof_canvas_mark = false;
    g_logs = 0;
    g_last_log[0] = 0;
}

/* ---- 1. 1 提示の内訳が正しく分かれる ---------------------------- */
static void t_breakdown(void)
{
    printf("t_breakdown\n");
    reset();
    /* 26 チャンク、各 flush 300us / wait 200us / rot 120us、全体 40ms */
    present(1000, 26, 300, 200, 120, 40000);
    CHECK(s_pm.refr == 1, "refr=%u", s_pm.refr);
    CHECK(s_pm.chunks == 26, "chunks=%u", s_pm.chunks);
    CHECK(s_pm.flush == 26 * 300, "flush=%lld", (long long)s_pm.flush);
    CHECK(s_pm.wait == 26 * 200, "wait=%lld", (long long)s_pm.wait);
    CHECK(s_pm.rot == 26 * 120, "rot=%lld", (long long)s_pm.rot);
    CHECK(s_pm.draw == 40000 - 26 * 500, "draw=%lld", (long long)s_pm.draw);
    CHECK(s_pm.w_chunks == 26, "worst chunks=%d", s_pm.w_chunks);
    CHECK(s_pm.dropped == 0, "dropped=%u", s_pm.dropped);
}

/* ---- 2. 回転は提示ごとにリセットされる (前の提示に足さない) ------ */
static void t_rot_per_presentation(void)
{
    printf("t_rot_per_presentation\n");
    reset();
    int64_t t = present(1000, 2, 300, 100, 500, 5000);
    CHECK(s_pm.rot == 2 * 500, "first rot=%lld", (long long)s_pm.rot);
    present(t + 1000, 2, 300, 100, 700, 5000);
    /* 2 提示ぶんの合計であって、1 提示目のぶんが二重に入らない */
    CHECK(s_pm.rot == 2 * 500 + 2 * 700, "rot=%lld", (long long)s_pm.rot);
    CHECK(s_pm.refr == 2, "refr=%u", s_pm.refr);
}

/* ---- 3. 外れ値は黙って捨てず、数える (C.0 規則 7) ---------------- */
static void t_outlier_counted(void)
{
    printf("t_outlier_counted\n");
    reset();
    present(1000, 1, 100, 100, 0, 2000000); /* 2 秒 = ありえない */
    CHECK(s_pm.dropped == 1, "dropped=%u", s_pm.dropped);
    CHECK(s_pm.refr == 0, "an outlier was folded in (refr=%u)", s_pm.refr);
    CHECK(s_pm.draw == 0, "an outlier polluted draw=%lld",
          (long long)s_pm.draw);
}

/* ---- 4. イベントの対が壊れて draw が負になったら外れ値 ---------- */
static void t_negative_draw_dropped(void)
{
    printf("t_negative_draw_dropped\n");
    reset();
    /* flush + wait が render 全体より長い = 対が揃わなかった */
    present(1000, 4, 3000, 3000, 0, 1000);
    CHECK(s_pm.dropped == 1, "dropped=%u", s_pm.dropped);
    CHECK(s_pm.refr == 0, "refr=%u", s_pm.refr);
}

/* ---- 5. 窓の中では 1 行も出さない / 窓を越えたら 1 行だけ出す ---- */
static void t_window(void)
{
    printf("t_window\n");
    reset();
    /* **起動から 20 秒の時刻から始める。** ここを 0 の近くから始めると、
       「最初の提示は窓の起点を置くだけ」を消しても now - 0 が窓に届かず、
       テストは何も見ない (最初に書いたときそうだった)。実機の時計は
       ブートからの µs なので、そちらに合わせる。 */
    int64_t t = 20000000;
    /* 最初の提示は窓の起点を置くだけ (報告しない) */
    t = present(t, 1, 100, 100, 0, 1000) + 1000;
    CHECK(g_logs == 0, "reported on the very first presentation");
    for (int i = 0; i < 20; i++) {
        t = present(t, 1, 100, 100, 0, 1000) + 1000;
        CHECK(g_logs == 0, "reported inside the window (i=%d)", i);
    }
    /* 窓を越える */
    t = present(t + 5000000, 3, 100, 100, 0, 1000);
    CHECK(g_logs == 1, "no report after the window elapsed (logs=%d)", g_logs);
    /* 窓は空になり、起点は書いた後の時刻 */
    CHECK(s_pm.refr == 0, "window not cleared (refr=%u)", s_pm.refr);
    CHECK(s_pm.chunks == 0, "window not cleared (chunks=%u)", s_pm.chunks);
    CHECK(s_pm.dropped == 0, "window not cleared (dropped=%u)", s_pm.dropped);
    CHECK(s_pm.t_report != 0, "window start not re-armed");
}

/* ---- 6. キャンバス由来の提示だけが cv に数えられる --------------- */
static void t_canvas_attribution(void)
{
    printf("t_canvas_attribution\n");
    reset();
    int64_t t = 1000;
    s_prof_canvas_mark = true;
    t = present(t, 1, 100, 100, 0, 1000) + 1000;
    CHECK(s_pm.refr_cv == 1, "canvas refresh not counted");
    CHECK(!s_prof_canvas_mark, "the mark was not consumed");
    t = present(t, 1, 100, 100, 0, 1000) + 1000;
    CHECK(s_pm.refr_cv == 1, "a non-canvas refresh was counted as canvas");
    CHECK(s_pm.refr == 2, "refr=%u", s_pm.refr);
}

/* ---- 7. いちばんチャンクの多かった提示が残る --------------------- */
static void t_worst_is_widest(void)
{
    printf("t_worst_is_widest\n");
    reset();
    int64_t t = 1000;
    t = present(t, 2, 100, 100, 0, 3000) + 1000;
    t = present(t, 26, 100, 100, 0, 30000) + 1000;
    t = present(t, 4, 100, 100, 0, 5000) + 1000;
    CHECK(s_pm.w_chunks == 26, "worst chunks=%d", s_pm.w_chunks);
    CHECK(s_pm.w_draw == 30000 - 26 * 200, "worst draw=%lld",
          (long long)s_pm.w_draw);
}

/* ---- 8. 報告の 1 行に必要な数字が入っている --------------------- */
static void t_report_line(void)
{
    printf("t_report_line\n");
    reset();
    s_landscape = true;
    int64_t t = present(1000, 1, 100, 100, 50, 1000) + 1000;
    present(t + 5000000, 26, 100, 100, 50, 30000);
    CHECK(g_logs == 1, "logs=%d", g_logs);
    CHECK(strstr(g_last_log, "L ") == g_last_log, "no orientation: %s",
          g_last_log);
    CHECK(strstr(g_last_log, "draw=") != NULL, "no draw: %s", g_last_log);
    CHECK(strstr(g_last_log, "rot=") != NULL, "no rot: %s", g_last_log);
    CHECK(strstr(g_last_log, "wait=") != NULL, "no wait: %s", g_last_log);
    CHECK(strstr(g_last_log, "max 26 ch") != NULL, "no worst: %s", g_last_log);
    CHECK(strstr(g_last_log, "drop=") != NULL, "no drop: %s", g_last_log);
    /* 1 行が長いと 115200 baud では実時間を食う。240 B を上限にしておく
       (~21ms)。これを越えるならフォーマットを削ること。 */
    CHECK(strlen(g_last_log) < 240, "report line is %u bytes: %s",
          (unsigned)strlen(g_last_log), g_last_log);
    printf("  line: %s\n", g_last_log);
    s_landscape = false;
}

/* ---- 9. イベントを 6 本とも登録する ------------------------------ */
static void t_attach(void)
{
    printf("t_attach\n");
    g_attached = 0;
    prof_attach(NULL);
    CHECK(g_attached == 6, "attached %d event callbacks", g_attached);
}

int main(void)
{
    t_breakdown();
    t_rot_per_presentation();
    t_outlier_counted();
    t_negative_draw_dropped();
    t_window();
    t_canvas_attribution();
    t_worst_is_widest();
    t_report_line();
    t_attach();
    if (g_fail) {
        printf("FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("ok\n");
    return 0;
}
