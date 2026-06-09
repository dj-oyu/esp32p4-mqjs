/*
 * mqjs_runtime.c - MicroQuickJS runtime for ESP32-P4 (Stamp-P4)
 *
 * Implements the C side of the device stdlib defined in device_stdlib.c:
 *   - print / console.log
 *   - setTimeout / setInterval / clearTimeout / clearInterval / delay
 *   - gpio.setMode / gpio.write / gpio.read / gpio.onChange
 *   - Date / performance.now
 *
 * Design notes:
 *   - Callbacks are held with JS_AddGCRef (persistent GC reference).
 *     The compacting GC moves objects, so raw JSValue must never be
 *     stored in C; JSGCRef.val is auto-updated by the GC. This is the
 *     same pattern mqjs.c uses for its own setTimeout.
 *   - ISRs NEVER touch the JS context. They only post an event to a
 *     FreeRTOS queue; the host loop (the task that owns the context)
 *     dispatches to JS.
 *   - A JS interrupt handler aborts any single JS run (eval or
 *     callback) that exceeds MQJS_MAX_RUN_MS, so a buggy task cannot
 *     hang the loop.
 *
 * Build with -DESP_PLATFORM (default under ESP-IDF). Without it, a
 * PC stub build is produced for desktop testing (gpio.* print to
 * stdout, onChange registers but never fires).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "cutils.h"
#include "mquickjs.h"
#include "mqjs_runtime.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
static const char *TAG = "mqjs";
#else
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#endif

#define MQJS_MAX_TIMERS    16
#define MQJS_MAX_GPIO_CB   8
#define MQJS_MAX_RUN_MS    5000  /* per JS_Eval / callback watchdog */
#define MQJS_QUEUE_LEN     32

/* ------------------------------------------------------------------ */
/* time / log                                                          */
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

static void js_log_func(void *opaque, const void *buf, size_t buf_len)
{
    fwrite(buf, 1, buf_len, stdout);
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

typedef struct {
    uint8_t pin;
    uint8_t level;
} GpioEvent;

static TimerSlot s_timers[MQJS_MAX_TIMERS];
static GpioSlot  s_gpio_cb[MQJS_MAX_GPIO_CB];
static volatile bool s_stop_req;
static int64_t s_run_deadline;          /* JS watchdog */

#ifdef ESP_PLATFORM
static QueueHandle_t s_gpio_queue;
static bool s_isr_service_installed;
#endif

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
    printf("mqjs: uncaught exception: ");
    JS_PrintValueF(ctx, e, JS_DUMP_LONG);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* print (also used by console.log via the stdlib table)               */
/* ------------------------------------------------------------------ */

JSValue js_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    for (int i = 0; i < argc; i++) {
        if (i != 0)
            putchar(' ');
        JSValue v = argv[i];
        if (JS_IsString(ctx, v)) {
            JSCStringBuf buf;
            size_t len;
            const char *str = JS_ToCStringLen(ctx, &len, v, &buf);
            fwrite(str, 1, len, stdout);
        } else {
            JS_PrintValueF(ctx, v, JS_DUMP_LONG);
        }
    }
    putchar('\n');
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
/* timers                                                              */
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
        TimerSlot *t = &s_timers[i];
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
    if (id >= 0 && id < MQJS_MAX_TIMERS && s_timers[id].used) {
        JS_DeleteGCRef(ctx, &s_timers[id].fn);
        s_timers[id].used = false;
    }
    return JS_UNDEFINED;
}

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

/* Run all expired timers once. Returns the delay in ms until the next
   timer (or `idle_max` if none is pending sooner). */
