/*
 * Tab5 widget layer (W1-2/W1-3, docs/widget-framework-design.md).
 *
 * LVGL-widget counterpart to the canvas pipeline in ui_tab5.cpp: the JS
 * runtime calls the ui_tab5_w_* functions on js_task and every one of
 * them runs synchronously under the esp_lvgl_port lock — that *is* the
 * "LVGL ロック下" rule of design §5 (the canvas keeps its lock-free
 * command queue because it is the hot path; widget work is forms and
 * settings pages, so a short lock per call is the simpler safe shape).
 * Events go the other way through the existing MqjsEvent queue:
 * LVGL task -> mqjs_post_widget() -> EV_WIDGET -> JS callback.
 *
 * Handles are (generation << 8 | slot). Destroying a screen bumps the
 * generation of every table entry it owned, so a JS object that outlives
 * its screen degenerates to a no-op instead of a dangling lv_obj_t*.
 *
 * Navigation (design §4②): the active screen is pushed onto a retain
 * stack when ui.screen() opens a new one — hidden, not destroyed — and
 * ui.back() re-shows it without rebuilding. Destruction happens in
 * exactly two places: back() (the screen being left is deleted after
 * the slide-back animation, via lv_screen_load_anim auto_del) and
 * retain-depth overflow (the deepest retained screen is destroyed and
 * its handle reported to the JS runtime so callbacks are released in
 * one sweep, design §4④). UI_NAV_RETAIN is the tunable N=3.
 *
 * NOTE on smooth_ui_toolkit lvgl_cpp (design §9): the wrappers are RAII
 * (each Widget deletes its own lv_obj in the destructor), which fights
 * the arena-style "free the whole tree with one lv_obj_del(root)" plan
 * of §4④ — so this table stores raw lv_obj_t* and uses the LVGL C API
 * directly. Screen transitions use lv_screen_load_anim, which is what
 * Screen::loadAnim wraps; nothing else of lvgl_cpp was load-bearing.
 */
#include "sdkconfig.h"
#if CONFIG_MQJS_TAB5_UI

#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "ui_tab5.h"
#include "ui_tab5_internal.h"

/* JS runtime entry point (extern decl instead of REQUIRES: mqjs already
   depends on this component — same trick as mqjs_post_touch). */
extern "C" void mqjs_post_widget(uint32_t handle, int32_t value);

static const char *TAG = "ui_w";

/* tunables (design §4②/§7: 定数化して調整可能に) */
#define UI_NAV_RETAIN 3   /* hidden screens kept alive behind the active one */
#define UI_W_MAX      160 /* widget table size (screens + widgets + rows)    */
#define UI_NAV_ANIM_MS 200

/* palette shared with the console side (ui_tab5.cpp) */
#define UI_COL_BG     0x0B0E11
#define UI_COL_PANEL  0x1A222C
#define UI_COL_TEXT   0xC9D1D9
#define UI_COL_DIM    0x8B98A5
#define UI_COL_ACCENT 0x4FC3F7 /* focused-field border */
#define UI_COL_PRESS  0x2E6BD6 /* pressed list row background */

typedef struct {
    lv_obj_t *obj;     /* screen root / button / textarea / switch / ... */
    uint16_t gen;      /* survives slot reuse; part of the handle */
    uint8_t kind;      /* ui_widget_kind_t, or UI_WK_SCREEN_SLOT */
    uint8_t scr_slot;  /* owning screen's table slot (screens: own slot) */
    bool used;
} ui_w_entry_t;

#define UI_WK_SCREEN_SLOT 0xFE /* table-internal kind for screen roots */
#define UI_SLOT_ROOT      0xFF /* retain-stack sentinel: console screen */

static ui_w_entry_t s_w[UI_W_MAX];
static uint8_t s_stack[UI_NAV_RETAIN + 4]; /* root + retained + headroom */
static int s_sp;
static int s_cur = -1;       /* slot of the ACTIVE widget screen, -1 = console.
                                Tracked here instead of lv_screen_active():
                                during a lv_screen_load_anim the display still
                                reports the OLD screen until the animation
                                lands, so back-to-back ui.back() calls (the
                                unwind idiom) raced and stopped after one pop
                                (user-reported W2 bug). */
