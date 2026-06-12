/*
 * Tab5 on-device UI, Phase 1: status bar + JS console on LVGL.
 *
 * Hardware values mirror the M5Tab5-UserDemo BSP (m5stack_tab5.c):
 *  - 5" 720x1280 MIPI-DSI panel, ILI9881C/ST7121/ST7123 controller
 *    depending on the production lot (detected at boot via the touch
 *    controller: GT911 -> ILI9881C, 0x55 -> ST712x)
 *  - LCD_RST (P4) and TP_RST (P5) sit on a PI4IOE5V6408 IO expander at
 *    0x43 on the internal I2C bus (SDA=G31 SCL=G32) — the 0x44 expander
 *    (C6 power) is handled separately by main/board_tab5.c
 *  - backlight is LEDC PWM on GPIO22 (12bit @5kHz)
 *
 * Threading (design doc §2): js_task (Core 0) and the other writers only
 * touch the mutex-guarded data plane below (line ring + status
 * snapshot). All LVGL work happens in the esp_lvgl_port task (Core 1,
 * low priority); an lv_timer drives Mooncake::update(), which runs the
 * StatusBar/ConsoleApp abilities. Mooncake is lifecycle glue only —
 * widgets and state live in this file.
 */
#include "sdkconfig.h"
#if CONFIG_MQJS_TAB5_UI

#include <memory>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "core/animation/animate_value/animate_value.hpp"
#include "mooncake.h"

#include "ui_tab5.h"
#include "ui_tab5_internal.h"

#include "esp_lcd_st7121.h"
#include "esp_lcd_st7123.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_st7123.h"

/* mqjs public API (extern decl instead of REQUIRES: mqjs already
   depends on this component for ui_tab5.h, same trick as wifi.c) */
extern "C" void mqjs_post_touch(int x, int y, int kind);
extern "C" void mqjs_post_key(const char *utf8, size_t len);
extern "C" void mqjs_focus(int slot);
extern "C" void mqjs_request_open(const char *name);

#include "ili9881_init_data.inc"
#include "st7123_init_data.inc"

/* Noto Sans CJK JP subset, fonts/font_noto_jp_20_4.c (compiled as C).
 * The hiz8 min TTF it was generated from has no glyph for U+0020 space
 * (or U+0022) — they render as tofu. ui_font() returns a mutable copy
 * with a fallback chain:
 *   Noto JP 20 -> Montserrat 20 (ASCII gaps + LV_SYMBOL FontAwesome)
 *              -> HackGen NF icons 20px (Nerd Font BMP ranges)
 * The NF link makes icon glyphs available to EVERY text surface
 * (status bar, widgets, console lines, ui.text) — apps just put
 * "\uE7xx"-style characters in strings (system decoration / @icon). */
extern "C" {
LV_FONT_DECLARE(font_noto_jp_20_4);
LV_FONT_DECLARE(font_nf_ui_20);
}

static const lv_font_t *ui_font(void)
{
    static lv_font_t jp, mont;
    if (!jp.line_height) {
        mont = lv_font_montserrat_20;
        mont.fallback = &font_nf_ui_20;
        jp = font_noto_jp_20_4;
        jp.fallback = &mont;
    }
    return &jp;
}

/* same font for the widget layer (ui_widgets.cpp) */
const lv_font_t *ui_tab5_jp_font(void)
{
    return ui_font();
}

/* Monospace terminal font (HackGen Console NF, fonts/font_term_mono.c;
 * includes the Nerd Font BMP icon ranges, cell-fitted by upstream).
 * Fixed cell grid for ui.cells/UI_CMD_CELLS: 9px advance (720/9 = 80 cols),
 * 24px line height. Glyphs are blitted directly (no lv_draw_label) and
 * clipped to the cell, so the box-drawing overhang (box_w up to 11) tiles
 * cleanly across cell edges. */
#include "driver/ppa.h"

extern "C" {
LV_FONT_DECLARE(font_term_mono);
}
#define UI_CELL_W 9
#define UI_CELL_H 24

/* PPA fill offload (device-measured 2026-06-12, main/ppa_bench.c): CPU
   fills are PSRAM-bandwidth-bound at ~43 Mpix/s regardless of store
   width, ppa_do_fill sustains ~175; the blocking-op overhead (~60us)
   puts the break-even near 2.5k px. Rects below the threshold (and all
   glyph blits) stay on the CPU, which wins there. */
#define UI_PPA_FILL_MIN_PX 4096
static ppa_client_handle_t s_ppa_fill;

/* PPA blend offload for cells runs (same bench: CPU A4 blend ~8 Mpix/s
   at -O2, PPA ~45 at row sizes, break-even ~900px ≈ 4 cells; 6 leaves
   margin for the compose pass). The run's glyph coverage is composed
   into an A8 buffer in internal SRAM — A8, not A4, because the PPA
   expands A4 alpha by <<4 (a=15 -> 240/255, never fully opaque,
   probed 2026-06-12 in main/ppa_bench.c) while A8 a4*17=255 matches
   ui_blend565's a/15 math exactly. One full text row max. */
#define UI_PPA_CELLS_MIN_CELLS 6
static ppa_client_handle_t s_ppa_blend;
/* 720 = canvas width; 64B-aligned (and 64B-multiple) for PPA cache ops */
static uint8_t s_cells_a8[720 * UI_CELL_H] __attribute__((aligned(64)));

/* minimal UTF-8 decode, shared by both cells paths */
static inline uint32_t cells_utf8_next(const uint8_t *&s)
{
    uint32_t cp = *s++;
    if (cp >= 0xF0 && (s[0] & 0xC0) == 0x80) {
        cp = ((cp & 0x07) << 18) | ((s[0] & 0x3F) << 12) |
             ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        s += 3;
    } else if (cp >= 0xE0 && (s[0] & 0xC0) == 0x80) {
        cp = ((cp & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
        s += 2;
    } else if (cp >= 0xC0 && (s[0] & 0xC0) == 0x80) {
        cp = ((cp & 0x1F) << 6) | (s[0] & 0x3F);
        s += 1;
    }
    return cp;
}

/* blend fg over bg on RGB565 by a 4-bit alpha (0=bg .. 15=fg) */
static inline uint16_t ui_blend565(uint16_t bg, uint16_t fg, int a)
{
    int br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    int fr = (fg >> 11) & 0x1F, fgc = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    int r = br + ((fr - br) * a) / 15;
    int g = bgc + ((fgc - bgc) * a) / 15;
    int b = bb + ((fb - bb) * a) / 15;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static const char *TAG = "ui_tab5";

/* ------------------------------------------------------------------ */
/* Data plane: console line ring + status snapshot                     */
/* Writers run on js_task (print sink) / wifi / mqtt tasks; the only   */
/* reader is the LVGL task. No LVGL calls on the writer side, ever.    */
/* ------------------------------------------------------------------ */

#define UI_LOG_LINES      200
#define UI_LOG_LINE_BYTES 256 /* the print sink splits lines at 256 bytes */
#define UI_LOG_LINE_STORE 320 /* + headroom for LVGL recolor markup       */

typedef struct {
    char text[UI_LOG_LINE_STORE + 2]; /* +closing '#' +NUL */
} ui_log_line_t;

static ui_log_line_t s_log[UI_LOG_LINES];
static uint32_t s_log_head; /* total lines ever written (monotonic) */
static SemaphoreHandle_t s_log_mtx;

static ui_status_t s_status;
static uint32_t s_status_gen; /* bumped on every snapshot */
static SemaphoreHandle_t s_status_mtx;

/* P4b: foreground/previous app names for the bar chip (same guard) */
typedef struct {
    char cur[32];
    char prev[32];
    bool prev_running;
} ui_fgapps_t;
static ui_fgapps_t s_fgapps;
static uint32_t s_fgapps_gen;

/* ANSI 16-color palette tuned for the dark console background */
static const uint32_t ansi_palette[16] = {
    0x55606B, 0xE05A4E, 0x2ECC71, 0xFFD479, /* 30-33 blk red grn yel */
    0x4FC3F7, 0xC678DD, 0x56B6C2, 0xC9D1D9, /* 34-37 blu mag cyn wht */
    0x8B98A5, 0xFF6B5E, 0x4AE38A, 0xFFE08A, /* 90-93 bright          */
    0x6FD3FF, 0xD898E8, 0x7FD8E0, 0xFFFFFF, /* 94-97                 */
};

/* SGR parameter list ("31", "0;92", "38;5;196", ...) -> color state.
   Only foreground colors are mapped (label recolor has no bg);
   38;5;N / 38;2;r;g;b arguments are consumed but approximated/ignored
   beyond the 16-color palette. */
static void sgr_apply(const char *p, uint32_t *color, bool *has_color)
{
    int skip = 0;
    while (*p) {
        int v = 0;
        bool any = false;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p++ - '0');
            any = true;
        }
        if (*p == ';')
            p++;
        if (!any)
            v = 0;
        if (skip > 0) {
            skip--;
            continue;
        }
        if (v == 0 || v == 39)
            *has_color = false;
        else if (v >= 30 && v <= 37) {
            *color = ansi_palette[v - 30];
            *has_color = true;
        } else if (v >= 90 && v <= 97) {
            *color = ansi_palette[v - 90 + 8];
            *has_color = true;
        } else if (v == 38 || v == 48) {
            /* extended color: "5;N" or "2;r;g;b" follows */
            int mode = 0;
            while (*p >= '0' && *p <= '9')
                mode = mode * 10 + (*p++ - '0');
            if (*p == ';')
                p++;
            skip = (mode == 2) ? 3 : 1;
        }
        /* bold/underline/bg etc.: ignored */
    }
}

/* print sink, called on js_task: copy into the ring and return. The
   short timeout (instead of portMAX_DELAY) means a stuck UI task can
   never stall JS execution; worst case the line is dropped.

   Sanitizing/translation on the way in:
   - ANSI SGR color sequences become LVGL recolor markup
     ("#RRGGBB text#"); color state persists across lines like a
     terminal. Other CSI sequences (cursor movement etc.) are skipped.
   - \t becomes two spaces (no tab glyph, LVGL doesn't expand tabs),
     other C0 controls are dropped, literal '#' is escaped for recolor.
   The CSI state survives across calls because the sink may split one
   logical line at 96 bytes mid-sequence. */
extern "C" void ui_tab5_log(const char *line, size_t n)
{
    /* js_task is the only producer; states are static on purpose */
    static bool in_csi;
    static char csi[24];
    static size_t csi_len;
    static uint32_t cur_color;
    static bool has_color;

    if (!s_log_mtx || !line || !n)
        return;
    if (xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(20)) != pdTRUE)
        return;
    char *slot = s_log[s_log_head % UI_LOG_LINES].text;
    size_t o = 0;
    bool span_open = false;
    if (has_color) { /* color carried over from the previous line */
        o += snprintf(slot, 12, "#%06X ", (unsigned)cur_color);
        span_open = true;
    }
    for (size_t i = 0; i < n && o < UI_LOG_LINE_STORE; i++) {
        unsigned char c = (unsigned char)line[i];
        if (in_csi) {
            if (c >= 0x40 && c <= 0x7E) { /* final byte */
                in_csi = false;
                if (c == 'm') {
                    csi[csi_len] = '\0';
                    sgr_apply(csi, &cur_color, &has_color);
                    if (span_open) {
                        slot[o++] = '#';
                        span_open = false;
                    }
                    if (has_color && o + 9 <= UI_LOG_LINE_STORE) {
                        o += snprintf(slot + o, 10, "#%06X ",
                                      (unsigned)cur_color);
                        span_open = true;
                    }
                }
            } else if (csi_len < sizeof csi - 1) {
                csi[csi_len++] = (char)c;
            }
            continue;
        }
        if (c == 0x1B) {
            /* ESC [ ... <final>; a lone ESC X just drops both bytes */
            if (i + 1 < n && line[i + 1] == '[') {
                in_csi = true;
                csi_len = 0;
            }
            i++;
            continue;
        }
        if (c == '\t') {
            /* 4 spaces: 2 was too easy to mistake for a single space */
            for (int t = 0; t < 4 && o < UI_LOG_LINE_STORE; t++)
                slot[o++] = ' ';
            continue;
        }
        if (c < 0x20)
            continue; /* other control chars: no glyph, drop */
        if (c == '#') { /* recolor is on: escape literal '#' */
            slot[o++] = '#';
            if (o < UI_LOG_LINE_STORE)
                slot[o++] = '#';
            continue;
        }
        slot[o++] = (char)c;
    }
    /* expansion can overflow the slot: trim a torn UTF-8 tail */
    size_t k = o;
    while (k > 0 && ((unsigned char)slot[k - 1] & 0xC0) == 0x80)
        k--;
    if (k > 0 && ((unsigned char)slot[k - 1] & 0x80)) {
        unsigned char lead = (unsigned char)slot[k - 1];
        size_t need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
        if (o - (k - 1) < need)
            o = k - 1;
    }
    if (span_open)
        slot[o++] = '#'; /* the slot reserves 2 bytes for this + NUL */
    slot[o] = '\0';
    if (o)
        s_log_head++;
    xSemaphoreGive(s_log_mtx);
}

