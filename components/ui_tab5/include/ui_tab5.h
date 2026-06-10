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
} ui_cmd_op_t;

typedef struct {
    uint8_t op;     /* ui_cmd_op_t */
    int16_t x, y;
    int16_t w, h;   /* RECT: size; LINE: end point; others: unused */
    uint32_t color; /* 0xRRGGBB */
    char *text;     /* TEXT only: heap copy. Consumed (freed) by the UI
                       on success; stays owned by the caller when
                       ui_tab5_cmd() returns false. */
} ui_cmd_t;

#if CONFIG_MQJS_TAB5_UI

/* Initialize panel + LVGL and start the UI task (call once, early). */
void ui_tab5_start(void);
/* Append one UTF-8 console line (thread-safe, copies, never blocks). */
void ui_tab5_log(const char *line, size_t n);
/* Publish a new status snapshot (thread-safe, copies). */
void ui_tab5_set_status(const ui_status_t *st);
/* Post a drawing command (thread-safe, non-blocking). Returns false
 * when the queue is full or the UI is absent — the command is dropped
 * (a drop counter shows up in the status bar). */
bool ui_tab5_cmd(const ui_cmd_t *cmd);
/* Logical canvas resolution; 0x0 when the UI is off or init failed. */
void ui_tab5_canvas_size(int *w, int *h);

#else /* stubs: UI disabled (Stamp-P4 and default builds) */

static inline void ui_tab5_start(void) {}
static inline void ui_tab5_log(const char *line, size_t n)
{
    (void)line;
    (void)n;
}
static inline void ui_tab5_set_status(const ui_status_t *st) { (void)st; }
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

#endif /* CONFIG_MQJS_TAB5_UI */

#ifdef __cplusplus
}
#endif
