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
#include "mqjs_runtime.h"
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

    for (;;) {
        /* fresh context per run: no leaked state between executions */
        int rc = mqjs_run_script(_binary_task_js_start,
                                 strlen(_binary_task_js_start),
                                 "task", mem, JS_MEM_SIZE);
        ESP_LOGI(TAG, "script ended rc=%d, restarting in 1s", rc);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    /* network first: blocks up to 30s for an IP, JS runs either way */
    wifi_start_and_wait(30000);

    /* the mquickjs VM does not use the C stack for JS frames, but the
       parser + bindings still need headroom */
    xTaskCreate(js_task, "mqjs", 16384, NULL, 5, NULL);
}
