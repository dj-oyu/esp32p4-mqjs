/*
 * edit_ui — edit_task とプレゼンタ。仕様 docs/native-editor-spec.md §A.2 / §A.3。
 *
 * ---- このファイルの住人は 1 タスクだけ ------------------------------
 *
 * `edit_task` (core 1, prio 5, stack 8 KB 内部 SRAM)。edit_core のインスタンス、
 * ime_t、ステージングバッファ、PPA クライアント、計測器 —— 可変状態は全部
 * このタスクだけが触る。だからロックが 1 つも要らない。
 *
 * 外から来るものは**すべてコマンドキュー経由**で、投げる側 (kbd タスク /
 * LVGL タスク / JS タスク / esp_timer タスク) は投げっぱなし。キューが満杯
 * なら落として数える —— ブロックしない。これは仕様 §A.2 の
 * 「JS タスクを待たない」「ppa_do_blend 以外でブロックしない」を、
 * 構造で守るための形。
 *
 * ---- edit_task の禁止事項 (§A.2。コードの規則) -----------------------
 *
 *   fs_* を呼ばない / esp_partition_*・NVS を呼ばない /
 *   lvgl_port_lock を ui_tab5_canvas_invalidate 以外で取らない /
 *   malloc しない / JS タスクを待たない / ppa_do_blend 以外でブロックしない
 *
 * このファイルは `#include "fs_core.h"` もしない。CMakeLists の REQUIRES に
 * fs_* が 1 つも無いことが、いちばん外側の見張りになっている —— 呼びたく
 * なった時点でビルドが落ちる。**その REQUIRES に fs_* を足したくなったら、
 * それは fs_io (§A.7) へ投げるべき仕事**。
 *
 * malloc は起動時 (edit_ui_start、= 呼び出し側のタスク) だけ。edit_core の
 * ブロックは PSRAM に固定上限で 1 回取り、以後は増えない。
 *
 * ---- フォアグラウンドに出た直後だけ、描画を数回くり返す理由 ----------
 *
 * `mqjs_native.h` の「落とし穴 1」がこう言っている: 切替では focus() の
 * 直前に **UI_CMD_RESET が UI キューへ post** され、それを掃くのは UI タスク
 * なので、focus() が返った時点で処理済みである保証は無い。RESET は
 * キャンバスを消したうえ **HIDDEN にもする**。
 *
 * つまり **こちらの最初の 1 枚は RESET に消されうる**。しかも消えたことを
 * 知る口が無い —— ui_tab5_cmd は投げっぱなしで、完了を待つ入口が存在しない。
 *
 * 採った形: フォーカス時は即座に 1 枚描き、そのあと EDIT_FIRST_PAINT_MS 間隔で
 * EDIT_FOCUS_REPAINTS 回だけ描き直す。同じ画素を上書きするだけなので
 * ちらつかず、代金は**アプリ切替のときにだけ**掛かる。UI タスクのキューは
 * LVGL の 1 周 (10〜20 ms) ごとに空になるので、この窓で足りる。
 *
 * 隠れたキャンバスを出すのはこちらの仕事で (同じ落とし穴 1)、
 * `ui_tab5_canvas_invalidate` がロックの内側で HIDDEN を落としてくれる。
 * **UI_CMD_CLEAR は投げない**: あれは UI タスクで非同期に fill_all するので、
 * こちらの直接描画を後から消す側に回る。背景は自分のタスクから
 * ui_tab5_canvas_fill で塗る。
 *
 * ---- スクロールを最適化していない理由 --------------------------------
 *
 * スクロールは全段 dirty になり、面積 invalidate では救えない (ssh_vt の
 * 実測で 22/26 段)。仕様 §A.3 は PPA の段ブリットを対策として挙げているが、
 * **Phase 1 では素直に全段描き直す**。今回の目的はその代金を測ることで、
 * 測る前に最適化すると「何が効いたか」が分からなくなる。
 *
 * 代わりに、差し替えられる形だけ用意した: 「全部描く」は render_all() 1 本に
 * 閉じてあり、EDIT_DIRTY_ALL も edit_scroll() の後も必ずそこを通る。
 * ブリット最適化はこの関数の中身だけを書き換える作業になる。
 */

#include "edit_ui.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_MQJS_TAB5_UI

#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ime_core.h"
#include "mqjs_native.h"
#include "ui_cell_width.h"
#include "ui_tab5.h"

static const char *TAG = "edit_ui";

/* ---- 定数 (仕様 §A.2 / §B.1) ---------------------------------------- */

#define EDIT_TASK_CORE      1     /* UI と同じコア。§B.1 の「core 1 の住人」 */
#define EDIT_TASK_PRIO      5     /* UI タスク (prio 4) より 1 段上 */
#define EDIT_TASK_STACK  8192     /* 内部 SRAM (xTaskCreatePinnedToCore の既定) */
#define EDIT_CMD_QLEN      32

/* オートセーブ / 構文チェックのアイドル判定と、計測ログの吐き出しに使う
   周期。仕様 §A.2 の TICK。 */
#define EDIT_TICK_MS      500

/* フォアグラウンドに出た直後の描き直し。理由は冒頭の「落とし穴 1」の節。 */
#define EDIT_FIRST_PAINT_MS  80
#define EDIT_FOCUS_REPAINTS   4

/* 計測ログの間隔。打鍵経路では**書式化もログもしない** (§C.0 規則 1) ので、
   吐くのは TICK の中だけ。 */
#define EDIT_PERF_LOG_US (3 * 1000 * 1000)

/* 1 段 > 1 秒は外れ値として捨て、捨てた数を数える (§C.0 規則 7)。 */
#define EDIT_PERF_OUTLIER_US 1000000u

/* 画面キーボードのモード。2 = キーボード + 端末コントロールバー
   (Esc/Tab/矢印/Copy/Paste/「あ」)。ドック装着中はコントロールバーだけが
   残り、ui_tab5_kb_reserved(2) がその高さを返す —— どちらの場合も数字は
   同じ経路から来るので、ここに決め打ちは無い。 */
#define EDIT_KB_MODE 2

/* 仕様 §A.1 の既定値。cfg == NULL のときだけ使う。 */
#define EDIT_DEF_MAX_BYTES  (128u * 1024u)
#define EDIT_DEF_MAX_LINES  8192u
#define EDIT_DEF_UNDO_BYTES (64u * 1024u)
#define EDIT_DEF_MAX_COLS   142u   /* 横 1280/9。両向きの最大 */
#define EDIT_DEF_MAX_ROWS   49u    /* 縦 (1280-88)/24 */

