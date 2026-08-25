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
/* Battery for the status bar. `pct` -1 = unknown/no pack, `state` is
   pwr_batt_state_t (components/pwr_tab5/include/pwr_tab5.h), `eta_min` -1 =
   unknown. Called at 1 Hz from the battery task, so main/app_main filters
   out the samples that would not change anything on screen. */
void ui_status_set_battery(int pct, int state, int eta_min);
