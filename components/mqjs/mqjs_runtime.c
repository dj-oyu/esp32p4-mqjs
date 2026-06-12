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
#include "app/mqjs_app_manager_internal.h"

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
#include <dirent.h>
#include "mqtt_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ui_tab5.h"
#include "sshc.h"
#include "cam_tab5.h"
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
               EV_FOCUS, EV_CLIP, EV_CAM, EV_HTTP } MqjsEventType;

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
        struct { char text[8]; uint8_t len; } key; /* one key as UTF-8 */
        struct { char *data; uint32_t len; int16_t id; } ssh; /* heap rx */
        struct { char reason[84]; int16_t id; } ssh_closed;
        struct { uint32_t handle; int32_t value; } widget; /* tap/change */
        struct { char *value; char from[32]; } signal; /* sys.signal;
                       from[] sized to MqjsWorker.name */
        struct { uint8_t target; } focus;
        struct { char code[14]; uint8_t ok; } cam; /* camera.scan result
                       (13 digits inline: no heap ownership to manage) */
        struct { char *body; uint32_t len; int16_t status; } http; /* http.get
                       result: heap body owned by the event (dispatcher
                       frees it), status<=0 = request failed */
    } u;
} MqjsEvent;

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
    JSGCRef data_fn, close_fn;
} SshCb;

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
    volatile bool touch_used; /* read by the UI task (poster) */
    JSGCRef   touch_cb;
    volatile bool key_used;   /* read by the UI task (poster) */
    JSGCRef   key_cb;
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
static int64_t s_run_deadline;   /* JS watchdog */

/* P4b: relaunchable app sources (embedded buffers, live forever).
   sys.launch(name) resolves here first; "launcher" is kept resident. */
typedef struct {
    bool used;
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
static void (*s_uninstall_hook)(const char *name);

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
static void sink_flush(void)
{
    MqjsWorker *app = s_cur_wk;
    char *line = app ? app->sink_line : s_orphan_line;
    size_t *plen = app ? &app->sink_len : &s_orphan_len;
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
    if (!s_print_sink)
        return;
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
        if (i2c_master_probe(s_i2c_bus[port], addr, 20) == ESP_OK)
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
    UI_CMD_CELLS, UI_CMD_SCROLL, UI_CMD_RESET,
} ui_cmd_op_t;
/* PC stub of the deferred screen-load commit (§3.4) */
#define ui_tab5_w_commit() ((void)0)
#endif

/* Is the calling app allowed to touch the screen? (No app on the C
   stack = runtime-internal call: allowed.) */
static bool ui_is_fg(void)
{
    return !s_cur_wk || s_cur_wk->idx == s_fg_worker;
}

/* Post one drawing command. Takes ownership of `text` (heap copy) in
   every outcome; drops are counted on-screen by the UI itself. `bg` is
   only used by UI_CMD_CELLS (cell background). Background apps: no-op. */
static void ui_post_bg(uint8_t op, int x, int y, int w, int h,
                       uint32_t color, uint32_t bg, char *text)
{
    if (!ui_is_fg()) {
        free(text);
        return;
    }
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
          "cells", "scroll", "reset" };
    printf("[ui] %s(x=%d, y=%d, w=%d, h=%d, fg=0x%06x, bg=0x%06x%s%s) (stub)\n",
           names[op], x, y, w, h, (unsigned)color, (unsigned)bg,
           text ? ", " : "", text ? text : "");
    free(text);
#endif
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

/* ui.cells(col, row, str, fg, bg): draw a monospace run of cells with a
   single fg/bg (the JS terminal splits each row into same-color runs). */
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

void mqjs_post_key(const char *utf8, size_t len)
{
#ifdef ESP_PLATFORM
    MqjsEvent ev = { .type = EV_KEY };
    MqjsWorker *fg = &s_workers[s_fg_worker]; /* keys always go to the fg app */
    if (!s_event_queue || !fg->used || !fg->key_used || !utf8)
        return;
    if (len == 0 || len > sizeof(ev.u.key.text))
        return;
    memcpy(ev.u.key.text, utf8, len);
    ev.u.key.len = (uint8_t)len;
    xQueueSend(s_event_queue, &ev, 0); /* full queue: drop, never block */
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
        if (!JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
            JSValue sec = JS_GetPropertyStr(ctx, argv[1], "secret");
            if (JS_IsException(sec))
                return JS_EXCEPTION;
            a = uiw_truthy(ctx, sec);
        }
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
        snprintf(ev.u.cam.code, sizeof ev.u.cam.code, "%s", code);
        ev.u.cam.ok = 1;
    }
    s_cam_active = false;
    ev_post(&ev, 0);
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
    if (!app->cam_used)
        return;
    if (JS_StackCheck(ctx, 3)) {
        dump_error(ctx);
        return;
    }
    JS_PushArg(ctx, ev->u.cam.ok ? JS_NewString(ctx, ev->u.cam.code)
                                 : JS_UNDEFINED);                /* arg0 */
    JS_PushArg(ctx, app->cam_cb.val);                            /* func */
    JS_PushArg(ctx, JS_NULL);                                    /* this */
    /* one-shot: release before the call (the arg stack roots the fn) so
       the handler can immediately camera.scan() again */
    app->cam_used = false;
    JS_DeleteGCRef(ctx, &app->cam_cb);
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 1);
    if (JS_IsException(ret))
        dump_error(ctx);
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