/* edit_view_row() に渡す 1 段ぶんのバッファ。max_cols は cfg で決まるが、
   バッファは静的に持ちたい (打鍵経路で malloc しない、8 KB のスタックに
   置かない) ので、上限側で固定する。cfg.core.max_cols がこれを超えたら
   start() で切り詰める。 */
#define EDIT_UI_MAX_COLS 160
/* run は最悪でもセル数を超えない (1 セル 1 run)。少し余裕を足す。 */
#define EDIT_UI_RUNS_CAP (EDIT_UI_MAX_COLS + 8)
/* 1 セルは最大 4 バイト (UTF-8)。preedit の重ね書きぶんを足しておく。 */
#define EDIT_UI_ROW_UTF8 (EDIT_UI_MAX_COLS * 4 + 192)

/* edit_core の preedit バッファ (edit_internal.h の EDIT_PREEDIT_MAX)。
   内部ヘッダは引かない約束なので、公開ヘッダのコメントが名指ししている
   「上限 128 B」をこちらで持つ。**両方が動くときは仕様 §A.1 が正**。 */
#define EDIT_UI_PREEDIT_MAX 128u

/* ステージング。720 × UI_CELL_H(24) = 17,280 B、80 セルぶん —— ui_tab5.cpp の
   s_cells_a8 と同じ寸法を、こちらのタスク専用にもう 1 枚持つ (§A.3)。
   64B (L1/L2 キャッシュライン) 整列は PPA がキャッシュコヒーレントに
   読むための条件。static なので内部 SRAM の .bss に乗る。 */
#define EDIT_A8_BYTES 17280

/* 開くパスの上限。fs_core.h を引かないのは、このファイルが fs_* に一切
   依存しないという約束をヘッダの依存関係でも守るため (冒頭の注記)。
   このバッファはコマンドを運ぶ器でしかなく、パスを解釈するのは M2 の
   fs_io 側。 */
#define EDIT_VPATH_MAX 128

/* ---- 配色 -----------------------------------------------------------
 *
 * edit_cls_t の 8 種に 1 色ずつ。彩色そのものは edit_core が決めており
 * (どのバイトが KEYWORD か)、ここは「その種類を何色で出すか」だけ。 */
#define EDIT_COL_BG        0x101418u
#define EDIT_COL_SEL_BG    0x2A3A55u
#define EDIT_COL_MARK_BG   0x5A1E1Eu
#define EDIT_COL_STATUS_BG 0x1E2430u
#define EDIT_COL_STATUS_FG 0x9FB3C8u

static const uint32_t s_palette[EDIT_CLS_N] = {
    [EDIT_CLS_PLAIN]   = 0xD8D8D8u,
    [EDIT_CLS_KEYWORD] = 0xC792EAu,
    [EDIT_CLS_IDENT]   = 0xD8D8D8u,
    [EDIT_CLS_NUMBER]  = 0xF78C6Cu,
    [EDIT_CLS_STRING]  = 0xC3E88Du,
    [EDIT_CLS_COMMENT] = 0x6A737Du,
    [EDIT_CLS_PUNCT]   = 0x89DDFFu,
    /* preedit は「まだ本文でない」ことが一目で分かる色にする。本文には
       入っていない (edit_core が重ねて描くだけ) ので、確定文字と同じ色に
       すると取り消せる文字列だと分からなくなる。 */
    [EDIT_CLS_PREEDIT] = 0xFFCB6Bu,
};

/* ---- 起動時に入れる本文 ---------------------------------------------
 *
 * **Phase 1 はファイルを読まない** (§A.2: edit_task は fs_* を呼ばない、
 * かつ逃がし先の fs_io が M2 でまだ無い)。空の画面では字句彩色も CJK の
 * 桁送りも回らないので、代わりにこの短いサンプルを置く。実機で見るべき
 * ものが全部 1 画面に入るように選んである: キーワード / 文字列 / 数値 /
 * 行コメント / ブロックコメント / 全角 (2 セル幅) / 空行。 */
static const char s_sample[] =
    "// edit_ui Phase 1 — ファイル入出力はまだ (fs_io は M2)\n"
    "/* このバッファは埋め込みのサンプル。保存はできない。\n"
    "   全角の桁送り: あいうえお漢字カナ ―― 1 文字 2 セル。 */\n"
    "\n"
    "function greet(name) {\n"
    "    const n = name || \"world\";\n"
    "    return \"hello, \" + n + \"!\";   // 24 行ぶん試す\n"
    "}\n"
    "\n"
    "for (var i = 0; i < 10; i++) {\n"
    "    print(greet(\"tab5\"), i * 3.14);\n"
    "}\n";

/* ---- コマンド -------------------------------------------------------
 *
 * 仕様 §A.2 の表のうち、Phase 1 で行き先のあるものだけ。FILE_LOADED /
 * FILE_SAVED / PARSE_RESULT / RUN_ERROR / PICKED / SNAP は、投げる側
 * (fs_io / JS タスク / fs_picker / sys.perf) がまだ無いので**作らない**。
 * 空の case を並べておくと「対応済み」に見えてしまう。
 *
 * union にしてあるのは器の大きさのため: vpath だけが 128 B で、それを
 * 全コマンドに払うと 32 段のキューが内部 SRAM を無駄に食う。 */
typedef enum {
    EDIT_CMD_KEY = 0,
    EDIT_CMD_TOUCH,
    EDIT_CMD_FOCUS,
    EDIT_CMD_BLUR,
    EDIT_CMD_TICK,
    EDIT_CMD_REPAINT,
    EDIT_CMD_OPEN,
    EDIT_CMD_NEW,
    EDIT_CMD_DICT,
} edit_cmd_kind_t;

typedef struct {
    uint8_t kind;
    union {
        struct {
            uint8_t  len;
            char     utf8[24];
            uint32_t t_post;   /* mqjs_post_key 入口 (µs の下位 32 bit) */
            uint32_t t_isr;    /* kb_isr。画面キーボードは 0 */
        } key;
        struct {
            int16_t x, y;
            uint8_t kind;      /* 0 down / 1 move / 2 up */
        } touch;
        struct {
            char vpath[EDIT_VPATH_MAX];
        } open;
        const void *dict;      /* const skk_dict_t * */
    } u;
} edit_cmd_t;

/* ---- 段別の計測 (§C.1 の骨。Phase 1 は簡易) --------------------------
 *
 * ヒストグラムは持たない (perf_meter が棚上げ中、§C.1 の hist は M0b の
 * 持ち物)。ここで持つのは各段の 件数 / 合計 / 最大 だけ。
 *
 * **1 打鍵あたりの時計読みは 4 回** (T1〜T4)。T_isr と T0(t_post) は
 * poster が刻んで持ってくるので、こちら側の読みは増えない —— 合計 6 回で
 * §C.0 規則 1 の上限ちょうど。増やさないこと。
 *
 * 書き手は edit_task 1 つだけ (§C.0 規則 2)。例外は s_drops で、キューに
 * 入らなかったコマンドは投げた側でしか数えられない。 */
