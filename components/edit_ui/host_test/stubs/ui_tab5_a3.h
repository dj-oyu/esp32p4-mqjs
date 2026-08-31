/*
 * The three entry points docs/native-editor-spec.md §A.3 adds to
 * ui_tab5.h, force-included (-include) while the real header does not
 * carry them yet.
 *
 * run_tests.sh greps ui_tab5.h for ui_tab5_cells_draw and drops this file
 * from the command line the moment it appears there, so the two can never
 * both be in scope — the typedef below would clash with the real one, and
 * that clash is the point: it is how a drifted copy announces itself
 * instead of quietly shadowing the shipped declaration.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int col, row, ncells;
    const char *utf8; size_t len;
    uint32_t fg, bg; unsigned attrs;
    uint8_t *a8; size_t a8_len;
    void *ppa;                       /* ppa_client_handle_t */
} ui_cells_draw_t;
bool ui_tab5_cells_draw(const ui_cells_draw_t *d);
void ui_tab5_canvas_fill(int x, int y, int w, int h, uint32_t rgb);
void ui_tab5_canvas_invalidate(int x, int y, int w, int h);
