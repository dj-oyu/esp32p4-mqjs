/*
 * M5Stack Tab5 keyboard dock (A164): STM32F030 scanner at I2C 0x6D on
 * the pogo connector (SDA=GPIO0 SCL=GPIO1, INT=GPIO50 active-low).
 *
 * Mode choice: the dock offers Normal (raw row/col, press AND release
 * for EVERY key), HID and Character modes. We run NORMAL and keep the
 * whole keymap host-side (tables lifted from the dock firmware's
 * user_keyboard_handle.c): it is the only mode that reports bare
 * modifier presses (sym/Aa/ctrl/alt are swallowed by the dock's own
 * state machine in HID/Character mode), which we need for
 *   - immediate modifier LED feedback (sym=blue, ctrl=cyan, alt=gray,
 *     Aa=red — WS2812 pair driven via the RGB custom-mode registers),
 *   - typematic repeat that stops on the held key's own release,
 *   - the Aa click/double-click/hold semantics (one-shot / caps lock /
 *     shift-while-held), mirrored from the dock firmware.
 *
 * Key sequences posted to mqjs_post_key() use the exact on-screen
 * keyboard / control-bar vocabulary, so apps need no awareness:
 * printables, "\n" "\b" "\t", "\x00name" tokens, Ctrl+letter as
 * control bytes, Alt as ESC prefix.
 *
 * Event flow (single task, no I2C from ISRs):
 *   GPIO50 negedge ISR -> semaphore -> drain INT_STA/EVENT_NUM/0x20
 *   queue -> keymap -> mqjs_post_key(). A 100 ms fallback poll covers
 *   edges missed while the line was already asserted, and a 2 s probe
 *   loop covers hot-plug both ways.
 */
#include "sdkconfig.h"

#if CONFIG_MQJS_TAB5_KEYBOARD

#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "kbd_tab5.h"
#include "mqjs_runtime.h"

static const char *TAG = "kbd_tab5";

#define KB_ADDR       0x6D
#define KB_SDA        0
#define KB_SCL        1
#define KB_INT_GPIO   50
#define KB_I2C_HZ     100000 /* dock default; 400k also supported */

#define KB_REG_INT_CFG   0x00
#define KB_REG_INT_STA   0x01
#define KB_REG_EVENT_NUM 0x02
#define KB_REG_MODE      0x10
#define KB_REG_RGB_MODE  0x11
#define KB_REG_KEY_EVENT 0x20
#define KB_REG_RGB_BASE  0x60 /* RGB1_B,G,R, RGB2_B,G,R */
#define KB_REG_VERSION   0xFE

#define KB_MODE_NORMAL    0
#define KB_RGB_CUSTOM     1
#define KB_INT_NORMAL_BIT 0x01

#define KB_PROBE_MS        2000 /* absent: how often to look for the dock */
#define KB_POLL_MS         100  /* present: fallback INT_STA poll */
#define KB_REPEAT_DELAY_MS 400  /* typematic: first repeat */
#define KB_REPEAT_TICK_MS  55   /* typematic: rate (~18 cps) */
#define KB_AA_CLICK_MS     400  /* Aa tap/double-tap window (dock FW value) */
#define KB_ERR_LIMIT       3    /* consecutive comm errors = detached */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_int_sem;
static volatile bool s_present;
static kbd_tab5_presence_cb_t s_presence_cb;

/* ---- modifier / lock state (kbd task only) -------------------------- */
static bool s_sym_held, s_ctrl_held, s_alt_held;
static bool s_aa_held;        /* Aa physically down (shift-while-held) */
static bool s_aa_lock;        /* double-tap caps lock */
static bool s_aa_oneshot;     /* single tap: next letter uppercase */
static int64_t s_aa_press_us, s_aa_release_us;
static int s_aa_clicks;

/* typematic state */
static bool s_held;
static uint8_t s_held_row, s_held_col;
static char s_rep_seq[8];
static size_t s_rep_len;
static int64_t s_next_rep_us;

/* ---- I2C helpers --------------------------------------------------- */

static bool rd_reg(uint8_t reg, uint8_t *val, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, len, 50) == ESP_OK;
}

