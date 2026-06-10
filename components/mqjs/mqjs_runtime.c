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
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>

#include "cutils.h"
#include "mquickjs.h"
#include "mqjs_runtime.h"

#ifdef ESP_PLATFORM
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "ui_tab5.h"
static const char *TAG = "mqjs";
#else
#include <time.h>
#include <unistd.h>
#endif

#define MQJS_MAX_TIMERS       16
#define MQJS_MAX_GPIO_CB      8
#define MQJS_MAX_MQTT_SUB     8
#define MQJS_MQTT_TOPIC_MAX   96
#define MQJS_MQTT_PAYLOAD_MAX 4096
#define MQJS_MAX_RUN_MS       5000  /* per JS_Eval / callback watchdog */
#define MQJS_QUEUE_LEN        32

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

/* print sink: tee of all JS-visible output, assembled into lines so the
   UI console can store fixed-size records (see mqjs_set_print_sink) */
static void (*s_print_sink)(const char *, size_t);
static char s_sink_line[96];
static size_t s_sink_len;

void mqjs_set_print_sink(void (*fn)(const char *, size_t))
{
    s_print_sink = fn;
}

static void sink_flush(void)
{
    if (s_print_sink && s_sink_len)
        s_print_sink(s_sink_line, s_sink_len);
    s_sink_len = 0;
}

static void out_write(const void *buf, size_t len)
{
    fwrite(buf, 1, len, stdout);
    if (!s_print_sink)
        return;
    const char *p = buf;
    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        if (c == '\n') {
            sink_flush();
            continue;
        }
        if (s_sink_len == sizeof(s_sink_line)) {
            /* split overlong lines at a UTF-8 sequence boundary */
            size_t cut = s_sink_len;
            while (cut > 0 && (s_sink_line[cut - 1] & 0xC0) == 0x80)
                cut--;
            if (cut > 0 && (s_sink_line[cut - 1] & 0x80))
                cut--; /* drop the lead byte of the split sequence too */
            if (cut == 0)
                cut = s_sink_len;
            size_t rest = s_sink_len - cut;
            char carry[4];
            memcpy(carry, s_sink_line + cut, rest);
            s_sink_len = cut;
            sink_flush();
            memcpy(s_sink_line, carry, rest);
            s_sink_len = rest;
        }
        s_sink_line[s_sink_len++] = c;
    }
}

