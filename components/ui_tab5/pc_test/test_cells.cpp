/*
 * test_cells.cpp — ui_tab5_surf.inc のホストテスト。
 *
 * 何を見ているか
 *   run の分割 (seg_max ごと、幅 2 のグリフをまたがせない)、列数と
 *   バイト長の二重の境界、NUL 終端でない (ポインタ+長さ) 入力、
 *   ncells が本文より長いときの残りの扱い、そして **止まること**。
 *   ASAN/UBSAN の下で走らせるので、範囲外の読み書きはここで落ちる。
 *
 * 何を見ていないか (実機でしか分からない)
 *   PPA の実際の合成結果、キャッシュ整合、LVGL への提示、回転、
 *   2 タスク同時描画のちらつき、フォントの実グリフ。
 *   PPA の設定フィールド名は**この偽物の定義**に合わせてあるので、
 *   本物の driver/ppa.h との食い違いはここでは検出できない
 *   (代わりに「HEAD の動いているコードと 1 行ずつ同じ」ことを確認した)。
 *
 * テストは surf_cells_run の呼ばれ方を記録して見る。glyph 層は偽物で、
 * 「この列にこの符号位置を描いた」を配列に残すだけ。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

/* 本物のヘッダ。ui_cells_draw_t と UI_CELL_ATTR_* をここから取るので、
   下で定義する 3 本の入口の**宣言と定義の食い違い**がコンパイル時に出る。 */
#include "ui_tab5.h"

#define UI_CELL_W 9
#define UI_CELL_H 24
#define UI_PPA_FILL_MIN_PX 4096
#define UI_PPA_CELLS_MIN_CELLS 6

/* ---- 偽の PPA -------------------------------------------------------
   本物と同じ形の設定構造体だけを置く。ppa_do_* は g_ppa_ok で成否を
   切り替えられるようにして、PPA 経路と CPU 経路の**両方**を同じ入力で
   走らせる (2 つの経路が食い違うのがこのファイルの歴史的な事故)。 */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef struct ppa_client_t *ppa_client_handle_t;
typedef enum { PPA_TRANS_MODE_BLOCKING, PPA_TRANS_MODE_NON_BLOCKING } ppa_trans_mode_t;
typedef enum { PPA_FILL_COLOR_MODE_RGB565 } ppa_fill_color_mode_t;
typedef enum { PPA_BLEND_COLOR_MODE_RGB565, PPA_BLEND_COLOR_MODE_A8 } ppa_blend_cm_t;
typedef enum { PPA_ALPHA_NO_CHANGE, PPA_ALPHA_FIX_VALUE, PPA_ALPHA_SCALE } ppa_alpha_update_mode_t;
struct rgb888 { uint32_t r, g, b; };
struct argb8888 { uint32_t a, r, g, b; };
struct ppa_in_blk {
    const void *buffer;
    uint32_t pic_w, pic_h, block_w, block_h, block_offset_x, block_offset_y;
    int blend_cm;
};
struct ppa_out_blk {
    void *buffer;
    size_t buffer_size;
    uint32_t pic_w, pic_h, block_offset_x, block_offset_y;
    int fill_cm, blend_cm;
};
typedef struct {
    ppa_out_blk out;
    uint32_t fill_block_w, fill_block_h;
    argb8888 fill_argb_color;
    ppa_trans_mode_t mode;
} ppa_fill_oper_config_t;
typedef struct {
    ppa_in_blk in_bg, in_fg;
    ppa_out_blk out;
    ppa_alpha_update_mode_t bg_alpha_update_mode;
    uint32_t bg_alpha_fix_val;
    ppa_alpha_update_mode_t fg_alpha_update_mode;
    rgb888 fg_fix_rgb_val;
    ppa_trans_mode_t mode;
} ppa_blend_oper_config_t;

static bool g_ppa_ok = true;     /* false = ドライバが失敗した想定 */
static int  g_fills, g_blends;

/* 偽 PPA でも本物と同じだけの画素を触る: そうしないと ASAN が
   「PPA 経路なら範囲外だった」を見逃す。 */
static esp_err_t ppa_do_fill(ppa_client_handle_t, const ppa_fill_oper_config_t *op)
{
    if (!g_ppa_ok)
        return ESP_FAIL;
    g_fills++;
    uint16_t *b = (uint16_t *)op->out.buffer;
    uint16_t px = (uint16_t)(((op->fill_argb_color.r >> 3) << 11) |
                             ((op->fill_argb_color.g >> 2) << 5) |
                             (op->fill_argb_color.b >> 3));
    for (uint32_t y = 0; y < op->fill_block_h; y++)
        for (uint32_t x = 0; x < op->fill_block_w; x++)
            b[(size_t)(op->out.block_offset_y + y) * op->out.pic_w +
              op->out.block_offset_x + x] = px;
    return ESP_OK;
}

