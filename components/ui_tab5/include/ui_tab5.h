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
    /* Battery (pushed by main/ui_status.c from the pwr_tab5 sample hook;
       plain ints on purpose so this header stays independent of the
       battery component -- the encoding is pwr_batt_state_t in
       components/pwr_tab5/include/pwr_tab5.h). -1 = unknown / no pack. */
    int8_t  batt_pct;
    uint8_t batt_state;
    int16_t batt_eta_min;
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
    UI_CMD_KEYBOARD,  /* on-screen keyboard mode in x: 0 = hide,
                         1 = keyboard, 2 = keyboard + terminal control
                         bar (T3a: Esc/Tab/Ctrl/Alt/Fn/arrows/Copy/Paste
                         as "\0name" key tokens) */
    UI_CMD_CELLS,     /* monospace run: text at cell (x=col, y=row), color=fg,
                         bg=bg. Drawn with the terminal grid font (ui.cells).
                         w = attribute bits (UI_CELL_ATTR_*, 0 for none):
                         one rule per run, drawn in fg after the glyphs
                         (docs/term-design.md §6, "1 本線描画"). */
    UI_CMD_SCROLL,    /* scroll cell-rows [x=top, y=bot] by w lines
                         (w>0 up, w<0 down); vacated rows filled with color */
    UI_CMD_OVERLAY,   /* floating window anchored to a caller-supplied point
                         (docs/ui-overlay-plan.md).

                         Drawn as LVGL objects ABOVE the canvas, never into
                         it. That is the whole point: an app that draws a
                         float itself has to remember which rows it covered
                         and force them to repaint, because the cells
                         renderer's dirty check is content-based and an
                         overdraw on unchanged content survives forever.
                         Compositing removes the problem instead of asking
                         every app to solve it.

                         x, y  anchor top-left (canvas pixels)
                         h     anchor height, so "below" clears the line
                         w     handle id, 0 .. UI_OVERLAY_SLOTS-1
                         color selected item index, -1 for none
                         bg    bit0-1 place: 0 auto, 1 below, 2 above
                               bit2   items direction: 0 horizontal, 1 vertical
                               bit3   lines carry LVGL recolor markup
                                      (#RRGGBB ...#; the IME paints the
                                      preedit's spans with it)
                         text  content, or NULL to hide this handle:
                                 line ("\1" line)* ["\2" item ("\1" item)*]
                               i.e. \1 separates, \2 starts the item list. */
    UI_CMD_RESET,     /* foreground-app switch: clear + hide the canvas and
                         hide the keyboard (same hygiene as a task switch) */
    UI_CMD_FIELD_KEY, /* I3: one keystroke for the focused widget FIELD.
                         text = heap copy, w = its LENGTH — the "\0name"
                         tokens start with a NUL, so strlen would read
                         them as empty. Posted from whichever task the
                         key came in on; applied to the lv_textarea by
                         the UI task (no lv_obj is touched off it). */
} ui_cmd_op_t;

/* Cell attributes a UI_CMD_CELLS run can carry (cmd.w). Everything else
   an SGR sequence can say is already folded into fg/bg by the time cells
   reach here — reverse swapped them, bold picked the bright twin — so the
   renderer needs no attribute logic beyond drawing these two rules
   (docs/term-design.md §6, term_core.h's cell contract). */
enum {
    UI_CELL_ATTR_UNDERLINE = 1u << 0, /* SGR 4 */
    UI_CELL_ATTR_STRIKE    = 1u << 1, /* SGR 9 */
};

typedef struct {
    uint8_t op;     /* ui_cmd_op_t */
    int16_t x, y;
    int16_t w, h;   /* RECT: size; LINE: end point; CELLS: w = UI_CELL_ATTR_*
                       bits, h unused; SCROLL: w=lines (signed);
                       others: unused */
    uint32_t color; /* 0xRRGGBB (CELLS: fg; SCROLL: fill) */
    uint32_t bg;    /* CELLS: background 0xRRGGBB; others: unused */
    char *text;     /* TEXT/CELLS only: heap copy. Consumed (freed) by the UI
                       on success; stays owned by the caller when
                       ui_tab5_cmd() returns false. */
} ui_cmd_t;

