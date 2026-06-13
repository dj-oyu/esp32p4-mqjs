/*
 * audio_device FreeRTOS wrapper: a manager task owns the pure core state
 * machine (audio_device_core); producers talk to it through a command
 * queue. The core decides grants/queueing/revocation and returns an
 * ordered action list; the manager executes those actions (drive
 * audio_tab5, fire producer callbacks) off the lock.
 *
 * Locking:
 *   - s_lock guards the core. Held only for the (short) core calls and the
 *     write-path token check — never across a blocking audio_tab5 call.
 *   - Backend effects (audio_tab5_start/stop) and producer callbacks run on
 *     the manager task with no lock held, so a 1 s drain never stalls a
 *     concurrent audio_stream_write (which only takes s_lock to compare a
 *     generation).
 */
#include "sdkconfig.h"

#if CONFIG_MQJS_TAB5_AUDIO

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "audio_device.h"
#include "audio_tab5.h"

static const char *TAG = "audev";

#define AUD_CMDQ_DEPTH 16
/* Per-stream overdub source ring: the mixer drains it at real time, so the
   producer's audio_stream_write paces itself via backpressure. ~85 ms at
   48 kHz stereo is plenty of slack to stay fed. */
#define AUD_SRC_RING_BYTES (16 * 1024)
#define MIX_FRAMES 240 /* mixer granularity, ~5 ms @48k */

typedef enum {
    CMD_REQUEST,
    CMD_CANCEL_REQ,
    CMD_FINISH,
    CMD_ABORT,
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    audio_request_id_t id;   /* REQUEST / CANCEL_REQ */
    audio_token_t token;     /* FINISH / ABORT */
    audio_format_t format;   /* REQUEST */
    audio_request_policy_t policy;
    audio_granted_cb_t on_granted;
    audio_cancelled_cb_t on_cancelled;
    void *arg;
} dev_cmd_t;

static audio_dev_core_t s_core;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmdq;
static TaskHandle_t s_task;
static volatile bool s_ready;
static volatile bool s_init_failed;
static bool s_init_started; /* guarded by s_init_mux */
static portMUX_TYPE s_id_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_next_id = 1;

/* ---- overdub mixer (P3) --------------------------------------------- */
/* One source ring per live MIXING stream. The mixer task sums them into
   the master path (audio_tab5). s_mix_lock guards the table; producers
   only fetch a ring handle under it, then send outside it (a ring is freed
   only by the mixer, and only after the producer has finished writing). */
typedef struct {
    bool used;
    uint32_t generation;
    RingbufHandle_t ring;
    bool started;            /* has primed PRIME_FRAMES and joined the sum */
    volatile bool finishing; /* drain remaining audio, then drop */
} mix_src_t;

/* A source contributes silence (and does not gate the mix) until it has
   buffered this much — so a freshly added stream ramps in without forcing
   the others to wait, and a synchronized onset waits for everyone to prime
   instead of chopping the fast ones. */
#define PRIME_FRAMES (MIX_FRAMES * 2)

static mix_src_t s_src[AUDIO_DEV_MAX_STREAMS];
static SemaphoreHandle_t s_mix_lock;
static TaskHandle_t s_mixer;
static audio_format_t s_mix_fmt;
static volatile bool s_mix_run;      /* mixer loop alive */
static volatile bool s_mix_stop_req; /* drain everything, then exit */

/* Pull up to MIX_FRAMES frames (4 bytes each, stereo s16) from a source
   ring into dst, coping with byte-buffer wrap by looping. Returns frames. */
static size_t mix_gather(RingbufHandle_t ring, int16_t *dst, size_t max_frames)
{
    size_t got = 0;
    while (got < max_frames) {
        size_t n = 0;
        void *p = xRingbufferReceiveUpTo(ring, &n, 0,
                                         (max_frames - got) * 4);
        if (!p)
            break;
        memcpy((uint8_t *)dst + got * 4, p, n);
        vRingbufferReturnItem(ring, p);
        got += n / 4; /* producers always send whole stereo frames */
    }
    return got;
}