static lv_obj_t *s_root;     /* console screen (bottom of every stack) */
static lv_obj_t *s_field_kb; /* shared lv_keyboard for FIELD textareas */
static int s_pending_load = -1; /* screen awaiting its slide-in: created by
                                   ui_tab5_w_screen but loaded only by
                                   ui_tab5_w_commit at end-of-dispatch, so
                                   children are built BEFORE the animation
                                   starts (P4a §3.4: no pop-in, no lock
                                   contention with the slide). */

static inline uint32_t w_handle(int slot)
{
    return ((uint32_t)s_w[slot].gen << 8) | (uint32_t)slot;
}

/* handle -> slot, -1 when stale/invalid */
static int w_lookup(uint32_t handle)
{
    int slot = (int)(handle & 0xFF);
    if (slot >= UI_W_MAX || !s_w[slot].used)
        return -1;
    if (s_w[slot].gen != (uint16_t)(handle >> 8))
        return -1;
    return slot;
}

static int w_alloc(uint8_t kind, uint8_t scr_slot, lv_obj_t *obj)
{
    for (int i = 0; i < UI_W_MAX && i < 256; i++) {
        if (s_w[i].used)
            continue;
        s_w[i].used = true;
        s_w[i].obj = obj;
        s_w[i].kind = kind;
        s_w[i].scr_slot = scr_slot;
        if (++s_w[i].gen == 0)
            s_w[i].gen = 1; /* gen 0 would make handle 0 == "invalid" */
        return i;
    }
    ESP_LOGW(TAG, "widget table full (%d)", UI_W_MAX);
    return -1;
}

/* Invalidate every table entry owned by screen `slot` (the screen entry
   itself included): one generation bump kills all JS handles at once.
   Purely bookkeeping — the lv tree is freed separately (lv_obj_del or
   the load-anim auto_del), one recursion for the whole tree (§4④). */
static void w_orphan_screen(int slot)
{
    if (s_field_kb && s_w[slot].obj &&
        lv_obj_get_screen(s_field_kb) == s_w[slot].obj)
        s_field_kb = NULL; /* dies with the tree */
    for (int i = 0; i < UI_W_MAX; i++) {
        if (s_w[i].used && s_w[i].scr_slot == slot) {
            s_w[i].used = false;
            s_w[i].obj = NULL;
        }
    }
}

/* ---- events: LVGL task -> JS ------------------------------------- */

static void w_event_cb(lv_event_t *e)
{
    uint32_t handle = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *t = (lv_obj_t *)lv_event_get_target(e);
    int32_t value = 0;
    int slot = (int)(handle & 0xFF);
    if (slot < UI_W_MAX && s_w[slot].used) {
        if (s_w[slot].kind == UI_WK_TOGGLE)
            value = lv_obj_has_state(t, LV_STATE_CHECKED) ? 1 : 0;
        else if (s_w[slot].kind == UI_WK_SLIDER)
            value = lv_slider_get_value(t);
    }
    mqjs_post_widget(handle, value);
}

/* ---- FIELD on-screen keyboard (LVGL task only) -------------------- */

static void field_kb_hide(lv_event_t *e)
{
    (void)e;
    if (s_field_kb)
        lv_obj_add_flag(s_field_kb, LV_OBJ_FLAG_HIDDEN);
}