static void js_log_func(void *opaque, const void *buf, size_t buf_len)
{
    out_write(buf, buf_len);
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

/* one queue feeds the JS loop; producers are the GPIO ISR and the
   esp-mqtt event task. mqtt strings are heap copies owned by the
   event: the dispatcher (or the drain in reset_slots) frees them. */
typedef enum { EV_GPIO, EV_MQTT_CONNECTED, EV_MQTT_DATA, EV_TOUCH } MqjsEventType;

typedef struct {
    uint8_t type;
    union {
        struct { uint8_t pin; uint8_t level; } gpio;
        struct { char *topic; char *payload; uint32_t len; } mqtt;
        struct { int16_t x, y; uint8_t kind; } touch;
    } u;
} MqjsEvent;

typedef struct {
    bool used;
    char topic[MQJS_MQTT_TOPIC_MAX];
    JSGCRef fn;
} MqttSub;

static TimerSlot s_timers[MQJS_MAX_TIMERS];
static GpioSlot  s_gpio_cb[MQJS_MAX_GPIO_CB];
static MqttSub   s_mqtt_subs[MQJS_MAX_MQTT_SUB];
static bool      s_mqtt_onconn_used;
static JSGCRef   s_mqtt_onconn;
static volatile bool s_touch_used; /* read by the UI task (poster) */
static JSGCRef   s_touch_cb;
static volatile bool s_stop_req;
static int64_t s_run_deadline;          /* JS watchdog */

#ifdef ESP_PLATFORM
static QueueHandle_t s_event_queue;
static bool s_isr_service_installed;
static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_mqtt_up;         /* broker session established */
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
    static const char pfx[] = "mqjs: uncaught exception: ";
    out_write(pfx, sizeof(pfx) - 1);
    JS_PrintValueF(ctx, e, JS_DUMP_LONG); /* goes through js_log_func */
    out_write("\n", 1);
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

static void dispatch_gpio_event(JSContext *ctx, const MqjsEvent *ev)
{
    for (int i = 0; i < MQJS_MAX_GPIO_CB; i++) {
        GpioSlot *g = &s_gpio_cb[i];
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
/* i2c (synchronous; transactions are sub-ms so they run inline)       */
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
/* same script runs on Stamp; PC build = print-only stubs)             */
/* ------------------------------------------------------------------ */

#ifndef ESP_PLATFORM
/* mirror of ui_cmd_op_t in ui_tab5.h (not includable on PC) */
typedef enum {
    UI_CMD_CLEAR = 0, UI_CMD_FILL, UI_CMD_RECT,
    UI_CMD_LINE, UI_CMD_TEXT, UI_CMD_PIXEL,
} ui_cmd_op_t;
#endif

/* Post one drawing command. Takes ownership of `text` (heap copy) in
   every outcome; drops are counted on-screen by the UI itself. */
static void ui_post(uint8_t op, int x, int y, int w, int h,
                    uint32_t color, char *text)
{
#ifdef ESP_PLATFORM
    ui_cmd_t c = {
        .op = op,
        .x = (int16_t)x, .y = (int16_t)y,
        .w = (int16_t)w, .h = (int16_t)h,
        .color = color,
        .text = text,
    };
    if (!ui_tab5_cmd(&c))
        free(text);
#else
    static const char *names[] =
        { "clear", "fill", "rect", "line", "text", "pixel" };
    printf("[ui] %s(x=%d, y=%d, w=%d, h=%d, c=0x%06x%s%s) (stub)\n",
           names[op], x, y, w, h, (unsigned)color,
           text ? ", " : "", text ? text : "");
    free(text);
#endif
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
   (x, y, kind) with kind 0=down 1=move 2=up in canvas coordinates */
JSValue js_ui_onTouch(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    if (s_touch_used)
        JS_DeleteGCRef(ctx, &s_touch_cb); /* re-register replaces */
    JSValue *pf = JS_AddGCRef(ctx, &s_touch_cb);
    *pf = argv[0];
    s_touch_used = true;
#ifndef ESP_PLATFORM
    printf("[ui] onTouch registered (stub: never fires on PC)\n");
#endif
    return JS_UNDEFINED;
}

void mqjs_post_touch(int x, int y, int kind)
{
#ifdef ESP_PLATFORM
    if (!s_event_queue || !s_touch_used)
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

static void dispatch_touch_event(JSContext *ctx, const MqjsEvent *ev)
{
    if (!s_touch_used)
        return;
    if (JS_StackCheck(ctx, 5)) {
        dump_error(ctx);
        return;
    }
    /* args are pushed in reverse: the last-pushed one becomes arg0 */
    JS_PushArg(ctx, JS_NewInt32(ctx, ev->u.touch.kind)); /* arg2 */
    JS_PushArg(ctx, JS_NewInt32(ctx, ev->u.touch.y));    /* arg1 */
    JS_PushArg(ctx, JS_NewInt32(ctx, ev->u.touch.x));    /* arg0 */
    JS_PushArg(ctx, s_touch_cb.val);                     /* func */
    JS_PushArg(ctx, JS_NULL);                            /* this */
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 3);
    if (JS_IsException(ret))
        dump_error(ctx);
}

/* ------------------------------------------------------------------ */
/* mqtt (esp-mqtt; PC build = print-only stubs)                        */
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
/* runs in the esp-mqtt task: copy + enqueue only, never touch JS */
static void mqtt_event_cb(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t e = event_data;
    MqjsEvent ev = { 0 };

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_up = true;
        ev.type = EV_MQTT_CONNECTED;
        xQueueSend(s_event_queue, &ev, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_up = false;   /* esp-mqtt auto-reconnects */
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
    if (s_mqtt)
        return JS_ThrowTypeError(ctx, "mqtt already started");
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,   /* copied by esp_mqtt_client_init */
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt)
        return JS_ThrowInternalError(ctx, "mqtt init failed (bad uri?)");
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_cb, NULL);
    if (esp_mqtt_client_start(s_mqtt) != ESP_OK) {
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
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
    if (s_mqtt) {
        esp_mqtt_client_stop(s_mqtt);
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
        s_mqtt_up = false;
    }
#else
    printf("[mqtt] disconnect (stub)\n");
#endif
    return JS_UNDEFINED;
}

JSValue js_mqtt_connected(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
#ifdef ESP_PLATFORM
    return JS_NewInt32(ctx, s_mqtt_up ? 1 : 0);
#else
    return JS_NewInt32(ctx, 0);
#endif
}

JSValue js_mqtt_onConnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    if (s_mqtt_onconn_used)
        JS_DeleteGCRef(ctx, &s_mqtt_onconn);
    JSValue *pf = JS_AddGCRef(ctx, &s_mqtt_onconn);
    *pf = argv[0];
    s_mqtt_onconn_used = true;
    return JS_UNDEFINED;
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
    if (!s_mqtt)
        return JS_ThrowTypeError(ctx, "mqtt not connected");
    int id = esp_mqtt_client_publish(s_mqtt, topic, payload, (int)plen,
                                     qos, retain);
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
        MqttSub *s = &s_mqtt_subs[i];
        if (s->used && !strcmp(s->topic, topic))
            return JS_ThrowTypeError(ctx, "topic already subscribed");
    }
    for (int i = 0; i < MQJS_MAX_MQTT_SUB; i++) {
        MqttSub *s = &s_mqtt_subs[i];
        if (s->used)
            continue;
        memcpy(s->topic, topic, tlen + 1);
        JSValue *pf = JS_AddGCRef(ctx, &s->fn);
        *pf = argv[1];
        s->used = true;
#ifdef ESP_PLATFORM
        if (s_mqtt && s_mqtt_up)
            esp_mqtt_client_subscribe(s_mqtt, s->topic, 0);
        /* not connected yet: dispatch_mqtt_connected() subscribes later */
#else
        printf("[mqtt] subscribe(%s) registered (stub: never fires on PC)\n",
               s->topic);
#endif
        return JS_UNDEFINED;
    }
    return JS_ThrowInternalError(ctx, "too many mqtt subscriptions");
}

static void dispatch_mqtt_connected(JSContext *ctx)
{
#ifdef ESP_PLATFORM
    /* (re)subscribe everything on each broker session; duplicate
       SUBSCRIBE packets are just a refresh for the broker */
    for (int i = 0; i < MQJS_MAX_MQTT_SUB; i++) {
        if (s_mqtt_subs[i].used && s_mqtt)
            esp_mqtt_client_subscribe(s_mqtt, s_mqtt_subs[i].topic, 0);
    }
#endif
    if (!s_mqtt_onconn_used)
        return;
    if (JS_StackCheck(ctx, 2)) {
        dump_error(ctx);
        return;
    }
    JS_PushArg(ctx, s_mqtt_onconn.val);  /* func */
    JS_PushArg(ctx, JS_NULL);            /* this */
    arm_watchdog();
    JSValue ret = JS_Call(ctx, 0);
    if (JS_IsException(ret))
        dump_error(ctx);
}

static void dispatch_mqtt_data(JSContext *ctx, MqjsEvent *ev)
{
    for (int i = 0; i < MQJS_MAX_MQTT_SUB; i++) {
        MqttSub *s = &s_mqtt_subs[i];
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
    if (s_touch_used) /* a touch handler keeps the loop alive */
        return true;
#ifdef ESP_PLATFORM
    if (s_mqtt)   /* active mqtt session keeps the loop alive */
        return true;
#endif
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
#ifdef ESP_PLATFORM
    /* stop producers first, then drain the queue and free mqtt copies */
    if (s_mqtt) {
        esp_mqtt_client_stop(s_mqtt);
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
        s_mqtt_up = false;
    }
    if (s_event_queue) {
        MqjsEvent ev;
        while (xQueueReceive(s_event_queue, &ev, 0) == pdTRUE) {
            if (ev.type == EV_MQTT_DATA) {
                free(ev.u.mqtt.topic);
                free(ev.u.mqtt.payload);
            }
        }
    }
#endif
    for (int i = 0; i < MQJS_MAX_MQTT_SUB; i++) {
        if (s_mqtt_subs[i].used) {
            JS_DeleteGCRef(ctx, &s_mqtt_subs[i].fn);
            s_mqtt_subs[i].used = false;
        }
    }
    if (s_mqtt_onconn_used) {
        JS_DeleteGCRef(ctx, &s_mqtt_onconn);
        s_mqtt_onconn_used = false;
    }
    if (s_touch_used) {
        s_touch_used = false; /* before the GCRef dies: gates the poster */
        JS_DeleteGCRef(ctx, &s_touch_cb);
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
    memset(s_mqtt_subs, 0, sizeof(s_mqtt_subs));
    s_mqtt_onconn_used = false;
    s_touch_used = false;

#ifdef ESP_PLATFORM
    if (!s_event_queue)
        s_event_queue = xQueueCreate(MQJS_QUEUE_LEN, sizeof(MqjsEvent));
#endif

    JSContext *ctx = JS_NewContext(mem_buf, mem_size, &js_stdlib);
    if (!ctx)
        return -1;
    JS_SetLogFunc(ctx, js_log_func);
    JS_SetInterruptHandler(ctx, js_interrupt_handler);

    /* 1) compile + run top-level (registers callbacks) */
    arm_watchdog();
    JSValue val;
    if (JS_IsBytecode((const uint8_t *)src, src_len)) {
        /* precompiled task: the buffer is patched in place and stays
           referenced for the whole context lifetime, so it must live in
           RAM (heap), never in flash. Trusted-source only: the loader
           does not validate bytecode (Ed25519 gate in task_source.c).
           Re-relocating the same buffer on a later run is a no-op. */
        if (JS_RelocateBytecode(ctx, (uint8_t *)src, (uint32_t)src_len)) {
            printf("mqjs: bytecode relocation failed\n");
            ret_code = -1;
            goto done;
        }
        val = JS_LoadBytecode(ctx, (const uint8_t *)src);
        if (!JS_IsException(val))
            val = JS_Run(ctx, val);
    } else {
        val = JS_Eval(ctx, src, src_len, name ? name : "<task>", 0);
    }
    if (JS_IsException(val)) {
        dump_error(ctx);
        ret_code = -1;
        goto done;
    }

    /* 2) pump events until nothing is scheduled */
    while (!s_stop_req && anything_pending()) {
        int idle = run_timers(ctx, 50 /* ms */);
#ifdef ESP_PLATFORM
        MqjsEvent ev;
        if (xQueueReceive(s_event_queue, &ev, pdMS_TO_TICKS(idle))) {
            switch (ev.type) {
            case EV_GPIO:           dispatch_gpio_event(ctx, &ev);    break;
            case EV_MQTT_CONNECTED: dispatch_mqtt_connected(ctx);     break;
            case EV_MQTT_DATA:      dispatch_mqtt_data(ctx, &ev);     break;
            case EV_TOUCH:          dispatch_touch_event(ctx, &ev);   break;
            }
        }
#else
        (void)dispatch_gpio_event;       /* unused in PC build */
        (void)dispatch_mqtt_connected;
        (void)dispatch_mqtt_data;
        (void)dispatch_touch_event;
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
