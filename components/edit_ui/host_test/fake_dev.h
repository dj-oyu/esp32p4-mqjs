/*
 * What the fake device records, so a test can assert on it.
 *
 * The presenter's whole output is "which cells, in what colour, and which
 * rectangles did you hand to LVGL" — so that is what this captures. It is
 * the same substitution docs/native-editor-plan.md §6 names for the thing
 * an agent cannot do ("画面を見る" → セル配列のスナップショット比較).
 *
 * WHAT IT DOES NOT SEE, and no host test can: the PPA blend itself, the
 * glyph raster, LVGL's flush, the DSI transfer, the real key path from the
 * GPIO50 ISR, and every timing number in spec §C.1. A green run here says
 * the presenter drives the contract correctly, not that the editor is fast
 * or even visible.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mqjs_native.h"

#define FAKE_ROWS 64
#define FAKE_COLS 200

typedef struct { int x, y, w, h; } FakeRect;
typedef struct { int row, col, ncells; } FakeDraw;

typedef struct {
    /* what the fake screen answers */
    int canvas_w, canvas_h, kb_reserved;
    bool canvas_up;          /* has the UI task processed a drawing command? */

    /* what edit_ui did */
    int  ime_face, focus_calls;
    int  task_core, task_prio;
    unsigned task_stack;
    char     grid[FAKE_ROWS][FAKE_COLS][8];   /* one UTF-8 cell each */
    uint32_t fg[FAKE_ROWS][FAKE_COLS];
    FakeRect inval[128];  int ninval;
    FakeRect fills[256];  int nfills;
    FakeDraw draws[512];  int ndraws;
    uint8_t  cmds[32];    int ncmds;          /* ui_cmd_op_t values */
    uint64_t rows_touched;
} FakeUi;

extern FakeUi fake;
extern mqjs_native_surface_t fake_surf;   /* what edit_ui_start registered */

/* Run edit_task until its queue is empty (the fake xQueueReceive jumps out
   instead of blocking, so the task's own for(;;) is the loop). */
void fake_pump(void);
/* Fire every armed esp_timer callback once. */
void fake_fire_timers(void);
/* Forget the recorded draws/fills/invalidates/commands. */
void fake_reset_marks(void);