/* sys.heap() -> [internal_free, psram_free, lvgl_pool_free] bytes.
   The third element matters most for widget churn: the LVGL tlsf pool
   is preallocated from PSRAM (W1-1), so leaks inside it are invisible
   to the OS heap counters. */
JSValue js_sys_heap(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    uint32_t internal = 0, psram = 0, lvgl = 0;
#ifdef ESP_PLATFORM
    internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    lvgl = ui_tab5_lv_mem_free();
#endif
    JSValue arr = JS_NewArray(ctx, 0);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewUint32(ctx, internal));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewUint32(ctx, psram));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewUint32(ctx, lvgl));
    return arr;
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
                              const char *name);
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
        if (!s_app_sources[i].used ||
            !strcmp(s_app_sources[i].name, "launcher"))
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

/* sys.store() -> [{name, title, icon, desc, size, installed}] straight
   from the broker catalog (§11). Catalog-only on purpose: the launcher
   merges it with sys.installed() itself, and a device-side install
   shows up here as installed=true on the next call. */
JSValue js_sys_store(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSGCRef arr_ref;
    JSValue arr = JS_NewArray(ctx, 0);
    if (JS_IsException(arr))
        return arr;
    JS_PUSH_VALUE(ctx, arr);
    int n = 0;
    int cnt = s_store_api ? s_store_api->count() : 0;
    for (int i = 0; i < cnt; i++) {
        char name[25], head[224];
        if (!s_store_api->get(i, name, sizeof name, head, sizeof head))
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
        JS_POP_VALUE(ctx, obj);
        JS_SetPropertyUint32(ctx, arr_ref.val, (uint32_t)n++, obj);
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
    bool ok = s_store_api && name[0] && !strchr(name, '/') &&
              s_store_api->install(name);
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
    if (flen <= 0 || flen > 64 * 1024) {
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
    if (app_start_internal(&s_workers[slot], buf, (size_t)flen, name)) {
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
        if (app_start_internal(&s_workers[slot], as->src, as->len, as->name))
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

static void dispatch_ssh_data(MqjsWorker *app, MqjsEvent *ev)
{
    JSContext *ctx = app->ctx;
    SshCb *c = sshcb_find(app, ev->u.ssh.id);
    if (c && c->data_used) {
        if (JS_StackCheck(ctx, 3)) {
            dump_error(ctx);
        } else {
            JS_PushArg(ctx, JS_NewStringLen(ctx, ev->u.ssh.data,
                                            ev->u.ssh.len));  /* arg0 */
            JS_PushArg(ctx, c->data_fn.val);                  /* func */
            JS_PushArg(ctx, JS_NULL);                         /* this */
            arm_watchdog();
            JSValue ret = JS_Call(ctx, 1);
            if (JS_IsException(ret))
                dump_error(ctx);
        }
    }
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

JSValue js_mqtt_connect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSCStringBuf buf;
    size_t len;
    const char *uri = JS_ToCStringLen(ctx, &len, argv[0], &buf);
    if (!uri)
        return JS_EXCEPTION;

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
#if defined(ESP_PLATFORM) && CONFIG_MQJS_CAMERA
    if (s_cam_active && s_cam_worker == app->idx)
        cam_tab5_cancel(); /* its result event dies on the gen check */
#endif
    for (int i = 0; i < MQJS_MAX_SSH_CB; i++) {
        if (app->ssh_cbs[i].used)
            sshcb_release(ctx, &app->ssh_cbs[i]);
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
                              const char *name)
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
    app->fg_used = app->bg_used = app->sig_used = false;
    app->stop_used = false;
    app->stopping = false;
    app->clip_used = false;
    app->sink_len = 0;
    snprintf(app->name, sizeof app->name, "%s", name ? name : "app");
    snprintf(app->vault_id, sizeof app->vault_id, "%s", name ? name : "app");

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
                             app->idx == MQJS_WORKER_LAUNCHER
                                 ? MQJS_APP_KIND_SYSTEM : MQJS_APP_KIND_APP,
                             time_ms());
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
    case EV_HTTP: {
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
    case EV_HTTP:           dispatch_http(app, ev);          break;
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
        uint8_t *m = heap_caps_malloc(MQJS_APP_MEM_SIZE, MALLOC_CAP_SPIRAM);
        if (!m) {
            ESP_LOGW(TAG, "PSRAM arena alloc failed for slot %d, trying "
                     "internal RAM", i);
            m = malloc(MQJS_APP_MEM_SIZE);
        }
        if (!m) {
            ESP_LOGE(TAG, "no arena for app slot %d", i);
            continue;
        }
        s_workers[i].mem = m;
        s_workers[i].mem_size = MQJS_APP_MEM_SIZE;
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
    return app_start_internal(&s_workers[slot], src, src_len, name);
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
#endif

    for (;;) {
        /* launcher residency: chrome (chip / open requests) depends on
           it. Phase 3: the restart DECISION is the record's policy
           (RESTART_ON_EXIT, part of the KIND_SYSTEM profile) — clear
           the bit and residency stops; the first boot start (no record
           yet) bootstraps it. Worker 0 stays the system app's pinned
           execution frame (an allocation rule, not policy). */
        if (!s_workers[MQJS_WORKER_LAUNCHER].used &&
            time_ms() >= s_launcher_retry_at) {
            const mqjs_app_snapshot_t *lrec = mqjs_app_record_find("launcher");
            if (!lrec || (lrec->policy.flags & MQJS_APP_RESTART_ON_EXIT)) {
                const AppSource *as = app_source_find("launcher");
                if (as)
                    app_start_internal(&s_workers[MQJS_WORKER_LAUNCHER],
                                       as->src, as->len, "launcher");
            }
            s_launcher_retry_at = time_ms() + 1000;
        }

        /* a push that arrived while the dev worker was idle or held */
        if (s_stop_req && !s_workers[MQJS_WORKER_DEV].used) {
            s_stop_req = false;
            dev_rearm(); /* a push always reopens a held dev worker */
        }
        if (next_dev && !s_workers[MQJS_WORKER_DEV].used && !dev_held() &&
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
        reap_idle_apps();
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
    if (app_start_internal(dev, src, src_len, name))
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
