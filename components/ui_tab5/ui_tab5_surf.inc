/*
 * ui_tab5_surf.inc — the cell/rect drawing core, in its own file so the
 * host test can compile the SAME text the device does.
 *
 * Included by ui_tab5.cpp at the point where every name it uses is in
 * scope (s_canvas_w/h, the fonts, ui_blend565, cells_utf8_next,
 * cells_glyph_cols, cells_note_clip, the PPA constants). It is not a
 * translation unit and has no includes of its own — that is deliberate:
 * pc_test/ supplies the same names from stubs and gets the real code.
 */

/* ------------------------------------------------------------------ */
/* Surface primitives (spec §A.3)                                      */
/*                                                                     */
/* These were private methods of CanvasApp. The native editor surface  */
/* has to reach the SAME pixels from its OWN task, with its own A8     */
/* staging buffer and its own PPA client, and the one thing this file  */
/* has already paid for twice is two drawing paths that disagree (see  */
/* run_right in surf_blit_glyph, and the straddle clamp in             */
/* compose_glyph_a8). So there is ONE implementation, parameterised by */
/* the target buffer, the staging buffer and the PPA clients, and      */
/* CanvasApp is now one of its two callers.                            */
/*                                                                     */
/* None of them takes the LVGL lock. The target is a plain RGB565      */
/* buffer that LVGL only ever READS (lv_canvas_set_buffer) and never   */
/* reallocates; presenting the result is a separate call               */
/* (ui_tab5_canvas_invalidate) and that one does take the lock, for    */
/* microseconds. Two tasks drawing into the same buffer tear; that is  */
/* the design's accepted cost (a torn cell is one frame old), not an   */
/* oversight.                                                          */
/*                                                                     */
/* s_canvas_w/h stay globals: both callers draw into the one canvas,   */
/* and its dimensions change only in ui_tab5_set_landscape.            */
/* ------------------------------------------------------------------ */

/* One coherent (width, height) pair, or false. Every primitive below
   starts here: they run off the LVGL lock, and ui_tab5_set_landscape
   changes the two globals one after the other, so reading them twice
   inside one function can produce a pair that never existed and does
   not fit the allocation (see s_canvas_px). */
static inline bool surf_geom(int *w, int *h)
{
    int cw = s_canvas_w, ch = s_canvas_h;
    if (cw <= 0 || ch <= 0 || (size_t)cw * (size_t)ch > s_canvas_px)
        return false;
    *w = cw;
    *h = ch;
    return true;
}

static void surf_fill_rect(uint16_t *buf, int x, int y, int w, int h,
                           uint16_t px, ppa_client_handle_t fill_cli)
{
    /* Deliberately SHADOWS the two globals. Renaming the uses instead
       would look tidier and be more dangerous: a missed occurrence
       would keep reading the global, still compile, and defeat the
       guard silently. Shadowing makes that impossible. */
    int s_canvas_w, s_canvas_h;
    if (!buf || !surf_geom(&s_canvas_w, &s_canvas_h))
        return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > s_canvas_w)
        x1 = s_canvas_w;
    if (y1 > s_canvas_h)
        y1 = s_canvas_h;
    if (fill_cli && x1 - x0 > 0 && y1 - y0 > 0 &&
        (x1 - x0) * (y1 - y0) >= UI_PPA_FILL_MIN_PX) {
        ppa_fill_oper_config_t op = {};
        op.out.buffer = buf;
        op.out.buffer_size =
            ((size_t)s_canvas_w * s_canvas_h * 2 + 63) & ~(size_t)63;
        op.out.pic_w = (uint32_t)s_canvas_w;
        op.out.pic_h = (uint32_t)s_canvas_h;
        op.out.block_offset_x = (uint32_t)x0;
        op.out.block_offset_y = (uint32_t)y0;
        op.out.fill_cm = PPA_FILL_COLOR_MODE_RGB565;
        op.fill_block_w = (uint32_t)(x1 - x0);
        op.fill_block_h = (uint32_t)(y1 - y0);
        /* 565 -> 888 by zero-extend; PPA truncates back, lossless */
        op.fill_argb_color.a = 0xFF;
        op.fill_argb_color.r = (uint32_t)(((px >> 11) & 0x1F) << 3);
        op.fill_argb_color.g = (uint32_t)(((px >> 5) & 0x3F) << 2);
        op.fill_argb_color.b = (uint32_t)((px & 0x1F) << 3);
        op.mode = PPA_TRANS_MODE_BLOCKING;
        if (ppa_do_fill(fill_cli, &op) == ESP_OK)
            return;
        /* any PPA error: fall through to the CPU loop */
    }
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            buf[(size_t)yy * s_canvas_w + xx] = px;
}

