/*
 * fs_io — 1 本のタスクが順番に I/O をこなす。詳細と理由はヘッダに。
 */
#include "fs_io.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "fs_core.h"
#include "mqjs_runtime.h"   /* MQJS_SCRIPT_MAX */

#define TAG "fs_io"

/* 8 段。§A.7。満杯で待たないのは、呼び出し側 (edit_task / UI) の応答性を
   I/O の都合で人質に取らないため。 */
#define FSIO_QUEUE_DEPTH 8
#define FSIO_PATH_MAX    192
#define FSIO_STACK       8192
#define FSIO_PRIO        3
#define FSIO_CORE        0

typedef struct {
    fsio_kind_t     kind;
    char            from[FSIO_PATH_MAX];
    char            to[FSIO_PATH_MAX];
    void           *buf;
    size_t          len;
    fsio_cb_t       cb;
    fsio_progress_t prog;
    void           *ctx;
} fsio_job_t;

static QueueHandle_t s_q;
static bool          s_started;

static void finish(const fsio_job_t *j, bool ok, esp_err_t err,
                   void *buf, size_t len)
{
    fsio_result_t r;

    r.kind = j->kind;
    r.ok = ok;
    r.err = err;
    r.buf = buf;
    r.len = len;
    if (j->cb)
        j->cb(j->ctx, &r);
    else if (buf)
        free(buf);      /* 受け取り手が居ないなら捨てる。漏らさない */
}

/* LOAD: PSRAM に読み、所有権を cb へ渡す。
   MQJS_SCRIPT_MAX + 1 を確保するのは、末尾に NUL を置いて「C 文字列として
   そのまま扱える」ことを保証するため (§A.7)。ファイルが上限より大きければ
   **切り詰めずに失敗させる** —— 途中まで読めた原稿を編集して保存すると、
   残りを黙って捨てることになる。 */
static void do_load(const fsio_job_t *j)
{
    fs_stat_t st;
    esp_err_t e = fs_stat(j->from, &st);
    size_t n = 0;
    char *buf;

    if (e != ESP_OK) {
        finish(j, false, e, NULL, 0);
        return;
    }
    if (st.size > (uint64_t)MQJS_SCRIPT_MAX) {
        ESP_LOGW(TAG, "%s は %llu B で上限 %d を超える", j->from,
                 (unsigned long long)st.size, MQJS_SCRIPT_MAX);
        finish(j, false, ESP_ERR_INVALID_SIZE, NULL, 0);
        return;
    }
    buf = heap_caps_malloc((size_t)MQJS_SCRIPT_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        finish(j, false, ESP_ERR_NO_MEM, NULL, 0);
        return;
    }
    e = fs_read(j->from, 0, buf, (size_t)MQJS_SCRIPT_MAX, &n);
    if (e != ESP_OK) {
        free(buf);
        finish(j, false, e, NULL, 0);
        return;
    }
    buf[n] = '\0';
    finish(j, true, ESP_OK, buf, n);
}

/* SAVE: <vpath>.tmp へ書いてから rename。
   **途中で電源が落ちても原本が残る**のがこの順序の全部で、直接上書きに
   すると「保存中に落ちた = 原稿が消えた」になる。rename は同一ボリューム
   なら fs_move が rename(2) に落ちる。 */
static void do_save(const fsio_job_t *j)
{
    char tmp[FSIO_PATH_MAX];
    esp_err_t e;
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", j->from);

    if (n < 0 || (size_t)n >= sizeof tmp) {
        finish(j, false, ESP_ERR_INVALID_ARG, NULL, 0);
        return;
    }
    e = fs_write(tmp, j->buf, j->len, false);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "write %s: %s", tmp, esp_err_to_name(e));
        finish(j, false, e, NULL, 0);
        return;
    }
    /* 既に本体があると rename が拒まれる実装があるので先に消す。
       ここで落ちても .tmp は残っているので原稿は失われない。 */
    fs_remove(j->from, false);
    e = fs_move(tmp, j->from);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "rename %s -> %s: %s", tmp, j->from,
                 esp_err_to_name(e));
        finish(j, false, e, NULL, 0);
        return;
    }
    finish(j, true, ESP_OK, NULL, 0);
}

static void run_job(fsio_job_t *j)
{
    esp_err_t e;

    switch (j->kind) {
    case FSIO_LOAD:
        do_load(j);
        break;
    case FSIO_SAVE:
        do_save(j);
        free(j->buf);      /* 所有権はこちらに移っている */
        j->buf = NULL;
        break;
    case FSIO_COPY:
        e = fs_copy(j->from, j->to, NULL, NULL);
        finish(j, e == ESP_OK, e, NULL, 0);
        break;
    case FSIO_MOVE:
        e = fs_move(j->from, j->to);
        finish(j, e == ESP_OK, e, NULL, 0);
        break;
    case FSIO_REMOVE:
        e = fs_remove(j->from, false);
        finish(j, e == ESP_OK, e, NULL, 0);
        break;
    default:
        finish(j, false, ESP_ERR_NOT_SUPPORTED, NULL, 0);
        break;
    }
}

static void fsio_task(void *arg)
{
    fsio_job_t j;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE)
            continue;
        run_job(&j);
    }
}

bool fsio_start(void)
{
    if (s_started)
        return true;
    if (!s_q)
        s_q = xQueueCreate(FSIO_QUEUE_DEPTH, sizeof(fsio_job_t));
    if (!s_q)
        return false;
    if (xTaskCreatePinnedToCore(fsio_task, "fs_io", FSIO_STACK, NULL,
                                FSIO_PRIO, NULL, FSIO_CORE) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    s_started = true;
    return true;
}

bool fsio_submit(fsio_kind_t k, const char *from, const char *to,
                 void *buf, size_t len,
                 fsio_cb_t cb, fsio_progress_t prog, void *ctx)
{
    fsio_job_t j;

    if (!s_q || !from)
        return false;
    memset(&j, 0, sizeof j);
    j.kind = k;
    if (strlen(from) >= sizeof j.from)
        return false;
    strcpy(j.from, from);
    if (to) {
        if (strlen(to) >= sizeof j.to)
            return false;
        strcpy(j.to, to);
    }
    j.buf = buf;
    j.len = len;
    j.cb = cb;
    j.prog = prog;
    j.ctx = ctx;
    /* 待たない。満杯は呼び出し側が決めること (再投函するか、諦めるか)。 */
    return xQueueSend(s_q, &j, 0) == pdTRUE;
}
