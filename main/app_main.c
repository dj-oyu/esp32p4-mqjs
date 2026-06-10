/*
 * Stamp-P4 mquickjs demo: run one JS task in a fresh context whose
 * memory lives in PSRAM.
 *
 * The script comes from examples/ and is embedded at build time:
 *   idf.py -DMQJS_SCRIPT=life.js build flash
 * (default: blink_button.js — LED on G2, button on G5, see the script)
 *
 * In the real platform the script arrives over the network (signed!)
 * or is loaded from LittleFS.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "board_tab5.h"
#include "mqjs_runtime.h"
#include "storage.h"
#include "task_source.h"
#include "wifi.h"

static const char *TAG = "app";

#define JS_MEM_SIZE (256 * 1024)

/* embedded by EMBED_TXTFILES (NUL-terminated) */
extern const char _binary_task_js_start[];

static void js_task(void *arg)
{
    uint8_t *mem = heap_caps_malloc(JS_MEM_SIZE, MALLOC_CAP_SPIRAM);
    if (!mem) {
        ESP_LOGW(TAG, "PSRAM alloc failed, falling back to internal RAM");
        mem = malloc(JS_MEM_SIZE);
    }
    if (!mem) {
        ESP_LOGE(TAG, "no memory for JS context");
        vTaskDelete(NULL);
    }

    /* a previously verified+persisted task takes over the embedded one.
       lengths are tracked explicitly: bytecode tasks contain NULs */
    size_t net_len = 0;
    char *net_script = storage_load_task(&net_len);

    for (;;) {
        const char *src = net_script ? net_script : _binary_task_js_start;
        size_t src_len = net_script ? net_len : strlen(_binary_task_js_start);
        /* fresh context per run: no leaked state between executions */
        int rc = mqjs_run_script(src, src_len,
                                 net_script ? "mqtt-task" : "task",
                                 mem, JS_MEM_SIZE);

        size_t next_len = 0;
        char *next = task_source_take(&next_len);
        if (!next) {
            ESP_LOGI(TAG, "script ended rc=%d, restarting in 1s", rc);
            vTaskDelay(pdMS_TO_TICKS(1000));
            /* re-check: a task that arrived during the pause would
               otherwise wait until the next run stops */
            next = task_source_take(&next_len);
        }
        if (next) {
            free(net_script);
            net_script = next;
            net_len = next_len;
            ESP_LOGI(TAG, "switching to task received over MQTT (%u bytes)",
                     (unsigned)next_len);
        }
    }
}

void app_main(void)
{
    board_tab5_power_init();   /* Tab5 only: C6 power rail (no-op elsewhere) */
    storage_init();            /* mount LittleFS for persisted tasks */

    /* network first: blocks up to 30s for an IP, JS runs either way */
    if (wifi_start_and_wait(30000))
        task_source_start();   /* accept replacement tasks over MQTT */

    /* the mquickjs VM does not use the C stack for JS frames, but the
       parser + bindings still need headroom */
    xTaskCreate(js_task, "mqjs", 16384, NULL, 5, NULL);
}
