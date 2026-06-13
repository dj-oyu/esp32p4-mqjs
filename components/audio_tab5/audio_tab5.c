/*
 * Tab5 speaker path: ES8388 via esp_codec_dev + I2S TX + PSRAM PCM ring.
 *
 * Hardware (same as the official m5stack_tab5 BSP / M5Tab5-UserDemo):
 *   - ES8388 on the shared internal I2C bus (ui_tab5_i2c_bus(), port 1,
 *     SDA=G31 SCL=G32) — no second master bus is created here.
 *   - I2S master TX: MCLK=G30 BCLK=G27 WS=G29 DOUT=G26 (DIN=G28 is the
 *     mic path, not used). 16-bit Philips stereo, MCLK = 256*fs.
 *   - No PA GPIO; the NS4150 amp is gated by SPK_EN = P1 of the PI4IOE
 *     expander at 0x43, which ui_tab5 already drives high at boot. We
 *     still read-modify-write it (never rewrite the whole register: P4
 *     LCD_RST / P5 TP_RST / P6 CAM_RST live in the same byte).
 *
 * Dataflow: audio_tab5_write() -> bounded byte ring (PSRAM) -> writer
 * task -> i2s_channel_write() -> ES8388. The I2S channel has auto_clear
 * set, so an underrun plays silence instead of stale DMA contents.
 */
#include "sdkconfig.h"

#if CONFIG_MQJS_TAB5_AUDIO

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "ui_tab5.h"
#include "audio_tab5.h"

#define AUD_I2S_MCLK GPIO_NUM_30
#define AUD_I2S_BCLK GPIO_NUM_27
#define AUD_I2S_WS   GPIO_NUM_29
#define AUD_I2S_DOUT GPIO_NUM_26

#define AUD_PI4IOE1_ADDR 0x43
#define AUD_PI4IOE_REG_OUT 0x05
#define AUD_SPK_EN_BIT (1u << 1) /* P1 */

#define AUD_RING_BYTES (CONFIG_MQJS_TAB5_AUDIO_RING_KB * 1024)
#define AUD_CHUNK_BYTES 2048 /* writer granularity: ~5.3 ms @48k stereo */

static const char *TAG = "audio5";

static SemaphoreHandle_t s_lock; /* guards init/reconfig vs writer/API */
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static i2s_chan_handle_t s_tx;
static esp_codec_dev_handle_t s_dev;
static RingbufHandle_t s_ring;
static TaskHandle_t s_task;
static bool s_inited;
static volatile bool s_running;
static uint32_t s_rate;
static int s_ch = 2;
static int s_vol = 70;
static volatile uint32_t s_underruns;
static volatile uint64_t s_frames_written;

/* ---- SPK_EN on the 0x43 expander, read-modify-write ---------------- */
static esp_err_t spk_enable(i2c_master_bus_handle_t bus, bool on)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AUD_PI4IOE1_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &dev), TAG,
                        "expander 0x43 add");
    const uint8_t reg = AUD_PI4IOE_REG_OUT;
    uint8_t out = 0;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &out, 1, 50);
    if (err == ESP_OK) {
        uint8_t want = on ? (out | AUD_SPK_EN_BIT)
                          : (out & (uint8_t)~AUD_SPK_EN_BIT);
        if (want != out) {
            uint8_t wr[2] = { reg, want };
            err = i2c_master_transmit(dev, wr, 2, 50);
        }
    }
    i2c_master_bus_rm_device(dev);
    return err;
}

/* ---- writer task ---------------------------------------------------- */
static void writer_task(void *arg)
{
    (void)arg;
    bool had_data = false;
    for (;;) {
        size_t len = 0;
        uint8_t *p = xRingbufferReceiveUpTo(s_ring, &len,
                                            pdMS_TO_TICKS(100),
                                            AUD_CHUNK_BYTES);
        if (p) {
            had_data = true;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            size_t written = 0;
            esp_err_t err = i2s_channel_write(s_tx, p, len, &written, 1000);
            xSemaphoreGive(s_lock);
            vRingbufferReturnItem(s_ring, p);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "i2s write: %s", esp_err_to_name(err));
            s_frames_written += written / 4; /* always 16-bit stereo */
        } else if (had_data) {
            /* ring drained while started: real underrun mid-stream,
               expected exactly once after the producer finishes */
            had_data = false;
            if (s_running)
                s_underruns++;
        }
    }
}

/* ---- bring-up / reconfig (s_lock held) ------------------------------ */
static esp_err_t i2s_apply_rate(uint32_t rate, bool first)
{
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate), /* MCLK = 256*fs */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUD_I2S_MCLK,
            .bclk = AUD_I2S_BCLK,
            .ws = AUD_I2S_WS,
            .dout = AUD_I2S_DOUT,
            .din = GPIO_NUM_NC,
        },
    };
    if (first)
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG,
                            "i2s std init");
    else
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx, &std.clk_cfg),
                            TAG, "i2s clk reconfig");
    return i2s_channel_enable(s_tx);
}