static int run_timers(JSContext *ctx, int idle_max)
{
    int64_t now = time_ms();
    int min_delay = idle_max;

    for (int i = 0; i < MQJS_MAX_TIMERS; i++) {
        TimerSlot *t = &s_timers[i];
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
/* ISR: post to queue only. NEVER call into JS from here. */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    GpioEvent ev = {
        .pin = (uint8_t)(intptr_t)arg,
        .level = (uint8_t)gpio_get_level((gpio_num_t)(intptr_t)arg),
    };
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_gpio_queue, &ev, &hp);
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

    for (int i = 0; i < MQJS_MAX_GPIO_CB; i++) {
        GpioSlot *g = &s_gpio_cb[i];
        if (g->used && g->pin == pin)
            return JS_ThrowTypeError(ctx, "pin already has a handler");
    }
    for (int i = 0; i < MQJS_MAX_GPIO_CB; i++) {
        GpioSlot *g = &s_gpio_cb[i];
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

static void dispatch_gpio_event(JSContext *ctx, const GpioEvent *ev)
{
    for (int i = 0; i < MQJS_MAX_GPIO_CB; i++) {
        GpioSlot *g = &s_gpio_cb[i];
        if (!g->used || g->pin != ev->pin)
            continue;
        if (JS_StackCheck(ctx, 3)) {
            dump_error(ctx);
            return;
        }
        JS_PushArg(ctx, JS_NewInt32(ctx, ev->level)); /* arg0 */
        JS_PushArg(ctx, g->fn.val);                   /* func */
        JS_PushArg(ctx, JS_NULL);                     /* this */
        arm_watchdog();
        JSValue ret = JS_Call(ctx, 1);
        if (JS_IsException(ret))
            dump_error(ctx);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* event loop                                                          */
/* ------------------------------------------------------------------ */

/* generated by the host tool from device_stdlib.c; references the
   binding functions above, so it must be included after them */
#include "device_stdlib.h"

static bool anything_pending(void)
{
    for (int i = 0; i < MQJS_MAX_TIMERS; i++)
        if (s_timers[i].used)
            return true;
    for (int i = 0; i < MQJS_MAX_GPIO_CB; i++)
        if (s_gpio_cb[i].used)
            return true;
    return false;
}

static void reset_slots(JSContext *ctx)
{
    for (int i = 0; i < MQJS_MAX_TIMERS; i++) {
        if (s_timers[i].used) {
            JS_DeleteGCRef(ctx, &s_timers[i].fn);
            s_timers[i].used = false;
        }
    }
    for (int i = 0; i < MQJS_MAX_GPIO_CB; i++) {
        if (s_gpio_cb[i].used) {
#ifdef ESP_PLATFORM
            gpio_isr_handler_remove(s_gpio_cb[i].pin);
#endif
            JS_DeleteGCRef(ctx, &s_gpio_cb[i].fn);
            s_gpio_cb[i].used = false;
        }
    }
}

void mqjs_runtime_stop(void)
{
    s_stop_req = true;
}

int mqjs_run_script(const char *src, size_t src_len, const char *name,
                    void *mem_buf, size_t mem_size)
{
    int ret_code = 0;

    s_stop_req = false;
    memset(s_timers, 0, sizeof(s_timers));
    memset(s_gpio_cb, 0, sizeof(s_gpio_cb));

#ifdef ESP_PLATFORM
    if (!s_gpio_queue)
        s_gpio_queue = xQueueCreate(MQJS_QUEUE_LEN, sizeof(GpioEvent));
#endif

    JSContext *ctx = JS_NewContext(mem_buf, mem_size, &js_stdlib);
    if (!ctx)
        return -1;
    JS_SetLogFunc(ctx, js_log_func);
    JS_SetInterruptHandler(ctx, js_interrupt_handler);

    /* 1) compile + run top-level (registers callbacks) */
    arm_watchdog();
    JSValue val = JS_Eval(ctx, src, src_len, name ? name : "<task>", 0);
    if (JS_IsException(val)) {
        dump_error(ctx);
        ret_code = -1;
        goto done;
    }

    /* 2) pump events until nothing is scheduled */
    while (!s_stop_req && anything_pending()) {
        int idle = run_timers(ctx, 50 /* ms */);
#ifdef ESP_PLATFORM
        GpioEvent ev;
        if (xQueueReceive(s_gpio_queue, &ev, pdMS_TO_TICKS(idle)))
            dispatch_gpio_event(ctx, &ev);
#else
        (void)dispatch_gpio_event;   /* unused in PC build */
        if (idle > 0)
            usleep((useconds_t)idle * 1000);
#endif
    }

done:
    reset_slots(ctx);
    JS_FreeContext(ctx);  /* runs user-object finalizers */
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "task '%s' finished (rc=%d)", name ? name : "<task>", ret_code);
#endif
    return ret_code;
}