/* One run of cells drawn straight into the canvas by a NATIVE presenter
 * (the editor/filer, docs/native-editor-spec.md §A.3) — as opposed to
 * UI_CMD_CELLS, which a JS app posts and the UI task executes.
 *
 * Same cell contract as ui.cells (see the top of ui_tab5.cpp): one
 * codepoint per column, a width-2 codepoint followed by a filler
 * codepoint. `ncells` is the columns this run OWNS: the background is
 * filled across all of them, and columns past the end of `utf8` stay
 * background — that is how a presenter clears the tail of a row in the
 * same call. `utf8`/`len` need not be NUL-terminated.
 *
 * `a8`   caller-owned staging buffer, 64-BYTE ALIGNED, internal SRAM,
 *        at least UI_CELL_H * ncells * UI_CELL_W bytes rounded up to 64
 *        (the PPA's cache maintenance works in cache lines). Runs wider
 *        than it fits are split; a NULL or unusable buffer is not an
 *        error, it just puts the run on the CPU path.
 * `ppa`  caller-owned ppa_client_handle_t for PPA_OPERATION_BLEND
 *        (ppa_register_client). Each client has its own queue, the
 *        engine serialises them; NULL = CPU path. Register it OFF the
 *        presenter's task — registering allocates.
 */
typedef struct {
    int col, row, ncells;
    const char *utf8;
    size_t len;
    uint32_t fg, bg;   /* 0xRRGGBB */
    unsigned attrs;    /* UI_CELL_ATTR_* */
    uint8_t *a8;
    size_t a8_len;
    void *ppa;         /* ppa_client_handle_t */
} ui_cells_draw_t;

/* Overlay handles per app, and how many items one overlay shows. Both are
   small on purpose: overlays are transient decoration, and the labels are
   allocated up-front per handle on first use. */
#define UI_OVERLAY_MAX   4
#define UI_OVERLAY_ITEMS 10

/* One more slot, above every app handle: the platform's own IME float
   (preedit + candidates), issued by C now that apps do not see a preedit
   at all. It is separate rather than "id 0 by convention" because an app
   that opts into the IME goes on using its own overlays — ssh_vt held id
   0 for the IME and gets it back. */
#define UI_OVERLAY_IME   UI_OVERLAY_MAX
#define UI_OVERLAY_SLOTS (UI_OVERLAY_MAX + 1)

/* What the control bar's 「あ」 key shows (design §6.2): the thing you
   press to change the mode is the thing that shows it, so no app needs a
   mode indicator of its own. Values, not skk_mode_t — the UI layer must
   not learn the engine's enum. */
typedef enum {
    UI_IME_FACE_ASCII = 0, /* IME off / ASCII: "A"  */
    UI_IME_FACE_KANA  = 1, /* hiragana:        "あ" */
    UI_IME_FACE_KATA  = 2, /* katakana:        "ア" */
} ui_ime_face_t;

/* Overlays are taken down by UI_CMD_RESET, which mqjs already posts both
   when the foreground app switches and when a foreground app stops — the
   same point that clears the canvas. No separate teardown call exists on
   purpose: one hygiene point is easier to keep correct than two. */

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
    UI_WK_FIELD  = 2, /* labelled one-line textarea; a = ui_field_mode_t */
    UI_WK_LIST   = 3,
    UI_WK_ITEM   = 4, /* list row; parent must be a UI_WK_LIST handle   */
    UI_WK_TOGGLE = 5, /* labelled switch; a!=0 -> initially on          */
    UI_WK_SLIDER = 6, /* a=min b=max c=initial value                    */
} ui_widget_kind_t;

