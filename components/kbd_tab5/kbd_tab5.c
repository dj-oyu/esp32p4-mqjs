/*
 * M5Stack Tab5 keyboard dock (A164): STM32F030 scanner at I2C 0x6D on
 * the pogo connector (SDA=GPIO0 SCL=GPIO1, INT=GPIO50 active-low).
 *
 * Mode choice: the dock firmware offers Normal (row/col), HID
 * (modifier+keycode, press AND release events) and Character (ready
 * strings, press only). We run HID: release events make host-side
 * typematic repeat possible (Character mode never says when a key is
 * let go), and the dock still resolves its own sym/Aa layer for us —
 * a shifted keycap arrives as LSHIFT|keycode, so one standard US
 * HID→ASCII table covers the whole 70-key layout.
 *
 * Event flow (single task, no I2C from ISRs):
 *   GPIO50 negedge ISR -> semaphore -> drain INT_STA/EVENT_NUM/0x30
 *   queue -> translate -> mqjs_post_key(). A 100 ms fallback poll
 *   covers edges missed while the queue was already asserted, and a
 *   2 s probe loop covers hot-plug both ways.
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
#define KB_REG_HID_EVENT 0x30
#define KB_REG_VERSION   0xFE

#define KB_MODE_HID      1
#define KB_INT_HID_BIT   0x02

/* HID modifier byte: L/R variants merged */
#define KB_MOD_CTRL  0x11
#define KB_MOD_SHIFT 0x22
#define KB_MOD_ALT   0x44

#define KB_PROBE_MS       2000 /* absent: how often to look for the dock */
#define KB_POLL_MS        100  /* present: fallback INT_STA poll */
#define KB_REPEAT_DELAY_MS 400 /* typematic: first repeat */
#define KB_REPEAT_TICK_MS  55  /* typematic: rate (~18 cps) */
#define KB_ERR_LIMIT      3    /* consecutive comm errors = detached */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_int_sem;
static volatile bool s_present;
static kbd_tab5_presence_cb_t s_presence_cb;

/* typematic state (task-local use only) */
static bool s_held;
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

/* ---- HID usage id -> ui.onKey sequence ------------------------------ */

/* keycodes 0x04 (A) .. 0x38 (SLASH): plain / shifted ASCII */
static const char s_ascii_map[0x39 - 0x04][2] = {
    { 'a', 'A' }, { 'b', 'B' }, { 'c', 'C' }, { 'd', 'D' }, { 'e', 'E' },
    { 'f', 'F' }, { 'g', 'G' }, { 'h', 'H' }, { 'i', 'I' }, { 'j', 'J' },
    { 'k', 'K' }, { 'l', 'L' }, { 'm', 'M' }, { 'n', 'N' }, { 'o', 'O' },
    { 'p', 'P' }, { 'q', 'Q' }, { 'r', 'R' }, { 's', 'S' }, { 't', 'T' },
    { 'u', 'U' }, { 'v', 'V' }, { 'w', 'W' }, { 'x', 'X' }, { 'y', 'Y' },
    { 'z', 'Z' },
    { '1', '!' }, { '2', '@' }, { '3', '#' }, { '4', '$' }, { '5', '%' },
    { '6', '^' }, { '7', '&' }, { '8', '*' }, { '9', '(' }, { '0', ')' },
    { '\n', '\n' },   /* ENTER */
    { 0, 0 },         /* ESC   -> token below */
    { '\b', '\b' },   /* BACKSPACE */
    { '\t', '\t' },   /* TAB */
    { ' ', ' ' },     /* SPACE */
    { '-', '_' }, { '=', '+' }, { '[', '{' }, { ']', '}' }, { '\\', '|' },
    { '#', '~' },     /* non-US # */
    { ';', ':' }, { '\'', '"' }, { '`', '~' }, { ',', '<' }, { '.', '>' },
    { '/', '?' },
};

/* keys the JS side names by "\x00token" (control-bar vocabulary; the
   terminal expands them via its own xterm table) */
