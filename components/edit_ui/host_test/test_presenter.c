/*
 * test_presenter — what edit_ui's presenter and edit_task can be held to
 * without a Tab5 in the room.
 *
 * WHAT THIS SUITE DOES NOT LOOK AT (docs/native-editor-plan.md §1 asks
 * every test to say so):
 *
 *   - every timing number in spec §C.1. The clock here is a counter.
 *   - the PPA blend, the glyph raster, LVGL's flush, the DSI transfer.
 *     ui_tab5_cells_draw is faked down to the cell CONTRACT.
 *   - the real key path (GPIO50 ISR → I2C → mqjs_post_key → native
 *     surface). Keys are injected straight into the surface callback.
 *   - Japanese input. ime_core is real and linked, but no dictionary is
 *     attached, so every key comes back IME_PASS by design (ime_core.h).
 *     The preedit overlay is therefore UNTESTED here.
 *   - whether anything is legible, or on the screen at all.
 *
 * A green run says: the presenter drives edit_core's dirty/run contract
 * correctly, derives its geometry from ui_tab5 instead of a constant, and
 * keeps the repainted area small for a keystroke and large for a scroll.
 *
 * Every CHECK below was verified by sabotage (plan §1): five deliberate
 * breakages of edit_ui.c, each of which failed the checks that name it —
 * see the list at the bottom of run_tests.sh.
 */
#include <stdio.h>
#include <string.h>

#include "edit_ui.h"
#include "ui_tab5.h"
#include "fake_dev.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); \
                   printf("      [%s:%d]\n", __func__, __LINE__); fails++; } \
} while (0)

static void key(const char *k, size_t n)
{
    fake_surf.key(fake_surf.ctx, k, n, 12345u, 12000u);
    fake_pump();
}
#define KEY(lit) key(lit, sizeof(lit) - 1)

static void dump_row(int r)
{
    printf("  row %2d |", r);
    for (int c = 0; c < 58; c++) {
        const char *s = fake.grid[r][c];
        printf("%s", s[0] ? s : " ");
    }
    printf("|\n");
}

/* The row as one string, so a test can name what should be on it. */
static int row_starts(int r, const char *want)
{
    char buf[1024];
    size_t k = 0;
    for (int c = 0; c < 100 && k < sizeof(buf) - 8; c++) {
        const char *s = fake.grid[r][c];
        if (!s[0]) { buf[k++] = ' '; continue; }
        size_t n = strlen(s);
        memcpy(buf + k, s, n);
        k += n;
    }
    buf[k] = 0;
    return strncmp(buf, want, strlen(want)) == 0;
}

/* Did the presenter clear the WHOLE canvas from its own task?
   Not "did it fill something": every row fills its own tail, so a weaker
   test passes even when the 8 px strip below the last row (632 is not a
   multiple of 24) still holds the previous app's pixels. */
static int had_full_clear(void)
{
    for (int i = 0; i < fake.nfills; i++)
        if (fake.fills[i].x == 0 && fake.fills[i].y == 0 &&
            fake.fills[i].w >= fake.canvas_w && fake.fills[i].h >= fake.canvas_h)
            return 1;
    return 0;
}

/* Screen rows covered by the invalidate rectangles of the last operation. */
static int inval_rows(void)
{
    int px = 0;
    for (int i = 0; i < fake.ninval; i++)
        px += fake.inval[i].h;
    return px / 24;
}

