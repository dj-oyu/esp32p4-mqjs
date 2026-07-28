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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "kbd_core.h"
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
#define KB_REG_RGB1      0x60 /* RGB1_B,G,R at 0x60..0x62 */
#define KB_REG_RGB2      0x64 /* RGB2_B,G,R at 0x64..0x66 — NOT 0x63!
                                 Device-verified: a 6-byte write at
                                 0x60 shifted LED2 by one channel (red
                                 came out green); the protocol sheet's
                                 register grid indeed leaves 0x63
                                 empty. */
#define KB_REG_VERSION   0xFE

#define KB_MODE_NORMAL    0
#define KB_RGB_CUSTOM     1
#define KB_INT_NORMAL_BIT 0x01

#define KB_PROBE_MS        2000 /* absent: how often to look for the dock */
#define KB_POLL_MS         100  /* present: fallback INT_STA poll */
#define KB_REPEAT_DELAY_MS 400  /* typematic: first repeat */
#define KB_REPEAT_TICK_MS  55   /* typematic: rate (~18 cps) */
#define KB_ERR_LIMIT       3    /* consecutive comm errors = detached */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_int_sem;
static volatile bool s_present;
static kbd_tab5_presence_cb_t s_presence_cb;

/* Modifier state (kbd task only). The state machine and the
   translation live in kbd_core, shared with the on-screen keyboard so
   both surfaces put identical bytes on the wire. */
static kbd_mods_t s_mods;

/* typematic state */
static bool s_held;
static uint8_t s_held_row, s_held_col;
static char s_rep_seq[KBD_SEQ_MAX];
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
 * Two WS2812s via the dock's RGB custom mode, split by DURATION rather
 * than by which key, so the transient and the latched state can never
 * be confused (they were indistinguishable while both shared one LED):
 *   LED1 (left)  = active for the next keystroke only — a physically
 *                  held modifier, or Aa's one-shot.
 *   LED2 (right) = LATCHED by double-tap; stays lit hands-off until
 *                  tapped again.
 * Colors in both: sym=blue, ctrl=cyan, alt=gray, Aa=red. */
#define KB_COL_SYM  0x0000FF
#define KB_COL_CTRL 0x00FFFF
#define KB_COL_ALT  0x808080
#define KB_COL_AA   0xFF0000

static uint64_t s_led_state = 1; /* impossible initial: force 1st write */