typedef struct {
    uint32_t n;
    uint32_t sum_us;
    uint32_t max_us;
} edit_stage_t;

enum {
    ST_ISR2POST = 0,   /* ISR → mqjs_post_key */
    ST_HOP,            /* post → edit_task が KEY を取り出す */
    ST_EDIT,           /* IME feed + edit_core */
    ST_RENDER,         /* render_dirty (PPA blocking 戻りまで) */
    ST_INVAL,          /* ui_tab5_canvas_invalidate (LVGL ロック待ち込み) */
    ST_N
};
static const char *const s_stage_name[ST_N] = {
    "isr>post", "hop", "edit", "draw", "inval"
};

static edit_stage_t s_stage[ST_N];
static uint32_t     s_perf_keys;
static uint32_t     s_perf_outliers;   /* 1 段 > 1 秒。捨てた数を数える */
static int64_t      s_perf_logged_us;
static volatile uint32_t s_drops;      /* poster 側で数える */

/* ---- 状態 (edit_task の所有物。起動時に 1 回だけ外から書く) ---------- */

static edit_t     *s_ed;
static void       *s_block;            /* PSRAM。edit_core のブロック */
static ime_t       s_ime;
static QueueHandle_t s_q;
static TaskHandle_t  s_task;
static esp_timer_handle_t s_tick_timer;
static esp_timer_handle_t s_paint_timer;
static ppa_client_handle_t s_ppa;
static int         s_native_id = -1;
static edit_ui_config_t s_cfg;

static uint8_t s_a8[EDIT_A8_BYTES] __attribute__((aligned(64)));
static edit_run_t s_runs[EDIT_UI_RUNS_CAP];
static char       s_row_utf8[EDIT_UI_ROW_UTF8];
static char       s_status[EDIT_UI_MAX_COLS * 2];
static char       s_vpath[EDIT_VPATH_MAX];   /* 開いている (ことになっている) パス */

/* 画面の幾何。すべて ui_tab5 から取る —— 横 142×26 / 縦 80×49 は
   max_cols/max_rows の初期値であって、コードのどこにも決め打ちを置かない
   (§A.3 / §E-9)。 */
static int s_cell_w, s_cell_h;
static int s_canvas_w, s_canvas_h;
static uint16_t s_cols, s_rows_text;   /* 本文の列数・段数 (ステータス行を除く) */

static bool s_fg;                      /* フォアグラウンドか */
static bool s_canvas_gone;             /* この描画で cells_draw が false を返した */
static int  s_paint_left;              /* まだ予定している描き直しの回数 */
static bool s_canvas_retried;          /* このフォーカスで 1 度積み直したか */

/* 今回の描画で触った段。invalidate はここから矩形にまとめる。
   rows ≤ 49 + ステータス 1 段 なので 64 bit に収まる (§A.1 の
   edit_dirty_rows と同じ理由)。 */
static uint64_t s_inval_rows;
static bool     s_inval_all;

/* タッチのドラッグ。最後に段をまたいだ y と、「指が動いたか」だけ持つ ——
   down の座標は要らない (タップ判定は s_touch_moved で足りる)。 */
static int  s_touch_last_y;
static bool s_touch_moved;

/* ---- 小道具 ---------------------------------------------------------- */

/* 1 コドポイントぶんの UTF-8 を読む。len はこのシーケンスの長さ。
   不正なバイトは 1 バイト 1 文字として扱う —— ここで止まる理由が無い
   (本文の妥当性は edit_core が入口で見ている)。 */
static uint32_t utf8_next(const char *s, size_t avail, size_t *len)
{
    const uint8_t *p = (const uint8_t *)s;
    uint8_t b = p[0];
    size_t n = 1;
    uint32_t cp = b;
    if (b >= 0xF0)      { n = 4; cp = b & 0x07u; }
    else if (b >= 0xE0) { n = 3; cp = b & 0x0Fu; }
    else if (b >= 0xC0) { n = 2; cp = b & 0x1Fu; }
    if (n > avail) { *len = 1; return b; }
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) { *len = 1; return b; }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    *len = n;
    return cp;
}

#define TOK_IS(tok, tlen, lit) \
    ((tlen) == sizeof(lit) - 1 && memcmp((tok), (lit), sizeof(lit) - 1) == 0)

/* 投げっぱなし。満杯なら落として数える。どのタスクからでも呼べる。 */
static void post(const edit_cmd_t *c)
{
    if (!s_q)
        return;
    if (xQueueSend(s_q, c, 0) != pdTRUE)
        s_drops++;
}

/* ---- プレゼンタ ------------------------------------------------------ */

static void draw_run(int col, int row, const char *utf8, size_t len,
                     int ncells, uint32_t fg, uint32_t bg)
{
    if (ncells <= 0)
        return;
    ui_cells_draw_t d;
    memset(&d, 0, sizeof(d));
    d.col = col;
    d.row = row;
    d.ncells = ncells;
    d.utf8 = utf8;
    d.len = len;
    d.fg = fg;
    d.bg = bg;
    d.attrs = 0;
    d.a8 = s_a8;
    d.a8_len = sizeof(s_a8);
    d.ppa = s_ppa;
    if (!ui_tab5_cells_draw(&d))
        s_canvas_gone = true;
}

/* 1 段。edit_view_row の run をそのまま 1 run 1 blit で出す。
 *
 * カーソルは EDIT_RUN_CURSOR の付いた run の**先頭セル**にある (§A.1)。
 * ブロックカーソルにするために、その 1 文字だけ fg と bg を入れ替えて
 * 別の blit にし、残りを続けて描く。run を切るのはここだけで、
 * edit_core 側には何も要求していない。 */