static bool wr_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 50) == ESP_OK;
}

static bool wr_regs(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[8];
    if (len + 1 > sizeof buf)
        return false;
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, 50) == ESP_OK;
}

/* ---- modifier LEDs --------------------------------------------------
 * The two WS2812s under the sym/Aa keys, driven through the dock's RGB
 * custom mode. One color for both, by held-modifier precedence. */

static uint32_t s_led_rgb = 1; /* impossible initial: force first write */

static void led_update(void)
{
    uint32_t rgb;
    if (s_sym_held)
        rgb = 0x0000FF; /* sym: blue */
    else if (s_ctrl_held)
        rgb = 0x00FFFF; /* ctrl: cyan */
    else if (s_alt_held)
        rgb = 0x808080; /* alt: gray */
    else if (s_aa_held || s_aa_lock || s_aa_oneshot)
        rgb = 0xFF0000; /* uppercase pending/locked: red */
    else
        rgb = 0x000000; /* idle: off */
    if (rgb == s_led_rgb)
        return;
    uint8_t r = rgb >> 16, g = rgb >> 8, b = rgb;
    uint8_t bgr2[6] = { b, g, r, b, g, r }; /* regs are B,G,R per LED */
    if (wr_regs(KB_REG_RGB_BASE, bgr2, 6))
        s_led_rgb = rgb;
}

/* ---- host keymap (dock FW user_keyboard_handle.c, 5x14) -------------
 * Cell codes: printable ASCII verbatim; >= 0x80 = named keys. The sym
 * layer only differs where the FW's key_modifier_flag was set. */

enum {
    K_NONE = 0x80,
    K_ESC, K_DEL, K_TAB, K_BS, K_ENTER,
    K_UP, K_LEFT, K_DOWN, K_RIGHT,
    K_SYM, K_AA, K_CTRL, K_ALT,
};

static const uint8_t s_map[5][14] = {
    { K_ESC, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '+',
      K_DEL },
    { '`', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '[', ']',
      '\\' },
    { K_TAB, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', ';', '\'',
      K_BS },
    { K_SYM, K_AA, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', K_UP, '_',
      K_ENTER },
    { K_CTRL, K_ALT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '.', K_LEFT,
      K_DOWN, K_RIGHT, ' ' },
};

static const uint8_t s_map_sym[5][14] = {
    { K_ESC, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '+',
      K_DEL },
    { '~', '?', '@', '#', '$', '%', '^', '&', '/', '<', '>', '{', '}',
      '|' },
    { K_TAB, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', ':', '"',
      K_BS },
    { K_SYM, K_AA, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', K_UP, '=',
      K_ENTER },
    { K_CTRL, K_ALT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', K_LEFT,
      K_DOWN, K_RIGHT, ' ' },
};

/* named keys -> the JS-side "\x00token" vocabulary (control-bar set) */
static const char *token_name(uint8_t code)
{
    switch (code) {
    case K_ESC:   return "esc";
    case K_DEL:   return "del";
    case K_UP:    return "up";
    case K_LEFT:  return "left";
    case K_DOWN:  return "down";
    case K_RIGHT: return "right";
    default:      return NULL;
    }
}

static bool uppercase_active(void)
{
    return s_aa_held || s_aa_lock || s_aa_oneshot;
}

/* Build the mqjs_post_key sequence for a non-modifier key press.
 * Returns length (0 = nothing to post), sets *repeats. */