static esp_err_t codec_open(uint32_t rate)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = rate,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_dev, &fs), TAG, "codec open");
    return esp_codec_dev_set_out_vol(s_dev, s_vol);
}

static esp_err_t hw_init(uint32_t rate)
{
    i2c_master_bus_handle_t bus = ui_tab5_i2c_bus();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG,
                        "ui_tab5 I2C bus not up (start UI first)");

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                        I2S_ROLE_MASTER);
    chan.auto_clear = true; /* underrun -> silence, not stale DMA data */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, NULL), TAG, "i2s chan");
    ESP_RETURN_ON_ERROR(i2s_apply_rate(rate, true), TAG, "i2s rate");

    audio_codec_i2s_cfg_t i2s_if_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "i2s data if");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 1, /* informational; bus_handle wins */
        .addr = ES8388_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "i2c ctrl if");

    es8388_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC, /* amp is SPK_EN on the expander */
        .master_mode = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec_if = es8388_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8388 new");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dev, ESP_FAIL, TAG, "codec dev new");

    ESP_RETURN_ON_ERROR(codec_open(rate), TAG, "codec");
    ESP_RETURN_ON_ERROR(spk_enable(bus, true), TAG, "SPK_EN");

    s_ring = xRingbufferCreateWithCaps(AUD_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_ring, ESP_ERR_NO_MEM, TAG, "ring %d KB",
                        CONFIG_MQJS_TAB5_AUDIO_RING_KB);

    /* JS runs on core 0, LVGL on core 1; the writer mostly blocks on DMA
       so it rides core 1 above the UI without starving it */
    ESP_RETURN_ON_FALSE(
        xTaskCreatePinnedToCore(writer_task, "audio_tx", 4096, NULL, 10,
                                &s_task, 1) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "writer task");
    return ESP_OK;
}

