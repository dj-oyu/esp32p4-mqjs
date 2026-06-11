/*
 * Stamp-P4 / Tab5 mquickjs host: the P4a multi-app runtime.
 *
 * js_task owns every JS context (cooperative multi-context, see
 * docs/launcher-multiapp-design.md). Two apps are started hardcoded —
 * the P4a verification setup, the P4b launcher replaces this:
 *   - dev slot (1): the classic development flow. The script comes from
 *     LittleFS (persisted) or the embedded examples/ file
 *     (idf.py -DMQJS_SCRIPT=life.js build flash) and is replaced by
 *     signed pushes over MQTT; it auto-reruns 1s after a natural end.
 *   - slot 2: the embedded background app (-DMQJS_APP2=p4_bg_app.js),
 *     proving that timers/mqtt keep running behind the foreground app.
 * Tapping the status bar cycles the foreground app.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "board_tab5.h"
#include "mqjs_runtime.h"
#include "storage.h"
#include "task_source.h"
#include "ui_status.h"
#include "ui_tab5.h"
#include "wifi.h"

static const char *TAG = "app";

/* embedded by EMBED_TXTFILES (NUL-terminated) */
extern const char _binary_task_js_start[];
extern const char _binary_app2_js_start[];

/* current dev-slot source; owned here (the runtime only borrows it,
   so the buffer must outlive the running app — see mqjs_app_start) */
static char *s_net_script;
static size_t s_net_len;
static const char *s_origin = "embedded";

/* Dev source provider (runs on js_task): pick up a pushed task if one
   arrived, otherwise rerun what we have — exactly the pre-P4 loop of
   run / take / 1s-recheck, just inverted into a callback. */
static bool dev_next_source(const char **src, size_t *len, const char **name,
                            void *user)
{
    size_t next_len = 0;
    char *next = task_source_take(&next_len);
    if (next) {
        free(s_net_script);
        s_net_script = next;
        s_net_len = next_len;
        s_origin = "mqtt";
        ESP_LOGI(TAG, "switching to task received over MQTT (%u bytes)",
                 (unsigned)next_len);
    }
    *src = s_net_script ? s_net_script : _binary_task_js_start;
    *len = s_net_script ? s_net_len : strlen(_binary_task_js_start);
    *name = s_net_script ? "mqtt-task" : "task";
    ui_status_set_task(*name, s_origin);
    return true;
}

static void js_task(void *arg)
{
    mqjs_rt_init(); /* arenas (4 x 256KB PSRAM) + shared event queue */

    /* a previously verified+persisted task takes over the embedded one.
       lengths are tracked explicitly: bytecode tasks contain NULs */
    s_net_script = storage_load_task(&s_net_len);
    if (s_net_script)
        s_origin = "persisted";

    /* P4a hardcoded second app (background-execution proof) */
    const char *app2 = _binary_app2_js_start;
    if (app2[0] != '\0' &&
        mqjs_app_start(2, app2, strlen(app2), "bg_app") != 0)
        ESP_LOGW(TAG, "embedded app2 failed to start");

    mqjs_runtime_run(dev_next_source, NULL); /* never returns */
}

void app_main(void)
{
    board_tab5_power_init();   /* Tab5 only: C6 power rail (no-op elsewhere) */
    ui_tab5_start();           /* Tab5 only: display + LVGL (no-op elsewhere) */
    mqjs_set_print_sink(ui_tab5_log); /* tee JS print to the UI console */
    storage_init();            /* mount LittleFS for persisted tasks */

    /* network first: blocks up to 30s for an IP, JS runs either way */
    if (wifi_start_and_wait(30000))
        task_source_start();   /* accept replacement tasks over MQTT */

    /* the mquickjs VM does not use the C stack for JS frames, but the
       parser + bindings still need headroom. Pinned to Core 0: the LVGL
       task lives on Core 1 (see ui_tab5) */
    xTaskCreatePinnedToCore(js_task, "mqjs", 16384, NULL, 5, NULL, 0);
}