/* Blit one monospace glyph, fg blended over whatever bg is already in
   the cell, clipped to its cell box — one cell wide, two for a
   width-2 codepoint (no lv_draw_label, no layer).
   `run_right` is the run's right edge in pixels: the CPU path has to
   honour it for the same reason the PPA path clamps to the A8 buffer
   (see compose_glyph_a8). Without it the two paths disagree for a
   width-2 codepoint that ends a run — PPA shears the glyph, the CPU
   paints its right half into the next column, which the bg fill never
   cleared, so half a kanji survives until that row is redrawn. Same
   run, two different pictures depending only on the run's length
   (UI_PPA_CELLS_MIN_CELLS).
   Font access is READ-ONLY and reentrant: lv_font_get_glyph_dsc only
   writes the caller's dsc (lv_font.c:90), and fmt_txt's own
   get_glyph_dsc/get_glyph_bitmap read const tables and return a
   pointer into the const bitmap when req_raw_bitmap is set
   (lv_font_fmt_txt.c:100,222). There is no glyph cache to race over,
   which is what makes calling this off the LVGL task legal. */
static void surf_blit_glyph(uint16_t *buf, int cx0, int cy0, uint32_t cp,
                            uint16_t fg, int run_right)
{
    int s_canvas_w, s_canvas_h; /* shadowed on purpose, see surf_fill_rect */
    if (!surf_geom(&s_canvas_w, &s_canvas_h))
        return;
    const lv_font_t *font = &font_term_mono;
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, cp, 0))
        return;
    if (g.box_w == 0 || g.box_h == 0) /* space etc. */
        return;
    /* Get the raw A4 bitmap and decode it ourselves. NOTE: the public
       lv_font_get_glyph_bitmap() force-resets req_raw_bitmap to 0
       (decoding to A8 into a draw_buf we don't pass -> NULL deref
       crash). Call the font's method directly so req_raw_bitmap=1
       sticks and we get the raw glyph_bitmap pointer. Packing is a
       continuous 4bpp bitstream, MSB nibble first (host-verified in
       fonts/decode_test.py). */
    const lv_font_t *rf = g.resolved_font ? g.resolved_font : font;
    if (!rf->get_glyph_bitmap)
        return;
    g.req_raw_bitmap = 1;
    const uint8_t *bmp = (const uint8_t *)rf->get_glyph_bitmap(&g, NULL);
    if (!bmp)
        return;
    /* fmt_txt 4bpp: rows are padded to `stride` bytes if stride>0, else
       the glyph is a continuous bitstream (no per-row padding). */
    const int bpp = 4;
    int baseline = cy0 + (UI_CELL_H - font->base_line);
    int gx0 = cx0 + g.ofs_x;
    int gy0 = baseline - g.ofs_y - g.box_h;
    int clipR = cx0 + cells_glyph_cols(cp) * UI_CELL_W;
    if (clipR > run_right) {
        clipR = run_right;
        cells_note_clip();
    }
    int clipB = cy0 + UI_CELL_H;
    for (int py = 0; py < g.box_h; py++) {
        int y = gy0 + py;
        if (y < cy0 || y >= clipB || y < 0 || y >= s_canvas_h)
            continue;
        int rowbit = g.stride ? py * g.stride * 8 : py * g.box_w * bpp;
        for (int px = 0; px < g.box_w; px++) {
            int x = gx0 + px;
            if (x < cx0 || x >= clipR || x < 0 || x >= s_canvas_w)
                continue;
            int bitpos = rowbit + px * bpp;
            uint8_t byte = bmp[bitpos >> 3];
            int a = (bitpos & 4) ? (byte & 0x0F) : (byte >> 4);
            if (!a)
                continue;
            size_t idx = (size_t)y * s_canvas_w + x;
            buf[idx] = a == 15 ? fg : ui_blend565(buf[idx], fg, a);
        }
    }
}