static void field_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_obj_t *scr = lv_obj_get_screen(ta);
        if (!s_field_kb || lv_obj_get_screen(s_field_kb) != scr) {
            if (s_field_kb)
                lv_obj_delete(s_field_kb);
            s_field_kb = lv_keyboard_create(scr);
            lv_obj_set_size(s_field_kb, LV_PCT(100), 400);
            lv_obj_align(s_field_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
            /* OK / close on the keyboard hides it again */
            lv_obj_add_event_cb(s_field_kb, field_kb_hide, LV_EVENT_READY,
                                NULL);
            lv_obj_add_event_cb(s_field_kb, field_kb_hide, LV_EVENT_CANCEL,
                                NULL);
        }
        lv_keyboard_set_textarea(s_field_kb, ta);
        lv_obj_remove_flag(s_field_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_field_kb);
    } else if (code == LV_EVENT_DEFOCUSED) {
        if (s_field_kb && lv_keyboard_get_textarea(s_field_kb) == ta)
            lv_obj_add_flag(s_field_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- screens ------------------------------------------------------ */

static bool ui_up(void)
{
    int w, h;
    ui_tab5_canvas_size(&w, &h);
    return w != 0;
}

extern "C" uint32_t ui_tab5_w_screen(const char *title, uint32_t *evicted)
{
    *evicted = 0;
    if (!ui_up())
        return 0;
    lvgl_port_lock(0);
    if (!s_root)
        s_root = lv_screen_active(); /* console screen: stack bottom */

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_style_text_font(scr, ui_tab5_jp_font(), 0);
    lv_obj_set_style_pad_all(scr, 16, 0);
    /* the status bar floats on lv_layer_top() above every screen:
       keep page content out from under it */
    lv_obj_set_style_pad_top(scr, UI_STATUSBAR_H + 12, 0);
    lv_obj_set_style_pad_row(scr, 12, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_AUTO);

    int slot = w_alloc(UI_WK_SCREEN_SLOT, 0, scr);
    if (slot < 0) {
        lv_obj_delete(scr);
        lvgl_port_unlock();
        return 0;
    }
    s_w[slot].scr_slot = (uint8_t)slot; /* screens own themselves */

    if (title && title[0]) {
        lv_obj_t *t = lv_label_create(scr);
        lv_label_set_text(t, title);
        lv_obj_set_width(t, LV_PCT(100));
        lv_obj_set_style_pad_bottom(t, 4, 0);
    }

    /* retain the screen we are leaving (console = sentinel) */
    uint8_t cur_slot = s_cur < 0 ? UI_SLOT_ROOT : (uint8_t)s_cur;
    if (s_sp < (int)sizeof(s_stack))
        s_stack[s_sp++] = cur_slot;
    s_cur = slot;

    /* defer the slide-in to ui_tab5_w_commit (§3.4). A second screen()
       in the same dispatch simply takes the pending spot over — the
       overtaken screen stays built-but-hidden like any retained one. */
    s_pending_load = slot;

    /* depth > N: destroy the deepest retained (non-console) screen */
    int retained = 0;
    for (int i = 0; i < s_sp; i++)
        if (s_stack[i] != UI_SLOT_ROOT)
            retained++;
    if (retained > UI_NAV_RETAIN) {
        for (int i = 0; i < s_sp; i++) {
            if (s_stack[i] == UI_SLOT_ROOT)
                continue;
            int victim = s_stack[i];
            *evicted = w_handle(victim);
            lv_obj_t *vobj = s_w[victim].obj;
            w_orphan_screen(victim);
            lv_obj_delete(vobj); /* hidden tree: free now, one recursion */
            memmove(&s_stack[i], &s_stack[i + 1], (size_t)(s_sp - i - 1));
            s_sp--;
            break;
        }
    }

    uint32_t h = w_handle(slot);
    lvgl_port_unlock();
    return h;
}

extern "C" uint32_t ui_tab5_w_back(void)
{
    if (!ui_up() || !s_root)
        return 0;
    lvgl_port_lock(0);
    int cur_slot = s_cur; /* tracked, NOT lv_screen_active(): see s_cur */
    if (cur_slot < 0 || !s_w[cur_slot].used || s_sp <= 0) {
        lvgl_port_unlock();
        return 0; /* console active (or stale state): nothing to pop */
    }
    lv_obj_t *cur_obj = s_w[cur_slot].obj;
    uint8_t prev_slot = s_stack[--s_sp];
    lv_obj_t *prev =
        (prev_slot == UI_SLOT_ROOT) ? s_root : s_w[prev_slot].obj;
    if (!prev) { /* retained screen was evicted: fall through to console */
        prev = s_root;
        prev_slot = UI_SLOT_ROOT;
        s_sp = 0;
    }
    s_cur = (prev_slot == UI_SLOT_ROOT) ? -1 : (int)prev_slot;
    uint32_t destroyed = w_handle(cur_slot);
    w_orphan_screen(cur_slot);
    if (cur_slot == s_pending_load) {
        /* the departing screen was never loaded (screen() and back() in
           one dispatch): the display still shows the screen below it.
           Free the hidden tree now and re-queue whatever we fell back
           to — commit skips it when it is already the active screen. */
        lv_obj_delete(cur_obj);
        s_pending_load = (prev == s_root) ? -1 : (int)prev_slot;
    } else {
        /* auto_del=true frees the departing tree after the animation — the
           design's "アニメ完了後に旧を解放" without any timer bookkeeping.
           Rapid chained back() calls are safe: lv_screen_load_anim force-
           completes an in-flight load (and del_prev-deletes the screen it
           was leaving) before starting the next one. */
        lv_screen_load_anim(prev, LV_SCR_LOAD_ANIM_MOVE_RIGHT, UI_NAV_ANIM_MS,
                            0, true);
    }
    lvgl_port_unlock();
    return destroyed;
}

/* End-of-dispatch hook from the JS runtime (§3.4): the pending screen
   is fully built by now, start its slide-in. Called on js_task like
   every other ui_tab5_w_*; cheap when nothing is pending. */
extern "C" void ui_tab5_w_commit(void)
{
    if (s_pending_load < 0 || !ui_up())
        return;
    lvgl_port_lock(0);
    int slot = s_pending_load;
    s_pending_load = -1;
    if (s_w[slot].used && s_w[slot].kind == UI_WK_SCREEN_SLOT &&
        s_w[slot].obj && s_cur == slot &&
        lv_screen_active() != s_w[slot].obj) {
        lv_screen_load_anim(s_w[slot].obj, LV_SCR_LOAD_ANIM_MOVE_LEFT,
                            UI_NAV_ANIM_MS, 0, false);
    }
    lvgl_port_unlock();
}

extern "C" void ui_tab5_w_reset(void)
{
    if (!ui_up() || !s_root)
        return;
    lvgl_port_lock(0);
    s_pending_load = -1;
    if (lv_screen_active() != s_root)
        lv_screen_load(s_root);
    s_field_kb = NULL;
    for (int i = 0; i < UI_W_MAX; i++) {
        if (s_w[i].used && s_w[i].kind == UI_WK_SCREEN_SLOT && s_w[i].obj) {
            lv_obj_t *scr = s_w[i].obj;
            w_orphan_screen(i);
            lv_obj_delete(scr);
        }
    }
    for (int i = 0; i < UI_W_MAX; i++) { /* stray non-screen leftovers */
        s_w[i].used = false;
        s_w[i].obj = NULL;
    }
    s_sp = 0;
    s_cur = -1;
    lvgl_port_unlock();
}

/* ---- widgets ------------------------------------------------------ */

extern "C" uint32_t ui_tab5_w_create(int kind, uint32_t parent,
                                     const char *text, int a, int b, int c)
{
    if (!ui_up())
        return 0;
    if (!text)
        text = "";
    lvgl_port_lock(0);
    int pslot = w_lookup(parent);
    bool parent_ok =
        pslot >= 0 && (kind == UI_WK_ITEM
                           ? s_w[pslot].kind == UI_WK_LIST
                           : s_w[pslot].kind == UI_WK_SCREEN_SLOT);
    if (!parent_ok) {
        lvgl_port_unlock();
        return 0;
    }
    lv_obj_t *p = s_w[pslot].obj;
    uint8_t scr_slot = s_w[pslot].scr_slot;
    lv_obj_t *obj = NULL; /* table entry: the value-carrying object */
    int slot = -1;

    switch (kind) {
    case UI_WK_BUTTON: {
        obj = lv_button_create(p);
        lv_obj_set_width(obj, LV_PCT(100));
        lv_obj_t *l = lv_label_create(obj);
        lv_label_set_text(l, text);
        lv_obj_center(l);
        break;
    }
    case UI_WK_LABEL: {
        obj = lv_label_create(p);
        lv_label_set_text(obj, text);
        lv_obj_set_width(obj, LV_PCT(100));
        break;
    }
    case UI_WK_FIELD: {
        lv_obj_t *cont = lv_obj_create(p);
        lv_obj_remove_style_all(cont);
        lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(cont, 4, 0);
        if (text[0]) {
            lv_obj_t *l = lv_label_create(cont);
            lv_label_set_text(l, text);
            lv_obj_set_style_text_color(l, lv_color_hex(UI_COL_DIM), 0);
        }
        obj = lv_textarea_create(cont);
        lv_textarea_set_one_line(obj, true);
        lv_obj_set_width(obj, LV_PCT(100));
        lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COL_PANEL), 0);
        lv_obj_set_style_text_color(obj, lv_color_hex(UI_COL_TEXT), 0);
        /* visible focus (user feedback): accent border + lighter bg on
           the focused field, dim hairline otherwise */
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(UI_COL_DIM), 0);
        lv_obj_set_style_border_width(obj, 3, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(obj, lv_color_hex(UI_COL_ACCENT),
                                      LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(obj, lv_color_hex(0x223240),
                                  LV_STATE_FOCUSED);
        if (a)
            lv_textarea_set_password_mode(obj, true);
        lv_obj_add_event_cb(obj, field_focus_cb, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(obj, field_focus_cb, LV_EVENT_DEFOCUSED, NULL);
        break;
    }
    case UI_WK_LIST: {
        obj = lv_list_create(p);
        lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COL_PANEL), 0);
        lv_obj_set_style_text_color(obj, lv_color_hex(UI_COL_TEXT), 0);
        break;
    }
    case UI_WK_ITEM: {
        obj = lv_list_add_button(p, NULL, text);
        /* solid row + unmistakable pressed state (user feedback: the
           transparent rows only showed the default theme's pressed
           transform, which read as "the divider changed length") */
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COL_PANEL), 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COL_PRESS),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_text_color(obj, lv_color_hex(UI_COL_TEXT), 0);
        lv_obj_set_style_text_color(obj, lv_color_white(),
                                    LV_STATE_PRESSED);
        /* kill the default theme's pressed shrink (the divider-length
           illusion) — feedback is the bg color, not geometry */
        lv_obj_set_style_transform_width(obj, 0, LV_STATE_PRESSED);
        lv_obj_set_style_transform_height(obj, 0, LV_STATE_PRESSED);
        break;
    }
    case UI_WK_TOGGLE: {
        lv_obj_t *cont = lv_obj_create(p);
        lv_obj_remove_style_all(cont);
        lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (text[0]) {
            lv_obj_t *l = lv_label_create(cont);
            lv_label_set_text(l, text);
        }
        obj = lv_switch_create(cont);
        if (a)
            lv_obj_add_state(obj, LV_STATE_CHECKED);
        break;
    }
    case UI_WK_SLIDER: {
        lv_obj_t *cont = lv_obj_create(p);
        lv_obj_remove_style_all(cont);
        lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(cont, 8, 0);
        lv_obj_set_style_pad_hor(cont, 8, 0);
        lv_obj_set_style_pad_ver(cont, 10, 0); /* room for the knob */
        if (text[0]) {
            lv_obj_t *l = lv_label_create(cont);
            lv_label_set_text(l, text);
            lv_obj_set_style_text_color(l, lv_color_hex(UI_COL_DIM), 0);
        }
        obj = lv_slider_create(cont);
        lv_obj_set_width(obj, LV_PCT(100));
        if (b > a)
            lv_slider_set_range(obj, a, b);
        lv_slider_set_value(obj, c, LV_ANIM_OFF);
        break;
    }
    default:
        break;
    }

    if (obj)
        slot = w_alloc((uint8_t)kind, scr_slot, obj);
    if (slot >= 0) {
        uint32_t h = w_handle(slot);
        if (kind == UI_WK_BUTTON || kind == UI_WK_ITEM)
            lv_obj_add_event_cb(obj, w_event_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)h);
        else if (kind == UI_WK_TOGGLE || kind == UI_WK_SLIDER)
            lv_obj_add_event_cb(obj, w_event_cb, LV_EVENT_VALUE_CHANGED,
                                (void *)(uintptr_t)h);
        lvgl_port_unlock();
        return h;
    }
    /* table full: drop the orphan LVGL subtree we just built */
    if (obj)
        lv_obj_delete(kind == UI_WK_FIELD || kind == UI_WK_TOGGLE ||
                              kind == UI_WK_SLIDER
                          ? lv_obj_get_parent(obj)
                          : obj);
    lvgl_port_unlock();
    return 0;
}

