#include "sdkconfig.h"

#if CONFIG_OPUS_PLAYER

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>

#include "audio_tab5.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ogg_opus.h"
#include "opus.h"
#include "opus_player.h"

#define OPUS_MAX_FRAME_SAMPLES 5760
#define OPUS_TASK_STACK 32768

static const char *TAG = "opus_player";
static volatile bool s_playing;
static volatile bool s_abort;

#if CONFIG_OPUS_PLAYER_DIAGNOSTICS
#define DIAG_HASH_FRAMES 4096
#define DIAG_FIRST_SAMPLES 16

typedef struct {
    uint64_t hash;
    size_t hash_frames;
    int16_t min;
    int16_t max;
    int16_t first[DIAG_FIRST_SAMPLES];
    size_t first_count;
} pcm_diag_t;

static void pcm_diag_add(pcm_diag_t *diag, const int16_t *pcm, size_t frames,
                         int channels)
{
    size_t samples = frames * (size_t)channels;
    for (size_t i = 0; i < samples; i++) {
        int16_t sample = pcm[i];
        if (sample < diag->min)
            diag->min = sample;
        if (sample > diag->max)
            diag->max = sample;
        if (diag->first_count < DIAG_FIRST_SAMPLES)
            diag->first[diag->first_count++] = sample;
    }

    size_t hash_frames = frames;
    if (hash_frames > DIAG_HASH_FRAMES - diag->hash_frames)
        hash_frames = DIAG_HASH_FRAMES - diag->hash_frames;
    const uint8_t *bytes = (const uint8_t *)pcm;
    size_t byte_count = hash_frames * (size_t)channels * sizeof(*pcm);
    for (size_t i = 0; i < byte_count; i++) {
        diag->hash ^= bytes[i];
        diag->hash *= UINT64_C(1099511628211);
    }
    diag->hash_frames += hash_frames;
}

static void pcm_diag_log(const pcm_diag_t *diag)
{
    ESP_LOGI(TAG, "pcm diag: first4096-fnv=%016" PRIx64 " min=%d max=%d",
             diag->hash, diag->min, diag->max);
    ESP_LOGI(TAG, "pcm first: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
             diag->first[0], diag->first[1], diag->first[2], diag->first[3],
             diag->first[4], diag->first[5], diag->first[6], diag->first[7],
             diag->first[8], diag->first[9], diag->first[10], diag->first[11],
             diag->first[12], diag->first[13], diag->first[14], diag->first[15]);
}
#endif

