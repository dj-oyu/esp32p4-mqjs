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
#include "wav.h"

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
static volatile bool s_wav_playing;
static volatile bool s_wav_abort;
/* fold stereo to (L+R)/2 mono. Default true: the only output we drive is
   the mono speaker (NS4150B). Set false for true-stereo routing. */
static bool s_downmix = true;

/* rolling window of the mono signal sent to the speaker, for the on-demand
   FFT analyzer (audio_tab5_spectrum). Written by the producer, read on the
   js_task; a torn read only yields a slightly stale window — fine for a
   visualizer. AUD_FFT_N is a power of two so the index wraps with a mask. */
#define AUD_FFT_N 512
static float s_fft_buf[AUD_FFT_N];
static volatile uint32_t s_fft_wr;
/* digital peak meter on the downmixed mono signal — proves whether the
   (L+R)/2 fold ever rails (it cannot, mathematically; this measures it).
   Reset on each stats read so it reports the peak of the last window. */
static volatile int s_peak;
static volatile uint32_t s_clipcnt;
static inline void fft_capture(int16_t v)
{
    s_fft_buf[s_fft_wr & (AUD_FFT_N - 1)] = (float)v;
    s_fft_wr++;
    int a = v < 0 ? -(int)v : (int)v;
    if (a > s_peak) s_peak = a;
    if (a >= 32767) s_clipcnt++;
}

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
    if (s_ch == 2 && !s_downmix) {
        /* true stereo passthrough (e.g. future headphone routing):
           zero-copy the caller's interleaved buffer straight to the ring */
        while (done < frames) {
            size_t n = frames - done;
            if (n > AUD_CHUNK_BYTES / 4)
                n = AUD_CHUNK_BYTES / 4;
            if (ring_send(pcm + done * 2, n * 4, deadline) == 0)
                break;
            done += n;
        }
    } else {
        /* mono path: build L=R stereo frames in a temp buffer. Source is
           either mono (duplicate the sample) or stereo folded to mono for
           the mono speaker ((L+R)/2 — the codec does NOT sum L/R, and the
           NS4150B has one input, so an unfolded stereo source would drop a
           channel; see docs/audio-tab5-status.md). */
        int16_t st[256 * 2];
        while (done < frames) {
            size_t n = frames - done;
            if (n > 256)
                n = 256;
            if (s_ch == 2) {
                const int16_t *src = pcm + done * 2;
                for (size_t i = 0; i < n; i++) {
                    /* sum fits int32; >>1 average is exactly within int16
                       range (no clip needed). arithmetic shift = round to
                       -inf, inaudible bias. */
                    int16_t m = (int16_t)(((int32_t)src[2 * i] +
                                           src[2 * i + 1]) >> 1);
                    st[2 * i] = m;
                    st[2 * i + 1] = m;
                    fft_capture(m);
                }
            } else {
                for (size_t i = 0; i < n; i++) {
                    st[2 * i] = pcm[done + i];
                    st[2 * i + 1] = pcm[done + i];
                    fft_capture(pcm[done + i]);
                }
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
    s_wav_abort = true; /* unblock any WAV streamer waiting on the ring */
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

void audio_tab5_set_downmix(bool on)
{
    s_downmix = on;
}

bool audio_tab5_downmix(void)
{
    return s_downmix;
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
        .peak = (uint16_t)s_peak,
        .clipped = s_clipcnt,
    };
    s_peak = 0;       /* peak meter: report the level since the last read */
    s_clipcnt = 0;
}

/* ---- 16-band FFT analyzer ------------------------------------------- */
/* Self-contained iterative radix-2 FFT (no esp-dsp dependency); 512 pts is
   ample for a 16-bar EQ at 48 kHz (~94 Hz/bin). Runs on the js_task when
   audio.stats() is polled, so the cost is bounded by the poll rate. */
static void fft_radix2(float *re, float *im)
{
    for (int i = 1, j = 0; i < AUD_FFT_N; i++) { /* bit-reversal */
        int bit = AUD_FFT_N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= AUD_FFT_N; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < AUD_FFT_N; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                float br = re[b] * cr - im[b] * ci;
                float bi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - br; im[b] = im[a] - bi;
                re[a] += br;        im[a] += bi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

void audio_tab5_spectrum(uint8_t bands[16])
{
    if (!bands)
        return;
    if (!s_running) {
        for (int i = 0; i < 16; i++) bands[i] = 0;
        return;
    }
    static float re[AUD_FFT_N], im[AUD_FFT_N], hann[AUD_FFT_N];
    static bool hann_ready;
    if (!hann_ready) {
        for (int i = 0; i < AUD_FFT_N; i++)
            hann[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i /
                                          (float)(AUD_FFT_N - 1)));
        hann_ready = true;
    }
    uint32_t wr = s_fft_wr; /* snapshot: oldest captured sample sits at wr */
    for (int i = 0; i < AUD_FFT_N; i++) {
        re[i] = s_fft_buf[(wr + i) & (AUD_FFT_N - 1)] * hann[i] / 32768.0f;
        im[i] = 0.0f;
    }
    fft_radix2(re, im);

    /* bins 1..N/2 grouped into 16 geometric (log) bands; mag -> dB -> 0..100.
       The geometric ratio spans bin 1..256 over 16 bands (~sqrt(2)/band). */
    const int half = AUD_FFT_N / 2;
    const float ratio = 1.4142136f;
    const float norm = 4.0f / (float)AUD_FFT_N; /* full-scale sine bin -> 1.0 */
    float edge = 1.0f;
    for (int b = 0; b < 16; b++) {
        int k0 = (int)(edge + 0.5f);
        edge *= ratio;
        int k1 = (int)(edge + 0.5f);
        if (k0 < 1) k0 = 1;
        if (k1 > half) k1 = half;
        if (k1 < k0) k1 = k0;
        float peak = 0.0f;                        /* peak-hold across the band */
        for (int k = k0; k <= k1; k++) {
            float mg = sqrtf(re[k] * re[k] + im[k] * im[k]) * norm;
            if (mg > peak) peak = mg;
        }
        float db = 20.0f * log10f(peak + 1e-9f);  /* 0 dBFS = full scale */
        int v = (int)((db + 72.0f) * (100.0f / 72.0f)); /* -72..0 dB -> 0..100 */
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        bands[b] = (uint8_t)v;
    }
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
    /* linear fade in/out (~5 ms) so short tones don't click. Without it a
       tone starts/ends mid-cycle -> a step discontinuity the ear hears as a
       plosive "pop" rather than a beep, which dominates very short tones. */
    size_t fade = (size_t)s_rate / 200;
    if (fade > total / 2)
        fade = total / 2;
    int16_t buf[240 * 2];
    float phase = 0.0f;
    int producer_ch = s_ch;
    for (size_t off = 0; off < total;) {
        size_t n = total - off;
        if (n > 240)
            n = 240;
        for (size_t i = 0; i < n; i++) {
            size_t pos = off + i;
            float env = 1.0f;
            if (fade > 0) {
                if (pos < fade)
                    env = (float)pos / (float)fade;
                else if (pos >= total - fade)
                    env = (float)(total - 1 - pos) / (float)fade;
            }
            int16_t v = (int16_t)(amp * env * sinf(phase));
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

/* ---- WAV playback ---------------------------------------------------- */
/* Parse a RIFF/WAVE blob held in memory and stream its PCM through the
 * normal pipeline. Blocking: feeds in chunks with backpressure, so the
 * caller is held for ~the clip duration (run it on a task — see the
 * async wrapper). audio_tab5_stop() / a new play aborts via s_wav_abort.
 * (s_wav_playing / s_wav_abort are declared with the other state up top
 * so audio_tab5_stop() can reach them.) */
esp_err_t audio_tab5_play_wav_mem(const uint8_t *data, size_t len)
{
    wav_info_t w;
    if (!wav_parse_mem(data, len, &w))
        return ESP_ERR_INVALID_ARG;
    if (w.format_tag != 1 || w.bits_per_sample != 16)
        return ESP_ERR_NOT_SUPPORTED; /* only integer 16-bit PCM */
    if (w.channels < 1 || w.channels > 2)
        return ESP_ERR_NOT_SUPPORTED;

    ESP_RETURN_ON_ERROR(audio_tab5_start(w.sample_rate, w.channels), TAG,
                        "wav start %lu Hz", (unsigned long)w.sample_rate);

    const size_t frame_samples = w.channels;        /* s16 per frame */
    const size_t total_frames = w.pcm_bytes / (frame_samples * 2);
    const int16_t *pcm = (const int16_t *)(const void *)w.pcm;

    ESP_LOGI(TAG, "wav: %lu Hz %uch %u frames (%.2fs)",
             (unsigned long)w.sample_rate, w.channels, (unsigned)total_frames,
             (double)total_frames / (double)w.sample_rate);

    s_wav_abort = false;
    s_wav_playing = true;
    size_t done = 0;
    esp_err_t ret = ESP_OK;
    while (done < total_frames && !s_wav_abort) {
        size_t n = total_frames - done;
        if (n > 4096)
            n = 4096;
        size_t sent = audio_tab5_write(pcm + done * frame_samples, n, 2000);
        if (sent == 0) { /* writer stuck >2 s: give up rather than hang */
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        done += sent;
    }
    s_wav_playing = false;
    return s_wav_abort ? ESP_OK : ret;
}

typedef struct {
    const uint8_t *data;
    size_t len;
} wav_req_t;

static void wav_task(void *arg)
{
    wav_req_t req = *(wav_req_t *)arg;
    free(arg);
    esp_err_t err = audio_tab5_play_wav_mem(req.data, req.len);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "wav play: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

bool audio_tab5_play_wav_mem_async(const uint8_t *data, size_t len)
{
    if (!data || len < 44)
        return false;
    if (s_wav_playing) /* preempt the current clip, then start the new one */
        s_wav_abort = true;
    wav_req_t *req = malloc(sizeof *req);
    if (!req)
        return false;
    req->data = data;
    req->len = len;
    /* slightly bigger stack than tone: wav_parse + logging */
    if (xTaskCreate(wav_task, "audio_wav", 4096, req, 6, NULL) != pdPASS) {
        free(req);
        return false;
    }
    return true;
}

bool audio_tab5_wav_playing(void)
{
    return s_wav_playing;
}

/* ---- firmware-embedded boot WAV ------------------------------------- */
#if CONFIG_MQJS_TAB5_AUDIO_BOOT_WAV
/* EMBED_FILES (see CMakeLists) exposes the blob as these linker symbols.
   Embedded with the .bin trailing-NUL convention off (binary file), so
   _end marks one past the last byte. */
extern const uint8_t boot_wav_start[] asm("_binary_tab5_boot_wav_start");
extern const uint8_t boot_wav_end[]   asm("_binary_tab5_boot_wav_end");

bool audio_tab5_play_boot_wav(void)
{
    return audio_tab5_play_wav_mem_async(boot_wav_start,
                                         (size_t)(boot_wav_end - boot_wav_start));
}
#else
bool audio_tab5_play_boot_wav(void)
{
    ESP_LOGW(TAG, "no boot WAV embedded (CONFIG_MQJS_TAB5_AUDIO_BOOT_WAV off)");
    return false;
}
#endif

/* ---- boot self-test (P2 gate) + optional WAV autoplay --------------- */
static void selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000)); /* let UI / Wi-Fi bring-up settle */

#if CONFIG_MQJS_TAB5_AUDIO_SELFTEST
    esp_err_t err = audio_tab5_start(48000, 2);
    if (err == ESP_OK) {
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
    } else {
        ESP_LOGE(TAG, "SELFTEST start failed: %s", esp_err_to_name(err));
    }
#endif

#if CONFIG_MQJS_TAB5_AUDIO_BOOT_WAV_AUTOPLAY
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "boot WAV autoplay");
    audio_tab5_play_boot_wav(); /* spawns its own streamer task */
#endif

    vTaskDelete(NULL);
}

void audio_tab5_selftest_async(void)
{
    xTaskCreate(selftest_task, "audio_st", 4096, NULL, 5, NULL);
}

#endif /* CONFIG_MQJS_TAB5_AUDIO */