static esp_err_t ppa_do_blend(ppa_client_handle_t, const ppa_blend_oper_config_t *op)
{
    if (!g_ppa_ok)
        return ESP_FAIL;
    g_blends++;
    uint16_t *out = (uint16_t *)op->out.buffer;
    const uint8_t *fg = (const uint8_t *)op->in_fg.buffer;
    uint16_t fgpx = (uint16_t)(((op->fg_fix_rgb_val.r >> 3) << 11) |
                               ((op->fg_fix_rgb_val.g >> 2) << 5) |
                               (op->fg_fix_rgb_val.b >> 3));
    for (uint32_t y = 0; y < op->in_fg.block_h; y++)
        for (uint32_t x = 0; x < op->in_fg.block_w; x++) {
            uint8_t a = fg[(size_t)y * op->in_fg.pic_w + x];
            if (!a)
                continue;
            out[(size_t)(op->out.block_offset_y + y) * op->out.pic_w +
                op->out.block_offset_x + x] = fgpx;
        }
    return ESP_OK;
}

/* ---- 偽のフォント ---------------------------------------------------
   本物の lv_font_fmt_txt と同じ「4bpp の連続ビット列、stride=0」で、
   グリフはセルいっぱいの真っ黒。幅 2 の符号位置は box_w=18。 */
struct lv_font_t;
typedef struct {
    uint16_t adv_w;
    uint16_t box_w, box_h;
    int16_t ofs_x, ofs_y;
    uint16_t stride;
    uint8_t format;
    uint8_t is_placeholder;
    uint8_t req_raw_bitmap;
    const lv_font_t *resolved_font;
    const void *(*get_glyph_bitmap_unused)(void *, void *);
} lv_font_glyph_dsc_t;
struct lv_font_t {
    uint16_t base_line;
    const void *(*get_glyph_bitmap)(lv_font_glyph_dsc_t *, void *);
};

static uint8_t g_glyph_bits[18 * 24 * 4 / 8]; /* 全部 0xFF = alpha 15 */
static const void *fake_bitmap(lv_font_glyph_dsc_t *, void *)
{
    return g_glyph_bits;
}
static lv_font_t font_term_mono = { 5, fake_bitmap };

static uint32_t g_missing_cp; /* この符号位置だけ「フォントに無い」 */

static bool lv_font_get_glyph_dsc(const lv_font_t *f, lv_font_glyph_dsc_t *g,
                                  uint32_t cp, uint32_t)
{
    memset(g, 0, sizeof *g);
    if (cp == ' ' || cp == 0 || cp == g_missing_cp)
        return false;
    g->box_w = (uint16_t)(cp >= 0x3000 ? 18 : 9);
    g->box_h = 24;
    g->ofs_x = 0;
    g->ofs_y = 0;
    g->stride = 0;
    g->resolved_font = f;
    return true;
}

/* ---- ui_tab5.cpp から切り出した本物のヘルパ ---- */
#include "ui_cell_width.h"
#include "cellhelpers.inc"

static int g_clips;
static void cells_note_clip(void) { g_clips++; }

/* ---- キャンバス ---- */
static int s_canvas_w, s_canvas_h;
static size_t s_canvas_px;

#include "ui_tab5_surf.inc"

/* ---- 偽の LVGL オブジェクト層 (native surface の入口ぶん) ----------
   invalidate に渡された矩形と、ロックの取り方だけを記録する。
   lv_obj_get_coords と lv_obj_get_content_coords に **違う原点** を
   返させてあるのが肝で、padding の付いた日に「残像」で壊れるほうを
   使っていたらテストが落ちる。 */
struct lv_obj_t;
typedef struct { int32_t x1, y1, x2, y2; } lv_area_t;
typedef uint32_t lv_color_t;
#define LV_OBJ_FLAG_HIDDEN 1u

static int g_lock_depth, g_lock_max, g_unhides;
static int g_inval_n;
static lv_area_t g_inval;