extern "C" void ui_tab5_set_status(const ui_status_t *st)
{
    if (!s_status_mtx || !st)
        return;
    xSemaphoreTake(s_status_mtx, portMAX_DELAY);
    s_status = *st;
    s_status_gen++;
    xSemaphoreGive(s_status_mtx);
}

extern "C" void ui_tab5_set_fg_apps(const char *cur, const char *prev,
                                    bool prev_running)
{
    if (!s_status_mtx)
        return;
    xSemaphoreTake(s_status_mtx, portMAX_DELAY);
    strlcpy(s_fgapps.cur, cur ? cur : "", sizeof s_fgapps.cur);
    strlcpy(s_fgapps.prev, prev ? prev : "", sizeof s_fgapps.prev);
    s_fgapps.prev_running = prev_running;
    s_fgapps_gen++;
    xSemaphoreGive(s_status_mtx);
}

/* drawing command queue (Phase 2): js_task posts, CanvasApp drains.
   Non-blocking by design — a full queue drops the command and bumps a
   counter that the status bar displays (visible backpressure, JS is
   never stalled). */
#define UI_CMD_QUEUE_DEPTH 128 /* 16B each; static scenes burst >64 */

static QueueHandle_t s_cmd_queue;
static volatile uint32_t s_cmd_drops;
static int s_canvas_w, s_canvas_h; /* set once the display is up */

extern "C" bool ui_tab5_cmd(const ui_cmd_t *cmd)
{
    if (!s_cmd_queue || !cmd)
        return false;
    if (xQueueSend(s_cmd_queue, cmd, 0) != pdTRUE) {
        s_cmd_drops = s_cmd_drops + 1;
        return false; /* caller keeps ownership of cmd->text */
    }
    return true;
}

extern "C" void ui_tab5_canvas_size(int *w, int *h)
{
    *w = s_canvas_w;
    *h = s_canvas_h;
}

extern "C" void ui_tab5_cell_size(int *w, int *h)
{
    *w = s_canvas_w ? UI_CELL_W : 0;
    *h = s_canvas_w ? UI_CELL_H : 0;
}

/* Synchronous metric query for the JS terminal-emulator work (Phase 4):
   ui.textSize() needs an answer, not a queued command. Called on
   js_task while the LVGL task renders with the same font — fine,
   because glyph dsc lookup in fmt_txt fonts only reads const tables
   (no cache in LVGL 9; bitmap decoding, which does touch caches, is
   never reached by lv_text_get_size). */
extern "C" void ui_tab5_text_size(const char *utf8, int *w, int *h)
{
    *w = 0;
    *h = 0;
    if (!s_canvas_w || !utf8)
        return;
    lv_point_t size;
    lv_text_get_size(&size, utf8, ui_font(), 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    *w = size.x;
    *h = size.y;
}

/*
 * esp-hosted 2.x runs its full transport init from a pre-scheduler C
 * constructor. At that point only the small early heap exists (no PSRAM,
 * ~110KB internal DMA RAM) and its two SDIO mempools need ~90KB of it —
 * this component's extra .bss (LVGL et al.) pushed that over the edge:
 * boot died in "assert failed: sdio_mempool_create ... (buf_mp_g)".
 *
 * Fix: wrap esp_hosted_init (-Wl,--wrap, only when MQJS_TAB5_UI=y) so
 * the constructor-time call becomes a no-op; main/wifi.c calls it again
 * once the scheduler is up (esp_hosted_init_done makes that the real,
 * one-and-only init). Side benefit: on Tab5 the C6 is power-gated off
 * until board_tab5_power_init(), so constructor-time init was always
 * too early on this board.
 */
extern "C" int __real_esp_hosted_init(void);
extern "C" int __wrap_esp_hosted_init(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        ESP_EARLY_LOGW(TAG, "deferring esp_hosted_init to after startup");
        return ESP_OK;
    }
    return __real_esp_hosted_init();
}

/* --- Tab5 display constants (M5Tab5-UserDemo BSP) --- */
#define UI_LCD_H_RES        720
#define UI_LCD_V_RES        1280
#define UI_DSI_LANES        2
#define UI_DPHY_LDO_CHAN    3
#define UI_DPHY_LDO_MV      2500
#define UI_BACKLIGHT_GPIO   22
#define UI_LVGL_BUF_LINES   50
/* UI_STATUSBAR_H lives in ui_tab5_internal.h (shared with ui_widgets.cpp) */

/* Tab5 shipped with different panels over time; the variant is identified
 * by which touch controller answers on the internal I2C bus
 * (GT911 -> ILI9881C, 0x55 + fw version -> ST7121/ST7123). */
typedef enum {
    UI_PANEL_NONE = 0,
    UI_PANEL_ILI9881C, /* 730Mbps lanes, DPI 60MHz */
    UI_PANEL_ST7121,   /* 965Mbps lanes, DPI 70MHz */
    UI_PANEL_ST7123,
} ui_panel_variant_t;

/* internal I2C bus */
#define UI_I2C_PORT         0
#define UI_I2C_SDA          31
#define UI_I2C_SCL          32
#define UI_PI4IOE1_ADDR     0x43 /* P4=LCD_RST, P5=TP_RST */
#define UI_GT911_ADDR       0x5D /* primary GT911 address */
#define UI_GT911_ADDR_BKP   0x14
#define UI_ST7123_TP_ADDR   0x55 /* touch of the ST712x panel variant */

static uint8_t s_gt911_addr = UI_GT911_ADDR; /* whichever addr answered */

/* Touch fw version register 0x0000 on the 0x55 controller tells the
 * ST712x flavour apart: 1 = ST7121, 3 (or anything else) = ST7123. */
static ui_panel_variant_t st712x_flavour(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = UI_ST7123_TP_ADDR;
    dev_cfg.scl_speed_hz = 100000;

    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK)
        return UI_PANEL_ST7123;

    const uint8_t reg[2] = { 0x00, 0x00 };
    uint8_t fw = 0xFF;
    esp_err_t err = i2c_master_transmit_receive(dev, reg, 2, &fw, 1, 100);
    i2c_master_bus_rm_device(dev);

    ESP_LOGI(TAG, "ST712x touch fw version: %u (%s)", fw,
             esp_err_to_name(err));
    return (err == ESP_OK && fw == 1) ? UI_PANEL_ST7121 : UI_PANEL_ST7123;
}

/*
 * Release LCD/TP reset via the 0x43 expander and sniff which touch
 * controller answers (tells us the panel variant). The bus is created
 * and deleted again so JS i2c.setup(0, ...) can claim the port later.
 */
static ui_panel_variant_t panel_reset_and_detect(void)
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = UI_I2C_PORT;
    bus_cfg.sda_io_num = (gpio_num_t)UI_I2C_SDA;
    bus_cfg.scl_io_num = (gpio_num_t)UI_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus for IO expander failed");
        return UI_PANEL_NONE;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = UI_PI4IOE1_ADDR;
    dev_cfg.scl_speed_hz = 400000;

    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "IO expander 0x43 not reachable");
        i2c_del_master_bus(bus);
        return UI_PANEL_NONE;
    }

    /* register writes mirror bsp_io_expander_pi4ioe_init() (0x43 half):
       P1 SPK_EN, P2 EXT5V_EN, P4 LCD_RST, P5 TP_RST, P6 CAM_RST high */
    static const uint8_t seq[][2] = {
        { 0x01, 0xFF },        /* chip reset */
        { 0x03, 0b01111111 },  /* IO direction (1=output) */
        { 0x07, 0b00000000 },  /* high-impedance off for used pins */
        { 0x0D, 0b01111111 },  /* pull select */
        { 0x0B, 0b01111111 },  /* pull enable */
        { 0x05, 0b01110110 },  /* OUT: LCD_RST/TP_RST released high */
    };
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]) && err == ESP_OK; i++)
        err = i2c_master_transmit(dev, seq[i], 2, 50);
    i2c_master_bus_rm_device(dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IO expander 0x43 init failed: %s", esp_err_to_name(err));
        i2c_del_master_bus(bus);
        return UI_PANEL_NONE;
    }

    /* give the touch controller time to boot out of reset, then probe.
       The panel variant is inferred from which touch chip answers, so a
       transient I2C miss (seen after a watchdog/USB reset that doesn't
       fully recycle the touch controller) would pick the WRONG panel and
       init the DSI with wrong timings -> black screen. Retry the probe a
       few times before giving up, and if nothing answers fall back to
       ST7123 (this repository's Tab5 lot) rather than ILI9881C — a wrong
       ILI9881C guess on an ST7123 panel is exactly what blanks it. */
    vTaskDelay(pdMS_TO_TICKS(100));
    ui_panel_variant_t variant = UI_PANEL_NONE;
    for (int attempt = 0; attempt < 8 && variant == UI_PANEL_NONE; attempt++) {
        if (i2c_master_probe(bus, UI_GT911_ADDR, 50) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 found -> ILI9881C panel variant");
            s_gt911_addr = UI_GT911_ADDR;
            variant = UI_PANEL_ILI9881C;
        } else if (i2c_master_probe(bus, UI_GT911_ADDR_BKP, 50) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 (backup addr) found -> ILI9881C panel variant");
            s_gt911_addr = UI_GT911_ADDR_BKP;
            variant = UI_PANEL_ILI9881C;
        } else if (i2c_master_probe(bus, UI_ST7123_TP_ADDR, 50) == ESP_OK) {
            variant = st712x_flavour(bus);
            ESP_LOGI(TAG, "touch @0x55 -> %s panel variant",
                     variant == UI_PANEL_ST7121 ? "ST7121" : "ST7123");
        } else {
            ESP_LOGW(TAG, "touch probe attempt %d: no answer, retrying", attempt);
            vTaskDelay(pdMS_TO_TICKS(60));
        }
    }
    if (variant == UI_PANEL_NONE) {
        ESP_LOGW(TAG, "no touch controller after retries; defaulting to ST7123");
        variant = UI_PANEL_ST7123;
    }
    i2c_del_master_bus(bus);
    return variant;
}

/*
 * Touch (Phase 3). The controller depends on the panel lot: GT911 on
 * ILI9881C units, ST7123 on ST712x units (INT on GPIO23, no reset pin
 * — TP_RST is the expander line released in panel_reset_and_detect).
 *
 * The bus is created on I2C port 1 and KEPT (unlike the probe bus):
 * port 0 stays free for JS i2c.setup(0, ...). Caveat: a JS task that
 * claims pins 31/32 itself would steal them from the touch controller.
 */
static lv_indev_t *s_touch_indev;
static i2c_master_bus_handle_t s_touch_bus; /* shared with camera SCCB */

void *ui_tab5_i2c_bus(void)
{
    return s_touch_bus;
}

