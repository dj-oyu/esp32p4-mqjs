/*
 * TEMPORARY boot-time benchmark: the same CPU-bound JS workload run via
 * mqjs_run_script() with the dev arena placed in PSRAM vs internal
 * SRAM — quantifies what an internal-SRAM "fast arena" would buy the
 * interpreter before committing 256KB of HP SRAM to it. Called once
 * from app_main before anything else (quiet system, no WiFi/LVGL).
 *
 * NOTE: the arena buffers are deliberately NOT freed — mqjs_run_script
 * leaves dev->mem pointing at the caller's buffer and a later
 * mqjs_rt_init would skip the slot's real arena alloc, so freeing here
 * would leave the dev slot dangling. This build is measure-and-reflash
 * only; remove the app_main call (and the leak) when done.
 */
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mqjs_runtime.h"

static const char *TAG = "jsmem_bench";

/* heap churn + pointer chasing + call frames, all inside the arena.
   Self-terminating: no timers, no events, the pump returns at the end. */
static const char BENCH_JS[] =
    "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }\n"
    "var f = 0;\n"
    "for (var r = 0; r < 5; r++) f += fib(20);\n"
    "var arr = [];\n"
    "for (var i = 0; i < 3000; i++) arr.push((i * 2654435761) % 65536);\n"
    "for (var r2 = 0; r2 < 20; r2++) { arr.sort(function(a,b){return a-b;}); arr.reverse(); }\n"
    "var s = '';\n"
    "for (var j = 0; j < 4000; j++) { s += 'x' + (j & 15); if (s.length > 512) s = s.slice(256); }\n"
    "var sum = 0;\n"
    "for (var r3 = 0; r3 < 40000; r3++) { var o = { a: r3, b: r3 * 2, c: 'k' + (r3 & 7) }; sum += o.b; }\n"
    "print('bench done ' + f + ' ' + arr[0] + ' ' + sum);\n";

static void run_one(const char *tag, uint32_t caps)
{
    uint8_t *mem = heap_caps_malloc(MQJS_APP_MEM_SIZE, caps);
    if (!mem) {
        ESP_LOGE(TAG, "%s: arena alloc failed", tag);
        return;
    }
    for (int round = 1; round <= 2; round++) {
        int64_t t = esp_timer_get_time();
        int rc = mqjs_run_script(BENCH_JS, sizeof(BENCH_JS) - 1, "jsmem",
                                 mem, MQJS_APP_MEM_SIZE);
        int64_t us = esp_timer_get_time() - t;
        ESP_LOGI(TAG, "%-8s round %d: %8lld us (rc=%d)", tag, round,
                 (long long)us, rc);
    }
    /* intentionally not freed, see file header */
}

void jsmem_bench_run(void)
{
    ESP_LOGI(TAG, "=== JS arena PSRAM vs internal SRAM (256KB each) ===");
    ESP_LOGI(TAG, "internal free %u (largest %u), psram free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    run_one("PSRAM", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    run_one("SRAM", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "internal free now %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "=== done ===");
}
