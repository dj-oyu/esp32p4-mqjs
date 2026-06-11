/*
 * Internals shared between ui_tab5.cpp (display/console/canvas) and
 * ui_widgets.cpp (W1 widget layer). Not part of the public C API.
 */
#pragma once

#include "lvgl.h"

/* Noto JP 20px with Montserrat fallback (defined in ui_tab5.cpp). */
const lv_font_t *ui_tab5_jp_font(void);