/* Frames currently buffered in a source ring (4 bytes/frame, stereo s16). */
static size_t ring_avail_frames(RingbufHandle_t ring)
{
    size_t freeb = xRingbufferGetCurFreeSize(ring);
    size_t used = (freeb < AUD_SRC_RING_BYTES) ? AUD_SRC_RING_BYTES - freeb : 0;
    return used / 4;
}

/* Single mixer instance -> static scratch keeps it off the task stack. */
static int32_t s_mix_acc[MIX_FRAMES * 2];
static int16_t s_mix_tmp[MIX_FRAMES * 2];
static int16_t s_mix_out[MIX_FRAMES * 2];

static void mixer_task(void *arg)
{
    (void)arg;
    while (s_mix_run) {
        /* Snapshot the source table; do the ring work off the lock (rings
           are internally thread-safe, the lock only guards the table). */
        struct {
            int idx;
            RingbufHandle_t ring;
            bool finishing;
        } live[AUDIO_DEV_MAX_STREAMS];
        int n = 0;
        xSemaphoreTake(s_mix_lock, portMAX_DELAY);
        for (int i = 0; i < AUDIO_DEV_MAX_STREAMS; i++) {
            if (!s_src[i].used)
                continue;
            live[n].idx = i;
            live[n].ring = s_src[i].ring;
            live[n].finishing = s_src[i].finishing;
            n++;
        }
        xSemaphoreGive(s_mix_lock);

        if (n == 0) {
            if (s_mix_stop_req)
                break;
            vTaskDelay(1); /* session opening but no source yet */
            continue;
        }

        /* Decide a single frame count to take from every contributor, so no
           live source is ever zero-padded (that chopping is the onset
           noise). A source joins only once primed; a finishing source
           drains whatever it has left. */
        bool contributes[AUDIO_DEV_MAX_STREAMS] = { false };
        size_t out = MIX_FRAMES;
        int ncontrib = 0;
        for (int k = 0; k < n; k++) {
            size_t avail = ring_avail_frames(live[k].ring);
            if (live[k].finishing) {
                if (avail == 0)
                    continue; /* drained — dropped below */
                contributes[k] = true;
                ncontrib++;
                if (avail < out)
                    out = avail;
            } else {
                if (!s_src[live[k].idx].started) {
                    if (avail >= PRIME_FRAMES)
                        s_src[live[k].idx].started = true;
                    else
                        continue; /* still priming: silent, does not gate */
                }
                contributes[k] = true;
                ncontrib++;
                if (avail < out)
                    out = avail; /* starved live source: keep alignment */
            }
        }

        /* Reap finished + fully-drained sources. */
        xSemaphoreTake(s_mix_lock, portMAX_DELAY);
        for (int k = 0; k < n; k++) {
            if (live[k].finishing &&
                ring_avail_frames(live[k].ring) == 0 &&
                s_src[live[k].idx].used) {
                vRingbufferDeleteWithCaps(s_src[live[k].idx].ring);
                s_src[live[k].idx].used = false;
                s_src[live[k].idx].ring = NULL;
            }
        }
        xSemaphoreGive(s_mix_lock);

        if (ncontrib == 0 || out == 0) {
            vTaskDelay(1); /* all priming, or momentarily starved */
            continue;
        }

        memset(s_mix_acc, 0, out * 2 * sizeof s_mix_acc[0]);
        for (int k = 0; k < n; k++) {
            if (!contributes[k])
                continue;
            size_t got = mix_gather(live[k].ring, s_mix_tmp, out);
            for (size_t j = 0; j < got * 2; j++)
                s_mix_acc[j] += s_mix_tmp[j];
        }
        for (size_t j = 0; j < out * 2; j++) {
            int32_t v = s_mix_acc[j];
            if (v > 32767)
                v = 32767;
            else if (v < -32768)
                v = -32768;
            s_mix_out[j] = (int16_t)v;
        }
        audio_tab5_write(s_mix_out, out, 1000);
    }
    s_mix_run = false;
    vTaskDelete(NULL);
}

