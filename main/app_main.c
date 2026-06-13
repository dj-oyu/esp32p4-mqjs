/*
 * Stamp-P4 / Tab5 mquickjs host: the P4b multi-app runtime + launcher.
 *
 * js_task owns every JS context (cooperative multi-context, see
 * docs/launcher-multiapp-design.md):
 *   - slot 0: the resident launcher (embedded examples/launcher.js,
 *     auto-started and kept alive by the scheduler, unstoppable) —
 *     the only embedded app.
 *   - dev slot (1): the classic development flow. The script comes from
 *     LittleFS (persisted) or the embedded examples/ file
 *     (idf.py -DMQJS_SCRIPT=life.js build flash) and is replaced by
 *     signed pushes over MQTT; it auto-reruns 1s after a natural end.
 *   - slots 2-3: apps installed over MQTT ("// @app <name>" push ->
 *     /littlefs/apps/), started from the launcher / sys.launch.
 * Status bar: the chip opens the previous app, a long-press opens the
 * launcher.
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
#include "cam_tab5.h"
#include "audio_tab5.h"
#include "wifi.h"

static const char *TAG = "app";

/* embedded by EMBED_TXTFILES (NUL-terminated) */
extern const char _binary_task_js_start[];
extern const char _binary_launcher_js_start[];

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

/* §11 store catalog: the runtime's sys.store()/sys.install() are backed
   by task_source's broker mirror (all entries safe pre-connect) */
static const mqjs_store_api_t s_store_api = {
    .count = task_source_store_count,
    .get = task_source_store_get,
    .install = task_source_install,
};

/* Fired once on the first IP (from the Wi-Fi event task). Releases the two
   things that were genuinely waiting on the network: the C-side task-delivery
   service, and any JS app parked in the net.onReady wait queue. */
static void on_net_up(void)
{
    task_source_start();   /* accept replacement tasks over MQTT */
    mqjs_notify_net_up();  /* drain the net.onReady wait queue (JS apps) */
}

static void js_task(void *arg)
{
    mqjs_rt_init(); /* arenas (4 x 256KB PSRAM) + shared event queue */

    /* the scheduler keeps the registered "launcher" resident in slot 0 */
    mqjs_register_app_source("launcher", _binary_launcher_js_start,
                             strlen(_binary_launcher_js_start));

    /* a previously verified+persisted task takes over the embedded one.
       lengths are tracked explicitly: bytecode tasks contain NULs */
    s_net_script = storage_load_task(&s_net_len);
    if (s_net_script)
        s_origin = "persisted";

    mqjs_runtime_run(dev_next_source, NULL); /* never returns */
}

void app_main(void)
{
    /* boot-time micro-benches (2026-06-12): ppa_bench_run() /
       ppa_bench_crossover() / jsmem_bench_run() — call here to
       re-measure on a quiet system. Measured at -O2: PPA 4x on big
       fills (CPU fill is PSRAM-bound at ~43Mpx/s, store width moot),
       5.5-7x on row+/full blends, blend crossover w=36/38 at h=24
       (~900px ~ 4 cells); JS arena SRAM-vs-PSRAM ~4% (skip). -O2
       itself: pixel loops ~2x, JS ~20% vs -Og. */
    board_tab5_power_init();   /* Tab5 only: C6 power rail (no-op elsewhere) */
    ui_tab5_start();           /* Tab5 only: display + LVGL (no-op elsewhere) */
    cam_tab5_set_i2c(ui_tab5_i2c_bus()); /* camera SCCB rides the touch bus
                                            (no-op stubs elsewhere) */
    mqjs_set_print_sink(ui_tab5_log); /* tee JS print to the UI console */
    mqjs_set_notify_sink(ui_status_set_event); /* sys.notify -> status bar */
    mqjs_set_store_provider(&s_store_api);     /* §11 catalog browse */
    mqjs_set_uninstall_hook(task_source_app_unsub); /* §11 no-resurrect */
    storage_init();            /* mount LittleFS for persisted tasks */

    /* Platform-owned network defaults: apps never hardcode the broker or the
       topic namespace. The namespace is the first segment of the task topic
       ("esp32p4-mqjs" from "esp32p4-mqjs/task/<id>") — single source of truth. */
    mqjs_set_default_broker(CONFIG_MQJS_TASK_BROKER);
    static char s_topic_prefix[32];
    {
        const char *t = CONFIG_MQJS_TASK_TOPIC;
        const char *slash = strchr(t, '/');
        size_t n = slash ? (size_t)(slash - t) : strlen(t);
        if (n >= sizeof s_topic_prefix)
            n = sizeof s_topic_prefix - 1;
        memcpy(s_topic_prefix, t, n);
        s_topic_prefix[n] = '\0';
        mqjs_set_topic_prefix(s_topic_prefix);
    }

    /* Launcher + UI runtime start immediately, NOT gated on Wi-Fi (so the
       UI is up at once even with a slow/absent network). The mquickjs VM
       does not use the C stack for JS frames, but the parser + bindings
       need headroom. Core 0: the LVGL task lives on Core 1 (see ui_tab5). */
    xTaskCreatePinnedToCore(js_task, "mqjs", 16384, NULL, 5, NULL, 0);

#if CONFIG_MQJS_TAB5_AUDIO_SELFTEST || CONFIG_MQJS_TAB5_AUDIO_BOOT_WAV_AUTOPLAY
    /* boot audio: the ES8388's shared I2C bus is up since ui_tab5_start()
       and audio needs no network, so it is not gated on the Wi-Fi wait. */
    audio_tab5_selftest_async();
#endif

    /* Wi-Fi comes up in the background while the above already runs. Nothing
       blocks here on the network: the services that need it are released from
       the got-IP event (on_net_up), so app_main returns immediately. */
    wifi_start(on_net_up);
}
