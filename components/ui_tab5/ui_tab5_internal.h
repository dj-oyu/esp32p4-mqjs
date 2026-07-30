/*
 * Internals shared between ui_tab5.cpp (display/console/canvas) and
 * ui_widgets.cpp (W1 widget layer). Not part of the public C API.
 */
#pragma once

#include "lvgl.h"

/* Status bar height. The bar lives on lv_layer_top() (above every
 * screen), so both the console/canvas area and W1 widget pages must
 * start below this line. */
#define UI_STATUSBAR_H 88

/* Noto JP 20px with Montserrat fallback (defined in ui_tab5.cpp). */
const lv_font_t *ui_tab5_jp_font(void);

/* ---- widget FIELD on the platform keyboard (I3) -------------------
 * docs/keyboard-ime-unification.md §5. The field used to raise a stock
 * lv_keyboard of its own; it now borrows the one keyboard/control bar
 * ui_tab5.cpp owns, so the dock, the modifier LEDs and the IME all
 * reach it. Both directions are LVGL-task-only (or under its lock).
 */

/* ui_widgets.cpp -> ui_tab5.cpp: a FIELD took or lost focus.
   `mode` is a ui_field_mode_t, or -1 for "no field focused". Raises the
   platform keyboard + control bar (restoring the app's own ui.keyboard
   mode on -1) and decides whether the bar's 「あ」 key is live. */
void ui_tab5_kb_field(int mode);

/* ui_tab5.cpp -> ui_widgets.cpp: apply one keystroke to the focused
   textarea, off the UI command queue. */
void ui_tab5_field_key_apply(const char *utf8, size_t len);