static void touch_init(ui_panel_variant_t variant, lv_display_t *disp)
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = 1;
    bus_cfg.sda_io_num = (gpio_num_t)UI_I2C_SDA;
    bus_cfg.scl_io_num = (gpio_num_t)UI_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "touch i2c bus failed (no touch)");
        return;
    }
    s_touch_bus = bus;

    /* both controllers use 16-bit register addresses, no control
       phase; only the device address differs (the GT911 config macro
       is not C++-friendly, so the struct is filled by hand) */
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = (variant == UI_PANEL_ILI9881C) ? s_gt911_addr
                                                     : UI_ST7123_TP_ADDR;
    io_cfg.control_phase_bytes = 1;
    io_cfg.dc_bit_offset = 0;
    io_cfg.lcd_cmd_bits = 16;
    io_cfg.flags.disable_control_phase = 1;
    io_cfg.scl_speed_hz = 400000;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch io failed: %s (no touch)", esp_err_to_name(err));
        return;
    }

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = UI_LCD_H_RES;
    tp_cfg.y_max = UI_LCD_V_RES;
    tp_cfg.rst_gpio_num = GPIO_NUM_NC;
    tp_cfg.int_gpio_num = (gpio_num_t)23;
    esp_lcd_touch_handle_t tp = NULL;
    if (variant == UI_PANEL_ILI9881C)
        err = esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &tp);
    else
        err = esp_lcd_touch_new_i2c_st7123(io, &tp_cfg, &tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch ctrl failed: %s (no touch)", esp_err_to_name(err));
        return;
    }

    /* esp_lvgl_port polls the controller and drives LVGL gestures
       (console flick scroll); the JS path observes the indev below */
    lvgl_port_touch_cfg_t touch_cfg = {};
    touch_cfg.disp = disp;
    touch_cfg.handle = tp;
    s_touch_indev = lvgl_port_add_touch(&touch_cfg);
    if (!s_touch_indev)
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
    else
        ESP_LOGI(TAG, "touch up (%s)",
                 variant == UI_PANEL_ILI9881C ? "GT911" : "ST7123");
}

/* runs in the LVGL task (from the mooncake lv_timer): mirror the indev
   state LVGL already polled into JS touch events. Coordinates are
   canvas-relative (status bar clamps to y=0); kind 0=down 1=move 2=up. */
static void touch_observe(void)
{
    static bool was_pressed;
    static lv_point_t last;

    if (!s_touch_indev)
        return;
    bool pressed = lv_indev_get_state(s_touch_indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t p;
    lv_indev_get_point(s_touch_indev, &p);
    p.y -= UI_STATUSBAR_H;
    if (p.y < 0)
        p.y = 0;
    if (pressed && !was_pressed)
        mqjs_post_touch(p.x, p.y, 0);
    else if (pressed && (p.x != last.x || p.y != last.y))
        mqjs_post_touch(p.x, p.y, 1);
    else if (!pressed && was_pressed)
        mqjs_post_touch(last.x, last.y, 2);
    was_pressed = pressed;
    last = p;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_cfg.duty_resolution = LEDC_TIMER_12_BIT;
    timer_cfg.timer_num = LEDC_TIMER_0;
    timer_cfg.freq_hz = 5000;
    /* P4: every LEDC timer shares ONE global clock mux. AUTO would pick
       the XTAL here, and the camera XCLK (cam_tab5, 24MHz on TIMER_2)
       cannot be derived from 40MHz XTAL — pin both to PLL_F80M or the
       second ledc_timer_config fails with "timer clock conflict". */
    timer_cfg.clk_cfg = LEDC_USE_PLL_DIV_CLK;
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK)
        return err;

    ledc_channel_config_t ch_cfg = {};
    ch_cfg.gpio_num = UI_BACKLIGHT_GPIO;
    ch_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_cfg.channel = LEDC_CHANNEL_1;
    ch_cfg.timer_sel = LEDC_TIMER_0;
    ch_cfg.duty = 0; /* stay dark until the first frame is up */
    ch_cfg.hpoint = 0;
    return ledc_channel_config(&ch_cfg);
}

