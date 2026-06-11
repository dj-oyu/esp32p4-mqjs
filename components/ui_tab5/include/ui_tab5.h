/*
 * Public C API of the Tab5 on-device UI. Everything behind it is C++
 * (LVGL + mooncake + smooth_ui_toolkit) but callers stay plain C.
 *
 * With CONFIG_MQJS_TAB5_UI=n every entry point is a no-op inline stub,
 * so main/ never needs #ifdefs and Stamp builds carry zero UI code
 * (same trick as board_tab5.h).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of platform state shown in the status bar. Writers
 * (wifi.c / task_source.c / app_main.c) call ui_tab5_set_status() on
 * change; the UI task reads a mutex-guarded copy every frame. */
typedef struct {
    char task_name[32];   /* "task" / "mqtt-task" ... */
    char task_origin[16]; /* embedded / mqtt / persisted */
    char ip[16];
    bool wifi_up;
    bool mqtt_up;
    char last_event[48];  /* "accepted (3644B)" / "bad signature" ... */
} ui_status_t;

/* Drawing command posted by the JS ui.* bindings (js_task) and consumed
 * by the CanvasApp (LVGL task). Coordinates are in the canvas' logical
 * resolution (the area below the status bar, see ui_tab5_canvas_size). */
typedef enum {
    UI_CMD_CLEAR = 0, /* fill whole canvas with color */
    UI_CMD_FILL,      /* same as CLEAR (kept for API symmetry) */
    UI_CMD_RECT,      /* filled rectangle x,y,w,h */
    UI_CMD_LINE,      /* line from x,y to w,h (endpoint, not size) */
    UI_CMD_TEXT,      /* UTF-8 text at x,y */
    UI_CMD_PIXEL,     /* single pixel at x,y */
    UI_CMD_KEYBOARD,  /* show (x!=0) / hide (x==0) the on-screen keyboard */
    UI_CMD_CELLS,     /* monospace run: text at cell (x=col, y=row), color=fg,
                         bg=bg. Drawn with the terminal grid font (ui.cells). */
    UI_CMD_SCROLL,    /* scroll cell-rows [x=top, y=bot] by w lines
                         (w>0 up, w<0 down); vacated rows filled with color */
    UI_CMD_RESET,     /* foreground-app switch: clear + hide the canvas and
                         hide the keyboard (same hygiene as a task switch) */
} ui_cmd_op_t;

typedef struct {
    uint8_t op;     /* ui_cmd_op_t */
    int16_t x, y;
    int16_t w, h;   /* RECT: size; LINE: end point; CELLS: unused;
                       SCROLL: w=lines (signed); others: unused */
    uint32_t color; /* 0xRRGGBB (CELLS: fg; SCROLL: fill) */
    uint32_t bg;    /* CELLS: background 0xRRGGBB; others: unused */
    char *text;     /* TEXT/CELLS only: heap copy. Consumed (freed) by the UI
                       on success; stays owned by the caller when
                       ui_tab5_cmd() returns false. */
} ui_cmd_t;

/* ------------------------------------------------------------------ */
/* W1-2/3 widget layer (docs/widget-framework-design.md).              */
/* All functions are called on js_task and run synchronously under the */
/* esp_lvgl_port lock ("LVGL ロック下", design §5). Handles are        */
/* slot|generation packed uint32 (0 = invalid/stale); a destroyed      */
/* screen bumps the generation of every entry it owned, so stale JS    */
/* handles turn into silent no-ops instead of dangling pointers.       */
/* ------------------------------------------------------------------ */

/* Widget kinds for ui_tab5_w_create(). Values are mirrored in
 * components/mqjs/mqjs_classes.h (the runtime can't include this header
 * in PC builds) — keep both lists in sync. */
typedef enum {
    UI_WK_BUTTON = 0,
    UI_WK_LABEL  = 1,
    UI_WK_FIELD  = 2, /* labelled one-line textarea; a!=0 -> password   */
    UI_WK_LIST   = 3,
    UI_WK_ITEM   = 4, /* list row; parent must be a UI_WK_LIST handle   */
    UI_WK_TOGGLE = 5, /* labelled switch; a!=0 -> initially on          */
    UI_WK_SLIDER = 6, /* a=min b=max c=initial value                    */
} ui_widget_kind_t;

#if CONFIG_MQJS_TAB5_UI

/* Initialize panel + LVGL and start the UI task (call once, early). */
void ui_tab5_start(void);
/* Append one UTF-8 console line (thread-safe, copies, never blocks). */
void ui_tab5_log(const char *line, size_t n);
/* Publish a new status snapshot (thread-safe, copies). */
void ui_tab5_set_status(const ui_status_t *st);
/* P4b: current/previous foreground app for the status-bar chip
 * (thread-safe, copies). `prev` may name a stopped app — the chip shows
 * it dimmed and a tap relaunches it via the launcher. Empty strings
 * clear the respective display. */
void ui_tab5_set_fg_apps(const char *cur, const char *prev,
                         bool prev_running);
/* Post a drawing command (thread-safe, non-blocking). Returns false
 * when the queue is full or the UI is absent — the command is dropped
 * (a drop counter shows up in the status bar). */
bool ui_tab5_cmd(const ui_cmd_t *cmd);
/* Logical canvas resolution; 0x0 when the UI is off or init failed. */
void ui_tab5_canvas_size(int *w, int *h);
/* Pixel size of a UTF-8 string in the canvas font (no wrapping; \n makes
 * it multi-line). 0x0 when the UI is off or init failed. Safe from any
 * task: only reads const font tables. */
