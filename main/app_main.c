/*
 * Stamp-P4 mquickjs demo: run one JS task that blinks an LED and
 * reacts to a button. The task's memory lives in PSRAM.
 *
 * Wiring (adjust pins to your board):
 *   LED_PIN    G2 : LED + resistor to GND
 *   BUTTON_PIN G5 : button to GND (internal pull-up)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mqjs_runtime.h"

static const char *TAG = "app";

#define JS_MEM_SIZE (256 * 1024)

/* In the real platform this arrives over the network (signed!) or is
   loaded from LittleFS. Hard-coded here for the first milestone. */
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
        int rc = mqjs_run_script(demo_task_js, strlen(demo_task_js),
                                 "demo", mem, JS_MEM_SIZE);
        ESP_LOGI(TAG, "script ended rc=%d, restarting in 1s", rc);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    /* the mquickjs VM does not use the C stack for JS frames, but the
       parser + bindings still need headroom */
    xTaskCreate(js_task, "mqjs", 16384, NULL, 5, NULL);
}
