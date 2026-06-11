/*
 * TEMPORARY boot-time benchmark: CPU pixel loops vs PPA on a PSRAM
 * RGB565 canvas matching ui_tab5's (720x1280). The CPU side replicates
 * ui_tab5.cpp's fill_rect / blit_glyph inner loops (A4 bitstream decode
 * + ui_blend565); the PPA side uses ppa_do_fill and ppa_do_blend with
 * an A4 foreground + fixed RGB color — exactly the shape a PPA-backed
 * cell renderer would use. Called once from app_main before the display
 * comes up (quiet system); remove the call when done measuring.
 */
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ppa.h"

static const char *TAG = "ppa_bench";

#define BW 720
#define BH 1280
#define CANVAS_BYTES ((size_t)BW * BH * 2)

static uint16_t *s_canvas;
static ppa_client_handle_t s_fill_client, s_blend_client;

/* === CPU reference: byte-for-byte the ui_tab5.cpp primitives ========= */

static inline uint16_t blend565(uint16_t bg, uint16_t fg, int a)
{
    int br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    int fr = (fg >> 11) & 0x1F, fgc = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    int r = br + ((fr - br) * a) / 15;
    int g = bgc + ((fgc - bgc) * a) / 15;
    int b = bb + ((fb - bb) * a) / 15;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void cpu_fill_rect(int x, int y, int w, int h, uint16_t px)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > BW)
        x1 = BW;
    if (y1 > BH)
        y1 = BH;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            s_canvas[(size_t)yy * BW + xx] = px;
}

/* blit_glyph's inner loop: continuous A4 bitstream, MSB nibble first */
static void cpu_blend_a4(const uint8_t *bmp, int bw, int bh, int x0, int y0,
                         uint16_t fg)
{
    for (int py = 0; py < bh; py++) {
        int y = y0 + py;
        if (y < 0 || y >= BH)
            continue;
        int rowbit = py * bw * 4;
        for (int px = 0; px < bw; px++) {
            int x = x0 + px;
            if (x < 0 || x >= BW)
                continue;
            int bitpos = rowbit + px * 4;
            uint8_t byte = bmp[bitpos >> 3];
            int a = (bitpos & 4) ? (byte & 0x0F) : (byte >> 4);
            if (!a)
                continue;
            size_t idx = (size_t)y * BW + x;
            s_canvas[idx] = a == 15 ? fg : blend565(s_canvas[idx], fg, a);
        }
    }
}

/* 32-bit stores, two pixels each — the "cheap portable fix" variant.
   Full-width rows only (contiguous run, even pixel count). */
static void cpu32_fill_rows(int y, int h, uint16_t px)
{
    uint32_t v = ((uint32_t)px << 16) | px;
    uint32_t *d = (uint32_t *)(s_canvas + (size_t)y * BW);
    size_t n = (size_t)BW * h / 2;
    for (size_t i = 0; i < n; i++)
        d[i] = v;
}

/* PIE 128-bit stores (xespv2p1, 8 px per store; the toolchain already
   targets the extension — see build_tab5/toolchain/cflags). Needs a
   16B-aligned contiguous run, byte count a multiple of 16: full-width
   rows qualify (row = 1440B, canvas 64B-aligned). Pattern is splatted
   via a 16B aligned scratch + esp.vld so only vld/vst mnemonics are
   needed. q0 is ours alone: nothing else in this firmware uses PIE. */
static void pie_fill_rows(int y, int h, uint16_t px)
{
    uint16_t pat[8] __attribute__((aligned(16))) = { px, px, px, px,
                                                     px, px, px, px };
    uint8_t *d = (uint8_t *)(s_canvas + (size_t)y * BW);
    size_t bytes = (size_t)BW * h * 2;
    const uint16_t *p = pat;
    asm volatile("esp.vld.128.ip q0, %[p], 0\n"
                 "1:\n"
                 "esp.vst.128.ip q0, %[d], 16\n"
                 "addi %[n], %[n], -16\n"
                 "bnez %[n], 1b\n"
                 : [d] "+r"(d), [n] "+r"(bytes), [p] "+r"(p)
                 :
                 : "memory");
}

