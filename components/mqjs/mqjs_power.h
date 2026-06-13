/*
 * Device power-state machine (docs/power-states.md), P0 screen slice.
 *
 * Drives the screen-power axis only: ACTIVE -> DIMMED -> SCREEN_OFF on
 * inactivity, back to ACTIVE on touch. App suspend/resume (Phase 5) and
 * OFF / deep-sleep are NOT here yet. Device-only — every caller in the
 * runtime is behind ESP_PLATFORM, so this never links into the PC runner.
 *
 * Threading: mqjs_power_update / _init run on the js_task (runtime loop).
 * mqjs_power_note_input runs on the LVGL/touch task. They share only
 * word-sized flags; see mqjs_power.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* js_task: latch ACTIVE and start the idle clock at now_ms. */
void mqjs_power_init(int64_t now_ms);

/* js_task: advance the state machine. Call every runtime-loop tick. */
void mqjs_power_update(int64_t now_ms);

/* touch task: note user input (resets the idle clock on the next tick).
   Returns true when the event should be SWALLOWED — i.e. the screen was
   off and this gesture only wakes it (M5 behavior). `kind`: 0=down,
   1=move, 2=up; the whole wake gesture is eaten through its up. */
bool mqjs_power_note_input(int kind);

#ifdef __cplusplus
}
#endif