/* What a UI_WK_FIELD accepts (I3). The platform, not the app, enforces
 * it: on focus a non-JA field forces the IME off and the control bar's
 * 「あ」 key goes DISABLED, so no app can forget to constrain a field.
 *
 * ASCII is the default deliberately. The two failure modes are not
 * symmetric — forgetting "ja" on a free-text field only means the user
 * cannot type Japanese there (obvious, harmless), while forgetting to
 * constrain a Wi-Fi SSID field means kana in an SSID and a connection
 * that fails for a reason nobody can see.
 *
 * PASSWORD == 1 on purpose: the old API was a truthy "secret" flag, so
 * every existing {secret:true} caller keeps its exact behaviour. Values
 * are mirrored in components/mqjs/mqjs_classes.h — keep both in sync. */
typedef enum {
    UI_FIELD_ASCII    = 0, /* ASCII only; the IME cannot be armed */
    UI_FIELD_PASSWORD = 1, /* password mode; the IME is never allowed */
    UI_FIELD_JA       = 2, /* Japanese allowed (「あ」 is live) */
} ui_field_mode_t;

typedef void (*ui_tab5_ready_cb_t)(void *arg);

#if CONFIG_MQJS_TAB5_UI

/* Initialize panel + LVGL and start the UI task (call once, early).
 * ready_cb runs once after touch/I2C init; it must not block. */
void ui_tab5_start(ui_tab5_ready_cb_t ready_cb, void *arg);
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

/* ---- native presenter surface (docs/native-editor-spec.md §A.3) ----
 *
 * The native editor/filer draws into the same canvas as ui.*, but from
 * its OWN task and without going through the command queue. Splitting
 * the pixels from the presentation is the point: a 26-row scroll takes
 * the LVGL lock ONCE, at the end, for microseconds.
 *
 * _cells_draw and _canvas_fill run on the CALLING task and take no
 * LVGL lock at all — they touch a plain RGB565 buffer LVGL only reads.
 * Two tasks drawing at once tear (a torn cell is one frame old); they
 * never corrupt. Neither allocates, neither blocks except inside a
 * blocking PPA operation.
 *
 * _canvas_invalidate takes the lock, marks the rectangle dirty and
 * returns. Coordinates are canvas-relative pixels and are clamped.
 * Pass the UNION of the rows that changed: LVGL renders in 720x50
 * chunks, so two adjacent 24px rows often cost one chunk while two
 * distant ones always cost two — merging them first is free.
 * It also un-hides the canvas, because the caller has just drawn.
 */
bool ui_tab5_cells_draw(const ui_cells_draw_t *d); /* false = no canvas */
void ui_tab5_canvas_fill(int x, int y, int w, int h, uint32_t rgb);
void ui_tab5_canvas_invalidate(int x, int y, int w, int h);
/* Pixel size of a UTF-8 string in the canvas font (no wrapping; \n makes
 * it multi-line). 0x0 when the UI is off or init failed. Safe from any
 * task: only reads const font tables. */
void ui_tab5_text_size(const char *utf8, int *w, int *h);
/* Cell size (advance width, line height) of the monospace terminal font
 * used by ui.cells/UI_CMD_CELLS. 0x0 when the UI is off. Const tables only. */
void ui_tab5_cell_size(int *w, int *h);
/* Pixels the keyboard overlay reserves at the canvas bottom in `mode`
 * (0/1/2, see UI_CMD_KEYBOARD); 0 when the UI is off. Synchronous
 * (ui.keyboard's return value: the JS terminal sizes its grid with it).
 * Depends on the keyboard dock: docked, mode 1 reserves nothing (the
 * dock types directly) and mode 2 only the control bar's height. */
int ui_tab5_kb_reserved(int mode);

/* Show the IME's mode on the control bar's 「あ」 key (see
 * ui_ime_face_t). Callable from any task (takes the LVGL lock; the
 * relabel itself is deferred to the LVGL loop like every other
 * control-bar map change). */