/* ---- public API ------------------------------------------------------ */
esp_err_t audio_tab5_start(uint32_t sample_rate, int channels)
{
    if (channels < 1 || channels > 2)
        return ESP_ERR_INVALID_ARG;
    switch (sample_rate) {
    case 8000: case 12000: case 16000: case 24000: case 48000:
    case 44100: /* not an Opus rate but the hardware is fine with it */
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_lock) {
        SemaphoreHandle_t l = xSemaphoreCreateMutex();
        if (!l)
            return ESP_ERR_NO_MEM;
        /* two tasks may race the very first call */
        taskENTER_CRITICAL(&s_init_mux);
        if (!s_lock)
            s_lock = l;
        taskEXIT_CRITICAL(&s_init_mux);
        if (s_lock != l)
            vSemaphoreDelete(l);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_inited) {
        err = hw_init(sample_rate);
        s_inited = (err == ESP_OK);
    } else if (sample_rate != s_rate) {
        esp_codec_dev_close(s_dev);
        i2s_channel_disable(s_tx);
        err = i2s_apply_rate(sample_rate, false);
        if (err == ESP_OK)
            err = codec_open(sample_rate);
    }
    if (err == ESP_OK) {
        s_rate = sample_rate;
        s_ch = channels;
        s_running = true;
        ESP_LOGI(TAG, "start %lu Hz %dch vol=%d", (unsigned long)sample_rate,
                 channels, s_vol);
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* push one chunk of stereo bytes with a shared deadline */
static size_t ring_send(const void *bytes, size_t len, TickType_t deadline)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t wait = (deadline > now) ? (deadline - now) : 0;
    return xRingbufferSend(s_ring, bytes, len, wait) == pdTRUE ? len : 0;
}

size_t audio_tab5_write(const int16_t *pcm, size_t frames, uint32_t timeout_ms)
{
    if (!s_running || !pcm || !frames)
        return 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    size_t done = 0;
    if (s_ch == 2) {
        while (done < frames) {
            size_t n = frames - done;
            if (n > AUD_CHUNK_BYTES / 4)
                n = AUD_CHUNK_BYTES / 4;
            if (ring_send(pcm + done * 2, n * 4, deadline) == 0)
                break;
            done += n;
        }
    } else {
        int16_t st[256 * 2];
        while (done < frames) {
            size_t n = frames - done;
            if (n > 256)
                n = 256;
            for (size_t i = 0; i < n; i++) {
                st[2 * i] = pcm[done + i];
                st[2 * i + 1] = pcm[done + i];
            }
            if (ring_send(st, n * 4, deadline) == 0)
                break;
            done += n;
        }
    }
    return done;
}

esp_err_t audio_tab5_stop(void)
{
    if (!s_inited)
        return ESP_OK;
    s_running = false;
    /* let the writer drain what the producer already queued */
    for (int i = 0; i < 100; i++) {
        size_t queued = AUD_RING_BYTES - xRingbufferGetCurFreeSize(s_ring);
        if (queued == 0)
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

esp_err_t audio_tab5_set_volume(int pct)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    s_vol = pct;
    return s_dev ? esp_codec_dev_set_out_vol(s_dev, pct) : ESP_OK;
}

int audio_tab5_volume(void)
{
    return s_vol;
}

void audio_tab5_get_stats(audio_tab5_stats_t *out)
{
    if (!out)
        return;
    *out = (audio_tab5_stats_t){
        .running = s_running,
        .sample_rate = s_rate,
        .channels = (uint8_t)s_ch,
        .queued_bytes = s_ring
            ? (uint32_t)(AUD_RING_BYTES - xRingbufferGetCurFreeSize(s_ring))
            : 0,
        .underruns = s_underruns,
        .frames_written = s_frames_written,
    };
}

esp_err_t audio_tab5_tone(int freq_hz, int duration_ms)
{
    if (freq_hz <= 0 || duration_ms <= 0)
        return ESP_ERR_INVALID_ARG;
    if (!s_running)
        ESP_RETURN_ON_ERROR(audio_tab5_start(48000, 2), TAG, "tone start");

    const float amp = 0.25f * 32767.0f;
    const float step = 2.0f * (float)M_PI * (float)freq_hz / (float)s_rate;
    size_t total = (size_t)s_rate * (size_t)duration_ms / 1000;
    int16_t buf[240 * 2];
    float phase = 0.0f;
    int producer_ch = s_ch;
    for (size_t off = 0; off < total;) {
        size_t n = total - off;
        if (n > 240)
            n = 240;
        for (size_t i = 0; i < n; i++) {
            int16_t v = (int16_t)(amp * sinf(phase));
            phase += step;
            if (phase > 2.0f * (float)M_PI)
                phase -= 2.0f * (float)M_PI;
            if (producer_ch == 2) {
                buf[2 * i] = v;
                buf[2 * i + 1] = v;
            } else {
                buf[i] = v;
            }
        }
        size_t sent = audio_tab5_write(buf, n, 1000);
        if (sent == 0)
            return ESP_ERR_TIMEOUT;
        off += sent;
    }
    return ESP_OK;
}

/* ---- async tone (JS audio.tone): never block the caller ------------- */
/* The blocking tone above can hold the caller for hundreds of ms (two
 * 300 ms tones outlast the 64 KB ring), which would stall the JS event
 * loop / trip the watchdog. JS calls this instead: it spawns a one-shot
 * task, guarded so a second call while one is sounding is rejected. */
static volatile bool s_tone_busy;

typedef struct {
    int freq_hz;
    int duration_ms;
} tone_req_t;

static void tone_task(void *arg)
{
    tone_req_t req = *(tone_req_t *)arg;
    free(arg);
    esp_err_t err = audio_tab5_tone(req.freq_hz, req.duration_ms);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "tone %dHz/%dms: %s", req.freq_hz, req.duration_ms,
                 esp_err_to_name(err));
    s_tone_busy = false;
    vTaskDelete(NULL);
}

bool audio_tab5_tone_async(int freq_hz, int duration_ms)
{
    if (freq_hz <= 0 || duration_ms <= 0 || duration_ms > 5000)
        return false;
    if (s_tone_busy)
        return false;
    tone_req_t *req = malloc(sizeof *req);
    if (!req)
        return false;
    req->freq_hz = freq_hz;
    req->duration_ms = duration_ms;
    s_tone_busy = true;
    if (xTaskCreate(tone_task, "audio_tone", 4096, req, 6, NULL) != pdPASS) {
        s_tone_busy = false;
        free(req);
        return false;
    }
    return true;
}

/* ---- boot self-test (P2 gate) ---------------------------------------- */
static void selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000)); /* let UI / Wi-Fi bring-up settle */
    esp_err_t err = audio_tab5_start(48000, 2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SELFTEST start failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    audio_tab5_set_volume(70);
    err = audio_tab5_tone(880, 300);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        err = audio_tab5_tone(1319, 300);
    }
    audio_tab5_stop();
    audio_tab5_stats_t st;
    audio_tab5_get_stats(&st);
    ESP_LOGI(TAG,
             "SELFTEST %s: frames=%llu underruns=%lu (1 expected: the gap "
             "between the tones) queued=%lu",
             err == ESP_OK ? "done" : esp_err_to_name(err),
             (unsigned long long)st.frames_written,
             (unsigned long)st.underruns, (unsigned long)st.queued_bytes);
    vTaskDelete(NULL);
}

void audio_tab5_selftest_async(void)
{
    xTaskCreate(selftest_task, "audio_st", 4096, NULL, 5, NULL);
}

#endif /* CONFIG_MQJS_TAB5_AUDIO */
