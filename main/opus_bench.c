/*
 * Optional boot-time baseline for the float Opus decoder and the first
 * replaceable P4 kernel. Enable CONFIG_MQJS_OPUS_BENCHMARK for a
 * measure-and-reflash build; keep it disabled in normal firmware.
 */
#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "opus.h"
#include "opus_p4_kernels.h"

static const char *TAG = "opus_bench";

#define SAMPLE_RATE 48000
#define FRAME_SIZE 960
#define MAX_PACKET_BYTES 1500
#define DECODE_ROUNDS 500
#define KERNEL_ROUNDS 4000

static float s_input[FRAME_SIZE];
static float s_output[FRAME_SIZE];
static float s_kernel_a[FRAME_SIZE];
static float s_kernel_b[FRAME_SIZE];
static unsigned char s_packet[MAX_PACKET_BYTES];
static volatile float s_sink;

static void init_input(void)
{
    uint32_t state = 0x12345678;
    for (int i = 0; i < FRAME_SIZE; i++) {
        state = state * 1664525u + 1013904223u;
        s_input[i] = ((int32_t)(state >> 8) / 8388608.0f) * 0.35f;
        s_kernel_a[i] = s_input[i];
        s_kernel_b[i] = s_input[(i * 17 + 31) % FRAME_SIZE];
    }
}

static void bench_kernel(int n)
{
    int64_t start = esp_timer_get_time();
    float sum = 0.0f;
    for (int i = 0; i < KERNEL_ROUNDS; i++)
        sum += opus_p4_inner_prod_f32(s_kernel_a, s_kernel_b, n);
    int64_t elapsed = esp_timer_get_time() - start;
    s_sink = sum;

    ESP_LOGI(TAG, "inner_prod/%d %-10s: %lld us total, %.3f us/call",
             n, opus_p4_kernel_impl(), (long long)elapsed,
             (double)elapsed / KERNEL_ROUNDS);
}

static void bench_decoder(void)
{
    int error = OPUS_OK;
    OpusEncoder *encoder =
        opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                            &error);
    if (!encoder || error != OPUS_OK) {
        ESP_LOGE(TAG, "encoder create failed: %s", opus_strerror(error));
        goto done;
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000));

    int packet_bytes = opus_encode_float(encoder, s_input, FRAME_SIZE, s_packet,
                                         sizeof(s_packet));
    if (packet_bytes < 0) {
        ESP_LOGE(TAG, "encode failed: %s", opus_strerror(packet_bytes));
        goto done;
    }

    OpusDecoder *decoder = opus_decoder_create(SAMPLE_RATE, 1, &error);
    if (!decoder || error != OPUS_OK) {
        ESP_LOGE(TAG, "decoder create failed: %s", opus_strerror(error));
        goto done;
    }

    for (int i = 0; i < 10; i++) {
        int decoded = opus_decode_float(decoder, s_packet, packet_bytes,
                                        s_output, FRAME_SIZE, 0);
        if (decoded < 0) {
            ESP_LOGE(TAG, "warmup decode failed: %s", opus_strerror(decoded));
            opus_decoder_destroy(decoder);
            goto done;
        }
    }

    int64_t start = esp_timer_get_time();
    int decoded = OPUS_OK;
    for (int i = 0; i < DECODE_ROUNDS; i++)
        decoded = opus_decode_float(decoder, s_packet, packet_bytes, s_output,
                                    FRAME_SIZE, 0);
    int64_t elapsed = esp_timer_get_time() - start;
    s_sink = s_output[decoded > 0 ? decoded - 1 : 0];

    if (decoded < 0) {
        ESP_LOGE(TAG, "decode failed: %s", opus_strerror(decoded));
    } else {
        double us_per_frame = (double)elapsed / DECODE_ROUNDS;
        ESP_LOGI(TAG,
                 "CELT float decode: packet=%d B, %lld us total, "
                 "%.3f us/frame, %.2f%% realtime",
                 packet_bytes, (long long)elapsed, us_per_frame,
                 us_per_frame / 200.0);
    }
    opus_decoder_destroy(decoder);

done:
    if (encoder)
        opus_encoder_destroy(encoder);
}

static TaskHandle_t s_waiter;

static void opus_bench_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "=== float Opus portable-C baseline ===");
    init_input();
    bench_kernel(120);
    bench_kernel(240);
    bench_kernel(480);
    bench_kernel(960);
    bench_decoder();
    ESP_LOGI(TAG, "=== done ===");
    xTaskNotifyGive(s_waiter);
    vTaskDelete(NULL);
}

void opus_bench_run(void)
{
    s_waiter = xTaskGetCurrentTaskHandle();
    BaseType_t created = xTaskCreatePinnedToCore(opus_bench_task, "opus_bench",
                                                 32768, NULL, 5, NULL, 0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create benchmark task");
        return;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