/* === PPA side ======================================================== */

static void ppa_fill_rect(int x, int y, int w, int h, uint16_t px)
{
    ppa_fill_oper_config_t op = {
        .out = {
            .buffer = s_canvas,
            .buffer_size = CANVAS_BYTES,
            .pic_w = BW,
            .pic_h = BH,
            .block_offset_x = (uint32_t)x,
            .block_offset_y = (uint32_t)y,
            .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w = (uint32_t)w,
        .fill_block_h = (uint32_t)h,
        .fill_argb_color = {
            .a = 255,
            .r = (uint32_t)(((px >> 11) & 0x1F) << 3),
            .g = (uint32_t)(((px >> 5) & 0x3F) << 2),
            .b = (uint32_t)((px & 0x1F) << 3),
        },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ESP_ERROR_CHECK(ppa_do_fill(s_fill_client, &op));
}

/* A4 fg (pic pw x ph, blend the w x h block at its origin) over the
   canvas at (x,y), fixed fg color, in place */
static void ppa_blend_a4(const uint8_t *fgbuf, int pw, int ph, int x, int y,
                         int w, int h, uint16_t fg)
{
    ppa_blend_oper_config_t op = {
        .in_bg = {
            .buffer = s_canvas,
            .pic_w = BW,
            .pic_h = BH,
            .block_w = (uint32_t)w,
            .block_h = (uint32_t)h,
            .block_offset_x = (uint32_t)x,
            .block_offset_y = (uint32_t)y,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer = fgbuf,
            .pic_w = (uint32_t)pw,
            .pic_h = (uint32_t)ph,
            .block_w = (uint32_t)w,
            .block_h = (uint32_t)h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .blend_cm = PPA_BLEND_COLOR_MODE_A4,
        },
        .out = {
            .buffer = s_canvas,
            .buffer_size = CANVAS_BYTES,
            .pic_w = BW,
            .pic_h = BH,
            .block_offset_x = (uint32_t)x,
            .block_offset_y = (uint32_t)y,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = 255,
        .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_fix_rgb_val = {
            .r = (uint32_t)(((fg >> 11) & 0x1F) << 3),
            .g = (uint32_t)(((fg >> 5) & 0x3F) << 2),
            .b = (uint32_t)((fg & 0x1F) << 3),
        },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ESP_ERROR_CHECK(ppa_do_blend(s_blend_client, &op));
}

/* === harness ========================================================= */

static void report(const char *name, int reps, int64_t us, int w, int h)
{
    double per = (double)us / reps;
    double mpix = ((double)w * h * reps) / (double)us; /* px/us == Mpix/s */
    ESP_LOGI(TAG, "%-22s %5dx %8.1f us/op %8.1f Mpix/s", name, reps, per,
             mpix);
}

/* walk positions so the working set doesn't stay L2-resident (the canvas
   is 1.8MB vs 128KB L2 — same as real cell rendering all over the screen) */
static inline int pos_x(int i, int w)
{
    return (int)(((uint32_t)i * 72u) % (uint32_t)(BW - w)) & ~1;
}
static inline int pos_y(int i, int h)
{
    return (int)(((uint32_t)i * 53u) % (uint32_t)(BH - h));
}

void ppa_bench_run(void)
{
    ESP_LOGI(TAG, "=== PPA vs CPU benchmark (canvas %dx%d RGB565, PSRAM) ===",
             BW, BH);

    s_canvas = heap_caps_aligned_alloc(64, CANVAS_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_canvas) {
        ESP_LOGE(TAG, "canvas alloc failed");
        return;
    }
    memset(s_canvas, 0, CANVAS_BYTES);

    ppa_client_config_t fill_cfg = { .oper_type = PPA_OPERATION_FILL };
    ppa_client_config_t blend_cfg = { .oper_type = PPA_OPERATION_BLEND };
    ESP_ERROR_CHECK(ppa_register_client(&fill_cfg, &s_fill_client));
    ESP_ERROR_CHECK(ppa_register_client(&blend_cfg, &s_blend_client));

    /* synthetic A4 glyph bitmaps (MSB nibble first, continuous rows like
       the font bitstream): alpha = (px+py)&0xF — a mix of skip / blend /
       opaque paths, same branch profile blit_glyph sees */
    enum { GW = 16, GH = 24 };           /* glyph-sized   */
    enum { RW = BW, RH = 24 };           /* one text row  */
    static uint8_t glyph_a4[GW * GH / 2];
    uint8_t *row_a4 = heap_caps_aligned_alloc(64, RW * RH / 2,
                                              MALLOC_CAP_SPIRAM);
    uint8_t *full_a4 = heap_caps_aligned_alloc(64, (size_t)BW * BH / 2,
                                               MALLOC_CAP_SPIRAM);
    if (!row_a4 || !full_a4) {
        ESP_LOGE(TAG, "a4 alloc failed");
        return;
    }
    for (int py = 0; py < GH; py++)
        for (int px = 0; px < GW; px += 2)
            glyph_a4[(py * GW + px) / 2] =
                (uint8_t)((((px + py) & 0xF) << 4) | ((px + 1 + py) & 0xF));
    for (int py = 0; py < RH; py++)
        for (int px = 0; px < RW; px += 2)
            row_a4[(py * RW + px) / 2] =
                (uint8_t)((((px + py) & 0xF) << 4) | ((px + 1 + py) & 0xF));
    for (int py = 0; py < BH; py++)
        for (int px = 0; px < BW; px += 2)
            full_a4[((size_t)py * BW + px) / 2] =
                (uint8_t)((((px + py) & 0xF) << 4) | ((px + 1 + py) & 0xF));

    int64_t t;
    const uint16_t C1 = 0x07E0, C2 = 0xF800, FG = 0xFFFF;

    /* --- sanity: PPA fill visible to CPU reads (cache invalidation) --- */
    ppa_fill_rect(64, 64, 32, 32, 0x1234);
    bool ok = s_canvas[(size_t)70 * BW + 70] == 0x1234;
    /* and PPA reads see CPU writes (cache writeback by the driver) */
    cpu_fill_rect(64, 64, 32, 32, 0x4321);
    ppa_blend_a4(glyph_a4, GW, GH, 64, 64, GW, GH, FG);
    bool ok2 = s_canvas[(size_t)70 * BW + 70] != 0x4321 ||
               s_canvas[(size_t)65 * BW + 65] != 0x4321;
    ESP_LOGI(TAG, "coherency: ppa->cpu %s, cpu->ppa %s", ok ? "OK" : "FAIL",
             ok2 ? "OK" : "FAIL");

    /* sanity: PIE fill writes what we think (alignment / pattern) */
    pie_fill_rows(100, 1, 0xA5A5);
    ESP_LOGI(TAG, "pie fill check: %s",
             (s_canvas[(size_t)100 * BW] == 0xA5A5 &&
              s_canvas[(size_t)100 * BW + BW - 1] == 0xA5A5 &&
              s_canvas[(size_t)101 * BW] != 0xA5A5) ? "OK" : "FAIL");

    /* ------------------------------- FILL ----------------------------- */
    int reps = 10;
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        cpu_fill_rect(0, 0, BW, BH, C1 + (uint16_t)i);
    report("fill full   CPU16", reps, esp_timer_get_time() - t, BW, BH);
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        cpu32_fill_rows(0, BH, C1 + (uint16_t)i);
    report("fill full   CPU32", reps, esp_timer_get_time() - t, BW, BH);
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        pie_fill_rows(0, BH, C1 + (uint16_t)i);
    report("fill full   PIE", reps, esp_timer_get_time() - t, BW, BH);
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        ppa_fill_rect(0, 0, BW, BH, C2 + (uint16_t)i);
    report("fill full   PPA", reps, esp_timer_get_time() - t, BW, BH);
    vTaskDelay(2);

    reps = 100;
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        cpu_fill_rect(0, pos_y(i, 50), BW, 50, C1);
    report("fill 720x50 CPU16", reps, esp_timer_get_time() - t, BW, 50);
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        cpu32_fill_rows(pos_y(i, 50), 50, C1);
    report("fill 720x50 CPU32", reps, esp_timer_get_time() - t, BW, 50);
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        pie_fill_rows(pos_y(i, 50), 50, C1);
    report("fill 720x50 PIE", reps, esp_timer_get_time() - t, BW, 50);
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        ppa_fill_rect(0, pos_y(i, 50), BW, 50, C2);
    report("fill 720x50 PPA", reps, esp_timer_get_time() - t, BW, 50);
    vTaskDelay(2);

    reps = 500;
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        cpu_fill_rect(pos_x(i, 100), pos_y(i, 100), 100, 100, C1);
    report("fill 100x100 CPU", reps, esp_timer_get_time() - t, 100, 100);
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        ppa_fill_rect(pos_x(i, 100), pos_y(i, 100), 100, 100, C2);
    report("fill 100x100 PPA", reps, esp_timer_get_time() - t, 100, 100);
    vTaskDelay(2);

    /* cell-background-sized: measures raw per-op overhead */
    reps = 2000;
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        cpu_fill_rect(pos_x(i, 10), pos_y(i, 24), 10, 24, C1);
    report("fill 10x24  CPU", reps, esp_timer_get_time() - t, 10, 24);
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        ppa_fill_rect(pos_x(i, 10), pos_y(i, 24), 10, 24, C2);
    report("fill 10x24  PPA", reps, esp_timer_get_time() - t, 10, 24);
    vTaskDelay(2);

    /* ------------------------------ BLEND ----------------------------- */
    reps = 2000;
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        cpu_blend_a4(glyph_a4, GW, GH, pos_x(i, GW), pos_y(i, GH), FG);
    report("blend 16x24 CPU", reps, esp_timer_get_time() - t, GW, GH);
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        ppa_blend_a4(glyph_a4, GW, GH, pos_x(i, GW), pos_y(i, GH), GW, GH, FG);
    report("blend 16x24 PPA", reps, esp_timer_get_time() - t, GW, GH);
    vTaskDelay(2);

    reps = 100;
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        cpu_blend_a4(row_a4, RW, RH, 0, pos_y(i, RH), FG);
    report("blend 720x24 CPU", reps, esp_timer_get_time() - t, RW, RH);
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        ppa_blend_a4(row_a4, RW, RH, 0, pos_y(i, RH), RW, RH, FG);
    report("blend 720x24 PPA", reps, esp_timer_get_time() - t, RW, RH);
    vTaskDelay(2);

    reps = 5;
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        cpu_blend_a4(full_a4, BW, BH, 0, 0, FG);
    report("blend full  CPU", reps, esp_timer_get_time() - t, BW, BH);
    vTaskDelay(2);
    t = esp_timer_get_time();
    for (int i = 0; i < reps; i++)
        ppa_blend_a4(full_a4, BW, BH, 0, 0, BW, BH, FG);
    report("blend full  PPA", reps, esp_timer_get_time() - t, BW, BH);

    ESP_LOGI(TAG, "=== done ===");

    ppa_unregister_client(s_fill_client);
    ppa_unregister_client(s_blend_client);
    heap_caps_free(full_a4);
    heap_caps_free(row_a4);
    heap_caps_free(s_canvas);
    s_canvas = NULL;
}

/* === CPU/PPA crossover search ======================================== */

/* blit a w x h block out of a pic_w-wide A4 picture (row stride follows
   the picture, the cost profile is the same as a standalone w-wide
   bitmap: only w pixels per row are read) */
static void cpu_blend_a4_pic(const uint8_t *bmp, int pic_w, int w, int h,
                             int x0, int y0, uint16_t fg)
{
    for (int py = 0; py < h; py++) {
        int y = y0 + py;
        if (y < 0 || y >= BH)
            continue;
        int rowbit = py * pic_w * 4;
        for (int px = 0; px < w; px++) {
            int x = x0 + px;
            if (x < 0 || x >= BW)
                continue;
            int bitpos = rowbit + px * 4;
            uint8_t byte = bmp[bitpos >> 3];
            int a = (bitpos & 4) ? (byte & 0x0F) : (byte >> 4);
            if (!a)
                continue;
            size_t idx = (size_t)y * BW + x;
            s_canvas[idx] = a == 15 ? fg : blend565(s_canvas[idx], fg, a);
        }
    }
}

/* Binary-search the block width (h = 24, one text-row height) where a
   PPA A4 blend starts beating the CPU loop. Assumes monotonicity, which
   the endpoint measurements (w=16: CPU wins, w=720: PPA 10x) support. */
void ppa_bench_crossover(void)
{
    enum { RW = BW, RH = 24, H = 24 };
    const uint16_t FG = 0xFFFF;

    ESP_LOGI(TAG, "=== blend crossover search (h=%d, w in [16,720]) ===", H);

    s_canvas = heap_caps_aligned_alloc(64, CANVAS_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *row_a4 = heap_caps_aligned_alloc(64, RW * RH / 2,
                                              MALLOC_CAP_SPIRAM);
    if (!s_canvas || !row_a4) {
        ESP_LOGE(TAG, "alloc failed");
        return;
    }
    memset(s_canvas, 0, CANVAS_BYTES);
    for (int py = 0; py < RH; py++)
        for (int px = 0; px < RW; px += 2)
            row_a4[(py * RW + px) / 2] =
                (uint8_t)((((px + py) & 0xF) << 4) | ((px + 1 + py) & 0xF));

    ppa_client_config_t blend_cfg = { .oper_type = PPA_OPERATION_BLEND };
    ESP_ERROR_CHECK(ppa_register_client(&blend_cfg, &s_blend_client));

    int lo = 16, hi = 720; /* invariant: CPU wins at lo, PPA wins at hi */
    while (hi - lo > 2) {
        int w = ((lo + hi) / 2) & ~1;

        int reps = 40000 / w;
        if (reps > 1000)
            reps = 1000;
        if (reps < 60)
            reps = 60;

        /* warmup both paths (first PPA trans after register is colder) */
        cpu_blend_a4_pic(row_a4, RW, w, H, 0, 0, FG);
        ppa_blend_a4(row_a4, RW, RH, 0, 0, w, H, FG);

        int64_t t = esp_timer_get_time();
        for (int i = 0; i < reps; i++)
            cpu_blend_a4_pic(row_a4, RW, w, H, 0, pos_y(i, H), FG);
        int64_t cpu_us = esp_timer_get_time() - t;

        t = esp_timer_get_time();
        for (int i = 0; i < reps; i++)
            ppa_blend_a4(row_a4, RW, RH, 0, pos_y(i, H), w, H, FG);
        int64_t ppa_us = esp_timer_get_time() - t;

        double cpu_per = (double)cpu_us / reps, ppa_per = (double)ppa_us / reps;
        bool ppa_wins = ppa_us < cpu_us;
        ESP_LOGI(TAG, "w=%3d (%5d px, %4dx): CPU %7.1f us  PPA %7.1f us  -> %s",
                 w, w * H, reps, cpu_per, ppa_per, ppa_wins ? "PPA" : "CPU");
        if (ppa_wins)
            hi = w;
        else
            lo = w;
        vTaskDelay(1);
    }
    ESP_LOGI(TAG, "crossover: CPU wins up to w=%d (%d px), PPA wins from w=%d (%d px)",
             lo, lo * H, hi, hi * H);
    ESP_LOGI(TAG, "=== done ===");

    ppa_unregister_client(s_blend_client);
    heap_caps_free(row_a4);
    heap_caps_free(s_canvas);
    s_canvas = NULL;
}