static bool lvgl_port_lock(uint32_t)
{
    g_lock_depth++;
    if (g_lock_depth > g_lock_max)
        g_lock_max = g_lock_depth;
    return true;
}
static void lvgl_port_unlock(void) { g_lock_depth--; }
static void lv_obj_remove_flag(lv_obj_t *, uint32_t f)
{
    if (f == LV_OBJ_FLAG_HIDDEN)
        g_unhides++;
}
/* 本体が使ってはいけないほう: 原点がずれている。**呼ばれないのが正解**
   なので unused 属性を付けてある —— 呼ばれたかどうかは矩形の原点で見る
   (t_native_invalidate)。 */
__attribute__((unused))
static void lv_obj_get_coords(lv_obj_t *, lv_area_t *a)
{
    a->x1 = 100;
    a->y1 = 200;
    a->x2 = 100 + 1279;
    a->y2 = 200 + 631;
}
/* 本体が使うべきほう */
static void lv_obj_get_content_coords(lv_obj_t *, lv_area_t *a)
{
    a->x1 = 107;
    a->y1 = 211;
    a->x2 = 107 + 1279;
    a->y2 = 211 + 631;
}
static void lv_obj_invalidate_area(lv_obj_t *, const lv_area_t *a)
{
    g_inval = *a;
    g_inval_n++;
}
static lv_color_t lv_color_hex(uint32_t c) { return c; }
static uint16_t lv_color_to_u16(lv_color_t c)
{
    return (uint16_t)((((c >> 16) & 0xFF) >> 3) << 11 |
                      (((c >> 8) & 0xFF) >> 2) << 5 | ((c & 0xFF) >> 3));
}

static lv_obj_t *s_js_canvas;
static uint16_t *s_js_canvas_buf;
static ppa_client_handle_t s_ppa_fill_nat;
static bool s_prof_canvas_mark; /* 本体では提示メータ側が持っている */

#include "native.inc"

/* ------------------------------------------------------------------ */
static int g_fail;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

/* ガードつきキャンバス: 前後に見張りの画素を置いて、はみ出しを
   ASAN が拾わないビルドでも見つけられるようにする。 */
struct Canvas {
    std::vector<uint16_t> mem;
    uint16_t *buf;
    int w, h;
    Canvas(int W, int H) : mem((size_t)W * H + 2048, 0xDEAD), w(W), h(H)
    {
        buf = mem.data() + 1024;
        for (size_t i = 0; i < (size_t)W * H; i++)
            buf[i] = 0x0000;
        s_canvas_w = W;
        s_canvas_h = H;
        /* 実機と同じ: 確保は縦向きの大きさ 1 回きり、回転は寸法を
           入れ替えて同じ確保に貼り直す。 */
        s_canvas_px = (size_t)720 * 1192;
    }
    bool guards_intact() const
    {
        for (size_t i = 0; i < 1024; i++)
            if (mem[i] != 0xDEAD)
                return false;
        for (size_t i = mem.size() - 1024; i < mem.size(); i++)
            if (mem[i] != 0xDEAD)
                return false;
        return true;
    }
    uint16_t at(int x, int y) const { return buf[(size_t)y * w + x]; }
    /* このセルに何か描かれたか (bg 以外の画素があるか) */
    bool cell_painted(int col, int row, uint16_t bg) const
    {
        for (int y = row * UI_CELL_H; y < (row + 1) * UI_CELL_H; y++)
            for (int x = col * UI_CELL_W; x < (col + 1) * UI_CELL_W; x++)
                if (at(x, y) != bg)
                    return true;
        return false;
    }
};

/* 64B 整列のステージング (本物と同じ形)。80 セルぶん。 */
alignas(64) static uint8_t g_a8[720 * UI_CELL_H];
static ppa_client_handle_t FAKE = (ppa_client_handle_t)1;

static void reset(void)
{
    g_fills = g_blends = g_clips = 0;
    g_ppa_ok = true;
    g_missing_cp = 0;
    memset(g_glyph_bits, 0xFF, sizeof g_glyph_bits);
}

/* ---- 1. 長い ASCII の run が分割され、全列が塗られる -------------- */
static void t_split_ascii(void)
{
    printf("t_split_ascii\n");
    for (int ppa = 0; ppa < 2; ppa++) {
        reset();
        g_ppa_ok = ppa != 0;
        Canvas cv(1280, 632);
        std::string s(142, 'X');
        surf_cells(cv.buf, 0, 3, (const uint8_t *)s.c_str(), nullptr, 0,
                   0xFFFF, 0x0000, 0, g_a8, sizeof g_a8, FAKE, FAKE);
        CHECK(cv.guards_intact(), "canvas guard clobbered (ppa=%d)", ppa);
        for (int c = 0; c < 142; c++)
            CHECK(cv.cell_painted(c, 3, 0x0000), "col %d unpainted (ppa=%d)",
                  c, ppa);
        /* 1280 / 9 = 142.2 なので 142 列目は半端 (x=1278,1279)。
           そこが手つかずなら run の外は塗られていない。 */
        CHECK(cv.at(1278, 3 * UI_CELL_H) == 0x0000, "painted past the run");
        CHECK(g_clips == 0, "unexpected clip x%d", g_clips);
        if (ppa)
            CHECK(g_blends == 2, "expected 2 segments (80+62), got %d",
                  g_blends);
    }
}

