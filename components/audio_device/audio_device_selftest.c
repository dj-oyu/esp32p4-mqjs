/*
 * Boot-time device self-test for the audio_device P1 token layer. Runs a
 * sequence of scenarios through the public API only (audio_request ->
 * audio_stream_write -> audio_stream_finish / audio_request_cancel), each
 * audible as a short beep, so the token grant / queue / cancel / dispatch
 * behaviour can be heard and read on the serial log without touching
 * audio_tab5 directly.
 *
 *   S1 EXCLUSIVE+QUEUE : A granted now, B queued -> two beeps in order.
 *   S2 CANCELABLE      : C active, D queued then cancelled -> one beep
 *                        (D never sounds; its on_cancelled fires).
 *   S3 OVERDUB         : E active, F requests OVERDUB. P1 has no mixer
 *                        (that is P3), so OVERDUB falls back to QUEUE and
 *                        F plays AFTER E -> two beeps in order, not mixed.
 *
 * The audible signature is 2 + 1 + 2 = five beeps in three groups.
 */
#include "sdkconfig.h"

#if CONFIG_MQJS_TAB5_AUDIO && CONFIG_MQJS_TAB5_AUDIO_DEVICE_SELFTEST

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "audio_device.h"
#include "audio_tab5.h"

static const char *TAG = "audev-st";

typedef struct {
    const char *name;
    int freq_hz;
    int duration_ms;
    audio_format_t format;
    SemaphoreHandle_t sem; /* given on grant OR cancel */
    audio_token_t token;
    audio_request_id_t req;
    volatile bool granted;
    volatile bool cancelled;
} beep_t;

static void on_granted(audio_request_id_t req, audio_token_t token, void *arg)
{
    beep_t *b = arg;
    b->token = token;
    b->granted = true;
    ESP_LOGI(TAG, "%s GRANTED req=%lu slot=%lu gen=%lu", b->name,
             (unsigned long)req, (unsigned long)token.slot,
             (unsigned long)token.generation);
    xSemaphoreGive(b->sem);
}

static void on_cancelled(audio_request_id_t req, void *arg)
{
    beep_t *b = arg;
    b->cancelled = true;
    ESP_LOGW(TAG, "%s CANCELLED req=%lu", b->name, (unsigned long)req);
    xSemaphoreGive(b->sem);
}

/* Generate a faded sine and stream it through the granted token. Phase is
 * derived from the absolute sample index so a partial (backpressured) write
 * never introduces a discontinuity. */
static esp_err_t play_beep(beep_t *b)
{
    const uint32_t rate = b->format.sample_rate;
    const int ch = b->format.channels;
    const float amp = 0.25f * 32767.0f;
    const float w = 2.0f * (float)M_PI * (float)b->freq_hz / (float)rate;
    const size_t total = (size_t)rate * (size_t)b->duration_ms / 1000;
    size_t fade = rate / 200; /* ~5 ms declick */
    if (fade > total / 2)
        fade = total / 2;

    int16_t buf[240 * 2];
    size_t pos = 0;
    while (pos < total) {
        size_t n = total - pos;
        if (n > 240)
            n = 240;
        for (size_t i = 0; i < n; i++) {
            size_t p = pos + i;
            float env = 1.0f;
            if (fade > 0) {
                if (p < fade)
                    env = (float)p / (float)fade;
                else if (p >= total - fade)
                    env = (float)(total - 1 - p) / (float)fade;
            }
            int16_t v = (int16_t)(amp * env * sinf(w * (float)p));
            if (ch == 2) {
                buf[2 * i] = v;
                buf[2 * i + 1] = v;
            } else {
                buf[i] = v;
            }
        }
        size_t acc = 0;
        esp_err_t err = audio_stream_write(b->token, buf, n, 1000, &acc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s write rejected: %s", b->name,
                     esp_err_to_name(err));
            return err;
        }
        if (acc == 0) {
            ESP_LOGE(TAG, "%s write timed out", b->name);
            return ESP_ERR_TIMEOUT;
        }
        pos += acc;
    }
    return ESP_OK;
}

static void beep_init(beep_t *b, const char *name, int freq, int ms)
{
    b->name = name;
    b->freq_hz = freq;
    b->duration_ms = ms;
    b->format.sample_rate = 48000;
    b->format.channels = 2;
    b->token = (audio_token_t){ 0, 0 };
    b->req = 0;
    b->granted = false;
    b->cancelled = false;
    /* sem is created once by the caller and reused. */
}