static const char *token_for(uint8_t code)
{
    switch (code) {
    case 0x29: return "esc";
    case 0x3a: return "f1";  case 0x3b: return "f2";
    case 0x3c: return "f3";  case 0x3d: return "f4";
    case 0x3e: return "f5";  case 0x3f: return "f6";
    case 0x40: return "f7";  case 0x41: return "f8";
    case 0x42: return "f9";  case 0x43: return "f10";
    case 0x44: return "f11"; case 0x45: return "f12";
    case 0x49: return "ins";  case 0x4a: return "home";
    case 0x4b: return "pgup"; case 0x4c: return "del";
    case 0x4d: return "end";  case 0x4e: return "pgdn";
    case 0x4f: return "right"; case 0x50: return "left";
    case 0x51: return "down";  case 0x52: return "up";
    default:   return NULL;
    }
}

/* tokens that make sense to auto-repeat while held */
static bool token_repeats(uint8_t code)
{
    return code == 0x4c /* del */ ||
           (code >= 0x4f && code <= 0x52) /* arrows */ ||
           code == 0x4b || code == 0x4e;  /* pgup/pgdn */
}

/* Build the mqjs_post_key sequence for one HID event.
 * Returns length (0 = nothing to post), sets *repeats. */
static size_t translate(uint8_t mod, uint8_t code, char out[8], bool *repeats)
{
    *repeats = false;

    const char *tok = token_for(code);
    if (tok) { /* Ctrl/Alt on named keys: dropped for now */
        size_t n = strlen(tok);
        out[0] = '\0';
        memcpy(out + 1, tok, n);
        *repeats = token_repeats(code);
        return n + 1;
    }

    if (code < 0x04 || code >= 0x39)
        return 0; /* rollover error, media keys, bare modifiers */

    char c = s_ascii_map[code - 0x04][(mod & KB_MOD_SHIFT) ? 1 : 0];
    if (!c)
        return 0;

    size_t n = 0;
    if (mod & KB_MOD_ALT)
        out[n++] = '\x1b'; /* Meta = ESC prefix (matches the terminal) */

    if (mod & KB_MOD_CTRL) {
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

    /* repeat printables and edit keys, not Enter/Esc */
    *repeats = (c != '\n');
    return n;
}

/* ---- event pump ----------------------------------------------------- */

static void handle_hid_event(uint8_t mod, uint8_t code)
{
    if (code == 0x00) { /* release (dock reports keycode 0) */
        s_held = false;
        return;
    }
    if (code == 0x01) /* roll-over overflow marker */
        return;

    char seq[8];
    bool repeats = false;
    size_t n = translate(mod, code, seq, &repeats);
    if (!n)
        return;
    mqjs_post_key(seq, n);

    s_held = repeats;
    if (repeats) {
        memcpy(s_rep_seq, seq, n);
        s_rep_len = n;
        s_next_rep_us = esp_timer_get_time() + (int64_t)KB_REPEAT_DELAY_MS * 1000;
    }
}

/* Drain the dock's HID queue. false = I2C died (dock likely detached). */
static bool drain_events(void)
{
    uint8_t sta = 0;
    if (!rd_reg(KB_REG_INT_STA, &sta, 1))
        return false;
    if (!(sta & KB_INT_HID_BIT))
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
            uint8_t ev[2];
            if (!rd_reg(KB_REG_HID_EVENT, ev, 2))
                return false;
            if (ev[0] == 0xFF && ev[1] == 0xFF)
                break; /* queue empty */
            handle_hid_event(ev[0], ev[1]);
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

/* Dock answering? Then switch it to HID mode with a clean queue. */
static bool probe_and_init(void)
{
    if (i2c_master_probe(s_bus, KB_ADDR, 20) != ESP_OK)
        return false;
    uint8_t ver = 0;
    if (!rd_reg(KB_REG_VERSION, &ver, 1))
        return false;
    if (!wr_reg(KB_REG_MODE, KB_MODE_HID))
        return false;
    (void)wr_reg(KB_REG_INT_CFG, KB_INT_HID_BIT); /* only HID irqs */
    (void)wr_reg(KB_REG_EVENT_NUM, 0);            /* clear queue */
    (void)wr_reg(KB_REG_INT_STA, 0);
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