/* Compose one glyph's coverage into the run's A8 buffer (no canvas
   access, no blending — just alpha bytes). Mirrors surf_blit_glyph's
   positioning/clipping; a4*17 so 15 -> 255 = fully opaque. */
static void compose_glyph_a8(uint8_t *dst, int dst_w, int cx0, uint32_t cp)
{
    const lv_font_t *font = &font_term_mono;
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, cp, 0))
        return;
    if (g.box_w == 0 || g.box_h == 0)
        return;
    const lv_font_t *rf = g.resolved_font ? g.resolved_font : font;
    if (!rf->get_glyph_bitmap)
        return;
    g.req_raw_bitmap = 1;
    const uint8_t *bmp = (const uint8_t *)rf->get_glyph_bitmap(&g, NULL);
    if (!bmp)
        return;
    const int bpp = 4;
    int baseline = UI_CELL_H - font->base_line;
    int gx0 = cx0 + g.ofs_x;
    int gy0 = baseline - g.ofs_y - g.box_h;
    int clipL = cx0 < 0 ? 0 : cx0;
    int clipR = cx0 + cells_glyph_cols(cp) * UI_CELL_W;
    /* Clamp to the A8 buffer, i.e. to the run's right edge. This is
       NOT just a last line of defence: surf_cells's straddle guard only
       runs inside the `while (n > seg_max)` splitting loop, so every
       run of <= seg_max cells (which is nearly every run ssh_vt's
       drawRow emits) and the final segment of a long one can still
       END on a width-2 codepoint — a selection dragged to the left
       half of a full-width character does exactly that. The CPU path
       clamps to the same edge (surf_blit_glyph's run_right) so the two
       paths agree; without that they diverge on run length alone. */
    if (clipR > dst_w) {
        clipR = dst_w;
        cells_note_clip();
    }
    for (int py = 0; py < g.box_h; py++) {
        int y = gy0 + py;
        if (y < 0 || y >= UI_CELL_H)
            continue;
        int rowbit = g.stride ? py * g.stride * 8 : py * g.box_w * bpp;
        uint8_t *drow = dst + (size_t)y * dst_w;
        for (int px = 0; px < g.box_w; px++) {
            int x = gx0 + px;
            if (x < clipL || x >= clipR)
                continue;
            int bitpos = rowbit + px * bpp;
            uint8_t byte = bmp[bitpos >> 3];
            int a = (bitpos & 4) ? (byte & 0x0F) : (byte >> 4);
            if (a)
                drow[x] = (uint8_t)(a * 17);
        }
    }
}

/* One segment of at most `a8_len / (UI_CELL_W*UI_CELL_H)` cells: PPA
   compose+blend when it qualifies, else per-glyph CPU blit. `e` bounds
   the UTF-8 walk (nullptr = walk to the NUL), `n` bounds it in CELLS —
   both are needed: the ui.cells path knows only the NUL, the native
   path knows only the length, and a caller may own more columns than
   it supplied codepoints for. */