int main(void)
{
    fake.canvas_w = 1280;          /* landscape, the docked geometry */
    fake.canvas_h = 632;
    fake.kb_reserved = 0;

    if (!edit_ui_start(NULL)) {
        printf("FAIL: edit_ui_start returned false\n");
        return 1;
    }

    /* ---- spec §B.1: where edit_task lives ---- */
    printf("edit_task: core=%d prio=%d stack=%u\n",
           fake.task_core, fake.task_prio, fake.task_stack);
    CHECK(fake.task_core == 1, "edit_task must be pinned to core 1\n");
    CHECK(fake.task_prio == 5, "edit_task prio must be 5 (UI is 4)\n");
    CHECK(fake.task_stack == 8192, "edit_task stack must be 8 KB\n");

    /* ---- focus while the canvas does not exist yet ----
       mqjs_native.h's trap 1: UI_CMD_RESET is posted just before focus()
       and runs on the UI task at some later point, wiping the canvas and
       HIDING it. So a native surface must expect its first paint to be
       lost, must NOT post a drawing command of its own (the UI task would
       execute it after this task's pixels), and must come back and paint
       again on its own schedule. */
    fake.canvas_up = false;
    fake_reset_marks();
    fake_surf.focus(fake_surf.ctx);
    fake_pump();
    int saw_kb = 0, saw_draw_cmd = 0;
    for (int i = 0; i < fake.ncmds; i++) {
        if (fake.cmds[i] == UI_CMD_KEYBOARD) saw_kb = 1;
        if (fake.cmds[i] == UI_CMD_CLEAR || fake.cmds[i] == UI_CMD_FILL ||
            fake.cmds[i] == UI_CMD_RECT  || fake.cmds[i] == UI_CMD_CELLS)
            saw_draw_cmd = 1;
    }
    CHECK(saw_kb, "focus must post UI_CMD_KEYBOARD\n");
    CHECK(!saw_draw_cmd,
          "focus must NOT post a drawing command: the UI task would run it "
          "after this task's own pixels\n");
    printf("paint attempt with no canvas: draws=%d\n", fake.ndraws);
    CHECK(fake.ndraws == 0, "nothing lands while there is no canvas\n");

    /* ---- the canvas exists now; the retry must find it ---- */
    fake.canvas_up = true;
    fake_reset_marks();
    fake_fire_timers();            /* the re-armed first-paint one-shot */
    fake_pump();
    printf("first paint: draws=%d invalidates=%d rows=%d fills=%d\n",
           fake.ndraws, fake.ninval, inval_rows(), fake.nfills);
    for (int r = 0; r < 6; r++)
        dump_row(r);
    dump_row(25);
    CHECK(fake.ndraws > 0, "the retry must paint once the canvas is up\n");

    /* ---- and it must keep repainting for a while, because the RESET that
       wipes the canvas can land after any one of those paints ---- */
    fake_reset_marks();
    fake_fire_timers();
    fake_pump();
    printf("scheduled repaint after the first: draws=%d full_clear=%d\n",
           fake.ndraws, had_full_clear());
    CHECK(fake.ndraws > 0,
          "focus must schedule more than one paint (RESET may land late)\n");
    CHECK(had_full_clear(),
          "every focus paint must clear the WHOLE canvas from this task: the "
          "strip below the last row is never covered by a row\n");
    CHECK(row_starts(0, "// edit_ui Phase 1"), "row 0 is the sample's line 1\n");
    CHECK(row_starts(4, "function greet(name) {"), "row 4 is the function line\n");
    CHECK(fake.ninval == 1, "a full paint is ONE rectangle, got %d\n", fake.ninval);

    /* ---- one keystroke stays small (the whole point of §A.3) ---- */
    fake_reset_marks();
    KEY("X");
    printf("one keystroke: draws=%d rects=%d rows=%d\n",
           fake.ndraws, fake.ninval, inval_rows());
    CHECK(inval_rows() <= 3, "one keystroke must dirty <= 3 rows, got %d\n",
          inval_rows());
    CHECK(row_starts(0, "X// edit_ui"), "X landed at the head of row 0\n");

    fake_reset_marks();
    KEY("\b");
    CHECK(row_starts(0, "// edit_ui Phase 1"), "backspace removed the X\n");

    /* ---- adjacent dirty rows merge into one rectangle (§A.3) ----
       A cursor move dirties the old row and the new one; they are
       neighbours and must arrive as a single 48 px rect. The status row is
       far away and stays its own. */
    fake_reset_marks();
    KEY("\0down");
    printf("cursor down: rects=%d first h=%d\n",
           fake.ninval, fake.ninval ? fake.inval[0].h : -1);
    CHECK(fake.ninval == 2,
          "old+new cursor rows merge, status separate: got %d rects\n",
          fake.ninval);
    CHECK(fake.ninval && fake.inval[0].h == 48,
          "the merged rect spans two 24 px rows, got %d\n",
          fake.ninval ? fake.inval[0].h : -1);
    KEY("\0up");

    /* ---- the motion tokens actually reach edit_core ---- */
    KEY("\0down"); KEY("\0down"); KEY("\0end");
    fake_reset_marks();
    KEY("!");
    printf("insert after motion:\n");
    dump_row(2);
    CHECK(fake.rows_touched & ((uint64_t)1 << 2), "the edit landed on row 2\n");

    /* ---- scrolling ----
       This needs a document TALLER than the view. The sample is 13 lines
       and the landscape view is 25 text rows, so PGDN on it moves the
       cursor and scrolls nothing: the first version of this check asserted
       a full repaint there and failed for exactly that reason. */
    KEY("\0end");
    for (int i = 0; i < 60; i++) { KEY("\n"); KEY("y"); }
    fake_reset_marks();
    KEY("\0pgup");
    printf("pgup on a ~73-line document: draws=%d rects=%d rows=%d\n",
           fake.ndraws, fake.ninval, inval_rows());
    CHECK(fake.ninval == 1, "a full repaint is ONE merged rectangle, got %d\n",
          fake.ninval);
    CHECK(fake.ninval && fake.inval[0].w == 1280,
          "a full repaint spans the canvas width\n");
    CHECK(inval_rows() >= 20, "a scroll repaints the view, got %d rows\n",
          inval_rows());

    fake_reset_marks();
    KEY("q");
    printf("keystroke after a scroll: rects=%d rows=%d\n",
           fake.ninval, inval_rows());
    CHECK(inval_rows() <= 3, "a post-scroll keystroke is small again, got %d\n",
          inval_rows());

    /* ---- rotation (§E-9): the geometry comes from ui_tab5, never from a
       constant. 142x26 / 80x49 are initial maxima, not hard-coded sizes. */
    fake_reset_marks();
    fake.canvas_w = 720;
    fake.canvas_h = 1280 - 88;
    KEY("\0rotate");
    printf("after rotate: rects=%d w=%d h=%d\n", fake.ninval,
           fake.ninval ? fake.inval[0].w : -1,
           fake.ninval ? fake.inval[0].h : -1);
    CHECK(fake.ninval >= 1 && fake.inval[0].w == 720,
          "rotate must re-derive the width from ui_tab5_canvas_size\n");
    CHECK(fake.ninval && fake.inval[0].h <= 1280 - 88,
          "the rows must fit the portrait canvas\n");

    /* the on-screen keyboard travels the same path */
    fake_reset_marks();
    fake.canvas_w = 1280;
    fake.canvas_h = 632;
    fake.kb_reserved = 240;
    KEY("\0rotate");
    printf("kb reserving 240 px: h=%d (must be <= %d)\n",
           fake.ninval ? fake.inval[0].h : -1, 632 - 240);
    CHECK(fake.ninval && fake.inval[0].h <= 632 - 240,
          "ui_tab5_kb_reserved must shrink the text area\n");

    /* ---- touch: a drag scrolls ---- */
    fake.kb_reserved = 0;
    KEY("\0rotate");
    fake_reset_marks();
    fake_surf.touch(fake_surf.ctx, 100, 300, 0);
    fake_surf.touch(fake_surf.ctx, 100, 300 - 24 * 5, 1);
    fake_surf.touch(fake_surf.ctx, 100, 300 - 24 * 5, 2);
    fake_pump();
    printf("drag of 5 rows: rects=%d rows=%d\n", fake.ninval, inval_rows());
    CHECK(fake.ninval >= 1, "a drag must repaint\n");

    /* ---- blur: the surface goes quiet ---- */
    fake_surf.blur(fake_surf.ctx);
    fake_pump();
    fake_reset_marks();
    KEY("Z");
    CHECK(fake.ndraws == 0, "keys after blur must not draw, got %d\n",
          fake.ndraws);

    if (fails)
        printf("\n%d CHECK(s) FAILED\n", fails);
    else
        printf("\nall checks passed\n");
    return fails ? 1 : 0;
}
