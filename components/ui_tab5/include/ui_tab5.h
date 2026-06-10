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

#if CONFIG_MQJS_TAB5_UI

/* Initialize panel + LVGL and start the UI task (call once, early). */
void ui_tab5_start(void);
/* Append one UTF-8 console line (thread-safe, copies, never blocks). */
void ui_tab5_log(const char *line, size_t n);
/* Publish a new status snapshot (thread-safe, copies). */
void ui_tab5_set_status(const ui_status_t *st);

#else /* stubs: UI disabled (Stamp-P4 and default builds) */

static inline void ui_tab5_start(void) {}
static inline void ui_tab5_log(const char *line, size_t n)
{
    (void)line;
    (void)n;
}
static inline void ui_tab5_set_status(const ui_status_t *st) { (void)st; }

#endif /* CONFIG_MQJS_TAB5_UI */

#ifdef __cplusplus
}
#endif