static void led_update(void)
{
    uint32_t led1 = s_mods.sym.held  ? KB_COL_SYM
                    : s_mods.ctrl.held ? KB_COL_CTRL
                    : s_mods.alt.held  ? KB_COL_ALT
                    : (s_mods.shift.held || s_mods.shift.oneshot)
                        ? KB_COL_AA
                        : 0x000000;
    uint32_t led2 = s_mods.sym.lock    ? KB_COL_SYM
                    : s_mods.ctrl.lock ? KB_COL_CTRL
                    : s_mods.alt.lock  ? KB_COL_ALT
                    : s_mods.shift.lock ? KB_COL_AA
                                        : 0x000000;

    uint64_t state = ((uint64_t)led1 << 24) | led2;
    if (state == s_led_state)
        return;
    uint8_t bgr1[3] = { (uint8_t)led1, (uint8_t)(led1 >> 8),
                        (uint8_t)(led1 >> 16) };
    uint8_t bgr2[3] = { (uint8_t)led2, (uint8_t)(led2 >> 8),
                        (uint8_t)(led2 >> 16) };
    if (wr_regs(KB_REG_RGB1, bgr1, 3) && wr_regs(KB_REG_RGB2, bgr2, 3))
        s_led_state = state;
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

/* dock cell code -> shared named-key id (kbd_core decides what each one
   becomes on the wire, so the on-screen keyboard agrees byte for byte) */
static kbd_key_t cell_key(uint8_t code)
{
    switch (code) {
    case K_ESC:   return KBD_K_ESC;
    case K_DEL:   return KBD_K_DEL;
    case K_TAB:   return KBD_K_TAB;
    case K_BS:    return KBD_K_BS;
    case K_ENTER: return KBD_K_ENTER;
    case K_UP:    return KBD_K_UP;
    case K_LEFT:  return KBD_K_LEFT;
    case K_DOWN:  return KBD_K_DOWN;
    case K_RIGHT: return KBD_K_RIGHT;
    default:      return KBD_K_NONE;
    }
}

/* ---- event handling -------------------------------------------------- */

static void handle_key_event(uint8_t ev)
{
    bool pressed = (ev & 0x80) != 0;
    uint8_t row = (ev >> 4) & 0x07, col = ev & 0x0F;
    if (row > 4 || col > 13)
        return;

    /* Modifiers run kbd_core's hold / tap-one-shot / double-tap-lock
       machine. Only Aa arms a one-shot from a bare tap: a stray Ctrl tap
       plus a later 'c' would read as SIGINT. */
    kbd_mod_t *mod = NULL;
    bool oneshot_arm = false;
    switch (s_map[row][col]) {
    case K_SYM:  mod = &s_mods.sym; break;
    case K_CTRL: mod = &s_mods.ctrl; break;
    case K_ALT:  mod = &s_mods.alt; break;
    case K_AA:   mod = &s_mods.shift; oneshot_arm = true; break;
    default: break;
    }
    if (mod) {
        kbd_mod_edge(mod, pressed, oneshot_arm);
        led_update();
        return;
    }

    if (!pressed) {
        /* only the held key's own release stops its repeat */
        if (s_held && row == s_held_row && col == s_held_col)
            s_held = false;
        return;
    }

    /* a real key while a modifier is held: that press was a chord, not
       a tap — it must never count toward a double-tap lock */
    kbd_mods_mark_chord(&s_mods);

    uint8_t code =
        (kbd_mod_active(&s_mods.sym) ? s_map_sym : s_map)[row][col];
    char seq[KBD_SEQ_MAX];
    bool repeats = false;
    size_t n = kbd_translate(&s_mods, code < 0x80 ? (char)code : 0,
                             cell_key(code), seq, &repeats);
    if (!n)
        return;
    mqjs_post_key(seq, n);
    kbd_mods_consume(&s_mods); /* one-shots are spent */
    led_update();

    s_held = repeats;
    if (repeats) {
        s_held_row = row;
        s_held_col = col;
        memcpy(s_rep_seq, seq, n);
        s_rep_len = n;
        s_next_rep_us = esp_timer_get_time()
                        + (int64_t)KB_REPEAT_DELAY_MS * 1000;
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

/* Dock answering? Switch to Normal mode + custom RGB, clean queue.
 *
 * DO NOT reintroduce i2c_master_probe() here. It panics this board.
 * IDF's i2c_master_probe() (6.0.1, still on master) differs from every
 * other transaction entry point in two fatal ways:
 *   - it publishes its own STACK-LOCAL 2-entry ops array into
 *     bus->i2c_trans.ops (dangling as soon as it returns), and
 *   - it does not reset read_buf_pos / read_len_static / contains_read,
 *     which s_i2c_transaction_start() does for every real transaction.
 * So a read that left contains_read=1 (a poll whose completion IRQ never
 * ran the STOP-phase handler) makes the NEXT probe's completion IRQ take
 * i2c_isr_receive_handler()'s non-READ branch and index ops[read_buf_pos]
 * — read_buf_pos is 4 after any 1-byte transmit_receive — i.e. 64 bytes
 * past a 2-entry array that lives in this task's stack. It then stores
 * RX FIFO bytes through whatever pointer it finds there:
 *   Store access fault, MTVAL 0x000030f0, i2c_master.c:766.
 * Device-verified on Tab5 (I2C0 = the pogo/dock bus): 2-3 panics per
 * 150 s once Wi-Fi/microlink is up. A plain register read is the
 * presence test instead — it runs the ordinary synchronous path, which
 * uses the driver-owned ops array and resets the ISR read state. A
 * NACK from an absent dock is only ESP_LOGD, so this is not noisier. */
static bool probe_and_init(void)
{
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
    kbd_mods_reset(&s_mods);
    s_held = false;
    s_led_state = 1;
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