static void mixer_start(const audio_format_t *fmt)
{
    s_mix_fmt = *fmt;
    for (int i = 0; i < AUDIO_DEV_MAX_STREAMS; i++)
        s_src[i] = (mix_src_t){ 0 };
    s_mix_stop_req = false;
    s_mix_run = true;
    if (xTaskCreatePinnedToCore(mixer_task, "audio_mix", 6144, NULL, 9,
                                &s_mixer, 1) != pdPASS) {
        s_mix_run = false;
        ESP_LOGE(TAG, "mixer task create failed");
    }
}

static void mixer_add_source(uint32_t slot, uint32_t generation)
{
    if (slot >= AUDIO_DEV_MAX_STREAMS)
        return;
    RingbufHandle_t ring = xRingbufferCreateWithCaps(
        AUD_SRC_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring) {
        ESP_LOGE(TAG, "source ring alloc failed (slot %lu)",
                 (unsigned long)slot);
        return; /* writes for this token will fail with INVALID_STATE */
    }
    xSemaphoreTake(s_mix_lock, portMAX_DELAY);
    s_src[slot].ring = ring;
    s_src[slot].generation = generation;
    s_src[slot].finishing = false;
    s_src[slot].started = false; /* reused slot index: re-prime from scratch */
    s_src[slot].used = true;
    xSemaphoreGive(s_mix_lock);
}

static void mixer_mark_finishing(uint32_t slot)
{
    if (slot >= AUDIO_DEV_MAX_STREAMS)
        return;
    xSemaphoreTake(s_mix_lock, portMAX_DELAY);
    if (s_src[slot].used)
        s_src[slot].finishing = true;
    xSemaphoreGive(s_mix_lock);
}

static void mixer_stop(void)
{
    /* Drain every remaining source, then let the loop exit. */
    xSemaphoreTake(s_mix_lock, portMAX_DELAY);
    for (int i = 0; i < AUDIO_DEV_MAX_STREAMS; i++)
        if (s_src[i].used)
            s_src[i].finishing = true;
    xSemaphoreGive(s_mix_lock);

    s_mix_stop_req = true;
    while (s_mix_run) /* mixer pushes the drained tail before clearing this */
        vTaskDelay(1);
    s_mixer = NULL;
}

/* ---- action execution (manager task, no lock held) ------------------ */
static void run_actions(const audio_action_list_t *acts)
{
    for (int i = 0; i < acts->n; i++) {
        const audio_action_t *a = &acts->a[i];
        switch (a->kind) {
        case AUDIO_ACT_BACKEND_START: {
            esp_err_t err = audio_tab5_start(a->u.fmt.sample_rate,
                                             a->u.fmt.channels);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "backend start %luHz/%uch: %s",
                         (unsigned long)a->u.fmt.sample_rate,
                         a->u.fmt.channels, esp_err_to_name(err));
            break;
        }
        case AUDIO_ACT_BACKEND_STOP:
        case AUDIO_ACT_BACKEND_FLUSH:
            /* No discard primitive yet: stop() drains the ring, leaving it
               empty for the next session either way. FLUSH vs STOP differ
               only in latency, which a real flush API (P2) will address. */
            audio_tab5_stop();
            break;
        case AUDIO_ACT_MIXER_START:
            mixer_start(&a->u.fmt);
            break;
        case AUDIO_ACT_MIXER_REMOVE:
            mixer_mark_finishing(a->u.slot);
            break;
        case AUDIO_ACT_MIXER_STOP:
            mixer_stop(); /* blocks until the drained tail reaches the master */
            break;
        case AUDIO_ACT_FIRE_GRANTED:
            /* Set up the source ring BEFORE handing the producer its token,
               so its first write has somewhere to go. */
            if (a->u.grant.mixing)
                mixer_add_source(a->u.grant.token.slot,
                                 a->u.grant.token.generation);
            if (a->u.grant.cb)
                a->u.grant.cb(a->u.grant.req, a->u.grant.token, a->u.grant.arg);
            break;
        case AUDIO_ACT_FIRE_CANCELLED:
            if (a->u.cancel.cb)
                a->u.cancel.cb(a->u.cancel.req, a->u.cancel.arg);
            break;
        }
    }
}