static void render_row(uint16_t row)
{
    int n = edit_view_row(s_ed, row, s_runs, EDIT_UI_RUNS_CAP,
                          s_row_utf8, sizeof(s_row_utf8));
    int used_cells = 0;
    if (n < 0) {
        /* utf8_cap 不足。段を空にして先へ進む —— 1 段が化けるより、
           1 段が空の方が「何が起きたか」が読める。 */
        static bool warned;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "edit_view_row: row buffer too small (%d cols, %u B)",
                     (int)s_cols, (unsigned)sizeof(s_row_utf8));
        }
        n = 0;
    }

    for (int i = 0; i < n; i++) {
        const edit_run_t *r = &s_runs[i];
        uint32_t fg = s_palette[r->cls < EDIT_CLS_N ? r->cls : EDIT_CLS_PLAIN];
        uint32_t bg = EDIT_COL_BG;
        if (r->flags & EDIT_RUN_MARK)
            bg = EDIT_COL_MARK_BG;
        if (r->flags & EDIT_RUN_SELECTED)
            bg = EDIT_COL_SEL_BG;

        const char *txt = s_row_utf8 + r->utf8_off;
        size_t tlen = r->utf8_len;
        int col = r->col, nc = r->ncells;

        if (r->flags & EDIT_RUN_CURSOR) {
            size_t clen = 0;
            int cw = 1;
            if (tlen > 0) {
                uint32_t cp = utf8_next(txt, tlen, &clen);
                cw = ui_cell_width(cp);
                if (cw < 1)
                    cw = 1;
            }
            if (cw > nc)
                cw = nc;
            if (cw > 0) {
                /* 行末のカーソル (文字が無い) は空文字列の blit になり、
                   cells_draw は bg で 1 セル塗る = そのままブロック
                   カーソルになる。 */
                draw_run(col, row, txt, clen, cw, EDIT_COL_BG, fg);
                col += cw;
                nc -= cw;
                txt += clen;
                tlen -= clen;
            }
        }
        draw_run(col, row, txt, tlen, nc, fg, bg);
        used_cells = r->col + r->ncells;
    }

    /* 段の右端の余白。§A.3 のとおり fill 1 回。 */
    int x0 = used_cells * s_cell_w;
    if (x0 < s_canvas_w)
        ui_tab5_canvas_fill(x0, row * s_cell_h, s_canvas_w - x0, s_cell_h,
                            EDIT_COL_BG);
    s_inval_rows |= (uint64_t)1 << row;
}

/* ステータス行。本文の下 1 段。行/桁/行数/変更フラグ/開いているパス。 */
static void render_status(void)
{
    edit_pos_t p = edit_cursor(s_ed);
    unsigned lines = (unsigned)edit_line_count(s_ed);
    const char *name = s_vpath[0] ? s_vpath : "(untitled)";
    int k = snprintf(s_status, sizeof(s_status), " %s%s  %u:%u  %u lines ",
                     edit_modified(s_ed) ? "*" : "", name,
                     (unsigned)p.line1, (unsigned)p.col1, lines);
    if (k < 0)
        k = 0;
    size_t len = (size_t)k < sizeof(s_status) ? (size_t)k : sizeof(s_status) - 1;

    /* セル数 = コドポイント数 (幅 2 の文字は filler セルを 1 つ食う、
       ui.cells の CONT 契約)。パス名に全角が入りうるので数えて出す。 */
    int cells = 0;
    for (size_t i = 0; i < len;) {
        size_t adv = 0;
        uint32_t cp = utf8_next(s_status + i, len - i, &adv);
        int w = ui_cell_width(cp);
        if (cells + (w > 0 ? w : 1) > (int)s_cols) {
            len = i;
            break;
        }
        cells += (w > 0 ? w : 1);
        i += adv;
    }

    draw_run(0, s_rows_text, s_status, len, cells,
             EDIT_COL_STATUS_FG, EDIT_COL_STATUS_BG);
    int x0 = cells * s_cell_w;
    if (x0 < s_canvas_w)
        ui_tab5_canvas_fill(x0, s_rows_text * s_cell_h, s_canvas_w - x0,
                            s_cell_h, EDIT_COL_STATUS_BG);
    s_inval_rows |= (uint64_t)1 << s_rows_text;
}

/* キャンバスまるごとを地の色にする。
 *
 * **段を全部描き直すのとは別物**で、両方が要る理由は端数にある: 段は
 * cell_h(24) 刻みだが canvas_h は 632 で、26 段 = 624 px —— 下に 8 px の
 * 帯が残り、そこは render_row() が一度も触らない。前に居たアプリの画素は
 * そこに残り続ける (横 1280 は 9 で割り切れないので右端にも同じ帯がある。
 * こちらは render_row の右端 fill が canvas_w まで塗るので消える)。
 *
 * 逆に **render_all() には置かない**: あちらはスクロールのたびに通る道で、
 * 各段は自分の bg を持って描き直されるから全面 fill は無駄な帯域になる。
 * ここを呼ぶのは切替 (repaint_all) と回転 (resize_view) だけ。 */
static void clear_canvas(void)
{
    ui_tab5_canvas_fill(0, 0, s_canvas_w, s_canvas_h, EDIT_COL_BG);
}

/* 「全部描く」はここ 1 箇所。
 *
 * EDIT_DIRTY_ALL (スクロール・set_view・set_text の後) は必ずここを通る。
 * スクロールは全段 dirty になるので面積では救えず、対策は PPA でキャンバスを
 * 段ぶん上下にブリットして新しく現れた段だけ描く古典的な手になる (§A.3) —— が、
 * **Phase 1 ではやらない**。まず全面再描画がいくらかを測る。
 * その差し替えのとき触るのは、この関数の中だけで済む。 */
static void render_all(void)
{
    for (uint16_t r = 0; r < s_rows_text; r++)
        render_row(r);
    s_inval_all = true;
}

static void render_dirty(void)
{
    uint32_t flags = edit_dirty_flags(s_ed);
    uint64_t rows = edit_dirty_rows(s_ed);

    if (flags & EDIT_DIRTY_ALL) {
        render_all();
    } else {
        for (uint16_t r = 0; r < s_rows_text; r++)
            if (rows & ((uint64_t)1 << r))
                render_row(r);
    }
    if (flags & EDIT_DIRTY_STATUS)
        render_status();
    edit_dirty_clear(s_ed);
}

/* 隣接する dirty 段は 1 つの矩形にまとめてから渡す (§A.3)。24 px の段に
   対して描画バッファのチャンクは 50 px なので、隣接 2 段は 1 チャンクで
   済むことがある。マージは無料。 */
static void invalidate_pending(void)
{
    if (s_inval_all) {
        ui_tab5_canvas_invalidate(0, 0, s_canvas_w,
                                  (s_rows_text + 1) * s_cell_h);
    } else if (s_inval_rows) {
        int total = s_rows_text + 1;   /* 本文 + ステータス */
        int r = 0;
        while (r < total) {
            if (!(s_inval_rows & ((uint64_t)1 << r))) { r++; continue; }
            int r0 = r;
            while (r < total && (s_inval_rows & ((uint64_t)1 << r)))
                r++;
            ui_tab5_canvas_invalidate(0, r0 * s_cell_h, s_canvas_w,
                                      (r - r0) * s_cell_h);
        }
    }
    s_inval_rows = 0;
    s_inval_all = false;
}

