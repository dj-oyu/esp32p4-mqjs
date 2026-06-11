/*
 * User JS class ids + widget-kind constants shared between
 * device_stdlib.c (the ROM-stdlib generator: class defs reference these
 * ids) and mqjs_runtime.c (the method implementations check them).
 *
 * The UIW_K_* values mirror ui_widget_kind_t in ui_tab5.h — that header
 * is ESP-only, while this one must also compile in the host generator
 * and the PC test build. Keep the two lists in sync.
 */
#pragma once

#define JS_CLASS_UI_SCREEN (JS_CLASS_USER + 0)
#define JS_CLASS_UI_WIDGET (JS_CLASS_USER + 1)
/* total class count; sizes the generated js_c_finalizer_table */
#define JS_CLASS_COUNT     (JS_CLASS_USER + 2)

/* == ui_widget_kind_t (ui_tab5.h) */
#define UIW_K_BUTTON 0
#define UIW_K_LABEL  1
#define UIW_K_FIELD  2
#define UIW_K_LIST   3
#define UIW_K_ITEM   4
#define UIW_K_TOGGLE 5
#define UIW_K_SLIDER 6
/* JS-side only: marks the opaque of a UiScreen object */
#define UIW_K_SCREEN 0xFF