/* ---- 2. 幅 2 のグリフが分割の境目をまたがない -------------------- */
static void t_split_wide(void)
{
    printf("t_split_wide\n");
    reset();
    Canvas cv(1280, 632);
    /* 「あ」(U+3042, 3 バイト) + CONT の空白の繰り返し。**先頭に ASCII を
       1 つ置く**のが肝で、そうすると「あ」が奇数列に寄り、seg_max の
       境目 (80 列目) がちょうど幅 2 グリフの左半分に当たる。
       ずらさずに書くと境目は必ず CONT に落ちて、このテストは
       straddle guard を一度も通らない (最初に書いたときそうだった)。 */
    std::string s = "A";
    for (int i = 0; i < 70; i++)
        s += "\xE3\x81\x82 ";
    s += "A";
    surf_cells(cv.buf, 0, 0, (const uint8_t *)s.c_str(), nullptr, 0, 0xFFFF,
               0x0000, 0, g_a8, sizeof g_a8, FAKE, FAKE);
    CHECK(cv.guards_intact(), "canvas guard clobbered");
    /* 分割で切られた幅 2 グリフが 1 つも無いこと = straddle guard が
       効いている。効いていなければ compose_glyph_a8 が clip を数える。 */
    CHECK(g_clips == 0, "a wide glyph was sheared at a segment boundary (x%d)",
          g_clips);
    CHECK(g_blends == 2, "expected 2 segments, got %d", g_blends);
    /* 「あ」の列 (1,3,5,...) と両端の ASCII が全部塗られていること。
       偶数列は CONT の空白なのでグリフは無い。 */
    for (int c = 0; c < 142; c++) {
        bool wide_head = (c % 2) == 1 && c < 141;
        if (c == 0 || c == 141 || wide_head)
            CHECK(cv.cell_painted(c, 0, 0x0000), "col %d unpainted", c);
    }
}

/* ---- 3. ポインタ+長さ: NUL 終端でない入力 ------------------------ */
static void t_len_bounded(void)
{
    printf("t_len_bounded\n");
    reset();
    Canvas cv(1280, 632);
    /* NUL の無いバッファ。末尾のすぐ後ろは 'Z' で埋めてあるので、
       長さを越えて読むと 'Z' が描かれてしまう。 */
    char raw[16];
    memset(raw, 'Z', sizeof raw);
    memcpy(raw, "abcde", 5);
    surf_cells(cv.buf, 0, 0, (const uint8_t *)raw, (const uint8_t *)raw + 5, 5,
               0xFFFF, 0x0000, 0, g_a8, sizeof g_a8, FAKE, FAKE);
    for (int c = 0; c < 5; c++)
        CHECK(cv.cell_painted(c, 0, 0x0000), "col %d unpainted", c);
    CHECK(!cv.cell_painted(5, 0, 0x0000), "read past the length bound");
    CHECK(cv.guards_intact(), "canvas guard clobbered");
}

/* ---- 4. 途中で切れた多バイト列を長さの外へ読まない ---------------- */
static void t_truncated_utf8(void)
{
    printf("t_truncated_utf8\n");
    reset();
    Canvas cv(1280, 632);
    /* "A" + U+3042 の先頭 2 バイトだけ。3 バイト目はバッファの外。
       ASAN 付きなら、境界を無視した読みはここで落ちる。 */
    std::vector<uint8_t> v = { 'A', 0xE3, 0x81 };
    uint8_t *heap = (uint8_t *)malloc(v.size());
    memcpy(heap, v.data(), v.size());
    surf_cells(cv.buf, 0, 0, heap, heap + v.size(), 2, 0xFFFF, 0x0000, 0,
               g_a8, sizeof g_a8, FAKE, FAKE);
    free(heap);
    CHECK(cv.guards_intact(), "canvas guard clobbered");
}

