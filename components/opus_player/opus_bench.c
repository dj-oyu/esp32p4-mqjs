/*
 * Boot-time, decoder-only benchmark using the real embedded stereo fixture.
 * Ogg parsing and audio output are deliberately outside packet timings.
 */
#include "sdkconfig.h"

#if CONFIG_MQJS_OPUS_BENCHMARK

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ogg_opus.h"
#include "opus.h"
#include "opus_p4_kernels.h"
#include "opus_player.h"

#define MAX_FRAME_SAMPLES 5760
#define BENCH_TASK_STACK 32768

static const char *TAG = "opus_bench";
static TaskHandle_t s_waiter;

extern const uint8_t boot_opus_start[] asm("_binary_tab5_boot_48k_opus_start");
extern const uint8_t boot_opus_end[] asm("_binary_tab5_boot_48k_opus_end");

typedef struct {
    uint64_t decode_us;
    uint64_t decode_cycles;
    uint64_t frames;
    uint64_t pcm_hash;
    uint32_t packets;
    uint32_t packet_bytes;
    uint32_t min_us;
    uint32_t max_us;
} pass_result_t;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t av = *(const uint32_t *)a;
    uint32_t bv = *(const uint32_t *)b;
    return (av > bv) - (av < bv);
}

static uint32_t percentile(const uint32_t *sorted, size_t count, unsigned pct)
{
    size_t index = ((count - 1) * pct + 99) / 100;
    return sorted[index];
}