static size_t translate(uint8_t row, uint8_t col, char out[8], bool *repeats)
{
    *repeats = false;
    uint8_t code = (s_sym_held ? s_map_sym : s_map)[row][col];

    const char *tok = token_name(code);
    if (tok) {
        size_t n = strlen(tok);
        out[0] = '\0';
        memcpy(out + 1, tok, n);
        /* arrows and del repeat; esc doesn't */
        *repeats = (code != K_ESC);
        return n + 1;
    }

    char c;
    switch (code) {
    case K_TAB:   c = '\t'; break;
    case K_BS:    c = '\b'; break;
    case K_ENTER: c = '\n'; break;
    default:
        if (code >= 0x80)
            return 0;
        c = (char)code;
        if (c >= 'a' && c <= 'z' && uppercase_active()) {
            c = (char)toupper((unsigned char)c);
            /* one-shot consumed by this letter (not while held/locked) */
            if (s_aa_oneshot && !s_aa_held && !s_aa_lock) {
                s_aa_oneshot = false;
                led_update();
            }
        }
        break;
    }

    size_t n = 0;
    if (s_alt_held)
        out[n++] = '\x1b'; /* Meta = ESC prefix (matches the terminal) */

    if (s_ctrl_held) {
        /* Ctrl+letter/@[\]^_ -> control byte; Ctrl+anything-else falls
           through as the plain char. Ctrl+Space would be NUL — that
           collides with the "\x00name" token marker, so it is dropped. */
        char uc = (char)toupper((unsigned char)c);
        if (uc == ' ')
            return 0;
        if (uc >= '@' && uc <= '_')
            c = (char)(uc & 0x1f);
    }
    out[n++] = c;

    *repeats = (c != '\n' && c != '\x1b');
    return n;
}

/* ---- event handling -------------------------------------------------- */

/* Aa mirrors the dock FW: hold = uppercase while held, tap = one-shot
   for the next letter, double-tap = caps lock, tap while locked =
   unlock. */
static void aa_key(bool pressed, int64_t now)
{
    if (pressed) {
        s_aa_held = true;
        s_aa_press_us = now;
        s_aa_clicks =
            (now - s_aa_release_us < KB_AA_CLICK_MS * 1000LL) ? s_aa_clicks + 1
                                                              : 1;
    } else {
        s_aa_held = false;
        s_aa_release_us = now;
        if (now - s_aa_press_us < KB_AA_CLICK_MS * 1000LL) {
            if (s_aa_clicks >= 2) {
                s_aa_lock = true; /* double-tap: caps lock */
                s_aa_oneshot = false;
            } else if (s_aa_lock || s_aa_oneshot) {
                s_aa_lock = false; /* tap while armed/locked: clear */
                s_aa_oneshot = false;
            } else {
                s_aa_oneshot = true; /* tap: next letter uppercase */
            }
        } else {
            /* long hold released: shift ends (lock survives) */
        }
    }
    led_update();
}

static void handle_key_event(uint8_t ev)
{
    bool pressed = (ev & 0x80) != 0;
    uint8_t row = (ev >> 4) & 0x07, col = ev & 0x0F;
    if (row > 4 || col > 13)
        return;
    int64_t now = esp_timer_get_time();

    switch (s_map[row][col]) {
    case K_SYM:
        s_sym_held = pressed;
        led_update();
        return;
    case K_CTRL:
        s_ctrl_held = pressed;
        led_update();
        return;
    case K_ALT:
        s_alt_held = pressed;
        led_update();
        return;
    case K_AA:
        aa_key(pressed, now);
        return;
    default:
        break;
    }

    if (!pressed) {
        /* only the held key's own release stops its repeat */
        if (s_held && row == s_held_row && col == s_held_col)
            s_held = false;
        return;
    }

    char seq[8];
    bool repeats = false;
    size_t n = translate(row, col, seq, &repeats);
    if (!n)
        return;
    mqjs_post_key(seq, n);

    s_held = repeats;
    if (repeats) {
        s_held_row = row;
        s_held_col = col;
        memcpy(s_rep_seq, seq, n);
        s_rep_len = n;
        s_next_rep_us = now + (int64_t)KB_REPEAT_DELAY_MS * 1000;
    }
}

/* Drain the dock's event queue. false = I2C died (dock detached). */
static bool drain_events(void)
{
    uint8_t sta = 0;
    if (!rd_reg(KB_REG_INT_STA, &sta, 1))
        return false;
    if (!(sta & KB_INT_NORMAL_BIT))
        return true;

    /* bounded re-check: events pushed between drain and clear keep the
       queue non-empty, so loop until EVENT_NUM really reads 0 */
    for (int pass = 0; pass < 4; pass++) {
        uint8_t count = 0;
        if (!rd_reg(KB_REG_EVENT_NUM, &count, 1))
            return false;
        if (count == 0 || count > 32)
            break;
        while (count--) {
            uint8_t ev;
            if (!rd_reg(KB_REG_KEY_EVENT, &ev, 1))
                return false;
            if (ev == 0xFF)
                break; /* queue empty */
            handle_key_event(ev);
        }
    }
    return wr_reg(KB_REG_INT_STA, 0); /* release the INT line */
}

