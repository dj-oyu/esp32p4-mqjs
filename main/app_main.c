/*
 * esp32p4-mqjs — a JavaScript runtime for M5Stack Tab5 / Stamp-P4.
 * Copyright (C) 2026 dj-oyu
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. It is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details, and THIRD_PARTY_NOTICES.md for the
 * licences of the components this firmware links against.
 */
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
#include "kbd_tab5.h"
#include "opus_player.h"
#include "wifi.h"
#include "tailscale_adapter.h"
#include "term_lp_ring.h"

static const char *TAG = "app";

/* embedded by EMBED_TXTFILES (NUL-terminated) */
extern const char _binary_task_js_start[];
extern const char _binary_launcher_js_start[];
extern const char _binary_device_settings_js_start[];

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
    tailscale_adapter_on_net_up();  /* start tailnet (NTP->microlink) if armed */
}

static void js_task(void *arg)
{
    mqjs_rt_init(); /* arenas (4 x 256KB PSRAM) + shared event queue */

    /* the scheduler keeps the registered "launcher" resident in slot 0 */
    mqjs_register_system_app_source("launcher", _binary_launcher_js_start,
                                    strlen(_binary_launcher_js_start));
    mqjs_register_system_app_source("device_settings",
                                    _binary_device_settings_js_start,
                                    strlen(_binary_device_settings_js_start));

    /* a previously verified+persisted task takes over the embedded one.
       lengths are tracked explicitly: bytecode tasks contain NULs */
    s_net_script = storage_load_task(&s_net_len);
    if (s_net_script)
        s_origin = "persisted";

    mqjs_runtime_run(dev_next_source, NULL); /* never returns */
}

#if CONFIG_MQJS_TAB5_KEYBOARD
/* Dock attach/detach -> input policy + screen orientation (kbd_tab5
   task context). Policy first: the landscape flip re-applies the
   foreground app's keyboard mode, and must do so under the new rule. */
static void kbd_dock_changed(bool present)
{
    ui_tab5_set_hw_keyboard(present);
    ui_tab5_set_landscape(present);
}
#endif

static void tab5_ui_ready(void *arg)
{
    (void)arg;
    cam_tab5_set_i2c(ui_tab5_i2c_bus()); /* camera SCCB rides the touch bus */

#if CONFIG_MQJS_TAB5_KEYBOARD
    /* Keyboard dock: own I2C bus (port 0, pogo pins), hot-pluggable.
       The dock holds the Tab5 sideways, so presence drives the screen
       orientation (the callback runs on the kbd task, never an ISR). */
    kbd_tab5_set_presence_cb(kbd_dock_changed);
    kbd_tab5_start();
#endif
    /* No boot-time probe: it brought the CSI/ISP pipeline up and left it
       STREAMING forever (~57MB/s MIPI->ISP->PSRAM DMA + per-frame ISP CCM
       errors), competing with esp_hosted's SDIO DMA. The pipeline is now
       brought up lazily on the first camera.scan and never at boot. */

#if CONFIG_MQJS_TAB5_AUDIO_SELFTEST
    audio_tab5_selftest_async();
#endif

#if CONFIG_OPUS_PLAYER_BOOT_AUTOPLAY
    if (!opus_player_play_boot())
        ESP_LOGW(TAG, "failed to start boot Opus playback");
#endif
}

void app_main(void)
{
    /* The LP SRAM black box (docs/term-design.md §4.4). FIRST STATEMENT ON
       PURPOSE: it validates the retained region, freezes the previous
       session's log tail as `lastboot` and re-arms the ring, and all of that
       has to happen before anything in this boot can log — and before any
       later boot stage can fail. Cheap (~31 KB memcpy only when there is
       something to freeze) and never fatal.
       See components/term_core/PHASE3_MANIFEST.md §2. */
    term_lp_ring_boot();

    /* boot-time micro-benches (2026-06-12): ppa_bench_run() /
       ppa_bench_crossover() / jsmem_bench_run() — call here to
       re-measure on a quiet system. Measured at -O2: PPA 4x on big
       fills (CPU fill is PSRAM-bound at ~43Mpx/s, store width moot),
       5.5-7x on row+/full blends, blend crossover w=36/38 at h=24
       (~900px ~ 4 cells); JS arena SRAM-vs-PSRAM ~4% (skip). -O2
       itself: pixel loops ~2x, JS ~20% vs -Og. */
#if CONFIG_MQJS_OPUS_BENCHMARK
    opus_player_bench_run();
#endif

    board_tab5_power_init();   /* Tab5 only: C6 power rail (no-op elsewhere) */
    ui_tab5_start(tab5_ui_ready, NULL); /* display + LVGL, then I2C users */
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

    /* Arm the Tailscale lifecycle before the network is up so its got-IP hook
       in on_net_up() can start the time-sync->microlink chain (no-op without a
       key). wifi.c owns SNTP; chain its sync callback to the adapter. */
    tailscale_adapter_init();
    wifi_set_time_sync_cb(tailscale_adapter_on_time_synced);
    /* Camera <-> network mutual exclusion (camera-lifecycle-plan §4): a scan
       suspends the microlink session (which otherwise starves the camera to
       0.2 fps) and resumes it on teardown. cam_tab5 stays network-agnostic;
       it calls these hooks. No-ops when Tailscale is off. */
    cam_tab5_set_net_hooks(tailscale_adapter_suspend, tailscale_adapter_resume);

    /* Wi-Fi comes up in the background while the above already runs. Nothing
       blocks here on the network: the services that need it are released from
       the got-IP event (on_net_up), so app_main returns immediately. */
    wifi_start(on_net_up);
}
