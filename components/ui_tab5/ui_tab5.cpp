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
/* private to esp_lvgl_port; on the include path via its COMPONENT_DIR
   (see CMakeLists) so __wrap_lvgl_port_ppa_create gets the real cfg
   type instead of a hand-copied one that could drift */
#include "lcd_ppa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "core/animation/animate_value/animate_value.hpp"
#include "mooncake.h"

#include "kbd_core.h"
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
 *   Noto JP 20 -> HackGen term mono 17 (ASCII gaps, box drawing, and
 *                 the whole Nerd Font BMP set incl. LV_SYMBOL's
 *                 FontAwesome points)
 *              -> Montserrat 14 (the handful of points nothing else has)
 * The NF link makes icon glyphs available to EVERY text surface
 * (status bar, widgets, console lines, ui.text) — apps just put
 * "\uE7xx"-style characters in strings (system decoration / @icon).
 *
 * The middle link used to be Montserrat 20 plus a SECOND 20px Nerd Font
 * (fonts/font_nf_ui_20.c). font_term_mono's codepoint coverage is a
 * strict superset of that font's — same ~3,490 icons, verified point by
 * point (design doc §7.6) — so the pair cost 533 KB of flash to draw the
 * same icons 3px larger; both are gone. Montserrat 14 stays on as the
 * tail because LV_SYMBOL_BACKSPACE (U+F55A) and LV_SYMBOL_NEW_LINE
 * (U+F8A2) — the on-screen keyboard's BS and Enter caps — exist in NO
 * other font we link, and CONFIG_LV_USE_FONT_PLACEHOLDER would draw
 * them as tofu boxes rather than drop them silently. It costs nothing:
 * LV_FONT_DEFAULT_MONTSERRAT_14 already pulls it into the image. It is
 * LAST so every point the two share resolves at 17px, not 14px. */
extern "C" {
LV_FONT_DECLARE(font_noto_jp_20_4);
LV_FONT_DECLARE(font_term_mono);
}