/* ---- 5. ncells が本文より長い: 残りは bg のまま、止まる ---------- */
static void t_ncells_longer_than_text(void)
{
    printf("t_ncells_longer_than_text\n");
    for (int ppa = 0; ppa < 2; ppa++) {
        reset();
        g_ppa_ok = ppa != 0;
        Canvas cv(1280, 632);
        for (size_t i = 0; i < (size_t)1280 * 632; i++)
            cv.buf[i] = 0x1234; /* 前の絵 */
        const char *txt = "hi";
        /* 142 列を所有し、本文は 2 文字。seg_max=80 なので分割ループにも
           入る —— ここが止まらないと実機はウォッチドッグで死ぬ。 */
        surf_cells(cv.buf, 0, 5, (const uint8_t *)txt,
                   (const uint8_t *)txt + 2, 142, 0xFFFF, 0x0000, 0, g_a8,
                   sizeof g_a8, FAKE, FAKE);
        CHECK(cv.guards_intact(), "canvas guard clobbered (ppa=%d)", ppa);
        CHECK(cv.at(10 * UI_CELL_W, 5 * UI_CELL_H) == 0x0000,
              "the owned tail was not cleared to bg (ppa=%d)", ppa);
        CHECK(cv.at(142 * UI_CELL_W, 5 * UI_CELL_H) == 0x1234,
              "painted past the owned columns (ppa=%d)", ppa);
        CHECK(cv.cell_painted(0, 5, 0x0000), "'h' missing (ppa=%d)", ppa);
        CHECK(!cv.cell_painted(2, 5, 0x0000),
              "something drawn past the text (ppa=%d)", ppa);
    }
}

/* ---- 5b. ncells が本文より短い: 所有していない列へはみ出さない ---
   ここが `c < n` の境界。バイト長 (e) だけで止めていると、呼び出し側が
   「6 列ぶんだけ」と言ったのに 10 文字ぶん塗ってしまう —— 隣の run の
   頭を食う、静かな壊れ方。PPA 経路と CPU 経路の両方で見る。 */
static void t_ncells_shorter_than_text(void)
{
    printf("t_ncells_shorter_than_text\n");
    for (int ppa = 0; ppa < 2; ppa++) {
        reset();
        g_ppa_ok = ppa != 0;
        Canvas cv(1280, 632);
        for (size_t i = 0; i < (size_t)1280 * 632; i++)
            cv.buf[i] = 0x1234; /* 前の絵 */
        const char *txt = "abcdefghij"; /* 10 文字、所有は 6 列 */
        surf_cells(cv.buf, 0, 4, (const uint8_t *)txt,
                   (const uint8_t *)txt + 10, 6, 0xFFFF, 0x0000, 0, g_a8,
                   sizeof g_a8, FAKE, FAKE);
        CHECK(cv.guards_intact(), "canvas guard clobbered (ppa=%d)", ppa);
        for (int c = 0; c < 6; c++)
            CHECK(cv.cell_painted(c, 4, 0x0000), "col %d unpainted (ppa=%d)",
                  c, ppa);
        /* 7 列目以降は所有していない: bg 塗りもグリフも来てはいけない */
        for (int x = 6 * UI_CELL_W; x < 10 * UI_CELL_W; x++)
            CHECK(cv.at(x, 4 * UI_CELL_H + 12) == 0x1234,
                  "painted col %d, which the run does not own (ppa=%d)",
                  x / UI_CELL_W, ppa);
        /* 画素だけ見ていると足りない: run_right (CPU) と dst_w (PPA) の
           clamp が、余分なグリフを **無音で** 捨ててしまうので、画面は
           正しいまま `c < n` の欠落を素通りする。clamp が働いた回数を
           見るとそれが分かる —— 正しいコードなら 1 回も起きない。 */
        CHECK(g_clips == 0,
              "glyphs past the owned columns reached the clamp (x%d, ppa=%d)",
              g_clips, ppa);
        if (ppa)
            CHECK(g_blends == 1, "expected one blend, got %d", g_blends);
    }
}