static void IRAM_ATTR kb_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_int_sem, &hp);
    if (hp)
        portYIELD_FROM_ISR();
}

/* Dock answering? Switch to Normal mode + custom RGB, clean queue. */
static bool probe_and_init(void)
{
    if (i2c_master_probe(s_bus, KB_ADDR, 20) != ESP_OK)
        return false;
    uint8_t ver = 0;
    if (!rd_reg(KB_REG_VERSION, &ver, 1))
        return false;
    if (!wr_reg(KB_REG_MODE, KB_MODE_NORMAL))
        return false;
    (void)wr_reg(KB_REG_INT_CFG, KB_INT_NORMAL_BIT);
    (void)wr_reg(KB_REG_EVENT_NUM, 0); /* clear queue */
    (void)wr_reg(KB_REG_INT_STA, 0);
    (void)wr_reg(KB_REG_RGB_MODE, KB_RGB_CUSTOM); /* we own the LEDs */
    /* fresh attach: no modifier can be known-held; reset + LEDs off */
    s_sym_held = s_ctrl_held = s_alt_held = false;
    s_aa_held = s_aa_lock = s_aa_oneshot = false;
    s_held = false;
    s_led_rgb = 1;
    led_update();
    ESP_LOGI(TAG, "dock attached (fw v%u)", ver);
    return true;
}

static void set_present(bool present)
{
    if (s_present == present)
        return;
    s_present = present;
    s_held = false;
    if (!present)
        ESP_LOGI(TAG, "dock detached");
    if (s_presence_cb)
        s_presence_cb(present);
}

static void kbd_task(void *arg)
{
    (void)arg;
    int errs = 0;
    for (;;) {
        if (!s_present) {
            if (probe_and_init()) {
                errs = 0;
                xSemaphoreTake(s_int_sem, 0); /* eat stale edges */
                set_present(true);
            } else {
                vTaskDelay(pdMS_TO_TICKS(KB_PROBE_MS));
            }
            continue;
        }

        TickType_t wait = pdMS_TO_TICKS(s_held ? KB_REPEAT_TICK_MS
                                                : KB_POLL_MS);
        (void)xSemaphoreTake(s_int_sem, wait);

        if (!drain_events()) {
            if (++errs >= KB_ERR_LIMIT)
                set_present(false);
            continue;
        }
        errs = 0;

        if (s_held && esp_timer_get_time() >= s_next_rep_us) {
            mqjs_post_key(s_rep_seq, s_rep_len);
            s_next_rep_us = esp_timer_get_time()
                            + (int64_t)KB_REPEAT_TICK_MS * 1000;
        }
    }
}

/* ---- public API ------------------------------------------------------ */

bool kbd_tab5_present(void)
{
    return s_present;
}

void kbd_tab5_set_presence_cb(kbd_tab5_presence_cb_t cb)
{
    s_presence_cb = cb;
}

void kbd_tab5_start(void)
{
    if (s_int_sem)
        return; /* already running */

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0, /* port 1 = internal (touch/codec) bus */
        .sda_io_num = KB_SDA,
        .scl_io_num = KB_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus failed (no dock support)");
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KB_ADDR,
        .scl_speed_hz = KB_I2C_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "i2c dev failed (no dock support)");
        return;
    }

    s_int_sem = xSemaphoreCreateBinary();

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << KB_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    /* mqjs' gpio binding may have installed the service already */
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "isr service: %s (poll only)", esp_err_to_name(e));
    gpio_isr_handler_add(KB_INT_GPIO, kb_isr, NULL);

    xTaskCreatePinnedToCore(kbd_task, "kbd_tab5", 4096, NULL, 5, NULL, 1);
}

#endif /* CONFIG_MQJS_TAB5_KEYBOARD */