static uint64_t hash_pcm(uint64_t hash, const int16_t *pcm, size_t samples)
{
    const uint8_t *bytes = (const uint8_t *)pcm;
    for (size_t i = 0; i < samples * sizeof(*pcm); i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool decode_pass(uint32_t *packet_us, size_t packet_capacity,
                        pass_result_t *out)
{
    ogg_opus_reader_t *reader = heap_caps_malloc(sizeof(*reader),
                                                  MALLOC_CAP_SPIRAM |
                                                  MALLOC_CAP_8BIT);
    if (!reader)
        return false;

    ogg_opus_info_t info;
    size_t fixture_len = (size_t)(boot_opus_end - boot_opus_start);
    if (ogg_opus_open(reader, boot_opus_start, fixture_len, &info) != 0) {
        free(reader);
        return false;
    }

    int error = OPUS_OK;
    OpusDecoder *decoder = opus_decoder_create(48000, info.channels, &error);
    if (!decoder || error != OPUS_OK) {
        free(reader);
        return false;
    }

    int16_t *pcm = heap_caps_malloc(
        MAX_FRAME_SAMPLES * info.channels * sizeof(*pcm),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        opus_decoder_destroy(decoder);
        free(reader);
        return false;
    }

    *out = (pass_result_t){
        .pcm_hash = UINT64_C(14695981039346656037),
        .min_us = UINT32_MAX,
    };

    const uint8_t *packet;
    size_t packet_len;
    bool ok = true;
    for (;;) {
        int next = ogg_opus_next(reader, &packet, &packet_len);
        if (next == 0)
            break;
        if (next < 0 || out->packets >= packet_capacity) {
            ok = false;
            break;
        }

        uint32_t cycle_start = esp_cpu_get_cycle_count();
        int64_t time_start = esp_timer_get_time();
        int decoded = opus_decode(decoder, packet, (opus_int32)packet_len, pcm,
                                  MAX_FRAME_SAMPLES, 0);
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() - time_start);
        uint32_t cycles = esp_cpu_get_cycle_count() - cycle_start;
        if (decoded < 0) {
            ESP_LOGE(TAG, "packet %lu: %s", (unsigned long)out->packets,
                     opus_strerror(decoded));
            ok = false;
            break;
        }

        packet_us[out->packets++] = elapsed;
        out->decode_us += elapsed;
        out->decode_cycles += cycles;
        out->frames += (uint32_t)decoded;
        out->packet_bytes += (uint32_t)packet_len;
        if (elapsed < out->min_us)
            out->min_us = elapsed;
        if (elapsed > out->max_us)
            out->max_us = elapsed;
        out->pcm_hash = hash_pcm(out->pcm_hash, pcm,
                                 (size_t)decoded * info.channels);
    }

    free(pcm);
    opus_decoder_destroy(decoder);
    free(reader);
    return ok;
}

static void run_fixture_benchmark(void)
{
    size_t timings_bytes =
        CONFIG_MQJS_OPUS_BENCH_MAX_PACKETS * sizeof(uint32_t);
    uint32_t *packet_us = heap_caps_malloc(timings_bytes,
                                            MALLOC_CAP_SPIRAM |
                                            MALLOC_CAP_8BIT);
    uint32_t *sorted_us = heap_caps_malloc(timings_bytes,
                                            MALLOC_CAP_SPIRAM |
                                            MALLOC_CAP_8BIT);
    pass_result_t result;
    if (!packet_us || !sorted_us) {
        ESP_LOGE(TAG, "failed to allocate packet timing arrays");
        free(packet_us);
        free(sorted_us);
        return;
    }

    opus_p4_pie_probe();

    ESP_LOGI(TAG,
             "fixture=%u bytes, cpu=%d MHz, warmup=%d, measured=%d, "
             "max_packets=%d",
             (unsigned)(boot_opus_end - boot_opus_start),
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             CONFIG_MQJS_OPUS_BENCH_WARMUP_PASSES,
             CONFIG_MQJS_OPUS_BENCH_PASSES,
             CONFIG_MQJS_OPUS_BENCH_MAX_PACKETS);

    for (int i = 0; i < CONFIG_MQJS_OPUS_BENCH_WARMUP_PASSES; i++) {
        if (!decode_pass(packet_us, CONFIG_MQJS_OPUS_BENCH_MAX_PACKETS,
                         &result)) {
            ESP_LOGE(TAG, "warmup pass %d failed", i + 1);
            goto done;
        }
    }

#if CONFIG_OPUS_P4_FUNCTION_PROFILE
    opus_p4_profile_reset();
#endif

    uint64_t total_decode_us = 0;
    uint64_t total_decode_cycles = 0;
    uint64_t total_frames = 0;
    uint64_t expected_hash = 0;
    for (int pass = 0; pass < CONFIG_MQJS_OPUS_BENCH_PASSES; pass++) {
        if (!decode_pass(packet_us, CONFIG_MQJS_OPUS_BENCH_MAX_PACKETS,
                         &result)) {
            ESP_LOGE(TAG, "measured pass %d failed", pass + 1);
            goto done;
        }
        memcpy(sorted_us, packet_us, result.packets * sizeof(*packet_us));
        qsort(sorted_us, result.packets, sizeof(*sorted_us), cmp_u32);

        double audio_us = (double)result.frames * 1000000.0 / 48000.0;
        ESP_LOGI(TAG,
                 "pass=%d packets=%lu bytes=%lu frames=%" PRIu64
                 " decode=%" PRIu64 " us cycles=%" PRIu64
                 " cycles/frame=%.1f load=%.2f%% "
                 "packet_us[min/p50/p95/p99/max]=%lu/%lu/%lu/%lu/%lu "
                 "pcm_fnv=%016" PRIx64,
                 pass + 1, (unsigned long)result.packets,
                 (unsigned long)result.packet_bytes, result.frames,
                 result.decode_us, result.decode_cycles,
                 (double)result.decode_cycles / result.frames,
                 100.0 * result.decode_us / audio_us,
                 (unsigned long)result.min_us,
                 (unsigned long)percentile(sorted_us, result.packets, 50),
                 (unsigned long)percentile(sorted_us, result.packets, 95),
                 (unsigned long)percentile(sorted_us, result.packets, 99),
                 (unsigned long)result.max_us, result.pcm_hash);

        if (pass == 0)
            expected_hash = result.pcm_hash;
        else if (result.pcm_hash != expected_hash)
            ESP_LOGE(TAG, "pass %d PCM hash mismatch", pass + 1);
        total_decode_us += result.decode_us;
        total_decode_cycles += result.decode_cycles;
        total_frames += result.frames;
    }

    double total_audio_us = (double)total_frames * 1000000.0 / 48000.0;
    ESP_LOGI(TAG,
             "summary passes=%d decode=%" PRIu64 " us audio=%.0f us "
             "cycles=%" PRIu64 " cycles/frame=%.1f load=%.2f%% "
             "throughput=%.2fx realtime",
             CONFIG_MQJS_OPUS_BENCH_PASSES, total_decode_us, total_audio_us,
             total_decode_cycles, (double)total_decode_cycles / total_frames,
             100.0 * total_decode_us / total_audio_us,
             total_audio_us / total_decode_us);
    ESP_LOGI(TAG, "kernel=%s verify_failures=%" PRIu32,
             opus_p4_kernel_impl(), opus_p4_kernel_verify_failures());
    {
        extern uint32_t opus_p4_comb_pie_calls, opus_p4_comb_c_calls;
        extern uint32_t opus_p4_denorm_pie_calls, opus_p4_denorm_c_calls;
        ESP_LOGI(TAG, "comb: pie_calls=%" PRIu32 " c_calls=%" PRIu32,
                 opus_p4_comb_pie_calls, opus_p4_comb_c_calls);
        ESP_LOGI(TAG, "denorm: pie_calls=%" PRIu32 " c_calls=%" PRIu32,
                 opus_p4_denorm_pie_calls, opus_p4_denorm_c_calls);
    }

#if CONFIG_OPUS_P4_FUNCTION_PROFILE
    opus_p4_profile_dump(30);
#endif

done:
    free(sorted_us);
    free(packet_us);
}

static void bench_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "=== real-fixture decoder benchmark (%s) ===",
             opus_p4_kernel_impl());
    run_fixture_benchmark();
    ESP_LOGI(TAG, "=== done ===");
    xTaskNotifyGive(s_waiter);
    vTaskDelete(NULL);
}

void opus_player_bench_run(void)
{
    s_waiter = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(bench_task, "opus_bench", BENCH_TASK_STACK,
                                NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "failed to create benchmark task");
        return;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

#endif