/* Trailing ✕ on a list row (P4b launcher: inline stop, no confirm
   page). The button is its own table entry — its CLICKED posts
   EV_WIDGET with its own handle; LVGL hit-testing targets the deepest
   clickable object, so a ✕ tap never fires the row's callback. */
extern "C" uint32_t ui_tab5_w_item_close(uint32_t item, int icon)
{
    if (!ui_up())
        return 0;
    lvgl_port_lock(0);
    int islot = w_lookup(item);
    if (islot < 0 || s_w[islot].kind != UI_WK_ITEM) {
        lvgl_port_unlock();
        return 0;
    }
    lv_obj_t *row = s_w[islot].obj;
    /* keep the row label clear of the button */
    lv_obj_set_style_pad_right(row, 76, 0);

    lv_obj_t *btn = lv_button_create(row);
    lv_obj_remove_style_all(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    /* list rows lay children out with flex: opt out so align() sticks */
    lv_obj_add_flag(btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(btn, 60, 44);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 68, 0); /* inside the pad zone */
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A3540), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE05A4E), LV_STATE_PRESSED);
    lv_obj_t *l = lv_label_create(btn);
    /* FontAwesome glyphs shipped inside the Montserrat fallback font */
    lv_label_set_text(l, icon == 1 ? LV_SYMBOL_TRASH : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_center(l);

    int slot = w_alloc(UI_WK_BUTTON, s_w[islot].scr_slot, btn);
    if (slot < 0) {
        lv_obj_delete(btn);
        lvgl_port_unlock();
        return 0;
    }
    uint32_t h = w_handle(slot);
    lv_obj_add_event_cb(btn, w_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)h);
    lvgl_port_unlock();
    return h;
}