void ui_tab5_ime_face(int face);

/* Keyboard dock presence (kbd_tab5): while set, ui.keyboard() requests
 * raise no on-screen keyboard — mode 2 keeps only the control bar
 * (F-keys/copy/paste have no dock equivalent) — and the foreground
 * app's last requested mode is re-applied immediately. Apps stay
 * unaware; their ui.keyboard() return value does the sizing. */
void ui_tab5_set_hw_keyboard(bool present);

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

/* I3: deliver one keystroke to the focused FIELD, if any. Returns true
 * when a field took it — i.e. "this key is not the JS app's". Callable
 * from any task (the mqjs key funnel runs on the LVGL task, on the
 * keyboard dock's task and on the IME's owner task): it only reads a
 * focus flag and posts a UI command, never an lv_obj. `utf8` may be a
 * "\0name" token, hence the explicit length. */
bool ui_tab5_field_key(const char *utf8, size_t len);

/* Destroy every widget screen and return to the console screen. Called
 * by the JS runtime when a task ends (same role as the canvas clear on
 * task switch). Safe to call when nothing was ever created. */
void ui_tab5_w_reset(void);

/* フォアグラウンド切替の画面掃除を同期でやる (UI_CMD_RESET の同期版)。
   完了して戻るので、呼んだ側は「以後この画面は自分のもの」と仮定してよい。
   任意タスク。lvgl_port_lock を取り、全画面 fill 1 回ぶん待つ。 */
void ui_tab5_canvas_reset_sync(void);

/* Start the slide-in animation of the most recent ui_tab5_w_screen()
 * (P4a, design §3.4): screens are created WITHOUT loading so the page
 * can be fully built first; the JS runtime calls this at the end of
 * each event dispatch. No-op when nothing is queued. */
void ui_tab5_w_commit(void);

/* Free bytes in the LVGL heap (builtin tlsf pool; 0 with CLIB malloc or
 * when the UI is down). Third element of sys.heap() — the W1-4 thrash
 * metric lives inside the preallocated pool, invisible to heap_caps. */
size_t ui_tab5_lv_mem_free(void);

/* Landscape rotation (keyboard dock). Flips the whole UI between
 * portrait (720x1280) and landscape (1280x720, panel content PPA-
 * rotated per flush): status bar/console/canvas re-size, the lazily
 * built overlays (on-screen keyboard, control bar, stats panel) are
 * rebuilt at the new width on next use, and the foreground app gets a
 * "\x00rotate" key token — its ui.size() has changed. Callable from
 * any task (takes the LVGL lock); no-op before the UI is up or when
 * already in the requested orientation. */
void ui_tab5_set_landscape(bool on);
bool ui_tab5_landscape(void);

/* The touch controller's I2C master bus handle (port 1, SDA31/SCL32) —
 * the internal bus the camera's SCCB also lives on. void* so this
 * header stays IDF-type-free; cast to i2c_master_bus_handle_t. NULL
 * until touch_init ran (or when it failed). */
void *ui_tab5_i2c_bus(void);

/* Camera viewfinder overlay (cam_tab5's scan task): returns the RGB565
 * pixel buffer of a w x h lv_canvas centered near the top of the
 * screen (created on first call, re-shown on later ones). NULL when
 * the UI is down or the buffer allocation failed. The scan task
 * writes frames into the buffer and calls _update (tearing is fine
 * for a viewfinder); _hide when the scan ends. */
void *ui_tab5_cam_canvas(int w, int h);
void ui_tab5_cam_canvas_update(void);
void ui_tab5_cam_canvas_hide(void);
/* Telemetry label below the viewfinder (decode/near-miss readout);
 * shown on first call, hidden together with the canvas. */
