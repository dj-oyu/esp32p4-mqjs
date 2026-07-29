/*
 * Terminal-grid cell width — the single source of truth for "how many
 * columns does this codepoint occupy".
 *
 * Two callers, one table, on purpose: ui.cellWidth() (components/mqjs)
 * hands the answer to the JS terminals so they can lay out columns and
 * pad wide characters, and the ui.cells glyph blitters
 * (components/ui_tab5/ui_tab5.cpp) use it to size the clip box. If the
 * two ever disagreed the mismatch would be silent — text would simply
 * drift a column at a time — so the table lives here rather than in
 * either caller.
 *
 * Header-only and deliberately free of sdkconfig.h / ESP-IDF headers:
 * mqjs_runtime.c also compiles for the host (tools/run_pc.c), where no
 * component manager sets up include paths. Same arrangement as
 * skk_core.h.
 *
 * This is a compact wcwidth: the East Asian Wide/Fullwidth blocks plus
 * the common zero-width ranges, not the full Unicode tables. Private
 * use (Nerd Font icons) stays width 1 on purpose — the icons are drawn
 * from a cell-fitted monospace font, so they occupy exactly one column.
 *
 * NB U+3099/U+309A (the combining kana voiced/semi-voiced marks) are
 * listed with the other zero-width ranges even though they sit inside
 * the wide 0x2E80-0xA4CF block. They are the NFD form of the dakuten,
 * which is what an SSH session to macOS returns for every filename
 * (APFS/HFS+ store NFD), so a wide classification there would spend two
 * extra columns per voiced kana and shear the rest of the line.
 */
#pragma once

#include <stdint.h>

static inline int ui_cell_width(uint32_t cp)
{
    if (cp == 0 || cp < 0x20 || (cp >= 0x7F && cp < 0xA0) ||
        cp == 0x200D ||
        (cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0x3099 && cp <= 0x309A) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) ||
        (cp >= 0xFE20 && cp <= 0xFE2F) ||
        (cp >= 0xE0100 && cp <= 0xE01EF))
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2329 && cp <= 0x232A) ||
        (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF01 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}