/* ---- 6. ステージングが無い / 小さい: CPU 1 本、無限ループ無し ---- */
static void t_no_staging(void)
{
    printf("t_no_staging\n");
    reset();
    Canvas cv(1280, 632);
    std::string s(142, 'W');
    surf_cells(cv.buf, 0, 1, (const uint8_t *)s.c_str(), nullptr, 0, 0xFFFF,
               0x0000, 0, nullptr, 0, FAKE, FAKE);
    CHECK(g_blends == 0, "blended without a staging buffer");
    for (int c = 0; c < 142; c++)
        CHECK(cv.cell_painted(c, 1, 0x0000), "col %d unpainted", c);

    /* ステージングが無いときに seg_max を n へ倒さないと、分割ループが
       1 セルずつ進む。1 セルの run では幅 2 グリフの run_right が 9px に
       なるので、CONT を正しく置いた呼び出し側でも clip が数えられる ——
       それが「run が刻まれた」ことの観測できる証拠になる。 */
    reset();
    Canvas cvw(1280, 632);
    surf_cells(cvw.buf, 0, 0, (const uint8_t *)"\xE3\x81\x82 ", nullptr, 0,
               0xFFFF, 0x0000, 0, nullptr, 0, FAKE, FAKE);
    CHECK(g_clips == 0, "the run was chopped into single cells (clips x%d)",
          g_clips);

    reset();
    Canvas cv2(1280, 632);
    /* 3 セルぶんしかない = UI_PPA_CELLS_MIN_CELLS 未満 */
    surf_cells(cv2.buf, 0, 1, (const uint8_t *)s.c_str(), nullptr, 0, 0xFFFF,
               0x0000, 0, g_a8, 3 * UI_CELL_W * UI_CELL_H, FAKE, FAKE);
    CHECK(g_blends == 0, "blended into a 3-cell staging buffer");
    for (int c = 0; c < 142; c++)
        CHECK(cv2.cell_painted(c, 1, 0x0000), "col %d unpainted (small a8)", c);
}

/* ---- 7. 整列していないステージングは PPA 経路に入らない ---------- */
static void t_unaligned_staging(void)
{
    printf("t_unaligned_staging\n");
    reset();
    Canvas cv(1280, 632);
    std::string s(80, 'A');
    surf_cells(cv.buf, 0, 2, (const uint8_t *)s.c_str(), nullptr, 0, 0xFFFF,
               0x0000, 0, g_a8 + 1, sizeof g_a8 - 1, FAKE, FAKE);
    CHECK(g_blends == 0, "blended with a misaligned A8 buffer");
    for (int c = 0; c < 80; c++)
        CHECK(cv.cell_painted(c, 2, 0x0000), "col %d unpainted", c);
}

/* ---- 8. 画面の外に置かれた run は画面を壊さない ------------------ */
static void t_offgrid(void)
{
    printf("t_offgrid\n");
    reset();
    Canvas cv(720, 1192);
    const char *txt = "hello";
    /* 最終行のさらに下、右端をまたぐ、負の列 */
    surf_cells(cv.buf, 78, 0, (const uint8_t *)txt, nullptr, 0, 0xFFFF,
               0x0000, 0, g_a8, sizeof g_a8, FAKE, FAKE);
    surf_cells(cv.buf, 0, 60, (const uint8_t *)txt, nullptr, 0, 0xFFFF,
               0x0000, 0, g_a8, sizeof g_a8, FAKE, FAKE);
    surf_cells(cv.buf, -3, 1, (const uint8_t *)txt, nullptr, 0, 0xFFFF,
               0x0000, 0, g_a8, sizeof g_a8, FAKE, FAKE);
    CHECK(cv.guards_intact(), "canvas guard clobbered by an off-grid run");
}

/* ---- 9. 下線と取り消し線は run 全体に 1 本ずつ -------------------- */
static void t_rules(void)
{
    printf("t_rules\n");
    reset();
    Canvas cv(1280, 632);
    const char *txt = "     "; /* 空白 = グリフ無し。線だけが残る */
    surf_cells(cv.buf, 0, 0, (const uint8_t *)txt, nullptr, 0, 0x07E0,
               0x0000, UI_CELL_ATTR_UNDERLINE | UI_CELL_ATTR_STRIKE, g_a8,
               sizeof g_a8, FAKE, FAKE);
    int painted_rows = 0;
    for (int y = 0; y < UI_CELL_H; y++)
        if (cv.at(0, y) != 0x0000)
            painted_rows++;
    CHECK(painted_rows == 2, "expected 2 rules, got %d rows", painted_rows);
    CHECK(cv.guards_intact(), "canvas guard clobbered");
}

