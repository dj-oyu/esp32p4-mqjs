/*
 * Tab5 speaker playback path (P2 of docs/opus-decoder-plan.md).
 *
 * Producer (Opus decode task, tone generator, future JS binding) pushes
 * interleaved signed 16-bit PCM frames; a writer task drains the bounded
 * ring buffer into I2S DMA feeding the ES8388. Rates 8/12/16/24/48 kHz,
 * mono or stereo (mono is duplicated to both DAC channels in software —
 * the I2S link always runs 16-bit stereo).
 *
 * All entry points are no-op stubs unless CONFIG_MQJS_TAB5_AUDIO=y.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"

typedef struct {
    bool running;          /* between start() and stop() */
    uint32_t sample_rate;
    uint8_t channels;      /* producer-side channel count (1 or 2) */
    uint32_t queued_bytes; /* PCM bytes waiting in the ring buffer */
    uint32_t underruns;    /* ring went empty while started; the final
                              drain after the producer stops counts one,
                              so a gapless stream of N segments ends at
                              exactly N — mid-stream increments are real
                              underruns */
    uint64_t frames_written; /* frames handed to I2S DMA since boot */
} audio_tab5_stats_t;

#if CONFIG_MQJS_TAB5_AUDIO

/* Bring up (or reconfigure) the playback path. First call initializes
 * SPK_EN, I2S TX and the ES8388 (needs ui_tab5_start() done: the codec
 * sits on the shared touch/SCCB I2C bus). Later calls switch sample
 * rate / channel count. sample_rate: 8/12/16/24/48 kHz. channels: 1|2. */
esp_err_t audio_tab5_start(uint32_t sample_rate, int channels);

/* Queue interleaved s16 PCM frames; blocks up to timeout_ms for ring
 * space and returns the number of frames accepted (backpressure). */
size_t audio_tab5_write(const int16_t *pcm, size_t frames, uint32_t timeout_ms);

/* Mark the stream stopped and wait (up to ~1 s) for the ring to drain.
 * The hardware stays initialized; start() resumes instantly. */
esp_err_t audio_tab5_stop(void);

/* 0..100, esp_codec_dev scale. Applies immediately and to future
 * start() calls. */
esp_err_t audio_tab5_set_volume(int pct);
int audio_tab5_volume(void);

/* Stereo->mono fold for the built-in mono speaker. When on (default), a
 * 2-channel source is mixed (L+R)/2 and sent to both lanes so no channel
 * is dropped (the ES8388/NS4150B path does not sum L/R). Turn off only
 * for genuine stereo routing (e.g. headphone out). */
void audio_tab5_set_downmix(bool on);
bool audio_tab5_downmix(void);

void audio_tab5_get_stats(audio_tab5_stats_t *out);

/* Blocking sine tone through the normal write path (P2 gate helper).
 * Starts the pipeline at the current rate (or 48 kHz stereo if idle). */
esp_err_t audio_tab5_tone(int freq_hz, int duration_ms);

/* Non-blocking tone: spawns a one-shot task and returns immediately.
 * true = started, false = bad args, already sounding, or no memory.
 * This is what the JS audio.tone() binding calls so the JS event loop
 * is never stalled. duration_ms capped at 5000. */
bool audio_tab5_tone_async(int freq_hz, int duration_ms);

/* Parse a RIFF/WAVE blob (16-bit integer PCM, 1|2 ch) held in memory
 * and stream it through the pipeline. Blocking ~clip duration. */
esp_err_t audio_tab5_play_wav_mem(const uint8_t *data, size_t len);

/* Async wrapper: spawns a task to play the blob; a clip already playing
 * is preempted. true = task started. The buffer must outlive playback
 * (an embedded blob or a static buffer — not a stack copy). */
bool audio_tab5_play_wav_mem_async(const uint8_t *data, size_t len);
bool audio_tab5_wav_playing(void);

/* Play the firmware-embedded boot WAV (assets/audio/tab5-boot.wav).
 * true = started; false if no WAV was embedded
 * (CONFIG_MQJS_TAB5_AUDIO_BOOT_WAV off). */
bool audio_tab5_play_boot_wav(void);

/* Spawn a one-shot task: wait 3 s after boot, beep twice, log stats.
 * Wired into app_main behind CONFIG_MQJS_TAB5_AUDIO_SELFTEST. */
void audio_tab5_selftest_async(void);

#else /* stubs, same pattern as ui_tab5.h */

static inline esp_err_t audio_tab5_start(uint32_t sample_rate, int channels)
{
    (void)sample_rate;
    (void)channels;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline size_t audio_tab5_write(const int16_t *pcm, size_t frames,
                                      uint32_t timeout_ms)
{
    (void)pcm;
    (void)frames;
    (void)timeout_ms;
    return 0;
}
static inline esp_err_t audio_tab5_stop(void) { return ESP_OK; }
static inline esp_err_t audio_tab5_set_volume(int pct)
{
    (void)pct;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline int audio_tab5_volume(void) { return 0; }
static inline void audio_tab5_set_downmix(bool on) { (void)on; }
static inline bool audio_tab5_downmix(void) { return true; }
static inline void audio_tab5_get_stats(audio_tab5_stats_t *out)
{
    if (out)
        *out = (audio_tab5_stats_t){ 0 };
}
static inline esp_err_t audio_tab5_tone(int freq_hz, int duration_ms)
{
    (void)freq_hz;
    (void)duration_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool audio_tab5_tone_async(int freq_hz, int duration_ms)
{
    (void)freq_hz;
    (void)duration_ms;
    return false;
}
static inline esp_err_t audio_tab5_play_wav_mem(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool audio_tab5_play_wav_mem_async(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return false;
}
static inline bool audio_tab5_wav_playing(void) { return false; }
static inline bool audio_tab5_play_boot_wav(void) { return false; }
static inline void audio_tab5_selftest_async(void) {}

#endif /* CONFIG_MQJS_TAB5_AUDIO */