static esp_err_t play_mem(const uint8_t *data, size_t len)
{
    ogg_opus_reader_t *reader = heap_caps_malloc(sizeof(*reader),
                                                  MALLOC_CAP_SPIRAM |
                                                  MALLOC_CAP_8BIT);
    if (!reader)
        return ESP_ERR_NO_MEM;

    ogg_opus_info_t info;
    if (ogg_opus_open(reader, data, len, &info) != 0) {
        free(reader);
        return ESP_ERR_INVALID_ARG;
    }

    int opus_error = OPUS_OK;
    OpusDecoder *decoder = opus_decoder_create(48000, info.channels,
                                                &opus_error);
    if (!decoder || opus_error != OPUS_OK) {
        free(reader);
        return ESP_ERR_NO_MEM;
    }

    size_t pcm_samples = OPUS_MAX_FRAME_SAMPLES * info.channels;
    int16_t *pcm = heap_caps_malloc(pcm_samples * sizeof(*pcm),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        opus_decoder_destroy(decoder);
        free(reader);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = audio_tab5_start(48000, info.channels);
    if (ret != ESP_OK)
        goto done;

    uint32_t skip = info.pre_skip;
    uint64_t frames = 0;
    unsigned packets = 0;
    const uint8_t *packet;
    size_t packet_len;
#if CONFIG_OPUS_PLAYER_DIAGNOSTICS
    pcm_diag_t diag = {
        .hash = UINT64_C(14695981039346656037),
        .min = INT16_MAX,
        .max = INT16_MIN,
    };
#endif

    while (!s_abort) {
        int next = ogg_opus_next(reader, &packet, &packet_len);
        if (next == 0)
            break;
        if (next < 0) {
            ret = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        int decoded = opus_decode(decoder, packet, (opus_int32)packet_len, pcm,
                                  OPUS_MAX_FRAME_SAMPLES, 0);
        if (decoded < 0) {
            ESP_LOGE(TAG, "decode packet %u: %s", packets,
                     opus_strerror(decoded));
            ret = ESP_FAIL;
            break;
        }
        packets++;

        size_t offset = skip < (uint32_t)decoded ? skip : (uint32_t)decoded;
        skip -= offset;
        size_t remaining = (size_t)decoded - offset;
#if CONFIG_OPUS_PLAYER_DIAGNOSTICS
        pcm_diag_add(&diag, pcm + offset * info.channels, remaining,
                     info.channels);
#endif
        while (remaining && !s_abort) {
            size_t sent = audio_tab5_write(
                pcm + offset * info.channels, remaining, 2000);
            if (!sent) {
                ret = ESP_ERR_TIMEOUT;
                s_abort = true;
                break;
            }
            offset += sent;
            remaining -= sent;
            frames += sent;
        }
    }

#if CONFIG_OPUS_PLAYER_DIAGNOSTICS
    audio_tab5_stats_t audio_stats;
    audio_tab5_get_stats(&audio_stats);
    ESP_LOGI(TAG, "audio diag: written=%llu queued=%lu underruns=%lu",
             (unsigned long long)audio_stats.frames_written,
             (unsigned long)audio_stats.queued_bytes,
             (unsigned long)audio_stats.underruns);
    pcm_diag_log(&diag);
#endif
    audio_tab5_stop();
    ESP_LOGI(TAG, "done: %u packets, %llu frames, %uch, %s", packets,
             (unsigned long long)frames, info.channels,
             esp_err_to_name(ret));

done:
    free(pcm);
    opus_decoder_destroy(decoder);
    free(reader);
    return ret;
}

esp_err_t opus_player_play_mem(const uint8_t *data, size_t len)
{
    if (!data || !len || s_playing)
        return ESP_ERR_INVALID_STATE;
    s_abort = false;
    s_playing = true;
    esp_err_t ret = play_mem(data, len);
    s_playing = false;
    return ret;
}

typedef struct {
    const uint8_t *data;
    size_t len;
} opus_request_t;

static void opus_task(void *arg)
{
    opus_request_t request = *(opus_request_t *)arg;
    free(arg);
    esp_err_t err = play_mem(request.data, request.len);
    s_playing = false;
    if (err != ESP_OK)
        ESP_LOGW(TAG, "play: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

bool opus_player_play_mem_async(const uint8_t *data, size_t len)
{
    if (!data || !len || s_playing)
        return false;
    opus_request_t *request = malloc(sizeof(*request));
    if (!request)
        return false;
    *request = (opus_request_t){ .data = data, .len = len };
    s_abort = false;
    s_playing = true;
    if (xTaskCreate(opus_task, "audio_opus", OPUS_TASK_STACK, request, 6,
                    NULL) != pdPASS) {
        s_playing = false;
        free(request);
        return false;
    }
    return true;
}

void opus_player_stop(void)
{
    if (!s_playing)
        return;
    s_abort = true;
    audio_tab5_stop();
}

bool opus_player_playing(void)
{
    return s_playing;
}

#if CONFIG_OPUS_PLAYER_BOOT_OPUS
extern const uint8_t boot_opus_start[] asm("_binary_tab5_boot_48k_opus_start");
extern const uint8_t boot_opus_end[] asm("_binary_tab5_boot_48k_opus_end");

bool opus_player_play_boot(void)
{
    return opus_player_play_mem_async(
        boot_opus_start, (size_t)(boot_opus_end - boot_opus_start));
}
#else
bool opus_player_play_boot(void)
{
    return false;
}
#endif

#endif