/* ---- 10. run の末尾で幅 2 が切られたら数える -------------------- */
static void t_clip_counted(void)
{
    printf("t_clip_counted\n");
    reset();
    Canvas cv(1280, 632);
    /* CONT の詰め物を落とした呼び出し側: 6 列に 6 つの「あ」。
       末尾は必ず切られるので clip が数えられていなければならない
       (PPA 経路は A8 の右端、CPU 経路は run の右端で clip する)。 */
    std::string s;
    for (int i = 0; i < 6; i++)
        s += "\xE3\x81\x82";
    surf_cells(cv.buf, 0, 0, (const uint8_t *)s.c_str(), nullptr, 0, 0xFFFF,
               0x0000, 0, g_a8, sizeof g_a8, FAKE, FAKE);
    CHECK(g_clips > 0, "a sheared wide glyph went uncounted");
    CHECK(cv.guards_intact(), "canvas guard clobbered");
}

/* ---- 11. native surface の入口: ロックと矩形 --------------------- */
static void native_reset(Canvas &cv)
{
    reset();
    s_js_canvas = (lv_obj_t *)1;
    s_js_canvas_buf = cv.buf;
    s_ppa_fill_nat = FAKE;
    g_lock_depth = g_lock_max = g_unhides = g_inval_n = 0;
    s_prof_canvas_mark = false;
    memset(&g_inval, 0, sizeof g_inval);
}

static void t_native_invalidate(void)
{
    printf("t_native_invalidate\n");
    Canvas cv(1280, 632);
    native_reset(cv);

    /* 3 段 (row 2..4) をまとめて 1 回。content の原点は (107,211)。 */
    ui_tab5_canvas_invalidate(0, 2 * UI_CELL_H, 1280, 3 * UI_CELL_H);
    CHECK(g_inval_n == 1, "invalidates=%d", g_inval_n);
    CHECK(g_lock_max == 1 && g_lock_depth == 0, "lock max=%d depth=%d",
          g_lock_max, g_lock_depth);
    CHECK(g_unhides == 1, "canvas not un-hidden");
    CHECK(s_prof_canvas_mark, "the presentation was not marked as canvas");
    /* content 原点を使うこと。lv_obj_get_coords なら (100,200) になる。 */
    CHECK(g_inval.x1 == 107 && g_inval.y1 == 211 + 48,
          "origin (%d,%d) — content coords not used", g_inval.x1, g_inval.y1);
    /* x2/y2 は **内側** (inclusive) */
    CHECK(g_inval.x2 == 107 + 1279 && g_inval.y2 == 211 + 48 + 71,
          "bottom-right (%d,%d) is off by one", g_inval.x2, g_inval.y2);
}

static void t_native_invalidate_clamped(void)
{
    printf("t_native_invalidate_clamped\n");
    Canvas cv(1280, 632);

    /* 左上へはみ出す */
    native_reset(cv);
    ui_tab5_canvas_invalidate(-40, -10, 100, 50);
    CHECK(g_inval_n == 1, "invalidates=%d", g_inval_n);
    CHECK(g_inval.x1 == 107 && g_inval.y1 == 211, "not clamped to (0,0)");
    CHECK(g_inval.x2 == 107 + 59 && g_inval.y2 == 211 + 39,
          "clamped size wrong (%d,%d)", g_inval.x2 - 107, g_inval.y2 - 211);

    /* 右下へはみ出す */
    native_reset(cv);
    ui_tab5_canvas_invalidate(1200, 600, 500, 500);
    CHECK(g_inval.x2 == 107 + 1279 && g_inval.y2 == 211 + 631,
          "not clamped to the canvas edge (%d,%d)", g_inval.x2, g_inval.y2);

    /* まるごと外 / 空: ロックすら取らない */
    native_reset(cv);
    ui_tab5_canvas_invalidate(2000, 0, 10, 10);
    ui_tab5_canvas_invalidate(0, 0, 0, 10);
    ui_tab5_canvas_invalidate(0, 0, 10, -5);
    CHECK(g_inval_n == 0, "invalidated an empty area (%d)", g_inval_n);
    CHECK(g_lock_max == 0, "took the LVGL lock for nothing");
}

static void t_native_no_canvas(void)
{
    printf("t_native_no_canvas\n");
    Canvas cv(1280, 632);
    native_reset(cv);
    s_js_canvas = NULL;
    s_js_canvas_buf = NULL;

    ui_cells_draw_t d = {};
    d.col = 0;
    d.row = 0;
    d.ncells = 3;
    d.utf8 = "abc";
    d.len = 3;
    d.fg = 0xFFFFFF;
    d.a8 = g_a8;
    d.a8_len = sizeof g_a8;
    d.ppa = FAKE;
    CHECK(!ui_tab5_cells_draw(&d), "drew without a canvas");
    ui_tab5_canvas_fill(0, 0, 10, 10, 0x112233); /* must not crash */
    ui_tab5_canvas_invalidate(0, 0, 10, 10);
    CHECK(g_inval_n == 0, "invalidated without a canvas");
    CHECK(g_lock_max == 0, "locked without a canvas");
}

