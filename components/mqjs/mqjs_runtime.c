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
#include "mqjs_classes.h"

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
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ui_tab5.h"
#include "sshc.h"
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
   UI console can store fixed-size records (see mqjs_set_print_sink).
   The split width bounds one on-screen console record: too small and
   long lines visibly break before the panel edge (256B = 85 CJK or
   256 ASCII glyphs, comfortably past one 720px row). */
static void (*s_print_sink)(const char *, size_t);
static char s_sink_line[256];
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
typedef enum { EV_GPIO, EV_MQTT_CONNECTED, EV_MQTT_DATA, EV_TOUCH, EV_KEY,
               EV_SSH_DATA, EV_SSH_CLOSED, EV_WIDGET } MqjsEventType;

typedef struct {
    uint8_t type;
    union {
        struct { uint8_t pin; uint8_t level; } gpio;
        struct { char *topic; char *payload; uint32_t len; } mqtt;
        struct { int16_t x, y; uint8_t kind; } touch;
        struct { char text[8]; uint8_t len; } key; /* one key as UTF-8 */
        struct { char *data; uint32_t len; int16_t id; } ssh; /* heap rx */
        struct { char reason[22]; int16_t id; } ssh_closed;
        struct { uint32_t handle; int32_t value; } widget; /* tap/change */
    } u;
} MqjsEvent;

typedef struct {
    bool used;
    char topic[MQJS_MQTT_TOPIC_MAX];
    JSGCRef fn;
} MqttSub;

/* One JS callback per interactive widget (button/list row/toggle/slider).
   `screen` is the owning screen's handle: when that screen is destroyed
   (ui.back() / retain-depth eviction / task end) every slot it owned is
   released in one sweep, so the compacting GC repacks once instead of
   per-widget (design §4④). */
typedef struct {
    bool used;
    uint32_t handle;  /* widget handle the LVGL side posts with */
    uint32_t screen;  /* owning screen handle */
    JSGCRef fn;
} WidgetCb;

static TimerSlot s_timers[MQJS_MAX_TIMERS];
static GpioSlot  s_gpio_cb[MQJS_MAX_GPIO_CB];
static MqttSub   s_mqtt_subs[MQJS_MAX_MQTT_SUB];
static WidgetCb  s_widget_cbs[MQJS_MAX_WIDGET_CB];
static bool      s_mqtt_onconn_used;
static JSGCRef   s_mqtt_onconn;
static volatile bool s_touch_used; /* read by the UI task (poster) */
static JSGCRef   s_touch_cb;
static volatile bool s_key_used;   /* read by the UI task (poster) */
static JSGCRef   s_key_cb;

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
static SshCb s_ssh_cbs[MQJS_MAX_SSH_CB];
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
    UI_CMD_LINE, UI_CMD_TEXT, UI_CMD_PIXEL, UI_CMD_KEYBOARD,
    UI_CMD_CELLS, UI_CMD_SCROLL,
} ui_cmd_op_t;
#endif

/* Post one drawing command. Takes ownership of `text` (heap copy) in
   every outcome; drops are counted on-screen by the UI itself. `bg` is
   only used by UI_CMD_CELLS (cell background). */
static void ui_post_bg(uint8_t op, int x, int y, int w, int h,
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
          "cells", "scroll" };
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

/* on-screen keyboard (Phase 4): ui.keyboard(show) toggles the LVGL
   keyboard overlay; keys arrive via mqjs_post_key as short UTF-8
   strings ("\n" enter, "\b" backspace, "\x1b[C"/"\x1b[D" arrows) */
JSValue js_ui_keyboard(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    int show = 1;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) &&
        JS_ToInt32(ctx, &show, argv[0]))
        return JS_EXCEPTION;
    ui_post(UI_CMD_KEYBOARD, show ? 1 : 0, 0, 0, 0, 0, NULL);
    return JS_UNDEFINED;
}