/* ---- manager task --------------------------------------------------- */
static void manager_task(void *arg)
{
    (void)arg;
    dev_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmdq, &cmd, portMAX_DELAY) != pdTRUE)
            continue;

        audio_action_list_t acts;
        memset(&acts, 0, sizeof acts);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        switch (cmd.kind) {
        case CMD_REQUEST:
            audio_dev_core_submit(&s_core, cmd.id, &cmd.format, &cmd.policy,
                                  cmd.on_granted, cmd.on_cancelled, cmd.arg,
                                  &acts);
            break;
        case CMD_CANCEL_REQ:
            audio_dev_core_cancel_request(&s_core, cmd.id, &acts);
            break;
        case CMD_FINISH:
            audio_dev_core_finish(&s_core, cmd.token, &acts);
            break;
        case CMD_ABORT:
            audio_dev_core_abort(&s_core, cmd.token, &acts);
            break;
        }
        xSemaphoreGive(s_lock);

        run_actions(&acts); /* backend + callbacks, off the lock */
    }
}

/* ---- init ----------------------------------------------------------- */
esp_err_t audio_device_init(void)
{
    if (s_ready)
        return ESP_OK;

    /* Elect a single initializer; later callers wait for it. (xSemaphore /
       xTaskCreate cannot run inside a critical section, so only the flag
       flip is protected here.) */
    taskENTER_CRITICAL(&s_init_mux);
    bool first = !s_init_started;
    s_init_started = true;
    taskEXIT_CRITICAL(&s_init_mux);
    if (!first) {
        while (!s_ready && !s_init_failed)
            vTaskDelay(1);
        return s_ready ? ESP_OK : ESP_ERR_NO_MEM;
    }

    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    SemaphoreHandle_t mlock = xSemaphoreCreateMutex();
    QueueHandle_t q = xQueueCreate(AUD_CMDQ_DEPTH, sizeof(dev_cmd_t));
    if (lock && mlock && q) {
        audio_dev_core_init(&s_core);
        s_lock = lock;
        s_mix_lock = mlock;
        s_cmdq = q;
        /* core 1 alongside the audio writer; below the writer (10) so PCM
           DMA feeding is never starved by control work. */
        if (xTaskCreatePinnedToCore(manager_task, "audio_mgr", 4096, NULL, 8,
                                    &s_task, 1) == pdPASS) {
            s_ready = true; /* publish last: writers gate on this */
            ESP_LOGI(TAG, "audio_device up");
            return ESP_OK;
        }
    }

    if (lock)
        vSemaphoreDelete(lock);
    if (mlock)
        vSemaphoreDelete(mlock);
    if (q)
        vQueueDelete(q);
    s_lock = NULL;
    s_mix_lock = NULL;
    s_cmdq = NULL;
    s_init_failed = true; /* one-shot: audio_device stays disabled this boot */
    ESP_LOGE(TAG, "audio_device init failed (out of memory)");
    return ESP_ERR_NO_MEM;
}

static audio_request_id_t reserve_id(void)
{
    taskENTER_CRITICAL(&s_id_mux);
    uint32_t id = s_next_id++;
    if (s_next_id == 0)
        s_next_id = 1; /* 0 is the invalid id */
    taskEXIT_CRITICAL(&s_id_mux);
    return id;
}