static void backlight_set(int percent)
{
    /* at 100% park the LEDC output at constant high - no PWM at all:
       even a 4095/4096 duty beats against the panel refresh and shows
       as a faint fluorescent-like shimmer (user-reported). NB: writing
       duty 4096 instead blanks the screen (masked to 0 in the 12-bit
       register); ledc_stop() with idle_level=1 is the supported way. */
    if (percent >= 100) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 1);
        return;
    }
    /* dimming below 100% resumes PWM */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1,
                  (4095u * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static esp_err_t display_init(ui_panel_variant_t variant,
                              esp_lcd_panel_io_handle_t *out_io,
                              esp_lcd_panel_handle_t *out_panel)
{
    bool ili9881c = (variant == UI_PANEL_ILI9881C);
    bool st7121 = (variant == UI_PANEL_ST7121);

    /* power the MIPI DPHY from the on-chip LDO */
    static esp_ldo_channel_handle_t phy_pwr = NULL;
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id = UI_DPHY_LDO_CHAN;
    ldo_cfg.voltage_mv = UI_DPHY_LDO_MV;
    esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DPHY LDO failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {};
    bus_cfg.bus_id = 0;
    bus_cfg.num_data_lanes = UI_DSI_LANES;
    bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_cfg.lane_bit_rate_mbps = ili9881c ? 730 : 965;
    err = esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DSI bus failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {};
    dbi_cfg.virtual_channel = 0;
    dbi_cfg.lcd_cmd_bits = 8;
    dbi_cfg.lcd_param_bits = 8;
    err = esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DBI io failed: %s", esp_err_to_name(err));
        return err;
    }

    /* timings per variant, straight from the UserDemo BSP */
    esp_lcd_dpi_panel_config_t dpi_cfg = {};
    dpi_cfg.virtual_channel = 0;
    dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_cfg.dpi_clock_freq_mhz = ili9881c ? 60 : 70;
    dpi_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_cfg.out_color_format = LCD_COLOR_FMT_RGB565;
    dpi_cfg.num_fbs = 1;
    dpi_cfg.video_timing.h_size = UI_LCD_H_RES;
    dpi_cfg.video_timing.v_size = UI_LCD_V_RES;
    if (ili9881c) {
        dpi_cfg.video_timing.hsync_pulse_width = 40;
        dpi_cfg.video_timing.hsync_back_porch = 140;
        dpi_cfg.video_timing.hsync_front_porch = 40;
        dpi_cfg.video_timing.vsync_pulse_width = 4;
        dpi_cfg.video_timing.vsync_back_porch = 20;
        dpi_cfg.video_timing.vsync_front_porch = 20;
    } else {
        dpi_cfg.video_timing.hsync_pulse_width = 2;
        dpi_cfg.video_timing.hsync_back_porch = 40;
        dpi_cfg.video_timing.hsync_front_porch = 40;
        dpi_cfg.video_timing.vsync_pulse_width = st7121 ? 20 : 2;
        dpi_cfg.video_timing.vsync_back_porch = st7121 ? 24 : 8;
        dpi_cfg.video_timing.vsync_front_porch = st7121 ? 200 : 220;
    }

    esp_lcd_panel_dev_config_t dev_cfg = {};
    dev_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    dev_cfg.reset_gpio_num = GPIO_NUM_NC; /* reset is on the IO expander */

    esp_lcd_panel_handle_t panel = NULL;
    if (ili9881c) {
        ili9881c_vendor_config_t vendor_cfg = {};
        vendor_cfg.init_cmds = tab5_lcd_ili9881c_specific_init_code_default;
        vendor_cfg.init_cmds_size = sizeof(tab5_lcd_ili9881c_specific_init_code_default) /
                                    sizeof(tab5_lcd_ili9881c_specific_init_code_default[0]);
        vendor_cfg.mipi_config.dsi_bus = dsi_bus;
        vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
        vendor_cfg.mipi_config.lane_num = UI_DSI_LANES;
        dev_cfg.bits_per_pixel = 16;
        dev_cfg.vendor_config = &vendor_cfg;
        err = esp_lcd_new_panel_ili9881c(io, &dev_cfg, &panel);
    } else if (st7121) {
        st7121_vendor_config_t vendor_cfg = {};
        vendor_cfg.init_cmds = NULL; /* driver-internal defaults */
        vendor_cfg.init_cmds_size = 0;
        vendor_cfg.mipi_config.dsi_bus = dsi_bus;
        vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
        dev_cfg.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
        dev_cfg.bits_per_pixel = 24;
        dev_cfg.vendor_config = &vendor_cfg;
        err = esp_lcd_new_panel_st7121(io, &dev_cfg, &panel);
    } else {
        st7123_vendor_config_t vendor_cfg = {};
        vendor_cfg.init_cmds = st7123_vendor_specific_init_default;
        vendor_cfg.init_cmds_size = sizeof(st7123_vendor_specific_init_default) /
                                    sizeof(st7123_vendor_specific_init_default[0]);
        vendor_cfg.mipi_config.dsi_bus = dsi_bus;
        vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
        vendor_cfg.mipi_config.lane_num = UI_DSI_LANES;
        dev_cfg.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
        dev_cfg.bits_per_pixel = 24;
        dev_cfg.vendor_config = &vendor_cfg;
        err = esp_lcd_new_panel_st7123(io, &dev_cfg, &panel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel create failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    /* IDF 6: dma2d blit is a runtime switch, not a config flag */
    esp_lcd_dpi_panel_enable_dma2d(panel);

    *out_io = io;
    *out_panel = panel;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* UI: StatusBar (UIAbility) + ConsoleApp (AppAbility)                 */
/* Both run from Mooncake::update() inside the LVGL task, so plain     */
/* LVGL calls are safe here (the port holds its lock around timers).   */
/* ------------------------------------------------------------------ */

#define UI_PAD 16

#define UI_COL_BG    0x0B0E11 /* console background */
#define UI_COL_BAR   0x1A222C /* status bar background */
#define UI_COL_TEXT  0xC9D1D9
#define UI_COL_DIM   0x8B98A5 /* task name (secondary info) */
#define UI_COL_OK    0x2ECC71 /* link-up indicator dots */
#define UI_COL_DOWN  0x55606B
#define UI_COL_EVENT 0xFFD479 /* last_event text */
#define UI_COL_FLASH 0x2E6BD6 /* highlight behind a fresh event */
#define UI_COL_DROP  0xE05A4E /* draw-cmd drop counter */

static lv_obj_t *make_label(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, ui_font(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    return lbl;
}

/* Top bar: WiFi/MQTT link dots, current task, last platform event.
   Reads the status snapshot every frame and only touches widgets when
   the generation counter moved.
   Lives on lv_layer_top() — the display-global layer above every
   screen — so it stays visible on W1 widget pages too (system chrome,
   user feedback 2026-06-11). It also does not slide with screen-load
   animations, which is exactly how a status bar should behave. Widget
   screens reserve UI_STATUSBAR_H of top padding (ui_widgets.cpp).

   P4b navigation chrome (launcher-multiapp-design §4):
   - the CHIP shows the previous foreground app by name — tap = open it
     (focus-or-relaunch via the launcher; dimmed when stopped). With no
     previous app it reads "アプリ一覧" and opens the launcher, so a
     visible path to the launcher always exists.
   - LONG-PRESS anywhere on the bar = launcher, with armed feedback: a
     progress strip fills while holding (driven per-frame here, NOT by
     LVGL's global long_press_time, so threshold and visual can't
     drift), turns green + "離すとランチャー" when armed; the action
     commits on RELEASE, sliding off the bar cancels (PRESS_LOST).
   - a short tap outside the chip does nothing: the bar is an
     information surface, accidental touches must stay consequence-free. */
#define UI_HOLD_MS 500 /* long-press threshold = strip fill time */

class StatusBar : public mooncake::UIAbility {
public:
    void onCreate() override
    {
        lv_obj_t *bar = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, 0, 0);
        lv_obj_set_size(bar, UI_LCD_H_RES, UI_STATUSBAR_H);
        lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COL_BAR), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(bar, press_cb, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(bar, release_cb, LV_EVENT_RELEASED, this);
        lv_obj_add_event_cb(bar, lost_cb, LV_EVENT_PRESS_LOST, this);

        lv_obj_t *row = lv_obj_create(bar);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, 0);
        lv_obj_set_size(row, UI_LCD_H_RES, 44);
        /* hit-testing falls through to the bar's press state machine */
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_hor(row, UI_PAD, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        /* link indicators: NF glyphs colored by state (was: bg-colored
           circles). wifi = nf-fa-wifi, broker = nf-fa-rss (pub/sub). */
        _net_dot = make_label(row, UI_COL_DOWN);
        lv_label_set_text(_net_dot, "");
        _net_lbl = make_label(row, UI_COL_TEXT);
        lv_label_set_text(_net_lbl, "未接続");

        _mqtt_dot = make_label(row, UI_COL_DOWN);
        lv_label_set_text(_mqtt_dot, "");
        _mqtt_lbl = make_label(row, UI_COL_TEXT);
        lv_label_set_text(_mqtt_lbl, "MQTT");

        _drop_lbl = make_label(row, UI_COL_DROP);
        lv_label_set_text(_drop_lbl, "");

        /* previous-app chip ("open X" button; self-labeling target) */
        _chip = lv_obj_create(row);
        lv_obj_remove_style_all(_chip);
        lv_obj_remove_flag(_chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(_chip, lv_color_hex(0x2A3540), 0);
        lv_obj_set_style_bg_opa(_chip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(_chip, 8, 0);
        lv_obj_set_style_pad_hor(_chip, 14, 0);
        lv_obj_set_style_pad_ver(_chip, 6, 0);
        lv_obj_set_size(_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        _chip_lbl = make_label(_chip, UI_COL_DIM);
        lv_label_set_text(_chip_lbl, " アプリ一覧");

        lv_obj_t *spacer = lv_obj_create(row);
        lv_obj_remove_style_all(spacer);
        lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_height(spacer, 1);
        lv_obj_set_flex_grow(spacer, 1);

        _task_lbl = make_label(row, UI_COL_DIM);
        lv_label_set_text(_task_lbl, "");
        /* pulse surface: highlights when the foreground app changes
           (feedback also for switches the user did not initiate) */
        lv_obj_set_style_pad_hor(_task_lbl, 6, 0);
        lv_obj_set_style_radius(_task_lbl, 4, 0);
        lv_obj_set_style_bg_color(_task_lbl, lv_color_hex(UI_COL_FLASH), 0);
        lv_obj_set_style_bg_opa(_task_lbl, LV_OPA_TRANSP, 0);

        _event_lbl = make_label(bar, UI_COL_EVENT);
        lv_obj_set_pos(_event_lbl, UI_PAD, 48);
        lv_obj_set_width(_event_lbl, UI_LCD_H_RES - 2 * UI_PAD);
        lv_obj_set_style_pad_hor(_event_lbl, 6, 0);
        lv_obj_set_style_radius(_event_lbl, 4, 0);
        lv_obj_set_style_bg_color(_event_lbl, lv_color_hex(UI_COL_FLASH), 0);
        lv_obj_set_style_bg_opa(_event_lbl, LV_OPA_TRANSP, 0);
        lv_label_set_text(_event_lbl, "");
        /* P4c: tapping a "[app] ..." notification opens its sender. The
           displayed text itself is parsed, so the tap can never chase a
           stale target; non-notify events just don't match. The label
           is its own click target — bar long-press is unaffected except
           when the press starts on this bottom strip. */
        lv_obj_add_flag(_event_lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(_event_lbl, notify_tap_cb, LV_EVENT_CLICKED,
                            this);

        /* long-press progress strip along the bar's bottom edge */
        _strip = lv_obj_create(bar);
        lv_obj_remove_style_all(_strip);
        lv_obj_remove_flag(_strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(_strip, 0, UI_STATUSBAR_H - 4);
        lv_obj_set_size(_strip, 1, 4);
        lv_obj_set_style_bg_color(_strip, lv_color_hex(0x4FC3F7), 0);
        lv_obj_set_style_bg_opa(_strip, LV_OPA_COVER, 0);
        lv_obj_add_flag(_strip, LV_OBJ_FLAG_HIDDEN);
    }

    void onForeground() override
    {
        ui_status_t st;
        ui_fgapps_t fa;
        xSemaphoreTake(s_status_mtx, portMAX_DELAY);
        uint32_t gen = s_status_gen;
        uint32_t fgen = s_fgapps_gen;
        st = s_status;
        fa = s_fgapps;
        xSemaphoreGive(s_status_mtx);

        if (gen != _seen_gen) {
            _seen_gen = gen;
            apply(st);
        }
        if (fgen != _fg_seen_gen || _chip_refresh) {
            _fg_seen_gen = fgen;
            _chip_refresh = false;
            apply_fgapps(fa);
        }

        /* long-press progress (self-driven: threshold == visual) */
        if (_pressed) {
            uint32_t held = lv_tick_elaps(_press_tick);
            uint32_t capped = held > UI_HOLD_MS ? UI_HOLD_MS : held;
            int w = (int)((uint32_t)UI_LCD_H_RES * capped / UI_HOLD_MS);
            lv_obj_set_width(_strip, w < 8 ? 8 : w);
            lv_obj_remove_flag(_strip, LV_OBJ_FLAG_HIDDEN);
            if (!_armed && held >= UI_HOLD_MS) {
                _armed = true;
                lv_obj_set_style_bg_color(_strip, lv_color_hex(UI_COL_OK), 0);
                lv_label_set_text(_chip_lbl, " 離すとランチャー");
                lv_obj_set_style_text_color(_chip_lbl,
                                            lv_color_hex(UI_COL_OK), 0);
            }
        }

        /* fade out the highlight behind a fresh event (spring to 0) */
        int opa = (int)(float)_flash;
        if (opa != _flash_opa) {
            _flash_opa = opa;
            lv_obj_set_style_bg_opa(_event_lbl, (lv_opa_t)opa, 0);
        }
        int fopa = (int)(float)_fgflash;
        if (fopa != _fgflash_opa) {
            _fgflash_opa = fopa;
            lv_obj_set_style_bg_opa(_task_lbl, (lv_opa_t)fopa, 0);
        }

        /* draw-command drop counter (visible backpressure, Phase 2) */
        uint32_t drops = s_cmd_drops;
        if (drops != _seen_drops) {
            _seen_drops = drops;
            lv_label_set_text_fmt(_drop_lbl, "drop %u", (unsigned)drops);
        }
    }

private:
    static void press_cb(lv_event_t *e)
    {
        auto *self = (StatusBar *)lv_event_get_user_data(e);
        lv_indev_t *indev = lv_indev_active();
        if (indev)
            lv_indev_get_point(indev, &self->_press_pt);
        self->_press_tick = lv_tick_get();
        self->_pressed = true;
        self->_armed = false;
    }

    static void release_cb(lv_event_t *e)
    {
        auto *self = (StatusBar *)lv_event_get_user_data(e);
        if (!self->_pressed)
            return;
        bool armed = self->_armed;
        self->cancel_press();
        if (armed) {
            mqjs_focus(0); /* launcher is resident: plain focus works */
            return;
        }
        /* short tap: only the chip acts (slop-inflated hit test) */
        lv_area_t a;
        lv_obj_get_coords(self->_chip, &a);
        lv_point_t p = self->_press_pt;
        if (p.x >= a.x1 - 8 && p.x <= a.x2 + 8 && p.y >= a.y1 - 8 &&
            p.y <= a.y2 + 8) {
            if (self->_chip_target[0])
                mqjs_request_open(self->_chip_target);
            else
                mqjs_focus(0);
        }
    }

    static void lost_cb(lv_event_t *e)
    {
        auto *self = (StatusBar *)lv_event_get_user_data(e);
        self->cancel_press();
    }

    static void notify_tap_cb(lv_event_t *e)
    {
        auto *self = (StatusBar *)lv_event_get_user_data(e);
        const char *t = lv_label_get_text(self->_event_lbl);
        if (!t)
            return;
        if (!strncmp(t, " ", 4))
            t += 4; /* the bell prefix apply() adds to notify lines */
        if (t[0] != '[')
            return; /* not a sys.notify line */
        const char *end = strchr(t, ']');
        if (!end || end == t + 1 || end - t > 32)
            return;
        char name[32];
        size_t n = (size_t)(end - t - 1);
        memcpy(name, t + 1, n);
        name[n] = '\0';
        mqjs_request_open(name);
    }

    void cancel_press()
    {
        _pressed = false;
        _armed = false;
        lv_obj_add_flag(_strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(_strip, lv_color_hex(0x4FC3F7), 0);
        _chip_refresh = true; /* restore the chip label next frame */
    }

    void apply(const ui_status_t &st)
    {
        lv_obj_set_style_text_color(
            _net_dot, lv_color_hex(st.wifi_up ? UI_COL_OK : UI_COL_DOWN), 0);
        lv_label_set_text(_net_lbl, st.wifi_up ? (st.ip[0] ? st.ip : "接続中")
                                               : "未接続");
        lv_obj_set_style_text_color(
            _mqtt_dot, lv_color_hex(st.mqtt_up ? UI_COL_OK : UI_COL_DOWN), 0);
        strlcpy(_st_task, st.task_name, sizeof _st_task);
        strlcpy(_st_origin, st.task_origin, sizeof _st_origin);
        update_task_label();
        if (strcmp(_last_event, st.last_event) != 0) {
            strlcpy(_last_event, st.last_event, sizeof _last_event);
            /* sys.notify lines ("[app] ...") get a bell and are tappable
               (notify_tap_cb skips the bell before parsing the sender) */
            if (st.last_event[0] == '[')
                lv_label_set_text_fmt(_event_lbl, " %s", st.last_event);
            else
                lv_label_set_text(_event_lbl, st.last_event);
            _flash.teleport(LV_OPA_60);
            _flash.move(0);
        }
    }

    void apply_fgapps(const ui_fgapps_t &fa)
    {
        strlcpy(_chip_target, fa.prev, sizeof _chip_target);
        if (fa.prev[0]) {
            /* nf-fa-reply: "go back to <app>" */
            lv_label_set_text_fmt(_chip_lbl, " %s", fa.prev);
            /* dimmed = stopped: tapping still works (relaunch), but the
               app starts fresh rather than "where you left it" */
            lv_obj_set_style_text_color(
                _chip_lbl,
                lv_color_hex(fa.prev_running ? UI_COL_TEXT : UI_COL_DIM), 0);
        } else {
            /* nf-fa-th grid: "app list" */
            lv_label_set_text(_chip_lbl, " アプリ一覧");
            lv_obj_set_style_text_color(_chip_lbl, lv_color_hex(UI_COL_DIM),
                                        0);
        }
        if (fa.cur[0] && strcmp(fa.cur, _fg_cur) != 0) {
            strlcpy(_fg_cur, fa.cur, sizeof _fg_cur);
            update_task_label();
            _fgflash.teleport(LV_OPA_60); /* who owns the screen now */
            _fgflash.move(0);
        }
    }

    void update_task_label()
    {
        /* foreground app name; the dev task keeps its origin suffix */
        if (_fg_cur[0] && strcmp(_fg_cur, _st_task) == 0)
            lv_label_set_text_fmt(_task_lbl, "%s (%s)", _fg_cur, _st_origin);
        else if (_fg_cur[0])
            lv_label_set_text(_task_lbl, _fg_cur);
        else if (_st_task[0])
            lv_label_set_text_fmt(_task_lbl, "%s (%s)", _st_task, _st_origin);
    }

    lv_obj_t *_net_dot = nullptr, *_net_lbl = nullptr;
    lv_obj_t *_mqtt_dot = nullptr, *_mqtt_lbl = nullptr;
    lv_obj_t *_task_lbl = nullptr, *_event_lbl = nullptr;
    lv_obj_t *_drop_lbl = nullptr;
    lv_obj_t *_chip = nullptr, *_chip_lbl = nullptr, *_strip = nullptr;
    uint32_t _seen_gen = 0;
    uint32_t _fg_seen_gen = 0;
    uint32_t _seen_drops = 0;
    int _flash_opa = -1, _fgflash_opa = -1;
    bool _pressed = false, _armed = false, _chip_refresh = false;
    uint32_t _press_tick = 0;
    lv_point_t _press_pt = { 0, 0 };
    char _chip_target[32] = "";
    char _fg_cur[32] = "";
    char _st_task[sizeof(ui_status_t::task_name)] = "";
    char _st_origin[sizeof(ui_status_t::task_origin)] = "";
    char _last_event[sizeof(ui_status_t::last_event)] = "";
    smooth_ui_toolkit::AnimateValue _flash{0};
    smooth_ui_toolkit::AnimateValue _fgflash{0};
};

/* Scrolling console below the bar: one label per ring line, capped at
   UI_LOG_LINES children, flick-scrollable. Follows the tail unless the
   user scrolled up to read history. */
class ConsoleApp : public mooncake::AppAbility {
public:
    ConsoleApp() { setAppInfo().name = "console"; }

    void onCreate() override
    {
        _panel = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(_panel);
        lv_obj_set_pos(_panel, 0, UI_STATUSBAR_H);
        lv_obj_set_size(_panel, UI_LCD_H_RES, UI_LCD_V_RES - UI_STATUSBAR_H);
        lv_obj_set_style_bg_color(_panel, lv_color_hex(UI_COL_BG), 0);
        lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
        /* slim side padding: console lines should use the full width
           (user feedback: 16px pads wasted ~3 half-width chars) */
        lv_obj_set_style_pad_ver(_panel, 8, 0);
        lv_obj_set_style_pad_hor(_panel, 4, 0);
        lv_obj_set_style_pad_row(_panel, 4, 0);
        lv_obj_set_flex_flow(_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(_panel, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(_panel, LV_SCROLLBAR_MODE_AUTO);
    }

    void onRunning() override
    {
        /* copy under the mutex, draw outside it; the batch bound keeps
           the producer's worst-case wait tiny. Static: LVGL task only. */
        static ui_log_line_t batch[16];
        size_t got = 0;
        xSemaphoreTake(s_log_mtx, portMAX_DELAY);
        if (s_log_head - _tail > UI_LOG_LINES)
            _tail = s_log_head - UI_LOG_LINES; /* ring lapped the reader */
        while (_tail != s_log_head && got < sizeof batch / sizeof batch[0]) {
            batch[got++] = s_log[_tail % UI_LOG_LINES];
            _tail++;
        }
        xSemaphoreGive(s_log_mtx);
        if (!got)
            return;

        /* tail-follow re-engages within ~4 lines of the bottom: flick
           momentum usually stops a few dozen px short of the edge, so
           a tight threshold (24px) never recovered (user-reported) */
        bool follow = lv_obj_get_scroll_bottom(_panel) <= 100;
        for (size_t i = 0; i < got; i++) {
            lv_obj_t *lbl = make_label(_panel, UI_COL_TEXT);
            lv_obj_set_width(lbl, LV_PCT(100));
            /* the producer translated ANSI SGR into recolor markup */
            lv_label_set_recolor(lbl, true);
            lv_label_set_text(lbl, batch[i].text);
        }
        while (lv_obj_get_child_count(_panel) > UI_LOG_LINES)
            lv_obj_delete(lv_obj_get_child(_panel, 0));
        if (follow) {
            lv_obj_update_layout(_panel);
            lv_obj_scroll_to_y(
                _panel,
                lv_obj_get_scroll_y(_panel) + lv_obj_get_scroll_bottom(_panel),
                LV_ANIM_ON);
        }
    }

private:
    lv_obj_t *_panel = nullptr;
    uint32_t _tail = 0;
};

/* ------------------------------------------------------------------ */
/* Phase 4: on-screen keyboard (JS terminal groundwork).               */
/* lv_keyboard overlay on the bottom of the screen, hidden until JS    */
/* calls ui.keyboard(1). Keys are forwarded to JS as small strings     */
/* through mqjs_post_key: printable keys verbatim, Enter/OK = "\n",    */
/* backspace = "\b", arrows = ANSI cursor sequences (terminal food).   */
/* All of this runs in the LVGL task (queue drain / event callback).   */
/* ------------------------------------------------------------------ */

#define UI_KB_H 400 /* 4 rows x 100px: comfortable on the 5" panel */
#define UI_CB_H 80  /* T3a control bar row above the keyboard */

static lv_obj_t *s_kb;
static lv_obj_t *s_cbar;    /* T3a terminal control bar (mode 2) */
static bool s_cbar_fn;      /* current map: false = main, true = F1-F12 */
static lv_obj_t *s_root_scr; /* console screen: fixed parent for s_kb (a
                                widget screen could be active when JS calls
                                ui.keyboard(1); parenting there would leave
                                s_kb dangling when that screen is freed) */

/* T3a control bar (ssh-terminal-design §7): generic key-token source.
   C only maps button -> "\0name" token (NUL-sentinel, design keytoken
   convention); the terminal's meaning (one-shot Ctrl/Alt, xterm F-key
   sequences, paste) lives in JS where it can be pushed OTA. Fn flips
   the bar between the main map and F1-F12 (handled here: it changes
   the bar itself, no token). Octal "\0" not hex: "\x00esc" would eat
   'e' as a hex digit. */
static const char *CB_MAP_MAIN[] = {
    "Esc", "Tab", "Ctrl", "Alt", "Fn",
    LV_SYMBOL_LEFT, LV_SYMBOL_DOWN, LV_SYMBOL_UP, LV_SYMBOL_RIGHT,
    LV_SYMBOL_COPY, LV_SYMBOL_PASTE, "",
};
static const char *CB_TOK_MAIN[] = {
    "\0esc", "\0tab", "\0ctrl", "\0alt", NULL /* Fn */,
    "\0left", "\0down", "\0up", "\0right",
    "\0copy", "\0paste",
};
static const char *CB_MAP_FN[] = {
    "Fn", "F1", "F2", "F3", "F4", "F5", "F6",
    "F7", "F8", "F9", "F10", "F11", "F12", "",
};
static const char *CB_TOK_FN[] = {
    NULL /* Fn */, "\0f1", "\0f2", "\0f3", "\0f4", "\0f5", "\0f6",
    "\0f7", "\0f8", "\0f9", "\0f10", "\0f11", "\0f12",
};

/* One-shot modifier latch, mirrored on the buttons themselves (user
   feedback: the tab-bar badge alone was too subtle). The SEMANTIC
   one-shot state lives in the terminal JS; this visual copy stays in
   sync by construction because both are driven by the same key
   stream — armed on the Ctrl/Alt tap, cleared by the next key from
   either the bar or the keyboard (exactly when JS consumes it). */
#define CB_ID_CTRL 2 /* index in CB_MAP_MAIN */
#define CB_ID_ALT  3
static bool s_mod_ctrl, s_mod_alt;

/* Push s_mod_ctrl/s_mod_alt onto the Ctrl/Alt buttons as a manual
   CHECKED flag. Deliberately NOT CHECKABLE: LVGL toggles a checkable
   button's CHECKED at RELEASED but fires VALUE_CHANGED at press time,
   so reading the flag from the event handler races the toggle (seen
   on device: the latch stuck yellow). Our bools are the only state;
   the flag is write-only from here. Re-call after every set_map —
   it wipes per-button ctrl flags (Fn flips back and forth). */
static void cbar_apply_mods(void)
{
    if (!s_cbar || s_cbar_fn)
        return;
    if (s_mod_ctrl)
        lv_buttonmatrix_set_button_ctrl(s_cbar, CB_ID_CTRL,
                                        LV_BUTTONMATRIX_CTRL_CHECKED);
    else
        lv_buttonmatrix_clear_button_ctrl(s_cbar, CB_ID_CTRL,
                                          LV_BUTTONMATRIX_CTRL_CHECKED);
    if (s_mod_alt)
        lv_buttonmatrix_set_button_ctrl(s_cbar, CB_ID_ALT,
                                        LV_BUTTONMATRIX_CTRL_CHECKED);
    else
        lv_buttonmatrix_clear_button_ctrl(s_cbar, CB_ID_ALT,
                                          LV_BUTTONMATRIX_CTRL_CHECKED);
}

/* the next key consumed the one-shot: drop the latch (no-op when idle) */
static void cbar_clear_mods(void)
{
    if (!s_mod_ctrl && !s_mod_alt)
        return;
    s_mod_ctrl = s_mod_alt = false;
    cbar_apply_mods();
}

static void cbar_show(bool show)
{
    if (!show) {
        if (s_cbar)
            lv_obj_add_flag(s_cbar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!s_cbar) {
        s_cbar = lv_buttonmatrix_create(s_root_scr ? s_root_scr
                                                   : lv_screen_active());
        lv_obj_set_size(s_cbar, UI_LCD_H_RES, UI_CB_H);
        lv_obj_align(s_cbar, LV_ALIGN_BOTTOM_MID, 0, -UI_KB_H);
        lv_obj_set_style_pad_all(s_cbar, 4, 0);
        lv_obj_set_style_pad_gap(s_cbar, 4, 0);
        lv_obj_set_style_text_font(s_cbar, ui_font(), 0);
        /* dark system palette (the default light theme reads as a
           bright flash whenever the destroy-on-switch model re-shows
           the overlay — user-reported on app switches) */
        lv_obj_set_style_bg_color(s_cbar, lv_color_hex(UI_COL_BG), 0);
        lv_obj_set_style_border_width(s_cbar, 0, 0);
        lv_obj_set_style_bg_color(s_cbar, lv_color_hex(UI_COL_BAR),
                                  LV_PART_ITEMS);
        lv_obj_set_style_text_color(s_cbar, lv_color_hex(UI_COL_TEXT),
                                    LV_PART_ITEMS);
        lv_obj_set_style_bg_color(s_cbar, lv_color_hex(UI_COL_FLASH),
                                  (uint32_t)LV_PART_ITEMS |
                                      (uint32_t)LV_STATE_PRESSED);
        /* armed one-shot Ctrl/Alt latch on the button itself (same
           amber as the terminal's tab-bar badge) */
        lv_obj_set_style_bg_color(s_cbar, lv_color_hex(0xFFD479),
                                  (uint32_t)LV_PART_ITEMS |
                                      (uint32_t)LV_STATE_CHECKED);
        lv_obj_set_style_text_color(s_cbar, lv_color_black(),
                                    (uint32_t)LV_PART_ITEMS |
                                        (uint32_t)LV_STATE_CHECKED);
        lv_buttonmatrix_set_map(s_cbar, CB_MAP_MAIN);
        s_cbar_fn = false;
        cbar_apply_mods();
        lv_obj_add_event_cb(
            s_cbar,
            [](lv_event_t *e) {
                lv_obj_t *bm = (lv_obj_t *)lv_event_get_current_target(e);
                uint32_t id = lv_buttonmatrix_get_selected_button(bm);
                if (id == LV_BUTTONMATRIX_BUTTON_NONE)
                    return;
                const char *tok;
                size_t ntok;
                if (s_cbar_fn) {
                    if (id >= sizeof CB_TOK_FN / sizeof CB_TOK_FN[0])
                        return;
                    tok = CB_TOK_FN[id];
                } else {
                    if (id >= sizeof CB_TOK_MAIN / sizeof CB_TOK_MAIN[0])
                        return;
                    tok = CB_TOK_MAIN[id];
                }
                if (!tok) { /* Fn: flip the map locally */
                    s_cbar_fn = !s_cbar_fn;
                    lv_buttonmatrix_set_map(bm, s_cbar_fn ? CB_MAP_FN
                                                          : CB_MAP_MAIN);
                    cbar_apply_mods(); /* set_map wiped the latch */
                    return;
                }
                ntok = 1 + strlen(tok + 1); /* NUL sentinel + name */
                if (!s_cbar_fn && id == CB_ID_CTRL) {
                    s_mod_ctrl = !s_mod_ctrl; /* same one-shot toggle as JS */
                    cbar_apply_mods();
                } else if (!s_cbar_fn && id == CB_ID_ALT) {
                    s_mod_alt = !s_mod_alt;
                    cbar_apply_mods();
                }
                mqjs_post_key(tok, ntok);
                /* any key but Ctrl/Alt themselves consumes the one-shot
                   (in the FN map ids 2/3 are F2/F3, not modifiers) */
                if (s_cbar_fn || (id != CB_ID_CTRL && id != CB_ID_ALT))
                    cbar_clear_mods();
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
    }
    lv_obj_remove_flag(s_cbar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_cbar);
}

/* px the keyboard overlay reserves at the canvas bottom in `mode` —
   ui.keyboard(mode)'s synchronous return, so the JS terminal derives
   its grid from screen height minus this (design §4d, no hardcode) */
extern "C" int ui_tab5_kb_reserved(int mode)
{
    if (!s_canvas_w || mode <= 0)
        return 0;
    return mode == 1 ? UI_KB_H : UI_KB_H + UI_CB_H;
}

/* ------------------------------------------------------------------ */
/* T3c: stats panel hidden behind the keyboard (ssh-terminal §7 T3c). */
/* The keyboard's close key COLLAPSES instead of hiding: the reserved  */
/* area shows a C-owned dashboard (fg app, uptime, heap, link state,   */
/* clipboard preview, brightness dial) — a resting / stability-check   */
/* surface for long keyboard sessions. The app's grid is untouched     */
/* (same reserved height), output keeps flowing above. The ⌨ button   */
/* (or the app re-requesting ui.keyboard) lifts the keyboard back.     */
/* App ui.keyboard(0) / UI_CMD_RESET = OFF: panel hidden too.          */
/* ------------------------------------------------------------------ */

extern "C" bool mqjs_clipboard_peek(char *type, size_t tcap, char *data,
                                    size_t dcap);
static void kb_show(int mode); /* fwd: the ⌨ button restores */

static int s_kb_mode = 2;   /* last shown mode: what ⌨ restores to */
static lv_obj_t *s_spanel;
static lv_obj_t *s_sp_stats, *s_sp_clip, *s_sp_pct;
static lv_obj_t *s_sp_arc;
static lv_timer_t *s_sp_timer;
static int s_brightness = 100; /* boot value set by ui_tab5_start */

static void spanel_apply_brightness(int v)
{
    if (v < 5)
        v = 5;   /* never to 0 from the dial: a black panel reads as
                    a crash, and recovery would need blind taps */
    if (v > 100)
        v = 100;
    v = ((v + 2) / 5) * 5; /* 5% snap */
    s_brightness = v;
    backlight_set(v);
    if (s_sp_arc)
        lv_arc_set_value(s_sp_arc, v);
    if (s_sp_pct) {
        char b[8];
        snprintf(b, sizeof b, "%d%%", v);
        lv_label_set_text(s_sp_pct, b);
    }
}

static void spanel_update(lv_timer_t *t)
{
    (void)t;
    if (!s_spanel || lv_obj_has_flag(s_spanel, LV_OBJ_FLAG_HIDDEN))
        return;

    ui_status_t st;
    ui_fgapps_t fa;
    xSemaphoreTake(s_status_mtx, portMAX_DELAY);
    st = s_status;
    fa = s_fgapps;
    xSemaphoreGive(s_status_mtx);

    int64_t up = esp_timer_get_time() / 1000000;
    unsigned ih = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) /
                             1024);
    unsigned ph = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) /
                             1024 / 1024);
    unsigned lh = (unsigned)(ui_tab5_lv_mem_free() / 1024);

    char buf[256];
    snprintf(buf, sizeof buf,
             "アプリ: %s\n"
             "稼働: %02d:%02d:%02d\n"
             "heap: 内蔵 %uKB / PSRAM %uMB / LVGL %uKB\n"
             "WiFi: %s %s   MQTT: %s",
             fa.cur[0] ? fa.cur : "-",
             (int)(up / 3600), (int)(up / 60 % 60), (int)(up % 60),
             ih, ph, lh,
             st.wifi_up ? "接続" : "切断", st.wifi_up ? st.ip : "",
             st.mqtt_up ? "接続" : "切断");
    lv_label_set_text(s_sp_stats, buf);

    char ctype[32], cdata[81];
    if (mqjs_clipboard_peek(ctype, sizeof ctype, cdata, sizeof cdata)) {
        /* control chars would garble the one-line preview */
        for (char *p = cdata; *p; p++)
            if ((unsigned char)*p < 0x20)
                *p = ' ';
        char cb[160];
        snprintf(cb, sizeof cb, "クリップボード [%s]\n%s", ctype, cdata);
        lv_label_set_text(s_sp_clip, cb);
    } else {
        lv_label_set_text(s_sp_clip, "クリップボード: (空)");
    }
}

static void spanel_build(void)
{
    s_spanel = lv_obj_create(s_root_scr ? s_root_scr : lv_screen_active());
    lv_obj_set_style_bg_color(s_spanel, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_spanel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_spanel, 1, 0);
    lv_obj_set_style_border_color(s_spanel, lv_color_hex(UI_COL_BAR), 0);
    lv_obj_set_style_border_side(s_spanel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(s_spanel, 0, 0);
    lv_obj_set_style_pad_all(s_spanel, 16, 0);
    lv_obj_remove_flag(s_spanel, LV_OBJ_FLAG_SCROLLABLE);

    /* left column: stats + clipboard preview (1s refresh) */
    s_sp_stats = make_label(s_spanel, UI_COL_TEXT);
    lv_obj_set_pos(s_sp_stats, 0, 0);
    lv_obj_set_width(s_sp_stats, 420);
    lv_label_set_text(s_sp_stats, "...");

    s_sp_clip = make_label(s_spanel, UI_COL_DIM);
    lv_obj_set_pos(s_sp_clip, 0, 200);
    lv_obj_set_width(s_sp_clip, 420);
    lv_label_set_long_mode(s_sp_clip, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_sp_clip, "");

    /* right: brightness arc (relative drag + 5% snap + ± nudge) */
    s_sp_arc = lv_arc_create(s_spanel);
    lv_obj_set_size(s_sp_arc, 200, 200);
    lv_obj_align(s_sp_arc, LV_ALIGN_TOP_RIGHT, -20, 0);
    lv_arc_set_range(s_sp_arc, 5, 100);
    lv_arc_set_value(s_sp_arc, s_brightness);
    lv_obj_set_style_arc_color(s_sp_arc, lv_color_hex(UI_COL_BAR),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_sp_arc, lv_color_hex(UI_COL_EVENT),
                               LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sp_arc, lv_color_hex(UI_COL_TEXT),
                              LV_PART_KNOB);
    lv_obj_add_event_cb(
        s_sp_arc,
        [](lv_event_t *e) {
            lv_obj_t *a = (lv_obj_t *)lv_event_get_current_target(e);
            spanel_apply_brightness(lv_arc_get_value(a));
        },
        LV_EVENT_VALUE_CHANGED, nullptr);

    s_sp_pct = make_label(s_sp_arc, UI_COL_TEXT);
    lv_obj_center(s_sp_pct);
    lv_label_set_text(s_sp_pct, "100%");

    /* ± nudge + keyboard-restore buttons under the arc */
    struct Btn { const char *txt; int dv; };
    static const Btn btns[] = { { "-", -5 }, { "+", +5 },
                                { LV_SYMBOL_KEYBOARD, 0 } };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(s_spanel);
        lv_obj_set_size(b, 88, 64);
        lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -20 - (2 - i) * 100, -8);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_BAR), 0);
        lv_obj_t *l = make_label(b, UI_COL_TEXT);
        lv_label_set_text(l, btns[i].txt);
        lv_obj_center(l);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t *e) {
                intptr_t dv = (intptr_t)lv_event_get_user_data(e);
                if (dv == 0)
                    kb_show(s_kb_mode); /* ⌨: lift the keyboard back */
                else
                    spanel_apply_brightness(s_brightness + (int)dv);
            },
            LV_EVENT_CLICKED, (void *)(intptr_t)btns[i].dv);
    }

    s_sp_timer = lv_timer_create(spanel_update, 1000, nullptr);
}

static void spanel_show(bool show)
{
    if (!show) {
        if (s_spanel) {
            lv_obj_add_flag(s_spanel, LV_OBJ_FLAG_HIDDEN);
            if (s_sp_timer)
                lv_timer_pause(s_sp_timer);
        }
        return;
    }
    if (!s_spanel)
        spanel_build();
    int h = ui_tab5_kb_reserved(s_kb_mode ? s_kb_mode : 2);
    lv_obj_set_size(s_spanel, UI_LCD_H_RES, h);
    lv_obj_align(s_spanel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(s_spanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_spanel);
    if (s_sp_timer) {
        lv_timer_resume(s_sp_timer);
        lv_timer_ready(s_sp_timer); /* refresh now, not in 1s */
    }
}

/* keyboard close key: collapse to the panel instead of plain hide */
static void kb_collapse(void)
{
    if (s_kb)
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    cbar_show(false);
    spanel_show(true);
}

/* mode: 0 = hide (OFF: stats panel gone too), 1 = keyboard,
   2 = keyboard + terminal control bar. Showing always lifts the
   keyboard over a collapsed panel (T3c SHOWN state). */
static void kb_show(int mode)
{
    cbar_show(mode >= 2);
    spanel_show(false);
    if (mode <= 0) {
        if (s_kb)
            lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    s_kb_mode = mode; /* what the panel's ⌨ button restores */
    if (s_kb) {
        lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_kb);
        return;
    }
    s_kb = lv_keyboard_create(s_root_scr ? s_root_scr : lv_screen_active());
    /* the default VALUE_CHANGED handler is built for a textarea and
       switches maps before our callback could read the key, so replace
       it wholesale: mode switching is redone below via set_mode */
    lv_obj_remove_event_cb(s_kb, lv_keyboard_def_event_cb);
    lv_obj_set_size(s_kb, UI_LCD_H_RES, UI_KB_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_kb, ui_font(), 0);
    /* dark system palette — the default light theme made every
       keyboard (re)appearance a bright blue-white flash */
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(UI_COL_BAR),
                              LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(UI_COL_TEXT),
                                LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(UI_COL_FLASH),
                              (uint32_t)LV_PART_ITEMS |
                                  (uint32_t)LV_STATE_PRESSED);
    lv_obj_add_event_cb(
        s_kb,
        [](lv_event_t *e) {
            lv_obj_t *kb = (lv_obj_t *)lv_event_get_current_target(e);
            uint32_t id = lv_buttonmatrix_get_selected_button(kb);
            if (id == LV_BUTTONMATRIX_BUTTON_NONE)
                return;
            const char *txt = lv_buttonmatrix_get_button_text(kb, id);
            if (!txt)
                return;
            /* map/mode switches post nothing to JS: return before the
               one-shot latch clear below */
            if (!strcmp(txt, "abc")) {
                lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
                return;
            }
            if (!strcmp(txt, "ABC")) {
                lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);
                return;
            }
            if (!strcmp(txt, "1#")) {
                lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_SPECIAL);
                return;
            }
            if (!strcmp(txt, LV_SYMBOL_CLOSE) ||
                !strcmp(txt, LV_SYMBOL_KEYBOARD)) {
                kb_collapse(); /* T3c: reveal the stats panel behind */
                return;
            }
            if (!strcmp(txt, LV_SYMBOL_BACKSPACE))
                mqjs_post_key("\b", 1);
            else if (!strcmp(txt, LV_SYMBOL_NEW_LINE) ||
                     !strcmp(txt, LV_SYMBOL_OK))
                mqjs_post_key("\n", 1);
            else if (!strcmp(txt, LV_SYMBOL_LEFT))
                mqjs_post_key("\x1b[D", 3);
            else if (!strcmp(txt, LV_SYMBOL_RIGHT))
                mqjs_post_key("\x1b[C", 3);
            else
                mqjs_post_key(txt, strlen(txt));
            /* every key the keyboard posts consumes a pending one-shot
               modifier in the terminal JS — mirror it on the bar latch
               (mode-switch taps returned above and don't get here) */
            cbar_clear_mods();
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
}

/* Phase 2: JS-drawable canvas over the console area. Hidden until the
   running script issues its first ui.* command; hides again (and
   clears) when a different task takes over, so console-only tasks get
   the console back. Primitives are drawn straight into the RGB565
   buffer (PSRAM); TEXT goes through an LVGL layer for font rendering.
   One invalidate per drained batch. */
class CanvasApp : public mooncake::AppAbility {
public:
    CanvasApp() { setAppInfo().name = "canvas"; }

    void onCreate() override
    {
        /* 64B (L1/L2 cache line) aligned start + size: lets the PPA do
           cache-coherent fills straight into the canvas */
        size_t bytes = ((size_t)s_canvas_w * s_canvas_h * 2 + 63) & ~(size_t)63;
        _buf = (uint16_t *)heap_caps_aligned_alloc(64, bytes,
                                                   MALLOC_CAP_SPIRAM);
        if (!_buf) {
            ESP_LOGE(TAG, "canvas buffer alloc failed (%u bytes)",
                     (unsigned)bytes);
            return;
        }
        if (!s_ppa_fill) {
            ppa_client_config_t cfg = {};
            cfg.oper_type = PPA_OPERATION_FILL;
            if (ppa_register_client(&cfg, &s_ppa_fill) != ESP_OK) {
                s_ppa_fill = nullptr;
                ESP_LOGW(TAG, "PPA unavailable, large fills stay on CPU");
            }
        }
        if (!s_ppa_blend) {
            ppa_client_config_t cfg = {};
            cfg.oper_type = PPA_OPERATION_BLEND;
            if (ppa_register_client(&cfg, &s_ppa_blend) != ESP_OK) {
                s_ppa_blend = nullptr; /* cells runs stay on the CPU */
                ESP_LOGW(TAG, "PPA blend unavailable, cells stay on CPU");
            }
        }
        fill_all(lv_color_to_u16(lv_color_hex(UI_COL_BG)));
        _canvas = lv_canvas_create(lv_screen_active());
        lv_canvas_set_buffer(_canvas, _buf, s_canvas_w, s_canvas_h,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(_canvas, 0, UI_STATUSBAR_H);
        lv_obj_add_flag(_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    void onRunning() override
    {
        if (!_canvas)
            return;
        track_task_switch();

        ui_cmd_t cmd;
        bool drew = false;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd.op == UI_CMD_KEYBOARD) {
                /* not a drawing op: must not unhide the canvas */
                kb_show(cmd.x);
                continue;
            }
            if (cmd.op == UI_CMD_RESET) {
                /* foreground-app switch (P4a): same hygiene as a task
                   switch — stale pixels gone, console visible again,
                   keyboard down. The next app redraws from its model. */
                fill_all(lv_color_to_u16(lv_color_hex(UI_COL_BG)));
                lv_obj_add_flag(_canvas, LV_OBJ_FLAG_HIDDEN);
                kb_show(0);
                cbar_clear_mods(); /* one-shot state died with the app */
                continue;
            }
            apply(cmd);
            free(cmd.text);
            drew = true;
        }
        if (drew) {
            if (lv_obj_has_flag(_canvas, LV_OBJ_FLAG_HIDDEN))
                lv_obj_remove_flag(_canvas, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(_canvas);
        }
    }

private:
    /* a new task starting means the canvas content is stale: clear it
       and drop back to the console until the task draws something */
    void track_task_switch()
    {
        char tn[sizeof(ui_status_t::task_name)];
        char to[sizeof(ui_status_t::task_origin)];
        xSemaphoreTake(s_status_mtx, portMAX_DELAY);
        uint32_t gen = s_status_gen;
        memcpy(tn, s_status.task_name, sizeof tn);
        memcpy(to, s_status.task_origin, sizeof to);
        xSemaphoreGive(s_status_mtx);
        if (gen == _seen_gen)
            return;
        _seen_gen = gen;
        if (strcmp(tn, _task) != 0 || strcmp(to, _origin) != 0) {
            memcpy(_task, tn, sizeof _task);
            memcpy(_origin, to, sizeof _origin);
            fill_all(lv_color_to_u16(lv_color_hex(UI_COL_BG)));
            lv_obj_add_flag(_canvas, LV_OBJ_FLAG_HIDDEN);
            kb_show(0); /* a new task should not inherit the kb */
            cbar_clear_mods(); /* ... nor an armed one-shot latch */
        }
    }

    void fill_all(uint16_t px)
    {
        fill_rect(0, 0, s_canvas_w, s_canvas_h, px);
    }

    void fill_rect(int x, int y, int w, int h, uint16_t px)
    {
        int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        int x1 = x + w, y1 = y + h;
        if (x1 > s_canvas_w)
            x1 = s_canvas_w;
        if (y1 > s_canvas_h)
            y1 = s_canvas_h;
        if (s_ppa_fill && x1 - x0 > 0 && y1 - y0 > 0 &&
            (x1 - x0) * (y1 - y0) >= UI_PPA_FILL_MIN_PX) {
            ppa_fill_oper_config_t op = {};
            op.out.buffer = _buf;
            op.out.buffer_size =
                ((size_t)s_canvas_w * s_canvas_h * 2 + 63) & ~(size_t)63;
            op.out.pic_w = (uint32_t)s_canvas_w;
            op.out.pic_h = (uint32_t)s_canvas_h;
            op.out.block_offset_x = (uint32_t)x0;
            op.out.block_offset_y = (uint32_t)y0;
            op.out.fill_cm = PPA_FILL_COLOR_MODE_RGB565;
            op.fill_block_w = (uint32_t)(x1 - x0);
            op.fill_block_h = (uint32_t)(y1 - y0);
            /* 565 -> 888 by zero-extend; PPA truncates back, lossless */
            op.fill_argb_color.a = 0xFF;
            op.fill_argb_color.r = (uint32_t)(((px >> 11) & 0x1F) << 3);
            op.fill_argb_color.g = (uint32_t)(((px >> 5) & 0x3F) << 2);
            op.fill_argb_color.b = (uint32_t)((px & 0x1F) << 3);
            op.mode = PPA_TRANS_MODE_BLOCKING;
            if (ppa_do_fill(s_ppa_fill, &op) == ESP_OK)
                return;
            /* any PPA error: fall through to the CPU loop */
        }
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++)
                _buf[(size_t)yy * s_canvas_w + xx] = px;
    }

    void put_pixel(int x, int y, uint16_t px)
    {
        if (x >= 0 && x < s_canvas_w && y >= 0 && y < s_canvas_h)
            _buf[(size_t)y * s_canvas_w + x] = px;
    }

    void line(int x0, int y0, int x1, int y1, uint16_t px)
    {
        /* Bresenham */
        int dx = x1 > x0 ? x1 - x0 : x0 - x1;
        int dy = y1 > y0 ? y1 - y0 : y0 - y1;
        int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx - dy;
        for (;;) {
            put_pixel(x0, y0, px);
            if (x0 == x1 && y0 == y1)
                break;
            int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

    void text(const ui_cmd_t &cmd)
    {
        if (!cmd.text)
            return;
        lv_layer_t layer;
        lv_canvas_init_layer(_canvas, &layer);
        lv_draw_label_dsc_t dsc;
        lv_draw_label_dsc_init(&dsc);
        dsc.color = lv_color_hex(cmd.color);
        dsc.text = cmd.text;
        dsc.font = ui_font();
        lv_area_t coords = { cmd.x, cmd.y, s_canvas_w - 1, s_canvas_h - 1 };
        lv_draw_label(&layer, &dsc, &coords);
        lv_canvas_finish_layer(_canvas, &layer);
    }

    /* Blit one monospace glyph, fg blended over whatever bg is already in
       the cell, clipped to the cell box (no lv_draw_label, no layer). */
    void blit_glyph(int cx0, int cy0, uint32_t cp, uint16_t fg)
    {
        const lv_font_t *font = &font_term_mono;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, cp, 0))
            return;
        if (g.box_w == 0 || g.box_h == 0) /* space etc. */
            return;
        /* Get the raw A4 bitmap and decode it ourselves. NOTE: the public
           lv_font_get_glyph_bitmap() force-resets req_raw_bitmap to 0
           (decoding to A8 into a draw_buf we don't pass -> NULL deref
           crash). Call the font's method directly so req_raw_bitmap=1
           sticks and we get the raw glyph_bitmap pointer. Packing is a
           continuous 4bpp bitstream, MSB nibble first (host-verified in
           fonts/decode_test.py). */
        const lv_font_t *rf = g.resolved_font ? g.resolved_font : font;
        if (!rf->get_glyph_bitmap)
            return;
        g.req_raw_bitmap = 1;
        const uint8_t *bmp = (const uint8_t *)rf->get_glyph_bitmap(&g, NULL);
        if (!bmp)
            return;
        /* fmt_txt 4bpp: rows are padded to `stride` bytes if stride>0, else
           the glyph is a continuous bitstream (no per-row padding). */
        const int bpp = 4;
        int baseline = cy0 + (UI_CELL_H - font->base_line);
        int gx0 = cx0 + g.ofs_x;
        int gy0 = baseline - g.ofs_y - g.box_h;
        int clipR = cx0 + UI_CELL_W, clipB = cy0 + UI_CELL_H;
        for (int py = 0; py < g.box_h; py++) {
            int y = gy0 + py;
            if (y < cy0 || y >= clipB || y < 0 || y >= s_canvas_h)
                continue;
            int rowbit = g.stride ? py * g.stride * 8 : py * g.box_w * bpp;
            for (int px = 0; px < g.box_w; px++) {
                int x = gx0 + px;
                if (x < cx0 || x >= clipR || x < 0 || x >= s_canvas_w)
                    continue;
                int bitpos = rowbit + px * bpp;
                uint8_t byte = bmp[bitpos >> 3];
                int a = (bitpos & 4) ? (byte & 0x0F) : (byte >> 4);
                if (!a)
                    continue;
                size_t idx = (size_t)y * s_canvas_w + x;
                _buf[idx] = a == 15 ? fg : ui_blend565(_buf[idx], fg, a);
            }
        }
    }

    /* Compose one glyph's coverage into the run's A8 buffer (no canvas
       access, no blending — just alpha bytes). Mirrors blit_glyph's
       positioning/clipping; a4*17 so 15 -> 255 = fully opaque. */
    void compose_glyph_a8(uint8_t *dst, int dst_w, int cx0, uint32_t cp)
    {
        const lv_font_t *font = &font_term_mono;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, cp, 0))
            return;
        if (g.box_w == 0 || g.box_h == 0)
            return;
        const lv_font_t *rf = g.resolved_font ? g.resolved_font : font;
        if (!rf->get_glyph_bitmap)
            return;
        g.req_raw_bitmap = 1;
        const uint8_t *bmp = (const uint8_t *)rf->get_glyph_bitmap(&g, NULL);
        if (!bmp)
            return;
        const int bpp = 4;
        int baseline = UI_CELL_H - font->base_line;
        int gx0 = cx0 + g.ofs_x;
        int gy0 = baseline - g.ofs_y - g.box_h;
        int clipL = cx0 < 0 ? 0 : cx0;
        int clipR = cx0 + UI_CELL_W;
        if (clipR > dst_w)
            clipR = dst_w;
        for (int py = 0; py < g.box_h; py++) {
            int y = gy0 + py;
            if (y < 0 || y >= UI_CELL_H)
                continue;
            int rowbit = g.stride ? py * g.stride * 8 : py * g.box_w * bpp;
            uint8_t *drow = dst + (size_t)y * dst_w;
            for (int px = 0; px < g.box_w; px++) {
                int x = gx0 + px;
                if (x < clipL || x >= clipR)
                    continue;
                int bitpos = rowbit + px * bpp;
                uint8_t byte = bmp[bitpos >> 3];
                int a = (bitpos & 4) ? (byte & 0x0F) : (byte >> 4);
                if (a)
                    drow[x] = (uint8_t)(a * 17);
            }
        }
    }

    /* Draw a run of cells (one fg/bg) starting at (col,row) using the
       monospace grid font. UTF-8 decoded to codepoints. Long runs that
       sit fully on the grid go compose-then-one-PPA-blend; short runs
       and any PPA failure use the per-glyph CPU blit. */
    void cells(const ui_cmd_t &cmd)
    {
        if (!cmd.text)
            return;
        int col = cmd.x, row = cmd.y;
        uint16_t fg = lv_color_to_u16(lv_color_hex(cmd.color));
        uint16_t bg = lv_color_to_u16(lv_color_hex(cmd.bg));
        const uint8_t *s = (const uint8_t *)cmd.text;
        /* the run has a single bg: clear it in one rect (instead of one
           9x24 fill per cell) — row-contiguous, and long runs clear the
           PPA threshold */
        int n = 0;
        for (const uint8_t *p = s; *p; p++)
            if ((*p & 0xC0) != 0x80)
                n++;
        int x0 = col * UI_CELL_W, y0 = row * UI_CELL_H, bw = n * UI_CELL_W;
        fill_rect(x0, y0, bw, UI_CELL_H, bg);

        if (s_ppa_blend && n >= UI_PPA_CELLS_MIN_CELLS && x0 >= 0 &&
            y0 >= 0 && x0 + bw <= s_canvas_w &&
            y0 + UI_CELL_H <= s_canvas_h &&
            (size_t)bw * UI_CELL_H <= sizeof(s_cells_a8)) {
            memset(s_cells_a8, 0, (size_t)bw * UI_CELL_H);
            const uint8_t *p = s;
            for (int c = 0; *p; c++)
                compose_glyph_a8(s_cells_a8, bw, c * UI_CELL_W,
                                 cells_utf8_next(p));
            ppa_blend_oper_config_t op = {};
            op.in_bg.buffer = _buf;
            op.in_bg.pic_w = (uint32_t)s_canvas_w;
            op.in_bg.pic_h = (uint32_t)s_canvas_h;
            op.in_bg.block_w = (uint32_t)bw;
            op.in_bg.block_h = UI_CELL_H;
            op.in_bg.block_offset_x = (uint32_t)x0;
            op.in_bg.block_offset_y = (uint32_t)y0;
            op.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
            op.in_fg.buffer = s_cells_a8;
            op.in_fg.pic_w = (uint32_t)bw;
            op.in_fg.pic_h = UI_CELL_H;
            op.in_fg.block_w = (uint32_t)bw;
            op.in_fg.block_h = UI_CELL_H;
            op.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_A8;
            op.out.buffer = _buf;
            op.out.buffer_size =
                ((size_t)s_canvas_w * s_canvas_h * 2 + 63) & ~(size_t)63;
            op.out.pic_w = (uint32_t)s_canvas_w;
            op.out.pic_h = (uint32_t)s_canvas_h;
            op.out.block_offset_x = (uint32_t)x0;
            op.out.block_offset_y = (uint32_t)y0;
            op.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
            op.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
            op.bg_alpha_fix_val = 255;
            op.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
            op.fg_fix_rgb_val.r = (uint32_t)(((fg >> 11) & 0x1F) << 3);
            op.fg_fix_rgb_val.g = (uint32_t)(((fg >> 5) & 0x3F) << 2);
            op.fg_fix_rgb_val.b = (uint32_t)((fg & 0x1F) << 3);
            op.mode = PPA_TRANS_MODE_BLOCKING;
            if (ppa_do_blend(s_ppa_blend, &op) == ESP_OK)
                return;
            /* any PPA error: fall through to the per-glyph CPU path */
        }

        int c = col;
        while (*s) {
            uint32_t cp = cells_utf8_next(s);
            blit_glyph(c * UI_CELL_W, row * UI_CELL_H, cp, fg);
            c++;
        }
    }

    /* Scroll cell-rows [top,bot] by n (n>0 up, n<0 down) in the buffer
       (memmove), filling the vacated rows with `fill`. */
    void scroll(int top, int bot, int n, uint16_t fill)
    {
        if (top < 0) top = 0;
        if (bot > s_canvas_h / UI_CELL_H - 1) bot = s_canvas_h / UI_CELL_H - 1;
        if (bot <= top || n == 0)
            return;
        int rows = bot - top + 1;
        int an = n < 0 ? -n : n;
        if (an >= rows)
            an = rows; /* whole region cleared */
        int y_top = top * UI_CELL_H;
        int move_rows = (rows - an) * UI_CELL_H; /* pixel rows to move */
        size_t rowbytes = (size_t)s_canvas_w * 2;
        if (move_rows > 0) {
            if (n > 0) { /* up: pull lower content toward the top */
                memmove(_buf + (size_t)y_top * s_canvas_w,
                        _buf + (size_t)(y_top + an * UI_CELL_H) * s_canvas_w,
                        (size_t)move_rows * rowbytes);
            } else { /* down: push content toward the bottom */
                memmove(_buf + (size_t)(y_top + an * UI_CELL_H) * s_canvas_w,
                        _buf + (size_t)y_top * s_canvas_w,
                        (size_t)move_rows * rowbytes);
            }
        }
        /* clear the vacated `an` cell-rows */
        if (n > 0)
            fill_rect(0, (bot + 1 - an) * UI_CELL_H, s_canvas_w, an * UI_CELL_H, fill);
        else
            fill_rect(0, y_top, s_canvas_w, an * UI_CELL_H, fill);
    }

    void apply(const ui_cmd_t &cmd)
    {
        uint16_t px = lv_color_to_u16(lv_color_hex(cmd.color));
        switch (cmd.op) {
        case UI_CMD_CLEAR:
        case UI_CMD_FILL:
            fill_all(px);
            break;
        case UI_CMD_RECT:
            fill_rect(cmd.x, cmd.y, cmd.w, cmd.h, px);
            break;
        case UI_CMD_LINE:
            line(cmd.x, cmd.y, cmd.w, cmd.h, px);
            break;
        case UI_CMD_TEXT:
            text(cmd);
            break;
        case UI_CMD_PIXEL:
            put_pixel(cmd.x, cmd.y, px);
            break;
        case UI_CMD_CELLS:
            cells(cmd);
            break;
        case UI_CMD_SCROLL:
            scroll(cmd.x, cmd.y, cmd.w, px);
            break;
        default:
            break;
        }
    }

    lv_obj_t *_canvas = nullptr;
    uint16_t *_buf = nullptr;
    uint32_t _seen_gen = 0;
    char _task[sizeof(ui_status_t::task_name)] = "";
    char _origin[sizeof(ui_status_t::task_origin)] = "";
};

extern "C" void ui_tab5_start(void)
{
    /* the data plane must exist before app_main registers the print
       sink, and must stay usable even if the panel init below fails
       (ui_tab5_log/set_status/cmd no-op while these are NULL) */
    s_log_mtx = xSemaphoreCreateMutex();
    s_status_mtx = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(UI_CMD_QUEUE_DEPTH, sizeof(ui_cmd_t));

    ui_panel_variant_t variant = panel_reset_and_detect();
    if (variant == UI_PANEL_NONE)
        return;
    if (backlight_init() != ESP_OK)
        ESP_LOGW(TAG, "backlight init failed, continuing");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    if (display_init(variant, &io, &panel) != ESP_OK)
        return;

    /* LVGL task on Core 1, low priority (js_task runs on Core 0) */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 1;
    port_cfg.task_priority = 4;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return;
    }

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = io;
    disp_cfg.panel_handle = panel;
    disp_cfg.buffer_size = UI_LCD_H_RES * UI_LVGL_BUF_LINES;
    disp_cfg.double_buffer = false;
    disp_cfg.hres = UI_LCD_H_RES;
    disp_cfg.vres = UI_LCD_V_RES;
    disp_cfg.monochrome = false;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_dma = true;

    lvgl_port_display_dsi_cfg_t dsi_cfg = {};
    dsi_cfg.flags.avoid_tearing = false;

    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed");
        return;
    }

    /* JS-visible canvas resolution (everything below the status bar);
       published before js_task starts, so ui.size() is always valid */
    s_canvas_w = UI_LCD_H_RES;
    s_canvas_h = UI_LCD_V_RES - UI_STATUSBAR_H;

    /* status bar + console + canvas, driven by mooncake from an
       lv_timer (i.e. inside the LVGL task, under the port lock) */
    lvgl_port_lock(0);
    s_root_scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(s_root_scr, lv_color_hex(UI_COL_BG), 0);
    auto &mc = mooncake::GetMooncake();
    mc.createExtension(std::make_unique<StatusBar>());
    mc.openApp(mc.installApp(std::make_unique<ConsoleApp>()));
    mc.openApp(mc.installApp(std::make_unique<CanvasApp>()));
    lv_timer_create(
        [](lv_timer_t *) {
            mooncake::GetMooncake().update();
            touch_observe();
        },
        16, nullptr);
    lvgl_port_unlock();

    touch_init(variant, disp);

    backlight_set(100);
    ESP_LOGI(TAG, "UI up (%dx%d)", UI_LCD_H_RES, UI_LCD_V_RES);

    /* note: no em-dash etc. here — U+2014 is in neither font (tofu) */
    static const char greet[] = "mqjs コンソール: JS の print がここに流れます";
    ui_tab5_log(greet, sizeof greet - 1);
}

/* ---- camera viewfinder overlay (cam_tab5) ----
   An lv_canvas on the top layer whose RGB565 buffer the cam_scan task
   writes downscaled frames into. Created once and kept (460KB PSRAM
   for 640x360); hidden between scans. All entry points take the LVGL
   port lock — they are called from the cam_scan task. */
static lv_obj_t *s_cam_cv;
static uint16_t *s_cam_cv_buf;
static lv_obj_t *s_cam_lbl;
static int s_cam_cv_h;

void *ui_tab5_cam_canvas(int w, int h)
{
    if (!s_root_scr)
        return NULL;
    lvgl_port_lock(0);
    if (!s_cam_cv) {
        /* PPA writes this buffer (cam_tab5 hardware-rotates frames into
           it): DMA-capable + cache-line aligned */
        s_cam_cv_buf = (uint16_t *)heap_caps_aligned_alloc(
            64, (size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (s_cam_cv_buf) {
            lv_canvas_set_buffer(s_cam_cv = lv_canvas_create(lv_layer_top()),
                                 s_cam_cv_buf, w, h, LV_COLOR_FORMAT_RGB565);
            lv_obj_align(s_cam_cv, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H + 24);
            lv_obj_set_style_border_width(s_cam_cv, 2, 0);
            lv_obj_set_style_border_color(s_cam_cv, lv_color_hex(0x2ECC71), 0);
            s_cam_cv_h = h;
        }
    }
    if (s_cam_cv)
        lv_obj_clear_flag(s_cam_cv, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
    return s_cam_cv_buf;
}

void ui_tab5_cam_canvas_update(void)
{
    if (!s_cam_cv)
        return;
    lvgl_port_lock(0);
    lv_obj_invalidate(s_cam_cv);
    lvgl_port_unlock();
}

void ui_tab5_cam_canvas_hide(void)
{
    if (!s_cam_cv)
        return;
    lvgl_port_lock(0);
    lv_obj_add_flag(s_cam_cv, LV_OBJ_FLAG_HIDDEN);
    if (s_cam_lbl)
        lv_obj_add_flag(s_cam_lbl, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

/* decode/near-miss readout under the viewfinder (the cam_scan task's
 * "ひげ線" callout box; the line itself is drawn into the canvas) */
void ui_tab5_cam_overlay_text(const char *utf8)
{
    if (!s_root_scr || !s_cam_cv)
        return;
    lvgl_port_lock(0);
    if (!s_cam_lbl) {
        s_cam_lbl = lv_label_create(lv_layer_top());
        lv_obj_set_width(s_cam_lbl, 560);
        lv_label_set_long_mode(s_cam_lbl, LV_LABEL_LONG_WRAP);
        /* default LVGL font has no JP glyphs (tofu) — use the Noto
           chain like every other label */
        lv_obj_set_style_text_font(s_cam_lbl, ui_font(), 0);
        lv_obj_set_style_text_color(s_cam_lbl, lv_color_hex(0xE0E6EA), 0);
        lv_obj_set_style_bg_color(s_cam_lbl, lv_color_hex(0x101820), 0);
        lv_obj_set_style_bg_opa(s_cam_lbl, LV_OPA_80, 0);
        lv_obj_set_style_pad_all(s_cam_lbl, 8, 0);
        lv_obj_align(s_cam_lbl, LV_ALIGN_TOP_MID, 0,
                     UI_STATUSBAR_H + 24 + s_cam_cv_h + 6);
    }
    lv_label_set_text(s_cam_lbl, utf8);
    lv_obj_clear_flag(s_cam_lbl, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

#endif /* CONFIG_MQJS_TAB5_UI */