JSValue js_ui_onKey(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    if (s_key_used)
        JS_DeleteGCRef(ctx, &s_key_cb); /* re-register replaces */
    JSValue *pf = JS_AddGCRef(ctx, &s_key_cb);
    *pf = argv[0];
    s_key_used = true;
#ifndef ESP_PLATFORM
    printf("[ui] onKey registered (stub: never fires on PC)\n");
#endif
    return JS_UNDEFINED;
}

void mqjs_post_key(const char *utf8, size_t len)
{
#ifdef ESP_PLATFORM
    MqjsEvent ev = { .type = EV_KEY };
    if (!s_event_queue || !s_key_used || !utf8)
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

static void dispatch_key_event(JSContext *ctx, const MqjsEvent *ev)
{
    if (!s_key_used)
        return;
    if (JS_StackCheck(ctx, 3)) {
        dump_error(ctx);
        return;
    }
    JS_PushArg(ctx, JS_NewStringLen(ctx, ev->u.key.text,
                                    ev->u.key.len));     /* arg0 */
    JS_PushArg(ctx, s_key_cb.val);                       /* func */
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
/* widget scripts smoke-test on the host.                              */
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
        WidgetCb *w = &s_widget_cbs[i];
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
static void wcb_release_screen(JSContext *ctx, uint32_t screen)
{
    for (int i = 0; i < MQJS_MAX_WIDGET_CB; i++) {
        WidgetCb *w = &s_widget_cbs[i];
        if (w->used && w->screen == screen) {
            w->used = false;
            JS_DeleteGCRef(ctx, &w->fn);
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

static void dispatch_widget_event(JSContext *ctx, const MqjsEvent *ev)
{
    for (int i = 0; i < MQJS_MAX_WIDGET_CB; i++) {
        WidgetCb *w = &s_widget_cbs[i];
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

/* ui.screen(title) -> UiScreen */
JSValue js_ui_screen(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    char title[64];
    if (uiw_copy_str(ctx, argv[0], title, sizeof title))
        return JS_EXCEPTION;
    uint32_t evicted = 0, h;
#ifdef ESP_PLATFORM
    h = ui_tab5_w_screen(title, &evicted);
#else
    h = pcw_screen(title, &evicted);
#endif
    if (evicted)
        wcb_release_screen(ctx, evicted);
    return uiw_make(ctx, h, h, UIW_K_SCREEN, JS_CLASS_UI_SCREEN);
}

/* ui.back() -> bool (false on the console screen / UI-less build).
   Also usable directly as a tap callback: s.button("Cancel", ui.back). */
JSValue js_ui_back(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    uint32_t destroyed;
#ifdef ESP_PLATFORM
    destroyed = ui_tab5_w_back();
#else
    destroyed = pcw_back();
#endif
    if (destroyed)
        wcb_release_screen(ctx, destroyed);
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

    uint32_t h;
#ifdef ESP_PLATFORM
    h = ui_tab5_w_create(magic, sh->handle, text, a, b, c);
#else
    h = s_pcw_next++;
    printf("[ui] widget(kind=%d, \"%s\") -> #%u (stub)\n", magic, text,
           (unsigned)h);
#endif
    if (h && JS_IsFunction(ctx, cb)) {
        if (wcb_add(ctx, h, sh->screen, cb))
            return JS_ThrowInternalError(ctx, "too many widget callbacks");
    }
    return uiw_make(ctx, h, sh->screen, magic, JS_CLASS_UI_WIDGET);
}

/* UiWidget.prototype.add(text, onTap) — list rows */
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
    uint32_t h;
#ifdef ESP_PLATFORM
    h = ui_tab5_w_create(UIW_K_ITEM, lh->handle, text, 0, 0, 0);
#else
    h = s_pcw_next++;
    printf("[ui] list.add(\"%s\") -> #%u (stub)\n", text, (unsigned)h);
#endif
    if (h && JS_IsFunction(ctx, cb)) {
        if (wcb_add(ctx, h, lh->screen, cb))
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
/* ------------------------------------------------------------------ */

#define MQJS_STORE_VAL_MAX 3900

#ifdef ESP_PLATFORM
static nvs_handle_t s_store;
static bool s_store_open;

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
#else
#define MQJS_PC_STORE 16
static struct {
    char key[16];
    char *val;
} s_pc_store[MQJS_PC_STORE];

static int pc_store_find(const char *k)
{
    for (int i = 0; i < MQJS_PC_STORE; i++)
        if (s_pc_store[i].val && !strcmp(s_pc_store[i].key, k))
            return i;
    return -1;
}
#endif

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
    bool ok = nvs_set_str(s_store, key, val) == ESP_OK &&
              nvs_commit(s_store) == ESP_OK;
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
    bool ok = nvs_erase_key(s_store, key) == ESP_OK &&
              nvs_commit(s_store) == ESP_OK;
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
/* sys (W1-4): heap telemetry for the navigation-churn measurement     */
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

/* ------------------------------------------------------------------ */
/* ssh (wolfSSH session tasks in components/sshc; PC = print stubs).   */
/* W3 handle-style: ssh.connect() returns a session id; write/resize/  */
/* close/connected/onData/onClose take it as the first argument, so up */
/* to 3 sessions can be kept concurrently (design §7). Each session    */
/* task posts EV_SSH_DATA (heap copies, owned by the event) with its   */
/* id and one EV_SSH_CLOSED; any active session keeps the JS loop      */
/* alive like an mqtt session does.                                    */
/* ------------------------------------------------------------------ */

static SshCb *sshcb_find(int32_t id)
{
    for (int i = 0; i < MQJS_MAX_SSH_CB; i++)
        if (s_ssh_cbs[i].used && s_ssh_cbs[i].id == id)
            return &s_ssh_cbs[i];
    return NULL;
}

static SshCb *sshcb_get(int32_t id)
{
    SshCb *c = sshcb_find(id);
    if (c)
        return c;
    for (int i = 0; i < MQJS_MAX_SSH_CB; i++) {
        if (!s_ssh_cbs[i].used) {
            c = &s_ssh_cbs[i];
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

#ifndef ESP_PLATFORM
static int s_pc_ssh_next = 1; /* fake session ids for PC smoke runs */
#endif

/* ssh.connect(host, port, user, pass, cols, rows) -> session id (> 0).
   Throws when all session slots are busy. */
JSValue js_ssh_connect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv)
{
    JSCStringBuf hbuf, ubuf, pbuf;
    size_t hlen, ulen, plen;
    int port = 22, cols = 80, rows = 24;

    /* copy host/user out first: each later ToCString may move earlier
       strings in the compacting GC heap */
    char host[64], user[32];
    const char *p = JS_ToCStringLen(ctx, &hlen, argv[0], &hbuf);
    if (!p)
        return JS_EXCEPTION;
    if (hlen >= sizeof host)
        hlen = sizeof host - 1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToInt32(ctx, &port, argv[1]))
        return JS_EXCEPTION;
    p = JS_ToCStringLen(ctx, &ulen, argv[2], &ubuf);
    if (!p)
        return JS_EXCEPTION;
    if (ulen >= sizeof user)
        ulen = sizeof user - 1;
    memcpy(user, p, ulen);
    user[ulen] = '\0';
    const char *pass = JS_ToCStringLen(ctx, &plen, argv[3], &pbuf);
    if (!pass)
        return JS_EXCEPTION;
    if (argc >= 5 && !JS_IsUndefined(argv[4]) && JS_ToInt32(ctx, &cols, argv[4]))
        return JS_EXCEPTION;
    if (argc >= 6 && !JS_IsUndefined(argv[5]) && JS_ToInt32(ctx, &rows, argv[5]))
        return JS_EXCEPTION;

#ifdef ESP_PLATFORM
    int id = mqjs_ssh_connect(host, port, user, pass, cols, rows);
    if (!id)
        return JS_ThrowTypeError(ctx, "no free ssh session (max %d)",
                                 SSHC_MAX_SESSIONS);
#else
    int id = s_pc_ssh_next++;
    printf("[ssh] connect(%s@%s:%d, pty %dx%d) -> #%d (stub: never connects "
           "on PC)\n", user, host, port, cols, rows, id);
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
    SshCb *c = sshcb_get(id);
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

static void dispatch_ssh_data(JSContext *ctx, MqjsEvent *ev)
{
    SshCb *c = sshcb_find(ev->u.ssh.id);
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
}

static void dispatch_ssh_closed(JSContext *ctx, const MqjsEvent *ev)
{
    SshCb *c = sshcb_find(ev->u.ssh_closed.id);
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
        /* distinct client id: the default is derived from the MAC, so a
           JS task connecting to the same broker as the task-delivery
           client (task_source.c) made the broker kick whichever
           connected first — task pushes flapped and the status-bar MQTT
           dot went gray (found the hard way, 2026-06-11) */
        .credentials.client_id = "mqjs-js-task",
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
    if (s_key_used)   /* so does a key handler */
        return true;
    for (int i = 0; i < MQJS_MAX_WIDGET_CB; i++)
        if (s_widget_cbs[i].used) /* a live widget screen, too */
            return true;
#ifdef ESP_PLATFORM
    if (s_mqtt)   /* active mqtt session keeps the loop alive */
        return true;
    if (mqjs_ssh_active()) /* so does an ssh session */
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
    mqjs_ssh_close_all(); /* blocks until session tasks died (bounded) */
    if (s_event_queue) {
        MqjsEvent ev;
        while (xQueueReceive(s_event_queue, &ev, 0) == pdTRUE) {
            if (ev.type == EV_MQTT_DATA) {
                free(ev.u.mqtt.topic);
                free(ev.u.mqtt.payload);
            } else if (ev.type == EV_SSH_DATA) {
                free(ev.u.ssh.data);
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
    if (s_key_used) {
        s_key_used = false;   /* ditto */
        JS_DeleteGCRef(ctx, &s_key_cb);
    }
    for (int i = 0; i < MQJS_MAX_SSH_CB; i++) {
        if (s_ssh_cbs[i].used)
            sshcb_release(ctx, &s_ssh_cbs[i]);
    }
    for (int i = 0; i < MQJS_MAX_WIDGET_CB; i++) {
        if (s_widget_cbs[i].used) {
            s_widget_cbs[i].used = false;
            JS_DeleteGCRef(ctx, &s_widget_cbs[i].fn);
        }
    }
    /* tear down this task's widget screens; the next task starts on the
       console screen (same hygiene as the canvas clear on task switch) */
#ifdef ESP_PLATFORM
    ui_tab5_w_reset();
#else
    pcw_reset();
#endif
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
    memset(s_widget_cbs, 0, sizeof(s_widget_cbs));
    s_mqtt_onconn_used = false;
    s_touch_used = false;
    s_key_used = false;
    memset(s_ssh_cbs, 0, sizeof(s_ssh_cbs));

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
            case EV_KEY:            dispatch_key_event(ctx, &ev);     break;
            case EV_SSH_DATA:       dispatch_ssh_data(ctx, &ev);      break;
            case EV_SSH_CLOSED:     dispatch_ssh_closed(ctx, &ev);    break;
            case EV_WIDGET:         dispatch_widget_event(ctx, &ev);  break;
            }
        }
#else
        (void)dispatch_gpio_event;       /* unused in PC build */
        (void)dispatch_mqtt_connected;
        (void)dispatch_mqtt_data;
        (void)dispatch_touch_event;
        (void)dispatch_key_event;
        (void)dispatch_ssh_data;
        (void)dispatch_ssh_closed;
        (void)dispatch_widget_event;
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