static void surf_cells_run(uint16_t *buf, int col, int row, const uint8_t *s,
                           const uint8_t *e, int n, uint16_t fg, uint8_t *a8,
                           size_t a8_len, ppa_client_handle_t blend_cli)
{
    int s_canvas_w, s_canvas_h; /* shadowed on purpose, see surf_fill_rect */
    if (!surf_geom(&s_canvas_w, &s_canvas_h))
        return;
    int x0 = col * UI_CELL_W, y0 = row * UI_CELL_H, bw = n * UI_CELL_W;
    /* The PPA's cache maintenance rounds the A8 window up to the cache
       line, so the staging buffer must start on one and have room for
       the rounded-up size. s_cells_a8 (aligned(64), 720*24 = 270 lines
       exactly) satisfies both, which is why this check does not change
       what the ui.cells path does. */
    size_t need = ((size_t)bw * UI_CELL_H + 63) & ~(size_t)63;

    if (blend_cli && a8 && ((uintptr_t)a8 & 63) == 0 && need <= a8_len &&
        n >= UI_PPA_CELLS_MIN_CELLS && x0 >= 0 && y0 >= 0 &&
        x0 + bw <= s_canvas_w && y0 + UI_CELL_H <= s_canvas_h) {
        memset(a8, 0, (size_t)bw * UI_CELL_H);
        const uint8_t *p = s;
        for (int c = 0; c < n && (e ? p < e : *p != 0); c++)
            compose_glyph_a8(a8, bw, c * UI_CELL_W, cells_utf8_next(p, e));
        ppa_blend_oper_config_t op = {};
        op.in_bg.buffer = buf;
        op.in_bg.pic_w = (uint32_t)s_canvas_w;
        op.in_bg.pic_h = (uint32_t)s_canvas_h;
        op.in_bg.block_w = (uint32_t)bw;
        op.in_bg.block_h = UI_CELL_H;
        op.in_bg.block_offset_x = (uint32_t)x0;
        op.in_bg.block_offset_y = (uint32_t)y0;
        op.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
        op.in_fg.buffer = a8;
        op.in_fg.pic_w = (uint32_t)bw;
        op.in_fg.pic_h = UI_CELL_H;
        op.in_fg.block_w = (uint32_t)bw;
        op.in_fg.block_h = UI_CELL_H;
        op.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_A8;
        op.out.buffer = buf;
        op.out.buffer_size =
            ((size_t)s_canvas_w * s_canvas_h * 2 + 63) & ~(size_t)63;
        op.out.pic_w = (uint32_t)s_canvas_w;
        op.out.pic_h = (uint32_t)s_canvas_h;
        op.out.block_offset_x = (uint32_t)x0;
        op.out.block_offset_y = (uint32_t)y0;
        op.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
        op.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
        op.bg_alpha_fix_val = 255;
        op.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
        op.fg_fix_rgb_val.r = (uint32_t)(((fg >> 11) & 0x1F) << 3);
        op.fg_fix_rgb_val.g = (uint32_t)(((fg >> 5) & 0x3F) << 2);
        op.fg_fix_rgb_val.b = (uint32_t)((fg & 0x1F) << 3);
        op.mode = PPA_TRANS_MODE_BLOCKING;
        if (ppa_do_blend(blend_cli, &op) == ESP_OK)
            return;
        /* any PPA error: fall through to the per-glyph CPU path */
    }

    int c = col;
    int run_right = (col + n) * UI_CELL_W;
    while (c - col < n && (e ? s < e : *s != 0)) {
        uint32_t cp = cells_utf8_next(s, e);
        surf_blit_glyph(buf, c * UI_CELL_W, row * UI_CELL_H, cp, fg,
                        run_right);
        c++;
    }
}

/* §6's "1 本線描画": underline and strike-through, the only two SGR
   attributes the cell contract does not fold into fg/bg. One fill per
   rule for the WHOLE run — not per cell — so a fully underlined
   142-column row costs two rect writes, and a run with no attributes
   costs one compare. Drawn after the glyphs so the rule sits on top of
   a descender rather than under it, which is what a terminal looks
   like. */
static void surf_cells_rules(uint16_t *buf, int col, int row, int n,
                             uint16_t fg, unsigned attrs,
                             ppa_client_handle_t fill_cli)
{
    if (!(attrs & (UI_CELL_ATTR_UNDERLINE | UI_CELL_ATTR_STRIKE)) || n <= 0)
        return;
    int y0 = row * UI_CELL_H;
    int x0 = col * UI_CELL_W, w = n * UI_CELL_W;
    if (attrs & UI_CELL_ATTR_UNDERLINE) {
        /* One pixel below the baseline (surf_blit_glyph's own baseline
           arithmetic), kept inside the cell so consecutive underlined
           rows never touch. */
        int y = y0 + (UI_CELL_H - (int)font_term_mono.base_line) + 1;
        if (y > y0 + UI_CELL_H - 1)
            y = y0 + UI_CELL_H - 1;
        surf_fill_rect(buf, x0, y, w, 1, fg, fill_cli);
    }
    if (attrs & UI_CELL_ATTR_STRIKE)
        surf_fill_rect(buf, x0, y0 + UI_CELL_H / 2, w, 1, fg, fill_cli);
}