/* ---- 幾何 (§E-9: 両向き) --------------------------------------------
 *
 * 向きの検出も列数・行数の算出もプレゼンタの仕事で、edit_core は数字を
 * 受け取るだけ。"\0rotate" と画面キーボードの出し入れは**同じ経路**を通る。
 *
 * 本文の段数からステータス行 1 段を引いているのは、仕様 §A.3 の
 * rows = (h − reserved) / UI_CELL_H に対する差分 —— ステータス行は
 * プレゼンタが自分で足した予約領域なので、reserved と同じ扱いにした。 */
static void resize_view(void)
{
    ui_tab5_cell_size(&s_cell_w, &s_cell_h);
    ui_tab5_canvas_size(&s_canvas_w, &s_canvas_h);
    if (s_cell_w <= 0 || s_cell_h <= 0 || s_canvas_w <= 0 || s_canvas_h <= 0) {
        s_cols = s_rows_text = 0;
        return;
    }
    int reserved = ui_tab5_kb_reserved(EDIT_KB_MODE);
    int usable = s_canvas_h - reserved;
    if (usable < s_cell_h * 2)
        usable = s_cell_h * 2;   /* 本文 1 段 + ステータス 1 段は必ず残す */

    int cols = s_canvas_w / s_cell_w;
    int rows = usable / s_cell_h - 1;   /* -1 = ステータス行 */
    if (cols > EDIT_UI_MAX_COLS)
        cols = EDIT_UI_MAX_COLS;
    if (cols > s_cfg.core.max_cols)
        cols = s_cfg.core.max_cols;
    if (rows < 1)
        rows = 1;
    if (rows > s_cfg.core.max_rows)
        rows = s_cfg.core.max_rows;

    s_cols = (uint16_t)cols;
    s_rows_text = (uint16_t)rows;
    edit_set_view(s_ed, s_cols, s_rows_text);   /* 全段 dirty になる */

    if (s_fg)
        clear_canvas();
}

/* ---- キー ------------------------------------------------------------ */

/* IME に吸われなかったキーを編集操作に落とす。
 *
 * ime_feed() を**必ず先に**通すこと (ime_core.h の順序規則)。ここが
 * 「\0left を先に解釈してしまう」を構造で防いでいる箇所。 */
static void key_to_edit(const char *k, size_t len)
{
    if (len >= 2 && k[0] == '\0') {
        const char *tok = k + 1;
        size_t tl = len - 1;
        if (TOK_IS(tok, tl, "rotate")) { resize_view(); return; }
        if (TOK_IS(tok, tl, "left"))   { edit_move(s_ed, EDIT_M_LEFT, 1); return; }
        if (TOK_IS(tok, tl, "right"))  { edit_move(s_ed, EDIT_M_RIGHT, 1); return; }
        if (TOK_IS(tok, tl, "up"))     { edit_move(s_ed, EDIT_M_UP, 1); return; }
        if (TOK_IS(tok, tl, "down"))   { edit_move(s_ed, EDIT_M_DOWN, 1); return; }
        if (TOK_IS(tok, tl, "home"))   { edit_move(s_ed, EDIT_M_HOME, 1); return; }
        if (TOK_IS(tok, tl, "end"))    { edit_move(s_ed, EDIT_M_END, 1); return; }
        if (TOK_IS(tok, tl, "pgup"))   { edit_move(s_ed, EDIT_M_PGUP, 1); return; }
        if (TOK_IS(tok, tl, "pgdn"))   { edit_move(s_ed, EDIT_M_PGDN, 1); return; }
        if (TOK_IS(tok, tl, "del"))    { edit_delete(s_ed, 1); return; }
        /* esc / f1..f12 / copy / paste / ime は Phase 1 では何もしない。
           クリップボードは mqjs 側の持ち物 (P4d)、検索とコマンド列は M5 以降。
           黙って捨てるのではなく、ここに「まだ無い」と書いてある。 */
        return;
    }
    if (len == 1) {
        unsigned char c = (unsigned char)k[0];
        if (c == '\b' || c == 0x7F) { edit_delete(s_ed, -1); return; }
        if (c == '\n' || c == '\r') { edit_insert(s_ed, "\n", 1); return; }
        if (c == '\t') { edit_insert(s_ed, "\t", 1); return; }
        /* ドックの Ctrl 和音。仕様に列挙は無いが、実機で undo を試すのに
           物理キー以外の入口が無いので 4 つだけ置く。増やすなら M5 の
           コマンド設計に合わせること。 */
        if (c == 0x1A) { edit_undo(s_ed); return; }          /* Ctrl+Z */
        if (c == 0x19) { edit_redo(s_ed); return; }          /* Ctrl+Y */
        if (c == 0x01) { edit_move(s_ed, EDIT_M_HOME, 1); return; }  /* Ctrl+A */
        if (c == 0x05) { edit_move(s_ed, EDIT_M_END, 1); return; }   /* Ctrl+E */
        if (c < 0x20) return;   /* その他の制御バイトは本文に入れない */
    }
    edit_insert(s_ed, k, len);
}

/* コントロールバーの「あ」に出す面。mqjs_runtime.c の同じ判定と揃える。 */
static int ime_face_now(void)
{
    if (!ime_on(&s_ime))
        return UI_IME_FACE_ASCII;
    return ime_mode(&s_ime) == SKK_MODE_KATA ? UI_IME_FACE_KATA
                                             : UI_IME_FACE_KANA;
}

static void ime_to_edit(ime_disp_t d)
{
    if (d == IME_TEXT) {
        size_t n = 0;
        const char *t = ime_text(&s_ime, &n);
        edit_set_preedit(s_ed, NULL, 0);
        if (t && n)
            edit_insert(s_ed, t, n);
        return;
    }
    /* IME_TAKEN: 本文は変えず、合成中の文字列をカーソル位置に重ねるだけ。
       edit_core の preedit バッファは 128 B 固定 (§A.1) なので、長すぎる
       ものは**コドポイント境界で**切る。バイト単位で切ると不正な UTF-8 に
       なって edit_set_preedit が E_UTF8 を返し、preedit が丸ごと消える。 */
    size_t n = 0;
    const char *pre = ime_on(&s_ime) ? ime_preedit(&s_ime, &n) : NULL;
    if (pre && n > EDIT_UI_PREEDIT_MAX) {
        size_t cut = 0;
        while (cut < n) {
            size_t adv = 0;
            utf8_next(pre + cut, n - cut, &adv);
            if (cut + adv > EDIT_UI_PREEDIT_MAX)
                break;
            cut += adv;
        }
        n = cut;
    }
    edit_set_preedit(s_ed, pre, pre ? n : 0);
}

