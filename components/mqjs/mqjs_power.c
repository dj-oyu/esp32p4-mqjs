/*
 * Device power-state machine — P0 screen slice. See mqjs_power.h and
 * docs/power-states.md.
 *
 * State lives in three statics:
 *   s_state         the current power state. Written on js_task
 *                   (mqjs_power_update), read on the touch task
 *                   (note_input). A word-sized read/write is atomic on
 *                   riscv32, and a one-tick-stale read only mis-times a
 *                   transition by <=50ms — harmless for a power timer.
 *   s_input_seen    set on the touch task, cleared on js_task. The touch
 *                   side never reads the int64 clock; it only raises this
 *                   flag and js_task stamps the time. Keeps the 64-bit
 *                   clock single-task (no torn read).
 *   s_eat_gesture   touch-task local: once a wake tap is swallowed, eat
 *                   the rest of that gesture (move/up) so the foreground
 *                   app never sees an orphaned move/up after waking.
 *
 * No locks: one coarse timer, single writer per field.
 */
#include "mqjs_power.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
/* reverse of the mqjs_post_touch extern trick: ui_tab5 owns the LEDC
   backlight; we apply a temporary dim/blank without disturbing the
   user's chosen level (ui_tab5_backlight_user), then restore it. */
extern void ui_tab5_backlight_apply(int percent);
extern int  ui_tab5_backlight_user(void);
static const char *TAG = "power";
#define PWR_LOG(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#include <stdio.h>
static void ui_tab5_backlight_apply(int percent) { (void)percent; }
static int  ui_tab5_backlight_user(void) { return 100; }
#define PWR_LOG(fmt, ...) printf("[power] " fmt "\n", ##__VA_ARGS__)
#endif

/* TEST thresholds — deliberately short for on-device verification via
   the serial monitor. Production values are 60s / 180s
   (docs/power-states.md, confirmed 2026-06-13). */
#define PWR_T_DIM_MS   5000
#define PWR_T_OFF_MS   10000
#define PWR_DIM_PCT    10

typedef enum {
    PWR_ACTIVE = 0,
    PWR_DIMMED,
    PWR_SCREEN_OFF,
} pwr_state_t;

static volatile int  s_state = PWR_ACTIVE;
static volatile bool s_input_seen = false;
static int64_t       s_last_input_ms = 0;
static bool          s_eat_gesture = false; /* touch task only */

static const char *state_name(int s)
{
    switch (s) {
    case PWR_ACTIVE:     return "ACTIVE";
    case PWR_DIMMED:     return "DIMMED";
    case PWR_SCREEN_OFF: return "SCREEN_OFF";
    default:             return "?";
    }
}

void mqjs_power_init(int64_t now_ms)
{
    s_state = PWR_ACTIVE;
    s_input_seen = false;
    s_eat_gesture = false;
    s_last_input_ms = now_ms;
    PWR_LOG("init: ACTIVE (T_dim=%dms T_off=%dms)", PWR_T_DIM_MS, PWR_T_OFF_MS);
}

bool mqjs_power_note_input(int kind)
{
    s_input_seen = true;

    /* Eat the gesture that wakes a blanked screen, through its up — even
       if the up arrives while still SCREEN_OFF (before js_task flips the
       state) or after the wake (state already ACTIVE). Latch on first
       contact, clear on up (kind==2). DIMMED/ACTIVE with no active eat:
       deliver normally, the screen is visible. */
    if (s_state == PWR_SCREEN_OFF || s_eat_gesture) {
        s_eat_gesture = (kind != 2);
        return true;
    }
    return false;
}

void mqjs_power_update(int64_t now_ms)
{
    if (s_input_seen) {
        s_input_seen = false;
        s_last_input_ms = now_ms;
        if (s_state != PWR_ACTIVE) {
            PWR_LOG("%s -> ACTIVE (wake)", state_name(s_state));
            ui_tab5_backlight_apply(ui_tab5_backlight_user());
            s_state = PWR_ACTIVE;
        }
        return;
    }

    int64_t idle = now_ms - s_last_input_ms;
    switch (s_state) {
    case PWR_ACTIVE:
        if (idle >= PWR_T_DIM_MS) {
            PWR_LOG("ACTIVE -> DIMMED (idle %lldms, backlight %d%%)",
                    (long long)idle, PWR_DIM_PCT);
            ui_tab5_backlight_apply(PWR_DIM_PCT);
            s_state = PWR_DIMMED;
        }
        break;
    case PWR_DIMMED:
        if (idle >= PWR_T_OFF_MS) {
            PWR_LOG("DIMMED -> SCREEN_OFF (idle %lldms, backlight off)",
                    (long long)idle);
            ui_tab5_backlight_apply(0);
            s_state = PWR_SCREEN_OFF;
            /* P1 here: suspend background apps + non-locked foreground */
        }
        break;
    case PWR_SCREEN_OFF:
        /* wake only via input (handled above) */
        break;
    }
}
