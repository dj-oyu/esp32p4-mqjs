#pragma once

/*
 * Single writer-side aggregation point for the Tab5 status bar: keeps
 * the one ui_status_t snapshot and pushes it to the UI on each change.
 * All functions are thread-safe and no-ops when the UI is disabled.
 */
#include <stdbool.h>

void ui_status_set_net(bool wifi_up, const char *ip);   /* ip NULL = "" */
void ui_status_set_mqtt(bool up);
void ui_status_set_task(const char *name, const char *origin);
void ui_status_set_event(const char *event);            /* last_event */