static void perf_add_us(int st, uint32_t us)
{
    /* 妥当性の上限を持ち、外れ値は捨てた数を報告する (§C.0 規則 7)。
       黙って捨てると「計測器は正しい」と誤読される。 */
    if (us > EDIT_PERF_OUTLIER_US) {
        s_perf_outliers++;
        return;
    }
    s_stage[st].n++;
    s_stage[st].sum_us += us;
    if (us > s_stage[st].max_us)
        s_stage[st].max_us = us;
}

/* poster が刻んだ 32 bit の時刻との差。**引き算は 32 bit のまま**やる ——
   esp_timer の µs を uint32 に切ると約 71 分で一周するので、64 bit へ
   広げてから引くと周回のたびに巨大な偽の値が出る (§C.0 規則 6 で
   M0a が踏んだのと同じ形の誤り)。剰余算なら周回を跨いでも差は正しい。 */
static void perf_add_32(int st, uint32_t a, uint32_t b)
{
    perf_add_us(st, b - a);
}

static void perf_add_64(int st, int64_t a, int64_t b)
{
    if (b < a)
        return;
    uint64_t us = (uint64_t)(b - a);
    perf_add_us(st, us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)us);
}

static void on_key(const edit_cmd_t *c)
{
    if (!s_fg || !s_ed || s_rows_text == 0)
        return;

    const int64_t t1 = esp_timer_get_time();

    /* 順序規則: 何より先に IME へ。 */
    ime_disp_t d = ime_feed(&s_ime, c->u.key.utf8, c->u.key.len);
    if (d == IME_PASS)
        key_to_edit(c->u.key.utf8, c->u.key.len);
    else
        ime_to_edit(d);

    /* 「あ」の表示は IME_V_MODE / IME_V_ENABLE が立ったときだけ更新する
       (毎打鍵 LVGL ロックを取らないため)。読んだ印は毎回落とす。 */
    uint32_t view = ime_view(&s_ime);
    if (view & (IME_V_MODE | IME_V_ENABLE))
        ui_tab5_ime_face(ime_face_now());
    if (view)
        ime_view_clear(&s_ime);

    const int64_t t2 = esp_timer_get_time();
    render_dirty();
    const int64_t t3 = esp_timer_get_time();
    invalidate_pending();
    const int64_t t4 = esp_timer_get_time();

    /* 時計読みはここまでで 4 回。t_isr / t_post は poster が刻んだもの
       (§C.0 規則 1 の 6 回に収める)。 */
    s_perf_keys++;
    if (c->u.key.t_isr)
        perf_add_32(ST_ISR2POST, c->u.key.t_isr, c->u.key.t_post);
    perf_add_32(ST_HOP, c->u.key.t_post, (uint32_t)t1);
    perf_add_64(ST_EDIT, t1, t2);
    perf_add_64(ST_RENDER, t2, t3);
    perf_add_64(ST_INVAL, t3, t4);
}

/* ---- タッチ ---------------------------------------------------------- */

/* ドラッグでスクロール、指を動かさずに離したらその位置へカーソル。
 *
 * 画面上の段からドキュメントの行を出すのに、先頭行を持っている口が
 * edit_core に無い。代わりに「カーソルの行番号 − カーソルの画面段」で
 * 先頭行を導く (edit_cursor と edit_cursor_view の差)。カーソルが画面外に
 * 居るときは cursor_view が false を返すので、そのタップは捨てる ——
 * 誤った行へ飛ぶより何も起きない方がよい。 */
static void on_touch(const edit_cmd_t *c)
{
    if (!s_fg || !s_ed || s_cell_h <= 0)
        return;
    int x = c->u.touch.x, y = c->u.touch.y;

    if (c->u.touch.kind == 0) {
        s_touch_last_y = y;
        s_touch_moved = false;
        return;
    }
    if (c->u.touch.kind == 1) {
        int dy = y - s_touch_last_y;
        int lines = dy / s_cell_h;
        if (lines != 0) {
            s_touch_last_y += lines * s_cell_h;
            s_touch_moved = true;
            /* 指を下へ動かす = 本文を下へ引く = 上の行が出てくる。 */
            edit_scroll(s_ed, -lines);
            render_dirty();
            invalidate_pending();
        }
        return;
    }

    /* up */
    if (s_touch_moved)
        return;
    uint16_t crow, ccol;
    if (!edit_cursor_view(s_ed, &crow, &ccol))
        return;
    edit_pos_t p = edit_cursor(s_ed);
    int row = y / s_cell_h;
    int col = x / s_cell_w;
    if (row >= s_rows_text)
        return;   /* ステータス行のタップ。Phase 1 では意味を持たせない */
    long line1 = (long)p.line1 - (long)crow + row;
    if (line1 < 1)
        line1 = 1;
    edit_goto(s_ed, (uint32_t)line1, (uint32_t)col + 1);
    render_dirty();
    invalidate_pending();
}

/* ---- フォーカス ------------------------------------------------------ */

/* 画面まるごと 1 枚。冒頭の「落とし穴 1」のとおり、フォーカス直後は
   これを EDIT_FOCUS_REPAINTS 回くり返す —— RESET がいつ通るか知る口が
   無いので、通ってから描いたことを回数で担保する。 */
static void repaint_all(void)
{
    if (!s_fg || !s_ed || s_rows_text == 0)
        return;
    s_canvas_gone = false;
    clear_canvas();
    render_all();
    render_status();
    edit_dirty_clear(s_ed);      /* 描いたので次の打鍵に持ち越さない */
    invalidate_pending();        /* この中で HIDDEN が落ちる */

    if (s_canvas_gone) {
        /* キャンバスがまだ作られていない (CanvasApp::onCreate 前 / UI が落ちて
           いる)。描いたものは捨てられたので、このフォーカスにつき 1 回だけ
           予定を積み直す。積み直しは 1 回きり —— 恒久ループにすると UI の
           無いボードでタイマが回り続ける。 */
        if (!s_canvas_retried) {
            s_canvas_retried = true;
            s_paint_left = EDIT_FOCUS_REPAINTS;
            ESP_LOGW(TAG, "canvas not up yet, retrying the first paint");
        }
    }
    if (s_paint_left > 0) {
        s_paint_left--;
        if (s_paint_timer)
            esp_timer_start_once(s_paint_timer, EDIT_FIRST_PAINT_MS * 1000);
    }
}