/* Draw a run of cells (one fg/bg) starting at (col,row) using the
   monospace grid font. UTF-8 decoded to codepoints. Long runs that sit
   fully on the grid go compose-then-one-PPA-blend; short runs and any
   PPA failure use the per-glyph CPU blit. Runs wider than the A8
   staging buffer are split into segments so the PPA path keeps winning
   instead of falling back to the CPU wholesale.
   `ncells` <= 0 means "count the columns from the text" (the ui.cells
   contract: one codepoint per column, wide or not). A caller that
   passes a positive ncells OWNS that many columns: the bg fill covers
   all of them, and columns past the end of the text stay bg — that is
   how a presenter clears the tail of a row in the same call. */
static void surf_cells(uint16_t *buf, int col, int row, const uint8_t *s,
                       const uint8_t *e, int ncells, uint16_t fg, uint16_t bg,
                       unsigned attrs, uint8_t *a8, size_t a8_len,
                       ppa_client_handle_t blend_cli,
                       ppa_client_handle_t fill_cli)
{
    if (!buf || !s)
        return;
    int n = ncells;
    if (n <= 0) {
        /* Columns == codepoints (the CONT contract at the top of this
           file), so counting UTF-8 lead bytes gives the run's width.
           Deliberately NOT sum(ui_cell_width): the caller already spent
           a column on the filler cell, and counting the wide glyph's
           second column here as well would overshoot the fill by one
           cell per CJK character and eat the head of the next run. */
        n = 0;
        for (const uint8_t *p = s; (e ? p < e : *p != 0); p++)
            if ((*p & 0xC0) != 0x80)
                n++;
    }
    if (n <= 0)
        return;
    /* the run has a single bg: clear it in one rect (instead of one
       9x24 fill per cell) — row-contiguous, and long runs clear the
       PPA threshold */
    surf_fill_rect(buf, col * UI_CELL_W, row * UI_CELL_H, n * UI_CELL_W,
                   UI_CELL_H, bg, fill_cli);
    const int run_col = col, run_cells = n;

    int seg_max = (int)(a8_len / ((size_t)UI_CELL_W * UI_CELL_H));
    if (!a8 || !blend_cli || seg_max < UI_PPA_CELLS_MIN_CELLS)
        seg_max = n; /* no usable staging: one CPU run, never split */
    while (n > seg_max) {
        /* Fill a segment, but never let one END on a width-2 glyph:
           its right half belongs to the next segment's first cell,
           which is outside the A8 buffer, so compose_glyph_a8 would
           shear it — and only on the PPA path, since the CPU blit
           writes straight to the canvas and would not. Hand the
           straddling glyph to the next segment instead. */
        const uint8_t *p = s;
        int take = 0;
        while (take < seg_max && (e ? p < e : *p != 0)) {
            const uint8_t *q = p;
            if (take + cells_glyph_cols(cells_utf8_next(p, e)) > seg_max) {
                p = q; /* rewind over the straddling glyph */
                break;
            }
            take++; /* one column per codepoint, wide or not */
        }
        if (take == 0) {
            if (e ? p >= e : *p == 0)
                break; /* text ran out early: the rest stays bg */
            cells_utf8_next(p, e); /* one glyph wider than a whole
                                      segment; no stuck loop */
            take = 1;
        }
        surf_cells_run(buf, col, row, s, p, take, fg, a8, a8_len, blend_cli);
        s = p;
        col += take;
        n -= take;
    }
    surf_cells_run(buf, col, row, s, e, n, fg, a8, a8_len, blend_cli);
    surf_cells_rules(buf, run_col, row, run_cells, fg, attrs, fill_cli);
}

