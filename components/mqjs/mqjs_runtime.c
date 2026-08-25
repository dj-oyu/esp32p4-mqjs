/*
 * mqjs_runtime.c - MicroQuickJS multi-app runtime for ESP32-P4 (P4a)
 *
 * Implements the C side of the device stdlib defined in device_stdlib.c:
 *   - print / console.log
 *   - setTimeout / setInterval / clearTimeout / clearInterval / delay
 *   - gpio.setMode / gpio.write / gpio.read / gpio.onChange
 *   - mqtt.* / ssh.* / ui.* / store.* / sys.*
 *   - Date / performance.now
 *
 * Design notes (docs/launcher-multiapp-design.md §3):
 *   - Up to MQJS_MAX_WORKERS cooperative contexts live on ONE FreeRTOS
 *     task. All per-app binding state is bundled in MqjsWorker; dispatch
 *     is serial, so a single global `s_cur_wk` tells every binding
 *     which app is calling — no TLS, no locks.
 *   - Ownership invariant: events carry (or resolve to) slot +
 *     generation. app_stop never drains the shared queue; the
 *     dispatcher drops (and frees) events whose owner died. Same
 *     pattern as W1 widget generations and W3 ssh session ids.
 *   - UI is exclusive to the foreground app: background ui.* calls are
 *     silent no-ops (queries still answer). On a foreground switch the
 *     outgoing app's screens are destroyed (ui_tab5_w_reset + canvas
 *     reset); the incoming app rebuilds in sys.onForeground.
 *   - Callbacks are held with JS_AddGCRef (persistent GC reference).
 *     The compacting GC moves objects, so raw JSValue must never be
 *     stored in C; JSGCRef.val is auto-updated by the GC.
 *   - ISRs NEVER touch a JS context. They only post an event to a
 *     FreeRTOS queue; the JS task dispatches.
 *   - A JS interrupt handler aborts any single JS run (eval or
 *     callback) that exceeds MQJS_MAX_RUN_MS, so a buggy app cannot
 *     hang the loop (it CAN stall other apps for up to that long —
 *     accepted cooperative-model worst case, design §6).
 *
 * Build with -DESP_PLATFORM (default under ESP-IDF). Without it, a
 * PC stub build is produced for desktop testing (gpio.* print to
 * stdout, onChange registers but never fires). The PC build keeps a
 * tiny in-process event ring so sys.signal / sys.focus and multi-app
 * scheduling are testable on the host.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>

#include "cutils.h"
#include "mquickjs.h"
#include "mqjs_runtime.h"
#include "mqjs_classes.h"
#include "mqjs_power.h"
#include "system_vault.h"
#include "tailscale_adapter.h"
/* Pure logic, no ESP-IDF headers, so it is included unconditionally and
   links into tools/run_pc exactly as it does into the firmware. */
#include "skk_core.h"
#include "ime_core.h"
/* Same deal: header-only, no ESP-IDF headers, so ui.cellWidth and the
   ui_tab5 cell renderer classify from one table on both targets. */
#include "ui_cell_width.h"
/* The native terminal (docs/term-design.md). term_registry.h and
   term_port.h are pure C99 over a function-pointer seam, so they come in
   unconditionally and term.* works in run_pc exactly as ui.* does. The
   device glue behind term_ui_tab5.h is the only ESP-only part. */
#include "term_registry.h"
#include "term_lp_ring.h"
#include "term_pipe.h"
/* Battery state + charge policy. Like term_registry.h above, the header is
   type-only outside an ESP build (stubs, no battery), so power.* exists in
   run_pc too and an app can be developed against it. */
#include "pwr_tab5.h"
#include "app/mqjs_app_manager_internal.h"
/* usleep(). Outside the ESP_PLATFORM block on purpose: the same code
   runs in run_pc, and inside it the host build fell back to an implicit
   declaration. */
#include <unistd.h>

#ifdef ESP_PLATFORM
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_partition.h"
#include <dirent.h>
#include "mqtt_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ui_tab5.h"
#include "sshc.h"
#include "cam_tab5.h"
#include "audio_tab5.h"
#include "term_ui_tab5.h"
/* ボリューム登録簿。fs.* はこの層の仮想パスしか知らない
   (docs/filer-storage-design.md §3)。 */
#include "fs_core.h"
static const char *TAG = "mqjs";
#else
#include <time.h>
#include <unistd.h>
#endif

#define MQJS_MAX_TIMERS       16
#define MQJS_MAX_GPIO_CB      8
#define MQJS_MAX_WIDGET_CB    48 /* buttons/list rows/toggles with a JS cb */
#define MQJS_MAX_MQTT_SUB     8
#define MQJS_MQTT_TOPIC_MAX   96
#define MQJS_MQTT_PAYLOAD_MAX 4096
#define MQJS_SIGNAL_VAL_MAX   4096
#define MQJS_MAX_RUN_MS       5000  /* per JS_Eval / callback watchdog */
#define MQJS_QUEUE_LEN        64 /* 32 dropped events under touch-move +
                                    MQTT bursts; 64 ≈ +1.3KB internal
                                    (hotspot audit §4) */
#define MQJS_DEV_RESTART_MS   1000  /* natural-end rerun delay (compat) */

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */

static int64_t time_ms(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time() / 1000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Monotonic microseconds. The 1 ms resolution of time_ms() cannot see
   anything on the IME's per-keystroke path (microseconds against a
   55 ms budget), so the skk bindings bracket skk_key() with this and
   sys.micros() exposes it to JS benchmarks. */
static int64_t time_us(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

static int64_t date_ms(void)
{
    /* wall clock: meaningful on ESP only after SNTP sync */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ------------------------------------------------------------------ */
/* runtime state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    bool used;
    bool repeat;
    int32_t period_ms;
    int64_t deadline;
    JSGCRef fn;
} TimerSlot;

typedef struct {
    bool used;
    int pin;
    JSGCRef fn;
} GpioSlot;

/* one queue feeds the JS task; producers are the GPIO ISR, the esp-mqtt
   event tasks, the UI task and the ssh session tasks. mqtt/ssh/signal
   strings are heap copies owned by the event: the dispatcher frees them
   (also when it drops the event because its owner died). */
typedef enum { EV_GPIO, EV_MQTT_CONNECTED, EV_MQTT_DATA, EV_TOUCH, EV_KEY,
               EV_SSH_DATA, EV_SSH_CLOSED, EV_WIDGET, EV_SIGNAL,
               EV_FOCUS, EV_CLIP, EV_CAM, EV_HTTP,
               EV_TERM_REPLY, /* term.onReply: a DSR/DA answer, inline */
               EV_NET, /* broadcast: release the net.onReady wait queue */
               EV_FSGRANT, /* fs.request: the consent answer came back */
               EV_FSOP /* a long fs job (format) finished on its own task */
             } MqjsEventType;

typedef struct {
    uint8_t type;
    uint8_t worker; /* owner worker for worker-addressed events (EV_MQTT_*,
                       EV_SIGNAL); other types resolve their owner at
                       dispatch time (§3.2 routing table) */
    uint16_t gen;   /* owner generation: stale events are dropped */
    union {
        struct { uint8_t pin; uint8_t level; } gpio;
        struct { char *topic; char *payload; uint32_t len; } mqtt;
        struct { int16_t x, y; uint8_t kind; } touch;
        struct { char text[64]; uint8_t len; } key; /* one key as UTF-8 */
        struct { char *data; uint32_t len; int16_t id; } ssh; /* heap rx */
        struct { char reason[84]; int16_t id; } ssh_closed;
        struct { uint32_t handle; int32_t value; } widget; /* tap/change */
        struct { uint32_t id; uint8_t ok; } fsgrant; /* fs.request answer */
        struct { uint8_t ok; } fsop;                 /* fs.format result */
        struct { char *value; char from[32]; } signal; /* sys.signal;
                       from[] sized to MqjsWorker.name */
        struct { uint8_t target; } focus;
        struct { char *text; uint32_t len; uint8_t ok; } cam; /* camera scan
                       result: heap text owned by the event */
        struct { char *body; uint32_t len; int16_t status; } http; /* http.get
                       result: heap body owned by the event (dispatcher
                       frees it), status<=0 = request failed */
        /* term.onReply (docs/term-design.md §6). Carried INLINE, not on the
           heap: a terminal reply is at most TERM_REPLY_MAX bytes and this
           event is posted from the UI task while the term registry's lock
           is held, where a malloc is exactly the thing not to do. */
        struct { char bytes[TERM_REPLY_MAX]; uint8_t len; int32_t id; } term;
    } u;
} MqjsEvent;

/* key.text は IME の確定文字列も運ぶので 8 バイトでは足りない。union は
   ssh_closed の reason[84] が支配しているので、そこに収まる限り広げても
   MqjsEvent は 1 バイトも太らない (riscv32 で 92B のまま)。キューは
   MQJS_QUEUE_LEN 個をまるごと確保するので、超えたら静かに RAM を食う。 */
_Static_assert(sizeof(((MqjsEvent *)0)->u.key)
                   <= sizeof(((MqjsEvent *)0)->u.ssh_closed),
               "widening key.text would grow every queued event");

typedef struct {
    bool used;
    char topic[MQJS_MQTT_TOPIC_MAX];
    JSGCRef fn;
} MqttSub;

/* One JS callback per interactive widget (button/list row/toggle/slider).
   `screen` is the owning screen's handle: when that screen is destroyed
   (ui.back() / retain-depth eviction / app end) every slot it owned is
   released in one sweep, so the compacting GC repacks once instead of
   per-widget (design §4④). */
typedef struct {
    bool used;
    uint32_t handle;  /* widget handle the LVGL side posts with */
    uint32_t screen;  /* owning screen handle */
    JSGCRef fn;
} WidgetCb;

/* per-session ssh callbacks (W3 handle-style: ssh.onData(id, fn)).
   Sized to the sshc session cap + slack; sshc.h is ESP-only so the
   constant is mirrored here (SSHC_MAX_SESSIONS = 3). */
#define MQJS_MAX_SSH_CB 4
typedef struct {
    bool used;
    int32_t id;
    bool data_used, close_used;
    uint8_t utf8_tail[3];
    uint8_t utf8_tail_len;
    JSGCRef data_fn, close_fn;
} SshCb;

/* term.onReply(id, cb) — per term, like ssh.onData is per session
   (docs/term-design.md §8). Four is the same reasoning as the ssh table:
   the shipped terminal keeps three sessions plus slack, and a term that is
   PIPED needs no sink at all (§6 routes its replies into the channel). */
#define MQJS_MAX_TERM_CB 4
typedef struct {
    bool used;
    bool fn_used;
    int32_t id;
    JSGCRef fn;
} TermCb;

/* All binding state of one app, bundled (design §3.1). ~2KB of internal
   RAM per slot; the 256KB context arena lives in PSRAM. */
typedef struct {
    bool used;
    bool kill_req;            /* deferred sys.stop (self-stop must not free
                                 the context under its own active JS frame:
                                 the reaper does it after the dispatch) */
    uint8_t idx;              /* own index (for s_fg_worker comparisons) */
    uint16_t gen;             /* bumped on every start: stale-event filter */
    char name[32];
    char vault_id[32];        /* immutable source identity; setAppName cannot
                                 impersonate another app's vault */
    bool trusted_system;       /* immutable: only firmware registry can set */
    uint8_t *mem;             /* fixed arena (design §3.6) */
    size_t mem_size;
    JSContext *ctx;
    char *src_owned;          /* sys.launch file source: freed at stop */

    TimerSlot timers[MQJS_MAX_TIMERS];
    GpioSlot  gpio_cb[MQJS_MAX_GPIO_CB];
    MqttSub   mqtt_subs[MQJS_MAX_MQTT_SUB];
    bool      mqtt_onconn_used;
    JSGCRef   mqtt_onconn;
    WidgetCb  widget_cbs[MQJS_MAX_WIDGET_CB]; /* only the fg app has live screens */
    SshCb     ssh_cbs[MQJS_MAX_SSH_CB];
    TermCb    term_cbs[MQJS_MAX_TERM_CB];
    volatile bool touch_used; /* read by the UI task (poster) */
    JSGCRef   touch_cb;
    volatile bool key_used;   /* read by the UI task (poster) */
    JSGCRef   key_cb;
    volatile bool ime_used;   /* ui.ime(1): 打鍵を IME に通す (poster が読む) */
    /* ui.caret(): 変換中の文字列を出す位置 (canvas 座標)。C が知り得ない
       唯一の値なので JS から貰う。IME の所有タスクが fg アプリの分だけを
       読む — 3 つの int16 で、ずれても 1 打鍵ぶんフロートの位置が古いだけ。 */
    volatile int16_t caret_x, caret_y, caret_h;
    bool fg_used;  JSGCRef fg_cb;   /* sys.onForeground */
    bool bg_used;  JSGCRef bg_cb;   /* sys.onBackground */
    bool sig_used; JSGCRef sig_cb;  /* sys.onSignal */
    bool stop_used; JSGCRef stop_cb; /* sys.onStop(reason) — last words;
                                        does NOT keep an idle app alive */
    bool stopping;            /* app_stop in progress: bars re-entry and
                                 marks the worker unavailable to evict */
    bool clip_used; JSGCRef clip_cb; /* clipboard.onChange (P4d) */
    bool cam_used; JSGCRef cam_cb;  /* camera.scan one-shot result */
    bool http_used; JSGCRef http_cb; /* http.get one-shot result */
    bool fsreq_used; JSGCRef fsreq_cb; /* fs.request one-shot: fires with the
                                          grant token, or 0 when refused */
    bool fsop_used;  JSGCRef fsop_cb;  /* fs.format one-shot: fires with the
                                          result once its task is done */
    bool net_used;  JSGCRef net_cb;  /* net.onReady one-shot: the app's ticket
                                        in the network wait queue (fires once
                                        the link is up, then auto-releases) */

#ifdef ESP_PLATFORM
    esp_mqtt_client_handle_t mqtt;  /* per-app client: "mqjs-app-<slot>" */
    volatile bool mqtt_up;          /* broker session established */
#endif

    /* print sink line assembler, per app so lines never interleave.
       The split width bounds one on-screen console record (256B = 85
       CJK or 256 ASCII glyphs, comfortably past one 720px row). */
    char sink_line[256];
    size_t sink_len;
} MqjsWorker;

static MqjsWorker s_workers[MQJS_MAX_WORKERS];
static MqjsWorker *s_cur_wk;        /* app whose JS is on the C stack — the
                                     single biggest dividend of the serial
                                     model: every binding reads it */
static volatile int s_fg_worker = MQJS_WORKER_DEV; /* boot: dev app in front */
static volatile bool s_stop_req; /* dev-slot stop (task push / PC ^C) */
/* Battery shutdown (pwr_tab5): stop EVERY app with reason "battery" so each
   gets its onStop, then tell the caller it may cut power. Posted from the
   battery task, executed on js_task like every other context touch. */
static volatile bool s_stopall_req;
static void (*s_stopall_done)(void);
static bool s_shutting_down;
static int64_t s_run_deadline;   /* JS watchdog */

/* P4b: relaunchable app sources (embedded buffers, live forever).
   sys.launch(name) resolves here first; "launcher" is kept resident. */
typedef struct {
    bool used;
    bool trusted_system;
    char name[32];
    const char *src;
    size_t len;
} AppSource;
static AppSource s_app_sources[4];

static int64_t s_dev_retry_at;   /* next time to ask the dev provider */
static int64_t s_launcher_retry_at;
static char s_last_dev_name[32]; /* what the dev app called itself: lets
                                    the chip relaunch a stopped dev task
                                    by its real name (sys.launch falls
                                    back to the provider on a match) */

/* Phase 3: the dev task's natural-end auto-rerun is a policy bit on
   its App record (RESTART_ON_EXIT), not a runtime flag (was
   s_dev_hold). "Held" = explicitly stopped = record stopped with the
   bit cleared; a push or sys.start("dev") re-arms it. */
static bool dev_held(void)
{
    if (!s_last_dev_name[0])
        return false;
    const mqjs_app_snapshot_t *rec = mqjs_app_record_find(s_last_dev_name);
    return rec && rec->state == MQJS_APP_STOPPED &&
           !(rec->policy.flags & MQJS_APP_RESTART_ON_EXIT);
}

static void dev_rearm(void)
{
    if (s_last_dev_name[0])
        mqjs_app_record_set_policy(s_last_dev_name,
                                   MQJS_APP_RESTART_ON_EXIT, 0);
    s_dev_retry_at = 0; /* the scheduler asks the provider next pass */
}

/* the status-bar chip target: the previous foreground app, kept by NAME
   (a relaunch may land in a different slot) */
static char s_prev_name[32];

/* Network readiness as a capability (event-driven Wi-Fi, see wifi.c). The
   token is the ONLY proof the link is up: net.onReady hands it to its callback
   and mqtt.connect demands it, so there is no token to pass at top level — a
   connect before the link cannot be expressed, not merely caught. It doubles
   as the crash floor (a call with no/stale token is rejected before lwip) and
   as lifetime: bumped on every up edge, so a token from a prior link is stale.
   0 = never up. PC/host builds start "ready" (1) so smoke tests run. */
static volatile uint32_t s_net_token =
#ifdef ESP_PLATFORM
    0;
#else
    1;
#endif

/* Platform-configured default MQTT broker (injected by app_main from
   CONFIG_MQJS_TASK_BROKER). mqtt.connect(token) uses it, so apps never
   hardcode "mqtt://...". A literal stub on PC keeps smoke tests connecting. */
static const char *s_default_broker
#ifndef ESP_PLATFORM
    = "mqtt://pc-stub"
#endif
    ;

void mqjs_set_default_broker(const char *uri)
{
    s_default_broker = uri;   /* expected to be a static string (not copied) */
}

/* Platform topic namespace (injected by app_main from CONFIG_MQJS_TASK_TOPIC's
   first segment). net.topic(name) prepends it, so apps hold only the leaf name
   and never hardcode "esp32p4-mqjs/...". */
static const char *s_topic_prefix;

void mqjs_set_topic_prefix(const char *prefix)
{
    s_topic_prefix = prefix;  /* expected to be a static string (not copied) */
}

static void (*s_notify_sink)(const char *text);

void mqjs_set_notify_sink(void (*fn)(const char *text))
{
    s_notify_sink = fn;
}

/* P4c: last notification per app (name-keyed; survives the app's stop
   so "what was that about?" stays answerable from the launcher) */
typedef struct {
    char app[32];
    char text[96];
    uint32_t seq; /* 0 = empty; ordering for sys.notices() */
} Notice;
static Notice s_notices[8];
static uint32_t s_notice_seq;

void mqjs_register_app_source(const char *name, const char *src, size_t len)
{
    for (int i = 0; i < (int)(sizeof s_app_sources / sizeof s_app_sources[0]);
         i++) {
        AppSource *as = &s_app_sources[i];
        if (as->used && strcmp(as->name, name) != 0)
            continue;
        as->used = true;
        as->trusted_system = false;
        snprintf(as->name, sizeof as->name, "%s", name);
        as->src = src;
        as->len = len;
        return;
    }
}

void mqjs_register_system_app_source(const char *name, const char *src,
                                     size_t len)
{
    for (int i = 0; i < (int)(sizeof s_app_sources / sizeof s_app_sources[0]);
         i++) {
        AppSource *as = &s_app_sources[i];
        if (as->used && strcmp(as->name, name) != 0)
            continue;
        as->used = true;
        as->trusted_system = true;
        snprintf(as->name, sizeof as->name, "%s", name);
        as->src = src;
        as->len = len;
        return;
    }
}

static const AppSource *app_source_find(const char *name)
{
    for (int i = 0; i < (int)(sizeof s_app_sources / sizeof s_app_sources[0]);
         i++)
        if (s_app_sources[i].used && !strcmp(s_app_sources[i].name, name))
            return &s_app_sources[i];
    return NULL;
}

/* push the current/previous app names to the status-bar chip. Driven by
   the events that can change them (switch / start / stop / rename) —
   never polled (see design §4: liveness is checked at tap time only). */
static void bar_update(void)
{
#ifdef ESP_PLATFORM
    MqjsWorker *fg = &s_workers[s_fg_worker];
    bool prev_running = false;
    for (int i = 0; i < MQJS_MAX_WORKERS; i++)
        if (s_workers[i].used && !strcmp(s_workers[i].name, s_prev_name)) {
            prev_running = true;
            break;
        }
    ui_tab5_set_fg_apps(fg->used ? fg->name : "", s_prev_name, prev_running);
#endif
}

#ifdef ESP_PLATFORM
static QueueHandle_t s_event_queue;
static bool s_isr_service_installed;
#else
/* PC build: tiny in-process ring instead of a FreeRTOS queue, so
   sys.signal / sys.focus work in host smoke runs (single thread: the
   only producers are bindings called from the loop itself). */
static MqjsEvent s_pc_q[MQJS_QUEUE_LEN];
static int s_pc_q_head, s_pc_q_count;
#endif

static bool ev_post(const MqjsEvent *ev, int wait_ms)
{
#ifdef ESP_PLATFORM
    if (!s_event_queue)
        return false;
    return xQueueSend(s_event_queue, ev, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
#else
    (void)wait_ms;
    if (s_pc_q_count == MQJS_QUEUE_LEN)
        return false;
    s_pc_q[(s_pc_q_head + s_pc_q_count) % MQJS_QUEUE_LEN] = *ev;
    s_pc_q_count++;
    return true;
#endif
}

/* got-IP: mint a fresh token (invalidating any prior one) and post one
   broadcast so the JS task drains the net.onReady wait queue. Callable from
   another task (enqueue only; the token int is written atomically enough). */
void mqjs_notify_net_up(void)
{
    if (++s_net_token == 0)   /* 0 stays reserved for "never up" */
        s_net_token = 1;
    MqjsEvent ev = { .type = EV_NET };
    ev_post(&ev, 0);
}

#ifndef ESP_PLATFORM
static bool pc_q_recv(MqjsEvent *ev)
{
    if (!s_pc_q_count)
        return false;
    *ev = s_pc_q[s_pc_q_head];
    s_pc_q_head = (s_pc_q_head + 1) % MQJS_QUEUE_LEN;
    s_pc_q_count--;
    return true;
}
#endif

/* ------------------------------------------------------------------ */
/* print sink (tee of all JS-visible output, assembled into lines)     */
/* ------------------------------------------------------------------ */

static void (*s_print_sink)(const char *, size_t);

/* output produced outside any app dispatch (early init etc.) */
static char s_orphan_line[256];
static size_t s_orphan_len;

void mqjs_set_print_sink(void (*fn)(const char *, size_t))
{
    s_print_sink = fn;
}

/* §11 store catalog provider + uninstall unsubscribe hook (both
   host-registered; NULL on the PC build) */
static const mqjs_store_api_t *s_store_api;
/* 同じ契約の第 2 のカタログ: microSD 上の署名済みアプリ。棚 (MQTT) と
   混ぜずに別ソースとして並べる —— 同名衝突は勝者を決めずに両方見せる
   (docs/filer-storage-design.md §13)。 */
static const mqjs_store_api_t *s_card_api;
static void (*s_uninstall_hook)(const char *name);

void mqjs_set_card_provider(const mqjs_store_api_t *api)
{
    s_card_api = api;
}

void mqjs_set_store_provider(const mqjs_store_api_t *api)
{
    s_store_api = api;
}

void mqjs_set_uninstall_hook(void (*fn)(const char *name))
{
    s_uninstall_hook = fn;
}

/* Flush one assembled line to the sink. Non-dev apps get a "[name] "
   prefix so the shared console stays attributable (§3.5). */
static void term_sink_line(const char *writer, term_wclass_t wc,
                           const char *line, size_t len);

static void sink_flush(void)
{
    MqjsWorker *app = s_cur_wk;
    char *line = app ? app->sink_line : s_orphan_line;
    size_t *plen = app ? &app->sink_len : &s_orphan_len;
    /* §3.1's second sink: the same assembled line also goes to the term
       registry's console AND to the §4.4 LP black box, tagged with the app
       that wrote it ({writer_id, class}). Independent of s_print_sink: the
       console screen must work whether or not the host wired a status-bar
       sink. The class is decided HERE because this is where the writer's
       trust level is known: platform code with no app on the stack and
       embedded system apps go to §4.4's 8KB system partition, every other
       app to the 23KB app partition, so a chatty app cannot flush the
       platform's last words. */
    if (*plen)
        term_sink_line(app && app->name[0] ? app->name : "system",
                       (!app || app->trusted_system) ? TERM_WCLASS_SYSTEM
                                                     : TERM_WCLASS_APP,
                       line, *plen);
    if (s_print_sink && *plen) {
        if (app && app->idx != MQJS_WORKER_DEV && app->name[0]) {
            char buf[sizeof(app->sink_line) + sizeof(app->name) + 4];
            int n = snprintf(buf, sizeof buf, "[%s] ", app->name);
            memcpy(buf + n, line, *plen);
            s_print_sink(buf, (size_t)n + *plen);
        } else {
            s_print_sink(line, *plen);
        }
    }
    *plen = 0;
}

static void out_write(const void *buf, size_t len)
{
    fwrite(buf, 1, len, stdout);
    /* Line assembly runs whether or not a status-bar sink was installed:
       sink_flush() has three consumers now (the status bar, the term console
       and §4.4's LP black box), and making the flight recorder depend on
       whether the host wired up a UI sink would be an accidental coupling —
       the one build without that sink (run_pc) is also the one where the
       tee most needs to be testable. */
    MqjsWorker *app = s_cur_wk;
    char *line = app ? app->sink_line : s_orphan_line;
    size_t *plen = app ? &app->sink_len : &s_orphan_len;
    size_t cap = app ? sizeof(app->sink_line) : sizeof(s_orphan_line);
    const char *p = buf;
    size_t i = 0;
    while (i < len) {
        if (p[i] == '\n') {
            sink_flush();
            i++;
            continue;
        }
        /* bulk-copy up to the next newline (a per-byte loop here made
           ANSI-animation apps pay milliseconds per frame — hotspot
           audit §2.2) */
        const char *nl = memchr(p + i, '\n', len - i);
        size_t chunk = nl ? (size_t)(nl - (p + i)) : len - i;
        while (chunk) {
            size_t space = cap - *plen;
            if (space == 0) {
                /* split overlong lines at a UTF-8 sequence boundary */
                size_t cut = *plen;
                while (cut > 0 && (line[cut - 1] & 0xC0) == 0x80)
                    cut--;
                if (cut > 0 && (line[cut - 1] & 0x80))
                    cut--; /* drop the lead byte of the split too */
                if (cut == 0)
                    cut = *plen;
                size_t rest = *plen - cut;
                char carry[4];
                memcpy(carry, line + cut, rest);
                *plen = cut;
                sink_flush();
                memcpy(line, carry, rest);
                *plen = rest;
                continue;
            }
            size_t n = chunk < space ? chunk : space;
            memcpy(line + *plen, p + i, n);
            *plen += n;
            i += n;
            chunk -= n;
        }
    }
}

static void js_log_func(void *opaque, const void *buf, size_t buf_len)
{
    out_write(buf, buf_len);
}

/* ------------------------------------------------------------------ */
/* watchdog: abort runaway JS                                          */
/* ------------------------------------------------------------------ */

static int js_interrupt_handler(JSContext *ctx, void *opaque)
{
    return time_ms() > s_run_deadline;
}

static void arm_watchdog(void)
{
    s_run_deadline = time_ms() + MQJS_MAX_RUN_MS;
}

/* ------------------------------------------------------------------ */
/* error reporting                                                     */
/* ------------------------------------------------------------------ */

static void dump_error(JSContext *ctx)
{
    JSValue e = JS_GetException(ctx);
    static const char pfx[] = "mqjs: uncaught exception: ";
    out_write(pfx, sizeof(pfx) - 1);
    JS_PrintValueF(ctx, e, JS_DUMP_LONG); /* goes through js_log_func */
    out_write("\n", 1);
}

/* ------------------------------------------------------------------ */
/* shared callback helpers                                             */
/* ------------------------------------------------------------------ */

/* replace-register one persistent callback (ui.onTouch idiom) */
static JSValue register_cb(JSContext *ctx, JSValue fn, bool *used,
                           JSGCRef *ref)
{
    if (!JS_IsFunction(ctx, fn))
        return JS_ThrowTypeError(ctx, "not a function");
    if (*used)
        JS_DeleteGCRef(ctx, ref);
    JSValue *pf = JS_AddGCRef(ctx, ref);
    *pf = fn;
    *used = true;
    return JS_UNDEFINED;
}

/* call a no-arg persistent callback on `app` (lifecycle hooks) */
static void app_call0(MqjsWorker *app, JSGCRef *fn)
{
    MqjsWorker *prev = s_cur_wk;
    s_cur_wk = app;
    if (JS_StackCheck(app->ctx, 2)) {
        dump_error(app->ctx);
    } else {
        JS_PushArg(app->ctx, fn->val);  /* func */
        JS_PushArg(app->ctx, JS_NULL);  /* this */
        arm_watchdog();
        JSValue ret = JS_Call(app->ctx, 0);
        if (JS_IsException(ret))
            dump_error(app->ctx);
    }
    s_cur_wk = prev;
}

/* ------------------------------------------------------------------ */
/* print (also used by console.log via the stdlib table)               */
/* ------------------------------------------------------------------ */

JSValue js_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    for (int i = 0; i < argc; i++) {
        if (i != 0)
            out_write(" ", 1);
        JSValue v = argv[i];
        if (JS_IsString(ctx, v)) {
            JSCStringBuf buf;
            size_t len;
            const char *str = JS_ToCStringLen(ctx, &len, v, &buf);
            out_write(str, len);
        } else {
            JS_PrintValueF(ctx, v, JS_DUMP_LONG);
        }
    }
    out_write("\n", 1);
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ */
/* Date / performance (referenced by the stdlib table)                 */
/* ------------------------------------------------------------------ */

JSValue js_date_constructor(JSContext *ctx, JSValue *this_val,
                            int argc, JSValue *argv)
{
    double val;
    argc &= ~FRAME_CF_CTOR;
    if (argc == 0) {
        val = (double)date_ms();
    } else if (argc == 1 && JS_IsNumber(ctx, argv[0])) {
        if (JS_ToNumber(ctx, &val, argv[0]))
            return JS_EXCEPTION;
    } else {
        return JS_ThrowTypeError(ctx, "unsupported Date() parameter");
    }
    return JS_NewDate(ctx, val);
}

JSValue js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return JS_NewInt64(ctx, date_ms());
}

JSValue js_performance_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return JS_NewInt64(ctx, time_ms());
}

/* ------------------------------------------------------------------ */
/* timers (per app)                                                    */
/* ------------------------------------------------------------------ */

static JSValue set_timer(JSContext *ctx, JSValue *argv, bool repeat)
{
    int delay;

    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    if (JS_ToInt32(ctx, &delay, argv[1]))
        return JS_EXCEPTION;
    if (delay < 0)
        delay = 0;
    if (repeat && delay < 1)
        delay = 1;

    for (int i = 0; i < MQJS_MAX_TIMERS; i++) {
        TimerSlot *t = &s_cur_wk->timers[i];
        if (!t->used) {
            JSValue *pf = JS_AddGCRef(ctx, &t->fn);
            *pf = argv[0];
            t->repeat = repeat;
            t->period_ms = delay;
            t->deadline = time_ms() + delay;
            t->used = true;
            return JS_NewInt32(ctx, i);
        }
    }
    return JS_ThrowInternalError(ctx, "too many timers");
}

JSValue js_setTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return set_timer(ctx, argv, false);
}

JSValue js_setInterval(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return set_timer(ctx, argv, true);
}

JSValue js_clearTimer(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int id;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (id >= 0 && id < MQJS_MAX_TIMERS && s_cur_wk->timers[id].used) {
        JS_DeleteGCRef(ctx, &s_cur_wk->timers[id].fn);
        s_cur_wk->timers[id].used = false;
    }
    return JS_UNDEFINED;
}

/* NB: delay() blocks the WHOLE JS task — under multi-app it stalls every
   other app too. Kept for dev-slot compatibility (README warns). */
JSValue js_delay(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int ms;
    if (JS_ToInt32(ctx, &ms, argv[0]))
        return JS_EXCEPTION;
    if (ms < 0)
        ms = 0;
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    usleep((useconds_t)ms * 1000);
#endif
    return JS_UNDEFINED;
}

/* Run all expired timers of ONE app. Returns the delay in ms until its
   next timer (or `idle_max` if none is pending sooner). */
static int run_timers(MqjsWorker *app, int idle_max)
{
    JSContext *ctx = app->ctx;
    int64_t now = time_ms();
    int min_delay = idle_max;

    for (int i = 0; i < MQJS_MAX_TIMERS; i++) {
        TimerSlot *t = &app->timers[i];
        if (!t->used)
            continue;

        int64_t delta = t->deadline - now;
        if (delta > 0) {
            if (delta < min_delay)
                min_delay = (int)delta;
            continue;
        }

        /* expired: push args before (possibly) releasing the ref so the
           function stays rooted on the JS stack during the call */
        if (JS_StackCheck(ctx, 2)) {
            dump_error(ctx);
            continue;
        }
        JS_PushArg(ctx, t->fn.val);  /* func */
        JS_PushArg(ctx, JS_NULL);    /* this */

        if (t->repeat) {
            t->deadline = now + t->period_ms;
        } else {
            JS_DeleteGCRef(ctx, &t->fn);
            t->used = false;
        }

        arm_watchdog();
        JSValue ret = JS_Call(ctx, 0);
        if (JS_IsException(ret)) {
            dump_error(ctx);
            if (t->used) {           /* misbehaving interval: cancel it */
                JS_DeleteGCRef(ctx, &t->fn);
                t->used = false;
            }
        }
        min_delay = 0;               /* re-scan immediately */
    }
    return min_delay;
}

/* One scheduler pass over every app's timers (§3.7 step 1). */
static int run_all_timers(int idle_max)
{
    int min_delay = idle_max;
    for (int i = 0; i < MQJS_MAX_WORKERS; i++) {
        if (!s_workers[i].used)
            continue;
        s_cur_wk = &s_workers[i];
        int d = run_timers(&s_workers[i], idle_max);
        s_cur_wk = NULL;
        if (d < min_delay)
            min_delay = d;
    }
    return min_delay;
}

/* ------------------------------------------------------------------ */
/* gpio                                                                */
/* ------------------------------------------------------------------ */

/* mode values must match gpio.IN / gpio.OUT / ... in device_stdlib.c */
enum { MODE_IN = 0, MODE_OUT = 1, MODE_IN_PULLUP = 2, MODE_IN_PULLDOWN = 3 };

JSValue js_gpio_setMode(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int pin, mode;
    if (JS_ToInt32(ctx, &pin, argv[0]) || JS_ToInt32(ctx, &mode, argv[1]))
        return JS_EXCEPTION;

#ifdef ESP_PLATFORM
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = (mode == MODE_OUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
        .pull_up_en = (mode == MODE_IN_PULLUP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (mode == MODE_IN_PULLDOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK)
        return JS_ThrowTypeError(ctx, "gpio.setMode failed");
#else
    printf("[gpio] setMode(pin=%d, mode=%d)\n", pin, mode);
#endif
    return JS_UNDEFINED;
}

JSValue js_gpio_write(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int pin, level;
    if (JS_ToInt32(ctx, &pin, argv[0]) || JS_ToInt32(ctx, &level, argv[1]))
        return JS_EXCEPTION;
#ifdef ESP_PLATFORM
    gpio_set_level(pin, level != 0);
#else
    printf("[gpio] write(pin=%d, level=%d)\n", pin, level != 0);
#endif
    return JS_UNDEFINED;
}

JSValue js_gpio_read(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int pin;
    if (JS_ToInt32(ctx, &pin, argv[0]))
        return JS_EXCEPTION;
#ifdef ESP_PLATFORM
    return JS_NewInt32(ctx, gpio_get_level(pin));
#else
    return JS_NewInt32(ctx, 0);
#endif
}

#ifdef ESP_PLATFORM
/* ISR: post to queue only. NEVER call into JS from here. The owning app
   is resolved at dispatch time via the pin -> slot registration scan. */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    MqjsEvent ev = {
        .type = EV_GPIO,
        .u.gpio = {
            .pin = (uint8_t)(intptr_t)arg,
            .level = (uint8_t)gpio_get_level((gpio_num_t)(intptr_t)arg),
        },
    };
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_event_queue, &ev, &hp);
    if (hp)
        portYIELD_FROM_ISR();
}
#endif

JSValue js_gpio_onChange(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int pin;
    if (JS_ToInt32(ctx, &pin, argv[0]))
        return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "not a function");

    /* a pin has one owner across ALL apps (shared hardware) */
    for (int a = 0; a < MQJS_MAX_WORKERS; a++) {
        if (!s_workers[a].used)
            continue;
        for (int i = 0; i < MQJS_MAX_GPIO_CB; i++) {
            GpioSlot *g = &s_workers[a].gpio_cb[i];
            if (g->used && g->pin == pin)
                return JS_ThrowTypeError(ctx, "pin already has a handler");
        }
    }
    for (int i = 0; i < MQJS_MAX_GPIO_CB; i++) {
        GpioSlot *g = &s_cur_wk->gpio_cb[i];
        if (!g->used) {
            JSValue *pf = JS_AddGCRef(ctx, &g->fn);
            *pf = argv[1];
            g->pin = pin;
            g->used = true;
#ifdef ESP_PLATFORM
            if (!s_isr_service_installed) {
                gpio_install_isr_service(0);
                s_isr_service_installed = true;
            }
            gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
            gpio_isr_handler_add(pin, gpio_isr_handler, (void *)(intptr_t)pin);
#else
            printf("[gpio] onChange(pin=%d) registered (stub: never fires on PC)\n", pin);
#endif
            return JS_UNDEFINED;
        }
    }
    return JS_ThrowInternalError(ctx, "too many gpio handlers");
}

static void dispatch_gpio_event(MqjsWorker *app, const MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    for (int i = 0; i < MQJS_MAX_GPIO_CB; i++) {
        GpioSlot *g = &app->gpio_cb[i];
        if (!g->used || g->pin != ev->u.gpio.pin)
            continue;
        if (JS_StackCheck(ctx, 3)) {
            dump_error(ctx);
            return;
        }
        JS_PushArg(ctx, JS_NewInt32(ctx, ev->u.gpio.level)); /* arg0 */
        JS_PushArg(ctx, g->fn.val);                          /* func */
        JS_PushArg(ctx, JS_NULL);                            /* this */
        arm_watchdog();
        JSValue ret = JS_Call(ctx, 1);
        if (JS_IsException(ret))
            dump_error(ctx);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* i2c (synchronous; transactions are sub-ms so they run inline).      */
/* Buses are SHARED hardware: they stay global and survive app stops   */
/* (like gpio pin config).                                             */
/* ------------------------------------------------------------------ */

#define MQJS_I2C_PORTS    2
#define MQJS_I2C_MAX_READ 32

#ifdef ESP_PLATFORM
static i2c_master_bus_handle_t s_i2c_bus[MQJS_I2C_PORTS];
static uint32_t s_i2c_hz[MQJS_I2C_PORTS];

/* buses survive script restarts (like gpio pin config); setup()
   tears down and recreates the port it is given */
static int i2c_arg_port(JSContext *ctx, JSValue v, JSValue *err)
{
    int port;
    if (JS_ToInt32(ctx, &port, v)) {
        *err = JS_EXCEPTION;
        return -1;
    }
    if (port < 0 || port >= MQJS_I2C_PORTS || !s_i2c_bus[port]) {
        *err = JS_ThrowTypeError(ctx, "i2c port not set up");
        return -1;
    }
    return port;
}
#endif

JSValue js_i2c_setup(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int port, sda, scl, hz = 400000;
    if (JS_ToInt32(ctx, &port, argv[0]) || JS_ToInt32(ctx, &sda, argv[1]) ||
        JS_ToInt32(ctx, &scl, argv[2]))
        return JS_EXCEPTION;
    if (argc >= 4 && !JS_IsUndefined(argv[3]) && JS_ToInt32(ctx, &hz, argv[3]))
        return JS_EXCEPTION;
#ifdef ESP_PLATFORM
    if (port < 0 || port >= MQJS_I2C_PORTS)
        return JS_ThrowTypeError(ctx, "bad i2c port");
    if (s_i2c_bus[port]) {
        i2c_del_master_bus(s_i2c_bus[port]);
        s_i2c_bus[port] = NULL;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&cfg, &s_i2c_bus[port]) != ESP_OK)
        return JS_ThrowInternalError(ctx, "i2c bus init failed");
    s_i2c_hz[port] = (uint32_t)hz;
#else
    printf("[i2c] setup(port=%d, sda=%d, scl=%d, hz=%d) (stub)\n",
           port, sda, scl, hz);
#endif
    return JS_UNDEFINED;
}

#ifdef ESP_PLATFORM
/* Address probe: does anything ACK at `addr`?
 *
 * Deliberately NOT i2c_master_probe(). That IDF entry point (6.0.1, and
 * still on master) points bus->i2c_trans.ops at its own 2-entry STACK
 * array and — unlike every real transaction — never resets read_buf_pos
 * / read_len_static / contains_read. A read left half-finished by a
 * timeout therefore makes the probe's completion IRQ index that dead
 * stack array out of bounds and store RX FIFO bytes through the garbage
 * it finds (Store access fault in i2c_master.c:766). We hit exactly that
 * on the keyboard dock bus; see components/kbd_tab5/kbd_tab5.c.
 *
 * A 1-byte read is the safe equivalent: it runs the ordinary synchronous
 * path (driver-owned ops array, ISR read state reset up front). Devices
 * that ACK their address but NACK a bare read report as absent, which is
 * the accepted cost of not panicking. */
static bool i2c_addr_present(int port, int addr)
{
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)addr,
        .scl_speed_hz = s_i2c_hz[port],
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(s_i2c_bus[port], &dc, &dev) != ESP_OK)
        return false;
    uint8_t b;
    esp_err_t e = i2c_master_receive(dev, &b, 1, 20);
    i2c_master_bus_rm_device(dev);
    return e == ESP_OK;
}
#endif

JSValue js_i2c_scan(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSValue arr = JS_NewArray(ctx, 0);
#ifdef ESP_PLATFORM
    JSValue err;
    int port = i2c_arg_port(ctx, argv[0], &err);
    if (port < 0)
        return err;
    int n = 0;
    for (int addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_addr_present(port, addr))
            JS_SetPropertyUint32(ctx, arr, n++, JS_NewInt32(ctx, addr));
    }
#else
    printf("[i2c] scan (stub: empty)\n");
#endif
    return arr;
}

#ifdef ESP_PLATFORM
/* one-shot device handle around a register transaction */
static esp_err_t i2c_reg_xfer(int port, int addr,
                              const uint8_t *wr, size_t wrlen,
                              uint8_t *rd, size_t rdlen)
{
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)addr,
        .scl_speed_hz = s_i2c_hz[port],
    };
    i2c_master_dev_handle_t dev;
    esp_err_t e = i2c_master_bus_add_device(s_i2c_bus[port], &dc, &dev);
    if (e != ESP_OK)
        return e;
    if (rdlen)
        e = i2c_master_transmit_receive(dev, wr, wrlen, rd, rdlen, 100);
    else
        e = i2c_master_transmit(dev, wr, wrlen, 100);
    i2c_master_bus_rm_device(dev);
    return e;
}
#endif

JSValue js_i2c_readReg(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int addr, reg, n;
    if (JS_ToInt32(ctx, &addr, argv[1]) || JS_ToInt32(ctx, &reg, argv[2]) ||
        JS_ToInt32(ctx, &n, argv[3]))
        return JS_EXCEPTION;
    if (n < 1 || n > MQJS_I2C_MAX_READ)
        return JS_ThrowTypeError(ctx, "read length 1..32");

    uint8_t buf[MQJS_I2C_MAX_READ] = { 0 };
#ifdef ESP_PLATFORM
    JSValue err;
    int port = i2c_arg_port(ctx, argv[0], &err);
    if (port < 0)
        return err;
    uint8_t r = (uint8_t)reg;
    if (i2c_reg_xfer(port, addr, &r, 1, buf, (size_t)n) != ESP_OK)
        return JS_ThrowInternalError(ctx, "i2c read failed");
#else
    printf("[i2c] readReg(addr=0x%02x, reg=0x%02x, n=%d) (stub: zeros)\n",
           addr, reg, n);
#endif
    JSValue arr = JS_NewArray(ctx, 0);
    for (int i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, buf[i]));
    return arr;
}

JSValue js_i2c_writeReg(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int addr, reg;
    if (JS_ToInt32(ctx, &addr, argv[1]) || JS_ToInt32(ctx, &reg, argv[2]))
        return JS_EXCEPTION;

    /* up to 8 data bytes, passed variadically after reg */
    uint8_t wr[1 + 8];
    wr[0] = (uint8_t)reg;
    int nd = 0;
    for (int i = 3; i < argc && nd < 8; i++, nd++) {
        int b;
        if (JS_IsUndefined(argv[i]))
            break;
        if (JS_ToInt32(ctx, &b, argv[i]))
            return JS_EXCEPTION;
        wr[1 + nd] = (uint8_t)b;
    }
#ifdef ESP_PLATFORM
    JSValue err;
    int port = i2c_arg_port(ctx, argv[0], &err);
    if (port < 0)
        return err;
    if (i2c_reg_xfer(port, addr, wr, (size_t)(1 + nd), NULL, 0) != ESP_OK)
        return JS_ThrowInternalError(ctx, "i2c write failed");
#else
    printf("[i2c] writeReg(addr=0x%02x, reg=0x%02x, %d data bytes) (stub)\n",
           addr, reg, nd);
#endif
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ */
/* ui (Tab5 on-device canvas; silent no-op on UI-less devices so the   */
/* same script runs on Stamp; PC build = print-only stubs).            */
/* P4a: the screen belongs to the FOREGROUND app only — background     */
/* apps' ui.* mutations are silent no-ops with dummy returns (§3.3);   */
/* query calls (size/textSize/cellSize) still answer.                  */
/* ------------------------------------------------------------------ */

#ifndef ESP_PLATFORM
/* mirror of ui_cmd_op_t in ui_tab5.h (not includable on PC) */
typedef enum {
    UI_CMD_CLEAR = 0, UI_CMD_FILL, UI_CMD_RECT,
    UI_CMD_LINE, UI_CMD_TEXT, UI_CMD_PIXEL, UI_CMD_KEYBOARD,
    UI_CMD_CELLS, UI_CMD_SCROLL, UI_CMD_OVERLAY, UI_CMD_RESET,
} ui_cmd_op_t;
/* keep in step with ui_tab5.h — the order IS the wire format */
#define UI_OVERLAY_MAX   4
#define UI_OVERLAY_ITEMS 10
#define UI_OVERLAY_IME   UI_OVERLAY_MAX
#define UI_OVERLAY_SLOTS (UI_OVERLAY_MAX + 1)
/* PC stub of the deferred screen-load commit (§3.4) */
#define ui_tab5_w_commit() ((void)0)
#endif

/* Is the calling app allowed to touch the screen? (No app on the C
   stack = runtime-internal call: allowed.) */
static bool ui_is_fg(void)
{
    return !s_cur_wk || s_cur_wk->idx == s_fg_worker;
}

/* Put one command on the UI queue, unconditionally. Takes ownership of
   `text` in every outcome.

   プラットフォーム発の描画 (IME のフロート) 専用の入口でもある: あれは
   IME の所有タスクから出るので、そこから見た s_cur_wk は「mqjs タスクが
   いまどのアプリの JS を回しているか」という別タスクの作業変数でしかなく、
   fg 判定に使うと裏で走ったバックグラウンドアプリのタイマー次第で
   preedit が消える。 */
static void ui_send(uint8_t op, int x, int y, int w, int h,
                    uint32_t color, uint32_t bg, char *text)
{
#ifdef ESP_PLATFORM
    ui_cmd_t c = {
        .op = op,
        .x = (int16_t)x, .y = (int16_t)y,
        .w = (int16_t)w, .h = (int16_t)h,
        .color = color,
        .bg = bg,
        .text = text,
    };
    if (!ui_tab5_cmd(&c))
        free(text);
#else
    static const char *names[] =
        { "clear", "fill", "rect", "line", "text", "pixel", "keyboard",
          "cells", "scroll", "overlay", "reset",
          "fieldkey" }; /* order == ui_cmd_op_t */
    printf("[ui] %s(x=%d, y=%d, w=%d, h=%d, fg=0x%06x, bg=0x%06x%s%s) (stub)\n",
           names[op], x, y, w, h, (unsigned)color, (unsigned)bg,
           text ? ", " : "", text ? text : "");
    free(text);
#endif
}

/* Post one drawing command from an app. Takes ownership of `text` (heap
   copy) in every outcome; drops are counted on-screen by the UI itself.
   `bg` is only used by UI_CMD_CELLS (cell background) and the overlay
   flags. Background apps: no-op. */
static void ui_post_bg(uint8_t op, int x, int y, int w, int h,
                       uint32_t color, uint32_t bg, char *text)
{
    if (!ui_is_fg()) {
        free(text);
        return;
    }
    ui_send(op, x, y, w, h, color, bg, text);
}

static void ui_post(uint8_t op, int x, int y, int w, int h,
                    uint32_t color, char *text)
{
    ui_post_bg(op, x, y, w, h, color, 0, text);
}

JSValue js_ui_size(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int w, h;
#ifdef ESP_PLATFORM
    ui_tab5_canvas_size(&w, &h);
#else
    w = 720; /* Tab5 canvas dimensions, so PC runs exercise real code paths */
    h = 1192;
#endif
    JSValue arr = JS_NewArray(ctx, 0);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, w));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, h));
    return arr;
}

/* Pixel size of a string in the canvas font: [w, h]. Synchronous query
   (not a queued command); [0, 0] without a screen, like ui.size(). The
   JS terminal uses this to derive its character grid. */
JSValue js_ui_textSize(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSCStringBuf buf;
    size_t len;
    const char *str = JS_ToCStringLen(ctx, &len, argv[0], &buf);
    if (!str)
        return JS_EXCEPTION;
    int w = 0, h = 0;
#ifdef ESP_PLATFORM
    ui_tab5_text_size(str, &w, &h);
#else
    /* stub: roughly the device font (Noto 20px), halfwidth 10px,
       fullwidth 20px, one 25px line — keeps grid math exercisable */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if ((c & 0xC0) == 0x80)
            continue; /* UTF-8 continuation */
        w += (c < 0x80) ? 10 : 20;
    }
    h = 25;
#endif
    JSValue arr = JS_NewArray(ctx, 0);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, w));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, h));
    return arr;
}

/* Cell size [w, h] of the monospace terminal-grid font (ui.cells).
   [0,0] without a screen. The JS terminal derives cols/rows from this. */
JSValue js_ui_cellSize(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int w = 0, h = 0;
#ifdef ESP_PLATFORM
    ui_tab5_cell_size(&w, &h);
#else
    w = 9; /* matches font_term_mono (HackGen size17): 9x24, 80 cols on 720 */
    h = 24;
#endif
    JSValue arr = JS_NewArray(ctx, 0);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, w));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, h));
    return arr;
}

/* ui.cellWidth(codePoint): compact wcwidth-like classification for grid
   UIs. The table is ui_cell_width() in ui_cell_width.h, shared verbatim
   with the ui.cells glyph blitter — see that header for why. */
JSValue js_ui_cellWidth(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int v; /* JS_ToInt32 takes int* — int32_t is long on riscv32 */
    if (JS_ToInt32(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, ui_cell_width((uint32_t)v));
}

/* ui.cells(col, row, str, fg, bg): draw a monospace run of cells with a
   single fg/bg (the JS terminal splits each row into same-color runs).

   CONTRACT: one codepoint per column. A width-2 codepoint (ui.cellWidth
   == 2) must be followed by a filler codepoint — a space — for the
   column it covers; ssh_vt.js calls that model cell CONT. The renderer
   widens the glyph's clip box to two cells but still advances one
   column per codepoint, so a caller that omits the filler gets the
   wide glyph's right half painted over its neighbour instead of a
   whole line shifted right. */
JSValue js_ui_cells(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int col, row, fg = 0xFFFFFF, bg = 0;
    if (JS_ToInt32(ctx, &col, argv[0]) || JS_ToInt32(ctx, &row, argv[1]))
        return JS_EXCEPTION;
    JSCStringBuf buf;
    size_t len;
    const char *str = JS_ToCStringLen(ctx, &len, argv[2], &buf);
    if (!str)
        return JS_EXCEPTION;
    if (argc >= 4 && !JS_IsUndefined(argv[3]) && JS_ToInt32(ctx, &fg, argv[3]))
        return JS_EXCEPTION;
    if (argc >= 5 && !JS_IsUndefined(argv[4]) && JS_ToInt32(ctx, &bg, argv[4]))
        return JS_EXCEPTION;
    char *copy = malloc(len + 1);
    if (!copy)
        return JS_ThrowInternalError(ctx, "out of memory");
    memcpy(copy, str, len);
    copy[len] = '\0';
    ui_post_bg(UI_CMD_CELLS, col, row, 0, 0, (uint32_t)fg, (uint32_t)bg, copy);
    return JS_UNDEFINED;
}

/* ui.scroll(top, bot, n[, bg]): scroll cell-rows [top,bot] by n
   (n>0 up, n<0 down) in the canvas buffer; vacated rows filled with bg. */
JSValue js_ui_scroll(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int top, bot, n, bg = 0;
    if (JS_ToInt32(ctx, &top, argv[0]) || JS_ToInt32(ctx, &bot, argv[1]) ||
        JS_ToInt32(ctx, &n, argv[2]))
        return JS_EXCEPTION;
    if (argc >= 4 && !JS_IsUndefined(argv[3]) && JS_ToInt32(ctx, &bg, argv[3]))
        return JS_EXCEPTION;
    ui_post(UI_CMD_SCROLL, top, bot, n, 0, (uint32_t)bg, NULL);
    return JS_UNDEFINED;
}

static JSValue ui_fill_op(JSContext *ctx, int argc, JSValue *argv, uint8_t op)
{
    int color = 0;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) &&
        JS_ToInt32(ctx, &color, argv[0]))
        return JS_EXCEPTION;
    ui_post(op, 0, 0, 0, 0, (uint32_t)color, NULL);
    return JS_UNDEFINED;
}

JSValue js_ui_clear(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return ui_fill_op(ctx, argc, argv, UI_CMD_CLEAR);
}

JSValue js_ui_fill(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return ui_fill_op(ctx, argc, argv, UI_CMD_FILL);
}

JSValue js_ui_rect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int x, y, w, h, color;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]) ||
        JS_ToInt32(ctx, &w, argv[2]) || JS_ToInt32(ctx, &h, argv[3]) ||
        JS_ToInt32(ctx, &color, argv[4]))
        return JS_EXCEPTION;
    ui_post(UI_CMD_RECT, x, y, w, h, (uint32_t)color, NULL);
    return JS_UNDEFINED;
}

JSValue js_ui_line(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int x0, y0, x1, y1, color;
    if (JS_ToInt32(ctx, &x0, argv[0]) || JS_ToInt32(ctx, &y0, argv[1]) ||
        JS_ToInt32(ctx, &x1, argv[2]) || JS_ToInt32(ctx, &y1, argv[3]) ||
        JS_ToInt32(ctx, &color, argv[4]))
        return JS_EXCEPTION;
    ui_post(UI_CMD_LINE, x0, y0, x1, y1, (uint32_t)color, NULL);
    return JS_UNDEFINED;
}

JSValue js_ui_pixel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int x, y, color;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]) ||
        JS_ToInt32(ctx, &color, argv[2]))
        return JS_EXCEPTION;
    ui_post(UI_CMD_PIXEL, x, y, 0, 0, (uint32_t)color, NULL);
    return JS_UNDEFINED;
}

JSValue js_ui_text(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int x, y, color = 0xFFFFFF;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]))
        return JS_EXCEPTION;
    JSCStringBuf buf;
    size_t len;
    const char *str = JS_ToCStringLen(ctx, &len, argv[2], &buf);
    if (!str)
        return JS_EXCEPTION;
    if (argc >= 4 && !JS_IsUndefined(argv[3]) &&
        JS_ToInt32(ctx, &color, argv[3]))
        return JS_EXCEPTION;
    char *copy = malloc(len + 1);
    if (!copy)
        return JS_ThrowInternalError(ctx, "out of memory");
    memcpy(copy, str, len);
    copy[len] = '\0';
    ui_post(UI_CMD_TEXT, x, y, 0, 0, (uint32_t)color, copy);
    return JS_UNDEFINED;
}

/* touch: the UI task polls the controller and posts here; JS receives
   (x, y, kind) with kind 0=down 1=move 2=up in canvas coordinates.
   Registration is allowed in the background (the callback just sleeps
   until the app is foreground again). */
JSValue js_ui_onTouch(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    if (s_cur_wk->touch_used)
        JS_DeleteGCRef(ctx, &s_cur_wk->touch_cb); /* re-register replaces */
    JSValue *pf = JS_AddGCRef(ctx, &s_cur_wk->touch_cb);
    *pf = argv[0];
    s_cur_wk->touch_used = true;
#ifndef ESP_PLATFORM
    printf("[ui] onTouch registered (stub: never fires on PC)\n");
#endif
    return JS_UNDEFINED;
}

void mqjs_post_touch(int x, int y, int kind)
{
#ifdef ESP_PLATFORM
    /* feed the device idle clock first, and let it swallow the gesture
       that only woke a blanked screen (before the fg-handler check, so a
       tap wakes even when the foreground app has no touch handler). */
    if (mqjs_power_note_input(kind))
        return;
    MqjsWorker *fg = &s_workers[s_fg_worker]; /* touch always goes to the fg app */
    if (!s_event_queue || !fg->used || !fg->touch_used)
        return;
    MqjsEvent ev = {
        .type = EV_TOUCH,
        .u.touch = { .x = (int16_t)x, .y = (int16_t)y, .kind = (uint8_t)kind },
    };
    xQueueSend(s_event_queue, &ev, 0); /* full queue: drop, never block */
#else
    (void)x;
    (void)y;
    (void)kind;
#endif
}

static void dispatch_touch_event(MqjsWorker *app, const MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    if (!app->touch_used)
        return;
    if (JS_StackCheck(ctx, 5)) {
        dump_error(ctx);
        return;
    }
    /* args are pushed in reverse: the last-pushed one becomes arg0 */
    JS_PushArg(ctx, JS_NewInt32(ctx, ev->u.touch.kind)); /* arg2 */
    JS_PushArg(ctx, JS_NewInt32(ctx, ev->u.touch.y));    /* arg1 */
    JS_PushArg(ctx, JS_NewInt32(ctx, ev->u.touch.x));    /* arg0 */
    JS_PushArg(ctx, app->touch_cb.val);                  /* func */
    JS_PushArg(ctx, JS_NULL);                            /* this */
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 3);
    if (JS_IsException(ret))
        dump_error(ctx);
}

/* on-screen keyboard (Phase 4): ui.keyboard(mode) drives the LVGL
   keyboard overlay — 0 = hide, 1 = keyboard, 2 = keyboard + terminal
   control bar (T3a). Keys arrive via mqjs_post_key as short UTF-8
   strings ("\n" enter, "\b" backspace, "\x1b[C"/"\x1b[D" arrows);
   control-bar buttons send "\x00name" tokens (esc tab ctrl alt
   left/down/up/right f1..f12 copy paste) whose meaning is the app's
   business. Returns the px height the overlay reserves at the canvas
   bottom, so a terminal derives its grid without hardcoding it;
   ui.keyboard(-m) returns mode m's height without changing anything
   (the startup grid probe). */
/* ui.overlay(id, spec | null) — a small window floated over the canvas at
 * a point the app supplies (docs/ui-overlay-plan.md).
 *
 *   ui.overlay(0, { col, row } | { x, y, h },
 *                 lines: [...], items: [...], sel: n,
 *                 place: "auto"|"below"|"above", dir: "h"|"v" });
 *   ui.overlay(0, null);   // hide
 *
 * The app supplies the ANCHOR and nothing else about placement: clamping,
 * flipping above when the on-screen keyboard is in the way, and — the
 * reason this is not a JS helper — repairing what the window covered.
 * Drawing a float into the canvas means tracking the rows it dirtied,
 * because the cells renderer's dirty check is content-based and an
 * overdraw on unchanged content never goes away. LVGL composites, so
 * that class of bug cannot happen and no app carries the bookkeeping.
 *
 * The content travels as ONE heap string in the existing ui_cmd_t (there
 * is no richer payload and adding one would touch every command):
 *   line ("\1" line)* ["\2" item ("\1" item)*]
 */
#define MQJS_OVL_TEXT_MAX 512

static int uiw_copy_str(JSContext *ctx, JSValue v, char *dst, size_t cap);

static int ovl_append(char *dst, size_t cap, size_t *len, const char *s)
{
    size_t n = strlen(s);
    if (*len + n >= cap)
        return -1;
    memcpy(dst + *len, s, n);
    *len += n;
    dst[*len] = '\0';
    return 0;
}

/* Collect arr[0..] into `dst`, separated by \1. Non-strings are skipped
   rather than coerced: a stray object would otherwise be typed into the
   window as "[object Object]". */
/* How many entries an item array has, bounded. */
static int ovl_count(JSContext *ctx, JSValue arr, int cap)
{
    int n = 0;
    if (!JS_IsArray(ctx, arr))
        return 0;
    while (n < cap) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)n);
        if (JS_IsUndefined(v) || JS_IsNull(v))
            break;
        n++;
    }
    return n;
}

/* First item to show, per overlay handle. Only UI_OVERLAY_ITEMS labels
   exist, so a long candidate list has to be windowed — and the window
   has to be REMEMBERED, which is why this is state and not a pure
   function of (count, sel).
 *
 * The window moves only when the selection would leave it. Recentring on
 * every step was tried and is worse on the device: the highlight then
 * never moves, the list slides underneath it, and you cannot tell that
 * pressing Space did anything at all. Edge-triggered scrolling keeps the
 * highlight visibly walking left and right, and the list only jumps when
 * it has to.
 *
 * A fresh candidate set arrives with sel = 0, which is below any nonzero
 * window and therefore resets it — no explicit "new list" signal needed.
 *
 * The IME's own slot is windowed by the IME's owner task and the app
 * slots by the mqjs task; two tasks, but never the same element. */
static int s_ovl_first[UI_OVERLAY_SLOTS];

static int ovl_window(int id, int count, int sel, int visible)
{
    int first = s_ovl_first[id];

    if (count <= visible || sel < 0) {
        s_ovl_first[id] = 0;
        return 0;
    }
    if (first > count - visible)
        first = count - visible;
    if (first < 0)
        first = 0;
    if (sel < first)
        first = sel;                        /* stepped off the left edge */
    else if (sel >= first + visible)
        first = sel - visible + 1;          /* stepped off the right edge */
    s_ovl_first[id] = first;
    return first;
}

static int ovl_join(JSContext *ctx, JSValue arr, char *dst, size_t cap,
                    size_t *len, int from, int max)
{
    int wrote = 0;
    if (!JS_IsArray(ctx, arr))
        return 0;
    for (int i = from; i < from + max; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        if (JS_IsUndefined(v) || JS_IsNull(v))
            break;
        if (!JS_IsString(ctx, v))
            continue;
        char item[128];
        if (uiw_copy_str(ctx, v, item, sizeof item))
            return -1;
        if (wrote && ovl_append(dst, cap, len, "\1"))
            break;
        if (ovl_append(dst, cap, len, item))
            break;
        wrote++;
    }
    return wrote;
}

JSValue js_ui_overlay(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    int id = 0;

    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (id < 0 || id >= UI_OVERLAY_MAX)
        return JS_ThrowRangeError(ctx, "ui.overlay: id 0..%d",
                                  UI_OVERLAY_MAX - 1);

    JSValue spec = argc >= 2 ? argv[1] : JS_UNDEFINED;
    if (JS_IsUndefined(spec) || JS_IsNull(spec)) {
        ui_post(UI_CMD_OVERLAY, 0, 0, id, 0, 0, NULL); /* hide */
        return JS_UNDEFINED;
    }

    /* Anchor: pixels win when both are given, because an app that can
       give pixels (proportional text) cannot express its cursor in
       cells without lying about the width. */
    int cw = 9, ch = 24;
#ifdef ESP_PLATFORM
    ui_tab5_cell_size(&cw, &ch);
#endif
    if (cw <= 0) cw = 9;
    if (ch <= 0) ch = 24;

    int x = 0, y = 0, h = ch, tmp = 0;
    JSValue v = JS_GetPropertyStr(ctx, spec, "col");
    if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v))
        x = tmp * cw;
    v = JS_GetPropertyStr(ctx, spec, "row");
    if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v))
        y = tmp * ch;
    v = JS_GetPropertyStr(ctx, spec, "x");
    if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v))
        x = tmp;
    v = JS_GetPropertyStr(ctx, spec, "y");
    if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v))
        y = tmp;
    v = JS_GetPropertyStr(ctx, spec, "h");
    if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v) && tmp > 0)
        h = tmp;

    int sel = -1;
    v = JS_GetPropertyStr(ctx, spec, "sel");
    if (JS_IsNumber(ctx, v) && JS_ToInt32(ctx, &sel, v))
        return JS_EXCEPTION;

    char place[8], dir[4];
    if (uiw_copy_str(ctx, JS_GetPropertyStr(ctx, spec, "place"),
                     place, sizeof place))
        return JS_EXCEPTION;
    if (uiw_copy_str(ctx, JS_GetPropertyStr(ctx, spec, "dir"), dir, sizeof dir))
        return JS_EXCEPTION;
    uint32_t flags = 0;
    if (strcmp(place, "below") == 0) flags |= 1;
    else if (strcmp(place, "above") == 0) flags |= 2;
    if (dir[0] == 'v') flags |= 4;

    char body[MQJS_OVL_TEXT_MAX];
    size_t len = 0;
    body[0] = '\0';
    if (ovl_join(ctx, JS_GetPropertyStr(ctx, spec, "lines"), body,
                 sizeof body, &len, 0, 8) < 0)
        return JS_EXCEPTION;
    JSValue items = JS_GetPropertyStr(ctx, spec, "items");
    if (JS_IsArray(ctx, items)) {
        /* Slide the window so `sel` is inside it, and report sel relative
           to what we actually send — the drawing side only ever sees the
           window, so an absolute index would highlight the wrong chip. */
        int total = ovl_count(ctx, items, 256);
        int first = ovl_window(id, total, sel, UI_OVERLAY_ITEMS);
        if (sel >= 0)
            sel -= first;
        if (ovl_append(body, sizeof body, &len, "\2") == 0) {
            if (ovl_join(ctx, items, body, sizeof body, &len, first,
                         UI_OVERLAY_ITEMS) < 0)
                return JS_EXCEPTION;
        }
    }
    if (!body[0]) {
        ui_post(UI_CMD_OVERLAY, 0, 0, id, 0, 0, NULL); /* nothing to show */
        return JS_UNDEFINED;
    }

    /* ui_post_bg TAKES OWNERSHIP and the UI task free()s it, so this has
       to be a heap copy — handing it `body` (a stack array) made the UI
       task free a stack address, which corrupted the heap and blew up
       later in an unrelated allocation (tlsf block_next assert inside
       LVGL's invalidate path). Same contract as js_ui_cells. */
    size_t blen = strlen(body);
    char *copy = malloc(blen + 1);
    if (!copy)
        return JS_ThrowOutOfMemory(ctx);
    memcpy(copy, body, blen + 1);
    ui_post_bg(UI_CMD_OVERLAY, x, y, id, h, (uint32_t)sel, flags, copy);
    return JS_UNDEFINED;
}

/* ui.caret(x, y, h) — where this app's cursor is, in canvas pixels
 * (design §7). The one fact the platform cannot work out for itself:
 * the IME float exists so the eye does not have to move, so a fixed
 * position would defeat it.
 *
 * Call it when the cursor MOVES, not per keystroke — the value is only
 * read when the composition changes. h is the caret's height, so the
 * float can sit below the line instead of on top of it.
 *
 * Nothing is posted here: the float is issued by the IME's owner task,
 * which reads these three numbers at the moment it draws. */
JSValue js_ui_caret(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    int x, y, h = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]))
        return JS_EXCEPTION;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToInt32(ctx, &h, argv[2]))
        return JS_EXCEPTION;
    s_cur_wk->caret_x = (int16_t)x;
    s_cur_wk->caret_y = (int16_t)y;
    s_cur_wk->caret_h = (int16_t)h;
    return JS_UNDEFINED;
}

JSValue js_ui_keyboard(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int mode = 1;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) &&
        JS_ToInt32(ctx, &mode, argv[0]))
        return JS_EXCEPTION;
    /* negative = pure metric query: return the reserved height of
       mode |m| WITHOUT touching visibility. The show-then-hide probe
       a terminal would otherwise need at startup can straddle a
       render tick and flash the full keyboard for a frame. */
    bool query = mode < 0;
    if (query)
        mode = -mode;
    if (mode > 2)
        mode = 2;
    if (!query)
        ui_post(UI_CMD_KEYBOARD, mode, 0, 0, 0, 0, NULL);
#ifdef ESP_PLATFORM
    return JS_NewInt32(ctx, ui_tab5_kb_reserved(mode));
#else
    /* stub mirrors the Tab5 constants so PC runs exercise grid math */
    return JS_NewInt32(ctx, mode == 0 ? 0 : mode == 1 ? 400 : 480);
#endif
}

JSValue js_ui_onKey(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    if (s_cur_wk->key_used)
        JS_DeleteGCRef(ctx, &s_cur_wk->key_cb); /* re-register replaces */
    JSValue *pf = JS_AddGCRef(ctx, &s_cur_wk->key_cb);
    *pf = argv[0];
    s_cur_wk->key_used = true;
#ifndef ESP_PLATFORM
    printf("[ui] onKey registered (stub: never fires on PC)\n");
#endif
    return JS_UNDEFINED;
}

#ifdef ESP_PLATFORM
/* IME は打鍵の分配より手前で噛ませる。実体は辞書の解決と同じ場所に要るので
   下の辞書セクションにある。true = IME の所有タスクが引き取った
   (確定文字列の配達もそちらがやる)。 */
static bool ime_route_key(const char *utf8, size_t len);

/* 打鍵を fg アプリの ui.onKey へ流す、唯一の出口。IME を通した後の配達も
   ここなので IME 所有タスクからも呼ばれる — どのタスクから来ても触るのは
   s_event_queue だけなので、それで足りる。 */
static void key_to_app(const char *utf8, size_t len)
{
    /* シンクの振り分け (§7): widget の field にフォーカスがあれば行き先は
       その textarea、無ければ従来どおり JS アプリ。ここに置くのは、素通しの
       打鍵も IME の確定文字列も必ずこの 1 か所を通るから — 分配の手前に
       置くと「英字は field に入るが日本語は入らない」になる。 */
    if (ui_tab5_field_key(utf8, len))
        return;
    MqjsWorker *fg = &s_workers[s_fg_worker]; /* keys always go to the fg app */
    if (!s_event_queue || !fg->used || !fg->key_used)
        return;
    /* 1 イベントに入り切らない入力は捨てずに続きとして送る。IME の確定
       文字列は 1 打鍵で 8 バイトを軽く超え、落とすと入力が黙って消える
       (端末も textarea も順序さえ保てればよい)。切り口は必ず UTF-8 の
       文字境界 — コードポイントの途中で切ると JS には壊れた文字が届く。 */
    while (len) {
        MqjsEvent ev = { .type = EV_KEY };
        size_t n = len;
        if (n > sizeof ev.u.key.text) {
            n = sizeof ev.u.key.text;
            while (n && ((unsigned char)utf8[n] & 0xC0) == 0x80)
                n--;                   /* 継続バイトの手前まで戻す */
            if (n == 0)
                return;                /* 1 文字が入らない = 不正な UTF-8 */
        }
        memcpy(ev.u.key.text, utf8, n);
        ev.u.key.len = (uint8_t)n;
        xQueueSend(s_event_queue, &ev, 0); /* full queue: drop, never block */
        utf8 += n;
        len -= n;
    }
}
#endif

void mqjs_post_key(const char *utf8, size_t len)
{
#ifdef ESP_PLATFORM
    /* keys feed the device idle clock like touch does (matters for the
       keyboard dock: typing must keep the screen awake). kind 2 = a
       discrete event with no gesture to eat through; a key that wakes a
       blanked screen is swallowed here, exactly like the wake tap. */
    if (mqjs_power_note_input(2))
        return;
    MqjsWorker *fg = &s_workers[s_fg_worker]; /* keys always go to the fg app */
    if (!s_event_queue || !fg->used || !utf8 || len == 0)
        return;

    /* IME はここ (docs/keyboard-ime-unification.md §7)。両側とも理由がある。
       復帰キーの握り潰しの「後」: 画面を起こしただけのキーで変換を始めない。
       fg->key_used チェックの「前」: あの行は ui.onKey を登録していない
       アプリの打鍵をここで殺しており (launcher や reading のような widget
       専用アプリがそれ)、後ろに置くと「ドックの物理キーが field に入らない」
       という §2 の根っこをそのまま踏む。 */
    if (ime_route_key(utf8, len))
        return;   /* この打鍵の行き先は所有タスクが決める */

    key_to_app(utf8, len);
#else
    (void)utf8;
    (void)len;
#endif
}

static void dispatch_key_event(MqjsWorker *app, const MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    if (!app->key_used)
        return;
    if (JS_StackCheck(ctx, 3)) {
        dump_error(ctx);
        return;
    }
    JS_PushArg(ctx, JS_NewStringLen(ctx, ev->u.key.text,
                                    ev->u.key.len));     /* arg0 */
    JS_PushArg(ctx, app->key_cb.val);                    /* func */
    JS_PushArg(ctx, JS_NULL);                            /* this */
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 1);
    if (JS_IsException(ret))
        dump_error(ctx);
}

/* ------------------------------------------------------------------ */
/* ui widgets (W1-2/3, docs/widget-framework-design.md). JS handle     */
/* objects (UiScreen/UiWidget user classes, ROM protos defined in      */
/* device_stdlib.c) wrap C-side handles; creation/mutation runs        */
/* synchronously in ui_tab5_w_* under the LVGL lock, events come back  */
/* through EV_WIDGET. PC build = handle bookkeeping + print stubs so   */
/* widget scripts smoke-test on the host. Background apps get inert    */
/* (handle 0) objects — their old handles went stale when their        */
/* screens were destroyed on the foreground switch.                    */
/* ------------------------------------------------------------------ */

/* opaque payload of UiScreen/UiWidget JS objects (C heap, freed by the
   class finalizer; the GC may move the JS object, never this struct) */
typedef struct {
    uint32_t handle; /* C-side widget handle (0 = inert: UI-less build) */
    uint32_t screen; /* handle of the owning screen (== handle for screens) */
    uint8_t kind;    /* UIW_K_* */
} JsUiHandle;

void js_ui_handle_finalizer(JSContext *ctx, void *opaque)
{
    (void)ctx;
    free(opaque); /* may be NULL when JS_SetOpaque was never reached */
}

JSValue js_ui_no_ctor(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return JS_ThrowTypeError(ctx, "use ui.screen()");
}

#ifndef ESP_PLATFORM
/* PC model: same navigation semantics as the device (current screen +
   retain stack of 3 + eviction) so long-running demos exercise the
   callback-release paths; widgets are just counted handles. */
#define PCW_RETAIN 3
static uint32_t s_pcw_next = 1;
static uint32_t s_pcw_cur;               /* 0 = console */
static uint32_t s_pcw_stack[PCW_RETAIN + 1];
static int s_pcw_sp;

static uint32_t pcw_screen(const char *title, uint32_t *evicted)
{
    *evicted = 0;
    if (s_pcw_cur) {
        if (s_pcw_sp == PCW_RETAIN) { /* full: evict the deepest */
            *evicted = s_pcw_stack[0];
            memmove(&s_pcw_stack[0], &s_pcw_stack[1],
                    (PCW_RETAIN - 1) * sizeof(uint32_t));
            s_pcw_sp--;
        }
        s_pcw_stack[s_pcw_sp++] = s_pcw_cur;
    }
    s_pcw_cur = s_pcw_next++;
    printf("[ui] screen(%s) -> #%u%s\n", title, (unsigned)s_pcw_cur,
           *evicted ? " (evicted one)" : "");
    return s_pcw_cur;
}

static uint32_t pcw_back(void)
{
    if (!s_pcw_cur)
        return 0;
    uint32_t destroyed = s_pcw_cur;
    s_pcw_cur = s_pcw_sp ? s_pcw_stack[--s_pcw_sp] : 0;
    printf("[ui] back: destroyed #%u, now on #%u\n", (unsigned)destroyed,
           (unsigned)s_pcw_cur);
    return destroyed;
}

static void pcw_reset(void)
{
    s_pcw_cur = 0;
    s_pcw_sp = 0;
}
#endif /* !ESP_PLATFORM */

/* register a widget callback; returns -1 when all slots are taken */
static int wcb_add(JSContext *ctx, uint32_t handle, uint32_t screen,
                   JSValue fn)
{
    for (int i = 0; i < MQJS_MAX_WIDGET_CB; i++) {
        WidgetCb *w = &s_cur_wk->widget_cbs[i];
        if (w->used)
            continue;
        JSValue *pf = JS_AddGCRef(ctx, &w->fn);
        *pf = fn;
        w->handle = handle;
        w->screen = screen;
        w->used = true;
        return 0;
    }
    return -1;
}

/* one sweep per destroyed screen: all its callbacks die together, so
   the compacting GC repacks once (design §4④) */
static void wcb_release_screen(MqjsWorker *app, uint32_t screen)
{
    for (int i = 0; i < MQJS_MAX_WIDGET_CB; i++) {
        WidgetCb *w = &app->widget_cbs[i];
        if (w->used && w->screen == screen) {
            w->used = false;
            JS_DeleteGCRef(app->ctx, &w->fn);
        }
    }
}

/* all screens of an app died at once (foreground switch / app stop) */
static void wcb_release_all(MqjsWorker *app)
{
    for (int i = 0; i < MQJS_MAX_WIDGET_CB; i++) {
        WidgetCb *w = &app->widget_cbs[i];
        if (w->used) {
            w->used = false;
            JS_DeleteGCRef(app->ctx, &w->fn);
        }
    }
}

/* called by ui_widgets.cpp from the LVGL task (never an ISR) */
void mqjs_post_widget(uint32_t handle, int32_t value)
{
#ifdef ESP_PLATFORM
    if (!s_event_queue)
        return;
    MqjsEvent ev = {
        .type = EV_WIDGET,
        .u.widget = { .handle = handle, .value = value },
    };
    xQueueSend(s_event_queue, &ev, 0); /* full queue: drop, never block */
#else
    (void)handle;
    (void)value;
#endif
}

static void dispatch_widget_event(MqjsWorker *app, const MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    for (int i = 0; i < MQJS_MAX_WIDGET_CB; i++) {
        WidgetCb *w = &app->widget_cbs[i];
        if (!w->used || w->handle != ev->u.widget.handle)
            continue;
        if (JS_StackCheck(ctx, 4)) {
            dump_error(ctx);
            return;
        }
        JS_PushArg(ctx, JS_NewInt32(ctx, ev->u.widget.value)); /* arg0 */
        JS_PushArg(ctx, w->fn.val);                            /* func */
        JS_PushArg(ctx, JS_NULL);                              /* this */
        arm_watchdog();
        JSValue ret = JS_Call(ctx, 1);
        if (JS_IsException(ret))
            dump_error(ctx);
        return; /* handles are unique */
    }
}

/* build the JS handle object. Runs JS allocation, so every C string
   must already be copied out before calling this. */
static JSValue uiw_make(JSContext *ctx, uint32_t handle, uint32_t screen,
                        int kind, int class_id)
{
    JSValue obj = JS_NewObjectClassUser(ctx, class_id);
    if (JS_IsException(obj))
        return obj;
    JsUiHandle *h = malloc(sizeof *h);
    if (!h)
        return JS_ThrowOutOfMemory(ctx); /* obj is GC garbage, opaque NULL */
    h->handle = handle;
    h->screen = screen;
    h->kind = (uint8_t)kind;
    JS_SetOpaque(ctx, obj, h);
    return obj;
}

static JsUiHandle *uiw_get(JSContext *ctx, JSValue *this_val, int class_id)
{
    if (JS_GetClassID(ctx, *this_val) != class_id)
        return NULL;
    return JS_GetOpaque(ctx, *this_val);
}

/* copy a JS string argument onto the C stack (bounded). The pointer
   returned by JS_ToCStringLen lives in the GC heap and dies on the next
   allocation — never keep it across uiw_make/JS_New*. */
static int uiw_copy_str(JSContext *ctx, JSValue v, char *dst, size_t cap)
{
    dst[0] = '\0';
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    JSCStringBuf buf;
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, v, &buf);
    if (!s)
        return -1;
    if (len >= cap)
        len = cap - 1; /* worst case tears a UTF-8 tail; caps are ample */
    memcpy(dst, s, len);
    dst[len] = '\0';
    return 0;
}

static int uiw_truthy(JSContext *ctx, JSValue v)
{
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (JS_IsBool(v))
        return v == JS_NewBool(1);
    if (JS_IsNumber(ctx, v)) {
        int i = 0;
        JS_ToInt32(ctx, &i, v);
        return i != 0;
    }
    return 1; /* objects/strings: truthy enough for an options flag */
}

/* 実体は辞書を解決する場所と同じなので下の辞書セクション。ja の field は
   「作った時点で辞書が開けたか」で ascii へ落ちるので、ここで要る。 */
static bool ime_arm(void);

/* field(label, mode) の第 2 引数 -> UIW_FIELD_* (I3)。
 *
 *   s.field("SSID")                     ascii — 既定。IME は armable ですらない
 *   s.field("タイトル", "ja")            日本語可
 *   s.field("パスワード", true)          password (旧 {secret:true} も同じ)
 *
 * 既定を ascii にしてあるのは、間違いの重さが釣り合わないから: "ja" を
 * 書き忘れた自由入力欄は「日本語が打てない」だけで目に見えるが、SSID の欄を
 * 縛り忘れると SSID に かな が入って接続が理由の見えない失敗をする。
 * 知らない文字列も ascii に倒す (綴り間違いが日本語入力を勝手に開かない)。 */
static int uiw_field_mode(JSContext *ctx, JSValue v, int *out)
{
    *out = UIW_FIELD_ASCII;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (JS_IsString(ctx, v)) {
        char s[16];
        if (uiw_copy_str(ctx, v, s, sizeof s))
            return -1;
        if (!strcmp(s, "ja"))
            *out = UIW_FIELD_JA;
        else if (!strcmp(s, "password") || !strcmp(s, "secret"))
            *out = UIW_FIELD_PASSWORD;
        return 0;
    }
    if (JS_IsBool(v) || JS_IsNumber(ctx, v)) { /* field(label, true) */
        if (uiw_truthy(ctx, v))
            *out = UIW_FIELD_PASSWORD;
        return 0;
    }
    /* 残りはオプション object — 旧 API の field(label, {secret:true}) */
    JSValue sec = JS_GetPropertyStr(ctx, v, "secret");
    if (JS_IsException(sec))
        return -1;
    if (uiw_truthy(ctx, sec))
        *out = UIW_FIELD_PASSWORD;
    return 0;
}

/* ui.screen(title) -> UiScreen (inert handle for background apps) */
JSValue js_ui_screen(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char title[64];
    if (uiw_copy_str(ctx, argv[0], title, sizeof title))
        return JS_EXCEPTION;
    uint32_t evicted = 0, h = 0;
    if (ui_is_fg()) {
#ifdef ESP_PLATFORM
        h = ui_tab5_w_screen(title, &evicted);
#else
        h = pcw_screen(title, &evicted);
#endif
        if (evicted)
            wcb_release_screen(s_cur_wk, evicted);
    }
    return uiw_make(ctx, h, h, UIW_K_SCREEN, JS_CLASS_UI_SCREEN);
}

/* ui.back() -> bool (false on the console screen / UI-less build /
   background app). Also usable directly as a tap callback. */
JSValue js_ui_back(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    if (!ui_is_fg())
        return JS_NewBool(0);
    uint32_t destroyed;
#ifdef ESP_PLATFORM
    destroyed = ui_tab5_w_back();
#else
    destroyed = pcw_back();
#endif
    if (destroyed)
        wcb_release_screen(s_cur_wk, destroyed);
    return JS_NewBool(destroyed != 0);
}

/* ui.navigate(builderFn): run the builder (it is expected to call
   ui.screen() itself). W1 keeps no builder reference — rebuilding a
   retain-depth-evicted screen on back() is the W2 follow-up; today an
   evicted screen just falls through to the console. */
JSValue js_ui_navigate(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    if (JS_StackCheck(ctx, 2))
        return JS_EXCEPTION;
    JS_PushArg(ctx, argv[0]); /* func */
    JS_PushArg(ctx, JS_NULL); /* this */
    return JS_Call(ctx, 0);
}

/* UiScreen.prototype.{button,label,field,list,toggle,slider} — one
   implementation, the magic value is the UIW_K_* widget kind.
     button(text, onTap)         label(text)
     field(label, {secret:bool}) list()
     toggle(text, init, onChange) slider(min, max, value, onChange) */
JSValue js_uiscreen_create(JSContext *ctx, JSValue *this_val, int argc,
                           JSValue *argv, int magic)
{
    JsUiHandle *sh = uiw_get(ctx, this_val, JS_CLASS_UI_SCREEN);
    if (!sh)
        return JS_ThrowTypeError(ctx, "not a UiScreen");
    char text[128] = "";
    int a = 0, b = 0, c = 0;
    JSValue cb = JS_UNDEFINED;

    switch (magic) {
    case UIW_K_BUTTON:
        if (uiw_copy_str(ctx, argv[0], text, sizeof text))
            return JS_EXCEPTION;
        cb = argv[1];
        break;
    case UIW_K_LABEL:
        if (uiw_copy_str(ctx, argv[0], text, sizeof text))
            return JS_EXCEPTION;
        break;
    case UIW_K_FIELD:
        if (uiw_copy_str(ctx, argv[0], text, sizeof text))
            return JS_EXCEPTION;
        if (uiw_field_mode(ctx, argv[1], &a))
            return JS_EXCEPTION;
#ifdef ESP_PLATFORM
        /* ja は辞書がある機体でだけ ja。開けなければ ascii へ落とす —
           「あ」が押せるのに何も起きない状態を作らないため (§4-4 の
           恒久ラッチ禁止と同じ理由で、ここでは毎回開き直す)。
           開くのは field を作るこの瞬間 = mqjs タスク。フォーカスは
           LVGL タスクで起きるので、そこでは辞書 open も ack 待ちもできない。 */
        if (a == UIW_FIELD_JA && !ime_arm())
            a = UIW_FIELD_ASCII;
#endif
        break;
    case UIW_K_LIST:
        break;
    case UIW_K_TOGGLE:
        if (uiw_copy_str(ctx, argv[0], text, sizeof text))
            return JS_EXCEPTION;
        a = uiw_truthy(ctx, argv[1]);
        cb = argv[2];
        break;
    case UIW_K_SLIDER:
        if ((!JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &a, argv[0])) ||
            (!JS_IsUndefined(argv[1]) && JS_ToInt32(ctx, &b, argv[1])) ||
            (!JS_IsUndefined(argv[2]) && JS_ToInt32(ctx, &c, argv[2])))
            return JS_EXCEPTION;
        if (b <= a) { /* default range when called as slider() */
            a = 0;
            b = 100;
        }
        cb = argv[3];
        break;
    default:
        return JS_ThrowInternalError(ctx, "bad widget kind");
    }
    if (!JS_IsUndefined(cb) && !JS_IsNull(cb) && !JS_IsFunction(ctx, cb))
        return JS_ThrowTypeError(ctx, "not a function");

    uint32_t h = 0;
    if (ui_is_fg()) {
#ifdef ESP_PLATFORM
        h = ui_tab5_w_create(magic, sh->handle, text, a, b, c);
#else
        h = s_pcw_next++;
        printf("[ui] widget(kind=%d, \"%s\") -> #%u (stub)\n", magic, text,
               (unsigned)h);
#endif
    }
    if (h && JS_IsFunction(ctx, cb)) {
        if (wcb_add(ctx, h, sh->screen, cb))
            return JS_ThrowInternalError(ctx, "too many widget callbacks");
    }
    return uiw_make(ctx, h, sh->screen, magic, JS_CLASS_UI_WIDGET);
}

/* UiWidget.prototype.add(text, onTap[, onClose[, icon]]) — list rows.
   With onClose the row gets a trailing action button (P4b/P4c: the
   launcher stops/uninstalls inline); tapping it fires onClose only,
   never onTap. icon: "trash" for uninstall semantics, default ✕. */
JSValue js_uiwidget_add(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JsUiHandle *lh = uiw_get(ctx, this_val, JS_CLASS_UI_WIDGET);
    if (!lh || lh->kind != UIW_K_LIST)
        return JS_ThrowTypeError(ctx, "not a list");
    char text[128];
    if (uiw_copy_str(ctx, argv[0], text, sizeof text))
        return JS_EXCEPTION;
    JSValue cb = argv[1];
    if (!JS_IsUndefined(cb) && !JS_IsNull(cb) && !JS_IsFunction(ctx, cb))
        return JS_ThrowTypeError(ctx, "not a function");
    JSValue ccb = argv[2];
    if (!JS_IsUndefined(ccb) && !JS_IsNull(ccb) && !JS_IsFunction(ctx, ccb))
        return JS_ThrowTypeError(ctx, "not a function");
    uint32_t h = 0;
    if (ui_is_fg()) {
#ifdef ESP_PLATFORM
        h = ui_tab5_w_create(UIW_K_ITEM, lh->handle, text, 0, 0, 0);
#else
        h = s_pcw_next++;
        printf("[ui] list.add(\"%s\") -> #%u (stub)\n", text, (unsigned)h);
#endif
    }
    if (h && JS_IsFunction(ctx, cb)) {
        if (wcb_add(ctx, h, lh->screen, cb))
            return JS_ThrowInternalError(ctx, "too many widget callbacks");
    }
    if (h && JS_IsFunction(ctx, ccb)) {
        char icon[16];
        if (uiw_copy_str(ctx, argv[3], icon, sizeof icon))
            return JS_EXCEPTION;
        int ic = strcmp(icon, "trash") == 0 ? 1 : 0;
        uint32_t hc;
#ifdef ESP_PLATFORM
        hc = ui_tab5_w_item_close(h, ic);
#else
        hc = s_pcw_next++;
        printf("[ui] list.add %s button -> #%u (stub)\n",
               ic ? "trash" : "close", (unsigned)hc);
#endif
        if (hc && wcb_add(ctx, hc, lh->screen, ccb))
            return JS_ThrowInternalError(ctx, "too many widget callbacks");
    }
    return uiw_make(ctx, h, lh->screen, UIW_K_ITEM, JS_CLASS_UI_WIDGET);
}

/* UiWidget.prototype.setText(str) — label/button/list-row text, or field
   content. Returns true when the widget accepted it. */
JSValue js_uiwidget_setText(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JsUiHandle *h = uiw_get(ctx, this_val, JS_CLASS_UI_WIDGET);
    if (!h)
        return JS_ThrowTypeError(ctx, "not a widget");
    char text[256];
    if (uiw_copy_str(ctx, argv[0], text, sizeof text))
        return JS_EXCEPTION;
#ifdef ESP_PLATFORM
    return JS_NewBool(ui_tab5_w_set_text(h->handle, text));
#else
    printf("[ui] setText(#%u, \"%s\") (stub)\n", (unsigned)h->handle, text);
    return JS_NewBool(1);
#endif
}

/* UiWidget.prototype.value() — field: string, toggle: 0/1, slider: int */
JSValue js_uiwidget_value(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JsUiHandle *h = uiw_get(ctx, this_val, JS_CLASS_UI_WIDGET);
    if (!h)
        return JS_ThrowTypeError(ctx, "not a widget");
    if (h->kind == UIW_K_FIELD) {
        char buf[256] = "";
#ifdef ESP_PLATFORM
        ui_tab5_w_value_str(h->handle, buf, sizeof buf);
#endif
        return JS_NewString(ctx, buf);
    }
    if (h->kind == UIW_K_TOGGLE || h->kind == UIW_K_SLIDER) {
#ifdef ESP_PLATFORM
        return JS_NewInt32(ctx, ui_tab5_w_value_int(h->handle));
#else
        return JS_NewInt32(ctx, 0);
#endif
    }
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ */
/* store (W2): tiny local key-value persistence, broker-independent    */
/* (design §6 local-first). NVS namespace "mqjs"; values are strings   */
/* (JS does JSON.stringify/parse on top). Keys 1-15 chars (NVS limit), */
/* values up to ~3.9KB (NVS string limit). Survives task switches and  */
/* reboots; PC build keeps a session-local table so flows smoke-test.  */
/* P4: one flat namespace shared by ALL apps — prefix keys "<app>.k"   */
/* by convention (§3.5); enforcement waits for the P4c manifest.       */
/* ------------------------------------------------------------------ */

#define MQJS_STORE_VAL_MAX 3900
#define MQJS_VAULT_VAL_MAX 127
#define MQJS_VAULT_NAME_MAX 127

/* Vault entries are automatically scoped to the calling app. There is no
   JS read API: consumers such as ssh.connect resolve the value in C. */
static uint64_t vault_hash(const char *app, const char *name)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (const char *p = app; *p; p++) {
        h ^= (unsigned char)*p;
        h *= UINT64_C(1099511628211);
    }
    h ^= 0;
    h *= UINT64_C(1099511628211);
    for (const char *p = name; *p; p++) {
        h ^= (unsigned char)*p;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static bool vault_key(const char *app, const char *name, char key[16])
{
    size_t n = strlen(name);
    if (!app || !app[0] || n == 0 || n > MQJS_VAULT_NAME_MAX)
        return false;
    snprintf(key, 16, "v%014llx",
             (unsigned long long)(vault_hash(app, name) &
                                  UINT64_C(0x00ffffffffffffff)));
    return true;
}

#ifdef ESP_PLATFORM
static nvs_handle_t s_store;
static bool s_store_open;
static nvs_handle_t s_vault;
static bool s_vault_open;

static bool store_open(void)
{
    if (s_store_open)
        return true;
    /* wifi.c normally ran nvs_flash_init already; do it lazily for
       UI-less / WiFi-less configurations (double init is a no-op) */
    if (nvs_open("mqjs", NVS_READWRITE, &s_store) != ESP_OK) {
        nvs_flash_init();
        if (nvs_open("mqjs", NVS_READWRITE, &s_store) != ESP_OK)
            return false;
    }
    s_store_open = true;
    return true;
}

static bool vault_open(void)
{
    if (s_vault_open)
        return true;
    if (nvs_open("mqjs_vault", NVS_READWRITE, &s_vault) != ESP_OK)
        return false;
    s_vault_open = true;
    return true;
}

/* Write-behind commit (hotspot audit §1.1): nvs_commit is a FLASH
   write (ms to tens of ms with page GC) and used to run synchronously
   ON the JS task — every store.set stalled every app. nvs_set_* only
   updates the RAM cache and is cheap; the commit is coalesced onto the
   esp_timer task after a 300ms quiet window (each burst of sets pays
   one flash commit). Power-loss window = at most ~300ms of writes;
   NVS itself stays consistent (it journals), worst case the last
   value reverts. */
static esp_timer_handle_t s_store_timer;

static void store_commit_cb(void *arg)
{
    (void)arg;
    nvs_commit(s_store); /* NVS API is thread-safe */
}

static void store_commit_later(void)
{
    if (!s_store_timer) {
        const esp_timer_create_args_t a = {
            .callback = store_commit_cb,
            .name = "nvs_commit",
        };
        if (esp_timer_create(&a, &s_store_timer) != ESP_OK) {
            s_store_timer = NULL;
            nvs_commit(s_store); /* fallback: old synchronous path */
            return;
        }
    }
    if (!esp_timer_is_active(s_store_timer))
        esp_timer_start_once(s_store_timer, 300 * 1000);
}
#else
#define MQJS_PC_STORE 16
static struct {
    char key[16];
    char *val;
} s_pc_store[MQJS_PC_STORE];
static struct {
    char key[16];
    char val[MQJS_VAULT_VAL_MAX + 1];
    bool used;
} s_pc_vault[MQJS_PC_STORE];

static int pc_store_find(const char *k)
{
    for (int i = 0; i < MQJS_PC_STORE; i++)
        if (s_pc_store[i].val && !strcmp(s_pc_store[i].key, k))
            return i;
    return -1;
}
#endif

static bool vault_read(const char *app, const char *name, char *dst, size_t cap)
{
    char key[16];
    if (!vault_key(app, name, key) || cap == 0)
        return false;
#ifdef ESP_PLATFORM
    if (!vault_open())
        return false;
    size_t len = cap;
    return nvs_get_str(s_vault, key, dst, &len) == ESP_OK;
#else
    for (int i = 0; i < MQJS_PC_STORE; i++)
        if (s_pc_vault[i].used && !strcmp(s_pc_vault[i].key, key)) {
            snprintf(dst, cap, "%s", s_pc_vault[i].val);
            return true;
        }
    return false;
#endif
}

JSValue js_vault_has(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSCStringBuf nbuf;
    const char *name = JS_ToCString(ctx, argv[0], &nbuf);
    char value[MQJS_VAULT_VAL_MAX + 1];
    bool ok = name && vault_read(s_cur_wk->vault_id, name, value, sizeof value);
    memset(value, 0, sizeof value);
    return name ? JS_NewBool(ok) : JS_EXCEPTION;
}

JSValue js_vault_put(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSCStringBuf nbuf, vbuf;
    size_t vlen;
    const char *name = JS_ToCString(ctx, argv[0], &nbuf);
    if (!name)
        return JS_EXCEPTION;
    char key[16];
    if (!vault_key(s_cur_wk->vault_id, name, key))
        return JS_ThrowRangeError(ctx, "vault name must be 1-%d chars",
                                  MQJS_VAULT_NAME_MAX);
    const char *value = JS_ToCStringLen(ctx, &vlen, argv[1], &vbuf);
    if (!value)
        return JS_EXCEPTION;
    if (vlen > MQJS_VAULT_VAL_MAX)
        return JS_ThrowRangeError(ctx, "vault value too large (max %d)",
                                  MQJS_VAULT_VAL_MAX);
#ifdef ESP_PLATFORM
    bool ok = vault_open() && nvs_set_str(s_vault, key, value) == ESP_OK;
    if (ok)
        nvs_commit(s_vault);
#else
    int slot = -1;
    for (int i = 0; i < MQJS_PC_STORE; i++)
        if (s_pc_vault[i].used && !strcmp(s_pc_vault[i].key, key)) {
            slot = i;
            break;
        } else if (slot < 0 && !s_pc_vault[i].used) {
            slot = i;
        }
    bool ok = slot >= 0;
    if (ok) {
        s_pc_vault[slot].used = true;
        snprintf(s_pc_vault[slot].key, sizeof s_pc_vault[slot].key, "%s", key);
        snprintf(s_pc_vault[slot].val, sizeof s_pc_vault[slot].val, "%s", value);
    }
#endif
    return JS_NewBool(ok);
}

JSValue js_vault_del(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSCStringBuf nbuf;
    const char *name = JS_ToCString(ctx, argv[0], &nbuf);
    if (!name)
        return JS_EXCEPTION;
    char key[16];
    if (!vault_key(s_cur_wk->vault_id, name, key))
        return JS_NewBool(0);
#ifdef ESP_PLATFORM
    bool ok = vault_open() && nvs_erase_key(s_vault, key) == ESP_OK;
    if (ok)
        nvs_commit(s_vault);
#else
    bool ok = false;
    for (int i = 0; i < MQJS_PC_STORE; i++)
        if (s_pc_vault[i].used && !strcmp(s_pc_vault[i].key, key)) {
            memset(&s_pc_vault[i], 0, sizeof s_pc_vault[i]);
            ok = true;
            break;
        }
#endif
    return JS_NewBool(ok);
}

/* ------------------------------------------------------------------ */
/* system settings: immutable-system-app-only, purpose-built writes.   */
/* Secrets are never returned to JS; status contains presence/public   */
/* metadata only. See docs/system-settings-design.md.                   */
/* ------------------------------------------------------------------ */

static bool system_api_allowed(JSContext *ctx)
{
    if (s_cur_wk && s_cur_wk->trusted_system)
        return true;
    JS_ThrowTypeError(ctx, "system settings require an embedded system app");
    return false;
}

JSValue js_system_wifi_set(JSContext *ctx, JSValue *this_val, int argc,
                           JSValue *argv)
{
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    JSCStringBuf sbuf, pbuf;
    size_t slen, plen;
    const char *ssid = JS_ToCStringLen(ctx, &slen, argv[0], &sbuf);
    if (!ssid)
        return JS_EXCEPTION;
    const char *pass = JS_ToCStringLen(ctx, &plen, argv[1], &pbuf);
    if (!pass)
        return JS_EXCEPTION;
    if (!slen || slen > SYSTEM_VAULT_WIFI_SSID_MAX ||
        plen > SYSTEM_VAULT_WIFI_PASS_MAX)
        return JS_ThrowRangeError(ctx, "invalid Wi-Fi credential length");
    return JS_NewBool(system_vault_wifi_set(ssid, pass));
}

JSValue js_system_wifi_status(JSContext *ctx, JSValue *this_val, int argc,
                              JSValue *argv)
{
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    bool configured = system_vault_wifi_has();
    char ssid[SYSTEM_VAULT_WIFI_SSID_MAX + 1] = "";
    if (configured)
        system_vault_wifi_ssid(ssid, sizeof ssid);
    JSGCRef obj_ref;
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_PUSH_VALUE(ctx, obj);
    JS_SetPropertyStr(ctx, obj_ref.val, "configured", JS_NewBool(configured));
    JS_SetPropertyStr(ctx, obj_ref.val, "ssid", JS_NewString(ctx, ssid));
    JS_SetPropertyStr(ctx, obj_ref.val, "state",
                      JS_NewString(ctx, configured ? "configured"
                                                   : "not-configured"));
    JS_POP_VALUE(ctx, obj);
    return obj;
}

JSValue js_system_wifi_forget(JSContext *ctx, JSValue *this_val, int argc,
                              JSValue *argv)
{
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    return JS_NewBool(system_vault_wifi_forget());
}

JSValue js_system_tailscale_set(JSContext *ctx, JSValue *this_val, int argc,
                                JSValue *argv)
{
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    JSCStringBuf buf;
    size_t len;
    const char *key = JS_ToCStringLen(ctx, &len, argv[0], &buf);
    if (!key)
        return JS_EXCEPTION;
    if (!len || len > SYSTEM_VAULT_TS_AUTH_MAX)
        return JS_ThrowRangeError(ctx, "invalid Tailscale auth key length");
    bool ok = system_vault_tailscale_set(key);
    if (ok)
        tailscale_adapter_reauth();   /* (re)start the session with the new key */
    return JS_NewBool(ok);
}

JSValue js_system_tailscale_status(JSContext *ctx, JSValue *this_val, int argc,
                                   JSValue *argv)
{
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    tailscale_status_t st;
    tailscale_adapter_get_status(&st);
    JSGCRef obj_ref;
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_PUSH_VALUE(ctx, obj);
    JS_SetPropertyStr(ctx, obj_ref.val, "configured", JS_NewBool(st.configured));
    JS_SetPropertyStr(ctx, obj_ref.val, "enabled", JS_NewBool(st.enabled));
    JS_SetPropertyStr(ctx, obj_ref.val, "retries", JS_NewInt32(ctx, st.retries));
    JS_SetPropertyStr(ctx, obj_ref.val, "peers", JS_NewInt32(ctx, st.peers));
    /* Strings created in their own statement so any GC during JS_NewString
       updates obj_ref.val before JS_SetPropertyStr reads it (moving GC). */
    JSValue v_state = JS_NewString(ctx, st.state);
    JS_SetPropertyStr(ctx, obj_ref.val, "state", v_state);
    JSValue v_detail = JS_NewString(ctx, st.detail);
    JS_SetPropertyStr(ctx, obj_ref.val, "detail", v_detail);
    JSValue v_ip = JS_NewString(ctx, st.ip);
    JS_SetPropertyStr(ctx, obj_ref.val, "ip", v_ip);
    JS_POP_VALUE(ctx, obj);
    return obj;
}

JSValue js_system_tailscale_forget(JSContext *ctx, JSValue *this_val, int argc,
                                   JSValue *argv)
{
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    tailscale_adapter_forget();   /* stop session first, then clear the vault */
    return JS_NewBool(system_vault_tailscale_forget());
}

JSValue js_system_tailscale_enable(JSContext *ctx, JSValue *this_val, int argc,
                                   JSValue *argv)
{
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    return JS_NewBool(tailscale_adapter_enable());
}

JSValue js_system_tailscale_disable(JSContext *ctx, JSValue *this_val, int argc,
                                    JSValue *argv)
{
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    return JS_NewBool(tailscale_adapter_disable());
}

/* copy the key argument onto the stack; NVS keys are at most 15 chars */
static int store_key(JSContext *ctx, JSValue v, char *dst /*[16]*/)
{
    JSCStringBuf buf;
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, v, &buf);
    if (!s)
        return -1;
    if (len == 0 || len > 15) {
        JS_ThrowRangeError(ctx, "store key must be 1-15 chars");
        return -1;
    }
    memcpy(dst, s, len);
    dst[len] = '\0';
    return 0;
}

/* store.get(key) -> string | undefined */
JSValue js_store_get(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char key[16];
    if (store_key(ctx, argv[0], key))
        return JS_EXCEPTION;
#ifdef ESP_PLATFORM
    if (!store_open())
        return JS_UNDEFINED;
    size_t len = 0;
    if (nvs_get_str(s_store, key, NULL, &len) != ESP_OK || len == 0)
        return JS_UNDEFINED;
    char *buf = malloc(len);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    if (nvs_get_str(s_store, key, buf, &len) != ESP_OK) {
        free(buf);
        return JS_UNDEFINED;
    }
    JSValue v = JS_NewStringLen(ctx, buf, len - 1); /* len includes NUL */
    free(buf);
    return v;
#else
    int i = pc_store_find(key);
    return i < 0 ? JS_UNDEFINED : JS_NewString(ctx, s_pc_store[i].val);
#endif
}

/* store.set(key, value) -> bool. Strings only; persist immediately. */
JSValue js_store_set(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char key[16];
    if (store_key(ctx, argv[0], key))
        return JS_EXCEPTION;
    JSCStringBuf vbuf;
    size_t vlen;
    /* read the value AFTER the key was copied out: a second ToCString
       may move the first string in the compacting GC heap */
    const char *val = JS_ToCStringLen(ctx, &vlen, argv[1], &vbuf);
    if (!val)
        return JS_EXCEPTION;
    if (vlen > MQJS_STORE_VAL_MAX)
        return JS_ThrowRangeError(ctx, "store value too large (max %d)",
                                  MQJS_STORE_VAL_MAX);
#ifdef ESP_PLATFORM
    if (!store_open())
        return JS_NewBool(0);
    /* val points into the JS heap, but nothing below allocates there */
    bool ok = nvs_set_str(s_store, key, val) == ESP_OK;
    if (ok)
        store_commit_later();
    return JS_NewBool(ok);
#else
    int i = pc_store_find(key);
    if (i < 0) {
        for (int k = 0; k < MQJS_PC_STORE; k++)
            if (!s_pc_store[k].val) {
                i = k;
                break;
            }
    }
    if (i < 0)
        return JS_NewBool(0);
    char *copy = malloc(vlen + 1);
    if (!copy)
        return JS_ThrowOutOfMemory(ctx);
    memcpy(copy, val, vlen);
    copy[vlen] = '\0';
    free(s_pc_store[i].val);
    strcpy(s_pc_store[i].key, key);
    s_pc_store[i].val = copy;
    printf("[store] set %s (%u bytes) (PC: session-only)\n", key,
           (unsigned)vlen);
    return JS_NewBool(1);
#endif
}

/* store.del(key) -> bool (true when it existed) */
JSValue js_store_del(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char key[16];
    if (store_key(ctx, argv[0], key))
        return JS_EXCEPTION;
#ifdef ESP_PLATFORM
    if (!store_open())
        return JS_NewBool(0);
    bool ok = nvs_erase_key(s_store, key) == ESP_OK;
    if (ok)
        store_commit_later();
    return JS_NewBool(ok);
#else
    int i = pc_store_find(key);
    if (i < 0)
        return JS_NewBool(0);
    free(s_pc_store[i].val);
    s_pc_store[i].val = NULL;
    return JS_NewBool(1);
#endif
}

/* ------------------------------------------------------------------ */
/* fs.*  — 内蔵ストレージと microSD (docs/filer-storage-design.md)      */
/*                                                                     */
/* このブロックは fs_core の仮想パス ("/internal/...", "/sd/...") しか  */
/* 知らない。"/littlefs" も SDMMC も LittleFS も一度も出てこないので、  */
/* Tab5 と Stamp-P4 で同じコードが動く —— 差は fs.volumes() が何本      */
/* 返すかだけになる (§2: Stamp は「カードが永久に入っていない Tab5」)。 */
/*                                                                     */
/* 読み取りは全アプリに開放 (この 2 ボリュームに秘密は無い。vault と    */
/* Wi-Fi 資格情報は NVS 側)。書き込み・削除・マウント操作は grant を    */
/* 要求する。grant は net.onReady のトークンと同じ不透明な整数で、      */
/* ユーザの同意を経ないと手に入らない = トップレベルで書けない (§7)。  */
/* 同意画面はランチャーが描く: 要求元のアプリに描かせると偽装できる。   */
/*                                                                     */
/* grant の**下**にもう 1 層ある。fs_core の予約サブツリー              */
/* (fs_path_reserved) は "/internal/apps" 以下の変更をどんな grant でも */
/* 通さない。同意画面は「内蔵への書き込み」としか言えないので、         */
/* "/internal" を丸ごと許した人が署名済みアプリの中身を差し替えられて   */
/* しまう —— そこだけは権限ではなく不変条件として fs_core が握る。      */
/* ここ (バインディング側) には対応するコードが 1 行も無いのが正しい。  */
/* ------------------------------------------------------------------ */

#define MQJS_FS_GRANTS    4
#define MQJS_FS_SCOPE_MAX 128
#define MQJS_FS_READ_MAX  65536
#define MQJS_FS_READ_DEF  8192
#define MQJS_FS_LIST_MAX  512
#define MQJS_FS_REASON_MAX 96

#ifdef ESP_PLATFORM

typedef struct {
    uint32_t       token;    /* 0 = 空き */
    uint8_t        worker;
    uint16_t       gen;
    bool           write;
    uint32_t       epoch;    /* 発行時のボリュームのマウント世代 */
    const fsvol_t *vol;
    char           root[MQJS_FS_SCOPE_MAX];
} FsGrant;

static FsGrant  s_fs_grants[MQJS_FS_GRANTS];
static uint32_t s_fs_token_seq;

/* 同意待ちの要求。ランチャーが sys.fsConsent(id, ok) で返事するまで
   ここに置く。要求 1 件がアプリ 1 つに対応する (fsreq_cb が一発限りの
   ハンドラなので、同じアプリからの二重要求は後勝ちで潰す)。 */
typedef struct {
    uint32_t id;             /* 0 = 空き */
    uint8_t  worker;
    uint16_t gen;
    bool     write;
    char     root[MQJS_FS_SCOPE_MAX];
} FsPending;

static FsPending s_fs_pending[MQJS_FS_GRANTS];
static uint32_t  s_fs_req_seq;

/* パス引数をスタックへ写す。JS の文字列は以降の割り当てで動きうるので、
   触る前に必ずコピーを取る (store_key と同じ理由)。 */
static int fs_path_arg(JSContext *ctx, JSValue v, char *dst, size_t cap)
{
    JSCStringBuf buf;
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, v, &buf);
    if (!s)
        return -1;
    if (len == 0 || len >= cap) {
        JS_ThrowRangeError(ctx, "fs: path length must be 1..%d", (int)cap - 1);
        return -1;
    }
    memcpy(dst, s, len);
    dst[len] = '\0';
    return 0;
}

static JSValue fs_throw(JSContext *ctx, const char *what, esp_err_t err)
{
    JS_ThrowTypeError(ctx, "fs.%s: %s", what, fs_err_str(err));
    return JS_EXCEPTION;
}

/* grant を引き当てる。持ち主・世代・マウント世代がすべて一致したものだけ
   が生きている: アプリが止まればその grant は使えなくなり、カードを
   抜き差しすれば epoch がずれて死ぬ。掃除の常駐処理は要らない (§7)。 */
static FsGrant *fs_grant_get(JSContext *ctx, JSValue v, bool need_write)
{
    int token = 0;   /* JS_ToInt32 は int* を取る (riscv32) */
    if (JS_ToInt32(ctx, &token, v) || token <= 0) {
        JS_ThrowTypeError(ctx, "fs: expected a grant from fs.request(...)");
        return NULL;
    }
    for (int i = 0; i < MQJS_FS_GRANTS; i++) {
        FsGrant *g = &s_fs_grants[i];
        if (!g->token || g->token != (uint32_t)token)
            continue;
        if (!s_cur_wk || g->worker != s_cur_wk->idx || g->gen != s_cur_wk->gen)
            break;
        if (fsvol_epoch(g->vol) != g->epoch) {
            g->token = 0;              /* 抜かれたカードの権限 */
            break;
        }
        if (need_write && !g->write) {
            JS_ThrowTypeError(ctx, "fs: this grant is read-only");
            return NULL;
        }
        return g;
    }
    JS_ThrowTypeError(ctx, "fs: grant expired or not yours");
    return NULL;
}

/* path が grant の範囲に入っているか。root そのものか root + '/'。 */
static bool fs_in_scope(const FsGrant *g, const char *path)
{
    size_t n = strlen(g->root);
    if (strncmp(path, g->root, n))
        return false;
    return path[n] == '\0' || path[n] == '/';
}

static FsGrant *fs_grant_for(JSContext *ctx, JSValue gv, const char *path,
                             bool need_write)
{
    FsGrant *g = fs_grant_get(ctx, gv, need_write);
    if (!g)
        return NULL;
    if (!fs_in_scope(g, path)) {
        JS_ThrowTypeError(ctx, "fs: '%s' is outside the granted scope '%s'",
                          path, g->root);
        return NULL;
    }
    return g;
}

/* 一番古い grant を潰して席を空ける (4 席、LRU ですらない単純な使い回し:
   アプリが同時に 4 つの範囲へ書くことは想定していない)。 */
static FsGrant *fs_grant_slot(void)
{
    for (int i = 0; i < MQJS_FS_GRANTS; i++)
        if (!s_fs_grants[i].token)
            return &s_fs_grants[i];
    FsGrant *oldest = &s_fs_grants[0];
    for (int i = 1; i < MQJS_FS_GRANTS; i++)
        if (s_fs_grants[i].token < oldest->token)
            oldest = &s_fs_grants[i];
    return oldest;
}

#endif /* ESP_PLATFORM */

/* fs.volumes() -> [{id,label,path,fstype,mounted,removable,system,total,free}]
   ボードにボリュームが 1 本しか無ければ 1 本返る。アプリはこの配列を
   回すだけで、どのボードで動いているかを知る必要がない。 */
JSValue js_fs_volumes(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSGCRef arr_ref, obj_ref;
    JSValue arr = JS_NewArray(ctx, 0);
    if (JS_IsException(arr))
        return arr;
#ifdef ESP_PLATFORM
    JS_PUSH_VALUE(ctx, arr);
    int n = 0;
    int count = fsvol_count();
    for (int i = 0; i < count; i++) {
        const fsvol_t *v = fsvol_at(i);
        if (!v)
            continue;
        bool mounted = fsvol_mounted(v);
        uint64_t total = 0, freeb = 0;
        if (mounted)
            fs_usage(v->id, &total, &freeb);

        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) {
            JS_POP_VALUE(ctx, arr);
            return obj;
        }
        JS_PUSH_VALUE(ctx, obj);
        /* 文字列は必ず自分の文だけで作る: 生成中の GC が obj を動かしても
           obj_ref.val は追随するが、入れ子にすると評価順で古い値を読む。 */
        JSValue s_id = JS_NewString(ctx, v->id);
        JS_SetPropertyStr(ctx, obj_ref.val, "id", s_id);
        JSValue s_label = JS_NewString(ctx, v->label ? v->label : v->id);
        JS_SetPropertyStr(ctx, obj_ref.val, "label", s_label);
        JSValue s_type = JS_NewString(ctx, v->fstype ? v->fstype : "");
        JS_SetPropertyStr(ctx, obj_ref.val, "fstype", s_type);
        char vroot[40];
        snprintf(vroot, sizeof vroot, "/%s", v->id);
        JSValue s_path = JS_NewString(ctx, vroot);
        JS_SetPropertyStr(ctx, obj_ref.val, "path", s_path);
        JS_SetPropertyStr(ctx, obj_ref.val, "mounted", JS_NewBool(mounted));
        /* 「入っていない」と「入っているが読めない」を混ぜない。混ぜると
           未フォーマットのカードが「入っていません」と出て、直す導線が
           画面から消える (docs/filer-storage-design.md §12)。 */
        const char *st = "absent";
        switch (fsvol_state(v)) {
        case FSVOL_ST_MOUNTED:    st = "mounted";    break;
        case FSVOL_ST_UNKNOWN:    st = "unknown";    break;
        case FSVOL_ST_UNREADABLE: st = "unreadable"; break;
        default:                                     break;
        }
        JSValue s_state = JS_NewString(ctx, st);
        JS_SetPropertyStr(ctx, obj_ref.val, "state", s_state);
        JS_SetPropertyStr(ctx, obj_ref.val, "removable",
                          JS_NewBool((v->flags & FSVOL_REMOVABLE) ? 1 : 0));
        JS_SetPropertyStr(ctx, obj_ref.val, "system",
                          JS_NewBool((v->flags & FSVOL_SYSTEM) ? 1 : 0));
        /* バイト数は 32bit int に収まらない (32GB カード)。倍精度なら
           2^53 まで正確なので、そのまま数として渡してよい。 */
        JS_SetPropertyStr(ctx, obj_ref.val, "total",
                          JS_NewFloat64(ctx, (double)total));
        JS_SetPropertyStr(ctx, obj_ref.val, "free",
                          JS_NewFloat64(ctx, (double)freeb));
        JS_POP_VALUE(ctx, obj);
        JS_SetPropertyUint32(ctx, arr_ref.val, n++, obj);
    }
    JS_POP_VALUE(ctx, arr);
#else
    (void)this_val; (void)argc; (void)argv;
#endif
    return arr;
}

/* fs.list(path[, {offset, limit, stat}]) -> [{name,dir,size,mtime}]
   size/mtime は既定で埋めるが、limit を大きく取ると FAT の線形検索で
   高くつくので {stat:false} で外せる。truncated は配列の .more に立つ。 */
JSValue js_fs_list(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    char path[FS_PATH_MAX];
    if (fs_path_arg(ctx, argv[0], path, sizeof path))
        return JS_EXCEPTION;

    int offset = 0, limit = MQJS_FS_LIST_MAX;
    bool want_stat = true;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        int tmp = 0;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "offset");
        if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v))
            offset = tmp;
        v = JS_GetPropertyStr(ctx, argv[1], "limit");
        if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v))
            limit = tmp;
        v = JS_GetPropertyStr(ctx, argv[1], "stat");
        if (!JS_IsUndefined(v))
            want_stat = uiw_truthy(ctx, v);
    }
    if (offset < 0)
        offset = 0;
    if (limit < 0 || limit > MQJS_FS_LIST_MAX)
        limit = MQJS_FS_LIST_MAX;

    fs_dir_t *dir = NULL;
    esp_err_t err = fs_dir_open(path, &dir);
    if (err != ESP_OK)
        return fs_throw(ctx, "list", err);

    JSGCRef arr_ref, obj_ref;
    JSValue arr = JS_NewArray(ctx, 0);
    if (JS_IsException(arr)) {
        fs_dir_close(dir);
        return arr;
    }
    JS_PUSH_VALUE(ctx, arr);
    int total = fs_dir_count(dir);
    int n = 0;
    for (int i = offset; i < total && n < limit; i++) {
        fs_entry_t e;
        if (!fs_dir_get(dir, i, &e))
            break;
        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj))
            break;
        JS_PUSH_VALUE(ctx, obj);
        /* 名前は dir のプールを指している。JS_NewString は JS ヒープしか
           動かさないので、プールが消えるのは fs_dir_close のときだけ。 */
        JSValue s_name = JS_NewString(ctx, e.name);
        JS_SetPropertyStr(ctx, obj_ref.val, "name", s_name);
        JS_SetPropertyStr(ctx, obj_ref.val, "dir", JS_NewBool(e.is_dir));
        if (want_stat) {
            fs_stat_t st;
            if (fs_dir_stat(dir, i, &st) == ESP_OK) {
                JS_SetPropertyStr(ctx, obj_ref.val, "size",
                                  JS_NewFloat64(ctx, (double)st.size));
                JS_SetPropertyStr(ctx, obj_ref.val, "mtime",
                                  JS_NewFloat64(ctx, (double)st.mtime));
            }
        }
        JS_POP_VALUE(ctx, obj);
        JS_SetPropertyUint32(ctx, arr_ref.val, n++, obj);
    }
    /* 一覧そのものが長すぎて切られたか (FS_DIR_MAX)、あるいは窓の先に
       まだ続きがあるか。黙って切ると「全部見た」と誤読される。 */
    JS_SetPropertyStr(ctx, arr_ref.val, "more",
                      JS_NewBool(fs_dir_truncated(dir) ||
                                 offset + n < total));
    JS_SetPropertyStr(ctx, arr_ref.val, "total", JS_NewInt32(ctx, total));
    JS_POP_VALUE(ctx, arr);
    fs_dir_close(dir);
    return arr;
#else
    (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(ctx, 0);
#endif
}

/* fs.stat(path) -> {name,dir,size,mtime} | undefined (存在しないとき) */
JSValue js_fs_stat(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    char path[FS_PATH_MAX];
    if (fs_path_arg(ctx, argv[0], path, sizeof path))
        return JS_EXCEPTION;
    fs_stat_t st;
    esp_err_t err = fs_stat(path, &st);
    if (err == ESP_ERR_NOT_FOUND)
        return JS_UNDEFINED;         /* 「無い」は例外ではない */
    if (err != ESP_OK)
        return fs_throw(ctx, "stat", err);

    JSGCRef obj_ref;
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_PUSH_VALUE(ctx, obj);
    JS_SetPropertyStr(ctx, obj_ref.val, "dir", JS_NewBool(st.is_dir));
    JS_SetPropertyStr(ctx, obj_ref.val, "size",
                      JS_NewFloat64(ctx, (double)st.size));
    JS_SetPropertyStr(ctx, obj_ref.val, "mtime",
                      JS_NewFloat64(ctx, (double)st.mtime));
    JS_POP_VALUE(ctx, obj);
    return obj;
#else
    (void)this_val; (void)argc; (void)argv; (void)ctx;
    return JS_UNDEFINED;
#endif
}

/* fs.read(path[, {offset, length, hex}]) -> string
   既定は先頭 8KB のテキスト。hex:true なら 16 進 2 桁/バイトで返す
   (バイナリを JS 文字列に押し込むと UTF-8 の検査で壊れるため)。 */
JSValue js_fs_read(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    char path[FS_PATH_MAX];
    if (fs_path_arg(ctx, argv[0], path, sizeof path))
        return JS_EXCEPTION;

    int offset = 0, length = MQJS_FS_READ_DEF;
    bool hex = false;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        int tmp = 0;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "offset");
        if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v))
            offset = tmp;
        v = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v))
            length = tmp;
        v = JS_GetPropertyStr(ctx, argv[1], "hex");
        hex = uiw_truthy(ctx, v);
    }
    if (offset < 0)
        offset = 0;
    if (length <= 0)
        return JS_NewStringLen(ctx, "", 0);
    if (length > MQJS_FS_READ_MAX)
        length = MQJS_FS_READ_MAX;
    if (hex && length > MQJS_FS_READ_MAX / 2)
        length = MQJS_FS_READ_MAX / 2;   /* 出力が倍になる */

    /* JS ヒープの外で読む: 数十 KB をアリーナに置くと GC が動く。 */
    char *buf = heap_caps_malloc((size_t)length, MALLOC_CAP_SPIRAM);
    if (!buf)
        buf = malloc((size_t)length);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    size_t got = 0;
    esp_err_t err = fs_read(path, (uint64_t)offset, buf, (size_t)length, &got);
    if (err != ESP_OK) {
        free(buf);
        return fs_throw(ctx, "read", err);
    }
    JSValue out;
    if (hex) {
        static const char HEX[] = "0123456789abcdef";
        char *h = malloc(got * 2 + 1);
        if (!h) {
            free(buf);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (size_t i = 0; i < got; i++) {
            h[i * 2]     = HEX[(unsigned char)buf[i] >> 4];
            h[i * 2 + 1] = HEX[(unsigned char)buf[i] & 15];
        }
        out = JS_NewStringLen(ctx, h, got * 2);
        free(h);
    } else {
        out = JS_NewStringLen(ctx, buf, got);
    }
    free(buf);
    return out;
#else
    (void)this_val; (void)argc; (void)argv;
    return JS_NewStringLen(ctx, "", 0);
#endif
}

/* JSON 文字列リテラルへ安全に埋める。reason はアプリが自由に書ける
   ので、素で連結すると引用符を閉じて別の op を注入できてしまう
   (ランチャーはこの JSON を JSON.parse する)。 */
static void fs_json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (; *src && o + 7 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/* fs.request({path, write, reason}, cb) -> bool
   同意を求め、返事が出たら cb(grant) を呼ぶ (拒否なら cb(0))。grant は
   ここでしか手に入らないので、トップレベルでの書き込みは書けない。 */
JSValue js_fs_request(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    char root[MQJS_FS_SCOPE_MAX];
    char reason[MQJS_FS_REASON_MAX];
    bool write = true;

    if (argc < 2 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return JS_ThrowTypeError(ctx,
            "fs.request({path, write, reason}, cb)");
    if (fs_path_arg(ctx, JS_GetPropertyStr(ctx, argv[0], "path"),
                    root, sizeof root))
        return JS_EXCEPTION;
    JSValue wv = JS_GetPropertyStr(ctx, argv[0], "write");
    if (!JS_IsUndefined(wv))
        write = uiw_truthy(ctx, wv);
    reason[0] = '\0';
    JSValue rv = JS_GetPropertyStr(ctx, argv[0], "reason");
    if (!JS_IsUndefined(rv) && uiw_copy_str(ctx, rv, reason, sizeof reason))
        return JS_EXCEPTION;

    /* 範囲が実在するボリュームを指しているか、ここで確かめる。カードが
       入っていないのに同意画面を出しても意味がない。 */
    const fsvol_t *vol = NULL;
    esp_err_t err = fsvol_resolve(root, &vol, NULL, 0);
    if (err != ESP_OK)
        return fs_throw(ctx, "request", err);

    JSValue r = register_cb(ctx, argv[1], &s_cur_wk->fsreq_used,
                            &s_cur_wk->fsreq_cb);
    if (JS_IsException(r))
        return r;

    /* 同じアプリの前の要求は捨てる (ハンドラは一発限りなので、返事が
       二度来ると片方が宙に浮く)。 */
    FsPending *p = NULL;
    for (int i = 0; i < MQJS_FS_GRANTS; i++) {
        if (s_fs_pending[i].id && s_fs_pending[i].worker == s_cur_wk->idx)
            s_fs_pending[i].id = 0;
        if (!s_fs_pending[i].id && !p)
            p = &s_fs_pending[i];
    }
    if (!p)
        p = &s_fs_pending[0];
    if (++s_fs_req_seq == 0)
        s_fs_req_seq = 1;
    p->id     = s_fs_req_seq;
    p->worker = s_cur_wk->idx;
    p->gen    = s_cur_wk->gen;
    p->write  = write;
    snprintf(p->root, sizeof p->root, "%s", root);

    /* dev スロットは同意を経ずに通す。camera.scanQr / sys.blackbox /
       system.* と同じ最高権限を既に持っており、ここだけ締めても新しい
       安全性は生まれない一方、MQTT で押し込む probe が画面を触れずに
       止まってしまう。 */
    if (s_cur_wk->idx == MQJS_WORKER_DEV) {
        MqjsEvent ev = { .type = EV_FSGRANT, .worker = s_cur_wk->idx,
                         .gen = s_cur_wk->gen };
        ev.u.fsgrant.id = p->id;
        ev.u.fsgrant.ok = 1;
        if (!ev_post(&ev, 0)) {
            p->id = 0;
            return JS_NewBool(0);
        }
        return JS_NewBool(1);
    }

    /* ランチャーへ同意要求を送る。届かない (ランチャーが居ない = UI の
       無いボードや起動直後) ときは黙って拒否 — 誰も尋ねられないなら
       書かせない、が既定。 */
    MqjsWorker *ui = &s_workers[MQJS_WORKER_LAUNCHER];
    if (!ui->used) {
        MqjsEvent ev = { .type = EV_FSGRANT, .worker = s_cur_wk->idx,
                         .gen = s_cur_wk->gen };
        ev.u.fsgrant.id = p->id;
        ev.u.fsgrant.ok = 0;
        ev_post(&ev, 0);
        return JS_NewBool(1);
    }

    char e_app[72], e_root[MQJS_FS_SCOPE_MAX * 2 + 8];
    char e_vol[72], e_reason[MQJS_FS_REASON_MAX * 2 + 8];
    fs_json_escape(e_app, sizeof e_app, s_cur_wk->name);
    fs_json_escape(e_root, sizeof e_root, root);
    fs_json_escape(e_vol, sizeof e_vol, vol->label ? vol->label : vol->id);
    fs_json_escape(e_reason, sizeof e_reason, reason);

    size_t jcap = sizeof e_app + sizeof e_root + sizeof e_vol +
                  sizeof e_reason + 128;
    char *json = malloc(jcap);
    if (!json) {
        p->id = 0;
        return JS_ThrowOutOfMemory(ctx);
    }
    snprintf(json, jcap,
             "{\"op\":\"fs-consent\",\"id\":%u,\"app\":\"%s\","
             "\"path\":\"%s\",\"write\":%s,\"vol\":\"%s\","
             "\"reason\":\"%s\"}",
             (unsigned)p->id, e_app, e_root, write ? "true" : "false",
             e_vol, e_reason);

    MqjsEvent sig = { .type = EV_SIGNAL, .worker = ui->idx, .gen = ui->gen };
    sig.u.signal.value = json;
    snprintf(sig.u.signal.from, sizeof sig.u.signal.from, "system");
    if (!ev_post(&sig, 0)) {
        free(json);
        p->id = 0;
        return JS_NewBool(0);
    }
    return JS_NewBool(1);
#else
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "fs.request: no filesystem on this build");
#endif
}

/* fs.release(grant) -> bool: 使い終わった権限を自分から返す。 */
JSValue js_fs_release(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    FsGrant *g = fs_grant_get(ctx, argv[0], false);
    if (!g)
        return JS_EXCEPTION;
    g->token = 0;
    return JS_NewBool(1);
#else
    (void)this_val; (void)argc; (void)argv; (void)ctx;
    return JS_NewBool(0);
#endif
}

/* sys.fsConsent(id, ok) -> bool: 同意画面を出したアプリだけが呼べる
   返事の口。ランチャー (組み込みシステムアプリ) 以外は弾く。 */
JSValue js_sys_fs_consent(JSContext *ctx, JSValue *this_val, int argc,
                          JSValue *argv)
{
#ifdef ESP_PLATFORM
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]) || id <= 0)
        return JS_ThrowTypeError(ctx, "sys.fsConsent(id, ok)");
    bool ok = uiw_truthy(ctx, argv[1]);
    for (int i = 0; i < MQJS_FS_GRANTS; i++) {
        FsPending *p = &s_fs_pending[i];
        if (p->id != (uint32_t)id)
            continue;
        MqjsEvent ev = { .type = EV_FSGRANT, .worker = p->worker,
                         .gen = p->gen };
        ev.u.fsgrant.id = p->id;
        ev.u.fsgrant.ok = ok ? 1 : 0;
        return JS_NewBool(ev_post(&ev, 0));
    }
    return JS_NewBool(0);   /* 期限切れ / 二重返答 */
#else
    (void)this_val; (void)argc; (void)argv; (void)ctx;
    return JS_NewBool(0);
#endif
}

/* ---- grant を要る操作 -------------------------------------------- */

JSValue js_fs_write(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    char path[FS_PATH_MAX];
    if (fs_path_arg(ctx, argv[1], path, sizeof path))
        return JS_EXCEPTION;
    if (!fs_grant_for(ctx, argv[0], path, true))
        return JS_EXCEPTION;

    bool append = false;
    if (argc >= 4 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3]))
        append = uiw_truthy(ctx, JS_GetPropertyStr(ctx, argv[3], "append"));

    JSCStringBuf dbuf;
    size_t dlen;
    const char *data = JS_ToCStringLen(ctx, &dlen, argv[2], &dbuf);
    if (!data)
        return JS_EXCEPTION;
    /* data は JS ヒープを指す。fs_write は JS の割り当てを一切しない。 */
    esp_err_t err = fs_write(path, data, dlen, append);
    if (err != ESP_OK)
        return fs_throw(ctx, "write", err);
    return JS_NewBool(1);
#else
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "fs.write: no filesystem on this build");
#endif
}

JSValue js_fs_mkdir(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    char path[FS_PATH_MAX];
    if (fs_path_arg(ctx, argv[1], path, sizeof path))
        return JS_EXCEPTION;
    if (!fs_grant_for(ctx, argv[0], path, true))
        return JS_EXCEPTION;
    esp_err_t err = fs_mkdir(path);
    if (err != ESP_OK)
        return fs_throw(ctx, "mkdir", err);
    return JS_NewBool(1);
#else
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "fs.mkdir: no filesystem on this build");
#endif
}

JSValue js_fs_remove(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    char path[FS_PATH_MAX];
    if (fs_path_arg(ctx, argv[1], path, sizeof path))
        return JS_EXCEPTION;
    if (!fs_grant_for(ctx, argv[0], path, true))
        return JS_EXCEPTION;
    bool recursive = argc >= 3 && uiw_truthy(ctx, argv[2]);
    esp_err_t err = fs_remove(path, recursive);
    if (err != ESP_OK)
        return fs_throw(ctx, "remove", err);
    return JS_NewBool(1);
#else
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "fs.remove: no filesystem on this build");
#endif
}

/* fs.rename(grant, from, to) — 両端が同じ grant の範囲に無ければ拒否。
   「読める範囲から書ける範囲へ動かす」ことはできない: 移動は元も消す。 */
JSValue js_fs_rename(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    if (fs_path_arg(ctx, argv[1], from, sizeof from) ||
        fs_path_arg(ctx, argv[2], to, sizeof to))
        return JS_EXCEPTION;
    if (!fs_grant_for(ctx, argv[0], from, true) ||
        !fs_grant_for(ctx, argv[0], to, true))
        return JS_EXCEPTION;
    esp_err_t err = fs_move(from, to);
    if (err != ESP_OK)
        return fs_throw(ctx, "rename", err);
    return JS_NewBool(1);
#else
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "fs.rename: no filesystem on this build");
#endif
}

/* fs.copy(grant, from, to) — 読み出し元は誰でも読めるので範囲外でよい。
   grant が要るのは書き込み先だけ。 */
JSValue js_fs_copy(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    if (fs_path_arg(ctx, argv[1], from, sizeof from) ||
        fs_path_arg(ctx, argv[2], to, sizeof to))
        return JS_EXCEPTION;
    if (!fs_grant_for(ctx, argv[0], to, true))
        return JS_EXCEPTION;
    esp_err_t err = fs_copy(from, to, NULL, NULL);
    if (err != ESP_OK)
        return fs_throw(ctx, "copy", err);
    return JS_NewBool(1);
#else
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "fs.copy: no filesystem on this build");
#endif
}

#ifdef ESP_PLATFORM
/* フォーマットは専用タスクで走らせる。大容量カードでは FAT テーブル
   だけで数十 MB 書くので、JS タスク上で回すと MQJS_MAX_RUN_MS (5 秒) の
   コールバック watchdog に確実に轢かれる。同時に 1 本だけ。 */
static const fsvol_t *s_fmt_vol;
static uint8_t        s_fmt_worker;
static uint16_t       s_fmt_gen;

static void fs_format_task(void *arg)
{
    (void)arg;
    esp_err_t err = fsvol_format(s_fmt_vol);
    MqjsEvent ev = { .type = EV_FSOP, .worker = s_fmt_worker,
                     .gen = s_fmt_gen };
    ev.u.fsop.ok = (err == ESP_OK) ? 1 : 0;
    ev_post(&ev, 0);
    s_fmt_vol = NULL;
    vTaskDelete(NULL);
}
#endif

/* fs.format(grant, volumeId, cb) -> bool (要求を受け付けたか)
   中身は消える。grant はそのボリュームへの書き込み権限で、フォーマットに
   成功しても失敗しても epoch が進むのでこの grant はここで死ぬ —— 消えた
   データへの権限が残らない。cb(ok) は終わってから呼ばれる。 */
JSValue js_fs_format(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
#ifdef ESP_PLATFORM
    char id[32];
    if (uiw_copy_str(ctx, argv[1], id, sizeof id))
        return JS_EXCEPTION;
    const fsvol_t *v = fsvol_find(id);
    if (!v)
        return fs_throw(ctx, "format", ESP_ERR_NOT_FOUND);
    /* 内蔵は消させない。アプリも設定も辞書もそこに在る。 */
    if (v->flags & FSVOL_SYSTEM)
        return JS_ThrowTypeError(ctx, "fs.format: '%s' is system storage", id);
    char vroot[40];
    snprintf(vroot, sizeof vroot, "/%s", id);
    if (!fs_grant_for(ctx, argv[0], vroot, true))
        return JS_EXCEPTION;
    if (s_fmt_vol)
        return JS_ThrowTypeError(ctx, "fs.format: already running");

    JSValue r = register_cb(ctx, argv[2], &s_cur_wk->fsop_used,
                            &s_cur_wk->fsop_cb);
    if (JS_IsException(r))
        return r;
    s_fmt_vol    = v;
    s_fmt_worker = s_cur_wk->idx;
    s_fmt_gen    = s_cur_wk->gen;
    /* 6KB: f_mkfs は作業バッファを自分で確保するが、VFS/FATFS の呼び出しが
       深いので既定の 4KB では心もとない。 */
    if (xTaskCreate(fs_format_task, "fs_format", 6144, NULL, 4, NULL) != pdPASS) {
        s_fmt_vol = NULL;
        s_cur_wk->fsop_used = false;
        JS_DeleteGCRef(ctx, &s_cur_wk->fsop_cb);
        return JS_NewBool(0);
    }
    return JS_NewBool(1);
#else
    (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "fs.format: no filesystem on this build");
#endif
}

/* fs.mount(volumeId) -> bool
   マウントは何も壊さない (入っているカードを読めるようにするだけ) ので
   grant を要らない。fs.unmount は別扱い: 他アプリが書いている最中に
   外せてしまうため、そのボリュームへの書き込み grant を要求する。 */
JSValue js_fs_mount(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; (void)argc;
#ifdef ESP_PLATFORM
    char id[32];
    if (uiw_copy_str(ctx, argv[0], id, sizeof id))
        return JS_EXCEPTION;
    const fsvol_t *v = fsvol_find(id);
    if (!v)
        return fs_throw(ctx, "mount", ESP_ERR_NOT_FOUND);
    esp_err_t err = fsvol_mount(v);
    if (err != ESP_OK)
        return fs_throw(ctx, "mount", err);
    return JS_NewBool(1);
#else
    (void)argv;
    return JS_ThrowTypeError(ctx, "fs.mount: no filesystem on this build");
#endif
}

JSValue js_fs_unmount(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val; (void)argc;
#ifdef ESP_PLATFORM
    char id[32];
    if (uiw_copy_str(ctx, argv[1], id, sizeof id))
        return JS_EXCEPTION;
    const fsvol_t *v = fsvol_find(id);
    if (!v)
        return fs_throw(ctx, "unmount", ESP_ERR_NOT_FOUND);
    char vroot[40];
    snprintf(vroot, sizeof vroot, "/%s", id);
    if (!fs_grant_for(ctx, argv[0], vroot, true))
        return JS_EXCEPTION;
    esp_err_t err = fsvol_unmount(v);
    if (err != ESP_OK)
        return fs_throw(ctx, "unmount", err);
    return JS_NewBool(1);
#else
    (void)argv;
    return JS_ThrowTypeError(ctx, "fs.unmount: no filesystem on this build");
#endif
}
/* ------------------------------------------------------------------ */
/* clipboard: typed, system-shared buffer (P4d, ssh-terminal §7).      */
/* One C-owned value outside every JS context: survives app stops and  */
/* foreground switches, and is the first app-to-app data hand-off      */
/* (calculator result -> terminal paste). The type tag is free-form    */
/* MIME-ish text ("text/plain", "text/csv", "application/json",       */
/* "number", ...) so the receiver can decide what to do with the data. */
/* Local-first (§7.1): persisted in NVS, NOT in retained MQTT — a      */
/* mirror app can bridge clipboard.onChange <-> mqtt later (layer 2).  */
/* clipboard.set posts EV_CLIP to every OTHER app with an onChange     */
/* handler (the setter knows what it did; this also keeps a future     */
/* mqtt-mirror app from echoing its own writes back to the broker).    */
/* Handlers read the CURRENT value at dispatch time: latest wins.      */
/* ------------------------------------------------------------------ */

#define MQJS_CLIP_DATA_MAX 4000 /* one NVS blob; covers a full 80x33
                                   terminal screen selection */
#define MQJS_CLIP_TYPE_MAX 31

static char *s_clip_data;          /* heap; NULL = empty clipboard */
static size_t s_clip_len;
static char s_clip_type[MQJS_CLIP_TYPE_MAX + 1];
#ifdef ESP_PLATFORM
/* T3c: the stats panel peeks at the clipboard from the LVGL task, so
   the (infrequent) buffer swaps and the peek copy take a small lock.
   Everything else clipboard runs on the JS task as before. */
static SemaphoreHandle_t s_clip_mtx;
#endif

#ifdef ESP_PLATFORM
static nvs_handle_t s_clip_nvs;
static bool s_clip_nvs_open;
static bool s_clip_loaded;

static bool clip_nvs_open(void)
{
    if (s_clip_nvs_open)
        return true;
    /* own namespace: store.* keys live in "mqjs" and must not collide */
    if (nvs_open("mqjsclip", NVS_READWRITE, &s_clip_nvs) != ESP_OK) {
        nvs_flash_init();
        if (nvs_open("mqjsclip", NVS_READWRITE, &s_clip_nvs) != ESP_OK)
            return false;
    }
    s_clip_nvs_open = true;
    return true;
}

/* lazy boot-time restore: makes the clipboard survive reboots (§7.1
   "再起動後も残す" without depending on a broker being up) */
static void clip_load(void)
{
    if (s_clip_loaded)
        return;
    s_clip_loaded = true;
    if (!clip_nvs_open())
        return;
    size_t len = 0;
    if (nvs_get_blob(s_clip_nvs, "data", NULL, &len) != ESP_OK ||
        len == 0 || len > MQJS_CLIP_DATA_MAX)
        return;
    char *buf = malloc(len);
    if (!buf)
        return;
    if (nvs_get_blob(s_clip_nvs, "data", buf, &len) != ESP_OK) {
        free(buf);
        return;
    }
    char type[MQJS_CLIP_TYPE_MAX + 1];
    size_t tlen = sizeof type;
    if (nvs_get_str(s_clip_nvs, "type", type, &tlen) != ESP_OK)
        snprintf(type, sizeof type, "text/plain");
    if (s_clip_mtx)
        xSemaphoreTake(s_clip_mtx, portMAX_DELAY);
    snprintf(s_clip_type, sizeof s_clip_type, "%s", type);
    s_clip_data = buf;
    s_clip_len = len;
    if (s_clip_mtx)
        xSemaphoreGive(s_clip_mtx);
}

static void clip_persist(void)
{
    if (!clip_nvs_open())
        return;
    if (!s_clip_data) {
        nvs_erase_key(s_clip_nvs, "data");
        nvs_erase_key(s_clip_nvs, "type");
    } else {
        nvs_set_blob(s_clip_nvs, "data", s_clip_data, s_clip_len);
        nvs_set_str(s_clip_nvs, "type", s_clip_type);
    }
    nvs_commit(s_clip_nvs);
}
#else
#define clip_load()    ((void)0)
#define clip_persist() ((void)0) /* PC: session-only */
#endif

/* clipboard.set(data[, type]) -> bool: replace the shared value and
   notify every other app. type defaults to "text/plain"; an empty
   data string clears the clipboard. */
JSValue js_clipboard_set(JSContext *ctx, JSValue *this_val, int argc,
                         JSValue *argv)
{
    char type[MQJS_CLIP_TYPE_MAX + 1];
    snprintf(type, sizeof type, "text/plain");
    if (argc >= 2 && !JS_IsUndefined(argv[1]) &&
        uiw_copy_str(ctx, argv[1], type, sizeof type))
        return JS_EXCEPTION;
    /* read the data AFTER the type was copied out (compacting GC may
       move the first string on a second ToCString — store.set idiom) */
    JSCStringBuf buf;
    size_t len;
    const char *data = JS_ToCStringLen(ctx, &len, argv[0], &buf);
    if (!data)
        return JS_EXCEPTION;
    if (len > MQJS_CLIP_DATA_MAX)
        return JS_ThrowRangeError(ctx, "clipboard data too large (max %d)",
                                  MQJS_CLIP_DATA_MAX);
    char *copy = NULL;
    if (len > 0) {
        copy = malloc(len);
        if (!copy)
            return JS_ThrowOutOfMemory(ctx);
        memcpy(copy, data, len);
    }
    clip_load(); /* mark loaded: this value now shadows whatever NVS had */
#ifdef ESP_PLATFORM
    if (s_clip_mtx)
        xSemaphoreTake(s_clip_mtx, portMAX_DELAY);
#endif
    free(s_clip_data);
    s_clip_data = copy;
    s_clip_len = copy ? len : 0;
    snprintf(s_clip_type, sizeof s_clip_type, "%s", type);
#ifdef ESP_PLATFORM
    if (s_clip_mtx)
        xSemaphoreGive(s_clip_mtx);
#endif
    clip_persist();

    /* wake the other listeners (best effort: a full queue drops the
       nudge, the value itself is never lost) */
    for (int i = 0; i < MQJS_MAX_WORKERS; i++) {
        MqjsWorker *app = &s_workers[i];
        if (!app->used || !app->clip_used || app == s_cur_wk)
            continue;
        MqjsEvent ev = { .type = EV_CLIP, .worker = app->idx,
                         .gen = app->gen };
        ev_post(&ev, 0);
    }
    return JS_NewBool(1);
}

/* T3c stats panel: copy the clipboard head for display (callable from
   the LVGL task — the only clipboard entry point off the JS task).
   Returns false on an empty clipboard. The data is truncated to dcap-1
   bytes at a UTF-8 boundary. */
bool mqjs_clipboard_peek(char *type, size_t tcap, char *data, size_t dcap)
{
#ifdef ESP_PLATFORM
    if (!s_clip_mtx)
        return false;
    xSemaphoreTake(s_clip_mtx, portMAX_DELAY);
    bool has = s_clip_data != NULL;
    if (has) {
        snprintf(type, tcap, "%s", s_clip_type);
        size_t n = s_clip_len < dcap - 1 ? s_clip_len : dcap - 1;
        if (n < s_clip_len) /* truncated: back off to a UTF-8 boundary */
            while (n > 0 && ((unsigned char)s_clip_data[n] & 0xC0) == 0x80)
                n--;
        memcpy(data, s_clip_data, n);
        data[n] = '\0';
    }
    xSemaphoreGive(s_clip_mtx);
    return has;
#else
    (void)type;
    (void)tcap;
    (void)data;
    (void)dcap;
    return false;
#endif
}

/* clipboard.get() -> {data, type} | undefined (empty clipboard) */
JSValue js_clipboard_get(JSContext *ctx, JSValue *this_val, int argc,
                         JSValue *argv)
{
    clip_load();
    if (!s_clip_data)
        return JS_UNDEFINED;
    JSGCRef obj_ref;
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    /* root obj: the compacting GC moves it when the strings allocate */
    JS_PUSH_VALUE(ctx, obj);
    JS_SetPropertyStr(ctx, obj_ref.val, "data",
                      JS_NewStringLen(ctx, s_clip_data, s_clip_len));
    JS_SetPropertyStr(ctx, obj_ref.val, "type",
                      JS_NewString(ctx, s_clip_type));
    JS_POP_VALUE(ctx, obj);
    return obj;
}

/* clipboard.onChange(fn(data, type)): fires when ANOTHER app replaces
   the clipboard. Registration counts as pending (an app may live as a
   pure clipboard listener, e.g. the future mqtt mirror). */
JSValue js_clipboard_onChange(JSContext *ctx, JSValue *this_val, int argc,
                              JSValue *argv)
{
    return register_cb(ctx, argv[0], &s_cur_wk->clip_used,
                       &s_cur_wk->clip_cb);
}

static void dispatch_clip(MqjsWorker *app, const MqjsEvent *ev)
{
    (void)ev; /* no payload: the handler reads the current value */
    JSContext *ctx = app->ctx;
    if (!app->clip_used || !s_clip_data)
        return; /* cleared again before dispatch: nothing to report */
    if (JS_StackCheck(ctx, 4)) {
        dump_error(ctx);
        return;
    }
    /* args are pushed in reverse: last-pushed becomes arg0 */
    JS_PushArg(ctx, JS_NewString(ctx, s_clip_type));               /* arg1 */
    JS_PushArg(ctx, JS_NewStringLen(ctx, s_clip_data, s_clip_len)); /* arg0 */
    JS_PushArg(ctx, app->clip_cb.val);                             /* func */
    JS_PushArg(ctx, JS_NULL);                                      /* this */
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 2);
    if (JS_IsException(ret))
        dump_error(ctx);
}

/* ------------------------------------------------------------------ */
/* camera: Tab5 barcode scan (camera.scan/cancel/status, cam_tab5)     */
/* One scanner system-wide. The scan task's callback marshals into the */
/* shared queue as a slot-addressed EV_CAM; the registering app's      */
/* one-shot callback fires with the code string (or undefined).        */
/* ------------------------------------------------------------------ */

#if defined(ESP_PLATFORM) && CONFIG_MQJS_CAMERA
static volatile bool s_cam_active;
static uint8_t s_cam_worker;
static uint16_t s_cam_gen;

/* runs on the cam_scan task */
static void cam_done_cb(const char *code, void *arg)
{
    (void)arg;
    MqjsEvent ev = { .type = EV_CAM, .worker = s_cam_worker, .gen = s_cam_gen };
    if (code) {
        size_t len = strnlen(code, CAM_TAB5_QR_PAYLOAD_MAX + 1);
        if (len <= CAM_TAB5_QR_PAYLOAD_MAX) {
            ev.u.cam.text = malloc(len + 1);
            if (ev.u.cam.text) {
                memcpy(ev.u.cam.text, code, len + 1);
                ev.u.cam.len = len;
                ev.u.cam.ok = 1;
            }
        }
    }
    s_cam_active = false;
    if (!ev_post(&ev, 0))
        free(ev.u.cam.text);
}
#endif

/* camera.scan(fn[, prefix]) -> 1 scan started / 0 busy-or-unavailable.
   fn(code_string | undefined) fires exactly once. prefix filters codes
   in C ("97" = ISBN Bookland 978/979 — Japanese books carry a second
   192... JAN right below the ISBN barcode, which this rejects). */
JSValue js_camera_scan(JSContext *ctx, JSValue *this_val, int argc,
                       JSValue *argv)
{
#if defined(ESP_PLATFORM) && CONFIG_MQJS_CAMERA
    char prefix[8] = "";
    if (argc >= 2 && !JS_IsUndefined(argv[1]) &&
        uiw_copy_str(ctx, argv[1], prefix, sizeof prefix))
        return JS_EXCEPTION;
    if (s_cam_active)
        return JS_NewBool(0);
    JSValue r = register_cb(ctx, argv[0], &s_cur_wk->cam_used,
                            &s_cur_wk->cam_cb);
    if (JS_IsException(r))
        return r;
    s_cam_worker = s_cur_wk->idx;
    s_cam_gen = s_cur_wk->gen;
    s_cam_active = true;
    /* (the scan task itself is created inside cam_tab5, pinned there) */
    if (!cam_tab5_scan_start(45000, prefix, cam_done_cb, NULL)) {
        s_cam_active = false;
        s_cur_wk->cam_used = false;
        JS_DeleteGCRef(ctx, &s_cur_wk->cam_cb);
        return JS_NewBool(0);
    }
    return JS_NewBool(1);
#else
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(0);
#endif
}

/* camera.scanQr(fn) -> trusted system apps only. The decoded text is passed
   to the callback for on-device provisioning tests, but never logged. */
JSValue js_camera_scan_qr(JSContext *ctx, JSValue *this_val, int argc,
                          JSValue *argv)
{
#if defined(ESP_PLATFORM) && CONFIG_MQJS_CAMERA
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    if (s_cam_active)
        return JS_NewBool(0);
    JSValue r = register_cb(ctx, argv[0], &s_cur_wk->cam_used,
                            &s_cur_wk->cam_cb);
    if (JS_IsException(r))
        return r;
    s_cam_worker = s_cur_wk->idx;
    s_cam_gen = s_cur_wk->gen;
    s_cam_active = true;
    if (!cam_tab5_qr_scan_start(45000, cam_done_cb, NULL)) {
        s_cam_active = false;
        s_cur_wk->cam_used = false;
        JS_DeleteGCRef(ctx, &s_cur_wk->cam_cb);
        return JS_NewBool(0);
    }
    return JS_NewBool(1);
#else
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    return JS_NewBool(0);
#endif
}

/* camera.cancel(): abort the running scan; its callback still fires
   (with undefined) through the normal event path. */
JSValue js_camera_cancel(JSContext *ctx, JSValue *this_val, int argc,
                         JSValue *argv)
{
#if defined(ESP_PLATFORM) && CONFIG_MQJS_CAMERA
    cam_tab5_cancel();
#endif
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

/* camera.status() -> last init/scan state string (remote diagnosis:
   the Tab5 has no usable serial in the field). */
JSValue js_camera_status(JSContext *ctx, JSValue *this_val, int argc,
                         JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
#ifdef ESP_PLATFORM
    return JS_NewString(ctx, cam_tab5_status());
#else
    return JS_NewString(ctx, "no camera on PC");
#endif
}

static void dispatch_cam(MqjsWorker *app, const MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    if (!app->cam_used) {
        free(ev->u.cam.text);
        return;
    }
    if (JS_StackCheck(ctx, 3)) {
        free(ev->u.cam.text);
        dump_error(ctx);
        return;
    }
    JS_PushArg(ctx, ev->u.cam.ok
                    ? JS_NewStringLen(ctx, ev->u.cam.text, ev->u.cam.len)
                    : JS_UNDEFINED);                            /* arg0 */
    JS_PushArg(ctx, app->cam_cb.val);                            /* func */
    JS_PushArg(ctx, JS_NULL);                                    /* this */
    /* one-shot: release before the call (the arg stack roots the fn) so
       the handler can immediately camera.scan() again */
    app->cam_used = false;
    JS_DeleteGCRef(ctx, &app->cam_cb);
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 1);
    free(ev->u.cam.text);
    if (JS_IsException(ret))
        dump_error(ctx);
}

/* fs.request の返事。grant はここ (JS タスクの上) で発行する: 権限表を
   触るのがこのタスクだけになり、ロックが要らなくなる。 */
static void dispatch_fsgrant(MqjsWorker *app, const MqjsEvent *ev)
{
#ifdef ESP_PLATFORM
    JSContext *ctx = app->ctx;
    uint32_t token = 0;

    FsPending *p = NULL;
    for (int i = 0; i < MQJS_FS_GRANTS; i++)
        if (s_fs_pending[i].id && s_fs_pending[i].id == ev->u.fsgrant.id) {
            p = &s_fs_pending[i];
            break;
        }
    if (p && ev->u.fsgrant.ok) {
        /* 同意が出るまでの間にカードが抜かれていることがある。ここで
           解決し直し、通らなければ黙って「拒否」と同じ結果にする。 */
        const fsvol_t *vol = NULL;
        if (fsvol_resolve(p->root, &vol, NULL, 0) == ESP_OK) {
            FsGrant *g = fs_grant_slot();
            s_fs_token_seq = (s_fs_token_seq + 1) & 0x7fffffff;
            if (!s_fs_token_seq)
                s_fs_token_seq = 1;
            g->token  = s_fs_token_seq;
            g->worker = p->worker;
            g->gen    = p->gen;
            g->write  = p->write;
            g->vol    = vol;
            g->epoch  = fsvol_epoch(vol);
            snprintf(g->root, sizeof g->root, "%s", p->root);
            token = g->token;
        }
    }
    if (p)
        p->id = 0;

    if (!app->fsreq_used)
        return;
    if (JS_StackCheck(ctx, 3)) {
        dump_error(ctx);
        return;
    }
    JS_PushArg(ctx, JS_NewInt32(ctx, (int32_t)token));   /* arg0 */
    JS_PushArg(ctx, app->fsreq_cb.val);                  /* func */
    JS_PushArg(ctx, JS_NULL);                            /* this */
    /* 一発限り: 呼ぶ前に解放しておけば、ハンドラの中から次の
       fs.request を出せる (cam/http と同じ約束)。 */
    app->fsreq_used = false;
    JS_DeleteGCRef(ctx, &app->fsreq_cb);
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 1);
    if (JS_IsException(ret))
        dump_error(ctx);
#else
    (void)app;
    (void)ev;
#endif
}

/* fs.format の結果。専用タスクから戻ってきた 1 ビットを渡すだけ。 */
static void dispatch_fsop(MqjsWorker *app, const MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    if (!app->fsop_used)
        return;
    if (JS_StackCheck(ctx, 3)) {
        dump_error(ctx);
        return;
    }
    JS_PushArg(ctx, JS_NewBool(ev->u.fsop.ok));   /* arg0 */
    JS_PushArg(ctx, app->fsop_cb.val);            /* func */
    JS_PushArg(ctx, JS_NULL);                     /* this */
    app->fsop_used = false;
    JS_DeleteGCRef(ctx, &app->fsop_cb);
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 1);
    if (JS_IsException(ret))
        dump_error(ctx);
}

/* ------------------------------------------------------------------ */
/* audio: Tab5 speaker (audio.start/stop/tone/volume/stats, audio_tab5) */
/* Scalar control + telemetry only — decoded PCM is fed from C (the     */
/* Opus path calls audio_tab5_write directly), so no buffer crosses the */
/* JS boundary. tone() is async (one-shot task) to never stall the loop.*/
/* All entries are stubs (return false / "off") off-device.            */
/* ------------------------------------------------------------------ */

/* audio.start(rate=48000, channels=2) -> true on success. */
JSValue js_audio_start(JSContext *ctx, JSValue *this_val, int argc,
                       JSValue *argv)
{
    (void)this_val;
    int rate = 48000, ch = 2;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &rate, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt32(ctx, &ch, argv[1]))
        return JS_EXCEPTION;
#if defined(ESP_PLATFORM) && CONFIG_MQJS_TAB5_AUDIO
    return JS_NewBool(audio_tab5_start((uint32_t)rate, ch) == ESP_OK);
#else
    printf("[audio] start(%d Hz, %d ch) (stub)\n", rate, ch);
    return JS_NewBool(0);
#endif
}

/* audio.stop() -> undefined. Drains the ring; HW stays initialized. */
JSValue js_audio_stop(JSContext *ctx, JSValue *this_val, int argc,
                      JSValue *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
#if defined(ESP_PLATFORM) && CONFIG_MQJS_TAB5_AUDIO
    audio_tab5_stop();
#endif
    return JS_UNDEFINED;
}

/* audio.tone(freq_hz, duration_ms) -> true if the beep was started.
   Non-blocking: a one-shot task plays it; a second call while sounding
   returns false. The P2-gate verifier callable remotely over MQTT. */
JSValue js_audio_tone(JSContext *ctx, JSValue *this_val, int argc,
                      JSValue *argv)
{
    (void)this_val;
    int freq = 0, ms = 0;
    if (argc < 2 || JS_ToInt32(ctx, &freq, argv[0]) ||
        JS_ToInt32(ctx, &ms, argv[1]))
        return JS_EXCEPTION;
#if defined(ESP_PLATFORM) && CONFIG_MQJS_TAB5_AUDIO
    return JS_NewBool(audio_tab5_tone_async(freq, ms));
#else
    printf("[audio] tone(%d Hz, %d ms) (stub)\n", freq, ms);
    return JS_NewBool(0);
#endif
}

/* audio.volume([pct]) -> current volume 0..100. With an arg, sets it. */
JSValue js_audio_volume(JSContext *ctx, JSValue *this_val, int argc,
                        JSValue *argv)
{
    (void)this_val;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        int pct;
        if (JS_ToInt32(ctx, &pct, argv[0]))
            return JS_EXCEPTION;
#if defined(ESP_PLATFORM) && CONFIG_MQJS_TAB5_AUDIO
        audio_tab5_set_volume(pct);
#else
        printf("[audio] volume(%d) (stub)\n", pct);
#endif
    }
#if defined(ESP_PLATFORM) && CONFIG_MQJS_TAB5_AUDIO
    return JS_NewInt32(ctx, audio_tab5_volume());
#else
    return JS_NewInt32(ctx, 0);
#endif
}

/* audio.downmix([on]) -> current bool. With an arg, sets stereo->mono
   (L+R)/2 fold for the mono speaker (default on). Off = true stereo. */
JSValue js_audio_downmix(JSContext *ctx, JSValue *this_val, int argc,
                         JSValue *argv)
{
    (void)this_val;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        int on;
        if (JS_ToInt32(ctx, &on, argv[0]))
            return JS_EXCEPTION;
#if defined(ESP_PLATFORM) && CONFIG_MQJS_TAB5_AUDIO
        audio_tab5_set_downmix(on != 0);
#else
        printf("[audio] downmix(%d) (stub)\n", on);
#endif
    }
#if defined(ESP_PLATFORM) && CONFIG_MQJS_TAB5_AUDIO
    return JS_NewBool(audio_tab5_downmix());
#else
    return JS_NewBool(1);
#endif
}

/* audio.playWav() -> false. The firmware-embedded boot WAV was removed
   (Opus is the boot audio now), so this no-arg verifier has nothing to play.
   Kept as an inert stub so existing callers do not break. */
JSValue js_audio_playwav(JSContext *ctx, JSValue *this_val, int argc,
                         JSValue *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(0);
}

/* audio.stats() -> JSON string. Remote readback (no usable serial in
   the field): running, rate, channels, queued PCM bytes, underruns,
   total frames written to I2S. */
JSValue js_audio_stats(JSContext *ctx, JSValue *this_val, int argc,
                       JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    char buf[160];
#if defined(ESP_PLATFORM) && CONFIG_MQJS_TAB5_AUDIO
    audio_tab5_stats_t st;
    audio_tab5_get_stats(&st);
    snprintf(buf, sizeof buf,
             "{\"running\":%s,\"rate\":%lu,\"ch\":%u,\"queued\":%lu,"
             "\"underruns\":%lu,\"frames\":%llu}",
             st.running ? "true" : "false", (unsigned long)st.sample_rate,
             (unsigned)st.channels, (unsigned long)st.queued_bytes,
             (unsigned long)st.underruns,
             (unsigned long long)st.frames_written);
#else
    snprintf(buf, sizeof buf, "{\"running\":false,\"audio\":\"off\"}");
#endif
    return JS_NewString(ctx, buf);
}

/* ------------------------------------------------------------------ */
/* power.*  (Tab5 battery: pwr_tab5)                                   */
/* ------------------------------------------------------------------ */
/* Reading is open to every app -- a battery percentage is not a        */
/* capability. Everything that CHANGES power behaviour (charge current, */
/* the longevity ceiling, cutting power, the bring-up sign override) is */
/* system-app only, same gate as camera.scanQr and system.*: those      */
/* belong to device_settings.js, not to an installed app.               */
/* Off-device and on a Tab5 built without the battery option, battery() */
/* reports state "unknown" and the setters are inert.                   */
/* ------------------------------------------------------------------ */

static const char *batt_state_str(int st)
{
    switch (st) {
    case PWR_BATT_NONE:        return "none";
    case PWR_BATT_DISCHARGING: return "discharging";
    case PWR_BATT_CHARGING:    return "charging";
    case PWR_BATT_FULL:        return "full";
    case PWR_BATT_LIMITED:     return "limited";
    default:                   return "unknown";
    }
}

/* power.battery() -> {ok, pct, state, mv, ma, ocv, mohm, mah, cap, eta,
   usb, charging, limit, tier, raw, n}. pct and eta are -1 when unknown
   (no pack, or no sample yet); `raw` is the expander input register, in
   the object for bring-up only. */
JSValue js_power_battery(JSContext *ctx, JSValue *this_val, int argc,
                         JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    pwr_batt_t b;
    bool ok = pwr_tab5_get(&b);

    JSGCRef obj_ref;
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_PUSH_VALUE(ctx, obj);
    JS_SetPropertyStr(ctx, obj_ref.val, "ok", JS_NewBool(ok));
    JS_SetPropertyStr(ctx, obj_ref.val, "pct", JS_NewInt32(ctx, b.pct));
    JS_SetPropertyStr(ctx, obj_ref.val, "mv", JS_NewInt32(ctx, b.mv));
    JS_SetPropertyStr(ctx, obj_ref.val, "ma", JS_NewInt32(ctx, b.ma));
    JS_SetPropertyStr(ctx, obj_ref.val, "ocv", JS_NewInt32(ctx, b.ocv_mv));
    JS_SetPropertyStr(ctx, obj_ref.val, "mohm", JS_NewInt32(ctx, b.mohm));
    JS_SetPropertyStr(ctx, obj_ref.val, "mah", JS_NewInt32(ctx, b.mah));
    JS_SetPropertyStr(ctx, obj_ref.val, "cap", JS_NewInt32(ctx, b.cap_mah));
    JS_SetPropertyStr(ctx, obj_ref.val, "eta", JS_NewInt32(ctx, b.eta_min));
    JS_SetPropertyStr(ctx, obj_ref.val, "usb", JS_NewBool(b.usb));
    JS_SetPropertyStr(ctx, obj_ref.val, "charging",
                      JS_NewBool(b.state == PWR_BATT_CHARGING));
    JS_SetPropertyStr(ctx, obj_ref.val, "limit", JS_NewInt32(ctx, b.limit_pct));
    JS_SetPropertyStr(ctx, obj_ref.val, "tier", JS_NewInt32(ctx, b.tier));
    JS_SetPropertyStr(ctx, obj_ref.val, "raw", JS_NewInt32(ctx, b.in_sta));
    JS_SetPropertyStr(ctx, obj_ref.val, "n", JS_NewInt32(ctx, (int32_t)b.samples));
    /* The string goes into a local first: creating it can move `obj`, and
       the evaluation order of the two arguments is unspecified. */
    JSValue st = JS_NewString(ctx, batt_state_str(b.state));
    JS_SetPropertyStr(ctx, obj_ref.val, "state", st);
    JS_POP_VALUE(ctx, obj);
    return obj;
}

/* power.charge([mA]) -> current selector. 0 = off, 500 = normal,
   1000 = quick charge. Without an argument it only reads. */
JSValue js_power_charge(JSContext *ctx, JSValue *this_val, int argc,
                        JSValue *argv)
{
    (void)this_val;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        if (!system_api_allowed(ctx))
            return JS_EXCEPTION;
        int ma;
        if (JS_ToInt32(ctx, &ma, argv[0]))
            return JS_EXCEPTION;
        pwr_tab5_set_charge_ma(ma);
    }
    return JS_NewInt32(ctx, pwr_tab5_charge_ma());
}

/* power.limit([pct]) -> the charge ceiling. 100 = charge to full. */
JSValue js_power_limit(JSContext *ctx, JSValue *this_val, int argc,
                       JSValue *argv)
{
    (void)this_val;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        if (!system_api_allowed(ctx))
            return JS_EXCEPTION;
        int pct;
        if (JS_ToInt32(ctx, &pct, argv[0]))
            return JS_EXCEPTION;
        pwr_tab5_set_limit(pct);
    }
    return JS_NewInt32(ctx, pwr_tab5_limit());
}

/* power.fullOnce(): ignore the ceiling until this charge terminates. */
JSValue js_power_full_once(JSContext *ctx, JSValue *this_val, int argc,
                           JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    pwr_tab5_full_charge_once();
    return JS_UNDEFINED;
}

/* power.off(): cut power now. No countdown, no onStop -- that path is the
   battery ladder's; this is the user asking. */
JSValue js_power_off(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    pwr_tab5_power_off();
    return JS_UNDEFINED;
}

/* power.sign(s): bring-up only -- flips the INA226 current polarity so a
   probe can settle the convention without a reflash. */
JSValue js_power_sign(JSContext *ctx, JSValue *this_val, int argc,
                      JSValue *argv)
{
    (void)this_val;
    if (!system_api_allowed(ctx))
        return JS_EXCEPTION;
    int s = 1;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &s, argv[0]))
        return JS_EXCEPTION;
    pwr_tab5_set_current_sign(s);
    return JS_NewInt32(ctx, s < 0 ? -1 : 1);
}

/* ------------------------------------------------------------------ */
/* http: one-shot GET (http.get, esp_http_client + esp_crt_bundle)     */
/* One request system-wide. A short-lived FreeRTOS task runs the        */
/* blocking esp_http_client and marshals the body into the shared       */
/* queue as a slot-addressed EV_HTTP; the registering app's one-shot    */
/* callback fires with (body_string|undefined, status_int). Mirrors the */
/* camera scanner pattern. LAN-first: http:// and https:// both allowed.*/
/* ------------------------------------------------------------------ */

#define MQJS_HTTP_MAX_URL  512
#define MQJS_HTTP_MAX_BODY 49152

#ifdef ESP_PLATFORM
static volatile bool s_http_active;
static uint8_t s_http_worker;
static uint16_t s_http_gen;
static char s_http_url[MQJS_HTTP_MAX_URL];

/* runs on the http_get task: blocking client, then post one EV_HTTP */
static void http_get_task(void *arg)
{
    (void)arg;
    MqjsEvent ev = { .type = EV_HTTP, .worker = s_http_worker, .gen = s_http_gen };
    ev.u.http.body = NULL;
    ev.u.http.len = 0;
    ev.u.http.status = -1;

    esp_http_client_config_t cfg = {
        .url = s_http_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli) {
        esp_err_t err = esp_http_client_open(cli, 0);
        if (err == ESP_OK) {
            int hdr = esp_http_client_fetch_headers(cli); /* <0 on error */
            (void)hdr;
            char *body = malloc(MQJS_HTTP_MAX_BODY);
            int total = 0;
            if (body) {
                while (total < MQJS_HTTP_MAX_BODY) {
                    int r = esp_http_client_read(cli, body + total,
                                                 MQJS_HTTP_MAX_BODY - total);
                    if (r <= 0) /* 0 = done, <0 = chunk-decode/transport end */
                        break;
                    total += r;
                }
                ev.u.http.body = body;
                ev.u.http.len = (uint32_t)total;
            }
            ev.u.http.status = (int16_t)esp_http_client_get_status_code(cli);
        } else {
            ESP_LOGW(TAG, "http.get open failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(cli);
    }
    s_http_active = false;
    if (!ev_post(&ev, 100))
        free(ev.u.http.body); /* queue full: don't leak the body */
    vTaskDelete(NULL);
}
#endif

/* http.get(url, fn) -> 1 request started / 0 busy-or-unavailable.
   fn(body_string | undefined, status_int) fires exactly once. status<=0
   means the request never produced a response (connect/TLS/timeout). */
JSValue js_http_get(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    (void)this_val;
    (void)argc;
    if (s_http_active)
        return JS_NewBool(0);
    if (!s_net_token)   /* link not up: report "not started" (its bool
                           contract), don't enter lwip. Wait on net.onReady. */
        return JS_NewBool(0);
    if (uiw_copy_str(ctx, argv[0], s_http_url, sizeof s_http_url))
        return JS_EXCEPTION;
    if (!s_http_url[0])
        return JS_NewBool(0);
    JSValue r = register_cb(ctx, argv[1], &s_cur_wk->http_used,
                            &s_cur_wk->http_cb);
    if (JS_IsException(r))
        return r;
    s_http_worker = s_cur_wk->idx;
    s_http_gen = s_cur_wk->gen;
    s_http_active = true;
    /* pinned to core 0 (with the JS task, which outranks it at prio 5):
       unpinned it competed with the core-1 LVGL task (audit §3.1) */
    if (xTaskCreatePinnedToCore(http_get_task, "http_get", 8192, NULL, 4,
                                NULL, 0) != pdPASS) {
        s_http_active = false;
        s_cur_wk->http_used = false;
        JS_DeleteGCRef(ctx, &s_cur_wk->http_cb);
        return JS_NewBool(0);
    }
    return JS_NewBool(1);
#else
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(0);
#endif
}

static void dispatch_http(MqjsWorker *app, MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    if (!app->http_used) {
        free(ev->u.http.body);
        ev->u.http.body = NULL;
        return;
    }
    if (JS_StackCheck(ctx, 4)) {
        dump_error(ctx);
        free(ev->u.http.body);
        ev->u.http.body = NULL;
        return;
    }
    JS_PushArg(ctx, JS_NewInt32(ctx, ev->u.http.status));            /* arg1 */
    JS_PushArg(ctx, ev->u.http.body ? JS_NewStringLen(ctx, ev->u.http.body,
                                                      ev->u.http.len)
                                    : JS_UNDEFINED);                 /* arg0 */
    JS_PushArg(ctx, app->http_cb.val);                               /* func */
    JS_PushArg(ctx, JS_NULL);                                        /* this */
    /* one-shot: release before the call (the arg stack roots the fn) so
       the handler can immediately http.get() again */
    app->http_used = false;
    JS_DeleteGCRef(ctx, &app->http_cb);
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 2);
    if (JS_IsException(ret))
        dump_error(ctx);
    free(ev->u.http.body);
    ev->u.http.body = NULL;
}

/* ------------------------------------------------------------------ */
/* sys: heap telemetry (W1-4) + P4a lifecycle / signals                */
/* ------------------------------------------------------------------ */

/* sys.heap() -> [internal_free, psram_free, lvgl_pool_free,
                  l2_free, l2_largest, l2_min_ever, lp_free, psram_largest].
   [0..2] are the legacy trio (existing callers index them).
   [3..5] view the 576 KB L2MEM through MALLOC_CAP_DMA — the only caps
   unique to L2 — because [0] (MALLOC_CAP_INTERNAL) also counts the
   LP SRAM overflow region (ALLOW_RTC_FAST_MEM_AS_HEAP) whose 32 KB
   block would mask real L2 numbers and fragmentation. [5] is the
   low-water mark since boot: transient dips (SDIO bursts, camera scan
   scratch) register here even when sampling misses them.
   The third element matters most for widget churn: the LVGL tlsf pool
   is preallocated from PSRAM (W1-1), so leaks inside it are invisible
   to the OS heap counters. */
JSValue js_sys_heap(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    uint32_t internal = 0, psram = 0, lvgl = 0;
    uint32_t l2f = 0, l2max = 0, l2min = 0, lpf = 0, psmax = 0;
#ifdef ESP_PLATFORM
    internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    lvgl = ui_tab5_lv_mem_free();
    l2f = heap_caps_get_free_size(MALLOC_CAP_DMA);
    l2max = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    l2min = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
    lpf = heap_caps_get_free_size(MALLOC_CAP_RTCRAM);
    psmax = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
#endif
    JSValue arr = JS_NewArray(ctx, 0);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewUint32(ctx, internal));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewUint32(ctx, psram));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewUint32(ctx, lvgl));
    JS_SetPropertyUint32(ctx, arr, 3, JS_NewUint32(ctx, l2f));
    JS_SetPropertyUint32(ctx, arr, 4, JS_NewUint32(ctx, l2max));
    JS_SetPropertyUint32(ctx, arr, 5, JS_NewUint32(ctx, l2min));
    JS_SetPropertyUint32(ctx, arr, 6, JS_NewUint32(ctx, lpf));
    JS_SetPropertyUint32(ctx, arr, 7, JS_NewUint32(ctx, psmax));
    return arr;
}

/* sys.micros() -> monotonic microseconds since boot.
   performance.now() divides the same hardware counter by 1000, which is
   useless for anything that costs microseconds (the IME key path, one
   ui.cells run): a single call reads as 0 ms. This keeps the µs.

   Two notes for benchmark authors:
     - short int tops out at 2^30-1, so past 17.9 minutes of uptime the
       return value becomes a float64 and costs one arena allocation per
       call. Read it OUTSIDE the loop you are timing, never inside.
     - it is NOT the wall clock; Date.now() jumps on SNTP sync, this
       does not. */
JSValue js_sys_micros(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt64(ctx, time_us());
}

/* sys.onForeground(fn): called after this app becomes foreground — the
   app rebuilds its screens/canvas here (destroy-on-switch model, §3.3).
   Registering any lifecycle/signal handler keeps the app alive. */
JSValue js_sys_onForeground(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return register_cb(ctx, argv[0], &s_cur_wk->fg_used, &s_cur_wk->fg_cb);
}

/* sys.onBackground(fn): called right BEFORE the app's screens are
   destroyed on a foreground switch (last chance to snapshot UI state). */
JSValue js_sys_onBackground(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return register_cb(ctx, argv[0], &s_cur_wk->bg_used, &s_cur_wk->bg_cb);
}

/* sys.onSignal(fn(value, fromName)): minimal app-to-app IPC sink (§3.8).
   Waiting for a signal counts as pending — "sleep until signalled". */
JSValue js_sys_onSignal(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return register_cb(ctx, argv[0], &s_cur_wk->sig_used, &s_cur_wk->sig_cb);
}

/* sys.onStop(fn(reason)): last-words hook (Phase 4) — fires once while
   the app is being stopped (reason "user" | "idle" | "updated" |
   "evicted" | "error"), before the bindings are torn down: the place
   to store.set state for the restore-on-next-start pattern (store.set
   is a RAM-cache write, safe in a dying app). Registering it does NOT
   keep an idle app alive, and the 5s watchdog bounds the handler. */
JSValue js_sys_onStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return register_cb(ctx, argv[0], &s_cur_wk->stop_used, &s_cur_wk->stop_cb);
}

/* sys.signal(appName, value) -> bool: queue a signal for the named app
   (value is stringified; JSON is the convention for structures). False
   when no such app is running or the queue is full. */
JSValue js_sys_signal(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char name[32];
    if (uiw_copy_str(ctx, argv[0], name, sizeof name))
        return JS_EXCEPTION;
    JSCStringBuf vbuf;
    size_t vlen;
    const char *val = JS_ToCStringLen(ctx, &vlen, argv[1], &vbuf);
    if (!val)
        return JS_EXCEPTION;
    if (vlen > MQJS_SIGNAL_VAL_MAX)
        return JS_ThrowRangeError(ctx, "signal value too large (max %d)",
                                  MQJS_SIGNAL_VAL_MAX);

    MqjsWorker *target = NULL;
    for (int i = 0; i < MQJS_MAX_WORKERS; i++) {
        if (s_workers[i].used && !strcmp(s_workers[i].name, name)) {
            target = &s_workers[i];
            break;
        }
    }
    if (!target)
        return JS_NewBool(0);

    char *copy = malloc(vlen + 1);
    if (!copy)
        return JS_ThrowOutOfMemory(ctx);
    memcpy(copy, val, vlen);
    copy[vlen] = '\0';

    MqjsEvent ev = { .type = EV_SIGNAL, .worker = target->idx,
                     .gen = target->gen };
    ev.u.signal.value = copy;
    snprintf(ev.u.signal.from, sizeof ev.u.signal.from, "%s",
             s_cur_wk ? s_cur_wk->name : "");
    if (!ev_post(&ev, 0)) {
        free(copy);
        return JS_NewBool(0);
    }
    return JS_NewBool(1);
}

/* App-manager migration Phase 1: stable app names are the public
   identity; slot numbers stay internal (Worker index). Resolve a name
   to its live slot. "dev" answers the dev slot whatever its setAppName
   identity is, so callers need not know the pushed task's name. */
static int app_slot_by_name(const char *name)
{
    for (int i = 0; i < MQJS_MAX_WORKERS; i++)
        if (s_workers[i].used && !strcmp(s_workers[i].name, name))
            return i;
    if (!strcmp(name, "dev") && s_workers[MQJS_WORKER_DEV].used)
        return MQJS_WORKER_DEV;
    return -1;
}

/* sys.focus(name) -> bool / sys.focus(slot) [compat]: request a
   foreground switch (queued — the switch never happens in the middle
   of the caller's own callback). P4a: any app may call it; restricting
   it to the launcher is a P4b/P4c rule. The name form returns false
   for an unknown/stopped app instead of throwing. */
JSValue js_sys_focus(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int slot;
    if (JS_IsString(ctx, argv[0])) {
        char name[32];
        if (uiw_copy_str(ctx, argv[0], name, sizeof name))
            return JS_EXCEPTION;
        slot = app_slot_by_name(name);
        if (slot < 0)
            return JS_NewBool(0);
        MqjsEvent ev = { .type = EV_FOCUS };
        ev.u.focus.target = (uint8_t)slot;
        return JS_NewBool(ev_post(&ev, 0));
    }
    if (JS_ToInt32(ctx, &slot, argv[0]))
        return JS_EXCEPTION;
    if (slot < 0 || slot >= MQJS_MAX_WORKERS)
        return JS_ThrowRangeError(ctx, "bad app slot");
    MqjsEvent ev = { .type = EV_FOCUS };
    ev.u.focus.target = (uint8_t)slot;
    ev_post(&ev, 0);
    return JS_UNDEFINED;
}

/* ---- P4b: launcher support (apps/launch/stop/setAppName/notify) ---- */

static int app_start_internal(MqjsWorker *app, const char *src, size_t src_len,
                              const char *name, bool trusted_system);
static void app_stop_internal(MqjsWorker *app);
static void app_stop_internal_r(MqjsWorker *app, mqjs_app_stop_reason_t reason);
static void switch_foreground(int new_slot);

/* sys.setAppName(name) -> bool: the app's identity for sys.signal, the
   status-bar chip and the launcher. False on a duplicate (names are
   the address space) or an unusable name. */
JSValue js_sys_setAppName(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char name[32];
    if (uiw_copy_str(ctx, argv[0], name, sizeof name))
        return JS_EXCEPTION;
    if (!name[0])
        return JS_NewBool(0);
    /* "system" is the sender the runtime itself signs its signals with
       (the fs.request consent ask, filer-storage-design §7). An app that
       could take that name could put a forged permission prompt in front
       of the launcher, so the name is reserved. */
    if (!strcmp(name, "system"))
        return JS_NewBool(0);
    for (const char *p = name; *p; p++) {
        /* names travel inside JSON open requests: keep them quote-free */
        if ((unsigned char)*p < 0x20 || *p == '"' || *p == '\\')
            return JS_NewBool(0);
    }
    for (int i = 0; i < MQJS_MAX_WORKERS; i++) {
        if (s_workers[i].used && &s_workers[i] != s_cur_wk &&
            !strcmp(s_workers[i].name, name))
            return JS_NewBool(0);
    }
    mqjs_app_record_on_rename(s_cur_wk->name, name);
    snprintf(s_cur_wk->name, sizeof s_cur_wk->name, "%s", name);
    if (s_cur_wk->idx == MQJS_WORKER_DEV)
        snprintf(s_last_dev_name, sizeof s_last_dev_name, "%s", name);
    bar_update();
    return JS_NewBool(1);
}

/* sys.apps() -> [{slot, name, running:true}] for every live app. The
   compacting GC moves objects on allocation, so parents are rooted with
   the JS_PUSH_VALUE stack refs while children are created. */
JSValue js_sys_apps(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSGCRef arr_ref, obj_ref;
    JSValue arr = JS_NewArray(ctx, 0);
    if (JS_IsException(arr))
        return arr;
    JS_PUSH_VALUE(ctx, arr);
    int n = 0;
    for (int i = 0; i < MQJS_MAX_WORKERS; i++) {
        if (!s_workers[i].used)
            continue;
        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) {
            JS_POP_VALUE(ctx, arr);
            return obj;
        }
        JS_PUSH_VALUE(ctx, obj);
        JSValue name = JS_NewString(ctx, s_workers[i].name);
        JS_SetPropertyStr(ctx, obj_ref.val, "name", name);
        JS_SetPropertyStr(ctx, obj_ref.val, "slot", JS_NewInt32(ctx, i));
        JS_SetPropertyStr(ctx, obj_ref.val, "running", JS_NewBool(1));
        /* Phase 1: launcher/dev specialness exposed as a kind string,
           not a slot number (the slot key is compat-only from here).
           Phase 2: the kind comes from the App record; "dev" stays a
           worker-position fact until Phase 3 turns it into policy.
           Allocate the string into a local FIRST: the compacting GC can
           move obj during JS_NewString, and a nested call may read
           obj_ref.val before the allocation runs (eval order). */
        const mqjs_app_snapshot_t *rec = mqjs_app_record_find(s_workers[i].name);
        JSValue kind = JS_NewString(ctx,
            i == MQJS_WORKER_DEV ? "dev"
            : rec && rec->kind == MQJS_APP_KIND_SYSTEM ? "system"
            : "app");
        JS_SetPropertyStr(ctx, obj_ref.val, "kind", kind);
        /* Phase 3: policy surfaces (migration table: apps() grows
           evictable). JS_NewBool is an immediate — no GC hazard. */
        JS_SetPropertyStr(ctx, obj_ref.val, "evictable",
            JS_NewBool(rec && (rec->policy.flags & MQJS_APP_EVICTABLE) ? 1 : 0));
        JS_POP_VALUE(ctx, obj);
        JS_SetPropertyUint32(ctx, arr_ref.val, n++, obj);
    }
    JS_POP_VALUE(ctx, arr);
    return arr;
}

/* Pull one "// @<key> value" directive out of a script's leading
   comment block (the in-file manifest, design §4.5). Stops at the
   first non-comment line so body strings can't spoof directives. */
static void manifest_field(const char *buf, size_t n, const char *key,
                           char *out, size_t cap)
{
    size_t klen = strlen(key);
    size_t i = 0;
    out[0] = '\0';
    while (i < n) {
        if (i + 1 < n && buf[i] == '/' && buf[i + 1] == '/') {
            if (i + klen <= n && !strncmp(buf + i, key, klen)) {
                size_t j = i + klen, k = 0;
                while (j < n && buf[j] != '\n' && buf[j] != '\r' &&
                       k < cap - 1)
                    out[k++] = buf[j++];
                while (k > 0 && out[k - 1] == ' ')
                    k--;
                out[k] = '\0';
                return;
            }
        } else if (buf[i] != '\n' && buf[i] != '\r' && buf[i] != ' ' &&
                   buf[i] != '\t') {
            return; /* past the header block */
        }
        while (i < n && buf[i] != '\n')
            i++;
        i++;
    }
}

/* presence-only manifest flag ("// @autostart"): true when the
   directive line exists in the leading comment block. The char after
   the key must end the word so "@autostartx" does not match. */
static bool manifest_has(const char *buf, size_t n, const char *key)
{
    size_t klen = strlen(key);
    size_t i = 0;
    while (i < n) {
        if (i + 1 < n && buf[i] == '/' && buf[i + 1] == '/') {
            if (i + klen <= n && !strncmp(buf + i, key, klen) &&
                (i + klen == n || buf[i + klen] == '\n' ||
                 buf[i + klen] == '\r' || buf[i + klen] == ' '))
                return true;
        } else if (buf[i] != '\n' && buf[i] != '\r' && buf[i] != ' ' &&
                   buf[i] != '\t') {
            return false; /* past the header block */
        }
        while (i < n && buf[i] != '\n')
            i++;
        i++;
    }
    return false;
}

#ifdef ESP_PLATFORM
/* ---- @autostart opt-in roster (design §8) ----
   Comma-separated app names in NVS ("mqjsauto"/"optin"). A name gets
   on the roster only when the app is launched LOCALLY while its
   installed file declares "// @autostart" — a shelf push alone never
   makes anything run at boot (the §6 principle extended to reboots).
   sys.uninstall takes the name off again. */
#define MQJS_AUTOSTART_LIST_MAX 480

static bool autostart_nvs(nvs_handle_t *out)
{
    static nvs_handle_t h;
    static bool opened;
    if (!opened) {
        if (nvs_open("mqjsauto", NVS_READWRITE, &h) != ESP_OK) {
            nvs_flash_init();
            if (nvs_open("mqjsauto", NVS_READWRITE, &h) != ESP_OK)
                return false;
        }
        opened = true;
    }
    *out = h;
    return true;
}

static void autostart_load(char *buf, size_t cap)
{
    buf[0] = '\0';
    nvs_handle_t h;
    if (!autostart_nvs(&h))
        return;
    size_t len = cap;
    if (nvs_get_str(h, "optin", buf, &len) != ESP_OK)
        buf[0] = '\0';
}

static bool autostart_list_has(const char *list, const char *name)
{
    size_t nl = strlen(name);
    const char *p = list;
    while (*p) {
        const char *q = strchr(p, ',');
        size_t l = q ? (size_t)(q - p) : strlen(p);
        if (l == nl && !strncmp(p, name, nl))
            return true;
        p = q ? q + 1 : p + l;
    }
    return false;
}

static void autostart_save(const char *list)
{
    nvs_handle_t h;
    if (!autostart_nvs(&h))
        return;
    if (list[0])
        nvs_set_str(h, "optin", list);
    else
        nvs_erase_key(h, "optin");
    nvs_commit(h);
}

static void autostart_optin_add(const char *name)
{
    char list[MQJS_AUTOSTART_LIST_MAX];
    autostart_load(list, sizeof list);
    if (autostart_list_has(list, name))
        return;
    size_t l = strlen(list);
    if (l + strlen(name) + 2 > sizeof list) {
        ESP_LOGW(TAG, "autostart roster full, '%s' not recorded", name);
        return;
    }
    snprintf(list + l, sizeof list - l, "%s%s", l ? "," : "", name);
    autostart_save(list);
    /* Phase 3: the record mirrors the NVS roster (the roster stays the
       reboot-surviving source of truth; records are RAM) */
    mqjs_app_record_set_policy(name, MQJS_APP_AUTOSTART, 0);
    ESP_LOGI(TAG, "autostart opt-in: '%s'", name);
}

static void autostart_optin_remove(const char *name)
{
    char list[MQJS_AUTOSTART_LIST_MAX];
    autostart_load(list, sizeof list);
    if (!autostart_list_has(list, name))
        return;
    char out[MQJS_AUTOSTART_LIST_MAX];
    size_t o = 0, nl = strlen(name);
    const char *p = list;
    while (*p) {
        const char *q = strchr(p, ',');
        size_t l = q ? (size_t)(q - p) : strlen(p);
        if (!(l == nl && !strncmp(p, name, nl))) {
            if (o)
                out[o++] = ',';
            memcpy(out + o, p, l);
            o += l;
        }
        p = q ? q + 1 : p + l;
    }
    out[o] = '\0';
    autostart_save(out);
    mqjs_app_record_set_policy(name, 0, MQJS_APP_AUTOSTART);
}
#endif /* ESP_PLATFORM */

/* append one {name, title, perm, icon, desc, size, autostart, optin}
   entry to the installed() array (store detail page, design §9).
   icon = a Nerd Font glyph character from the "// @icon " directive
   (the ui_font fallback chain renders it anywhere, design §4.5). */
static int installed_push(JSContext *ctx, JSGCRef *arr_ref, int idx,
                          const char *name, const char *head, size_t hlen,
                          size_t fsize)
{
    char title[48], perm[64], icon[12], desc[96];
    manifest_field(head, hlen, "// @title ", title, sizeof title);
    manifest_field(head, hlen, "// @perm ", perm, sizeof perm);
    manifest_field(head, hlen, "// @icon ", icon, sizeof icon);
    manifest_field(head, hlen, "// @desc ", desc, sizeof desc);
    bool autostart = manifest_has(head, hlen, "// @autostart");
    bool optin = false;
#ifdef ESP_PLATFORM
    if (autostart) {
        char roster[MQJS_AUTOSTART_LIST_MAX];
        autostart_load(roster, sizeof roster);
        optin = autostart_list_has(roster, name);
    }
#endif

    JSGCRef obj_ref;
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return -1;
    JS_PUSH_VALUE(ctx, obj);
    JSValue v = JS_NewString(ctx, name);
    JS_SetPropertyStr(ctx, obj_ref.val, "name", v);
    v = JS_NewString(ctx, title[0] ? title : name);
    JS_SetPropertyStr(ctx, obj_ref.val, "title", v);
    v = JS_NewString(ctx, perm);
    JS_SetPropertyStr(ctx, obj_ref.val, "perm", v);
    v = JS_NewString(ctx, icon);
    JS_SetPropertyStr(ctx, obj_ref.val, "icon", v);
    v = JS_NewString(ctx, desc);
    JS_SetPropertyStr(ctx, obj_ref.val, "desc", v);
    JS_SetPropertyStr(ctx, obj_ref.val, "size",
                      JS_NewInt32(ctx, (int32_t)fsize));
    JS_SetPropertyStr(ctx, obj_ref.val, "autostart", JS_NewBool(autostart));
    JS_SetPropertyStr(ctx, obj_ref.val, "optin", JS_NewBool(optin));
    JS_POP_VALUE(ctx, obj);
    JS_SetPropertyUint32(ctx, arr_ref->val, (uint32_t)idx, obj);
    return 0;
}

/* sys.installed() -> [{name, title, perm}] of launchable apps: the
   embedded source registry + the .js files under /littlefs/apps.
   title/perm come from the in-file manifest directives. */
JSValue js_sys_installed(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSGCRef arr_ref;
    JSValue arr = JS_NewArray(ctx, 0);
    if (JS_IsException(arr))
        return arr;
    JS_PUSH_VALUE(ctx, arr);
    int n = 0;
    for (int i = 0; i < (int)(sizeof s_app_sources / sizeof s_app_sources[0]);
         i++) {
        if (!s_app_sources[i].used || s_app_sources[i].trusted_system)
            continue;
        size_t hlen = s_app_sources[i].len < 512 ? s_app_sources[i].len : 512;
        if (installed_push(ctx, &arr_ref, n, s_app_sources[i].name,
                           s_app_sources[i].src, hlen,
                           s_app_sources[i].len)) {
            JS_POP_VALUE(ctx, arr);
            return JS_EXCEPTION;
        }
        n++;
    }
#ifdef ESP_PLATFORM
    DIR *d = opendir("/littlefs/apps");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t l = strlen(e->d_name);
            if (l < 4 || l > 34 || strcmp(e->d_name + l - 3, ".js"))
                continue;
            char base[32];
            snprintf(base, sizeof base, "%.*s", (int)(l - 3), e->d_name);
            char head[512];
            size_t hlen = 0, fsize = 0;
            char path[96];
            snprintf(path, sizeof path, "/littlefs/apps/%.40s", e->d_name);
            FILE *f = fopen(path, "rb");
            if (f) {
                hlen = fread(head, 1, sizeof head, f);
                fseek(f, 0, SEEK_END);
                long fl = ftell(f);
                fsize = fl > 0 ? (size_t)fl : 0;
                fclose(f);
            }
            if (installed_push(ctx, &arr_ref, n, base, head, hlen, fsize)) {
                closedir(d);
                JS_POP_VALUE(ctx, arr);
                return JS_EXCEPTION;
            }
            n++;
        }
        closedir(d);
    }
#endif
    JS_POP_VALUE(ctx, arr);
    return arr;
}

/* sys.store() -> [{name, title, icon, desc, size, installed, src,
   verified}] straight from the broker catalog (§11). Catalog-only on
   purpose: the launcher merges it with sys.installed() itself, and a
   device-side install shows up here as installed=true on the next call.
   `verified` is always false: a catalogue row's signature is not checked
   until the body is actually installed. */
JSValue js_sys_store(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSGCRef arr_ref;
    JSValue arr = JS_NewArray(ctx, 0);
    if (JS_IsException(arr))
        return arr;
    JS_PUSH_VALUE(ctx, arr);
    int n = 0;
    /* 棚 (MQTT) とカード (microSD) を続けて並べる。同名が両方に在っても
       勝者を決めない: 出所を row に付けて両方見せ、どちらを入れるかは
       人が選ぶ (docs/filer-storage-design.md §13)。カード側は count() が
       走査そのものなので、ストア画面を開いたときだけ読みに行くことに
       なる —— 常駐の監視は置かない。 */
    const mqjs_store_api_t *const cats[2] = { s_store_api, s_card_api };
    static const char *const cat_src[2]   = { "mqtt", "sd" };
    for (int c = 0; c < 2; c++) {
    const mqjs_store_api_t *api = cats[c];
    int cnt = api ? api->count() : 0;
    for (int i = 0; i < cnt; i++) {
        char name[25], head[224];
        if (!api->get(i, name, sizeof name, head, sizeof head))
            continue;
        size_t hlen = strlen(head);
        char title[48], icon[8], desc[120], perm[48], sizes[16];
        manifest_field(head, hlen, "// @title ", title, sizeof title);
        manifest_field(head, hlen, "// @icon ", icon, sizeof icon);
        manifest_field(head, hlen, "// @desc ", desc, sizeof desc);
        manifest_field(head, hlen, "// @perm ", perm, sizeof perm);
        manifest_field(head, hlen, "// @size ", sizes, sizeof sizes);
        long size = atol(sizes);
        bool inst = false;
        char path[96];
        snprintf(path, sizeof path, "/littlefs/apps/%.40s.js", name);
        FILE *f = fopen(path, "rb");
        if (f) {
            inst = true;
            fseek(f, 0, SEEK_END);
            long fl = ftell(f);
            if (fl > 0)
                size = fl; /* the device's copy wins over @size */
            fclose(f);
        }
        JSGCRef obj_ref;
        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) {
            JS_POP_VALUE(ctx, arr);
            return obj;
        }
        JS_PUSH_VALUE(ctx, obj);
        JSValue v = JS_NewString(ctx, name);
        JS_SetPropertyStr(ctx, obj_ref.val, "name", v);
        v = JS_NewString(ctx, title[0] ? title : name);
        JS_SetPropertyStr(ctx, obj_ref.val, "title", v);
        v = JS_NewString(ctx, icon);
        JS_SetPropertyStr(ctx, obj_ref.val, "icon", v);
        v = JS_NewString(ctx, desc);
        JS_SetPropertyStr(ctx, obj_ref.val, "desc", v);
        v = JS_NewString(ctx, perm);
        JS_SetPropertyStr(ctx, obj_ref.val, "perm", v);
        JS_SetPropertyStr(ctx, obj_ref.val, "size",
                          JS_NewInt32(ctx, (int32_t)size));
        JS_SetPropertyStr(ctx, obj_ref.val, "installed", JS_NewBool(inst));
        JSValue v_src = JS_NewString(ctx, cat_src[c]);
        JS_SetPropertyStr(ctx, obj_ref.val, "src", v_src);
        /* カタログ行は署名を**見ていない**。棚は retained メッセージを
           読んだだけ、カードはファイルの先頭 224 バイトを読んだだけで、
           どちらも本体の検証はインストールの瞬間まで走らない (カードで
           実機 226ms/本、一覧で回すと画面を開くたびに秒単位で固まる)。
           行に書いてある @title も @desc も自称にすぎないので、UI が
           「署名済み」と言い切ってよい根拠はここには無い。 */
        JS_SetPropertyStr(ctx, obj_ref.val, "verified", JS_NewBool(0));
        JS_POP_VALUE(ctx, obj);
        JS_SetPropertyUint32(ctx, arr_ref.val, (uint32_t)n++, obj);
    }
    }
    JS_POP_VALUE(ctx, arr);
    return arr;
}

/* sys.install(name) -> bool: request the async fetch of a catalog
   app's signed body (§11). true = request accepted; completion lands
   through the registry path ("installed: <name>" status/event). */
JSValue js_sys_install(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char name[64];
    if (uiw_copy_str(ctx, argv[0], name, sizeof name))
        return JS_EXCEPTION;
    /* 同じ名前が棚とカードの両方に在りうるので、どちらから入れるかは
       呼び出し側が名指しする (§13)。省略時は従来どおり棚。 */
    char src[8] = "";
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) &&
        uiw_copy_str(ctx, argv[1], src, sizeof src))
        return JS_EXCEPTION;
    const mqjs_store_api_t *api = !strcmp(src, "sd") ? s_card_api : s_store_api;
    bool ok = api && name[0] && !strchr(name, '/') && api->install(name);
    return JS_NewBool(ok);
}

static int app_free_slot(void)
{
    /* 0 = launcher, 1 = dev: launched apps live in the user slots */
    for (int i = MQJS_WORKER_DEV + 1; i < MQJS_MAX_WORKERS; i++)
        if (!s_workers[i].used)
            return i;
    return -1;
}

/* ESP arenas come from mqjs_rt_init; the PC build allocates lazily */
static bool app_ensure_mem(MqjsWorker *app)
{
#ifndef ESP_PLATFORM
    if (!app->mem) {
        app->mem = malloc(MQJS_APP_MEM_SIZE);
        app->mem_size = MQJS_APP_MEM_SIZE;
    }
#endif
    return app->mem != NULL;
}

/* Start /littlefs/apps/<arg>.js (or the verbatim path when arg has a
   '/') in `slot`. The file source is owned by the slot (freed at
   stop). Returns the slot or -1. A successful start of a file whose
   manifest declares "// @autostart" records the boot opt-in (§8):
   that local launch is exactly the user gesture the roster wants. */
static int start_from_file(int slot, const char *arg, const char *name)
{
    char path[128];
    if (strchr(arg, '/'))
        snprintf(path, sizeof path, "%.127s", arg);
    else
        snprintf(path, sizeof path, "/littlefs/apps/%.96s.js", arg);
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen <= 0 || flen > MQJS_SCRIPT_MAX) {
        fclose(f);
        return -1;
    }
#ifdef ESP_PLATFORM
    char *buf = heap_caps_malloc((size_t)flen + 1, MALLOC_CAP_SPIRAM);
    if (!buf)
        buf = malloc((size_t)flen + 1);
#else
    char *buf = malloc((size_t)flen + 1);
#endif
    if (!buf || fread(buf, 1, (size_t)flen, f) != (size_t)flen) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);
    buf[flen] = '\0';
    if (app_start_internal(&s_workers[slot], buf, (size_t)flen, name, false)) {
        free(buf);
        return -1;
    }
    s_workers[slot].src_owned = buf;
#ifdef ESP_PLATFORM
    if (manifest_has(buf, (size_t)flen, "// @autostart"))
        autostart_optin_add(name);
#endif
    return slot;
}

/* Phase 4 eviction (doc: 空きWorkerなし -> evictableなbackground App
   からLRUを選択). Candidates must be: running, not mid-stop, not the
   foreground, not the caller, EVICTABLE by policy — and not the dev
   worker (that frame belongs to the developer; reusing it would also
   inherit the dev rerun flow). Returns the freed worker index or -1. */
static int app_evict_lru(void)
{
    int victim = -1;
    int64_t oldest = 0;
    for (int i = 0; i < MQJS_MAX_WORKERS; i++) {
        MqjsWorker *w = &s_workers[i];
        if (!w->used || w->stopping || i == s_fg_worker ||
            i == MQJS_WORKER_DEV || w == s_cur_wk)
            continue;
        const mqjs_app_snapshot_t *rec = mqjs_app_record_find(w->name);
        if (!rec || !(rec->policy.flags & MQJS_APP_EVICTABLE))
            continue;
        if (victim < 0 || rec->last_active_ms < oldest) {
            victim = i;
            oldest = rec->last_active_ms;
        }
    }
    if (victim < 0)
        return -1;
    /* visible trace: console line + per-app notice ("what happened to
       my app?" stays answerable from the launcher) */
    char line[96];
    snprintf(line, sizeof line, "sys: evict '%s' (worker %d, LRU)\n",
             s_workers[victim].name, victim);
    out_write(line, strlen(line));
    Notice *n = &s_notices[0];
    for (int i = 1; i < (int)(sizeof s_notices / sizeof s_notices[0]); i++)
        if (s_notices[i].seq < n->seq)
            n = &s_notices[i];
    snprintf(n->app, sizeof n->app, "system");
    snprintf(n->text, sizeof n->text, "evicted: %.60s (枠を譲りました)",
             s_workers[victim].name);
    n->seq = ++s_notice_seq;
    if (s_notify_sink) {
        char msg[96];
        snprintf(msg, sizeof msg, "[system] evicted: %s",
                 s_workers[victim].name);
        s_notify_sink(msg);
    }
    app_stop_internal_r(&s_workers[victim], MQJS_APP_STOP_EVICTED);
    return victim;
}

/* Start-by-name core shared by sys.launch (slot compat), sys.start and
   sys.open (Phase 1 name API). Returns the slot (>= 0) or -1. Resolution:
   "dev" re-enables the dev provider; a running app returns its slot;
   then the embedded registry; then /littlefs/apps/<name>.js (or the
   verbatim path when it contains '/'). File sources are owned by the
   slot and freed at stop. */
static int sys_launch_core(const char *arg)
{
    if (!arg[0])
        return -1;

    if (!strcmp(arg, "dev")) {
        dev_rearm(); /* re-arm the rerun policy; provider asked next pass */
        return MQJS_WORKER_DEV;
    }

    /* app name = basename without .js (also the path case) */
    const char *base = arg;
    for (const char *p = arg; *p; p++)
        if (*p == '/')
            base = p + 1;
    char name[32];
    snprintf(name, sizeof name, "%.31s", base);
    char *dot = strrchr(name, '.');
    if (dot && dot != name)
        *dot = '\0';

    for (int i = 0; i < MQJS_MAX_WORKERS; i++)
        if (s_workers[i].used && !strcmp(s_workers[i].name, name))
            return i; /* already running */
    if (!strcmp(name, "launcher")) /* resident in slot 0, never elsewhere */
        return -1;
    /* the stopped dev task addressed by its setAppName identity (the
       chip remembers "ssh_vt", not "dev"): rerun via the provider */
    if (!s_workers[MQJS_WORKER_DEV].used && s_last_dev_name[0] &&
        !strcmp(name, s_last_dev_name)) {
        dev_rearm();
        return MQJS_WORKER_DEV;
    }

    int slot = app_free_slot();
    if (slot < 0)
        slot = app_evict_lru(); /* Phase 4: trade the LRU background app */
    if (slot < 0 || !app_ensure_mem(&s_workers[slot]))
        return -1;

    const AppSource *as = app_source_find(name);
    if (as) {
        if (app_start_internal(&s_workers[slot], as->src, as->len, as->name,
                               as->trusted_system))
            return -1;
        return slot;
    }

    return start_from_file(slot, arg, name);
}

/* sys.launch(nameOrPath) -> slot or -1 [compat]. Prefer sys.start /
   sys.open: new code should never need the returned worker index. */
JSValue js_sys_launch(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char arg[96];
    if (uiw_copy_str(ctx, argv[0], arg, sizeof arg))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, sys_launch_core(arg));
}

/* sys.start(name) -> bool: start (or confirm running) by name, without
   exposing the worker index (Phase 1 name API). */
JSValue js_sys_start(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char arg[96];
    if (uiw_copy_str(ctx, argv[0], arg, sizeof arg))
        return JS_EXCEPTION;
    return JS_NewBool(sys_launch_core(arg) >= 0);
}

/* sys.open(name) -> bool: the launcher's focus-or-launch staple as one
   call — start if stopped, then bring to the foreground (queued). */
JSValue js_sys_open(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char arg[96];
    if (uiw_copy_str(ctx, argv[0], arg, sizeof arg))
        return JS_EXCEPTION;
    int slot = sys_launch_core(arg);
    if (slot < 0)
        return JS_NewBool(0);
    MqjsEvent ev = { .type = EV_FOCUS };
    ev.u.focus.target = (uint8_t)slot;
    return JS_NewBool(ev_post(&ev, 0));
}

/* sys.stop(name) -> bool (slot form kept for compat). Open to every
   app (the signing gate is the trust boundary); the C invariants hold
   regardless of caller: the launcher is unstoppable, an explicitly
   stopped dev slot stays down until the next push / sys.start("dev"),
   and every stop is attributed on the console. Self-stop is deferred
   to the reaper (the context cannot be freed under its own running JS
   frame). The name form returns false for an unknown/stopped app. */
JSValue js_sys_stop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int slot;
    if (JS_IsString(ctx, argv[0])) {
        char name[32];
        if (uiw_copy_str(ctx, argv[0], name, sizeof name))
            return JS_EXCEPTION;
        slot = app_slot_by_name(name);
        if (slot < 0)
            return JS_NewBool(0);
    } else if (JS_ToInt32(ctx, &slot, argv[0]))
        return JS_EXCEPTION;
    if (slot < 0 || slot >= MQJS_MAX_WORKERS || !s_workers[slot].used)
        return slot == MQJS_WORKER_LAUNCHER
            ? JS_ThrowTypeError(ctx, "the launcher cannot be stopped")
            : JS_NewBool(0);

    MqjsWorker *app = &s_workers[slot];
    /* Phase 3: stop permission is POLICY (the launcher's KIND_SYSTEM
       profile lacks STOPPABLE), not a worker-index special case. The
       dev worker stays stoppable regardless of the record: a pushed
       task could share a protected app's name, and a name collision
       must never brick the dev flow. */
    const mqjs_app_snapshot_t *rec = mqjs_app_record_find(app->name);
    if (rec && !(rec->policy.flags & MQJS_APP_STOPPABLE) &&
        slot != MQJS_WORKER_DEV)
        return JS_ThrowTypeError(ctx, "the launcher cannot be stopped");
    char line[96];
    snprintf(line, sizeof line, "sys: stop '%s' (worker %d) by '%s'\n",
             app->name, slot, s_cur_wk ? s_cur_wk->name : "?");
    out_write(line, strlen(line));

    if (slot == MQJS_WORKER_DEV)
        /* explicit stop disarms the natural-end auto-rerun (was the
           s_dev_hold flag; the record's policy is the authority now) */
        mqjs_app_record_set_policy(app->name, 0, MQJS_APP_RESTART_ON_EXIT);
    if (app == s_cur_wk) {
        app->kill_req = true; /* reaper finishes after this dispatch */
        return JS_NewBool(1);
    }
    bool was_fg = (slot == s_fg_worker);
    app_stop_internal(app);
    if (was_fg && s_workers[MQJS_WORKER_LAUNCHER].used)
        switch_foreground(MQJS_WORKER_LAUNCHER);
    return JS_NewBool(1);
}

/* sys.notify(text): one status-bar line, prefixed with the sender, and
   recorded as the sender's latest notice (sys.notices / launcher). */
JSValue js_sys_notify(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSCStringBuf buf;
    size_t len;
    const char *text = JS_ToCStringLen(ctx, &len, argv[0], &buf);
    if (!text)
        return JS_EXCEPTION;
    const char *app = s_cur_wk ? s_cur_wk->name : "?";

    /* keep the latest notice per app: reuse the sender's entry, else
       the oldest one */
    Notice *n = NULL;
    for (int i = 0; i < (int)(sizeof s_notices / sizeof s_notices[0]); i++) {
        if (!strcmp(s_notices[i].app, app)) {
            n = &s_notices[i];
            break;
        }
        if (!n || s_notices[i].seq < n->seq)
            n = &s_notices[i];
    }
    snprintf(n->app, sizeof n->app, "%s", app);
    snprintf(n->text, sizeof n->text, "%.*s", (int)len, text);
    n->seq = ++s_notice_seq;

    if (s_notify_sink) {
        char line[128];
        snprintf(line, sizeof line, "[%s] %.*s", app, (int)len, text);
        s_notify_sink(line);
    }
    return JS_UNDEFINED;
}

/* sys.notices() -> [{app, text}] newest first (the per-app latest). */
JSValue js_sys_notices(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSGCRef arr_ref, obj_ref;
    JSValue arr = JS_NewArray(ctx, 0);
    if (JS_IsException(arr))
        return arr;
    JS_PUSH_VALUE(ctx, arr);
    int cnt = (int)(sizeof s_notices / sizeof s_notices[0]);
    int idx = 0;
    uint32_t last = 0xFFFFFFFFu;
    for (;;) {
        /* next-highest seq below `last` (n is tiny: scan per element) */
        Notice *best = NULL;
        for (int i = 0; i < cnt; i++) {
            if (s_notices[i].seq && s_notices[i].seq < last &&
                (!best || s_notices[i].seq > best->seq))
                best = &s_notices[i];
        }
        if (!best)
            break;
        last = best->seq;
        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) {
            JS_POP_VALUE(ctx, arr);
            return obj;
        }
        JS_PUSH_VALUE(ctx, obj);
        JSValue app = JS_NewString(ctx, best->app);
        JS_SetPropertyStr(ctx, obj_ref.val, "app", app);
        JSValue text = JS_NewString(ctx, best->text);
        JS_SetPropertyStr(ctx, obj_ref.val, "text", text);
        JS_POP_VALUE(ctx, obj);
        JS_SetPropertyUint32(ctx, arr_ref.val, idx++, obj);
    }
    JS_POP_VALUE(ctx, arr);
    return arr;
}

/* sys.uninstall(name) -> bool: remove /littlefs/apps/<name>.js. A
   running instance is untouched; a registry-managed app comes back on
   the next broker sync unless its retained message was tombstoned. */
JSValue js_sys_uninstall(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char name[64];
    if (uiw_copy_str(ctx, argv[0], name, sizeof name))
        return JS_EXCEPTION;
    if (!name[0])
        return JS_NewBool(0);
    char path[128];
    if (strchr(name, '/'))
        snprintf(path, sizeof path, "%.127s", name);
    else
        snprintf(path, sizeof path, "/littlefs/apps/%.96s.js", name);
#ifdef ESP_PLATFORM
    if (!strchr(name, '/'))
        autostart_optin_remove(name); /* uninstall revokes the boot opt-in */
#endif
    bool ok = remove(path) == 0;
    /* §11: drop the registry subscription too, or the retained body
       would reinstall the app on the next broker sync */
    if (ok && !strchr(name, '/') && s_uninstall_hook)
        s_uninstall_hook(name);
    return JS_NewBool(ok);
}

/* Status-bar chip / notification tap: ask the resident launcher to
   open `name` (it decides focus vs relaunch and resolves the source).
   Callable from the LVGL task; a benign race on gen just drops the
   request. */
void mqjs_request_open(const char *name)
{
    MqjsWorker *l = &s_workers[MQJS_WORKER_LAUNCHER];
    if (!name || !name[0] || !l->used)
        return;
    size_t n = strlen(name);
    if (n > 31)
        return;
    char *val = malloc(n + 32);
    if (!val)
        return;
    snprintf(val, n + 32, "{\"op\":\"open\",\"app\":\"%s\"}", name);
    MqjsEvent ev = { .type = EV_SIGNAL, .worker = MQJS_WORKER_LAUNCHER,
                     .gen = l->gen };
    ev.u.signal.value = val;
    snprintf(ev.u.signal.from, sizeof ev.u.signal.from, "system");
    if (!ev_post(&ev, 0))
        free(val);
}

static void dispatch_signal(MqjsWorker *app, MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    if (app->sig_used) {
        if (JS_StackCheck(ctx, 4)) {
            dump_error(ctx);
        } else {
            /* args are pushed in reverse: last-pushed becomes arg0 */
            JS_PushArg(ctx, JS_NewString(ctx, ev->u.signal.from)); /* arg1 */
            JS_PushArg(ctx, JS_NewString(ctx, ev->u.signal.value)); /* arg0 */
            JS_PushArg(ctx, app->sig_cb.val);                      /* func */
            JS_PushArg(ctx, JS_NULL);                              /* this */
            arm_watchdog();
            JSValue ret = JS_Call(ctx, 2);
            if (JS_IsException(ret))
                dump_error(ctx);
        }
    }
    free(ev->u.signal.value);
    ev->u.signal.value = NULL;
}

/* ------------------------------------------------------------------ */
/* ssh (wolfSSH session tasks in components/sshc; PC = print stubs).   */
/* W3 handle-style: ssh.connect() returns a session id; write/resize/  */
/* close/connected/onData/onClose take it as the first argument, so up */
/* to 3 sessions can be kept concurrently (design §7). The id -> app   */
/* owner map (recorded at connect) routes EV_SSH_* to the opening app; */
/* app_stop closes only that app's sessions. The session cap is GLOBAL */
/* (all apps together), sshc does not know about apps (§3.5).          */
/* ------------------------------------------------------------------ */

static SshCb *sshcb_find(MqjsWorker *app, int32_t id)
{
    for (int i = 0; i < MQJS_MAX_SSH_CB; i++)
        if (app->ssh_cbs[i].used && app->ssh_cbs[i].id == id)
            return &app->ssh_cbs[i];
    return NULL;
}

static SshCb *sshcb_get(MqjsWorker *app, int32_t id)
{
    SshCb *c = sshcb_find(app, id);
    if (c)
        return c;
    for (int i = 0; i < MQJS_MAX_SSH_CB; i++) {
        if (!app->ssh_cbs[i].used) {
            c = &app->ssh_cbs[i];
            memset(c, 0, sizeof *c);
            c->used = true;
            c->id = id;
            return c;
        }
    }
    return NULL;
}

static void sshcb_release(JSContext *ctx, SshCb *c)
{
    if (c->data_used)
        JS_DeleteGCRef(ctx, &c->data_fn);
    if (c->close_used)
        JS_DeleteGCRef(ctx, &c->close_fn);
    memset(c, 0, sizeof *c);
}

#ifdef ESP_PLATFORM
/* session id -> owning app (slot is enough: entries die with the app) */
typedef struct {
    bool used;
    int16_t id;
    uint8_t worker;
} SshOwner;
static SshOwner s_ssh_owner[MQJS_MAX_SSH_CB * 2];

static void ssh_owner_add(int id, int slot)
{
    for (int i = 0; i < (int)(sizeof s_ssh_owner / sizeof s_ssh_owner[0]); i++) {
        if (!s_ssh_owner[i].used) {
            s_ssh_owner[i].used = true;
            s_ssh_owner[i].id = (int16_t)id;
            s_ssh_owner[i].worker = (uint8_t)slot;
            return;
        }
    }
}

static SshOwner *ssh_owner_find(int id)
{
    for (int i = 0; i < (int)(sizeof s_ssh_owner / sizeof s_ssh_owner[0]); i++)
        if (s_ssh_owner[i].used && s_ssh_owner[i].id == id)
            return &s_ssh_owner[i];
    return NULL;
}
#endif

#ifndef ESP_PLATFORM
static int s_pc_ssh_next = 1; /* fake session ids for PC smoke runs */
#endif

/* ssh.connect(host, port, user, passwordName, hostKeyName, cols, rows)
   resolves both values inside the caller's app-scoped vault. A missing host
   key deliberately starts a TOFU probe which rejects before password auth
   and returns "hostkey:<sha256-hex>" through onClose. */
JSValue js_ssh_connect(JSContext *ctx, JSValue *this_val, int argc,
                       JSValue *argv)
{
    JSCStringBuf hbuf, ubuf, pnbuf, knbuf;
    size_t hlen, ulen;
    int port = 22, cols = 80, rows = 24;
    char host[64], user[32], pass[64], hostkey[65];

    const char *p = JS_ToCStringLen(ctx, &hlen, argv[0], &hbuf);
    if (!p)
        return JS_EXCEPTION;
    if (hlen >= sizeof host)
        hlen = sizeof host - 1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    if (JS_ToInt32(ctx, &port, argv[1]))
        return JS_EXCEPTION;
    p = JS_ToCStringLen(ctx, &ulen, argv[2], &ubuf);
    if (!p)
        return JS_EXCEPTION;
    if (ulen >= sizeof user)
        ulen = sizeof user - 1;
    memcpy(user, p, ulen);
    user[ulen] = '\0';
    const char *namep = JS_ToCString(ctx, argv[3], &pnbuf);
    if (!namep)
        return JS_EXCEPTION;
    char pass_name[MQJS_VAULT_NAME_MAX + 1];
    snprintf(pass_name, sizeof pass_name, "%s", namep);
    namep = JS_ToCString(ctx, argv[4], &knbuf);
    if (!namep)
        return JS_EXCEPTION;
    char key_name[MQJS_VAULT_NAME_MAX + 1];
    snprintf(key_name, sizeof key_name, "%s", namep);
    if (argc >= 6 && JS_ToInt32(ctx, &cols, argv[5]))
        return JS_EXCEPTION;
    if (argc >= 7 && JS_ToInt32(ctx, &rows, argv[6]))
        return JS_EXCEPTION;
    if (!vault_read(s_cur_wk->vault_id, pass_name, pass, sizeof pass))
        return JS_ThrowTypeError(ctx, "vault password not found");
    if (!vault_read(s_cur_wk->vault_id, key_name, hostkey, sizeof hostkey))
        hostkey[0] = '\0';

#ifdef ESP_PLATFORM
    if (!s_net_token) {   /* link not up: don't enter lwip (would assert) */
        memset(pass, 0, sizeof pass);
        return JS_ThrowInternalError(ctx, "network not ready");
    }
    int id = mqjs_ssh_connect(host, port, user, pass, hostkey, cols, rows);
    memset(pass, 0, sizeof pass);
    if (!id)
        return JS_ThrowTypeError(ctx, "no free ssh session (max %d)",
                                 SSHC_MAX_SESSIONS);
    ssh_owner_add(id, s_cur_wk->idx);
#else
    memset(pass, 0, sizeof pass);
    int id = s_pc_ssh_next++;
    printf("[ssh] connect(%s@%s:%d, pty %dx%d) -> #%d "
           "(stub: never connects on PC)\n", user, host, port, cols, rows, id);
#endif
    return JS_NewInt32(ctx, id);
}

JSValue js_ssh_write(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int id;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    JSCStringBuf buf;
    size_t len;
    const char *str = JS_ToCStringLen(ctx, &len, argv[1], &buf);
    if (!str)
        return JS_EXCEPTION;
#ifdef ESP_PLATFORM
    return JS_NewInt32(ctx, mqjs_ssh_write(id, str, len) ? 1 : 0);
#else
    printf("[ssh] write(#%d, %u bytes) (stub)\n", id, (unsigned)len);
    return JS_NewInt32(ctx, 1);
#endif
}

JSValue js_ssh_resize(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int id, cols, rows;
    if (JS_ToInt32(ctx, &id, argv[0]) || JS_ToInt32(ctx, &cols, argv[1]) ||
        JS_ToInt32(ctx, &rows, argv[2]))
        return JS_EXCEPTION;
#ifdef ESP_PLATFORM
    mqjs_ssh_resize(id, cols, rows);
#else
    printf("[ssh] resize(#%d, %dx%d) (stub)\n", id, cols, rows);
#endif
    return JS_UNDEFINED;
}

JSValue js_ssh_close(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int id;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
#ifdef ESP_PLATFORM
    mqjs_ssh_close(id);
#else
    printf("[ssh] close(#%d) (stub)\n", id);
#endif
    return JS_UNDEFINED;
}

JSValue js_ssh_connected(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int id;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
#ifdef ESP_PLATFORM
    return JS_NewInt32(ctx, mqjs_ssh_up(id) ? 1 : 0);
#else
    (void)id;
    return JS_NewInt32(ctx, 0);
#endif
}

/* ssh.onData(id, fn) / ssh.onClose(id, fn): per-session callbacks.
   Re-registering for the same id replaces the previous function. */
static JSValue ssh_register(JSContext *ctx, JSValue *argv, bool close_cb)
{
    int id;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "not a function");
    SshCb *c = sshcb_get(s_cur_wk, id);
    if (!c)
        return JS_ThrowInternalError(ctx, "too many ssh callbacks");
    JSGCRef *ref = close_cb ? &c->close_fn : &c->data_fn;
    bool *used = close_cb ? &c->close_used : &c->data_used;
    if (*used)
        JS_DeleteGCRef(ctx, ref);
    JSValue *pf = JS_AddGCRef(ctx, ref);
    *pf = argv[1];
    *used = true;
    return JS_UNDEFINED;
}

JSValue js_ssh_onData(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return ssh_register(ctx, argv, false);
}

JSValue js_ssh_onClose(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return ssh_register(ctx, argv, true);
}

/* Called by an sshc session task. Takes ownership of `data` (heap) on
   success; returns false when the queue stayed full (caller retries —
   terminal bytes must never be dropped, TCP provides the upstream
   backpressure). */
bool mqjs_post_ssh_data(int id, char *data, size_t len)
{
#ifdef ESP_PLATFORM
    if (!s_event_queue)
        return false;
    MqjsEvent ev = {
        .type = EV_SSH_DATA,
        .u.ssh = { .data = data, .len = (uint32_t)len, .id = (int16_t)id },
    };
    return xQueueSend(s_event_queue, &ev, pdMS_TO_TICKS(100)) == pdTRUE;
#else
    (void)id;
    (void)data;
    (void)len;
    return true;
#endif
}

void mqjs_post_ssh_closed(int id, const char *reason)
{
#ifdef ESP_PLATFORM
    if (!s_event_queue)
        return;
    MqjsEvent ev = { .type = EV_SSH_CLOSED };
    ev.u.ssh_closed.id = (int16_t)id;
    strlcpy(ev.u.ssh_closed.reason, reason ? reason : "closed",
            sizeof ev.u.ssh_closed.reason);
    xQueueSend(s_event_queue, &ev, pdMS_TO_TICKS(500));
#else
    (void)id;
    (void)reason;
#endif
}

static size_t ssh_utf8_complete_prefix(const char *buf, size_t len)
{
    if (!len)
        return 0;
    size_t start = len;
    while (start > 0 && len - start < 3 &&
           (((uint8_t)buf[start - 1] & 0xC0) == 0x80))
        start--;
    if (start < len && start > 0)
        start--; /* move from the first continuation byte to its lead */
    else if (start == len)
        start--; /* final byte may itself be a lead */

    uint8_t lead = (uint8_t)buf[start];
    size_t need = (lead & 0xE0) == 0xC0 ? 2 :
                  (lead & 0xF0) == 0xE0 ? 3 :
                  (lead & 0xF8) == 0xF0 ? 4 : 1;
    return len - start < need ? start : len;
}

static void dispatch_ssh_data(MqjsWorker *app, MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    SshCb *c = sshcb_find(app, ev->u.ssh.id);
    if (c && c->data_used) {
        size_t n = c->utf8_tail_len + ev->u.ssh.len;
        char *buf = malloc(n);
        if (!buf)
            goto done;
        memcpy(buf, c->utf8_tail, c->utf8_tail_len);
        memcpy(buf + c->utf8_tail_len, ev->u.ssh.data, ev->u.ssh.len);
        c->utf8_tail_len = 0;

        /* SSH stream reads may split a UTF-8 codepoint. JS_NewStringLen
           decodes each call independently, so retain an incomplete suffix
           and deliver it with the next event instead of corrupting it. */
        size_t cut = ssh_utf8_complete_prefix(buf, n);
        if (cut < n) {
            c->utf8_tail_len = (uint8_t)(n - cut);
            memcpy(c->utf8_tail, buf + cut, c->utf8_tail_len);
        }

        if (cut > 0) {
            if (JS_StackCheck(ctx, 3)) {
                dump_error(ctx);
            } else {
                JS_PushArg(ctx, JS_NewStringLen(ctx, buf, cut));  /* arg0 */
                JS_PushArg(ctx, c->data_fn.val);                  /* func */
                JS_PushArg(ctx, JS_NULL);                         /* this */
                arm_watchdog();
                JSValue ret = JS_Call(ctx, 1);
                if (JS_IsException(ret))
                    dump_error(ctx);
            }
        }
        free(buf);
    }
done:
    free(ev->u.ssh.data);
    ev->u.ssh.data = NULL;
}

static void dispatch_ssh_closed(MqjsWorker *app, const MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
#ifdef ESP_PLATFORM
    SshOwner *o = ssh_owner_find(ev->u.ssh_closed.id);
    if (o)
        o->used = false; /* session gone: stop counting it as pending */
#endif
    SshCb *c = sshcb_find(app, ev->u.ssh_closed.id);
    if (!c)
        return;
    if (c->close_used) {
        if (JS_StackCheck(ctx, 3)) {
            dump_error(ctx);
        } else {
            JS_PushArg(ctx, JS_NewString(ctx, ev->u.ssh_closed.reason));
            JS_PushArg(ctx, c->close_fn.val);
            JS_PushArg(ctx, JS_NULL);
            arm_watchdog();
            JSValue ret = JS_Call(ctx, 1);
            if (JS_IsException(ret))
                dump_error(ctx);
        }
    }
    /* the session is gone: release both callbacks in one sweep */
    sshcb_release(ctx, c);
}

/* ------------------------------------------------------------------ */
/* mqtt (esp-mqtt; PC build = print-only stubs). Per-app client with   */
/* client_id "mqjs-app-<slot>" so apps cannot kick each other off the  */
/* broker (the id-collision lesson of 2026-06-11 applied per app).     */
/* ------------------------------------------------------------------ */

/* MQTT topic filter match incl. '+' and '#' wildcards */
static bool mqtt_topic_match(const char *filter, const char *topic)
{
    while (*filter && *topic) {
        if (*filter == '+') {
            filter++;
            while (*topic && *topic != '/')
                topic++;
        } else if (*filter == '#') {
            return true;
        } else {
            if (*filter != *topic)
                return false;
            filter++;
            topic++;
        }
    }
    if (*filter == '\0' && *topic == '\0')
        return true;
    /* "a/#" also matches "a"; lone trailing '+' matches the empty level */
    if (filter[0] == '/' && filter[1] == '#' && filter[2] == '\0')
        return *topic == '\0';
    if (filter[0] == '#' && filter[1] == '\0')
        return true;
    if (filter[0] == '+' && filter[1] == '\0')
        return *topic == '\0';
    return false;
}

#ifdef ESP_PLATFORM
/* runs in the esp-mqtt task: copy + enqueue only, never touch JS. The
   handler argument carries the owning slot + generation (§3.2). */
#define MQTT_ARG_PACK(slot, gen) ((void *)(uintptr_t)((slot) | ((gen) << 8)))

static void mqtt_event_cb(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t e = event_data;
    uintptr_t packed = (uintptr_t)arg;
    uint8_t slot = (uint8_t)(packed & 0xFF);
    uint16_t gen = (uint16_t)(packed >> 8);
    MqjsEvent ev = { .worker = slot, .gen = gen };

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_workers[slot].mqtt_up = true;
        ev.type = EV_MQTT_CONNECTED;
        xQueueSend(s_event_queue, &ev, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_workers[slot].mqtt_up = false;   /* esp-mqtt auto-reconnects */
        break;
    case MQTT_EVENT_DATA: {
        /* fragmented payloads (> internal rx buffer) are not supported */
        if (e->current_data_offset != 0 || e->data_len != e->total_data_len)
            break;
        if (e->topic_len == 0 || e->data_len > MQJS_MQTT_PAYLOAD_MAX)
            break;
        char *topic = malloc((size_t)e->topic_len + 1);
        char *payload = malloc((size_t)e->data_len + 1);
        if (!topic || !payload) {
            free(topic);
            free(payload);
            break;
        }
        memcpy(topic, e->topic, e->topic_len);
        topic[e->topic_len] = '\0';
        memcpy(payload, e->data, e->data_len);
        payload[e->data_len] = '\0';
        ev.type = EV_MQTT_DATA;
        ev.u.mqtt.topic = topic;
        ev.u.mqtt.payload = payload;
        ev.u.mqtt.len = (uint32_t)e->data_len;
        if (xQueueSend(s_event_queue, &ev, 0) != pdTRUE) {
            free(topic);
            free(payload);
        }
        break;
    }
    default:
        break;
    }
}
#endif

/* mqtt.connect(token[, uri]): the token comes from net.onReady's callback and
   is the capability proving the link is up — there is no way to obtain a valid
   token without the event, so a top-level connect cannot be written, and a
   stale/absent token is rejected here before lwip (no "Invalid mbox" assert).
   uri is optional: omit it to use the platform-configured broker, so apps do
   not hardcode "mqtt://...". */
JSValue js_mqtt_connect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int token = 0;   /* JS_ToInt32 wants int* (not int32_t*) on riscv32 */
    if (argc < 1 || JS_ToInt32(ctx, &token, argv[0]))
        return JS_ThrowTypeError(ctx,
            "mqtt.connect(token[, uri]): token comes from net.onReady(token => ...)");
    if (!s_net_token || (uint32_t)token != s_net_token)
        return JS_ThrowInternalError(ctx,
            "network not ready: connect inside net.onReady (stale/no token)");

    JSCStringBuf buf;
    size_t len;
    const char *uri;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        uri = JS_ToCStringLen(ctx, &len, argv[1], &buf);  /* explicit override */
        if (!uri)
            return JS_EXCEPTION;
    } else if (s_default_broker) {
        uri = s_default_broker;                           /* platform default */
    } else {
        return JS_ThrowTypeError(ctx, "no broker configured (pass a uri)");
    }

#ifdef ESP_PLATFORM
    MqjsWorker *app = s_cur_wk;
    if (app->mqtt)
        return JS_ThrowTypeError(ctx, "mqtt already started");
    /* distinct client id per app AND distinct from the task-delivery
       client (task_source.c): same-id clients kick each other off the
       broker (found the hard way, 2026-06-11) */
    char client_id[16];
    snprintf(client_id, sizeof client_id, "mqjs-app-%d", app->idx);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,   /* copied by esp_mqtt_client_init */
        .credentials.client_id = client_id,
    };
    app->mqtt = esp_mqtt_client_init(&cfg);
    if (!app->mqtt)
        return JS_ThrowInternalError(ctx, "mqtt init failed (bad uri?)");
    esp_mqtt_client_register_event(app->mqtt, ESP_EVENT_ANY_ID, mqtt_event_cb,
                                   MQTT_ARG_PACK(app->idx, app->gen));
    if (esp_mqtt_client_start(app->mqtt) != ESP_OK) {
        esp_mqtt_client_destroy(app->mqtt);
        app->mqtt = NULL;
        return JS_ThrowInternalError(ctx, "mqtt start failed");
    }
#else
    printf("[mqtt] connect(%s) (stub)\n", uri);
#endif
    return JS_UNDEFINED;
}

JSValue js_mqtt_disconnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    MqjsWorker *app = s_cur_wk;
    if (app->mqtt) {
        esp_mqtt_client_stop(app->mqtt);
        esp_mqtt_client_destroy(app->mqtt);
        app->mqtt = NULL;
        app->mqtt_up = false;
    }
#else
    printf("[mqtt] disconnect (stub)\n");
#endif
    return JS_UNDEFINED;
}

JSValue js_mqtt_connected(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    return JS_NewInt32(ctx, s_cur_wk->mqtt_up ? 1 : 0);
#else
    return JS_NewInt32(ctx, 0);
#endif
}

JSValue js_mqtt_onConnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    return register_cb(ctx, argv[0], &s_cur_wk->mqtt_onconn_used,
                       &s_cur_wk->mqtt_onconn);
}

/* net.onReady(cb): park the app in the network wait queue. cb is invoked as
   cb(token) once the link is up; if it is already up, it runs on the next loop
   turn (so the edge is never missed). The token must be handed to mqtt.connect
   — it is the only way to obtain one, so a connect outside this callback (e.g.
   at top level, before the link) cannot be written. One-shot: re-arm inside cb
   to wait for the next bring-up. Event-driven, not polling: the app yields and
   is woken by got-IP. */
JSValue js_net_onReady(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSValue r = register_cb(ctx, argv[0], &s_cur_wk->net_used,
                            &s_cur_wk->net_cb);
    if (JS_IsException(r))
        return r;
    if (s_net_token) {         /* already up: fire (with the token) next turn */
        MqjsEvent ev = { .type = EV_NET };
        ev_post(&ev, 0);
    }
    return JS_UNDEFINED;
}

/* net.topic(name): prepend the platform topic namespace, so apps publish/
   subscribe with just a leaf ("clipboard") and never hardcode the prefix.
   With no prefix configured (PC/host) the name passes through unchanged. */
JSValue js_net_topic(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSCStringBuf buf;
    size_t len;
    const char *name = JS_ToCStringLen(ctx, &len, argv[0], &buf);
    if (!name)
        return JS_EXCEPTION;
    char topic[160];
    if (s_topic_prefix && s_topic_prefix[0])
        snprintf(topic, sizeof topic, "%s/%s", s_topic_prefix, name);
    else
        snprintf(topic, sizeof topic, "%s", name);
    return JS_NewStringLen(ctx, topic, strlen(topic));
}

JSValue js_mqtt_publish(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSCStringBuf tbuf, pbuf;
    size_t tlen, plen;
    int qos = 0, retain = 0;

    const char *topic = JS_ToCStringLen(ctx, &tlen, argv[0], &tbuf);
    if (!topic)
        return JS_EXCEPTION;
    const char *payload = JS_ToCStringLen(ctx, &plen, argv[1], &pbuf);
    if (!payload)
        return JS_EXCEPTION;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToInt32(ctx, &qos, argv[2]))
        return JS_EXCEPTION;
    if (argc >= 4 && !JS_IsUndefined(argv[3]) && JS_ToInt32(ctx, &retain, argv[3]))
        return JS_EXCEPTION;

#ifdef ESP_PLATFORM
    if (!s_cur_wk->mqtt)
        return JS_ThrowTypeError(ctx, "mqtt not connected");
    int id = esp_mqtt_client_publish(s_cur_wk->mqtt, topic, payload,
                                     (int)plen, qos, retain);
    return JS_NewInt32(ctx, id);
#else
    printf("[mqtt] publish(%s, %s, qos=%d, retain=%d) (stub)\n",
           topic, payload, qos, retain);
    return JS_NewInt32(ctx, 0);
#endif
}

JSValue js_mqtt_subscribe(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSCStringBuf tbuf;
    size_t tlen;
    const char *topic = JS_ToCStringLen(ctx, &tlen, argv[0], &tbuf);
    if (!topic)
        return JS_EXCEPTION;
    if (tlen == 0 || tlen >= MQJS_MQTT_TOPIC_MAX)
        return JS_ThrowTypeError(ctx, "bad topic length");
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "not a function");

    for (int i = 0; i < MQJS_MAX_MQTT_SUB; i++) {
        MqttSub *s = &s_cur_wk->mqtt_subs[i];
        if (s->used && !strcmp(s->topic, topic))
            return JS_ThrowTypeError(ctx, "topic already subscribed");
    }
    for (int i = 0; i < MQJS_MAX_MQTT_SUB; i++) {
        MqttSub *s = &s_cur_wk->mqtt_subs[i];
        if (s->used)
            continue;
        memcpy(s->topic, topic, tlen + 1);
        JSValue *pf = JS_AddGCRef(ctx, &s->fn);
        *pf = argv[1];
        s->used = true;
#ifdef ESP_PLATFORM
        if (s_cur_wk->mqtt && s_cur_wk->mqtt_up)
            esp_mqtt_client_subscribe(s_cur_wk->mqtt, s->topic, 0);
        /* not connected yet: dispatch_mqtt_connected() subscribes later */
#else
        printf("[mqtt] subscribe(%s) registered (stub: never fires on PC)\n",
               s->topic);
#endif
        return JS_UNDEFINED;
    }
    return JS_ThrowInternalError(ctx, "too many mqtt subscriptions");
}

static void dispatch_mqtt_connected(MqjsWorker *app)
{
    JSContext *ctx = app->ctx;
#ifdef ESP_PLATFORM
    /* (re)subscribe everything on each broker session; duplicate
       SUBSCRIBE packets are just a refresh for the broker */
    for (int i = 0; i < MQJS_MAX_MQTT_SUB; i++) {
        if (app->mqtt_subs[i].used && app->mqtt)
            esp_mqtt_client_subscribe(app->mqtt, app->mqtt_subs[i].topic, 0);
    }
#endif
    if (!app->mqtt_onconn_used)
        return;
    if (JS_StackCheck(ctx, 2)) {
        dump_error(ctx);
        return;
    }
    JS_PushArg(ctx, app->mqtt_onconn.val);  /* func */
    JS_PushArg(ctx, JS_NULL);               /* this */
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 0);
    if (JS_IsException(ret))
        dump_error(ctx);
}

static void dispatch_mqtt_data(MqjsWorker *app, MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    for (int i = 0; i < MQJS_MAX_MQTT_SUB; i++) {
        MqttSub *s = &app->mqtt_subs[i];
        if (!s->used || !mqtt_topic_match(s->topic, ev->u.mqtt.topic))
            continue;
        if (JS_StackCheck(ctx, 4)) {
            dump_error(ctx);
            break;
        }
        /* args are pushed in reverse: the last-pushed one becomes arg0 */
        JS_PushArg(ctx, JS_NewStringLen(ctx, ev->u.mqtt.payload,
                                        ev->u.mqtt.len));                /* arg1 */
        JS_PushArg(ctx, JS_NewString(ctx, ev->u.mqtt.topic));            /* arg0 */
        JS_PushArg(ctx, s->fn.val);                                      /* func */
        JS_PushArg(ctx, JS_NULL);                                        /* this */
        arm_watchdog();
        JSValue ret = JS_Call(ctx, 2);
        if (JS_IsException(ret))
            dump_error(ctx);
        /* no break: several filters may match one topic */
    }
    free(ev->u.mqtt.topic);
    free(ev->u.mqtt.payload);
    ev->u.mqtt.topic = NULL;
    ev->u.mqtt.payload = NULL;
}

/* ------------------------------------------------------------------ */
/* Dictionary images (docs/skk-ime-design.md)                           */
/*                                                                      */
/* THERE IS NO LONGER A JS-FACING skk.* API. Apps ask for Japanese      */
/* input with ui.ime(1) and receive committed text through ui.onKey;    */
/* everything between those two lives in ime_core behind the platform   */
/* session (see "platform IME session" below). What survives here is    */
/* what that session needs: loading and refcounting the dictionary      */
/* image. Nothing else — the learning store was deleted on 2026-07-30   */
/* (docs/skk-ime-design.md §S7), and with it the only piece of this     */
/* subsystem that had two touching tasks and a file to write.           */
/*                                                                      */
/* The handle API (skk.open/key/preedit/candidates/... , a 4-slot table */
/* with generation-tagged ids) was deleted on 2026-07-30 once its last  */
/* caller moved to the platform IME. Its whole reason for existing —    */
/* letting an app drive the engine directly — is the thing that made    */
/* the 「あ」 key's face lie (§6.2), so this is a removal, not a pause. */
/* ------------------------------------------------------------------ */

#define MQJS_MAX_SKK_DICT  2   /* distinct dictionary images resident */
#define MQJS_SKK_PATH_MAX  96
#define MQJS_SKK_IMAGE_MAX (9u * 1024 * 1024) /* pine (L) is 8.3 MB */

#ifdef ESP_PLATFORM
#define MQJS_SKK_DIR          "/littlefs/skk"
#else
#define MQJS_SKK_DIR          "skk"
#endif
#define MQJS_SKK_DEFAULT_DICT MQJS_SKK_DIR "/skk_dict_M.bin"

/* Where a dictionary can come from. The string is the cache key, so each
 * source names exactly one set of bytes:
 *
 *   ""              the image linked into the firmware (plum, §6.6)
 *   "part:<name>"   a flash partition, mmap'd and read in place —
 *                   bamboo and pine (§6.7)
 *   anything else   a file, read into PSRAM
 *
 * They are tried in that order, best first, so flashing a bigger
 * dictionary into `jisyo` upgrades every app without an app change and
 * removing it falls back instead of failing. Nothing selects a source by
 * hand any more — the argument existed for skk.open(), which is gone. */
#define MQJS_SKK_PART_PREFIX "part:"
#define MQJS_SKK_PARTITION   "jisyo"
/* The custom data subtype partitions.csv gives `jisyo`. No built-in
   subtype means "a blob we mmap", and 0x40 is the first of the range
   reserved for applications. */
#define MQJS_SKK_PART_SUBTYPE 0x40

/* One loaded image, shared by every handle that named the same path:
   skk_dict_t is explicitly read-only and shareable, and two apps each
   holding their own 290 KB copy of SKK-JISYO.M would be a waste that
   grows to 8 MB at the pine stage. Refcounted, freed at zero. */
typedef struct {
    bool       used;
    bool       owned;     /* malloc'd; rodata and mmap are not */
    uint16_t   refs;
    char       path[MQJS_SKK_PATH_MAX]; /* "" = the built-in image */
    const uint8_t *base;  /* 8-byte aligned image */
    size_t     len;
    skk_dict_t dict;
    uint32_t   load_us;   /* read + open, for ui.imeStats() */
#ifdef ESP_PLATFORM
    esp_partition_mmap_handle_t mmap_h;
    bool       mapped;    /* base is an mmap window, munmap it at zero */
#endif
} SkkImage;
static SkkImage s_skk_img[MQJS_MAX_SKK_DICT];

/* The image must be 8-byte aligned: skk_dict_open() reads the packed-key
   arrays as uint64 directly and returns SKK_ERR_ALIGN rather than risk
   an unaligned trap. */
static uint8_t *skk_image_alloc(size_t len)
{
    len = (len + 7u) & ~(size_t)7; /* aligned_alloc() wants a multiple of the
                                      alignment; the extra bytes are unread —
                                      the header's image_len is authoritative */
#ifdef ESP_PLATFORM
    /* PSRAM only, deliberately: internal SRAM's largest contiguous block
       is 34-43 KB and lwIP/SDIO want it (design §4.3). Failing here is
       better than starving WiFi or SSH. */
    return heap_caps_aligned_alloc(8, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(len);
#endif
}

static void skk_image_free(uint8_t *p)
{
#ifdef ESP_PLATFORM
    heap_caps_free(p);
#else
    free(p);
#endif
}

#ifdef ESP_PLATFORM
/* Attach a dictionary partition without reading it.
 *
 * This chip maps flash through the MMU, so bamboo and pine cost exactly
 * what plum costs: no allocation, no load, no copy. skk_dict_t just
 * points at the window (design §6.6/§6.7) and the search reads the
 * mmap'd bytes in place, the same way font_term_mono's 772 KB of glyphs
 * are read on the draw path.
 *
 * Mapped in TWO steps, and the second one matters: `jisyo` is sized for
 * pine (8.5 MB) but bamboo only fills 1.86 MB of it. The header says how
 * long the image really is, so map a page, read image_len, and map only
 * that — otherwise 6.6 MB of erased flash would sit in the MMU window,
 * which this SoC shares between PSRAM, the instruction cache and every
 * other mapping.
 *
 * The image base lands on a partition boundary, so the 8-byte alignment
 * skk_dict_open() requires comes for free — but it is still checked
 * there rather than assumed here. */
static int skkpart_open(const char *name, SkkImage *im)
{
    const esp_partition_t *p;
    esp_partition_mmap_handle_t h;
    const void *win = NULL;
    uint32_t magic, image_len;
    size_t probe;
    int rc;

    p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 (esp_partition_subtype_t)MQJS_SKK_PART_SUBTYPE,
                                 name);
    if (!p)
        return -103;                      /* no such partition in the table */

    probe = p->size < 4096 ? p->size : 4096;
    if (probe < sizeof(skk_image_hdr_t) ||
        esp_partition_mmap(p, 0, probe, ESP_PARTITION_MMAP_DATA, &win, &h) != ESP_OK)
        return -100;
    magic     = ((const uint32_t *)win)[0];
    image_len = ((const uint32_t *)win)[2];   /* skk_image_hdr_t.image_len */
    esp_partition_munmap(h);

    /* An erased partition reads as 0xFF everywhere, which is the normal
       state of a device that has never been given a dictionary. Say so
       distinctly instead of reporting a corrupt image. */
    if (magic == 0xFFFFFFFFu)
        return -104;
    if (magic != SKK_IMAGE_MAGIC)
        return SKK_ERR_MAGIC;
    if (image_len < sizeof(skk_image_hdr_t) || image_len > p->size)
        return SKK_ERR_TRUNCATED;

    if (esp_partition_mmap(p, 0, image_len, ESP_PARTITION_MMAP_DATA, &win, &h) != ESP_OK)
        return -100;

    /* VERIFY, unlike the built-in image: this one lives OUTSIDE the app
       image, so the bootloader's per-boot SHA-256 never saw it and a
       half-written flash would show up as wrong conversions rather than
       as a failure (design §6.10). ~190 ms for bamboo, paid once. */
    skk_blob_t blob = { (const uint8_t *)win, image_len };
    rc = skk_dict_open(&im->dict, blob, SKK_OPEN_VERIFY);
    if (rc != SKK_OK) {
        esp_partition_munmap(h);
        return rc;
    }
    im->base   = (const uint8_t *)win;
    im->len    = image_len;
    im->mmap_h = h;
    im->mapped = true;
    im->owned  = false;
    return SKK_OK;
}
#endif /* ESP_PLATFORM */

/* Load `path` (or take another reference to it) and return its index in
   s_skk_img, or -1 with *out_err set to an skk_err_t / -100 for I/O. */
static int skkimg_acquire(const char *path, int *out_err)
{
    int free_slot = -1;

    *out_err = SKK_ERR_ARG;
    for (int i = 0; i < MQJS_MAX_SKK_DICT; i++) {
        if (s_skk_img[i].used) {
            if (strcmp(s_skk_img[i].path, path) == 0) {
                s_skk_img[i].refs++;
                return i;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        *out_err = SKK_ERR_NOSPACE;
        return -1;
    }

    int64_t t0 = time_us();

    /* "" = the image linked into the firmware (docs/skk-ime-design.md
       §6.6). Read in place out of flash rodata: no file, no allocation,
       no copy, and no CRC pass — the bootloader already validates the
       whole app image with SHA-256 on every boot, so verifying here
       would redo that with a weaker checksum for 20-30 ms (§6.10). */
    if (!path[0]) {
        skk_blob_t blob = skk_builtin_image();
        if (!blob.base) {
            *out_err = -102; /* built without a dictionary image */
            return -1;
        }
        SkkImage *bi = &s_skk_img[free_slot];
        int rc = skk_dict_open(&bi->dict, blob, 0);
        if (rc != SKK_OK) {
            *out_err = rc;
            return -1;
        }
        bi->used = true;
        bi->owned = false;
        bi->refs = 1;
        bi->base = blob.base;
        bi->len = blob.len;
        bi->load_us = (uint32_t)(time_us() - t0);
        bi->path[0] = '\0';
        *out_err = SKK_OK;
        return free_slot;
    }

    /* "part:<name>" — mmap'd flash, the bamboo/pine stages. Also
       allocation-free and load-free; see skkpart_open(). */
    if (strncmp(path, MQJS_SKK_PART_PREFIX, sizeof(MQJS_SKK_PART_PREFIX) - 1) == 0) {
#ifdef ESP_PLATFORM
        SkkImage *pi = &s_skk_img[free_slot];
        int rc = skkpart_open(path + sizeof(MQJS_SKK_PART_PREFIX) - 1, pi);
        if (rc != SKK_OK) {
            memset(pi, 0, sizeof *pi);   /* skkpart_open may have half-filled it */
            *out_err = rc;
            return -1;
        }
        pi->used = true;
        pi->refs = 1;
        pi->load_us = (uint32_t)(time_us() - t0);
        snprintf(pi->path, sizeof pi->path, "%s", path);
        *out_err = SKK_OK;
        return free_slot;
#else
        *out_err = -103;                 /* no partitions off-device */
        return -1;
#endif
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        *out_err = -100;
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        *out_err = -100;
        return -1;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz < (long)sizeof(skk_image_hdr_t) || (unsigned long)sz > MQJS_SKK_IMAGE_MAX) {
        fclose(f);
        *out_err = SKK_ERR_TRUNCATED;
        return -1;
    }
    uint8_t *base = skk_image_alloc((size_t)sz);
    if (!base) {
        fclose(f);
        *out_err = -101;
        return -1;
    }
    size_t got = fread(base, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        skk_image_free(base);
        *out_err = SKK_ERR_TRUNCATED;
        return -1;
    }

    SkkImage *im = &s_skk_img[free_slot];
    skk_blob_t blob = { base, (size_t)sz };
    /* VERIFY here but NOT for the built-in image: a file on littlefs or
       a partition is written by us and nobody checks it afterwards, so a
       partial write is a real and silent failure mode (a corrupt index
       does not crash, it answers wrong). The CRC pass costs 20-30 ms,
       paid once per open. */
    int rc = skk_dict_open(&im->dict, blob, SKK_OPEN_VERIFY);
    if (rc != SKK_OK) {
        skk_image_free(base);
        *out_err = rc;
        return -1;
    }
    im->used = true;
    im->owned = true;
    im->refs = 1;
    im->base = base;
    im->len = (size_t)sz;
    im->load_us = (uint32_t)(time_us() - t0);
    snprintf(im->path, sizeof im->path, "%s", path);
    *out_err = SKK_OK;
    return free_slot;
}

/* 対になる skkimg_release() はもう居ません。手放す相手が消えたからです:
   イメージを持つのはプラットフォームのセッション 1 つだけで、それは端末の
   寿命そのもの (アプリが止まっても畳むだけ、辞書は載せたまま — 次に日本語を
   打つアプリが 200 ms の CRC を払い直さずに済む)。skk.* のハンドルが有った
   頃は close ごとに refs を落として munmap / free まで走らせていましたが、
   その経路は 2026-07-30 に到達不能になったので、動かないコードとして
   残さずに消しました (戻すときは git から。refs はここで 1 のまま増えます)。 */

/* 辞書ソースの探索順を 1 か所に閉じ込める。`path` が空なら
   partition → 内蔵 (無ければファイル) の best-first。開けた s_skk_img の
   index、失敗なら -1。
   `*out_err` は最初の候補の失敗理由、`*out_first` はその名前 — 最後の
   候補で答えると、消えた `jisyo` パーティションの話なのに
   skk_dict_M.bin を指してしまい、探す場所を間違えさせる。
   順序そのものが「大きい辞書を焼けば全アプリが賢くなる」(設計 §6.6) の
   実装なので、ここを触るときはそれを壊していないか見ること。 */
static int skkimg_acquire_best(const char *path, int *out_err,
                               const char **out_first)
{
    const char *tried[3];
    int ntried = 0;

    if (path && path[0]) {
        tried[ntried++] = path;
    } else {
        tried[ntried++] = MQJS_SKK_PART_PREFIX MQJS_SKK_PARTITION;
        if (skk_builtin_image().base)
            tried[ntried++] = "";                    /* plum, in rodata */
        else
            tried[ntried++] = MQJS_SKK_DEFAULT_DICT; /* nothing built in */
    }
    *out_first = tried[0];

    int err = 0, first_err = 0, img = -1;
    for (int i = 0; i < ntried && img < 0; i++) {
        img = skkimg_acquire(tried[i], &err);
        if (i == 0)
            first_err = err;
    }
    *out_err = first_err;
    return img;
}

/* ---- platform IME session (docs/keyboard-ime-unification.md §7) ------
 *
 * ONE ime_t for the device, not one per app. 辞書は既に device 単位で
 * 共有されていて (skkimg_acquire)、セッションだけ
 * 増やしても「どこにも描かれない 2 つ目の preedit」ができるだけ。誰の
 * 打鍵を通すかは MqjsWorker.ime_used (ui.ime(1) の opt-in) が決めるので、
 * ゲームのキーが黙って食われることはない。
 *
 * かつて隣に skk.* のハンドル API があり、アプリがエンジンを直接叩けた。
 * 2026-07-30 に削除 — 最後の利用者 (skk_test.js) がここへ移ったのと、
 * 直叩きこそが「あ」キーの面を嘘にしていた当のものだったため (§6.2)。
 *
 * 所有タスク: s_ime に触ってよいのは「所有タスク」1 つだけ、例外なし。
 * ime_t は単一タスク所有が前提 (ime_core.h) なのに、打鍵の入口
 * mqjs_post_key() は LVGL タスク (オンスクリーン kbd と自動回転の
 * "\0rotate") とドックの kbd タスクの両方から呼ばれ、ui.ime() は mqjs
 * タスクで走る — ドックで打ちながら回転するのは普通に起きる。排他を足す
 * のではなく、tailscale_adapter の lifecycle キューと同じ形 (単一所有者 +
 * どの文脈からも post するだけのコマンドキュー) に寄せた。あちらが状態を
 * 再帰ミューテックスでも守っているのは公開 API から直接読まれるからで、
 * s_ime は所有タスクの外から一切見えないので鍵は要らない。 */
static ime_t s_ime;
static bool  s_ime_init;
static int   s_ime_img = -1;

#ifdef ESP_PLATFORM
/* widget の field がフォーカスを持っている間の入力方針 (I3)。
   -1 = field 非フォーカス (ui.ime() の opt-in がそのまま効く)、
    0 = ascii / password の field — アプリが ui.ime(1) 済みでも IME は通さない、
    1 = ja の field。
   書くのは LVGL タスク (フォーカスのイベント)、読むのは打鍵の来たタスク。
   int なので破れず、古い値を読んでも打鍵 1 つぶん行き先がずれるだけ。
   アンカーは field の矩形: widget アプリは preedit の存在を知らないので
   ui.caret を呼ばず、C が持たないとフロートが画面の隅に出る。 */
static volatile int s_ime_field = -1;
static volatile int16_t s_ime_field_x, s_ime_field_y, s_ime_field_h;
#endif

/* --- ここから下は所有タスクの上でしか呼ばない (ESP では IME タスク、
   ホストには入力タスクが無いので mqjs タスクがそのまま所有者)。
   IME に属する可変状態はもう s_ime だけで、それに触るタスクは 1 つ。
   だからこの層に鍵は 1 つも無い (学習ストアを消した 2026-07-30 まで、
   他タスクから読まれる唯一の構造体がそれだった)。 --- */

/* セッションを使える状態にする。辞書は初回だけ開く。
   失敗を恒久ラッチしないのが肝で (§4-4)、skk_dict.bin は gitignore 対象
   = クリーンビルドのファームには本当に辞書が無く、後から焼かれる。
   ラッチすると焼いた後もアプリ再起動まで直らない。 */
static bool ime_arm_now(void)
{
    if (!s_ime_init) {
        ime_init(&s_ime);
        s_ime_init = true;
    }
    if (s_ime_img < 0) {
        int err = 0;
        const char *first = "";
        int img = skkimg_acquire_best("", &err, &first);
        if (img < 0) {
            /* 辞書が無いことを黙って起こさない。旧 skk.open() は JS 例外で
               「どこに何を焼けばいいか」まで名指ししていたが、その口は消えた。
               ui.ime(1) が返す false だけでは、アプリ側は「この機種は日本語が
               打てない」としか分からず、辞書を焼き忘れただけの実機と区別が
               付かない。ログを埋めないよう 1 起動 1 回。 */
            static bool warned;
            if (!warned) {
                warned = true;
#ifdef ESP_PLATFORM
                ESP_LOGW(TAG, "IME: no dictionary (%s, err %d) — flash one into "
                              "the `jisyo` partition (README 3.5) or pick one in "
                              "menuconfig", first[0] ? first : "(built-in)", err);
#else
                printf("[ime] no dictionary (%s, err %d)\n",
                       first[0] ? first : "(built-in)", err);
#endif
            }
            return false;
        }
        s_ime_img = img;
        ime_attach(&s_ime, &s_skk_img[img].dict);
    }
    return true;
}

/* IME を降りるときの後始末。読みかけは捨てる (確定させない — 誤操作で
   リモートのシェルへ文字列を押し込むより、数文字打ち直す方がまし)。
   メモリの中だけで完結する: この境界に書き戻すものはもう無い。 */
static void ime_fold_now(void)
{
    if (!s_ime_init)
        return;
    ime_set_on(&s_ime, false);
    ime_reset(&s_ime);
    ime_view_clear(&s_ime);
}

/* ---- 実測 (ui.imeStats) ---------------------------------------------
 *
 * 計測は IME と一緒に C へ来た。打鍵はもう JS を跨がないので、アプリ側で
 * 時間を挟む手段が無い — skk_test.js が skk.key() を挟んで測っていた数字は、
 * この移行で「誰も通らない道の値」になった。
 *
 * ここで測るのは**出荷する道**そのもの: 入力面が post してから所有タスクが
 * 拾うまで (hop) と、ime_feed の中身 (辞書を引いた打鍵とそうでない打鍵で
 * 別のバケツ)。hop はこの構造にしか存在せず、「打鍵が重いか」に答えるのは
 * 実はこちら — エンジンが 77µs でも、キューで 30ms 待たされていれば遅い。
 *
 * 触ってよいのは所有タスクだけ (s_ime と同じ規律)。唯一の例外が
 * s_ime_drops で、キューに入らなかった打鍵は poster 側でしか数えられない。 */
typedef struct {
    uint32_t key_calls, key_us, key_max_us;    /* 辞書を引かなかった打鍵 */
    uint32_t conv_calls, conv_us, conv_max_us; /* 引いた打鍵 */
    uint32_t hop_calls, hop_us, hop_max_us;    /* post → 所有タスクが拾う */
    uint32_t last_lookups;   /* 上の振り分けに使う前回値 */
} ImeMeter;
static ImeMeter s_ime_m;
/* 数えるのは打鍵を投げる側 (LVGL / ドックの kbd)。2 つのタスクの非アトミック
   な ++ なので稀に 1 数え落とすが、意味があるのは「0 か 0 でないか」。 */
static volatile uint32_t s_ime_drops;

/* 所有タスクが撮るスナップショット。カウンタを外のタスクから直接読むと
   「半分だけ新しい」姿が見えるうえ、skk_stats() は構造体まるごとの複製で、
   所有タスクが書いている最中に重なりうる。 */
typedef struct {
    skk_stats_t eng;
    ImeMeter    m;
    uint32_t    drops;
    uint32_t    img_len, img_load_us, nasi, ari, levels;
    const char *dict;   /* s_skk_img[].path (静的配列) を指す */
    bool        ready, on;
} ImeSnap;
static ImeSnap s_ime_snap;

static bool ime_snap_now(void)
{
    if (!s_ime_init || s_ime_img < 0)
        return false;      /* 一度も arm されていない = 語る数字が無い */
    skk_stats(&s_ime.skk, &s_ime_snap.eng);
    s_ime_snap.m = s_ime_m;
    s_ime_snap.drops = s_ime_drops;
    const SkkImage *im = &s_skk_img[s_ime_img];
    s_ime_snap.img_len = (uint32_t)im->len;
    s_ime_snap.img_load_us = im->load_us;
    s_ime_snap.nasi = (uint32_t)im->dict.blk[SKK_BLK_NASI].count;
    s_ime_snap.ari = (uint32_t)im->dict.blk[SKK_BLK_ARI].count;
    s_ime_snap.levels = (uint32_t)im->dict.blk[SKK_BLK_NASI].levels;
    s_ime_snap.dict = im->path[0] ? im->path : "builtin";
    s_ime_snap.ready = ime_ready(&s_ime);
    s_ime_snap.on = ime_on(&s_ime);
    return true;
}

#ifdef ESP_PLATFORM
/* ---- 変換中を見せる (docs/keyboard-ime-unification.md §6) ------------
 *
 * フロートを出すのは C 側。アプリは preedit も候補も見ないので、
 * ui.overlay を呼ぶ材料をそもそも持っていない。新しい配管は無く、
 * js_ui_overlay と同じ文字列を組んで同じキューへ載せるだけ。 */

/* preedit の塗り分け (skk_span_kind_t で引く)。
 *
 * ⚠ 暫定値。色は実機で見て決める約束のもの (§6.4) なので、1 か所に
 * 集めてある — 実機で気に入らなければこの表の 1 行を書き換えるだけで済む。
 * 下線や淡い背景が使えないのは LVGL の制約で (CONFIG_LV_USE_SPAN が無く、
 * recolor は前景色しか変えられない §6.3)、§6.1 の表はそのままでは実装
 * できない。色だけで「どこが仮でどこが決まったか」を出すのが出発点。 */
static const uint32_t IME_SPAN_COL[] = {
    [SKK_SPAN_MARK]    = 0x7F8C99, /* ▽/▼ 自体は記号なので一段落とす     */
    [SKK_SPAN_READING] = 0xFFFFFF, /* 打ったばかりの読み = 素の文字色     */
    [SKK_SPAN_CAND]    = 0xFFD479, /* 選択中の候補 = 制御バーの latch と同じ琥珀 */
    [SKK_SPAN_SEP]     = 0x7F8C99, /* '*' は区切りで、読みの一部ではない  */
    [SKK_SPAN_OKURI]   = 0x8FD3FF, /* 送り仮名 = 候補と別物だと分かる色   */
    [SKK_SPAN_ROMA]    = 0x9AA5AD, /* かなになる前のローマ字 = 一番仮     */
};

/* span 1 つを "#RRGGBB …#" で包んで足す。'#' は recolor の記号なので、
   本文に出てきたら 2 つに増やして逃がす (コンソールのログ整形と同じ作法)。
   入り切らないときは足さずに諦める — 途中で切ると markup が閉じず、
   そこから先の色が全部化ける。 */
static size_t ime_ovl_span(char *dst, size_t cap, size_t len, uint32_t col,
                           const char *s, size_t n)
{
    char head[10];
    if (n == 0)
        return len;      /* 空の span に色だけ置いても何も見えない */
    int hn = snprintf(head, sizeof head, "#%06X ", (unsigned)(col & 0xFFFFFF));
    if (hn < 0 || len + (size_t)hn + n * 2 + 1 >= cap)
        return len;
    memcpy(dst + len, head, (size_t)hn);
    len += (size_t)hn;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '#')
            dst[len++] = '#';
        dst[len++] = s[i];
    }
    dst[len++] = '#';
    dst[len] = '\0';
    return len;
}

static bool s_ime_ovl_on;   /* いま出ているか。所有タスクしか触らない */

/* preedit と候補を 1 枚のフロートにして出す (無ければ引っ込める)。
   呼ぶのは view ビットが「変わった」と言ったときだけ。 */
static void ime_float_update(void)
{
    size_t plen = 0;
    const char *pre = ime_on(&s_ime) ? ime_preedit(&s_ime, &plen) : NULL;

    if (!pre || plen == 0) {
        if (s_ime_ovl_on) {
            ui_send(UI_CMD_OVERLAY, 0, 0, UI_OVERLAY_IME, 0, 0, 0, NULL);
            s_ime_ovl_on = false;
        }
        return;
    }

    /* 所有タスク以外は触らないので static でよい。IME タスクのスタックは
       4KB しかなく、512B の作業配列を積むと辞書引きと同居できない。 */
    static char body[MQJS_OVL_TEXT_MAX];
    size_t len = 0;
    body[0] = '\0';

    /* 境界はエンジンから貰う。▼ では候補と送り仮名が区切り記号なしで
       連結されるので、文字列をいくら眺めても切れ目は出てこない (§6.1)。 */
    const skk_span_t *sp = NULL;
    int nsp = ime_preedit_spans(&s_ime, &sp);
    for (int i = 0; i < nsp; i++) {
        uint32_t col = (size_t)sp[i].kind <
                               (sizeof IME_SPAN_COL / sizeof *IME_SPAN_COL)
                           ? IME_SPAN_COL[sp[i].kind]
                           : IME_SPAN_COL[SKK_SPAN_READING];
        len = ime_ovl_span(body, sizeof body, len, col, pre + sp[i].off,
                           sp[i].len);
    }
    if (nsp <= 0)   /* 境界を貰えない preedit: 丸ごと素の色で出す */
        len = ime_ovl_span(body, sizeof body, len,
                           IME_SPAN_COL[SKK_SPAN_READING], pre, plen);

    int sel = ime_sel(&s_ime);
    int ncand = ime_cand_count(&s_ime);
    if (ncand > 0 && sel >= 0) {
        /* 窓の送りは ovl_window の担当 (端でだけ動く。中央固定はハイライトが
           止まって手応えが消えると実機で却下済み)。送った分だけを描く側は
           見るので、sel も窓の中の番号に直す。 */
        int first = ovl_window(UI_OVERLAY_IME, ncand, sel, UI_OVERLAY_ITEMS);
        sel -= first;
        if (ovl_append(body, sizeof body, &len, "\2") == 0) {
            for (int i = first; i < first + UI_OVERLAY_ITEMS && i < ncand; i++) {
                size_t cl = 0;
                const char *c = ime_cand(&s_ime, i, &cl);
                if (!c || len + cl + 1 >= sizeof body)
                    break;
                if (i > first)
                    body[len++] = '\1';
                memcpy(body + len, c, cl);  /* skk_cand は NUL 終端しない */
                len += cl;
                body[len] = '\0';
            }
        }
    } else {
        sel = -1;
    }

    /* アンカーはアプリのカーソル (ui.caret)。ただし field にフォーカスが
       あるときは、その矩形が正しい行き先 — アプリは preedit を知らないので
       ui.caret を呼ばない (I3)。高さを貰っていなければ端末の 1 行ぶんで
       代用する — 0 だと preedit が行に重なる。 */
    const MqjsWorker *fg = &s_workers[s_fg_worker];
    int ax = fg->caret_x, ay = fg->caret_y;
    int h = fg->caret_h;
    if (s_ime_field >= 0) {
        ax = s_ime_field_x;
        ay = s_ime_field_y;
        h = s_ime_field_h;
    }
    if (h <= 0) {
        int cw = 0, ch = 0;
        ui_tab5_cell_size(&cw, &ch);
        h = ch > 0 ? ch : 24;
    }

    /* ⚠ ui_send は所有権を取り、UI タスクが free() する。だからここは必ず
       ヒープ複製: body (静的でも配列) をそのまま渡せば UI タスクが自分の
       ものでない番地を free することになる。スタック配列を渡した前例は、
       無関係な確保の中で tlsf のアサートを踏んで初めて見つかった。 */
    char *copy = malloc(len + 1);
    if (!copy)
        return;
    memcpy(copy, body, len + 1);
    /* flags: bit0-1 = 0 (auto 配置)、bit2 = 0 (候補は横並び)、bit3 = recolor */
    ui_send(UI_CMD_OVERLAY, ax, ay, UI_OVERLAY_IME, h,
            (uint32_t)sel, 0x8, copy);
    s_ime_ovl_on = true;
}

/* 「あ」キーの面 = モード表示 (§6.2)。押すものと状態を見るものが同じに
   なるので、アプリの画面からモード表示が消える。
   変換中 (▽/▼) は基底が かな か カナ か分からず「あ」に倒れるが、その間
   状態を語っているのは preedit の方で (§6.1)、キーの面が要るのは preedit が
   無いときだけ。 */
static void ime_face_update(void)
{
    int face = UI_IME_FACE_ASCII;
    if (ime_on(&s_ime))
        face = ime_mode(&s_ime) == SKK_MODE_KATA ? UI_IME_FACE_KATA
                                                 : UI_IME_FACE_KANA;
    ui_tab5_ime_face(face);
}

/* 打鍵 1 つを IME に通し、残ったもの (素通し or 確定文字列) をアプリへ
   配達する。判定と配達が同じタスクの同じ関数に居るのが肝: 打鍵を渡した側は
   「食われたか」を聞かずに済み、キューが 1 本なので順序も勝手に保たれる。 */
static void ime_owner_key(const char *key, size_t len, uint32_t t_post)
{
    ime_disp_t d = IME_PASS;

    /* キューで待った時間。素通しの打鍵も数える — 待ちは打鍵の種類に関係なく
       全部が払っており、消費された打鍵だけ数えると数字が実際より良く見える。
       32bit の巻き戻り (71.6 分ごと) は引き算で消えるので下位だけで足りる。 */
    uint32_t hop = (uint32_t)time_us() - t_post;
    s_ime_m.hop_calls++;
    s_ime_m.hop_us += hop;
    if (hop > s_ime_m.hop_max_us)
        s_ime_m.hop_max_us = hop;

    if (s_ime_img >= 0) {
        int64_t t0 = time_us();
        d = ime_feed(&s_ime, key, len);
        /* 素通しは計測の手前で抜ける (§4.2)。辞書を引いていない = 帰属先が
           無いし、ASCII の 1 打鍵に 2 回目の時計読みと struct 複製を足すのは
           bitmask 設計が消したはずのコストそのもの。 */
        if (d != IME_PASS) {
            uint32_t us = (uint32_t)(time_us() - t0);
            /* 「この打鍵は変換か」はエンジンの lookups で決める。候補ゼロの
               空振りはビットが 1 本も立たないので、状態ビットで振り分けると
               いちばん高い打鍵が打鍵バケツに紛れる。 */
            skk_stats_t es;
            skk_stats(&s_ime.skk, &es);
            if (es.lookups != s_ime_m.last_lookups) {
                s_ime_m.last_lookups = es.lookups;
                s_ime_m.conv_calls++;
                s_ime_m.conv_us += us;
                if (us > s_ime_m.conv_max_us)
                    s_ime_m.conv_max_us = us;
            } else {
                s_ime_m.key_calls++;
                s_ime_m.key_us += us;
                if (us > s_ime_m.key_max_us)
                    s_ime_m.key_max_us = us;
            }
        }
        if (d == IME_TEXT) {
            key = ime_text(&s_ime, &len);
            if (!key || len == 0)
                d = IME_TAKEN;      /* 空の確定は流さない */
        }

        /* 何を描き直すかは view ビットだけで決める (再導出しない)。素通しの
           打鍵では 1 本も立たないので post も起きないし、▼ の中の SPACE 連打は
           SEL しか立てないので候補配列を組み直すのもそこだけで済む。 */
        uint32_t view = ime_view(&s_ime);
        if (view & (IME_V_PREEDIT | IME_V_CANDS | IME_V_SEL | IME_V_ENABLE))
            ime_float_update();
        if (view & (IME_V_MODE | IME_V_ENABLE))
            ime_face_update();
        ime_view_clear(&s_ime);
    }
    if (d == IME_TAKEN)
        return;                     /* 変換中: preedit は外へ 1 バイトも出さない */
    key_to_app(key, len);
}

/* コマンドキュー。打鍵は投げっぱなし、ARM/FOLD/STATS だけ ack を待つ。
   ImeCmd.key は KBD_SEQ_MAX(16) を包む — 入力面が作る打鍵はここに必ず収まる。 */
#define MQJS_IME_KEY_MAX  24
#define MQJS_IME_QUEUE_LEN 16   /* 32B×16 + キュー本体 ≒ 600B。連打 0.5 秒分:
                                   辞書引き 1 回の裏で溢れる深さではない。 */
enum { IME_CMD_KEY, IME_CMD_ARM, IME_CMD_FOLD, IME_CMD_STATS };

typedef struct {
    uint8_t  kind;
    uint8_t  len;
    /* post した時刻 (time_us の下位 32bit)。所有タスクが拾った時刻との差が
       hop = 打鍵がキューで待った時間。詰めの都合で len の直後に置く。 */
    uint32_t t_post;
    char     key[MQJS_IME_KEY_MAX];
} ImeCmd;

static QueueHandle_t     s_ime_q;
static SemaphoreHandle_t s_ime_ack;   /* 待つのは mqjs タスクだけ (JS 束縛) */
static volatile bool     s_ime_cmd_ok;

static void ime_owner_task(void *arg)
{
    (void)arg;
    ImeCmd c;
    for (;;) {
        if (xQueueReceive(s_ime_q, &c, portMAX_DELAY) != pdTRUE)
            continue;
        switch (c.kind) {
        case IME_CMD_KEY:    ime_owner_key(c.key, c.len, c.t_post); break;
        case IME_CMD_ARM:    s_ime_cmd_ok = ime_arm_now();
                             xSemaphoreGive(s_ime_ack);     break;
        /* 計測も所有タスクの上で撮る。ack を待つのは mqjs タスク 1 人という
           前提は変わらない (呼び手は JS 束縛だけ)。 */
        case IME_CMD_STATS:  s_ime_cmd_ok = ime_snap_now();
                             xSemaphoreGive(s_ime_ack);     break;
        case IME_CMD_FOLD:   ime_fold_now();
                             /* 畳んだのに読みかけが画面に残ると、次のアプリの
                                上に他人の preedit が浮いたままになる。 */
                             ime_float_update();
                             ime_face_update();
                             s_ime_cmd_ok = false;
                             xSemaphoreGive(s_ime_ack);     break;
        }
    }
}

/* 最初の opt-in で 1 度だけ立てる。誰も ui.ime() を呼ばないファームでは
   タスクもキューも作られない = RAM を 1 バイトも払わない。落とす手段は
   用意しない: 走らせたまま眠っているタスクより、生きているタスクを消す
   方がよほど危ない (microlink の UAF の教訓)。 */
static bool ime_owner_start(void)
{
    if (s_ime_q)
        return true;
    if (!s_ime_ack)
        s_ime_ack = xSemaphoreCreateBinary();
    if (!s_ime_ack)
        return false;
    s_ime_q = xQueueCreate(MQJS_IME_QUEUE_LEN, sizeof(ImeCmd));
    if (!s_ime_q)
        return false;
    /* 優先度は打鍵を渡してくる側 (mqjs/kbd_tab5 = 5) と同じ。上げると入力面を
       押しのけ、下げると打鍵がここで待たされる。 */
    if (xTaskCreate(ime_owner_task, "mqjs_ime", 4096, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_ime_q);
        s_ime_q = NULL;
        return false;
    }
    return true;
}

/* mqjs タスクから 1 コマンド投げて、完了を待つ。待つのは ack であって
   時間ではない (盲目の vTaskDelay で join しない — 単一所有者を入れた
   tailscale_adapter で払った授業料)。上限を付けてあるのは、所有タスクが
   何かで詰まったときにアプリの起動/停止まで道連れにしないため。
   FIFO が 1 本なので、FOLD の ack は「手前に並んでいた打鍵を配り終えた」
   印も兼ねる — 畳んだ拍子に打ち終わりが消えることはない。 */
static bool ime_cmd_sync(uint8_t kind)
{
    if (!s_ime_q)
        return false;
    /* 前回タイムアウトした ack が残っていると次のコマンドが即座に「完了」に
       見えてしまう。投げる前に捨てる。 */
    xSemaphoreTake(s_ime_ack, 0);
    ImeCmd c = { .kind = kind };
    if (xQueueSend(s_ime_q, &c, pdMS_TO_TICKS(200)) != pdTRUE)
        return false;
    if (xSemaphoreTake(s_ime_ack, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "ime cmd %u: no ack", (unsigned)kind);
        return false;
    }
    return s_ime_cmd_ok;
}

/* 打鍵の入口 (LVGL タスク / ドックの kbd タスク) から呼ばれる。opt-in して
   いないアプリでは 1 回の分岐で false = 従来どおりの直行便。 */
static bool ime_route_key(const char *utf8, size_t len)
{
    MqjsWorker *fg = &s_workers[s_fg_worker];
    /* field にフォーカスがある間は、その field のモードだけが判断材料。
       ui.ime(1) 済みのアプリの都合を通すと、SSID の欄に かな が入る (I3)。 */
    int fmode = s_ime_field;
    bool want = fmode < 0 ? fg->ime_used : fmode > 0;
    /* len 超過は入力面には作れない (KBD_SEQ_MAX=16) が、通ってきたら IME を
       迂回させる — 千切って渡すと変換の途中に嘘の打鍵を混ぜることになる。 */
    if (!want || !s_ime_q || len > MQJS_IME_KEY_MAX)
        return false;
    ImeCmd c = { .kind = IME_CMD_KEY, .len = (uint8_t)len,
                 .t_post = (uint32_t)time_us() };
    memcpy(c.key, utf8, len);
    /* キューが溢れたら握り潰す。素通しにすると、変換中に飲まれるはずの打鍵が
       アプリへ抜けるうえ順序も崩れる — 落とす方がまし (s_event_queue が満杯の
       ときと同じ方針)。ただし黙って消えると原因不明の「打鍵が抜ける」に
       なるので、数だけは残す (ui.imeStats の drops)。 */
    if (xQueueSend(s_ime_q, &c, 0) != pdTRUE)
        s_ime_drops++;
    return true;
}

/* widget の field がフォーカスを取った/失った (I3)。呼ぶのは LVGL タスク
   なので、ここでは何も待たないし、辞書も開かない:

   - ack を待てない。s_ime_ack を待つのは mqjs タスク 1 人という前提で、
     2 人目が入ると ime_cmd_sync の「取り残しを捨ててから投げる」が壊れる。
     UI タスクを辞書引きの裏で 2 秒止める、という別の害もある。
   - 辞書は field を作った時点 (js_uiscreen_create) で mqjs タスクが
     開いている。ja の field は arm に成功したものだけなので、ここへ来た
     時点で s_ime_q は必ず出来上がっている。

   ja でないフォーカス (と、フォーカスを失ったとき) は FOLD を投げる:
   IME を切り、読みかけを捨て (確定させない)、「あ」の面を A へ戻す。 */
void mqjs_ime_field_focus(int mode, int x, int y, int h)
{
    s_ime_field_x = (int16_t)x;
    s_ime_field_y = (int16_t)y;
    s_ime_field_h = (int16_t)h;
    /* 先に落とす: poster (このタスク自身とドックの kbd タスク) を止めてから
       畳む。逆順だと、畳んだ直後の 1 打鍵が消えたセッションへ流れる。 */
    s_ime_field = mode < 0 ? -1 : (mode == UIW_FIELD_JA ? 1 : 0);
    if (s_ime_field == 1 || !s_ime_q)
        return;
    ImeCmd c = { .kind = IME_CMD_FOLD };
    xQueueSend(s_ime_q, &c, 0);
}

/* --- ここから下は mqjs タスク側 (JS 束縛から呼ばれる) --- */

static bool ime_arm(void)
{
    return ime_owner_start() && ime_cmd_sync(IME_CMD_ARM);
}

/* 畳むのは所有タスク。畳み終わるのを待つだけで、この後に続く仕事は無い。 */
static void ime_disarm(void)
{
    (void)ime_cmd_sync(IME_CMD_FOLD);
}
#else
/* ホストには打鍵を投げてくるタスクが無く、mqjs タスクが唯一の所有者。 */
static bool ime_arm(void)    { return ime_arm_now(); }
static void ime_disarm(void) { ime_fold_now(); }
#endif

/* ui.ime(mode) — canvas アプリの opt-in (§7/§8.2)。1 = このアプリの打鍵を
   IME に通す、0 = 通さない。既存アプリの挙動を変えないための明示 opt-in で、
   widget の field は I3 で自動になる。
   返すのは「実際に有効になったか」— 辞書の無いファームでは false で、
   アプリは見えない変換を始めずに済む。 */
JSValue js_ui_ime(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    int mode;
    if (JS_ToInt32(ctx, &mode, argv[0]))
        return JS_EXCEPTION;
    if (mode) {
        if (!ime_arm())
            return JS_NewBool(false);
        s_cur_wk->ime_used = true;
        return JS_NewBool(true);
    }
    if (s_cur_wk->ime_used) {
        s_cur_wk->ime_used = false;  /* 先に落とす: poster を止めてから畳む */
        ime_disarm();
    }
    return JS_NewBool(false);
}

/* ui.imeStats() -> object | null。プラットフォーム IME の実測値。
 *
 * ui.* に置いたのは、アプリから見える IME がもう ui.ime / ui.caret の 2 本
 * しかないから — 計測もその隣に置く。文字列 (audio.stats の形)
 * ではなくオブジェクトを返すのは、呼び手が必ずやることが JSON.stringify で
 * MQTT へ流すことだから: 文字列で返すと JSON.parse して組み直すぶん、
 * アプリのアリーナで無駄に往復する。
 *
 * null = 一度も arm されていない (辞書の無いファーム / 誰も ui.ime(1) を
 * 呼んでいない)。0 を並べたオブジェクトを返すと「速い」と読めてしまう。 */
JSValue js_ui_imeStats(JSContext *ctx, JSValue *this_val, int argc,
                       JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
#ifdef ESP_PLATFORM
    /* 撮るのは所有タスク。s_ime_q が無い = まだ誰も opt-in していない。 */
    if (!ime_cmd_sync(IME_CMD_STATS))
        return JS_NULL;
#else
    if (!ime_snap_now())
        return JS_NULL;
#endif
    const ImeSnap *s = &s_ime_snap;
    JSGCRef obj_ref;
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_PUSH_VALUE(ctx, obj);
    /* エンジンが数えるもの (skk_stats_t) */
    JS_SetPropertyStr(ctx, obj_ref.val, "keys", JS_NewInt32(ctx, (int)s->eng.keys));
    JS_SetPropertyStr(ctx, obj_ref.val, "consumed",
                      JS_NewInt32(ctx, (int)s->eng.consumed));
    JS_SetPropertyStr(ctx, obj_ref.val, "lookups",
                      JS_NewInt32(ctx, (int)s->eng.lookups));
    JS_SetPropertyStr(ctx, obj_ref.val, "probes",
                      JS_NewInt32(ctx, (int)s->eng.probes));
    JS_SetPropertyStr(ctx, obj_ref.val, "fullcmp",
                      JS_NewInt32(ctx, (int)s->eng.fullcmp));
    JS_SetPropertyStr(ctx, obj_ref.val, "cands",
                      JS_NewInt32(ctx, (int)s->eng.cands));
    JS_SetPropertyStr(ctx, obj_ref.val, "dropped",
                      JS_NewInt32(ctx, (int)s->eng.dropped));
    JS_SetPropertyStr(ctx, obj_ref.val, "commits",
                      JS_NewInt32(ctx, (int)s->eng.commits));
    /* ime_feed の中身。辞書を引いた打鍵とそうでない打鍵は予算が 3 桁違う */
    JS_SetPropertyStr(ctx, obj_ref.val, "keyCalls",
                      JS_NewInt32(ctx, (int)s->m.key_calls));
    JS_SetPropertyStr(ctx, obj_ref.val, "keyUs",
                      JS_NewInt32(ctx, (int)s->m.key_us));
    JS_SetPropertyStr(ctx, obj_ref.val, "keyMaxUs",
                      JS_NewInt32(ctx, (int)s->m.key_max_us));
    JS_SetPropertyStr(ctx, obj_ref.val, "convCalls",
                      JS_NewInt32(ctx, (int)s->m.conv_calls));
    JS_SetPropertyStr(ctx, obj_ref.val, "convUs",
                      JS_NewInt32(ctx, (int)s->m.conv_us));
    JS_SetPropertyStr(ctx, obj_ref.val, "convMaxUs",
                      JS_NewInt32(ctx, (int)s->m.conv_max_us));
    /* キューで待った時間。この構造にしか無い数字で、「打鍵が重い」の答えは
       たいていこちら側に居る */
    JS_SetPropertyStr(ctx, obj_ref.val, "hopCalls",
                      JS_NewInt32(ctx, (int)s->m.hop_calls));
    JS_SetPropertyStr(ctx, obj_ref.val, "hopUs",
                      JS_NewInt32(ctx, (int)s->m.hop_us));
    JS_SetPropertyStr(ctx, obj_ref.val, "hopMaxUs",
                      JS_NewInt32(ctx, (int)s->m.hop_max_us));
    JS_SetPropertyStr(ctx, obj_ref.val, "drops",
                      JS_NewInt32(ctx, (int)s->drops));
    /* どの辞書を引いているのか (梅/竹/松の区別は段数とバイト数で付く) */
    JS_SetPropertyStr(ctx, obj_ref.val, "dictBytes",
                      JS_NewInt32(ctx, (int)s->img_len));
    JS_SetPropertyStr(ctx, obj_ref.val, "loadUs",
                      JS_NewInt32(ctx, (int)s->img_load_us));
    JS_SetPropertyStr(ctx, obj_ref.val, "nasi", JS_NewInt32(ctx, (int)s->nasi));
    JS_SetPropertyStr(ctx, obj_ref.val, "ari", JS_NewInt32(ctx, (int)s->ari));
    JS_SetPropertyStr(ctx, obj_ref.val, "levels",
                      JS_NewInt32(ctx, (int)s->levels));
    JS_SetPropertyStr(ctx, obj_ref.val, "on", JS_NewBool(s->on));
    JS_SetPropertyStr(ctx, obj_ref.val, "ready", JS_NewBool(s->ready));
    /* 文字列は必ず自分の文で作る: JS_SetPropertyStr の引数の中で作ると、
       その中の GC が obj_ref.val を動かした後で古い値が読まれうる。 */
    JSValue v_dict = JS_NewString(ctx, s->dict ? s->dict : "");
    JS_SetPropertyStr(ctx, obj_ref.val, "dict", v_dict);
    JS_POP_VALUE(ctx, obj);
    return obj;
}

/* ------------------------------------------------------------------ */
/* term.* — the native terminal (docs/term-design.md §8)               */
/* ------------------------------------------------------------------ */
/*
 * Thin argument marshalling over term_registry, holding no state of its
 * own. Three rules from the design shape every function here:
 *
 *   - The OWNER is never taken from JS. It is s_cur_wk->name, i.e. the
 *     app identity the signed push established (§3.1 I3). An app cannot
 *     name another app's namespace, so it cannot read or write another
 *     app's terminal, and there is no cross-app read API at all (§7.2).
 *   - Errors are RETURN VALUES, not exceptions (§8): a closed, reused or
 *     foreign id yields a negative term_err_t (null for the two string
 *     getters). A terminal error must not kill an app.
 *   - Nothing blocks. create/log/feed/resize/show/close are copy-and-
 *     return; snapshot/read post to the UI task and join with a cap
 *     (§7.2), which the registry does, not this layer.
 */

/* Bring the subsystem up on first use. On the device this installs the
   FreeRTOS port, starts the reaper and hooks the UI frame; on the host
   it installs a single-threaded fake so run_pc exercises the same code.
   Doing it lazily keeps ~100KB of PSRAM unspent until an app actually
   asks for a terminal. */
#ifndef ESP_PLATFORM
/* PC port: run_pc is single-threaded, so the mutex is a debug counter,
   the "UI task" is the caller, and the signal is never waited on. */
static void *tp_mutex_create(void) { return calloc(1, sizeof(int)); }
static void tp_mutex_destroy(void *m) { free(m); }
static bool tp_mutex_lock(void *m, uint32_t ms)
{
    (void)ms;
    int *held = m;
    if (*held)
        return false; /* would be a self-deadlock: report, never hang */
    *held = 1;
    return true;
}
static void tp_mutex_unlock(void *m) { *(int *)m = 0; }
static int64_t tp_now_ms(void) { return time_ms(); }
static void *tp_signal_create(void) { return calloc(1, sizeof(int)); }
static void tp_signal_destroy(void *s) { free(s); }
static void tp_signal_set(void *s) { *(int *)s = 1; }
static bool tp_signal_wait(void *s, uint32_t ms) { (void)ms; return *(int *)s != 0; }
static bool tp_ui_post(term_ui_job_fn fn, void *arg, uint32_t ms)
{
    (void)ms;
    fn(arg);
    return true;
}
static bool tp_ui_is_current(void) { return true; }
static void *tp_mem_alloc(size_t n, unsigned flags) { (void)flags; return malloc(n); }
static void tp_mem_free(void *p) { free(p); }

static const term_port_t s_term_pc_port = {
    tp_mutex_create, tp_mutex_destroy, tp_mutex_lock, tp_mutex_unlock,
    tp_now_ms,
    tp_signal_create, tp_signal_destroy, tp_signal_set, tp_signal_wait,
    tp_ui_post, tp_ui_is_current,
    tp_mem_alloc, tp_mem_free,
    NULL, NULL,
};
#endif /* !ESP_PLATFORM */

/* Grid that fills the canvas, clamped to what one term block was sized
   for (§4.1 worst case: 142 cols / 53 rows / 4,260 cells). */
static void term_default_grid(int *cols, int *rows)
{
    int cw = 9, ch = 24, w = 720, h = 1192;
#ifdef ESP_PLATFORM
    ui_tab5_cell_size(&cw, &ch);
    ui_tab5_canvas_size(&w, &h);
#endif
    if (cw <= 0) cw = 9;
    if (ch <= 0) ch = 24;
    if (w <= 0) w = 720;
    if (h <= 0) h = 1192;
    *cols = w / cw;
    *rows = h / ch;
    if (*cols < 1) *cols = 1;
    if (*rows < 1) *rows = 1;
    if (*cols > TERM_MAX_COLS_DEFAULT) *cols = TERM_MAX_COLS_DEFAULT;
    if (*rows > TERM_MAX_ROWS_DEFAULT) *rows = TERM_MAX_ROWS_DEFAULT;
    while (*cols * *rows > TERM_MAX_CELLS_DEFAULT)
        (*rows)--;
}

/* ---- §10.2: the caret, pushed from C into the platform's caret state ----
 *
 * A JS app tells the platform where its cursor is by calling ui.caret();
 * the value lands in its worker's caret_x/y/h and the IME's owner task
 * reads the FOREGROUND worker's copy when it draws the preedit float. A
 * native term is the same kind of writer, one layer down: the drain notices
 * that the cursor cell moved and pushes the rectangle into the very same
 * three fields (§10.2 — "JS アプリが ui.caret を呼ぶのと同じプラットフォーム
 * 側 caret シンクへ C-to-C で更新を push する").
 *
 * That is the whole of the term<->IME relationship. term does not know
 * ime_core exists, ime_core does not know term exists, and the polling
 * alternative the design rejects by name (ime asking term for its cursor)
 * has no entry point to call.
 *
 * Runs on the UI task, once per term per frame, after the registry has
 * released its lock. Three int16 stores, so a reader that catches it
 * mid-update is one keystroke stale in the float's position — the same
 * tolerance ui.caret already documents. */
static void term_caret_sink(void *user, const term_caret_ev_t *ev)
{
    (void)user;
    if (!ev->visible)
        return; /* cursor hidden: the anchor would be a lie, so say nothing
                   and let the float fall back the way it does for an app
                   that never called ui.caret */
    for (int i = 0; i < MQJS_MAX_WORKERS; i++) {
        MqjsWorker *w = &s_workers[i];
        if (!w->used || strcmp(w->name, ev->owner) != 0)
            continue;
        w->caret_x = (int16_t)ev->x;
        w->caret_y = (int16_t)ev->y;
        w->caret_h = (int16_t)ev->h;
        return;
    }
}

static bool term_ready(void)
{
    if (term_registry_ready())
        return true;

    /* The shared system console (§11.2's "汎用 console 画面"): a TERM_LOG
       term owned by the platform that every app's print() tees into.
       Created here rather than at boot so a device that never opens a
       terminal never pays its ~110KB of PSRAM. */
    term_create_opts_t console;
    term_registry_config_t cfg;
    int cols, rows;

    term_default_grid(&cols, &rows);
    memset(&console, 0, sizeof console);
    console.name = "console";
    console.owner = TERM_OWNER_SYSTEM; /* term_registry_init forces this */
    console.mode = TERM_LOG;
    console.cols = cols;
    console.rows = rows;
    memset(&cfg, 0, sizeof cfg);
    cfg.console = &console;
    /* §10.2's conversion needs the grid's pixel pitch, and this is the
       only layer that knows both it and the registry. */
    cfg.cell_w = 9;
    cfg.cell_h = 24;
#ifdef ESP_PLATFORM
    ui_tab5_cell_size(&cfg.cell_w, &cfg.cell_h);
    if (cfg.cell_w <= 0 || cfg.cell_h <= 0) {
        cfg.cell_w = 9;
        cfg.cell_h = 24;
    }
    if (term_ui_tab5_start(&cfg) != TERM_OK)
        return false;
#else
    if (!term_port_installed() && !term_port_install(&s_term_pc_port))
        return false;
    if (term_registry_init(&cfg) != TERM_OK)
        return false;
#endif
    term_registry_set_caret_sink(term_caret_sink, NULL);
    return true;
}

/*
 * Give the registry its frame time on builds that have no UI task
 * (run_pc). On the device this is a no-op: the drain and the blit are
 * driven by ui_tab5's frame hook, and the reaper by its own task —
 * neither of them is js_task's business (§5, §3.1).
 */
static void mqjs_term_pump(void)
{
#ifndef ESP_PLATFORM
    if (!term_registry_ready())
        return;
    term_registry_ui_drain();
    /* The host pipe stub's "own task" moment: a requested detach becomes an
       ack here, one pump after it was asked for, the way the ssh session
       task does it on the device (term_pipe.h). Before the reap, so a close
       that was waiting only on the producer completes in the same pass. */
    term_pipe_pump();
    term_registry_reap();
#endif
}

/* The print sink's tee (§3.1 "print sink -> registry", §4.4's black box).
   Never allocates, never blocks. No readiness check on purpose: the black
   box half of term_registry_platform_log works before the registry is up,
   and a line from a boot that never reached the console is exactly the kind
   the flight recorder exists to keep. */
static void term_sink_line(const char *writer, term_wclass_t wc,
                           const char *line, size_t len)
{
    term_registry_platform_log(writer, wc, line, len);
}

/* The calling app's identity, or NULL when there is no app on the
   stack (early init). NULL means "refuse", never "skip the gate". */
static const char *term_owner(void)
{
    if (!s_cur_wk || !s_cur_wk->name[0])
        return NULL;
    return s_cur_wk->name;
}

static JSValue term_err_value(JSContext *ctx, term_err_t e)
{
    return JS_NewInt32(ctx, (int)e);
}

/* term.create({name, mode, persist, cols, rows}) -> id, or a negative
   term_err_t. A second create with the same name from the same app
   re-attaches to the persist term it left behind (§3.1, tmux). */
JSValue js_term_create(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    const char *owner = term_owner();
    if (!owner)
        return term_err_value(ctx, TERM_ERR_NOT_READY);
    if (!term_ready())
        return term_err_value(ctx, TERM_ERR_NOT_READY);

    /* The name is the per-owner re-attach key (§3.1) — the thing that
       makes term.create idempotent across an app restart — so there is
       no default for it: a term nobody named could never be picked back
       up. An over-long one is REJECTED, never truncated, for the reason
       §3.1 gives about identities that silently collide; hence the
       oversized landing buffer, so the length is measured before any
       clamping can hide it. */
    char name[TERM_NAME_MAX + 16];
    char mode[8] = "";
    int cols = 0, rows = 0, tmp = 0;
    bool persist = false;
    JSValue v;

    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return term_err_value(ctx, TERM_ERR_INVAL);
    if (uiw_copy_str(ctx, JS_GetPropertyStr(ctx, argv[0], "name"),
                     name, sizeof name))
        return JS_EXCEPTION;
    if (!name[0] || strlen(name) >= TERM_NAME_MAX)
        return term_err_value(ctx, TERM_ERR_INVAL);
    if (uiw_copy_str(ctx, JS_GetPropertyStr(ctx, argv[0], "mode"),
                     mode, sizeof mode))
        return JS_EXCEPTION;
    persist = uiw_truthy(ctx, JS_GetPropertyStr(ctx, argv[0], "persist"));
    v = JS_GetPropertyStr(ctx, argv[0], "cols");
    if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v))
        cols = tmp;
    v = JS_GetPropertyStr(ctx, argv[0], "rows");
    if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v))
        rows = tmp;
    if (cols < 1 || rows < 1) {
        int dc, dr;
        term_default_grid(&dc, &dr);
        if (cols < 1) cols = dc;
        if (rows < 1) rows = dr;
    }

    term_create_opts_t o;
    memset(&o, 0, sizeof o);
    o.name = name;
    o.owner = owner;
    o.mode = (mode[0] == 'v') ? TERM_VT : TERM_LOG; /* default: log */
    o.persist = persist;
    o.cols = cols;
    o.rows = rows;

    term_id_t id = TERM_ID_INVALID;
    term_err_t e = term_registry_create(&o, &id, NULL);
    if (e != TERM_OK)
        return term_err_value(ctx, e);
    return JS_NewInt32(ctx, (int)id);
}

/* term.show(id, {x, y, w, h}) -> 0 or a negative term_err_t. A missing
   rect hides the term without forgetting where it was (§8: tab
   switching is show/hide, not create/destroy). */
JSValue js_term_show(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    const char *owner = term_owner();
    int id = 0, tmp = 0;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (!owner || !term_registry_ready())
        return term_err_value(ctx, TERM_ERR_NOT_READY);

    if (argc < 2 || JS_IsUndefined(argv[1]) || JS_IsNull(argv[1]))
        return term_err_value(ctx,
                              term_registry_show((term_id_t)id, owner, NULL));

    term_view_t view;
    memset(&view, 0, sizeof view);
    view.visible = true;
    JSValue v = JS_GetPropertyStr(ctx, argv[1], "x");
    if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v)) view.x = (int16_t)tmp;
    v = JS_GetPropertyStr(ctx, argv[1], "y");
    if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v)) view.y = (int16_t)tmp;
    v = JS_GetPropertyStr(ctx, argv[1], "w");
    if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v)) view.w = (int16_t)tmp;
    v = JS_GetPropertyStr(ctx, argv[1], "h");
    if (JS_IsNumber(ctx, v) && !JS_ToInt32(ctx, &tmp, v)) view.h = (int16_t)tmp;
    return term_err_value(ctx, term_registry_show((term_id_t)id, owner, &view));
}

/* term.log(id, str) -> 0 or a negative term_err_t. Line-atomic and
   lossy: a full ring drops the whole line and counts it (§3.2), which
   is why logging can never stall the app that logs. */
JSValue js_term_log(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    const char *owner = term_owner();
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    JSCStringBuf buf;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[1], &buf);
    if (!s)
        return JS_EXCEPTION;
    if (!owner || !term_registry_ready())
        return term_err_value(ctx, TERM_ERR_NOT_READY);
    return term_err_value(ctx,
                          term_registry_log((term_id_t)id, owner, s, len));
}

/* term.feed(id, str) -> 0 or a negative term_err_t. Raw bytes with full
   VT interpretation; parsed later, on the UI task (§5). Piped terms
   refuse it (TERM_ERR_BUSY = -10) because a VT term has exactly one
   producer. */
JSValue js_term_feed(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    const char *owner = term_owner();
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    JSCStringBuf buf;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[1], &buf);
    if (!s)
        return JS_EXCEPTION;
    if (!owner || !term_registry_ready())
        return term_err_value(ctx, TERM_ERR_NOT_READY);
    return term_err_value(ctx,
                          term_registry_feed((term_id_t)id, owner,
                                             (const uint8_t *)s, len, NULL));
}

/* term.resize(id, cols, rows) -> 0 or a negative term_err_t. Notifying
   the pty is the caller's business (§8). */
JSValue js_term_resize(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    const char *owner = term_owner();
    int id = 0, cols = 0, rows = 0;
    if (JS_ToInt32(ctx, &id, argv[0]) || JS_ToInt32(ctx, &cols, argv[1]) ||
        JS_ToInt32(ctx, &rows, argv[2]))
        return JS_EXCEPTION;
    if (!owner || !term_registry_ready())
        return term_err_value(ctx, TERM_ERR_NOT_READY);
    return term_err_value(ctx,
                          term_registry_resize((term_id_t)id, owner, cols, rows));
}

/* term.snapshot(id) -> the visible screen as text, or null. Serialised
   on the UI task at a frame boundary, and non-mutating (§7.2). */
JSValue js_term_snapshot(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    const char *owner = term_owner();
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (!owner || !term_registry_ready())
        return JS_NULL;
    /* §7.2 bounds one screen at ~14KB; 16KB covers it with the row
       separators. Off the JS heap on purpose — a screenful of text
       should not move an app's 256KB arena. */
    size_t cap = 16384;
    char *buf = malloc(cap);
    if (!buf)
        return JS_NULL;
    size_t len = 0;
    term_err_t e = term_registry_snapshot((term_id_t)id, owner, buf, cap, &len);
    if (e != TERM_OK && e != TERM_ERR_TRUNC) {
        free(buf);
        return JS_NULL;
    }
    if (len > cap - 1)
        len = cap - 1; /* TRUNC reports what was needed, not what fits */
    JSValue v = JS_NewStringLen(ctx, buf, len);
    free(buf);
    return v;
}

/* term.read(id, from, n) -> n logical scrollback lines joined by '\n',
   or null. `from` below the surviving range starts at the oldest line
   rather than failing (§8). */
JSValue js_term_read(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    const char *owner = term_owner();
    int id = 0, from = 0, n = 1;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt32(ctx, &from, argv[1]))
        return JS_EXCEPTION;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToInt32(ctx, &n, argv[2]))
        return JS_EXCEPTION;
    /* No clamp on n: term_registry.h makes n <= 0 a TERM_ERR_INVAL, and a
     * binding that silently turned -1 into 1 would answer a malformed
     * request with somebody's scrollback. The omitted argument still
     * defaults to 1; an explicit 0 or negative goes to the registry and
     * comes back as null. */
    if (!owner || !term_registry_ready())
        return JS_NULL;
    size_t cap = 8192;
    char *buf = malloc(cap);
    if (!buf)
        return JS_NULL;
    term_read_result_t res;
    memset(&res, 0, sizeof res);
    term_err_t e = term_registry_read((term_id_t)id, owner, (uint32_t)from, n,
                                      buf, cap, &res);
    if (e != TERM_OK) {
        free(buf);
        return JS_NULL;
    }
    JSValue v = JS_NewStringLen(ctx, buf, res.bytes);
    free(buf);
    return v;
}

/* ---- term.pipe / term.unpipe (§5, §8) ---------------------------------
 *
 * The point of the pipe is what does NOT happen in JS: after this call the
 * ssh channel's bytes go into the term's ring in C, are parsed on the UI
 * task and become pixels, without one of them entering the app's heap or
 * costing an interpreted instruction (§9.1). The app orchestrates connect
 * and tab switching; that is all.
 *
 * Off the device there is no ssh, and the handle binds the host stub
 * producer instead (term_pipe.h) — real enough that feed/log answer BUSY
 * and that the re-pipe rule is observable, which is what run_pc tests. */
JSValue js_term_pipe(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    const char *owner = term_owner();
    int id = 0, handle = 0;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) &&
        JS_ToInt32(ctx, &handle, argv[1]))
        return JS_EXCEPTION;
    if (!owner || !term_registry_ready())
        return term_err_value(ctx, TERM_ERR_NOT_READY);
    return term_err_value(ctx, term_pipe_bind((term_id_t)id, owner, handle));
}

/* term.unpipe(id) -> 0 or a negative term_err_t. Asks the producer to
   detach and returns; the term stops being piped when the ack lands
   (§3.1 stage 1 — nothing is joined here). */
JSValue js_term_unpipe(JSContext *ctx, JSValue *this_val, int argc,
                       JSValue *argv)
{
    (void)this_val;
    (void)argc;
    const char *owner = term_owner();
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (!owner || !term_registry_ready())
        return term_err_value(ctx, TERM_ERR_NOT_READY);
    return term_err_value(ctx, term_pipe_unbind((term_id_t)id, owner));
}

/* ---- term.onReply (§6, §8) --------------------------------------------
 *
 * The sink runs on the UI task inside the drain, with the registry's table
 * lock held, so it cannot call JS and cannot allocate. It posts the reply
 * INLINE in an event instead — replies are at most TERM_REPLY_MAX bytes —
 * and dispatch_term_reply() calls the app's function from js_task.
 *
 * `user` carries the worker's slot and generation, not a pointer: an app
 * that stopped and restarted between the DSR and its answer must not have
 * its successor's callback invoked (the same stale-event rule §3.2 applies
 * to every worker-addressed event). */
static void *term_reply_cookie(const MqjsWorker *w)
{
    return (void *)(uintptr_t)(((uint32_t)w->gen << 8) | w->idx);
}

static void term_reply_sink(void *user, term_id_t id, const char *bytes,
                            size_t len)
{
    uint32_t cookie = (uint32_t)(uintptr_t)user;
    MqjsEvent ev;

    if (len > sizeof ev.u.term.bytes)
        return; /* term_reply_sink_fn caps it at TERM_REPLY_MAX; a longer
                   reply would be a core bug, and truncating a control
                   sequence is worse than dropping it whole */
    memset(&ev, 0, sizeof ev);
    ev.type = EV_TERM_REPLY;
    ev.worker = (uint8_t)(cookie & 0xFF);
    ev.gen = (uint16_t)(cookie >> 8);
    ev.u.term.id = (int32_t)id;
    ev.u.term.len = (uint8_t)len;
    memcpy(ev.u.term.bytes, bytes, len);
    /* Zero wait: this is the UI task under a lock. A refused reply is one
       the remote asks again for; a blocked frame is a frozen device. */
    ev_post(&ev, 0);
}

static TermCb *termcb_get(MqjsWorker *app, int32_t id)
{
    int i;
    for (i = 0; i < MQJS_MAX_TERM_CB; i++)
        if (app->term_cbs[i].used && app->term_cbs[i].id == id)
            return &app->term_cbs[i];
    for (i = 0; i < MQJS_MAX_TERM_CB; i++) {
        if (!app->term_cbs[i].used) {
            memset(&app->term_cbs[i], 0, sizeof app->term_cbs[i]);
            app->term_cbs[i].used = true;
            app->term_cbs[i].id = id;
            return &app->term_cbs[i];
        }
    }
    return NULL;
}

static void termcb_release(JSContext *ctx, TermCb *c)
{
    if (c->fn_used)
        JS_DeleteGCRef(ctx, &c->fn);
    memset(c, 0, sizeof *c);
}

/* term.onReply(id, fn) -> 0 or a negative term_err_t. fn null/undefined
   removes the sink. A PIPED term never calls it: its replies go back over
   the channel in C (§6), which is the answer the remote asked for. */
JSValue js_term_onReply(JSContext *ctx, JSValue *this_val, int argc,
                        JSValue *argv)
{
    (void)this_val;
    const char *owner = term_owner();
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (!owner || !term_registry_ready())
        return term_err_value(ctx, TERM_ERR_NOT_READY);

    bool clear = argc < 2 || JS_IsUndefined(argv[1]) || JS_IsNull(argv[1]);
    TermCb *c = termcb_get(s_cur_wk, (int32_t)id);
    if (!c)
        return term_err_value(ctx, TERM_ERR_NO_SLOT);
    if (clear) {
        term_err_t e = term_registry_set_reply((term_id_t)id, owner, NULL,
                                               NULL);
        termcb_release(ctx, c);
        return term_err_value(ctx, e);
    }
    /* Install in the registry FIRST: if the id is not ours the callback is
       never stored, so a rejected call leaves no rooted function behind. */
    term_err_t e = term_registry_set_reply((term_id_t)id, owner,
                                          term_reply_sink,
                                          term_reply_cookie(s_cur_wk));
    if (e != TERM_OK) {
        termcb_release(ctx, c);
        return term_err_value(ctx, e);
    }
    if (c->fn_used)
        JS_DeleteGCRef(ctx, &c->fn);
    JSValue *pf = JS_AddGCRef(ctx, &c->fn);
    *pf = argv[1];
    c->fn_used = true;
    return term_err_value(ctx, TERM_OK);
}

static void dispatch_term_reply(MqjsWorker *app, const MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    TermCb *c = NULL;
    for (int i = 0; i < MQJS_MAX_TERM_CB; i++)
        if (app->term_cbs[i].used && app->term_cbs[i].id == ev->u.term.id)
            c = &app->term_cbs[i];
    if (!c || !c->fn_used)
        return;
    if (JS_StackCheck(ctx, 3)) {
        dump_error(ctx);
        return;
    }
    JS_PushArg(ctx, JS_NewStringLen(ctx, ev->u.term.bytes, ev->u.term.len));
    JS_PushArg(ctx, c->fn.val);
    JS_PushArg(ctx, JS_NULL);
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 1);
    if (JS_IsException(ret))
        dump_error(ctx);
}

/* ---- term.record / term.recordScreen (§4.4's 2026-07-30 exception) -----
 *
 * term.record(id)        -> 1 recording, 0 not, or a negative term_err_t
 * term.record(id, on)    -> 0 or a negative term_err_t
 * term.recordScreen(id)  -> 0 or a negative term_err_t
 *
 * THE ONE-ARGUMENT FORM IS A QUERY, and it is the only introspection point
 * for recording state (term_registry.h, R1). A UI that draws a "REC" badge
 * must read it from here every time it repaints rather than caching a
 * boolean of its own: the platform clears recording at half a dozen
 * lifecycle transitions (re-attach, detach, pipe bind, session end, close),
 * and a cached copy is how a badge ends up claiming a session is being
 * recorded when it is not.
 *
 * THERE IS NO CREATE-TIME OPTION on purpose, and adding one would break the
 * user's decision of 2026-07-30, not just its spirit: a field on
 * term.create's config object is a field an app puts in its saved-tab record
 * in `store` and replays after a reboot. The only way recording is ever on is
 * a human turning it on during that live session.
 */
JSValue js_term_record(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    const char *owner = term_owner();
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (!owner || !term_registry_ready())
        return term_err_value(ctx, TERM_ERR_NOT_READY);
    if (argc < 2 || JS_IsUndefined(argv[1]))
        return JS_NewInt32(ctx, term_registry_recording((term_id_t)id, owner));
    return term_err_value(ctx,
                          term_registry_record((term_id_t)id, owner,
                                               uiw_truthy(ctx, argv[1])));
}

JSValue js_term_recordScreen(JSContext *ctx, JSValue *this_val, int argc,
                             JSValue *argv)
{
    (void)this_val;
    (void)argc;
    const char *owner = term_owner();
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (!owner || !term_registry_ready())
        return term_err_value(ctx, TERM_ERR_NOT_READY);
    return term_err_value(ctx,
                          term_registry_record_screen((term_id_t)id, owner));
}

/* term.close(id) -> 0 or a negative term_err_t. Stage 1 only: the id is
   dead to the caller the moment this returns, and nothing is joined
   (§3.1) — the reaper frees the memory once the acks are in. */
JSValue js_term_close(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    const char *owner = term_owner();
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]))
        return JS_EXCEPTION;
    if (!owner || !term_registry_ready())
        return term_err_value(ctx, TERM_ERR_NOT_READY);
    return term_err_value(ctx, term_registry_close((term_id_t)id, owner));
}

/* sys.blackbox([what[, from]]) -> the LP SRAM black box (docs/term-design.md
   §4.4), as a JSON string. See term_lp_ring.h for both shapes.

     sys.blackbox()               stats: presence, boot_seq, this boot's reset
                                  cause, the retention verdict, per-partition
                                  byte/record counters. NO content and no
                                  writer names, so any app may ask — it is
                                  telemetry of the same kind as sys.heap().
     sys.blackbox("lastboot")     the frozen previous session's log tail
     sys.blackbox("live")         this session's tail so far
     sys.blackbox(what, from)     skip `from` records; the reply's "next" is
                                  the cursor for the following chunk

   The two content reads are PRIVILEGED (§7.2: the black box is platform data
   and mixes every app's lines, so no ordinary app may read it). The gate is
   the dev slot or an embedded system app — the dev slot arrives by the same
   Ed25519-signed push as the MQTT responder's requests, i.e. the device
   owner's key, which is precisely §7.2's gate for this data. Everyone else
   gets a TypeError rather than a value, because a silent empty answer would
   look like an empty black box.

   The MQTT responder of §7.2 is the other consumer of the same C read API;
   it does not go through this binding. */
JSValue js_sys_blackbox(JSContext *ctx, JSValue *this_val, int argc,
                        JSValue *argv)
{
    (void)this_val;
    char what[16];
    int from = 0;
    size_t cap, len;
    char *buf;
    JSValue v;

    what[0] = '\0';
    if (argc >= 1 && uiw_copy_str(ctx, argv[0], what, sizeof what))
        return JS_EXCEPTION;
    if (argc >= 2 && JS_ToInt32(ctx, &from, argv[1]))
        return JS_EXCEPTION;
    if (from < 0)
        from = 0;

    if (what[0]) {
        bool last = !strcmp(what, "lastboot");
        if (!last && strcmp(what, "live")) {
            JS_ThrowTypeError(ctx, "blackbox: expected \"live\" or \"lastboot\"");
            return JS_EXCEPTION;
        }
        if (!s_cur_wk ||
            !(s_cur_wk->idx == MQJS_WORKER_DEV || s_cur_wk->trusted_system)) {
            JS_ThrowTypeError(ctx, "blackbox content requires the dev slot "
                                   "or an embedded system app");
            return JS_EXCEPTION;
        }
        cap = TERM_LP_DUMP_MAX;
        /* Off the JS heap: kilobytes of report should not move an app's
           arena, and the buffer is gone before JS_NewStringLen can GC. */
        buf = malloc(cap);
        if (!buf)
            return JS_NULL;
        len = term_lp_dump_json(last ? TERM_LP_SRC_LASTBOOT
                                     : TERM_LP_SRC_LIVE,
                               (uint32_t)from, buf, cap);
    } else {
        cap = TERM_LP_REPORT_MAX;
        buf = malloc(cap);
        if (!buf)
            return JS_NULL;
        len = term_lp_report(buf, cap);
    }
    if (!len) {
        free(buf);
        return JS_NULL;
    }
    v = JS_NewStringLen(ctx, buf, len);
    free(buf);
    return v;
}

/* sys.panic() -> 0 when the fault is armed, -1 off-device.

   The black box's DRILL BUTTON, not an app API: it deliberately crashes the
   device so that §2a's panic note and §3's `lastboot` pull can be exercised
   on demand (PHASE3_MANIFEST.md §3's device plan, step 4). Before this
   existed the only way to test the flight recorder's most important record
   was to wait for a real bug.

   Gated exactly like sys.blackbox's content reads — the dev slot or an
   embedded system app (§7.2) — which is the device owner's Ed25519-signed
   push. Anything else gets a TypeError, the same convention: a binding that
   silently did nothing would look like a black box that failed to record.

   WHY A REAL FAULT, NOT abort(). Both reach __wrap_esp_panic_handler
   (main/panic_note.c handles `abort` explicitly, via g_panic_abort), so
   either would produce a note. A fault is chosen because only it carries an
   mepc and an mcause, and the device-side cross-check §2a asks for is
   precisely "the note's pc= matches the address in the serial Guru
   Meditation block" — the same fact from two independent paths. A volatile
   NULL read is also the exact trigger §1's cause table measured (cause 0,
   full retention), so the drill exercises a path with known-good results.

   WHY IT IS DELAYED. The fault is armed on the esp_timer task 500 ms out and
   this binding returns immediately, so the caller can finish the MQTT
   publish that says the drill is about to fire and let it leave the device
   before the chip dies. Not a tuning knob: shorten it and the acknowledging
   publish is lost, which is the one thing that distinguishes a deliberate
   drill from a real crash in the operator's log. A second call inside the
   window is a no-op (the timer is already armed), not a second fault. */
#ifdef ESP_PLATFORM
/* Both `volatile`: the pointer so the compiler cannot fold a known-NULL
   dereference into an unreachable trap, the pointee (and the sink) so the
   load itself cannot be elided. What the CPU executes is a real 32-bit load
   from address 0 -> LoadProhibited -> esp_panic_handler -> the note. */
static volatile uint32_t *volatile s_drill_addr;
static volatile uint32_t s_drill_sink;

static void panic_drill_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "panic drill: faulting now (sys.panic)");
    s_drill_sink = *s_drill_addr;
}
#endif

JSValue js_sys_panic(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;

    if (!s_cur_wk ||
        !(s_cur_wk->idx == MQJS_WORKER_DEV || s_cur_wk->trusted_system)) {
        JS_ThrowTypeError(ctx, "panic requires the dev slot or an embedded "
                               "system app");
        return JS_EXCEPTION;
    }
#ifdef ESP_PLATFORM
    {
        static esp_timer_handle_t s_panic_timer;
        if (!s_panic_timer) {
            const esp_timer_create_args_t a = {
                .callback = panic_drill_cb,
                .name = "panic_drill",
            };
            if (esp_timer_create(&a, &s_panic_timer) != ESP_OK) {
                s_panic_timer = NULL;
                return JS_NewInt32(ctx, -1);
            }
        }
        if (!esp_timer_is_active(s_panic_timer))
            esp_timer_start_once(s_panic_timer, 500 * 1000);
        return JS_NewInt32(ctx, 0);
    }
#else
    /* run_pc: the gate is real, the fault is not. A host binary that faulted
       here would take the PC test runner down with it, and there is no panic
       handler and no LP SRAM to record into anyway — so the honest answer is
       "unsupported", -1, and the caller's own logic is still testable. */
    return JS_NewInt32(ctx, -1);
#endif
}

/* ------------------------------------------------------------------ */
/* scheduler (design §3.7)                                             */
/* ------------------------------------------------------------------ */

/* generated by the host tool from device_stdlib.c; references the
   binding functions above, so it must be included after them */
#include "device_stdlib.h"

/* Does this app still have a reason to live? (§3.2: an app with no
   timers, handlers or sessions is reaped.) */
static bool anything_pending(const MqjsWorker *app)
{
    for (int i = 0; i < MQJS_MAX_TIMERS; i++)
        if (app->timers[i].used)
            return true;
    for (int i = 0; i < MQJS_MAX_GPIO_CB; i++)
        if (app->gpio_cb[i].used)
            return true;
    if (app->touch_used || app->key_used)
        return true;
    if (app->fg_used || app->bg_used || app->sig_used || app->clip_used)
        return true; /* lifecycle/signal/clipboard sinks keep the app
                        alive (§3.8: "sleep until something happens") */
    if (app->cam_used)
        return true; /* a camera.scan in flight: its result must land */
    if (app->http_used)
        return true; /* an http.get in flight: its result must land */
    if (app->net_used)
        return true; /* parked in the net.onReady wait queue: keep it alive
                        until the link comes up (§3.8) */
    for (int i = 0; i < MQJS_MAX_WIDGET_CB; i++)
        if (app->widget_cbs[i].used) /* a live widget screen, too */
            return true;
#ifdef ESP_PLATFORM
    if (app->mqtt)   /* active mqtt session keeps the app alive */
        return true;
    for (int i = 0; i < (int)(sizeof s_ssh_owner / sizeof s_ssh_owner[0]); i++)
        if (s_ssh_owner[i].used && s_ssh_owner[i].worker == app->idx)
            return true; /* so does an ssh session it owns */
#endif
    return false;
}

/* Release everything one app holds (per-app version of the old
   reset_slots). The shared event queue is NOT drained: in-flight
   events of this app die in the dispatcher (slot+gen check). */
static void app_reset_bindings(MqjsWorker *app)
{
    JSContext *ctx = app->ctx;

    for (int i = 0; i < MQJS_MAX_TIMERS; i++) {
        if (app->timers[i].used) {
            JS_DeleteGCRef(ctx, &app->timers[i].fn);
            app->timers[i].used = false;
        }
    }
    for (int i = 0; i < MQJS_MAX_GPIO_CB; i++) {
        if (app->gpio_cb[i].used) {
#ifdef ESP_PLATFORM
            gpio_isr_handler_remove(app->gpio_cb[i].pin);
#endif
            JS_DeleteGCRef(ctx, &app->gpio_cb[i].fn);
            app->gpio_cb[i].used = false;
        }
    }
#ifdef ESP_PLATFORM
    if (app->mqtt) {
        esp_mqtt_client_stop(app->mqtt);
        esp_mqtt_client_destroy(app->mqtt);
        app->mqtt = NULL;
        app->mqtt_up = false;
    }
    /* close only the ssh sessions THIS app opened (§3.6: shared-resource
       ownership); other apps' sessions stay untouched */
    for (int i = 0; i < (int)(sizeof s_ssh_owner / sizeof s_ssh_owner[0]); i++) {
        if (s_ssh_owner[i].used && s_ssh_owner[i].worker == app->idx) {
            mqjs_ssh_close(s_ssh_owner[i].id);
            s_ssh_owner[i].used = false;
        }
    }
#endif
    /* No skk handles to release any more. NB the dictionary image no
       longer comes back when an app stops: the platform session holds it
       for the rest of the boot (one ime_t per device), and the app-stop
       path below only folds the reading. That is a
       property of the shared session, not something lost with the
       handles — it has been true since the first ui.ime(1) landed. */
    /* §3.1's teardown hook, in the same sweep as the widget retain
       stack: this app's non-persist terms enter stage 1, its persist
       ones go DETACHED and wait to be re-attached (or LRU-evicted). */
    if (app->name[0])
        term_registry_owner_stopped(app->name);
    for (int i = 0; i < MQJS_MAX_MQTT_SUB; i++) {
        if (app->mqtt_subs[i].used) {
            JS_DeleteGCRef(ctx, &app->mqtt_subs[i].fn);
            app->mqtt_subs[i].used = false;
        }
    }
    if (app->mqtt_onconn_used) {
        JS_DeleteGCRef(ctx, &app->mqtt_onconn);
        app->mqtt_onconn_used = false;
    }
    if (app->touch_used) {
        app->touch_used = false; /* before the GCRef dies: gates the poster */
        JS_DeleteGCRef(ctx, &app->touch_cb);
    }
    if (app->key_used) {
        app->key_used = false;   /* ditto */
        JS_DeleteGCRef(ctx, &app->key_cb);
    }
    if (app->ime_used) {
        app->ime_used = false;   /* ditto */
        ime_disarm();            /* 読みかけを次のアプリへ持ち越さない (§4-7) */
    }
#ifdef ESP_PLATFORM
    /* field 経由で IME を使ったアプリは ime_used が立たないので、上の枝では
       拾えない (I3)。 */
    if (s_ime_field >= 0) {
        s_ime_field = -1;      /* 先に落とす: poster を止めてから畳む */
        ime_disarm();          /* 読みかけを次のアプリへ持ち越さない */
    }
#endif
    if (app->fg_used) {
        JS_DeleteGCRef(ctx, &app->fg_cb);
        app->fg_used = false;
    }
    if (app->bg_used) {
        JS_DeleteGCRef(ctx, &app->bg_cb);
        app->bg_used = false;
    }
    if (app->sig_used) {
        JS_DeleteGCRef(ctx, &app->sig_cb);
        app->sig_used = false;
    }
    if (app->stop_used) {
        JS_DeleteGCRef(ctx, &app->stop_cb);
        app->stop_used = false;
    }
    if (app->clip_used) {
        JS_DeleteGCRef(ctx, &app->clip_cb);
        app->clip_used = false;
    }
    if (app->cam_used) {
        JS_DeleteGCRef(ctx, &app->cam_cb);
        app->cam_used = false;
    }
    if (app->http_used) {
        JS_DeleteGCRef(ctx, &app->http_cb);
        app->http_used = false;
        /* a request in flight cannot be cancelled mid-flight; its result
           event dies on the gen check and frees its own body (§3.2) */
    }
    if (app->net_used) {
        JS_DeleteGCRef(ctx, &app->net_cb);
        app->net_used = false;
    }
    if (app->fsreq_used) {
        JS_DeleteGCRef(ctx, &app->fsreq_cb);
        app->fsreq_used = false;
    }
    if (app->fsop_used) {
        JS_DeleteGCRef(ctx, &app->fsop_cb);
        app->fsop_used = false;
        /* 走っているフォーマットは止められない。結果イベントは
           世代チェックで捨てられる (§3.2)。 */
    }
#ifdef ESP_PLATFORM
    /* 権限は世代で自然に失効するが (fs_grant_get)、席を空けておく方が
       素直。同意待ちの要求も、返事が来ても行き先が無いので捨てる。 */
    for (int i = 0; i < MQJS_FS_GRANTS; i++) {
        if (s_fs_grants[i].worker == app->idx)
            s_fs_grants[i].token = 0;
        if (s_fs_pending[i].worker == app->idx)
            s_fs_pending[i].id = 0;
    }
#endif
#if defined(ESP_PLATFORM) && CONFIG_MQJS_CAMERA
    if (s_cam_active && s_cam_worker == app->idx)
        cam_tab5_cancel(); /* its result event dies on the gen check */
#endif
    for (int i = 0; i < MQJS_MAX_SSH_CB; i++) {
        if (app->ssh_cbs[i].used)
            sshcb_release(ctx, &app->ssh_cbs[i]);
    }
    /* term.onReply sinks. The registry side needs no unhooking: the
       terms themselves have just gone DYING or DETACHED in the sweep
       above, and a reply that races us in dies on the generation check
       in event_owner (§3.2). */
    for (int i = 0; i < MQJS_MAX_TERM_CB; i++) {
        if (app->term_cbs[i].used)
            termcb_release(ctx, &app->term_cbs[i]);
    }
    wcb_release_all(app);

    /* flush a half-assembled console line so it is not attributed to
       whichever app prints next */
    MqjsWorker *prev = s_cur_wk;
    s_cur_wk = app;
    sink_flush();
    s_cur_wk = prev;

    /* the foreground app owned the screen: tear it down. The next
       foreground app rebuilds in its sys.onForeground. */
    if (app->idx == s_fg_worker) {
#ifdef ESP_PLATFORM
        ui_tab5_w_reset();
        ui_cmd_t c = { .op = UI_CMD_RESET };
        ui_tab5_cmd(&c);
#else
        pcw_reset();
#endif
    }
}

static const char *stop_reason_str(mqjs_app_stop_reason_t r)
{
    switch (r) {
    case MQJS_APP_STOP_IDLE:    return "idle";
    case MQJS_APP_STOP_UPDATED: return "updated";
    case MQJS_APP_STOP_EVICTED: return "evicted";
    case MQJS_APP_STOP_ERROR:   return "error";
    case MQJS_APP_STOP_BATTERY: return "battery";
    default:                    return "user";
    }
}

static void app_stop_internal_r(MqjsWorker *app, mqjs_app_stop_reason_t reason)
{
    if (!app->used || app->stopping)
        return;
    app->stopping = true;
    /* last words first (doc lifecycle: app_stop -> onStop -> release ->
       destroy): the app may persist state via the store. Errors are
       dumped and ignored; the watchdog bounds the handler. Anything it
       registers is torn down right below. */
    if (app->stop_used && app->ctx) {
        JSContext *sctx = app->ctx;
        MqjsWorker *prev = s_cur_wk;
        s_cur_wk = app;
        if (JS_StackCheck(sctx, 3)) {
            dump_error(sctx);
        } else {
            JS_PushArg(sctx, JS_NewString(sctx, stop_reason_str(reason)));
            JS_PushArg(sctx, app->stop_cb.val);
            JS_PushArg(sctx, JS_NULL);
            arm_watchdog();
            JSValue ret = JS_Call(sctx, 1);
            if (JS_IsException(ret))
                dump_error(sctx);
        }
        s_cur_wk = prev;
    }
    /* a dying foreground app becomes the chip target: "tap to bring it
       back" survives the stop (design §4: open = focus-or-relaunch) */
    if (app->idx == s_fg_worker)
        snprintf(s_prev_name, sizeof s_prev_name, "%s", app->name);
    app_reset_bindings(app);
    JS_FreeContext(app->ctx);  /* runs user-object finalizers */
    app->ctx = NULL;
    app->used = false;
    app->kill_req = false;
    free(app->src_owned);      /* sys.launch file source, if any */
    app->src_owned = NULL;
    /* the App record outlives the worker: state -> STOPPED (Phase 2) */
    mqjs_app_record_on_stop(app->name, reason);
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "app '%s' (worker %d) stopped (%s)", app->name, app->idx,
             stop_reason_str(reason));
#endif
    app->stopping = false;
    bar_update();
}

static void app_stop_internal(MqjsWorker *app)
{
    app_stop_internal_r(app, MQJS_APP_STOP_USER);
}

static int app_start_internal(MqjsWorker *app, const char *src, size_t src_len,
                              const char *name, bool trusted_system)
{
    if (app->used || !app->mem)
        return -1;

    app->gen++; /* stale events of the previous occupant die now */
    app->kill_req = false;
    memset(app->timers, 0, sizeof app->timers);
    memset(app->gpio_cb, 0, sizeof app->gpio_cb);
    memset(app->mqtt_subs, 0, sizeof app->mqtt_subs);
    memset(app->widget_cbs, 0, sizeof app->widget_cbs);
    memset(app->ssh_cbs, 0, sizeof app->ssh_cbs);
    app->mqtt_onconn_used = false;
    app->touch_used = false;
    app->key_used = false;
    app->ime_used = false;
    app->fg_used = app->bg_used = app->sig_used = false;
    app->stop_used = false;
    app->stopping = false;
    app->clip_used = false;
    app->net_used = false;
    app->sink_len = 0;
    snprintf(app->name, sizeof app->name, "%s", name ? name : "app");
    snprintf(app->vault_id, sizeof app->vault_id, "%s", name ? name : "app");
    app->trusted_system = trusted_system;

    JSContext *ctx = JS_NewContext(app->mem, app->mem_size, &js_stdlib);
    if (!ctx)
        return -1;
    app->ctx = ctx;
    app->used = true;
    JS_SetLogFunc(ctx, js_log_func);
    JS_SetInterruptHandler(ctx, js_interrupt_handler);

    /* compile + run the top level (registers callbacks) */
    MqjsWorker *prev = s_cur_wk;
    s_cur_wk = app;
    arm_watchdog();
    bool failed = false;
    if (JS_IsBytecode((const uint8_t *)src, src_len)) {
        /* precompiled task: the buffer is patched in place and stays
           referenced for the whole context lifetime, so it must live in
           RAM (heap), never in flash. Trusted-source only: the loader
           does not validate bytecode (Ed25519 gate in task_source.c).
           Re-relocating the same buffer on a later run is a no-op. */
        if (JS_RelocateBytecode(ctx, (uint8_t *)src, (uint32_t)src_len)) {
            printf("mqjs: bytecode relocation failed\n");
            failed = true;
        } else {
            JSValue val = JS_LoadBytecode(ctx, (const uint8_t *)src);
            if (!JS_IsException(val))
                val = JS_Run(ctx, val);
            if (JS_IsException(val)) {
                dump_error(ctx);
                failed = true;
            }
        }
    } else {
        JSValue val = JS_Eval(ctx, src, src_len, name ? name : "<task>", 0);
        if (JS_IsException(val)) {
            dump_error(ctx);
            failed = true;
        }
    }
    s_cur_wk = prev;

    if (failed) {
        app_stop_internal_r(app, MQJS_APP_STOP_ERROR);
        return -1;
    }
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "app '%s' started in worker %d", app->name, app->idx);
#endif
    if (app->idx == MQJS_WORKER_DEV)
        snprintf(s_last_dev_name, sizeof s_last_dev_name, "%s", app->name);
    /* App record: find-or-create, state -> RUNNING (Phase 2). */
    mqjs_app_record_on_start(app->name, app->idx,
                             trusted_system ? MQJS_APP_KIND_SYSTEM
                                            : MQJS_APP_KIND_APP,
                             time_ms());
    /* Only the launcher is resident. Other embedded system screens are
       trusted but launch-on-demand and may release their worker. */
    if (trusted_system && app->idx != MQJS_WORKER_LAUNCHER)
        mqjs_app_record_set_policy(
            app->name, MQJS_APP_EVICTABLE | MQJS_APP_STOPPABLE,
            MQJS_APP_AUTOSTART | MQJS_APP_RESTART_ON_EXIT |
                MQJS_APP_KEEP_ALIVE);
    /* Phase 3: the dev task's classic natural-end auto-rerun is policy
       now — every (re)start arms it; an explicit sys.stop clears it. */
    if (app->idx == MQJS_WORKER_DEV)
        mqjs_app_record_set_policy(app->name, MQJS_APP_RESTART_ON_EXIT, 0);
    bar_update();
    return 0;
}

/* Foreground switch protocol (§3.3), synchronous on the JS task so the
   order background-cb -> teardown -> foreground-cb cannot interleave
   with other dispatches. */
static void switch_foreground(int new_slot)
{
    if (new_slot < 0 || new_slot >= MQJS_MAX_WORKERS || new_slot == s_fg_worker ||
        !s_workers[new_slot].used)
        return;

    MqjsWorker *old = &s_workers[s_fg_worker];
    if (old->used) {
        if (old->bg_used)
            app_call0(old, &old->bg_cb); /* may snapshot UI state */
        /* destroy the outgoing app's screen state (§3.3 / decision 3) */
        wcb_release_all(old);
#ifdef ESP_PLATFORM
        ui_tab5_w_reset();
        ui_cmd_t c = { .op = UI_CMD_RESET };
        ui_tab5_cmd(&c);
#else
        pcw_reset();
#endif
        /* the outgoing app becomes the status-bar chip target */
        snprintf(s_prev_name, sizeof s_prev_name, "%s", old->name);
        mqjs_app_record_set_view(old->name, MQJS_APP_VIEW_BACKGROUND);
    }

    s_fg_worker = new_slot;
    MqjsWorker *nw = &s_workers[new_slot];
    mqjs_app_record_set_view(nw->name, MQJS_APP_VIEW_FOREGROUND);
    mqjs_app_record_touch(nw->name, time_ms());
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "foreground -> '%s' (worker %d)", nw->name, new_slot);
#else
    printf("[sys] foreground -> '%s' (worker %d)\n", nw->name, new_slot);
#endif
    bar_update();
    if (nw->fg_used)
        app_call0(nw, &nw->fg_cb);   /* the app rebuilds its UI here */
    ui_tab5_w_commit();              /* §3.4: anim only after the rebuild */
}

static void focus_apply(int target)
{
    if (target < MQJS_MAX_WORKERS)
        switch_foreground(target);
}

/* EV_NET broadcast: release every app parked in the net.onReady wait queue.
   Runs on the JS task, so iterating the workers and calling into their
   contexts is safe (the poster only enqueued the broadcast). One-shot: the
   ticket is cleared before the call so the handler may re-arm for the next
   bring-up, exactly like the http.get / camera.scan one-shots. */
static void net_ready_broadcast(void)
{
    MqjsWorker *prev = s_cur_wk;
    for (int i = 0; i < MQJS_MAX_WORKERS; i++) {
        MqjsWorker *app = &s_workers[i];
        if (!app->used || !app->net_used)
            continue;
        s_cur_wk = app;
        if (JS_StackCheck(app->ctx, 2)) {
            dump_error(app->ctx);
            app->net_used = false;
            JS_DeleteGCRef(app->ctx, &app->net_cb);
            continue;
        }
        JS_PushArg(app->ctx, JS_NewInt32(app->ctx, (int)s_net_token)); /* arg0: token */
        JS_PushArg(app->ctx, app->net_cb.val);  /* func (rooted on arg stack) */
        JS_PushArg(app->ctx, JS_NULL);          /* this */
        app->net_used = false;                  /* release before the call so */
        JS_DeleteGCRef(app->ctx, &app->net_cb); /* the cb may re-arm onReady  */
        arm_watchdog();
        JSValue ret = JS_Call(app->ctx, 1);
        if (JS_IsException(ret))
            dump_error(app->ctx);
    }
    s_cur_wk = prev;
}

/* drop an event without dispatching: free its heap payload */
static void free_event_payload(MqjsEvent *ev)
{
    switch (ev->type) {
    case EV_MQTT_DATA:
        free(ev->u.mqtt.topic);
        free(ev->u.mqtt.payload);
        break;
    case EV_SSH_DATA:
        free(ev->u.ssh.data);
        break;
    case EV_SIGNAL:
        free(ev->u.signal.value);
        break;
    case EV_HTTP:
        free(ev->u.http.body);
        break;
    case EV_CAM:
        free(ev->u.cam.text);
        break;
    default:
        break;
    }
}

/* §3.2 routing table: which app owns this event? NULL = drop it.
   Slot-addressed events also check the generation (stale = drop). */
static MqjsWorker *event_owner(const MqjsEvent *ev)
{
    switch (ev->type) {
    case EV_GPIO:
        for (int a = 0; a < MQJS_MAX_WORKERS; a++) {
            if (!s_workers[a].used)
                continue;
            for (int i = 0; i < MQJS_MAX_GPIO_CB; i++)
                if (s_workers[a].gpio_cb[i].used &&
                    s_workers[a].gpio_cb[i].pin == ev->u.gpio.pin)
                    return &s_workers[a];
        }
        return NULL;
    case EV_MQTT_CONNECTED:
    case EV_MQTT_DATA:
    case EV_SIGNAL:
    case EV_CLIP:
    case EV_CAM:
    case EV_HTTP:
    case EV_FSGRANT:
    case EV_FSOP:
    case EV_TERM_REPLY: {
        MqjsWorker *app = &s_workers[ev->worker];
        return (app->used && app->gen == ev->gen) ? app : NULL;
    }
#ifdef ESP_PLATFORM
    case EV_SSH_DATA:
    case EV_SSH_CLOSED: {
        int id = ev->type == EV_SSH_DATA ? ev->u.ssh.id : ev->u.ssh_closed.id;
        SshOwner *o = ssh_owner_find(id);
        return (o && s_workers[o->worker].used) ? &s_workers[o->worker] : NULL;
    }
#endif
    case EV_TOUCH:
    case EV_KEY:
    case EV_WIDGET: {
        MqjsWorker *fg = &s_workers[s_fg_worker]; /* input is foreground-only */
        return fg->used ? fg : NULL;
    }
    default:
        return NULL;
    }
}

static void dispatch_event(MqjsWorker *app, MqjsEvent *ev)
{
    MqjsWorker *prev = s_cur_wk;
    s_cur_wk = app;
    switch (ev->type) {
    case EV_GPIO:           dispatch_gpio_event(app, ev);    break;
    case EV_MQTT_CONNECTED: dispatch_mqtt_connected(app);    break;
    case EV_MQTT_DATA:      dispatch_mqtt_data(app, ev);     break;
    case EV_TOUCH:          dispatch_touch_event(app, ev);   break;
    case EV_KEY:            dispatch_key_event(app, ev);     break;
    case EV_SSH_DATA:       dispatch_ssh_data(app, ev);      break;
    case EV_SSH_CLOSED:     dispatch_ssh_closed(app, ev);    break;
    case EV_WIDGET:         dispatch_widget_event(app, ev);  break;
    case EV_SIGNAL:         dispatch_signal(app, ev);        break;
    case EV_CLIP:           dispatch_clip(app, ev);          break;
    case EV_CAM:            dispatch_cam(app, ev);           break;
    case EV_FSGRANT:        dispatch_fsgrant(app, ev);       break;
    case EV_FSOP:           dispatch_fsop(app, ev);          break;
    case EV_HTTP:           dispatch_http(app, ev);          break;
    case EV_TERM_REPLY:     dispatch_term_reply(app, ev);    break;
    }
    s_cur_wk = prev;
}

/* receive + dispatch one event, waiting up to `idle` ms. Returns true
   when an event was handled. */
static bool pump_one_event(int idle)
{
    MqjsEvent ev;
#ifdef ESP_PLATFORM
    if (!s_event_queue ||
        xQueueReceive(s_event_queue, &ev, pdMS_TO_TICKS(idle)) != pdTRUE)
        return false;
#else
    if (!pc_q_recv(&ev)) {
        if (idle > 0)
            usleep((useconds_t)idle * 1000);
        return false;
    }
#endif
    if (ev.type == EV_FOCUS) {
        focus_apply(ev.u.focus.target);
        return true;
    }
    if (ev.type == EV_NET) {   /* broadcast: not owned by one slot */
        net_ready_broadcast();
        return true;
    }
    MqjsWorker *owner = event_owner(&ev);
    if (!owner)
        free_event_payload(&ev); /* dead-slot leftovers: drop (§3.2) */
    else
        dispatch_event(owner, &ev);
    return true;
}

/* Reap apps that are done: nothing pending, a deferred sys.stop
   (kill_req), or a dev slot whose replacement was requested. The
   foreground falls back to the launcher when its app went away —
   except a dev natural end, which auto-reruns and keeps the screen. */
static void reap_idle_apps(void)
{
    for (int i = 0; i < MQJS_MAX_WORKERS; i++) {
        MqjsWorker *app = &s_workers[i];
        if (!app->used)
            continue;
        bool stop = app->kill_req ||
                    (i == MQJS_WORKER_DEV && s_stop_req) ||
                    !anything_pending(app);
        if (!stop)
            continue;
        /* Phase 4: the stop reason reaches onStop + the App record */
        mqjs_app_stop_reason_t reason =
            app->kill_req ? MQJS_APP_STOP_USER
            : (i == MQJS_WORKER_DEV && s_stop_req) ? MQJS_APP_STOP_UPDATED
            : MQJS_APP_STOP_IDLE;
        bool was_fg = (i == s_fg_worker);
        app_stop_internal_r(app, reason);
        if (i == MQJS_WORKER_DEV) {
            /* push-replace = ask the provider right away; natural end =
               the classic 1s-rerun (unless an explicit stop holds it) */
            s_dev_retry_at = s_stop_req ? 0 : time_ms() + MQJS_DEV_RESTART_MS;
            s_stop_req = false;
        }
        if (was_fg && s_workers[MQJS_WORKER_LAUNCHER].used &&
            (i != MQJS_WORKER_DEV || dev_held()))
            switch_foreground(MQJS_WORKER_LAUNCHER);
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void mqjs_rt_init(void)
{
    mqjs_apps_init(); /* App record table (manager Phase 2) */
    for (int i = 0; i < MQJS_MAX_WORKERS; i++)
        s_workers[i].idx = (uint8_t)i;
#ifdef ESP_PLATFORM
    if (!s_event_queue)
        s_event_queue = xQueueCreate(MQJS_QUEUE_LEN, sizeof(MqjsEvent));
    if (!s_clip_mtx)
        s_clip_mtx = xSemaphoreCreateMutex();
    clip_load(); /* eager: the T3c panel peeks before any app touches it */
    /* fixed arenas, allocated once and kept (design §3.6): app_start can
       never fail with OOM and the PSRAM heap is not churned */
    for (int i = 0; i < MQJS_MAX_WORKERS; i++) {
        if (s_workers[i].mem)
            continue;
        size_t asz = MQJS_APP_MEM_SIZE;
        uint8_t *m = NULL;
#if CONFIG_MQJS_DEV_ARENA_KB > 0
        /* A/B probe (MQJS_DEV_ARENA_KB / _INTERNAL): dev slot only */
        if (i == MQJS_WORKER_DEV) {
            asz = (size_t)CONFIG_MQJS_DEV_ARENA_KB * 1024;
#ifdef CONFIG_MQJS_DEV_ARENA_INTERNAL
            m = heap_caps_malloc(asz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!m)
                ESP_LOGW(TAG, "dev arena: internal %u KB failed, PSRAM "
                         "fallback", (unsigned)(asz / 1024));
#endif
            ESP_LOGW(TAG, "dev arena probe: %u KB, %s", (unsigned)(asz / 1024),
                     m ? "internal L2" : "PSRAM");
        }
#endif
        if (!m)
            m = heap_caps_malloc(asz, MALLOC_CAP_SPIRAM);
        if (!m) {
            ESP_LOGW(TAG, "PSRAM arena alloc failed for slot %d, trying "
                     "internal RAM", i);
            m = malloc(asz);
        }
        if (!m) {
            ESP_LOGE(TAG, "no arena for app slot %d", i);
            continue;
        }
        s_workers[i].mem = m;
        s_workers[i].mem_size = asz;
    }
#endif
}

int mqjs_app_start(int slot, const char *src, size_t src_len,
                   const char *name)
{
    if (slot < 0 || slot >= MQJS_MAX_WORKERS)
        return -1;
    s_workers[slot].idx = (uint8_t)slot;
    if (!app_ensure_mem(&s_workers[slot]))
        return -1;
    return app_start_internal(&s_workers[slot], src, src_len, name, false);
}

void mqjs_app_stop(int slot)
{
    if (slot < 0 || slot >= MQJS_MAX_WORKERS)
        return;
    app_stop_internal(&s_workers[slot]);
}

bool mqjs_app_running(int slot)
{
    return slot >= 0 && slot < MQJS_MAX_WORKERS && s_workers[slot].used;
}

void mqjs_focus(int slot)
{
    if (slot < 0 || slot >= MQJS_MAX_WORKERS)
        return;
    MqjsEvent ev = { .type = EV_FOCUS };
    ev.u.focus.target = (uint8_t)slot;
    ev_post(&ev, 0);
}

void mqjs_runtime_stop(void)
{
    s_stop_req = true;
}

void mqjs_request_stop_all(void (*done)(void))
{
    s_stopall_done = done;
    s_stopall_req = true;
}

#ifdef ESP_PLATFORM
/* Boot pass of the @autostart roster (design §8): start every
   installed app that (a) the user opted in by launching it locally
   once and (b) STILL declares "// @autostart" (an update that drops
   the directive stops the boot launch without touching the roster).
   Runs once before the scheduler loop; failures and slot overflow are
   log/notice only — boot is never blocked. Order = directory order. */
static void autostart_boot(void)
{
    char list[MQJS_AUTOSTART_LIST_MAX];
    autostart_load(list, sizeof list);
    if (!list[0])
        return;
    DIR *d = opendir("/littlefs/apps");
    if (!d)
        return;
    char started[96];
    size_t so = 0;
    started[0] = '\0';
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l < 4 || l > 34 || strcmp(e->d_name + l - 3, ".js"))
            continue;
        char name[32];
        snprintf(name, sizeof name, "%.*s", (int)(l - 3), e->d_name);
        if (!autostart_list_has(list, name))
            continue;
        char path[96];
        snprintf(path, sizeof path, "/littlefs/apps/%.40s", e->d_name);
        char head[512];
        size_t hlen = 0;
        FILE *f = fopen(path, "rb");
        if (f) {
            hlen = fread(head, 1, sizeof head, f);
            fclose(f);
        }
        if (!manifest_has(head, hlen, "// @autostart"))
            continue;
        int slot = app_free_slot();
        if (slot < 0) {
            ESP_LOGW(TAG, "autostart: no free slot for '%s'", name);
            continue;
        }
        if (start_from_file(slot, name, name) < 0) {
            ESP_LOGW(TAG, "autostart: '%s' failed to start", name);
            continue;
        }
        /* roster member, booted: stamp the policy bit on the record
           (start_from_file's opt-in path is a no-op for known names) */
        mqjs_app_record_set_policy(name, MQJS_APP_AUTOSTART, 0);
        ESP_LOGI(TAG, "autostart: '%s' -> worker %d", name, slot);
        if (so < sizeof started - strlen(name) - 3)
            so += snprintf(started + so, sizeof started - so, "%s%s",
                           so ? ", " : "", name);
    }
    closedir(d);
    if (!started[0])
        return;
    /* visible trace: the per-app notice table (launcher 通知 section)
       + the status bar when its sink is already wired */
    Notice *n = &s_notices[0];
    for (int i = 1; i < (int)(sizeof s_notices / sizeof s_notices[0]); i++)
        if (s_notices[i].seq < n->seq)
            n = &s_notices[i];
    snprintf(n->app, sizeof n->app, "system");
    snprintf(n->text, sizeof n->text, "autostart: %.84s", started);
    n->seq = ++s_notice_seq;
    if (s_notify_sink) {
        char line[128];
        snprintf(line, sizeof line, "[system] autostart: %s", started);
        s_notify_sink(line);
    }
}
#endif /* ESP_PLATFORM */

/* The permanent multi-app loop (§3.7). The dev slot is restarted from
   the provider: immediately after a stop request (task push), 1s after
   a natural end (the pre-P4 auto-rerun behavior), never while an
   explicit sys.stop holds it. The launcher (when a source named
   "launcher" was registered) is kept resident in slot 0. */
void mqjs_runtime_run(mqjs_dev_source_fn next_dev, void *user)
{
    s_dev_retry_at = 0; /* 0 = ask the provider right away */
#ifdef ESP_PLATFORM
    autostart_boot(); /* §8: opted-in resident apps come back at boot */
    mqjs_power_init(time_ms()); /* screen power-state machine */
#endif

    for (;;) {
        /* launcher residency: chrome (chip / open requests) depends on
           it. Phase 3: the restart DECISION is the record's policy
           (RESTART_ON_EXIT, part of the KIND_SYSTEM profile) — clear
           the bit and residency stops; the first boot start (no record
           yet) bootstraps it. Worker 0 stays the system app's pinned
           execution frame (an allocation rule, not policy). */
        if (!s_shutting_down && !s_workers[MQJS_WORKER_LAUNCHER].used &&
            time_ms() >= s_launcher_retry_at) {
            const mqjs_app_snapshot_t *lrec = mqjs_app_record_find("launcher");
            if (!lrec || (lrec->policy.flags & MQJS_APP_RESTART_ON_EXIT)) {
                const AppSource *as = app_source_find("launcher");
                if (as)
                    app_start_internal(&s_workers[MQJS_WORKER_LAUNCHER],
                                       as->src, as->len, "launcher", true);
            }
            s_launcher_retry_at = time_ms() + 1000;
        }

        /* a push that arrived while the dev worker was idle or held */
        if (s_stop_req && !s_workers[MQJS_WORKER_DEV].used) {
            s_stop_req = false;
            dev_rearm(); /* a push always reopens a held dev worker */
        }
        if (!s_shutting_down && next_dev &&
            !s_workers[MQJS_WORKER_DEV].used && !dev_held() &&
            time_ms() >= s_dev_retry_at) {
            const char *src = NULL, *name = NULL;
            size_t len = 0;
            s_stop_req = false;
            if (next_dev(&src, &len, &name, user) && src) {
                if (mqjs_app_start(MQJS_WORKER_DEV, src, len, name) != 0)
                    s_dev_retry_at = time_ms() + MQJS_DEV_RESTART_MS;
            } else {
                s_dev_retry_at = time_ms() + MQJS_DEV_RESTART_MS;
            }
        }

        int idle = run_all_timers(50 /* ms */);
        pump_one_event(idle);
        ui_tab5_w_commit(); /* §3.4: start a queued screen-load anim only
                               after this dispatch finished building */
        mqjs_term_pump(); /* no-op on the device: the UI task drains */
        reap_idle_apps();
        if (s_stopall_req) {
            /* Battery shutdown. Every app gets its onStop (the handler is
               watchdog-bounded, so this cannot hang the countdown), the
               restart machinery above is off for good, and the caller is
               told once the last handler has returned. */
            s_stopall_req = false;
            s_shutting_down = true;
            for (int i = 0; i < MQJS_MAX_WORKERS; i++)
                if (s_workers[i].used)
                    app_stop_internal_r(&s_workers[i], MQJS_APP_STOP_BATTERY);
            if (s_stopall_done)
                s_stopall_done();
        }
#ifdef ESP_PLATFORM
        mqjs_power_update(time_ms()); /* dim/blank on idle, wake on touch */
#endif
    }
}

/* Single-app compatibility pump (PC run_pc / test_pc; also keeps the
   pre-P4 embedded API working). Uses the caller's buffer as the dev
   arena and returns when no app has anything pending. */
int mqjs_run_script(const char *src, size_t src_len, const char *name,
                    void *mem_buf, size_t mem_size)
{
    for (int i = 0; i < MQJS_MAX_WORKERS; i++)
        s_workers[i].idx = (uint8_t)i;
#ifdef ESP_PLATFORM
    if (!s_event_queue)
        s_event_queue = xQueueCreate(MQJS_QUEUE_LEN, sizeof(MqjsEvent));
#endif
    MqjsWorker *dev = &s_workers[MQJS_WORKER_DEV];
    if (dev->used)
        return -1;
    dev->mem = mem_buf;
    dev->mem_size = mem_size;

    s_stop_req = false;
    if (app_start_internal(dev, src, src_len, name, false))
        return -1;

    for (;;) {
        bool any = false;
        for (int i = 0; i < MQJS_MAX_WORKERS; i++)
            any |= s_workers[i].used;
        if (!any || s_stop_req)
            break;
        int idle = run_all_timers(50 /* ms */);
        pump_one_event(idle);
        ui_tab5_w_commit();
        mqjs_term_pump();
        for (int i = 0; i < MQJS_MAX_WORKERS; i++) {
            MqjsWorker *app = &s_workers[i];
            if (app->used && (app->kill_req || !anything_pending(app)))
                app_stop_internal(app);
        }
    }
    for (int i = 0; i < MQJS_MAX_WORKERS; i++)
        app_stop_internal(&s_workers[i]);
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "task '%s' finished", name ? name : "<task>");
#endif
    return 0;
}