static void on_focus(void)
{
    s_fg = true;
    resize_view();

    /* キーボード (ドック装着中はコントロールバーだけ) を出す。返り値は
       読まない —— 段数は resize_view が ui_tab5_kb_reserved から取り直す。
       これは描画コマンドではないのでキャンバスを露出させない (ui_tab5.cpp
       が UI_CMD_KEYBOARD をそう扱っている)。露出は invalidate の仕事。 */
    ui_cmd_t kb;
    memset(&kb, 0, sizeof(kb));
    kb.op = UI_CMD_KEYBOARD;
    kb.x = EDIT_KB_MODE;
    ui_tab5_cmd(&kb);

    s_paint_left = EDIT_FOCUS_REPAINTS;
    s_canvas_retried = false;
    repaint_all();

    if (s_tick_timer) {
        esp_timer_stop(s_tick_timer);
        esp_timer_start_periodic(s_tick_timer, EDIT_TICK_MS * 1000);
    }
}

static void on_blur(void)
{
    s_fg = false;
    if (s_tick_timer)
        esp_timer_stop(s_tick_timer);
    if (s_paint_timer)
        esp_timer_stop(s_paint_timer);
    s_paint_left = 0;
    /* 読みかけは捨てる。確定させない —— 誤操作で本文に half-baked な
       かなを残すより、数文字打ち直す方がまし (ime_core の同じ判断)。 */
    ime_reset(&s_ime);
    ime_view_clear(&s_ime);
    edit_set_preedit(s_ed, NULL, 0);
}

static void on_repaint(void) { repaint_all(); }

/* ---- TICK ------------------------------------------------------------
 *
 * オートセーブ (2 s / 100 編集) と構文チェックのアイドル判定がここに入る
 * 予定だが、どちらも投げる先 (fs_io / mqjs_parse_request) が Phase 1 には
 * 無い。今は計測ログを吐くだけ。
 *
 * **ログを TICK に置いたのは意図的**: 打鍵経路では書式化もログもしない
 * (§C.0 規則 1)。ここは打鍵の外なので UART が詰まっても打鍵に響かない。 */
static void on_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (s_perf_keys == 0 || now - s_perf_logged_us < EDIT_PERF_LOG_US)
        return;
    s_perf_logged_us = now;

    char line[192];
    int k = snprintf(line, sizeof(line), "keys=%u", (unsigned)s_perf_keys);
    for (int i = 0; i < ST_N && k > 0 && k < (int)sizeof(line); i++) {
        unsigned avg = s_stage[i].n ? s_stage[i].sum_us / s_stage[i].n : 0;
        k += snprintf(line + k, sizeof(line) - (size_t)k, " %s %u/%u",
                      s_stage_name[i], avg, (unsigned)s_stage[i].max_us);
    }
    ESP_LOGI(TAG, "%s us(avg/max) drops=%u outliers=%u",
             line, (unsigned)s_drops, (unsigned)s_perf_outliers);
}

/* ---- タスク ---------------------------------------------------------- */

static void edit_task_fn(void *arg)
{
    (void)arg;
    edit_cmd_t c;
    for (;;) {
        if (xQueueReceive(s_q, &c, portMAX_DELAY) != pdTRUE)
            continue;
        switch (c.kind) {
        case EDIT_CMD_KEY:     on_key(&c); break;
        case EDIT_CMD_TOUCH:   on_touch(&c); break;
        case EDIT_CMD_FOCUS:   on_focus(); break;
        case EDIT_CMD_BLUR:    on_blur(); break;
        case EDIT_CMD_TICK:    on_tick(); break;
        case EDIT_CMD_REPAINT: on_repaint(); break;
        case EDIT_CMD_NEW:
            edit_set_text(s_ed, "", 0);
            s_vpath[0] = '\0';
            if (s_fg) { render_dirty(); invalidate_pending(); }
            break;
        case EDIT_CMD_OPEN:
            /* **読まない。** §A.2 が edit_task から fs_* を呼ぶことを禁じて
               おり、逃がし先の fs_io は M2 でまだ無い。パスだけ覚えて
               ステータス行に出す (「開いたつもりで空だった」を画面から
               判別できるように、本文はサンプルのままにしない)。 */
            memcpy(s_vpath, c.u.open.vpath, sizeof(s_vpath));
            s_vpath[sizeof(s_vpath) - 1] = '\0';
            edit_set_text(s_ed, "", 0);
            if (s_fg) { render_dirty(); invalidate_pending(); }
            ESP_LOGW(TAG, "open(%s): Phase 1 では読み込まない (fs_io は M2)",
                     s_vpath);
            break;
        case EDIT_CMD_DICT:
            /* ime_t を触ってよいのはこのタスクだけ。だから attach も
               ここで起きる (§E-2 の安全条件 2)。 */
            ime_attach(&s_ime, (const skk_dict_t *)c.u.dict);
            break;
        default:
            break;
        }
    }
}

static void tick_cb(void *arg)
{
    (void)arg;
    edit_cmd_t c = { .kind = EDIT_CMD_TICK };
    post(&c);
}

static void paint_cb(void *arg)
{
    (void)arg;
    edit_cmd_t c = { .kind = EDIT_CMD_REPAINT };
    post(&c);
}

/* ---- native surface (§A.6) -------------------------------------------
 *
 * 4 本とも「キューへ投げるだけ」。focus/blur は JS タスク、key は poster の
 * タスク (kbd_task / LVGL / IME 所有)、touch も同じ。どれもここで
 * edit_core にも ime_t にも触らない —— 触ったらその瞬間に単一所有者が
 * 崩れる。 */
static void nat_focus(void *ctx)
{
    (void)ctx;
    edit_cmd_t c = { .kind = EDIT_CMD_FOCUS };
    post(&c);
}

static void nat_blur(void *ctx)
{
    (void)ctx;
    edit_cmd_t c = { .kind = EDIT_CMD_BLUR };
    post(&c);
}

static void nat_key(void *ctx, const char *utf8, size_t len,
                    uint32_t t_post, uint32_t t_isr)
{
    (void)ctx;
    if (!utf8 || len == 0)
        return;
    edit_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = EDIT_CMD_KEY;
    if (len > sizeof(c.u.key.utf8))
        len = sizeof(c.u.key.utf8);   /* kbd_core の KBD_SEQ_MAX は 16 */
    memcpy(c.u.key.utf8, utf8, len);
    c.u.key.len = (uint8_t)len;
    c.u.key.t_post = t_post;
    c.u.key.t_isr = t_isr;
    post(&c);
}

static void nat_touch(void *ctx, int x, int y, int kind)
{
    (void)ctx;
    edit_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = EDIT_CMD_TOUCH;
    c.u.touch.x = (int16_t)x;
    c.u.touch.y = (int16_t)y;
    c.u.touch.kind = (uint8_t)kind;
    post(&c);
}

/* ---- 公開 API -------------------------------------------------------- */

