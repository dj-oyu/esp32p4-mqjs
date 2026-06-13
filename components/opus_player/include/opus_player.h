#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_OPUS_PLAYER

/* Decode an Ogg Opus blob and stream it through audio_tab5. The input
 * buffer must remain valid until playback completes. */
esp_err_t opus_player_play_mem(const uint8_t *data, size_t len);
bool opus_player_play_mem_async(const uint8_t *data, size_t len);
bool opus_player_play_boot(void);
void opus_player_stop(void);
bool opus_player_playing(void);
#if CONFIG_MQJS_OPUS_BENCHMARK
void opus_player_bench_run(void);
#endif

#else

static inline esp_err_t opus_player_play_mem(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool opus_player_play_mem_async(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return false;
}
static inline bool opus_player_play_boot(void) { return false; }
static inline void opus_player_stop(void) {}
static inline bool opus_player_playing(void) { return false; }

#endif