static const lv_font_t *ui_font(void)
{
    static lv_font_t jp, term;
    if (!jp.line_height) {
        term = font_term_mono; /* mutable copy: .fallback is ours to set */
        term.fallback = &lv_font_montserrat_14;
        jp = font_noto_jp_20_4;
        jp.fallback = &term;
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
 * 24px line height. Glyphs are blitted directly (no lv_draw_label).
 *
 * Clipping is driven by ui_cell_width() — the SAME table ui.cellWidth
 * exposes to JS — and NOT by the glyph's own box_w:
 *   - width 1 (and 0) clips to one cell. This is load-bearing, not
 *     conservatism: the box-drawing glyphs are 11px wide with ofs_x=-1,
 *     i.e. they deliberately overhang the 9px cell on both sides, and
 *     cutting them at the cell edge is what makes U+2500-259F tile
 *     seamlessly. Widening the clip to box_w would bleed U+2588 into
 *     the neighbouring cell and fight the next run's bg fill.
 *   - width 2 (CJK) clips to two cells, because the glyph really does
 *     span them.
 * CALLER CONTRACT (ui.cells): one codepoint per column. After a width-2
 * codepoint the caller appends a filler codepoint (a space — ssh_vt.js
 * calls the model cell CONT) so that codepoint count == column count.
 * The C side therefore measures width to size the CLIP, never to
 * advance the column: doing both would double-count and shear every
 * line of CJK to the right. */
#include "driver/ppa.h"
#include "ui_cell_width.h"

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
/* 720px = 80 cells: the max PPA segment (cells_run splits longer runs,
   e.g. 142-cell landscape rows); 64B-aligned (and 64B-multiple) for PPA
   cache ops. Deliberately NOT grown for landscape: internal SRAM. */
static uint8_t s_cells_a8[720 * UI_CELL_H] __attribute__((aligned(64)));

/* Columns one glyph is allowed to paint, per the shared ui_cell_width()
   table. Width 0 (combining marks) is clamped UP to 1 rather than
   skipped: ui.cells is a column-indexed API, so whatever the caller put
   in a column gets that column — dropping it would leave the cell blank
   with no way for the caller to notice. */
static inline int cells_glyph_cols(uint32_t cp)
{
    return ui_cell_width(cp) == 2 ? 2 : 1;
}

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

/* A width-2 glyph that had to be cut short because it ran past the end of
   its run. Both cells paths clamp (the CPU blit to the run's right edge,
   the PPA compose to the A8 buffer) so nothing is ever written out of
   bounds — but the clamp is silent, and a sheared kanji on screen looks
   exactly like a font that is missing the glyph's right half. Count it and
   say so, rate-limited, so "the caller forgot the CONT filler" can be told
   apart from "the font is wrong" on the device instead of by guesswork. */
static uint32_t s_cells_clip_hits;
static void cells_note_clip(void)
{
    uint32_t n = ++s_cells_clip_hits;
    if (n == 1 || (n & 0xFF) == 0)
        ESP_LOGW(TAG, "cells: wide glyph clipped at run end x%u "
                      "(caller dropped the CONT filler?)", (unsigned)n);
}

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

/* 64KB console scrollback: PSRAM-resident (text only, never DMA/ISR) to keep
   internal SRAM free for DMA-capable allocations (esp_hosted SDIO RX path). */
static ui_log_line_t *s_log;
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

/* Landscape rotation (keyboard dock). The handles below are the few
   fixed-size widgets that ui_tab5_set_landscape must re-size by hand —
   everything else is LV_PCT/flex/align-based and follows the display's
   resolution change on its own. */
static lv_display_t *s_disp;
static bool s_landscape;
static bool s_hw_kb;      /* keyboard dock present: on-screen keys off */
static int s_app_kb_mode; /* the ui.keyboard mode the app last asked for
                             (0/1/2) — re-applied when the dock
                             (dis)appears; apps stay unaware of it */
static lv_obj_t *s_sb_bar, *s_sb_row, *s_sb_event; /* status bar chrome */
static lv_obj_t *s_console_panel;
static lv_obj_t *s_js_canvas;      /* CanvasApp presentation object */
static uint16_t *s_js_canvas_buf;  /* ... and its pixel buffer */
static size_t s_js_canvas_bytes;   /* allocation size (portrait = max) */

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

/* ------------------------------------------------------------------ */
/* UI-task work seam (docs/term-design.md §5/§7.2). A generic job queue */
/* plus a per-frame hook: the native terminal parses and serialises on  */
/* this task so its grid stays single-writer, and ui_tab5 never learns  */
/* what term_core is.                                                   */
/* ------------------------------------------------------------------ */
#define UI_JOB_QUEUE_DEPTH 16

typedef struct {
    ui_tab5_job_fn fn;
    void *arg;
} ui_job_t;

static QueueHandle_t s_job_queue;
static TaskHandle_t s_ui_task;      /* stamped by the frame timer */
static ui_tab5_job_fn s_frame_fn;
static void *s_frame_arg;

extern "C" bool ui_tab5_post_job(ui_tab5_job_fn fn, void *arg,
                                 uint32_t timeout_ms)
{
    if (!s_job_queue || !fn)
        return false;
    ui_job_t job = { fn, arg };
    TickType_t ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : 0;
    return xQueueSend(s_job_queue, &job, ticks) == pdTRUE;
}

extern "C" bool ui_tab5_is_ui_task(void)
{
    return s_ui_task && xTaskGetCurrentTaskHandle() == s_ui_task;
}

extern "C" void ui_tab5_set_frame_cb(ui_tab5_job_fn fn, void *arg)
{
    s_frame_arg = arg;
    s_frame_fn = fn;
}

/* Called from the mooncake frame timer, i.e. on the UI task under the
   LVGL port lock, after the canvas has drained its command queue. */
static void ui_run_frame_work(void)
{
    s_ui_task = xTaskGetCurrentTaskHandle();
    if (s_job_queue) {
        ui_job_t job;
        while (xQueueReceive(s_job_queue, &job, 0) == pdTRUE)
            job.fn(job.arg);
    }
    if (s_frame_fn)
        s_frame_fn(s_frame_arg);
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

/* Landscape support (keyboard dock): the panel scans portrait, content
   is PPA-rotated per flush by esp_lvgl_port (flags.sw_rotate +
   CONFIG_LVGL_PORT_ENABLE_PPA). The dock holds the Tab5 turned 90° to
   the left (CCW), so content must rotate clockwise into panel coords —
   if the device shows it upside down, flip this one constant to
   LV_DISPLAY_ROTATION_270. */
#define UI_ROT_LANDSCAPE LV_DISPLAY_ROTATION_90

/* current logical width — overlays created after a rotation must size
   themselves to this, not the compile-time portrait width */
static inline int ui_cur_hres(void)
{
    return s_landscape ? UI_LCD_V_RES : UI_LCD_H_RES;
}
#define UI_DSI_LANES        2
#define UI_DPHY_LDO_CHAN    3
#define UI_DPHY_LDO_MV      2500
#define UI_BACKLIGHT_GPIO   22
/* 50 lines. This was halved to 25 because sw_rotate makes esp_lvgl_port
   allocate a second, equally sized PPA rotation scratch with the SAME
   caps (internal DMA), so a 50-line buffer cost 144KB of the scarcest
   pool on this build (esp-hosted SDIO owns most of internal SRAM).
   __wrap_lvgl_port_ppa_create now puts that scratch in PSRAM, so the
   internal footprint is 72KB either way — the same as before — and the
   draw buffer gets all of it. Fewer, taller chunks: a keyboard map swap
   goes from 16 chunks to 8, and every chunk is a fresh walk of the
   widget tree (see s_kb_row).
   MEASURED on device, 25 vs 50 lines, everything else equal (draw us /
   chunks). Chunks halve exactly and draw follows:
     map swap, case flip   7940 / 12  ->  5881 / 6    -26%
     map swap, all 4 rows  9955 / 16  ->  7368 / 8    -26%
     whole screen         28418 / 52  -> 22787 / 26   -20%
     one key                561 / 1   ->   554 / 1    (1 chunk either way)
   Internal DMA is unchanged: free-at-net-up was 179707 B with the old
   25-line pair and 179803 B with this one.
   Do NOT raise this further without re-checking the heap: 50 lines is
   72KB and the largest contiguous internal block is ~80KB, so 75 lines
   (108KB) would not fit. */
#define UI_LVGL_BUF_LINES   50

/* The rotation scratch is written by the PPA and read by the LCD DMA —
   never by the CPU — so PSRAM costs it bandwidth, not cache misses, and
   only in landscape. Landscape means the keyboard dock is attached,
   which is exactly when the on-screen keyboard is suppressed and the
   repaints are cheap. The draw buffers themselves stay internal: PSRAM
   for those was measured at ~2x the render time (see below).
   The port gives the scratch and the draw buffers one shared pair of
   caps flags (lvgl_port_display_cfg_t has a single buff_dma/buff_spiram
   for both), so splitting them means intercepting the one call that
   allocates it. P4 PSRAM is DMA-capable (SOC_PSRAM_DMA_CAPABLE=1), so
   asking for DMA|SPIRAM together is legal here. */
extern "C" lvgl_port_ppa_handle_t
__real_lvgl_port_ppa_create(const lvgl_port_ppa_cfg_t *cfg);
extern "C" lvgl_port_ppa_handle_t
__wrap_lvgl_port_ppa_create(const lvgl_port_ppa_cfg_t *cfg)
{
    lvgl_port_ppa_cfg_t in_psram = *cfg;
    in_psram.flags.buff_spiram = 1;
    ESP_LOGI(TAG, "PPA rotation scratch -> PSRAM (%u bytes)",
             (unsigned)in_psram.buffer_size);
    return __real_lvgl_port_ppa_create(&in_psram);
}
/* Touch sampling period. LVGL's default is LV_DEF_REFR_PERIOD (33 ms),
   which is too coarse to catch a quick tap — see lvgl_port_add_touch. */
#define UI_TOUCH_READ_MS    10
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
       ILI9881C guess on an ST7123 panel is exactly what blanks it.

       i2c_master_probe() is safe HERE and only here: this bus is freshly
       created, every probe below runs BEFORE the single read on it
       (st712x_flavour), and the bus is deleted right after. IDF's probe
       leaves bus->i2c_trans.ops pointing at its own stack and never
       resets the ISR read state, so a probe that FOLLOWS a read on the
       same bus panics — that is the dock-bus crash written up in
       components/kbd_tab5/kbd_tab5.c. Keep the probes ahead of the read:
       st712x_flavour() never returns UI_PANEL_NONE, so the retry loop
       cannot wrap back around to a probe after it. */
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
    if (!s_touch_indev) {
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
    } else {
        /* LVGL samples the panel from the same timer loop it renders in,
           every LV_DEF_REFR_PERIOD (33 ms) by default. That is coarser
           than a quick tap: press and release can both fall between two
           samples, which is why typing fast on the on-screen keyboard
           dropped keys and a double-tap could not be landed at all. One
           small I2C read every 10 ms buys three times the resolution. */
        lv_timer_set_period(lv_indev_get_read_timer(s_touch_indev),
                            UI_TOUCH_READ_MS);
        ESP_LOGI(TAG, "touch up (%s, %d ms sampling)",
                 variant == UI_PANEL_ILI9881C ? "GT911" : "ST7123",
                 UI_TOUCH_READ_MS);
    }
}

/* runs in the LVGL task (from the mooncake lv_timer): mirror the indev
   state LVGL already polled into JS touch events. Coordinates are
   canvas-relative (status bar clamps to y=0); kind 0=down 1=move 2=up. */
/* viewfinder modal state (set under the LVGL lock by the cam entry
   points below; read on the LVGL task) — while true, JS gets NO touch
   events: the scrim owns the screen like a web modal backdrop */
static volatile bool s_cam_modal;

static void touch_observe(void)
{
    static bool was_pressed;
    static lv_point_t last;

    if (!s_touch_indev)
        return;
    if (s_cam_modal) {
        /* close any in-flight gesture so ui.onTouch apps don't hang in
           "pressed" state, then go silent for the modal's lifetime */
        if (was_pressed)
            mqjs_post_touch(last.x, last.y, 2);
        was_pressed = false;
        return;
    }
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
        s_sb_bar = bar; /* resized on rotation */
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
        s_sb_row = row; /* resized on rotation */
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
        s_sb_event = _event_lbl; /* resized on rotation */
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
            /* bar width, not UI_LCD_H_RES: tracks landscape rotation */
            int w = (int)((uint32_t)lv_obj_get_width(s_sb_bar) * capped /
                          UI_HOLD_MS);
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
        s_console_panel = _panel; /* resized on rotation */
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
        if (!s_log_mtx)
            return;
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
/* Phase 4: on-screen keyboard + T3a control bar.                      */
/* Hidden until JS calls ui.keyboard(1|2). Every key goes through      */
/* kbd_core — the same engine the A164 dock driver uses — so a touch   */
/* Ctrl+C and a dock Ctrl+C put the identical byte on the wire, and    */
/* Shift/Ctrl/Alt behave the same on both surfaces (tap = one shot,    */
/* double-tap = lock). Printables and \b \t \n travel as themselves;   */
/* Esc, arrows, F-keys, copy/paste travel as "\0name" tokens whose     */
/* meaning is the app's (design §7 keytoken).                          */
/* All of this runs in the LVGL task (queue drain / event callback).   */
/* ------------------------------------------------------------------ */

#define UI_KB_H      400 /* clipboard strip + 4 key rows on the 5" panel */
#define UI_KB_TOP_H  48  /* the strip: clipboard preview + collapse */
#define UI_KB_LOCK_W 150 /* the strip's latched-modifier readout */
#define UI_CB_H      80  /* T3a control bar row above the keyboard */

#define KB_ROWS      4
/* Geometry the four row matrices have to reproduce exactly, taken from
   what one 4-row matrix laid out in the same box (lv_buttonmatrix.c
   row_y1/row_y2): content height 352-2*4, minus three 4px row gaps,
   split four ways = an 83px key, and rows pitched 87 apart starting
   4px down. Each row matrix is one pitch tall and carries the gap as
   its own bottom padding, so key rects land on the same scanlines the
   monolithic matrix used. */
#define KB_AREA_H    (UI_KB_H - UI_KB_TOP_H)                       /* 352 */
#define KB_PAD       4
#define KB_KEY_H     ((KB_AREA_H - 2 * KB_PAD - 3 * KB_PAD) / KB_ROWS) /* 83 */
#define KB_ROW_PITCH (KB_KEY_H + KB_PAD)                            /* 87 */

/* One button matrix PER ROW, not one for the whole keyboard. LVGL walks
   every button of a matrix for every 25-line chunk it renders, with no
   clip test in the loop (lv_buttonmatrix.c draw_main) — each walked
   button costs two descriptor memcpys, a glyph-level text measure and
   two malloc'd draw tasks whether or not it intersects the chunk. One
   matrix meant 15 chunks x 36 keys = 540 walks for 144 useful ones.
   Chunks span the full width, so a row either intersects or it does
   not, and LVGL's own per-object intersect (lv_refr.c) now rejects the
   2-3 rows a chunk misses before any of that runs — the same rejection
   a clip test inside the loop would achieve, without forking LVGL.
   MEASURED on device, same session A/B against main (portrait, -O2,
   draw us / chunks). Note main's "map swap (case)" re-maps all four
   rows, so the honest pair is that against "all 4 rows":
                          main (one matrix)   split
     one key invalidate      1085 / 1          561 / 1   -48%
     map swap, all 4 rows   17142 / 15        9955 / 16  -42%
     map swap, sym          16029 / 15        9987 / 16  -38%
     whole screen           33294 / 52       28418 / 52  -15%
   and the case flip — three rows re-mapped, which is what a Shift lock
   actually does — 7940 / 12, i.e. 17.1 -> 7.9 ms, -54%.
   s_kb stays the handle the rest of the file uses: it is now the
   container the rows live in, and it paints the background (the rows
   only cover from the first key down). */
static lv_obj_t *s_kb;       /* key area container; parent of s_kb_row */
static lv_obj_t *s_kb_row[KB_ROWS]; /* one button matrix per key row */
static lv_obj_t *s_kb_top;   /* strip above it (clipboard + collapse) */
static lv_obj_t *s_kb_clip;  /* paste button; its label is the preview */
static lv_obj_t *s_kb_clip_lbl;
static lv_obj_t *s_kb_lock_lbl; /* which modifiers are latched, in words */
static lv_timer_t *s_kb_clip_tmr; /* another app may replace the value */
static bool s_kb_sym;        /* symbol layer showing */
static int s_kb_map_shown = -1; /* layer the matrix currently draws;
                                   -1 = none yet (also after a rebuild) */
/* Where the Shift key currently is. Row as well as id now: the layers
   move it around, and the sym layer has none at all. */
static int s_kb_shift_row = -1;
static uint32_t s_kb_shift_id = LV_BUTTONMATRIX_BUTTON_NONE;
static lv_obj_t *s_cbar;     /* T3a terminal control bar (mode 2) */
static bool s_cbar_fn;       /* current map: false = main, true = F1-F12 */
static int s_cbar_map_fn = -1; /* map the bar actually draws; -1 = none */
static lv_obj_t *s_root_scr; /* console screen: fixed parent for s_kb (a
                                widget screen could be active when JS calls
                                ui.keyboard(1); parenting there would leave
                                s_kb dangling when that screen is freed) */

/* Modifier state of the TOUCH surface (keyboard + bar together, so a
   bar Ctrl applies to the next on-screen letter). The dock keeps its
   own instance: a held physical Ctrl and a tapped one-shot Ctrl are
   deliberately different things. LVGL task only, hence no locking. */
static kbd_mods_t s_ui_mods;

static void kb_apply_map(void);        /* fwd: the bar spends Shift */
static void kb_apply_shift_flag(void); /* fwd: ... and must show it */
static void kb_lock_refresh(void);  /* fwd: which modifiers are latched */
static void ui_kb_refresh(void);    /* fwd: deferred map/label work */

/* T3a control bar (ssh-terminal-design §7). Ctrl/Alt drive the shared
   engine (tap = one-shot, double-tap = lock) instead of posting a
   token, so C resolves the chord exactly as it does for the dock. Fn
   flips the bar between the main map and F1-F12 (bar-local, no key). */
static const char *CB_LBL_CTRL = "Ctrl";
static const char *CB_LBL_CTRL_LOCK = "CTRL"; /* locked: shouting = latched */
static const char *CB_LBL_ALT = "Alt";
static const char *CB_LBL_ALT_LOCK = "ALT";

/* one row, so array index == button id */
/* "あ" is the IME toggle: it sends "\0ime", which an app with Japanese
   input turns into skk.enable() and every other app ignores. It is here
   rather than on a control byte because the classic SKK toggles are all
   unreachable on this keyboard — see the comment on KBD_K_IME. */
static const char *CB_MAP_MAIN[] = {
    "Esc", "Tab", "Ctrl", "Alt", "Fn",
    LV_SYMBOL_LEFT, LV_SYMBOL_DOWN, LV_SYMBOL_UP, LV_SYMBOL_RIGHT,
    LV_SYMBOL_COPY, LV_SYMBOL_PASTE, "\xe3\x81\x82" /* あ */, "",
};
#define CB_ID_CTRL 2
#define CB_ID_ALT  3
#define CB_ID_FN   4
static const kbd_key_t CB_KEY_MAIN[] = {
    KBD_K_ESC, KBD_K_TAB,
    KBD_K_NONE /* Ctrl */, KBD_K_NONE /* Alt */, KBD_K_NONE /* Fn */,
    KBD_K_LEFT, KBD_K_DOWN, KBD_K_UP, KBD_K_RIGHT,
    KBD_K_COPY, KBD_K_PASTE, KBD_K_IME,
};
static const char *CB_MAP_FN[] = {
    "Fn", "F1", "F2", "F3", "F4", "F5", "F6",
    "F7", "F8", "F9", "F10", "F11", "F12", "",
};
static const kbd_key_t CB_KEY_FN[] = {
    KBD_K_NONE /* Fn */, KBD_K_F1, KBD_K_F2, KBD_K_F3, KBD_K_F4,
    KBD_K_F5, KBD_K_F6, KBD_K_F7, KBD_K_F8, KBD_K_F9, KBD_K_F10,
    KBD_K_F11, KBD_K_F12,
};

/* ------------------------------------------------------------------ */
/* Overlay: a small window floated over the canvas, anchored to a point
 * the app supplies (docs/ui-overlay-plan.md).
 *
 * WHY THIS IS NOT DRAWN INTO THE CANVAS. The cells renderer's dirty
 * check is content-based, so anything drawn *over* an unchanged row
 * survives every later repaint. An app that floats its own window must
 * therefore remember the rows it covered and force them to repaint —
 * and forgetting is not a crash, it is a smear that accumulates
 * (skk_test.js shipped exactly that bug for its cursor). LVGL composites
 * over the canvas instead, so the problem cannot occur and no app needs
 * the bookkeeping.
 *
 * Second reason, specific to Japanese: these labels use ui_font()
 * (Noto JP, 3,517 kanji), NOT the terminal grid font, which has zero
 * CJK glyphs. An IME window is therefore readable today, before the
 * cells renderer learns double-width and the terminal font is
 * regenerated.
 *
 * Objects per handle are created on first use and then reused: text
 * changes every keystroke, so per-keystroke object churn is exactly what
 * we do not want. */
struct ui_overlay_t {
    lv_obj_t *box;                       /* container, positioned/clamped */
    lv_obj_t *lines;                     /* the preedit line(s) */
    lv_obj_t *row;                       /* item container (flex) */
    lv_obj_t *item[UI_OVERLAY_ITEMS];    /* candidate chips */
};
static ui_overlay_t s_ovl[UI_OVERLAY_MAX];

#define UI_OVL_PAD 6

static void ovl_build(ui_overlay_t *o)
{
    lv_obj_t *parent = s_root_scr ? s_root_scr : lv_screen_active();

    o->box = lv_obj_create(parent);
    lv_obj_remove_style_all(o->box);
    lv_obj_add_flag(o->box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(o->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o->box, LV_OBJ_FLAG_CLICKABLE); /* never steal touch */
    lv_obj_set_style_bg_opa(o->box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o->box, lv_color_hex(UI_COL_BAR), 0);
    lv_obj_set_style_radius(o->box, 4, 0);
    lv_obj_set_style_pad_all(o->box, UI_OVL_PAD, 0);
    lv_obj_set_style_pad_row(o->box, 2, 0);
    lv_obj_set_style_border_width(o->box, 1, 0);
    lv_obj_set_style_border_color(o->box, lv_color_hex(UI_COL_DOWN), 0);
    lv_obj_set_style_text_font(o->box, ui_font(), 0);
    lv_obj_set_size(o->box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o->box, LV_FLEX_FLOW_COLUMN);

    o->lines = lv_label_create(o->box);
    lv_obj_set_style_text_color(o->lines, lv_color_hex(UI_COL_EVENT), 0);
    lv_label_set_text(o->lines, "");

    o->row = lv_obj_create(o->box);
    lv_obj_remove_style_all(o->row);
    lv_obj_remove_flag(o->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o->row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(o->row, 10, 0);
    lv_obj_set_style_pad_row(o->row, 2, 0);
    for (int i = 0; i < UI_OVERLAY_ITEMS; i++) {
        o->item[i] = lv_label_create(o->row);
        lv_obj_set_style_text_color(o->item[i], lv_color_hex(UI_COL_TEXT), 0);
        lv_obj_set_style_pad_hor(o->item[i], 4, 0);
        lv_obj_set_style_radius(o->item[i], 3, 0);
        lv_obj_add_flag(o->item[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(o->item[i], "");
    }
}

static void ovl_hide(int id)
{
    if (id < 0 || id >= UI_OVERLAY_MAX || !s_ovl[id].box)
        return;
    lv_obj_add_flag(s_ovl[id].box, LV_OBJ_FLAG_HIDDEN);
}

/* UI task only. The public ui_tab5_overlay_hide_all() posts commands. */
static void ovl_hide_all(void)
{
    for (int i = 0; i < UI_OVERLAY_MAX; i++)
        ovl_hide(i);
}

/* Split `p` on `sep` into up to `max` NUL-terminated pieces, in place. */
static int ovl_split(char *p, char sep, char **out, int max)
{
    int n = 0;
    if (!p || !*p)
        return 0;
    out[n++] = p;
    for (; *p; p++) {
        if (*p != sep)
            continue;
        *p = '\0';
        if (n < max)
            out[n++] = p + 1;
    }
    return n;
}

extern "C" int ui_tab5_kb_reserved(int mode); /* defined below */

/* Position the box against the anchor.
 *
 * The anchor is in CANVAS coordinates (what the app draws in); the box is
 * a child of the screen, which starts UI_STATUSBAR_H higher.
 *
 * Rules, in the order they matter:
 *   - below the anchor by default, above it when below would run past the
 *     usable bottom — and "usable" subtracts the on-screen keyboard,
 *     otherwise the window a user is typing into hides under the keys;
 *   - keep the anchor's x if at all possible, because the entire point is
 *     that the eye does not move. Only clamp when the box would leave the
 *     screen;
 *   - never cover the status bar. */
static void ovl_place(ui_overlay_t *o, const ui_cmd_t &cmd)
{
    lv_obj_update_layout(o->box);
    int bw = lv_obj_get_width(o->box);
    int bh = lv_obj_get_height(o->box);

    int scr_w = s_canvas_w > 0 ? s_canvas_w : UI_LCD_H_RES;
    int top = UI_STATUSBAR_H;
    int bottom = top + s_canvas_h - ui_tab5_kb_reserved(s_app_kb_mode);

    int ax = cmd.x;
    int ay = cmd.y + top;
    int place = cmd.bg & 0x3;

    int y;
    if (place == 2) {
        y = ay - bh;
    } else {
        y = ay + cmd.h;                       /* below */
        if (place == 0 && y + bh > bottom)
            y = ay - bh;                      /* auto: flip above */
    }
    if (y + bh > bottom)
        y = bottom - bh;
    if (y < top)
        y = top;

    int x = ax;
    if (x + bw > scr_w)
        x = scr_w - bw;
    if (x < 0)
        x = 0;

    lv_obj_set_pos(o->box, x, y);
}

static void ovl_apply(const ui_cmd_t &cmd)
{
    int id = cmd.w;
    if (id < 0 || id >= UI_OVERLAY_MAX)
        return;
    if (!cmd.text) {
        ovl_hide(id);
        return;
    }
    ui_overlay_t *o = &s_ovl[id];
    if (!o->box)
        ovl_build(o);

    /* content: "line\1line\2item\1item" */
    char *body = cmd.text;
    char *items = strchr(body, '\2');
    if (items)
        *items++ = '\0';

    /* LVGL renders '\n' as a line break, so the line separator only has
       to become one. */
    for (char *q = body; *q; q++)
        if (*q == '\1')
            *q = '\n';
    lv_label_set_text(o->lines, body);
    if (*body)
        lv_obj_remove_flag(o->lines, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(o->lines, LV_OBJ_FLAG_HIDDEN);

    char *iv[UI_OVERLAY_ITEMS];
    int n = items ? ovl_split(items, '\1', iv, UI_OVERLAY_ITEMS) : 0;
    int sel = (int)cmd.color;
    bool vertical = (cmd.bg & 0x4) != 0;
    lv_obj_set_flex_flow(o->row, vertical ? LV_FLEX_FLOW_COLUMN
                                          : LV_FLEX_FLOW_ROW);
    for (int i = 0; i < UI_OVERLAY_ITEMS; i++) {
        if (i < n) {
            lv_label_set_text(o->item[i], iv[i]);
            bool on = (i == sel);
            lv_obj_set_style_bg_opa(o->item[i],
                                    on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(o->item[i],
                                      lv_color_hex(UI_COL_FLASH), 0);
            lv_obj_set_style_text_color(
                o->item[i], lv_color_hex(on ? 0xFFFFFF : UI_COL_TEXT), 0);
            lv_obj_remove_flag(o->item[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(o->item[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (n)
        lv_obj_remove_flag(o->row, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(o->row, LV_OBJ_FLAG_HIDDEN);

    /* Unhide BEFORE measuring: lv_obj_update_layout() does not lay out a
       hidden subtree, so measuring first returns 0 and the box lands at
       the wrong place (and, with LV_SIZE_CONTENT, can be shown at a size
       that was never computed). */
    lv_obj_remove_flag(o->box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(o->box);
    ovl_place(o, cmd);
}

/* Mirror the engine's Ctrl/Alt state on the buttons: CHECKED (amber)
   while in effect. Only writes one ctrl bit per button and invalidates
   those buttons, so unlike a set_map this is safe to call from inside
   an event callback — modifier feedback has to feel immediate.
   Deliberately NOT CHECKABLE: LVGL toggles a checkable button's
   CHECKED at RELEASED but fires VALUE_CHANGED at press time, so
   reading the flag from the event handler races the toggle (seen on
   device: the latch stuck yellow). The engine is the only state; these
   flags are write-only from here. Re-call after any set_map that
   changed the button count — that reallocates and zeroes the flags. */
static void cbar_apply_mods(void)
{
    if (!s_cbar || s_cbar_fn)
        return;
    struct {
        uint32_t id;
        bool on;
    } mods[] = {
        { CB_ID_CTRL, kbd_mod_armed(&s_ui_mods.ctrl) },
        { CB_ID_ALT, kbd_mod_armed(&s_ui_mods.alt) },
    };
    for (auto &m : mods) {
        if (m.on)
            lv_buttonmatrix_set_button_ctrl(s_cbar, m.id,
                                            LV_BUTTONMATRIX_CTRL_CHECKED);
        else
            lv_buttonmatrix_clear_button_ctrl(s_cbar, m.id,
                                              LV_BUTTONMATRIX_CTRL_CHECKED);
    }
}

/* Everything about the bar that needs a set_map: the Fn layer, and the
   Ctrl/Alt faces shouting when LOCKED so a latch is never mistaken for
   a one-shot. Deferred (see ui_kb_refresh) — the two maps differ in key
   count, so installing one from inside the bar's own event would free
   the arrays LVGL is still using for that press. */
static void cbar_apply_labels(void)
{
    if (!s_cbar)
        return;
    const char *ctrl_lbl =
        s_ui_mods.ctrl.lock ? CB_LBL_CTRL_LOCK : CB_LBL_CTRL;
    const char *alt_lbl = s_ui_mods.alt.lock ? CB_LBL_ALT_LOCK : CB_LBL_ALT;
    bool relabel = CB_MAP_MAIN[CB_ID_CTRL] != ctrl_lbl ||
                   CB_MAP_MAIN[CB_ID_ALT] != alt_lbl;
    CB_MAP_MAIN[CB_ID_CTRL] = ctrl_lbl;
    CB_MAP_MAIN[CB_ID_ALT] = alt_lbl;
    if (!relabel && s_cbar_map_fn == (int)s_cbar_fn)
        return;
    s_cbar_map_fn = s_cbar_fn;
    lv_buttonmatrix_set_map(s_cbar, s_cbar_fn ? CB_MAP_FN : CB_MAP_MAIN);
    cbar_apply_mods(); /* the key count changed: flags were zeroed */
}

/* Repainting a whole button matrix — every set_map — must NOT happen
   inside that widget's own event callback. It reallocates the button
   arrays LVGL is still using for the press in flight (the letter and
   symbol layers differ in key count), and the full-keyboard repaint
   delays the next touch sample enough to swallow a key: taps went
   missing while typing fast, which is why a double-tap could not be
   landed at all. Queued here and applied from the LVGL loop instead. */
static void ui_kb_refresh(void)
{
    lv_async_call(
        [](void *) {
            kb_apply_map();
            cbar_apply_labels();
            kb_lock_refresh();
        },
        nullptr);
}

/* app switch / UI reset: no modifier may survive into another app */
static void cbar_clear_mods(void)
{
    kbd_mods_reset(&s_ui_mods);
    cbar_apply_mods();
    ui_kb_refresh();
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
        lv_obj_set_size(s_cbar, ui_cur_hres(), UI_CB_H);
        lv_obj_set_style_pad_all(s_cbar, 4, 0);
        lv_obj_set_style_pad_gap(s_cbar, 4, 0);
        lv_obj_set_style_text_font(s_cbar, ui_font(), 0);
        /* dark system palette (the default light theme reads as a
           bright flash whenever the destroy-on-switch model re-shows
           the overlay — user-reported on app switches) */
        lv_obj_set_style_bg_color(s_cbar, lv_color_hex(UI_COL_BG), 0);
        lv_obj_set_style_border_width(s_cbar, 0, 0);
        lv_obj_set_style_radius(s_cbar, 0, LV_PART_MAIN); /* see s_kb */
        lv_obj_set_style_bg_color(s_cbar, lv_color_hex(UI_COL_BAR),
                                  LV_PART_ITEMS);
        lv_obj_set_style_text_color(s_cbar, lv_color_hex(UI_COL_TEXT),
                                    LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(s_cbar, 0, LV_PART_ITEMS); /* see s_kb */
        lv_obj_set_style_radius(s_cbar, 4, LV_PART_ITEMS);
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
        s_cbar_map_fn = 0;
        cbar_apply_mods();
        lv_obj_add_event_cb(
            s_cbar,
            [](lv_event_t *e) {
                lv_obj_t *bm = (lv_obj_t *)lv_event_get_current_target(e);
                uint32_t id = lv_buttonmatrix_get_selected_button(bm);
                if (id == LV_BUTTONMATRIX_BUTTON_NONE)
                    return;
                if (s_cbar_fn ? id == 0 : id == CB_ID_FN) {
                    /* Fn: flip the map, bar-local, nothing posted */
                    s_cbar_fn = !s_cbar_fn;
                    ui_kb_refresh(); /* the swap itself waits for the loop */
                    return;
                }
                if (!s_cbar_fn && (id == CB_ID_CTRL || id == CB_ID_ALT)) {
                    /* touch can't hold a key while typing another, so a
                       tap arms a one-shot and a double-tap locks */
                    kbd_mod_tap(id == CB_ID_CTRL ? &s_ui_mods.ctrl
                                                 : &s_ui_mods.alt,
                                true);
                    cbar_apply_mods();  /* highlight now... */
                    kb_lock_refresh();
                    ui_kb_refresh();    /* ...face swap off the event */
                    return;
                }
                const kbd_key_t *keys = s_cbar_fn ? CB_KEY_FN : CB_KEY_MAIN;
                size_t count = s_cbar_fn
                                   ? sizeof CB_KEY_FN / sizeof CB_KEY_FN[0]
                                   : sizeof CB_KEY_MAIN /
                                         sizeof CB_KEY_MAIN[0];
                if (id >= count || keys[id] == KBD_K_NONE)
                    return;
                char seq[KBD_SEQ_MAX];
                size_t n = kbd_translate(&s_ui_mods, 0, keys[id], seq,
                                         nullptr);
                if (n)
                    mqjs_post_key(seq, n);
                kbd_mods_mark_chord(&s_ui_mods);
                kbd_mods_consume(&s_ui_mods); /* one-shots are spent */
                cbar_apply_mods();
                kb_apply_shift_flag(); /* no map work: faces track locks */
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
    }
    /* above the on-screen keyboard normally; at the very bottom when
       the dock types for us and no keyboard will be raised */
    lv_obj_align(s_cbar, LV_ALIGN_BOTTOM_MID, 0, s_hw_kb ? 0 : -UI_KB_H);
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
    if (s_hw_kb) /* dock types directly; mode 2 keeps only the bar */
        return mode == 1 ? 0 : UI_CB_H;
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

/* Power-state overlay (docs/power-states.md): the power manager dims or
   blanks the backlight WITHOUT touching s_brightness, then restores the
   user's level on wake. Distinct from spanel_apply_brightness, which is
   the user's source-of-truth dial. Called from mqjs_power.c via extern. */
extern "C" void ui_tab5_backlight_apply(int percent) { backlight_set(percent); }
extern "C" int  ui_tab5_backlight_user(void) { return s_brightness; }

/* SCREEN_OFF input eater. The JS mqjs_post_touch path is already swallowed
   by the power manager, but LVGL dispatches touch to widgets independently,
   so a tap on the dark screen would still press a launcher button. Raise a
   fullscreen clickable object on the top layer (same web-modal backdrop
   trick as the camera viewfinder s_cam_scrim) so the wake tap lands on it,
   not on a widget. Transparent — invisible anyway with the backlight off.
   The indev still reports the raw press, so touch_observe -> wake detection
   is unaffected. Called from mqjs_power on js_task; takes the LVGL lock. */
static lv_obj_t *s_pwr_scrim;

extern "C" void ui_tab5_screen_scrim(bool on)
{
    lvgl_port_lock(0);
    if (on) {
        if (!s_pwr_scrim) {
            s_pwr_scrim = lv_obj_create(lv_layer_top());
            lv_obj_set_size(s_pwr_scrim, LV_PCT(100), LV_PCT(100));
            lv_obj_set_pos(s_pwr_scrim, 0, 0);
            lv_obj_set_style_radius(s_pwr_scrim, 0, 0);
            lv_obj_set_style_border_width(s_pwr_scrim, 0, 0);
            lv_obj_set_style_pad_all(s_pwr_scrim, 0, 0);
            lv_obj_set_style_bg_opa(s_pwr_scrim, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(s_pwr_scrim, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(s_pwr_scrim, LV_OBJ_FLAG_SCROLLABLE);
        }
        lv_obj_clear_flag(s_pwr_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_pwr_scrim);
    } else if (s_pwr_scrim) {
        lv_obj_add_flag(s_pwr_scrim, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

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
    lv_obj_set_size(s_spanel, ui_cur_hres(), h);
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
    if (s_kb_top)
        lv_obj_add_flag(s_kb_top, LV_OBJ_FLAG_HIDDEN);
    cbar_show(false);
    spanel_show(true);
}

/* ---- key layers ------------------------------------------------------
 * Touch-sized (10 keys a row), not a copy of the dock's 14-column
 * matrix: mirroring the dock would mean 51px keys in portrait. What IS
 * shared is the meaning — "Aa" is the dock's Shift with the same
 * tap/double-tap semantics, "sym" reaches the symbols the dock puts on
 * its sym layer, and both surfaces emit through kbd_translate.
 * Row shape is identical across layers (Enter, Backspace and the wide
 * space stay put) so muscle memory survives a layer flip.
 * ` ^ { } and < are dock-only: three rows of ten cannot hold every
 * symbol and these lost the vote to the shell's daily set. */
#define KB_LBL_SHIFT "Aa"
#define KB_LBL_SYM   "sym"
#define KB_LBL_ABC   "abc"
#define KB_SPACE     " " /* the widest key; an empty face is the hint */

/* One map per ROW per layer (see s_kb_row): a row matrix takes a map of
   its own row only, terminated by "" instead of "\n". Row 3 is the same
   object for lower and upper — no letters live there — which is what
   lets a case flip repaint three rows instead of four.
   Layers are indexed by the same 0/1/2 kb_apply_map computes. */
static const char *KB_R0_LOWER[] = { "q", "w", "e", "r", "t",
                                     "y", "u", "i", "o", "p", "" };
static const char *KB_R1_LOWER[] = { "a", "s", "d", "f", "g", "h",
                                     "j", "k", "l", LV_SYMBOL_NEW_LINE,
                                     "" };
static const char *KB_R2_LOWER[] = { KB_LBL_SHIFT, "z", "x", "c", "v",
                                     "b", "n", "m", ".",
                                     LV_SYMBOL_BACKSPACE, "" };
static const char *KB_R0_UPPER[] = { "Q", "W", "E", "R", "T",
                                     "Y", "U", "I", "O", "P", "" };
static const char *KB_R1_UPPER[] = { "A", "S", "D", "F", "G", "H",
                                     "J", "K", "L", LV_SYMBOL_NEW_LINE,
                                     "" };
static const char *KB_R2_UPPER[] = { KB_LBL_SHIFT, "Z", "X", "C", "V",
                                     "B", "N", "M", ".",
                                     LV_SYMBOL_BACKSPACE, "" };
/* shared by lower and upper */
static const char *KB_R3_ALPHA[] = { KB_LBL_SYM, ",", KB_SPACE,
                                     LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT,
                                     LV_SYMBOL_COPY, "" };
static const char *KB_R0_SYM[] = { "1", "2", "3", "4", "5",
                                   "6", "7", "8", "9", "0", "" };
static const char *KB_R1_SYM[] = { "-", "_", "=", "+", "/", "\\",
                                   "|", ":", ";", LV_SYMBOL_NEW_LINE,
                                   "" };
static const char *KB_R2_SYM[] = { "!", "?", "@", "#", "$", "%",
                                   "&", "*", "~", LV_SYMBOL_BACKSPACE,
                                   "" };
static const char *KB_R3_SYM[] = { KB_LBL_ABC, "\"", "'", KB_SPACE,
                                   ">", "[", "]", "" };

static const char *const *KB_LAYER[3][KB_ROWS] = {
    { KB_R0_LOWER, KB_R1_LOWER, KB_R2_LOWER, KB_R3_ALPHA }, /* 0 lower */
    { KB_R0_UPPER, KB_R1_UPPER, KB_R2_UPPER, KB_R3_ALPHA }, /* 1 upper */
    { KB_R0_SYM,   KB_R1_SYM,   KB_R2_SYM,   KB_R3_SYM   }, /* 2 sym   */
};
/* what each row is actually pointed at, so a layer change only touches
   the rows that differ (lower<->upper leaves row 3 alone), and where
   Shift sits in each — both survive a row that was not re-mapped */
static const char *const *s_kb_row_map[KB_ROWS];
static uint32_t s_kb_row_shift[KB_ROWS] = {
    LV_BUTTONMATRIX_BUTTON_NONE, LV_BUTTONMATRIX_BUTTON_NONE,
    LV_BUTTONMATRIX_BUTTON_NONE, LV_BUTTONMATRIX_BUTTON_NONE,
};

/* Shift shown on the Shift key alone: one ctrl bit and one button
   invalidated, measured at 3 ms — against 63 ms to swap the whole map
   (see kb_apply_map). Cheap enough to run inside an event callback,
   which is what makes a one-shot feel instant. */
static void kb_apply_shift_flag(void)
{
    if (s_kb_shift_row < 0 || !s_kb_row[s_kb_shift_row] ||
        s_kb_shift_id == LV_BUTTONMATRIX_BUTTON_NONE)
        return;
    lv_obj_t *m = s_kb_row[s_kb_shift_row];
    if (kbd_mod_armed(&s_ui_mods.shift))
        lv_buttonmatrix_set_button_ctrl(m, s_kb_shift_id,
                                        LV_BUTTONMATRIX_CTRL_CHECKED);
    else
        lv_buttonmatrix_clear_button_ctrl(m, s_kb_shift_id,
                                          LV_BUTTONMATRIX_CTRL_CHECKED);
}

/* Point the matrix at the layer the current state implies.
   Uppercase FACES are shown for a Shift LOCK only, never for a one-shot.
   Measured on this panel (single draw buffer, 25-line chunks, PPA
   rotate per flush): a map swap costs 63 ms — two touch samples' worth
   of blackout, since LVGL reads the panel from the same loop it renders
   in. Paying that on every one-shot put it right inside the gesture the
   user was still performing, which is what made a double-tap
   impossible; a lock is a deliberate, rare toggle and lands after the
   gesture is over. The one-shot still shows on the Shift key itself and
   in the strip's LOCK line.
   set_map wipes per-button widths and (when the key count changes) the
   ctrl flags, so both are restored here. */

/* Widths and no-repeat for one row, by label so a layer may place these
   keys wherever it likes. Also records where Shift landed, since the
   layers move it and the sym layer has none.
   Only ever called on a row that was just re-mapped: both
   lv_buttonmatrix_set_button_width (via update_map) and
   set_button_ctrl invalidate unconditionally, with no check that the
   value changed — calling this on an unchanged row would repaint it
   and give back exactly what the split is here to save. */
static void kb_row_apply_flags(int r)
{
    lv_obj_t *m = s_kb_row[r];
    s_kb_row_shift[r] = LV_BUTTONMATRIX_BUTTON_NONE;
    for (uint32_t id = 0;; id++) {
        const char *t = lv_buttonmatrix_get_button_text(m, id);
        if (!t)
            break;
        bool shift = !strcmp(t, KB_LBL_SHIFT);
        if (!strcmp(t, KB_SPACE))
            lv_buttonmatrix_set_button_width(m, id, 4);
        /* A held button repeats VALUE_CHANGED by default, which is what
           makes typematic work for letters and Backspace — but on a
           modifier or a layer key it would machine-gun taps and flip
           the lock on and off. */
        if (shift || !strcmp(t, KB_LBL_SYM) || !strcmp(t, KB_LBL_ABC) ||
            !strcmp(t, LV_SYMBOL_NEW_LINE) || !strcmp(t, LV_SYMBOL_COPY))
            lv_buttonmatrix_set_button_ctrl(m, id,
                                            LV_BUTTONMATRIX_CTRL_NO_REPEAT);
        if (shift)
            s_kb_row_shift[r] = id;
    }
}

static void kb_apply_map(void)
{
    if (!s_kb_row[0])
        return;
    bool upper = s_ui_mods.shift.lock;
    int want = s_kb_sym ? 2 : upper ? 1 : 0;
    if (want == s_kb_map_shown)
        return;
    s_kb_map_shown = want;

    /* Only the rows whose map actually changes are re-set, and only
       those repaint: a case flip leaves row 3 alone (16.1 -> 8.8 ms
       draw, measured on perf/kb-row-split-bench). An untouched row
       keeps its widths, flags and Shift id, which is why those are
       cached per row rather than recomputed for the whole keyboard. */
    for (int r = 0; r < KB_ROWS; r++) {
        const char *const *m = KB_LAYER[want][r];
        if (m == s_kb_row_map[r])
            continue;
        s_kb_row_map[r] = m;
        lv_buttonmatrix_set_map(s_kb_row[r], m);
        kb_row_apply_flags(r);
    }
    s_kb_shift_row = -1;
    s_kb_shift_id = LV_BUTTONMATRIX_BUTTON_NONE;
    for (int r = 0; r < KB_ROWS; r++)
        if (s_kb_row_shift[r] != LV_BUTTONMATRIX_BUTTON_NONE) {
            s_kb_shift_row = r;
            s_kb_shift_id = s_kb_row_shift[r];
        }
    /* set AND clear: swapping between two maps of the same key count
       keeps the old flags (LVGL only reallocates when the count
       changes), which used to leave Shift stuck amber */
    kb_apply_shift_flag();
}

/* Which modifiers are LATCHED, spelled out. A one-shot and a lock look
   the same on the keys themselves (both highlight), and the user cannot
   be expected to remember which tap did what — so name the locks. */
static void kb_lock_refresh(void)
{
    if (!s_kb_lock_lbl)
        return;
    char buf[48] = "";
    size_t n = 0;
    struct {
        bool on;
        const char *name;
    } locks[] = {
        { s_ui_mods.shift.lock, KB_LBL_SHIFT },
        { s_ui_mods.ctrl.lock, "Ctrl" },
        { s_ui_mods.alt.lock, "Alt" },
        { s_ui_mods.sym.lock, KB_LBL_SYM },
    };
    for (auto &l : locks) {
        if (!l.on)
            continue;
        if (!n)
            n += (size_t)snprintf(buf, sizeof buf, "LOCK");
        n += (size_t)snprintf(buf + n, sizeof buf - n, " %s", l.name);
    }
    lv_label_set_text(s_kb_lock_lbl, buf);
}

/* Clipboard preview on the paste button: seeing what you are about to
   paste is the point (the value can come from any app, even over MQTT).
   mqjs_clipboard_peek is the one clipboard entry point callable off the
   JS task, so this is safe from the LVGL task. */
static void kb_clip_refresh(void)
{
    if (!s_kb_clip_lbl)
        return;
    char type[32], data[192], line[72];
    if (mqjs_clipboard_peek(type, sizeof type, data, sizeof data) &&
        kbd_clip_format(data, line, sizeof line) > 0) {
        lv_label_set_text_fmt(s_kb_clip_lbl, LV_SYMBOL_PASTE "  %s", line);
        lv_obj_remove_state(s_kb_clip, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(s_kb_clip_lbl,
                          LV_SYMBOL_PASTE "  クリップボードは空");
        lv_obj_add_state(s_kb_clip, LV_STATE_DISABLED);
    }
}

/* The strip above the keys: tap-to-paste (with preview) + collapse. */
static void kb_top_create(void)
{
    int w = ui_cur_hres();
    s_kb_top = lv_obj_create(s_root_scr ? s_root_scr : lv_screen_active());
    lv_obj_remove_flag(s_kb_top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_kb_top, w, UI_KB_TOP_H);
    lv_obj_align(s_kb_top, LV_ALIGN_BOTTOM_MID, 0,
                 -(UI_KB_H - UI_KB_TOP_H));
    lv_obj_set_style_pad_all(s_kb_top, 2, 0);
    lv_obj_set_style_radius(s_kb_top, 0, 0);
    lv_obj_set_style_border_width(s_kb_top, 0, 0);
    lv_obj_set_style_bg_color(s_kb_top, lv_color_hex(UI_COL_BG), 0);

    s_kb_clip = lv_button_create(s_kb_top);
    lv_obj_set_size(s_kb_clip, w - UI_KB_TOP_H * 2 - UI_KB_LOCK_W - 12,
                    UI_KB_TOP_H - 4);
    lv_obj_align(s_kb_clip, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(s_kb_clip, 4, 0);
    lv_obj_set_style_bg_color(s_kb_clip, lv_color_hex(UI_COL_BAR), 0);
    lv_obj_set_style_bg_color(s_kb_clip, lv_color_hex(UI_COL_FLASH),
                              LV_STATE_PRESSED);
    s_kb_clip_lbl = lv_label_create(s_kb_clip);
    lv_label_set_long_mode(s_kb_clip_lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s_kb_clip_lbl, lv_pct(100));
    lv_obj_set_style_text_font(s_kb_clip_lbl, ui_font(), 0);
    lv_obj_set_style_text_color(s_kb_clip_lbl, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(s_kb_clip_lbl);
    lv_obj_add_event_cb(
        s_kb_clip,
        [](lv_event_t *) {
            /* paste stays a token: only the app knows whether to bracket
               it (DECSET 2004) or to confirm a multi-line payload */
            char seq[KBD_SEQ_MAX];
            size_t n = kbd_translate(&s_ui_mods, 0, KBD_K_PASTE, seq,
                                     nullptr);
            if (n)
                mqjs_post_key(seq, n);
            kbd_mods_consume(&s_ui_mods);
            cbar_apply_mods();
            kb_apply_shift_flag();
        },
        LV_EVENT_CLICKED, nullptr);

    /* latched modifiers, named in words (see kb_lock_refresh) */
    s_kb_lock_lbl = lv_label_create(s_kb_top);
    lv_obj_set_width(s_kb_lock_lbl, UI_KB_LOCK_W);
    lv_obj_align(s_kb_lock_lbl, LV_ALIGN_RIGHT_MID,
                 -(UI_KB_TOP_H * 2 + 8), 0);
    lv_label_set_long_mode(s_kb_lock_lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_kb_lock_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_kb_lock_lbl, ui_font(), 0);
    lv_obj_set_style_text_color(s_kb_lock_lbl, lv_color_hex(0xFFD479), 0);
    kb_lock_refresh();

    lv_obj_t *close = lv_button_create(s_kb_top);
    lv_obj_set_size(close, UI_KB_TOP_H * 2, UI_KB_TOP_H - 4);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(close, 4, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(UI_COL_BAR), 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(UI_COL_FLASH),
                              LV_STATE_PRESSED);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_color(cl, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(
        close, [](lv_event_t *) { kb_collapse(); }, LV_EVENT_CLICKED,
        nullptr);

    kb_clip_refresh();
    if (!s_kb_clip_tmr)
        s_kb_clip_tmr = lv_timer_create(
            [](lv_timer_t *) {
                if (s_kb_top && !lv_obj_has_flag(s_kb_top,
                                                 LV_OBJ_FLAG_HIDDEN))
                    kb_clip_refresh();
            },
            1000, nullptr);
}

/* mode: 0 = hide (OFF: stats panel gone too), 1 = keyboard,
   2 = keyboard + terminal control bar. Showing always lifts the
   keyboard over a collapsed panel (T3c SHOWN state).
   Keyboard dock present (s_hw_kb): the app's request is honored in
   spirit without any app change — mode 1 raises nothing (the dock
   types directly), mode 2 keeps only the slim control bar, whose
   F-keys / copy / paste have no physical equivalent on the dock.
   ui_tab5_kb_reserved returns matching heights, so ui.keyboard()'s
   synchronous return already sizes the app's grid correctly. */
static void kb_show(int mode)
{
    s_app_kb_mode = mode > 0 ? mode : 0; /* re-applied on dock change */
    cbar_show(mode >= 2);
    spanel_show(false);
    if (mode <= 0 || s_hw_kb) {
        if (s_kb)
            lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        if (s_kb_top)
            lv_obj_add_flag(s_kb_top, LV_OBJ_FLAG_HIDDEN);
        if (mode > 0)
            s_kb_mode = mode; /* dock removed later: ⌨ restores this */
        return;
    }
    s_kb_mode = mode; /* what the panel's ⌨ button restores */
    if (s_kb) {
        lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_kb);
        if (s_kb_top) {
            lv_obj_remove_flag(s_kb_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_kb_top);
            kb_clip_refresh(); /* it may have changed while hidden */
        }
        return;
    }
    s_kb_map_shown = -1; /* fresh matrix: kb_apply_map must really run */
    kb_top_create();
    /* The container. It owns the background for the whole key area —
       the row matrices only paint from the first key down, so the 4px
       above row 0 would otherwise show the console through. */
    s_kb = lv_obj_create(s_root_scr ? s_root_scr : lv_screen_active());
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_kb, ui_cur_hres(), KB_AREA_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(s_kb, 0, 0);
    /* dark system palette — the default light theme made every
       keyboard (re)appearance a bright blue-white flash */
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_border_width(s_kb, 0, 0);
    /* The theme's card radius (10px at this DPI) was never cleared
       here, and a rounded background is not a COVER — so LVGL could not
       skip the console under any chunk the keyboard's corners touch,
       and drew both. The corners sit on UI_COL_BG either way. */
    lv_obj_set_style_radius(s_kb, 0, 0);

    for (int r = 0; r < KB_ROWS; r++) {
        lv_obj_t *m = lv_buttonmatrix_create(s_kb);
        s_kb_row[r] = m;
        lv_obj_set_size(m, ui_cur_hres(), KB_ROW_PITCH);
        /* one pitch tall, carrying the inter-row gap as its own bottom
           padding, so the keys land where the 4-row matrix put them */
        lv_obj_align(m, LV_ALIGN_BOTTOM_MID, 0,
                     -(KB_AREA_H - KB_PAD - (r + 1) * KB_ROW_PITCH));
        lv_obj_set_style_pad_hor(m, KB_PAD, 0);
        lv_obj_set_style_pad_top(m, 0, 0);
        lv_obj_set_style_pad_bottom(m, KB_PAD, 0);
        lv_obj_set_style_pad_gap(m, KB_PAD, 0);
        lv_obj_set_style_text_font(m, ui_font(), 0);
        lv_obj_set_style_bg_color(m, lv_color_hex(UI_COL_BG), 0);
        lv_obj_set_style_border_width(m, 0, 0);
        lv_obj_set_style_radius(m, 0, 0); /* cover, as above */
        lv_obj_set_style_bg_color(m, lv_color_hex(UI_COL_BAR),
                                  LV_PART_ITEMS);
        lv_obj_set_style_text_color(m, lv_color_hex(UI_COL_TEXT),
                                    LV_PART_ITEMS);
        /* The default theme gives every button a BLURRED DROP SHADOW,
           and LV_DRAW_SW_SHADOW_CACHE_SIZE is 0 — so all 36 keys
           re-blurred theirs on every repaint. lv_keyboard's own theme
           strips them (lv_theme_default.c keyboard_button_bg); a raw
           button matrix keeps them. Invisible on this dark palette, so
           pure cost. */
        lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
        /* The theme's ~13px radius sends every key fill through the
           masked rounded-rect path; 4px keeps the keys visibly rounded
           and measured 15% off the repaint (draw 18.4 -> 15.7 ms). */
        lv_obj_set_style_radius(m, 4, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(m, lv_color_hex(UI_COL_FLASH),
                                  (uint32_t)LV_PART_ITEMS |
                                      (uint32_t)LV_STATE_PRESSED);
        /* Shift in effect: same amber as the bar's latch and the
           terminal's badge, so one visual language across every
           surface */
        lv_obj_set_style_bg_color(m, lv_color_hex(0xFFD479),
                                  (uint32_t)LV_PART_ITEMS |
                                      (uint32_t)LV_STATE_CHECKED);
        lv_obj_set_style_text_color(m, lv_color_black(),
                                    (uint32_t)LV_PART_ITEMS |
                                        (uint32_t)LV_STATE_CHECKED);
        s_kb_row_map[r] = KB_LAYER[0][r];
        lv_buttonmatrix_set_map(m, s_kb_row_map[r]);
        kb_row_apply_flags(r);
    }
    /* One callback for all four rows: it already resolves everything
       from the event's own target and the button's LABEL, never from a
       matrix-wide id, so splitting the matrix costs it nothing. */
    lv_event_cb_t key_cb =
        [](lv_event_t *e) {
            lv_obj_t *bm = (lv_obj_t *)lv_event_get_current_target(e);
            uint32_t id = lv_buttonmatrix_get_selected_button(bm);
            if (id == LV_BUTTONMATRIX_BUTTON_NONE)
                return;
            const char *txt = lv_buttonmatrix_get_button_text(bm, id);
            if (!txt)
                return;

            /* keys that only change the keyboard: nothing is posted */
            if (!strcmp(txt, KB_LBL_SYM) || !strcmp(txt, KB_LBL_ABC)) {
                s_kb_sym = !strcmp(txt, KB_LBL_SYM);
                ui_kb_refresh();
                return;
            }
            if (!strcmp(txt, KB_LBL_SHIFT)) {
                kbd_mod_tap(&s_ui_mods.shift, true);
                kb_apply_shift_flag(); /* 3 ms: the tap answers at once */
                kb_lock_refresh();
                ui_kb_refresh(); /* a lock also swaps the faces, later */
                return;
            }

            kbd_key_t key = KBD_K_NONE;
            char base = 0;
            if (!strcmp(txt, LV_SYMBOL_BACKSPACE))
                key = KBD_K_BS;
            else if (!strcmp(txt, LV_SYMBOL_NEW_LINE))
                key = KBD_K_ENTER;
            else if (!strcmp(txt, LV_SYMBOL_LEFT))
                key = KBD_K_LEFT;
            else if (!strcmp(txt, LV_SYMBOL_RIGHT))
                key = KBD_K_RIGHT;
            else if (!strcmp(txt, LV_SYMBOL_COPY))
                key = KBD_K_COPY;
            else if (!(txt[0] & 0x80) && !txt[1])
                base = txt[0]; /* a plain character face */
            else
                return; /* an icon we have no key for: post nothing */

            char seq[KBD_SEQ_MAX];
            size_t n = kbd_translate(&s_ui_mods, base, key, seq, nullptr);
            if (n)
                mqjs_post_key(seq, n);
            kbd_mods_mark_chord(&s_ui_mods); /* breaks pending tap chains */
            kbd_mods_consume(&s_ui_mods);
            /* both cheap, and the spent one-shots must stop showing now */
            kb_apply_shift_flag();
            cbar_apply_mods();
        };
    for (int r = 0; r < KB_ROWS; r++)
        lv_obj_add_event_cb(s_kb_row[r], key_cb, LV_EVENT_VALUE_CHANGED,
                            nullptr);
    kb_apply_map(); /* widths, and whatever state survived an app switch */
}

/* Temporary instrumentation: what does each kind of keyboard repaint
   actually cost on this panel? (single draw buffer, 25-line chunks,
   PPA rotate per flush). Times the render+flush by forcing it. */
/* Flip to 1 to re-measure after touching the keyboard or the display
   pipeline. It also logs where each repaint goes (draw / flush_cb /
   DMA wait). PORTRAIT, -O2, shadows off: a keyboard map swap is 20 ms
   of which draw is 18.4 (92%), flush_cb 0.7, DMA wait 1.1 — this
   pipeline is CPU-rasterisation-bound, NOT transfer-bound, so buffer
   tricks (double buffering, zero-copy) can only ever touch that last
   ~9%. The 18.4 ms itself: fills 9.2, the theme's 13px corner radius
   3.9, the 36 text labels 5.4.
   Tried and dropped: LV_OBJ_STYLE_CACHE moved a keyboard repaint by
   ~1% (15.75 -> 15.57 ms draw), i.e. nothing — per-chunk style
   resolution is not where the time goes.
   Where the rest goes: a 1-chunk repaint of ONE key costs ~1.0 ms
   while a 1-chunk repaint of the strip label costs ~0.6 ms, and the
   only difference is that LVGL walks the matrix's 36 buttons to build
   draw tasks for every chunk. That puts the per-chunk walk near
   0.4 ms — about 6 ms of a 15-chunk map swap.
   FIXED by the row split (see s_kb_row): one matrix per row lets
   LVGL's own per-object intersect reject the rows a chunk misses.
   Device A/B against main: map swap draw 17.14 -> 9.96 ms, one key
   1.09 -> 0.56, and a real case flip (three rows re-mapped) 7.94.
   The per-key ITEMS radius of 4 costs 1.24 ms of that 9.96 (radius 0
   measures 8.66) and the 36 labels 3.13 ms (r0-no-text measures 5.54).
   The other half of that 6 ms — halving the chunk count with a 50-line
   draw buffer — is still on the table but costs 36KB internal, since
   sw_rotate has esp_lvgl_port allocate a PPA scratch of the same size
   at init whether or not the display is ever rotated. In portrait that
   scratch is dead weight (landscape means the dock, and the dock
   suppresses this keyboard), but freeing it needs a fork of the port:
   the handle is private and there is no runtime API. Worth ~1 ms now
   that the walk is gone, so it waits for a measurement.
   Older numbers, same steps (ms: one key | map swap | screen):
     -Og portrait,  shadows      3 | 63 |  99
     -O2 landscape, shadows      3 | 81 | 125   (1.8x the pixels, + PPA
                                                 rotate, so per pixel
                                                 -O2 is ~40% faster on
                                                 button/text content;
                                                 flat fills are memory
                                                 bound and unchanged)
     -O2 landscape, no shadows   1 | 47 |  85   <- shipped
     ... + PSRAM draw buffers    3 | 85 | 160   (frees 72KB internal,
                                                 costs ~2x render)
     ... + double buffer (int)   1 | 40 |  73   (costs 36KB internal)
   The flush path — draw buffer count and size — sets the floor; the
   compiler and the styles set the slope. */
#ifndef UI_KB_BENCH
#define UI_KB_BENCH 0
#endif
#if UI_KB_BENCH
/* Where a repaint actually goes. LVGL brackets a refresh with
   RENDER_START/READY and each flush with FLUSH_START/FINISH plus
   FLUSH_WAIT_START/FINISH (lv_refr.c:755,823,1415,1423,1433,1446), so
   total - flush_cb - wait = the CPU pixel work. flush_cb includes the
   PPA rotation in landscape; wait is time blocked on the previous
   chunk's DMA, which is exactly what a second draw buffer would hide. */
static struct {
    int64_t t_render, t_flush, t_wait;
    int64_t render, flush, wait;
    int flushes;
} s_prof;

static void prof_event(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_RENDER_START:      s_prof.t_render = now; break;
    case LV_EVENT_RENDER_READY:      s_prof.render += now - s_prof.t_render; break;
    case LV_EVENT_FLUSH_START:       s_prof.t_flush = now; break;
    case LV_EVENT_FLUSH_FINISH:      s_prof.flush += now - s_prof.t_flush;
                                     s_prof.flushes++; break;
    case LV_EVENT_FLUSH_WAIT_START:  s_prof.t_wait = now; break;
    case LV_EVENT_FLUSH_WAIT_FINISH: s_prof.wait += now - s_prof.t_wait; break;
    default: break;
    }
}

static void kb_bench(void)
{
    int64_t t0;
    static const lv_event_code_t prof_codes[] = {
        LV_EVENT_RENDER_START,     LV_EVENT_RENDER_READY,
        LV_EVENT_FLUSH_START,      LV_EVENT_FLUSH_FINISH,
        LV_EVENT_FLUSH_WAIT_START, LV_EVENT_FLUSH_WAIT_FINISH,
    };
    for (auto code : prof_codes)
        lv_display_add_event_cb(s_disp, prof_event, code, nullptr);

#define KB_BENCH_STEP(label, body)                                        \
    do {                                                                  \
        memset(&s_prof, 0, sizeof s_prof);                                \
        t0 = esp_timer_get_time();                                        \
        body;                                                             \
        lv_refr_now(s_disp);                                              \
        ESP_LOGW(TAG,                                                     \
                 "bench %-22s %5lld ms | draw %5lld + flush %5lld + wait " \
                 "%5lld us over %d chunks",                               \
                 label, (esp_timer_get_time() - t0) / 1000,               \
                 s_prof.render - s_prof.flush - s_prof.wait, s_prof.flush, \
                 s_prof.wait, s_prof.flushes);                            \
    } while (0)

    /* the dock suppresses the on-screen keyboard, so lift that policy for
       the measurement (and put it back at the end) */
    bool had_dock = s_hw_kb;
    s_hw_kb = false;
    ESP_LOGW(TAG, "bench: %s, canvas %dx%d",
             s_landscape ? "landscape" : "portrait", s_canvas_w, s_canvas_h);
    KB_BENCH_STEP("kb create+first draw", kb_show(2));
    if (!s_kb_row[0]) {
        ESP_LOGW(TAG, "bench: no matrix (canvas_w=%d) — skipped", s_canvas_w);
        s_hw_kb = had_dock;
        return;
    }
    /* Re-map rows 0..rows_n-1 to a layer whether or not they changed —
       kb_apply_map deliberately skips unchanged rows, which is the
       thing being measured, so the bench has to force the work. */
    auto set_layer = [](int layer, int rows_n) {
        s_kb_map_shown = -1;
        for (int r = 0; r < rows_n; r++) {
            s_kb_row_map[r] = KB_LAYER[layer][r];
            lv_buttonmatrix_set_map(s_kb_row[r], s_kb_row_map[r]);
            kb_row_apply_flags(r);
        }
    };
    auto all_rows_style = [](void (*f)(lv_obj_t *)) {
        for (int r = 0; r < KB_ROWS; r++)
            f(s_kb_row[r]);
    };
    KB_BENCH_STEP("one key invalidate",
                  lv_buttonmatrix_set_button_ctrl(
                      s_kb_row[0], 0, LV_BUTTONMATRIX_CTRL_CHECKED));
    KB_BENCH_STEP("one key invalidate #2",
                  lv_buttonmatrix_clear_button_ctrl(
                      s_kb_row[0], 0, LV_BUTTONMATRIX_CTRL_CHECKED));
    /* the real case flip: row 3 has no letters, so it is not re-mapped
       and does not repaint */
    KB_BENCH_STEP("map swap (case, 3 rows)", set_layer(1, 3));
    KB_BENCH_STEP("map swap (all 4 rows)", set_layer(0, KB_ROWS));
    KB_BENCH_STEP("map swap (sym, realloc)", set_layer(2, KB_ROWS));
    /* What the per-key styling costs. NB the steps after these include a
       keyboard repaint, since restoring the styles invalidates it. */
    KB_BENCH_STEP("map swap (radius 4)", {
        all_rows_style([](lv_obj_t *m) {
            lv_obj_set_style_radius(m, 4, LV_PART_ITEMS);
        });
        set_layer(0, KB_ROWS);
    });
    KB_BENCH_STEP("map swap (radius 0)", {
        all_rows_style([](lv_obj_t *m) {
            lv_obj_set_style_radius(m, 0, LV_PART_ITEMS);
        });
        set_layer(1, KB_ROWS);
    });
    KB_BENCH_STEP("map swap (r0, no text)", {
        all_rows_style([](lv_obj_t *m) {
            lv_obj_set_style_text_opa(m, LV_OPA_TRANSP, LV_PART_ITEMS);
        });
        set_layer(0, KB_ROWS);
    });
    all_rows_style([](lv_obj_t *m) {
        lv_obj_set_style_text_opa(m, LV_OPA_COVER, LV_PART_ITEMS);
        lv_obj_set_style_radius(m, 4, LV_PART_ITEMS); /* back to shipped */
    });
    KB_BENCH_STEP("strip label", kb_lock_refresh());
    KB_BENCH_STEP("clip label", kb_clip_refresh());
    KB_BENCH_STEP("whole screen", lv_obj_invalidate(lv_screen_active()));
    ESP_LOGW(TAG, "bench indev read period %d ms, refr %d ms",
             (int)LV_DEF_REFR_PERIOD, (int)LV_DEF_REFR_PERIOD);
    s_kb_map_shown = -1;
    kb_show(0);
    s_hw_kb = had_dock;
    kb_show(s_app_kb_mode); /* back to whatever the app asked for */
#undef KB_BENCH_STEP
}
#endif

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
        /* rotation rebinds the same allocation with swapped dims (boot
           is always portrait, whose w*h is the larger of the two) */
        s_js_canvas = _canvas;
        s_js_canvas_buf = _buf;
        s_js_canvas_bytes = bytes;
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
            if (cmd.op == UI_CMD_OVERLAY) {
                /* not a canvas op: composited above, so it must not
                   unhide the canvas or count as "drew" */
                ovl_apply(cmd);
                free(cmd.text);
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
                ovl_hide_all();    /* a dead app's float must not survive */
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
       the cell, clipped to its cell box — one cell wide, two for a
       width-2 codepoint (no lv_draw_label, no layer).
       `run_right` is the run's right edge in pixels: the CPU path has to
       honour it for the same reason the PPA path clamps to the A8 buffer
       (see compose_glyph_a8). Without it the two paths disagree for a
       width-2 codepoint that ends a run — PPA shears the glyph, the CPU
       paints its right half into the next column, which fill_rect never
       cleared, so half a kanji survives until that row is redrawn. Same
       ui.cells call, two different pictures depending only on the run's
       length (UI_PPA_CELLS_MIN_CELLS). */
    void blit_glyph(int cx0, int cy0, uint32_t cp, uint16_t fg, int run_right)
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
        int clipR = cx0 + cells_glyph_cols(cp) * UI_CELL_W;
        if (clipR > run_right) {
            clipR = run_right;
            cells_note_clip();
        }
        int clipB = cy0 + UI_CELL_H;
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
        int clipR = cx0 + cells_glyph_cols(cp) * UI_CELL_W;
        /* Clamp to the A8 buffer, i.e. to the run's right edge. This is
           NOT just a last line of defence: cells()'s straddle guard only
           runs inside the `while (n > seg_max)` splitting loop, so every
           run of <= seg_max cells (which is nearly every run ssh_vt's
           drawRow emits) and the final segment of a long one can still
           END on a width-2 codepoint — a selection dragged to the left
           half of a full-width character does exactly that. The CPU path
           clamps to the same edge (blit_glyph's run_right) so the two
           paths agree; without that they diverge on run length alone. */
        if (clipR > dst_w) {
            clipR = dst_w;
            cells_note_clip();
        }
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
       and any PPA failure use the per-glyph CPU blit. Runs wider than
       the A8 compose buffer (80 cells = the portrait width; landscape
       rows are 142) are split into segments so the PPA path keeps
       winning instead of falling back to the CPU wholesale. */
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
           PPA threshold. Columns == codepoints (the CONT contract at the
           top of this file), so counting UTF-8 lead bytes gives the run's
           width. Deliberately NOT sum(ui_cell_width): the caller already
           spent a column on the filler cell, and counting the wide
           glyph's second column here as well would overshoot the fill by
           one cell per CJK character and eat the head of the next run. */
        int n = 0;
        for (const uint8_t *p = s; *p; p++)
            if ((*p & 0xC0) != 0x80)
                n++;
        fill_rect(col * UI_CELL_W, row * UI_CELL_H, n * UI_CELL_W,
                  UI_CELL_H, bg);

        const int seg_max = (int)(sizeof(s_cells_a8) /
                                  ((size_t)UI_CELL_W * UI_CELL_H));
        while (n > seg_max) {
            /* Fill a segment, but never let one END on a width-2 glyph:
               its right half belongs to the next segment's first cell,
               which is outside the A8 buffer, so compose_glyph_a8 would
               shear it — and only on the PPA path, since the CPU blit
               writes straight to the canvas and would not. Hand the
               straddling glyph to the next segment instead. */
            const uint8_t *p = s;
            int take = 0;
            while (take < seg_max) {
                const uint8_t *q = p;
                if (take + cells_glyph_cols(cells_utf8_next(p)) > seg_max) {
                    p = q; /* rewind over the straddling glyph */
                    break;
                }
                take++; /* one column per codepoint, wide or not */
            }
            if (take == 0) { /* unreachable at seg_max=80; no stuck loop */
                cells_utf8_next(p);
                take = 1;
            }
            cells_run(col, row, s, p, take, fg);
            s = p;
            col += take;
            n -= take;
        }
        cells_run(col, row, s, nullptr, n, fg);
    }

    /* One ≤80-cell segment: PPA compose+blend when it qualifies, else
       per-glyph CPU blit. `e` bounds the UTF-8 walk (nullptr = NUL). */
    void cells_run(int col, int row, const uint8_t *s, const uint8_t *e,
                   int n, uint16_t fg)
    {
        int x0 = col * UI_CELL_W, y0 = row * UI_CELL_H, bw = n * UI_CELL_W;

        if (s_ppa_blend && n >= UI_PPA_CELLS_MIN_CELLS && x0 >= 0 &&
            y0 >= 0 && x0 + bw <= s_canvas_w &&
            y0 + UI_CELL_H <= s_canvas_h &&
            (size_t)bw * UI_CELL_H <= sizeof(s_cells_a8)) {
            memset(s_cells_a8, 0, (size_t)bw * UI_CELL_H);
            const uint8_t *p = s;
            for (int c = 0; *p && (!e || p < e); c++)
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
        int run_right = (col + n) * UI_CELL_W;
        while (*s && (!e || s < e)) {
            uint32_t cp = cells_utf8_next(s);
            blit_glyph(c * UI_CELL_W, row * UI_CELL_H, cp, fg, run_right);
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

/* ------------------------------------------------------------------ */
/* Landscape rotation (keyboard dock). Callable from any task; takes   */
/* the LVGL port lock. Content is PPA-rotated per flush by             */
/* esp_lvgl_port (sw_rotate), touch is rotated by LVGL itself          */
/* (lv_indev applies the display rotation to pointer coords).          */
/* ------------------------------------------------------------------ */

extern "C" bool ui_tab5_landscape(void)
{
    return s_landscape;
}

/* Keyboard dock present: stop raising the on-screen keyboard (mode 1
   shows nothing, mode 2 keeps only the control bar) and re-apply the
   foreground app's last requested mode under the new policy. Kept
   separate from ui_tab5_set_landscape on purpose — orientation and
   input policy are coupled by the dock today, not by the UI. */
extern "C" void ui_tab5_set_hw_keyboard(bool present)
{
    if (s_hw_kb == present)
        return;
    s_hw_kb = present;
    if (!s_disp || !s_canvas_w)
        return;
    lvgl_port_lock(0);
    kb_show(s_app_kb_mode);
    lvgl_port_unlock();
}

extern "C" void ui_tab5_set_landscape(bool on)
{
    if (!s_disp || !s_canvas_w || s_landscape == on)
        return;
    lvgl_port_lock(0);
    s_landscape = on;
    int hres = on ? UI_LCD_V_RES : UI_LCD_H_RES;
    int vres = on ? UI_LCD_H_RES : UI_LCD_V_RES;

    lv_display_set_rotation(s_disp, on ? UI_ROT_LANDSCAPE
                                       : LV_DISPLAY_ROTATION_0);

    /* the few fixed-size chrome widgets (everything else is pct/flex
       and follows the resolution change on its own) */
    if (s_sb_bar)
        lv_obj_set_size(s_sb_bar, hres, UI_STATUSBAR_H);
    if (s_sb_row)
        lv_obj_set_width(s_sb_row, hres);
    if (s_sb_event)
        lv_obj_set_width(s_sb_event, hres - 2 * UI_PAD);
    if (s_console_panel)
        lv_obj_set_size(s_console_panel, hres, vres - UI_STATUSBAR_H);

    /* lazily built overlays are sized at creation: drop them and
       re-apply the app's keyboard mode below, which rebuilds them at
       the new width under the current dock policy */
    int app_kb = s_app_kb_mode;
    if (s_kb) {
        lv_obj_delete(s_kb); /* takes the four row matrices with it */
        s_kb = nullptr;
        for (int r = 0; r < KB_ROWS; r++) {
            s_kb_row[r] = nullptr;
            s_kb_row_map[r] = nullptr; /* the rebuild must really set_map */
            s_kb_row_shift[r] = LV_BUTTONMATRIX_BUTTON_NONE;
        }
        s_kb_shift_row = -1;
        s_kb_shift_id = LV_BUTTONMATRIX_BUTTON_NONE;
        s_kb_map_shown = -1;
    }
    if (s_kb_clip_tmr) {
        lv_timer_delete(s_kb_clip_tmr);
        s_kb_clip_tmr = nullptr;
    }
    if (s_kb_top) {
        lv_obj_delete(s_kb_top); /* takes the paste + collapse children */
        s_kb_top = nullptr;
        s_kb_clip = s_kb_clip_lbl = s_kb_lock_lbl = nullptr;
    }
    if (s_cbar) {
        lv_obj_delete(s_cbar);
        s_cbar = nullptr;
        s_cbar_fn = false;
        s_cbar_map_fn = -1;
    }
    kbd_mods_reset(&s_ui_mods); /* nothing may be held across a rebuild */
    if (s_sp_timer) {
        lv_timer_delete(s_sp_timer);
        s_sp_timer = nullptr;
    }
    if (s_spanel) {
        lv_obj_delete(s_spanel);
        s_spanel = nullptr;
        s_sp_stats = s_sp_clip = s_sp_pct = nullptr;
        s_sp_arc = nullptr;
    }

    /* JS canvas: rebind the same allocation with swapped dims (boot is
       always portrait, whose w*h is the larger of the two) and drop to
       the console until the app redraws — same hygiene as UI_CMD_RESET */
    s_canvas_w = hres;
    s_canvas_h = vres - UI_STATUSBAR_H;
    if (s_js_canvas && s_js_canvas_buf &&
        (size_t)s_canvas_w * s_canvas_h * 2 <= s_js_canvas_bytes) {
        lv_canvas_set_buffer(s_js_canvas, s_js_canvas_buf, s_canvas_w,
                             s_canvas_h, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_js_canvas, 0, UI_STATUSBAR_H);
        lv_obj_add_flag(s_js_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    /* rebuild the overlays the app had up, at the new width and under
       the (possibly changed) dock policy */
    kb_show(app_kb);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "rotation: %s (canvas %dx%d)",
             on ? "landscape" : "portrait", s_canvas_w, s_canvas_h);

    /* tell the foreground app its ui.size() changed (same token channel
       as the control bar; apps that don't care just ignore it) */
    mqjs_post_key("\x00rotate", 7);
}

extern "C" void ui_tab5_start(ui_tab5_ready_cb_t ready_cb, void *arg)
{
    /* the data plane must exist before app_main registers the print
       sink, and must stay usable even if the panel init below fails
       (ui_tab5_log/set_status/cmd no-op while these are NULL) */
    /* allocate the scrollback in PSRAM; gate the mutex on success so the
       NULL-mtx guards in the writer/reader also cover a failed allocation. */
    s_log = (ui_log_line_t *)heap_caps_calloc(UI_LOG_LINES, sizeof(ui_log_line_t),
                                              MALLOC_CAP_SPIRAM);
    s_log_mtx = s_log ? xSemaphoreCreateMutex() : NULL;
    s_status_mtx = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(UI_CMD_QUEUE_DEPTH, sizeof(ui_cmd_t));
    s_job_queue = xQueueCreate(UI_JOB_QUEUE_DEPTH, sizeof(ui_job_t));

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
    /* MEASURED 2026-07-28: double_buffer=true overlaps render with the
       DMA flush and is worth ~15% on big repaints (keyboard map swap
       47 -> 40 ms, whole screen 85 -> 73 ms) for another 36KB of
       internal DMA — largest contiguous internal block drops 80KB ->
       46KB at network-up. Not taken: the camera and esp-hosted need
       that headroom more than the keyboard needs 7 ms, now that the
       repaint is off the typing path entirely. */
    disp_cfg.double_buffer = false;
    disp_cfg.hres = UI_LCD_H_RES;
    disp_cfg.vres = UI_LCD_V_RES;
    disp_cfg.monochrome = false;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_dma = true;
    /* MEASURED 2026-07-28: PSRAM draw buffers (flags.buff_spiram, legal
       here since P4 PSRAM is DMA-capable) free the ~72KB of internal DMA
       these two buffers hold — and cost roughly 2x the render time
       (keyboard map swap 47 -> 85 ms, whole screen 85 -> 160 ms). The
       buffers stay internal. */
    /* keyboard-dock landscape: PPA rotates each flush (needs
       CONFIG_LVGL_PORT_ENABLE_PPA; without it the port falls back to a
       CPU rotate through draw_buffs[2] — same memory, more CPU) */
    disp_cfg.flags.sw_rotate = true;

    lvgl_port_display_dsi_cfg_t dsi_cfg = {};
    dsi_cfg.flags.avoid_tearing = false;

    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed");
        return;
    }
    s_disp = disp; /* ui_tab5_set_landscape rotates this display */

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
            /* after the canvas consumed its commands, so a term's blit
               lands in the same frame it was parsed in */
            ui_run_frame_work();
        },
        16, nullptr);
    lvgl_port_unlock();

    touch_init(variant, disp);

    backlight_set(100);
    ESP_LOGI(TAG, "UI up (%dx%d)", UI_LCD_H_RES, UI_LCD_V_RES);

#if UI_KB_BENCH
    {
        lv_timer_t *t = lv_timer_create(
            [](lv_timer_t *) { kb_bench(); }, 6000, nullptr);
        lv_timer_set_repeat_count(t, 1);
    }
#endif
    if (ready_cb)
        ready_cb(arg);

    /* note: no em-dash etc. here — U+2014 is in neither font (tofu) */
    static const char greet[] = "mqjs コンソール: JS の print がここに流れます";
    ui_tab5_log(greet, sizeof greet - 1);
}

/* ---- camera viewfinder overlay (cam_tab5) ----
   An lv_canvas on the top layer whose RGB565 buffer the cam_scan task
   writes final-size frames into. Created once and kept (960KB PSRAM
   for 600x800); hidden between scans. All entry points take the LVGL
   port lock — they are called from the cam_scan task. */
static lv_obj_t *s_cam_cv;
static uint16_t *s_cam_cv_buf;
static lv_obj_t *s_cam_lbl;
static lv_obj_t *s_cam_scrim;
static int s_cam_cv_h;
static void (*s_cam_dismiss)(void);

void ui_tab5_cam_set_dismiss_cb(void (*cb)(void))
{
    s_cam_dismiss = cb;
}

/* tap on the backdrop (= outside viewfinder + readout, which are
   CLICKABLE and absorb their own taps) — web-modal dismiss. Runs on
   the LVGL task; the registered cb only flips the scan's cancel flag */
static void cam_scrim_clicked(lv_event_t *e)
{
    (void)e;
    if (s_cam_dismiss)
        s_cam_dismiss();
}

void *ui_tab5_cam_canvas(int w, int h)
{
    if (!s_root_scr)
        return NULL;
    lvgl_port_lock(0);
    if (!s_cam_scrim) {
        /* full-screen translucent backdrop UNDER the canvas: blocks
           every LVGL widget behind it (clicks land here, not below) */
        s_cam_scrim = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_cam_scrim, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(s_cam_scrim, 0, 0);
        lv_obj_set_style_radius(s_cam_scrim, 0, 0);
        lv_obj_set_style_border_width(s_cam_scrim, 0, 0);
        lv_obj_set_style_pad_all(s_cam_scrim, 0, 0);
        lv_obj_set_style_bg_color(s_cam_scrim, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_cam_scrim, LV_OPA_50, 0);
        lv_obj_add_flag(s_cam_scrim, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_cam_scrim, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_cam_scrim, cam_scrim_clicked,
                            LV_EVENT_CLICKED, NULL);
    }
    if (!s_cam_cv) {
        /* PPA writes this buffer (cam_tab5 hardware-rotates frames into
           it): DMA-capable + cache-line aligned */
        s_cam_cv_buf = (uint16_t *)heap_caps_aligned_alloc(
            64, (size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (s_cam_cv_buf) {
            lv_canvas_set_buffer(s_cam_cv = lv_canvas_create(lv_layer_top()),
                                 s_cam_cv_buf, w, h, LV_COLOR_FORMAT_RGB565);
            /* 1:1 presentation: the PPA already produced the final
               on-screen size (cam_tab5 PV_SCALE). The earlier
               lv_image_set_scale(512) 2x path made LVGL bilinear-
               resample the whole window per frame on the CPU — that
               WAS the sluggish viewfinder (UserDemo pattern: hardware
               makes the pixels, LVGL only blends them). */
            lv_obj_align(s_cam_cv, LV_ALIGN_TOP_LEFT,
                         (UI_LCD_H_RES - w) / 2, UI_STATUSBAR_H + 24);
            lv_obj_set_style_border_width(s_cam_cv, 2, 0);
            lv_obj_set_style_border_color(s_cam_cv, lv_color_hex(0x2ECC71), 0);
            /* modal content: absorb taps (don't dismiss, don't pass) */
            lv_obj_add_flag(s_cam_cv, LV_OBJ_FLAG_CLICKABLE);
            s_cam_cv_h = h;
        }
    }
    if (s_cam_cv) {
        lv_obj_clear_flag(s_cam_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_cam_cv, LV_OBJ_FLAG_HIDDEN);
        /* keep modal stack on top of whatever joined the layer since */
        lv_obj_move_foreground(s_cam_scrim);
        lv_obj_move_foreground(s_cam_cv);
        if (s_cam_lbl)
            lv_obj_move_foreground(s_cam_lbl);
        s_cam_modal = true;
    }
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
    s_cam_modal = false;
    lv_obj_add_flag(s_cam_cv, LV_OBJ_FLAG_HIDDEN);
    if (s_cam_scrim)
        lv_obj_add_flag(s_cam_scrim, LV_OBJ_FLAG_HIDDEN);
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
        /* modal content like the canvas: absorb taps, don't dismiss */
        lv_obj_add_flag(s_cam_lbl, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_label_set_text(s_cam_lbl, utf8);
    lv_obj_clear_flag(s_cam_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_cam_lbl);
    lvgl_port_unlock();
}

#endif /* CONFIG_MQJS_TAB5_UI */