static void t_native_cells_draw(void)
{
    printf("t_native_cells_draw\n");
    Canvas cv(1280, 632);
    native_reset(cv);

    ui_cells_draw_t d = {};
    d.col = 4;
    d.row = 6;
    d.ncells = 8;
    d.utf8 = "hello";
    d.len = 5; /* 5 文字で 8 列を所有 = 残り 3 列は bg */
    d.fg = 0xFFFFFF;
    d.bg = 0x000000;
    d.a8 = g_a8;
    d.a8_len = sizeof g_a8;
    d.ppa = FAKE;
    CHECK(ui_tab5_cells_draw(&d), "cells_draw returned false");
    for (int c = 4; c < 9; c++)
        CHECK(cv.cell_painted(c, 6, 0x0000), "col %d unpainted", c);
    CHECK(!cv.cell_painted(10, 6, 0x0000), "painted past the text");
    CHECK(g_clips == 0, "unexpected clip x%d", g_clips);
    CHECK(g_inval_n == 0, "cells_draw invalidated by itself");
    CHECK(g_lock_max == 0, "cells_draw took the LVGL lock");

    /* 空の run と NULL は false、描かない */
    d.ncells = 0;
    CHECK(!ui_tab5_cells_draw(&d), "accepted ncells=0");
    CHECK(!ui_tab5_cells_draw(NULL), "accepted a NULL request");

    /* canvas_fill もロックを取らない */
    native_reset(cv);
    ui_tab5_canvas_fill(0, 0, 128, 128, 0xFF0000);
    CHECK(g_lock_max == 0, "canvas_fill took the LVGL lock");
    CHECK(cv.at(0, 0) == lv_color_to_u16(0xFF0000), "fill colour wrong");
    CHECK(cv.at(128, 0) == 0x0000, "filled past the rectangle");
}

/* ---- 12. 回転の途中で見えた「ありえない寸法」を描かない ----------
   native surface は LVGL ロックを取らずに描く。ui_tab5_set_landscape は
   幅と高さを順に書き換えるので、その隙間に読むと (1280, 1192) —— 確保
   された 858,240 画素に収まらない組 —— が見える。そのまま描くと
   1,525,759 番地、67 万画素ぶん外へ出る。 */
static void t_rotation_torn_geometry(void)
{
    printf("t_rotation_torn_geometry\n");
    Canvas cv(720, 1192); /* 縦向き = 確保そのもの */
    reset();
    /* 幅だけ landscape になった瞬間を作る */
    s_canvas_w = 1280;
    s_canvas_h = 1192;
    const char *txt = "abcdefgh";
    surf_cells(cv.buf, 0, 40, (const uint8_t *)txt, nullptr, 0, 0xFFFF,
               0x0000, 0, g_a8, sizeof g_a8, FAKE, FAKE);
    surf_fill_rect(cv.buf, 0, 1100, 1280, 80, 0xFFFF, FAKE);
    surf_blit_glyph(cv.buf, 0, 1100, 'X', 0xFFFF, 9);
    CHECK(cv.guards_intact(), "wrote outside the allocation");
    CHECK(g_fills == 0 && g_blends == 0,
          "handed an impossible geometry to the PPA (%d/%d)", g_fills,
          g_blends);
    /* 元に戻せば普通に描ける */
    s_canvas_w = 720;
    s_canvas_h = 1192;
    surf_cells(cv.buf, 0, 40, (const uint8_t *)txt, nullptr, 0, 0xFFFF,
               0x0000, 0, g_a8, sizeof g_a8, FAKE, FAKE);
    CHECK(cv.cell_painted(0, 40, 0x0000), "the guard never lets go");
}

int main(void)
{
    t_split_ascii();
    t_split_wide();
    t_len_bounded();
    t_truncated_utf8();
    t_ncells_longer_than_text();
    t_ncells_shorter_than_text();
    t_no_staging();
    t_unaligned_staging();
    t_offgrid();
    t_rules();
    t_clip_counted();
    t_native_invalidate();
    t_native_invalidate_clamped();
    t_native_no_canvas();
    t_native_cells_draw();
    t_rotation_torn_geometry();
    if (g_fail) {
        printf("FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("ok\n");
    return 0;
}