static void fill_defaults(edit_ui_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->core.max_bytes = EDIT_DEF_MAX_BYTES;
    c->core.max_lines = EDIT_DEF_MAX_LINES;
    c->core.undo_bytes = EDIT_DEF_UNDO_BYTES;
    c->core.max_cols = EDIT_DEF_MAX_COLS;
    c->core.max_rows = EDIT_DEF_MAX_ROWS;
    c->autosave_idle_ms = 2000;
    c->autosave_edits = 100;
    c->syntax_idle_ms = 1000;
}

bool edit_ui_start(const edit_ui_config_t *cfg)
{
    /* 宣言はここに集める: 下の `goto fail` が初期化子付きの宣言を飛び越すと
       C では合法でも -Wjump-misses-init が鳴り、C++ なら不正になる。 */
    size_t need;
    ppa_client_config_t pc;
    esp_timer_create_args_t ta;
    mqjs_native_surface_t surf;

    if (s_task)
        return true;   /* 起動時 1 回。二度目は黙って成功 */

    if (cfg)
        s_cfg = *cfg;
    else
        fill_defaults(&s_cfg);
    if (s_cfg.core.max_cols == 0 || s_cfg.core.max_cols > EDIT_UI_MAX_COLS)
        s_cfg.core.max_cols = EDIT_UI_MAX_COLS;
    if (s_cfg.core.max_rows == 0 || s_cfg.core.max_rows > 63)
        s_cfg.core.max_rows = EDIT_DEF_MAX_ROWS;

    /* ブロックは PSRAM に、起動時に、固定上限で 1 回だけ (§A.2)。
       8B 整列は edit_init の要求。 */
    need = edit_mem_size(&s_cfg.core);
    s_block = heap_caps_aligned_alloc(8, need, MALLOC_CAP_SPIRAM);
    if (!s_block) {
        ESP_LOGE(TAG, "edit_core block alloc failed (%u B PSRAM)",
                 (unsigned)need);
        return false;
    }
    if (edit_init(s_block, need, &s_cfg.core, &s_ed) != EDIT_OK) {
        ESP_LOGE(TAG, "edit_init failed (%u B)", (unsigned)need);
        heap_caps_free(s_block);
        s_block = NULL;
        return false;
    }
    edit_set_text(s_ed, s_sample, sizeof(s_sample) - 1);

    ime_init(&s_ime);   /* 辞書はまだ。全部 IME_PASS で ASCII は打てる */

    s_q = xQueueCreate(EDIT_CMD_QLEN, sizeof(edit_cmd_t));
    if (!s_q) {
        ESP_LOGE(TAG, "command queue alloc failed");
        goto fail;
    }

    /* PPA の blend クライアントは自前 (エンジンは共有、キューは別、§A.3)。
       取れなくても止めない —— cells_draw が CPU 経路へ落ちるだけ。 */
    memset(&pc, 0, sizeof(pc));
    pc.oper_type = PPA_OPERATION_BLEND;
    if (ppa_register_client(&pc, &s_ppa) != ESP_OK) {
        s_ppa = NULL;
        ESP_LOGW(TAG, "PPA blend unavailable, cells stay on CPU");
    }

    memset(&ta, 0, sizeof(ta));
    ta.callback = tick_cb;
    ta.name = "edit_tick";
    if (esp_timer_create(&ta, &s_tick_timer) != ESP_OK)
        s_tick_timer = NULL;
    memset(&ta, 0, sizeof(ta));
    ta.callback = paint_cb;
    ta.name = "edit_paint";
    if (esp_timer_create(&ta, &s_paint_timer) != ESP_OK)
        s_paint_timer = NULL;

    memset(&surf, 0, sizeof(surf));
    surf.name = "editor";
    surf.title = "Editor";
    surf.focus = nat_focus;
    surf.blur = nat_blur;
    surf.key = nat_key;
    surf.touch = nat_touch;
    surf.ctx = NULL;
    s_native_id = mqjs_native_register(&surf);
    if (s_native_id < 0) {
        ESP_LOGE(TAG, "native surface register failed");
        goto fail;
    }

    if (xTaskCreatePinnedToCore(edit_task_fn, "edit", EDIT_TASK_STACK, NULL,
                                EDIT_TASK_PRIO, &s_task,
                                EDIT_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "edit_task create failed");
        s_task = NULL;
        goto fail;
    }

    /* §A.3 の判定材料。**ビルド時ではなく起動ログの数字で**決める:
       これが 40 KB を切るなら、自前の 17,280 B ステージングをやめて
       ui_tab5 の s_cells_a8 を専用ミューテックス付きで共有する側へ倒す。
       s_a8 は .bss なのでこの数字には既に反映されている (差し引き済み)。 */
    ESP_LOGI(TAG, "started: core block %u B PSRAM, staging %u B internal, "
                  "internal largest free %u B (< 40960 なら s_cells_a8 共有へ)",
             (unsigned)need, (unsigned)sizeof(s_a8),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;

fail:
    if (s_q) { vQueueDelete(s_q); s_q = NULL; }
    if (s_ppa) { ppa_unregister_client(s_ppa); s_ppa = NULL; }
    if (s_tick_timer) { esp_timer_delete(s_tick_timer); s_tick_timer = NULL; }
    if (s_paint_timer) { esp_timer_delete(s_paint_timer); s_paint_timer = NULL; }
    heap_caps_free(s_block);
    s_block = NULL;
    s_ed = NULL;
    return false;
}

void edit_ui_open(const char *vpath)
{
    if (!s_q || !vpath)
        return;
    edit_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = EDIT_CMD_OPEN;
    size_t n = strlen(vpath);
    if (n >= sizeof(c.u.open.vpath))
        n = sizeof(c.u.open.vpath) - 1;
    memcpy(c.u.open.vpath, vpath, n);
    post(&c);
    if (s_native_id >= 0)
        mqjs_native_focus(s_native_id);
}

void edit_ui_new(void)
{
    if (!s_q)
        return;
    edit_cmd_t c = { .kind = EDIT_CMD_NEW };
    post(&c);
    if (s_native_id >= 0)
        mqjs_native_focus(s_native_id);
}

void edit_ui_attach_dict(const skk_dict_t *dict)
{
    if (!s_q)
        return;
    edit_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = EDIT_CMD_DICT;
    c.u.dict = dict;
    post(&c);
}

#else /* CONFIG_MQJS_TAB5_UI = n (Stamp など画面の無いボード) */

bool edit_ui_start(const edit_ui_config_t *cfg) { (void)cfg; return false; }
void edit_ui_open(const char *vpath) { (void)vpath; }
void edit_ui_new(void) { }
void edit_ui_attach_dict(const skk_dict_t *dict) { (void)dict; }

#endif /* CONFIG_MQJS_TAB5_UI */