void ui_tab5_cam_overlay_text(const char *utf8);
/* While the viewfinder is shown it behaves like a web modal: a
 * translucent scrim absorbs every touch (nothing reaches the widgets
 * or ui.onTouch behind it) and a tap OUTSIDE the viewfinder/readout
 * fires cb (runs on the LVGL task — keep it to a flag write).
 * cam_tab5 registers its cancel here before each scan. */
void ui_tab5_cam_set_dismiss_cb(void (*cb)(void));

/* ------------------------------------------------------------------ */
/* UI-task work seam (docs/term-design.md §5, §7.2).                   */
/*                                                                     */
/* The native terminal parses, resizes and serialises snapshots ON the */
/* UI frame task, which is what keeps its grid single-writer and its   */
/* snapshots frame-consistent. It needs three things from here and     */
/* nothing else — ui_tab5 stays unaware that term_core exists.         */
/* ------------------------------------------------------------------ */

typedef void (*ui_tab5_job_fn)(void *arg);

/* Queue fn(arg) for the UI task. Returns false when the queue is full
 * or the UI is down; the caller reports an error rather than blocking.
 * `arg` must stay alive until the job has run. */
bool ui_tab5_post_job(ui_tab5_job_fn fn, void *arg, uint32_t timeout_ms);

/* True when the calling task IS the UI task. A caller that is already
 * there runs its work inline instead of posting to itself. */
bool ui_tab5_is_ui_task(void);

/* Run fn(arg) once per UI frame, after the canvas has consumed its
 * command queue. One slot; a second call replaces the first, NULL
 * clears. The callback runs on the UI task under the LVGL lock and
 * must stay short — it shares the frame with LVGL. */
void ui_tab5_set_frame_cb(ui_tab5_job_fn fn, void *arg);

#else /* stubs: UI disabled (Stamp-P4 and default builds) */

typedef void (*ui_tab5_job_fn)(void *arg);
static inline bool ui_tab5_post_job(ui_tab5_job_fn fn, void *arg,
                                    uint32_t timeout_ms)
{
    (void)fn;
    (void)arg;
    (void)timeout_ms;
    return false;
}
static inline bool ui_tab5_is_ui_task(void) { return false; }
static inline void ui_tab5_set_frame_cb(ui_tab5_job_fn fn, void *arg)
{
    (void)fn;
    (void)arg;
}

static inline void ui_tab5_start(ui_tab5_ready_cb_t ready_cb, void *arg)
{
    (void)ready_cb;
    (void)arg;
}
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
static inline bool ui_tab5_cells_draw(const ui_cells_draw_t *d)
{
    (void)d;
    return false;
}
static inline void ui_tab5_canvas_fill(int x, int y, int w, int h,
                                       uint32_t rgb)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)rgb;
}
static inline void ui_tab5_canvas_invalidate(int x, int y, int w, int h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
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
static inline int ui_tab5_kb_reserved(int mode)
{
    (void)mode;
    return 0;
}
static inline void ui_tab5_ime_face(int face) { (void)face; }
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
static inline bool ui_tab5_field_key(const char *utf8, size_t len)
{
    (void)utf8;
    (void)len;
    return false;
}
static inline void ui_tab5_w_reset(void) {}
static inline void ui_tab5_w_commit(void) {}
static inline size_t ui_tab5_lv_mem_free(void) { return 0; }
static inline void *ui_tab5_i2c_bus(void) { return 0; }
static inline void *ui_tab5_cam_canvas(int w, int h)
{
    (void)w;
    (void)h;
    return 0;
}
static inline void ui_tab5_cam_canvas_update(void) {}
static inline void ui_tab5_cam_canvas_hide(void) {}
static inline void ui_tab5_cam_overlay_text(const char *utf8)
{
    (void)utf8;
}
static inline void ui_tab5_cam_set_dismiss_cb(void (*cb)(void)) { (void)cb; }

#endif /* CONFIG_MQJS_TAB5_UI */

#ifdef __cplusplus
}
#endif
