/*
 * Stamp-P4 mquickjs platform: runs one JS task whose source can be
 * replaced over MQTT (see net_mqtt.c). The active script is persisted
 * in NVS and reloaded on boot; without one, a built-in blink demo runs.
 *
 * Wiring (adjust pins to your board):
 *   LED_PIN    G2 : LED + resistor to GND
 *   BUTTON_PIN G5 : button to GND (internal pull-up)
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "mqjs_runtime.h"
#include "script_runner.h"
#include "script_store.h"
#include "net_mqtt.h"

static const char *TAG = "app";

#define JS_MEM_SIZE (256 * 1024)

/* fallback task when nothing has been distributed yet */
static const char demo_task_js[] =
    "print('JS task started');\n"
    "var LED = 2, BTN = 5;\n"
    "gpio.setMode(LED, gpio.OUT);\n"
    "gpio.setMode(BTN, gpio.IN_PULLUP);\n"
    "\n"
    "var on = false;\n"
    "setInterval(function() {\n"
    "    on = !on;\n"
    "    gpio.write(LED, on ? 1 : 0);\n"
    "}, 500);\n"
    "\n"
    "gpio.onChange(BTN, function(level) {\n"
    "    print('button level:', level, 'at', performance.now(), 'ms');\n"
    "});\n";

/* ------------------------------------------------------------------ */
/* runner state (shared with the MQTT task)                            */
/* ------------------------------------------------------------------ */

static SemaphoreHandle_t s_lock;   /* guards s_pending / s_cmd */
static SemaphoreHandle_t s_wake;   /* nudges the runner task */
static char  *s_pending;
static size_t s_pending_len;
static script_cmd_t s_cmd = SCRIPT_CMD_NONE;

void *script_psram_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!p)
        p = malloc(size);
    return p;
}

void script_runner_submit(char *src, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_pending);
    s_pending = src;
    s_pending_len = len;
    s_cmd = SCRIPT_CMD_RESTART;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wake);
    mqjs_runtime_stop();           /* abort the current run, if any */
}

void script_runner_control(script_cmd_t cmd)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cmd = cmd;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wake);
    mqjs_runtime_stop();
}

/* ------------------------------------------------------------------ */
/* runner task                                                         */
/* ------------------------------------------------------------------ */

static void js_task(void *arg)
{
    uint8_t *mem = script_psram_alloc(JS_MEM_SIZE);
    if (!mem) {
        ESP_LOGE(TAG, "no memory for JS context");
        vTaskDelete(NULL);
        return;
    }

    size_t len = 0;
    char *script = script_store_load(&len);
    if (script) {
        ESP_LOGI(TAG, "running stored script (%u bytes)", (unsigned)len);
    } else {
        len = sizeof(demo_task_js) - 1;
        script = script_psram_alloc(len + 1);
        if (script)
            memcpy(script, demo_task_js, len + 1);
        ESP_LOGI(TAG, "no stored script, running built-in demo");
    }

    bool run_now = true;
    for (;;) {
        /* apply whatever the MQTT side queued up. mqjs_runtime_stop()
           is sticky, so a script submitted a moment ago cannot slip
           past the clear below: either we see it here, or the stop
           flag aborts the stale run and we loop back around. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        script_cmd_t cmd = s_cmd;
        s_cmd = SCRIPT_CMD_NONE;
        if (s_pending) {
            free(script);
            script = s_pending;
            len = s_pending_len;
            s_pending = NULL;
            cmd = SCRIPT_CMD_RESTART;
        }
        mqjs_runtime_clear_stop();
        xSemaphoreGive(s_lock);

        switch (cmd) {
        case SCRIPT_CMD_STOP:
            run_now = false;
            break;
        case SCRIPT_CMD_CLEAR:
            free(script);
            script = NULL;
            len = 0;
            run_now = false;
            break;
        case SCRIPT_CMD_RESTART:
            run_now = true;
            break;
        default:
            break;
        }

        if (run_now && script) {
            net_mqtt_publish_status("running", 0);
            int rc = mqjs_run_script(script, len, "task", mem, JS_MEM_SIZE);
            ESP_LOGI(TAG, "script ended rc=%d", rc);
            net_mqtt_publish_status(rc == 0 ? "ended" : "error", rc);
            /* a script that ends on its own is re-run after 1s (same
               behavior as before); a wake from MQTT cuts this short */
            xSemaphoreTake(s_wake, pdMS_TO_TICKS(1000));
        } else {
            net_mqtt_publish_status("idle", 0);
            xSemaphoreTake(s_wake, portMAX_DELAY);
        }
    }
}

void script_runner_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    /* the mquickjs VM does not use the C stack for JS frames, but the
       parser + bindings still need headroom */
    xTaskCreate(js_task, "mqjs", 16384, NULL, 5, NULL);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    script_runner_init();
    net_mqtt_start();
}