static audio_request_id_t beep_submit(beep_t *b,
                                      const audio_request_policy_t *pol)
{
    b->granted = false;
    b->cancelled = false;
    b->req = audio_request(&b->format, pol, on_granted, on_cancelled, b);
    return b->req;
}

/* Wait for grant/cancel, then (if granted) play and finish. */
static void beep_play_if_granted(beep_t *b, uint32_t wait_ms)
{
    if (xSemaphoreTake(b->sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "%s no grant/cancel within %lums", b->name,
                 (unsigned long)wait_ms);
        return;
    }
    if (!b->granted)
        return; /* cancelled: silent by design */
    esp_err_t err = play_beep(b);
    esp_err_t fin = audio_stream_finish(b->token);
    ESP_LOGI(TAG, "%s played=%s finished=%s", b->name, esp_err_to_name(err),
             esp_err_to_name(fin));
}

static const audio_request_policy_t POL_QUEUE = {
    .conflict = AUDIO_CONFLICT_QUEUE, .cancelable = true, .priority = 10
};
static const audio_request_policy_t POL_OVERDUB = {
    .conflict = AUDIO_CONFLICT_OVERDUB, .cancelable = true, .priority = 10
};

/* One concurrent player per chord note: each waits for its own grant, then
 * streams its tone into its source ring while the mixer sums all of them. */
typedef struct {
    beep_t *note;
    SemaphoreHandle_t done;
} chord_player_t;

static void chord_task(void *arg)
{
    chord_player_t *cp = arg;
    beep_play_if_granted(cp->note, 6000);
    xSemaphoreGive(cp->done);
    vTaskDelete(NULL);
}