/* ---- public API ----------------------------------------------------- */
audio_request_id_t audio_request(const audio_format_t *format,
                                 const audio_request_policy_t *policy,
                                 audio_granted_cb_t on_granted,
                                 audio_cancelled_cb_t on_cancelled, void *arg)
{
    if (!format || !policy)
        return 0;
    if (audio_device_init() != ESP_OK)
        return 0;

    audio_request_id_t id = reserve_id();
    dev_cmd_t cmd = {
        .kind = CMD_REQUEST,
        .id = id,
        .format = *format,
        .policy = *policy,
        .on_granted = on_granted,
        .on_cancelled = on_cancelled,
        .arg = arg,
    };
    if (xQueueSend(s_cmdq, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full; request dropped");
        return 0; /* never submitted: no callback will fire */
    }
    return id;
}

bool audio_request_cancel(audio_request_id_t request)
{
    if (!request || !s_ready)
        return false;
    dev_cmd_t cmd = { .kind = CMD_CANCEL_REQ, .id = request };
    return xQueueSend(s_cmdq, &cmd, 0) == pdTRUE;
}

esp_err_t audio_stream_write(audio_token_t token, const int16_t *pcm,
                             size_t frames, uint32_t timeout_ms,
                             size_t *accepted)
{
    if (accepted)
        *accepted = 0;
    if (!s_ready)
        return ESP_ERR_INVALID_STATE;
    if (!pcm || !frames)
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool live = audio_dev_core_token_active(&s_core, token);
    bool mixing = live && audio_dev_core_token_is_mixing(&s_core, token);
    xSemaphoreGive(s_lock);
    if (!live)
        return ESP_ERR_INVALID_STATE; /* stale/unknown token: no PCM consumed */

    if (mixing) {
        /* Route to this stream's source ring; the mixer sums it in. Fetch
           the handle under the lock, send outside it (the ring is only
           freed by the mixer, after this producer has finished). */
        xSemaphoreTake(s_mix_lock, portMAX_DELAY);
        RingbufHandle_t ring = NULL;
        if (token.slot < AUDIO_DEV_MAX_STREAMS && s_src[token.slot].used &&
            s_src[token.slot].generation == token.generation)
            ring = s_src[token.slot].ring;
        xSemaphoreGive(s_mix_lock);
        if (!ring)
            return ESP_ERR_INVALID_STATE;

        size_t bytes = frames * (size_t)s_mix_fmt.channels * 2;
        if (xRingbufferSend(ring, pcm, bytes, pdMS_TO_TICKS(timeout_ms)) ==
            pdTRUE) {
            if (accepted)
                *accepted = frames;
        }
        return ESP_OK; /* accepted stays 0 on backpressure timeout */
    }

    size_t n = audio_tab5_write(pcm, frames, timeout_ms);
    if (accepted)
        *accepted = n;
    return ESP_OK;
}

static esp_err_t end_stream(audio_token_t token, cmd_kind_t kind)
{
    if (!s_ready)
        return ESP_ERR_INVALID_STATE;
    /* Fast reject of an obviously dead token; the manager re-validates. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool live = audio_dev_core_token_active(&s_core, token);
    xSemaphoreGive(s_lock);
    if (!live)
        return ESP_ERR_INVALID_STATE;

    dev_cmd_t cmd = { .kind = kind, .token = token };
    return xQueueSend(s_cmdq, &cmd, portMAX_DELAY) == pdTRUE
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t audio_stream_finish(audio_token_t token)
{
    return end_stream(token, CMD_FINISH);
}

esp_err_t audio_stream_abort(audio_token_t token)
{
    return end_stream(token, CMD_ABORT);
}

bool audio_token_valid(audio_token_t token)
{
    if (!s_ready)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool live = audio_dev_core_token_active(&s_core, token);
    xSemaphoreGive(s_lock);
    return live;
}

#endif /* CONFIG_MQJS_TAB5_AUDIO */