void ui_tab5_text_size(const char *utf8, int *w, int *h);
/* Cell size (advance width, line height) of the monospace terminal font
 * used by ui.cells/UI_CMD_CELLS. 0x0 when the UI is off. Const tables only. */
void ui_tab5_cell_size(int *w, int *h);

/* Create a widget screen (flex column + title), retain the previously
 * active screen on the navigation stack and slide the new one in.
 * Returns the screen handle (0 if the display is down). When pushing
 * exceeded the retain depth (UI_NAV_RETAIN), the deepest retained screen
 * was destroyed and its handle is stored in *evicted (else 0) so the JS
 * runtime can release that screen's callbacks in one sweep (design §4④). */
uint32_t ui_tab5_w_screen(const char *title, uint32_t *evicted);

/* Pop: destroy the active widget screen (deleted after the slide-back
 * animation) and re-show the retained one below it — zero rebuild, zero
 * churn (design §4②). Returns the destroyed screen's handle so the JS
 * runtime can sweep its callbacks; 0 when the console screen is active. */
uint32_t ui_tab5_w_back(void);

/* Create one widget on a screen (or list, for UI_WK_ITEM). `text` is the
 * label/title (may be ""); a/b/c are kind-specific (see ui_widget_kind_t).
 * Returns the widget handle, 0 on failure/stale parent. */
uint32_t ui_tab5_w_create(int kind, uint32_t parent, const char *text,
                          int a, int b, int c);

/* Add a trailing action button to a UI_WK_ITEM list row (P4b/P4c: the
 * launcher stops apps / uninstalls inline instead of via a confirm
 * page). `icon`: 0 = close (✕, stop), 1 = trash (uninstall) — both
 * FontAwesome glyphs from the Montserrat fallback font. Returns the
 * button's own widget handle (its tap posts EV_WIDGET like any
 * button), 0 for stale/non-item handles. Tapping it does not trigger
 * the row's own callback. */
uint32_t ui_tab5_w_item_close(uint32_t item, int icon);

/* Replace the visible text of a LABEL / BUTTON / ITEM (child label) or
 * the content of a FIELD. false for stale handles / other kinds. */
bool ui_tab5_w_set_text(uint32_t handle, const char *text);

/* Current value of a FIELD (UTF-8 into buf, true on success) ... */
bool ui_tab5_w_value_str(uint32_t handle, char *buf, size_t cap);
/* ... or of a TOGGLE (0/1) / SLIDER (int). 0 for stale handles. */
int ui_tab5_w_value_int(uint32_t handle);

/* Destroy every widget screen and return to the console screen. Called
 * by the JS runtime when a task ends (same role as the canvas clear on
 * task switch). Safe to call when nothing was ever created. */
void ui_tab5_w_reset(void);

/* Start the slide-in animation of the most recent ui_tab5_w_screen()
 * (P4a, design §3.4): screens are created WITHOUT loading so the page
 * can be fully built first; the JS runtime calls this at the end of
 * each event dispatch. No-op when nothing is queued. */
void ui_tab5_w_commit(void);

/* Free bytes in the LVGL heap (builtin tlsf pool; 0 with CLIB malloc or
 * when the UI is down). Third element of sys.heap() — the W1-4 thrash
 * metric lives inside the preallocated pool, invisible to heap_caps. */
size_t ui_tab5_lv_mem_free(void);

#else /* stubs: UI disabled (Stamp-P4 and default builds) */

static inline void ui_tab5_start(void) {}
static inline void ui_tab5_log(const char *line, size_t n)
{
    (void)line;
    (void)n;
}
static inline void ui_tab5_set_status(const ui_status_t *st) { (void)st; }
static inline void ui_tab5_set_fg_apps(const char *cur, const char *prev,
                                       bool prev_running)
{
    (void)cur;
    (void)prev;
    (void)prev_running;
}
static inline bool ui_tab5_cmd(const ui_cmd_t *cmd)
{
    (void)cmd;
    return false;
}
static inline void ui_tab5_canvas_size(int *w, int *h)
{
    *w = 0;
    *h = 0;
}
static inline void ui_tab5_text_size(const char *utf8, int *w, int *h)
{
    (void)utf8;
    *w = 0;
    *h = 0;
}
static inline void ui_tab5_cell_size(int *w, int *h)
{
    *w = 0;
    *h = 0;
}
static inline uint32_t ui_tab5_w_screen(const char *title, uint32_t *evicted)
{
    (void)title;
    *evicted = 0;
    return 0;
}
static inline uint32_t ui_tab5_w_back(void) { return 0; }
static inline uint32_t ui_tab5_w_create(int kind, uint32_t parent,
                                        const char *text, int a, int b, int c)
{
    (void)kind;
    (void)parent;
    (void)text;
    (void)a;
    (void)b;
    (void)c;
    return 0;
}
static inline uint32_t ui_tab5_w_item_close(uint32_t item, int icon)
{
    (void)item;
    (void)icon;
    return 0;
}
static inline bool ui_tab5_w_set_text(uint32_t handle, const char *text)
{
    (void)handle;
    (void)text;
    return false;
}
static inline bool ui_tab5_w_value_str(uint32_t handle, char *buf, size_t cap)
{
    (void)handle;
    if (cap)
        buf[0] = '\0';
    return false;
}
static inline int ui_tab5_w_value_int(uint32_t handle)
{
    (void)handle;
    return 0;
}
static inline void ui_tab5_w_reset(void) {}
static inline void ui_tab5_w_commit(void) {}
static inline size_t ui_tab5_lv_mem_free(void) { return 0; }

#endif /* CONFIG_MQJS_TAB5_UI */

#ifdef __cplusplus
}
#endif