static void selftest_task(void *arg)
{
    (void)arg;
    if (audio_device_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio_device_init failed");
        vTaskDelete(NULL);
        return;
    }
    audio_tab5_set_volume(70);

    static beep_t a, b, c, d, e, f;
    SemaphoreHandle_t sems[6];
    for (int i = 0; i < 6; i++) {
        sems[i] = xSemaphoreCreateBinary();
        if (!sems[i]) {
            ESP_LOGE(TAG, "no memory for semaphores");
            vTaskDelete(NULL);
            return;
        }
    }
    a.sem = sems[0]; b.sem = sems[1]; c.sem = sems[2];
    d.sem = sems[3]; e.sem = sems[4]; f.sem = sems[5];

    /* ---- S1: EXCLUSIVE + QUEUE -> two beeps in order --------------- */
    ESP_LOGI(TAG, "S1 EXCLUSIVE+QUEUE: A granted now, B queues");
    beep_init(&a, "S1.A", 880, 300);
    beep_init(&b, "S1.B", 1319, 300);
    beep_submit(&a, &POL_QUEUE);
    beep_submit(&b, &POL_QUEUE); /* queues behind A */
    beep_play_if_granted(&a, 5000);
    beep_play_if_granted(&b, 5000); /* dispatched only after A finishes */
    ESP_LOGI(TAG, "S1 result: A.granted=%d B.granted=%d (expect 1/1)",
             a.granted, b.granted);
    vTaskDelay(pdMS_TO_TICKS(400));

    /* ---- S2: CANCELABLE -> queued request withdrawn, one beep ------ */
    ESP_LOGI(TAG, "S2 CANCELABLE: C active, D queued then cancelled");
    beep_init(&c, "S2.C", 988, 300);
    beep_init(&d, "S2.D", 1319, 300);
    beep_submit(&c, &POL_QUEUE);
    audio_request_id_t rd = beep_submit(&d, &POL_QUEUE); /* queues behind C */
    /* FIFO command queue guarantees D is enqueued before this cancel runs,
       even though we only wait for C's grant first. */
    if (xSemaphoreTake(c.sem, pdMS_TO_TICKS(5000)) == pdTRUE && c.granted) {
        audio_request_cancel(rd); /* withdraw the still-queued D */
        esp_err_t err = play_beep(&c);
        esp_err_t fin = audio_stream_finish(c.token);
        ESP_LOGI(TAG, "S2.C played=%s finished=%s", esp_err_to_name(err),
                 esp_err_to_name(fin));
    } else {
        ESP_LOGE(TAG, "S2.C never granted");
    }
    /* D should arrive as a cancellation, not a grant. */
    if (xSemaphoreTake(d.sem, pdMS_TO_TICKS(2000)) != pdTRUE)
        ESP_LOGE(TAG, "S2.D neither granted nor cancelled");
    ESP_LOGI(TAG, "S2 result: D.cancelled=%d D.granted=%d (expect 1/0)",
             d.cancelled, d.granted);
    vTaskDelay(pdMS_TO_TICKS(400));

    /* ---- S3: OVERDUB -> P1 fallback to QUEUE (no mixer until P3) ---- */
    ESP_LOGI(TAG, "S3 OVERDUB: E active, F requests OVERDUB (P1->QUEUE)");
    beep_init(&e, "S3.E", 880, 300);
    beep_init(&f, "S3.F", 1568, 300);
    beep_submit(&e, &POL_QUEUE);
    beep_submit(&f, &POL_OVERDUB); /* OVERDUB has no compatible session in
                                      P1 -> queued, plays after E, not mixed */
    beep_play_if_granted(&e, 5000);
    beep_play_if_granted(&f, 5000);
    ESP_LOGI(TAG, "S3 result: E.granted=%d F.granted=%d (sequential)",
             e.granted, f.granted);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* ---- S4: OVERDUB CHORD -> all three struck at once ------------- */
    /* Simultaneous onset is the hard case for the mixer: the per-stream
       prime gate + min-frame mixing (see audio_device.c) lets all three
       enter together cleanly instead of chopping the rings that fill
       fastest. Hold the full triad ~2 s. */
    ESP_LOGI(TAG, "S4 OVERDUB CHORD: C5+E5+G5 struck together, hold ~2s");
    static beep_t n1, n2, n3;
    SemaphoreHandle_t nsem[3];
    SemaphoreHandle_t done = xSemaphoreCreateCounting(3, 0);
    bool ok = (done != NULL);
    for (int i = 0; i < 3 && ok; i++) {
        nsem[i] = xSemaphoreCreateBinary();
        ok = ok && nsem[i];
    }
    if (ok) {
        beep_init(&n1, "S4.C5", 523, 2000); n1.sem = nsem[0];
        beep_init(&n2, "S4.E5", 659, 2000); n2.sem = nsem[1];
        beep_init(&n3, "S4.G5", 784, 2000); n3.sem = nsem[2];
        static chord_player_t cp1, cp2, cp3;
        cp1 = (chord_player_t){ &n1, done };
        cp2 = (chord_player_t){ &n2, done };
        cp3 = (chord_player_t){ &n3, done };
        /* First OVERDUB opens the MIXING session; the other two join it, so
           all three are granted at once and start together. */
        beep_submit(&n1, &POL_OVERDUB);
        beep_submit(&n2, &POL_OVERDUB);
        beep_submit(&n3, &POL_OVERDUB);
        xTaskCreatePinnedToCore(chord_task, "chordC5", 4096, &cp1, 6, NULL, 0);
        xTaskCreatePinnedToCore(chord_task, "chordE5", 4096, &cp2, 6, NULL, 1);
        xTaskCreatePinnedToCore(chord_task, "chordG5", 4096, &cp3, 6, NULL, 0);

        for (int i = 0; i < 3; i++)
            xSemaphoreTake(done, pdMS_TO_TICKS(12000));
        ESP_LOGI(TAG,
                 "S4 result: notes granted C5=%d E5=%d G5=%d (all 1 => "
                 "simultaneous chord)",
                 n1.granted, n2.granted, n3.granted);
        vSemaphoreDelete(done);
        for (int i = 0; i < 3; i++)
            vSemaphoreDelete(nsem[i]);
    } else {
        ESP_LOGE(TAG, "S4 setup out of memory");
    }

    audio_tab5_stats_t st;
    audio_tab5_get_stats(&st);
    ESP_LOGI(TAG,
             "SELFTEST done: frames=%llu underruns=%lu queued=%lu",
             (unsigned long long)st.frames_written, (unsigned long)st.underruns,
             (unsigned long)st.queued_bytes);

    for (int i = 0; i < 6; i++)
        vSemaphoreDelete(sems[i]);
    vTaskDelete(NULL);
}

void audio_device_selftest_async(void)
{
    xTaskCreatePinnedToCore(selftest_task, "audev_st", 4096, NULL, 6, NULL, 0);
}

#endif /* CONFIG_MQJS_TAB5_AUDIO && CONFIG_MQJS_TAB5_AUDIO_DEVICE_SELFTEST */