extern "C" bool ui_tab5_w_set_text(uint32_t handle, const char *text)
{
    if (!ui_up() || !text)
        return false;
    lvgl_port_lock(0);
    int slot = w_lookup(handle);
    bool ok = false;
    if (slot >= 0) {
        lv_obj_t *o = s_w[slot].obj;
        switch (s_w[slot].kind) {
        case UI_WK_LABEL:
            lv_label_set_text(o, text);
            ok = true;
            break;
        case UI_WK_FIELD:
            lv_textarea_set_text(o, text);
            ok = true;
            break;
        case UI_WK_BUTTON:
        case UI_WK_ITEM:
            /* the visible text is a child label */
            for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
                lv_obj_t *c = lv_obj_get_child(o, i);
                if (lv_obj_check_type(c, &lv_label_class)) {
                    lv_label_set_text(c, text);
                    ok = true;
                    break;
                }
            }
            break;
        default:
            break;
        }
    }
    lvgl_port_unlock();
    return ok;
}

extern "C" bool ui_tab5_w_value_str(uint32_t handle, char *buf, size_t cap)
{
    if (cap)
        buf[0] = '\0';
    if (!ui_up())
        return false;
    lvgl_port_lock(0);
    int slot = w_lookup(handle);
    bool ok = slot >= 0 && s_w[slot].kind == UI_WK_FIELD;
    if (ok) {
        const char *t = lv_textarea_get_text(s_w[slot].obj);
        if (t && cap) {
            size_t n = strlen(t);
            if (n >= cap)
                n = cap - 1; /* NB: may tear a UTF-8 tail; cap is ample */
            memcpy(buf, t, n);
            buf[n] = '\0';
        }
    }
    lvgl_port_unlock();
    return ok;
}

extern "C" int ui_tab5_w_value_int(uint32_t handle)
{
    if (!ui_up())
        return 0;
    lvgl_port_lock(0);
    int slot = w_lookup(handle);
    int v = 0;
    if (slot >= 0) {
        if (s_w[slot].kind == UI_WK_TOGGLE)
            v = lv_obj_has_state(s_w[slot].obj, LV_STATE_CHECKED) ? 1 : 0;
        else if (s_w[slot].kind == UI_WK_SLIDER)
            v = lv_slider_get_value(s_w[slot].obj);
    }
    lvgl_port_unlock();
    return v;
}

extern "C" size_t ui_tab5_lv_mem_free(void)
{
    if (!ui_up())
        return 0;
    lv_mem_monitor_t mon;
    lvgl_port_lock(0);
    lv_mem_monitor(&mon);
    lvgl_port_unlock();
    return mon.free_size;
}

#endif /* CONFIG_MQJS_TAB5_UI */
