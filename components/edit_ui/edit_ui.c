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
#include "fs_io.h"
#include "fs_picker.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ime_core.h"
#include "mqjs_native.h"
#include "ui_cell_width.h"
#include "ui_tab5.h"
#if CONFIG_MQJS_UI_GEOM_PROBE
/* 診断専用の口。公開ヘッダには出さない (旗が無いビルドに影を落とさない)。 */
void ui_tab5_geom_probe(void);
#endif

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
/* 0 = 最初の 1 枚だけ。以前は 4 回の描き直しを重ねていた —— UI_CMD_RESET
   が非同期で、いつ通るか知る口が無かったため。ui_tab5_canvas_reset_sync()
   が同期になったので保険は要らない。全画面 1 枚は実測 74.6 ms なので、
   5 枚 = アプリ切替のたびに core 1 が 370 ms 持っていかれていた。 */
#define EDIT_FOCUS_REPAINTS   0

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

/* SKK 確定時のアーティファクト調査用。**既定オフ。**
   役目は終わった —— 原因はここではなく、フレームバッファへの二重書き込み
   だった (ui_tab5.cpp の canvas_present_direct)。残してあるのは、段ごとの
   run を見たくなる日がまた来るため。**1 にするなら、段ごとに無条件で
   吐かないこと** —— 1 行 ~50B を 115200 baud へ同期に出すと 4.3 ms、
   26 段で 1 フレーム 113 ms になり、計測器がスクロールを壊す (実際にやった)。 */
#ifndef EDIT_TRACE_RUNS
#define EDIT_TRACE_RUNS 0
/* 2026-08-27 の切り分け用 (回転の宛先が逆だった件)。既定オフ。
   1 にすると 1 バッチ 1 行、壊れた形は間引かずに出す。 */
#ifndef EDIT_TRACE_PAINT
#define EDIT_TRACE_PAINT 0
#endif

#if EDIT_TRACE_PAINT
static int s_tp_rows, s_tp_nonempty, s_tp_neg;
static int s_tp_drawn, s_tp_failed;
/* 視界を動かしたのは誰か。scr=タッチのドラッグ、gt=タップの edit_goto。
   どちらも 0 なのに本文が消えるなら、動かしたのは core の追従。 */
static int s_tp_scr, s_tp_scr_lines, s_tp_gt, s_tp_gt_line;
static int s_tp_rect_y, s_tp_rect_h, s_tp_rects;
static int64_t s_tp_last;
#endif

#endif

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
    EDIT_CMD_SAVE,     /* 明示保存。vpath が空なら既定の置き場へ */
    EDIT_CMD_SAVE_AS,  /* ピッカーで選んだ先へ保存 */
    EDIT_CMD_LOADED,   /* fs_io から。buf の所有権が来る */
    EDIT_CMD_SAVED,    /* fs_io から。結果だけ */
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
        struct {
            void  *buf;        /* PSRAM。このタスクが free する */
            uint32_t len;
            int32_t err;       /* esp_err_t。0 = 成功 */
        } io;
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
/* 一言の通知。ステータス行の末尾に出して、数秒で消す。**トーストは作らない**
   —— ui_tab5_set_status は構造体まるごとの差し替えで、他の欄を巻き添えに
   する。エディタの持ち物はエディタのステータス行に出すのが素直。 */
static char       s_note[40];
static int64_t    s_note_until;
/* 保存の宛先。新規は §E-11 のとおり最初からここに固定する ——
   あとから動かすとユーザの原稿が迷子になる。 */
#define EDIT_DEFAULT_DIR "/internal/scripts"

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
    if (!ui_tab5_cells_draw(&d)) {
        s_canvas_gone = true;
#if EDIT_TRACE_PAINT
        s_tp_failed++;
    } else {
        s_tp_drawn++;
#endif
    }
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
#if EDIT_TRACE_PAINT
    s_tp_rows++;
    if (n > 0) s_tp_nonempty++;
    if (n < 0) s_tp_neg++;
#endif
    int used_cells = 0;
#if EDIT_TRACE_RUNS
    bool trace_this_row = false;
#endif
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

#if EDIT_TRACE_RUNS
    /* SKK の確定でアーティファクトが残る件 (2026-08-26)。静的解析では
       原因が出なかった —— 塗り・順序・ベースライン・dirty の変換はどれも
       正しい。**実際に何が描かれたか**を記録して読む。
       preedit が在る段と、その直後の段だけを出す (毎段出すと UART が溢れる)。 */
    {
        static int s_trace_left;
        bool has_pre = false;
        trace_this_row = false;
        for (int i = 0; i < n; i++)
            if (s_runs[i].cls == EDIT_CLS_PREEDIT)
                has_pre = true;
        if (has_pre)
            s_trace_left = 3;      /* 確定後の 2 段ぶんも追う */
        if (s_trace_left > 0) {
            s_trace_left--;
            trace_this_row = true;
            char b[192];
            int o = snprintf(b, sizeof b, "row=%d pre=%d runs=%d:", row,
                             has_pre ? 1 : 0, n);
            for (int i = 0; i < n && o > 0 && o < (int)sizeof b - 24; i++)
                o += snprintf(b + o, sizeof b - (size_t)o, " [c%u n%u cls%u]",
                              (unsigned)s_runs[i].col,
                              (unsigned)s_runs[i].ncells,
                              (unsigned)s_runs[i].cls);
            ESP_LOGW(TAG, "TRACE %s", b);
        }
    }
#endif

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
#if EDIT_TRACE_RUNS
    /* **段ごとに無条件で吐いてはいけない。** 1 行 ~50B を 115200 baud へ
       同期に出すと 4.3 ms、26 段で 1 フレーム 113 ms —— 計測器がスクロールを
       壊す (実機で「遅くなった」と報告された)。preedit の前後だけに絞る。 */
    if (trace_this_row)
        ESP_LOGW(TAG, "TRACE row=%d tail_from_cell=%d px=%d..%d", row,
                 used_cells, x0, s_canvas_w);
#endif
    if (x0 < s_canvas_w)
        ui_tab5_canvas_fill(x0, row * s_cell_h, s_canvas_w - x0, s_cell_h,
                            EDIT_COL_BG);
    s_inval_rows |= (uint64_t)1 << row;
}

/* ステータス行。本文の下 1 段。行/桁/行数/変更フラグ/開いているパス。 */
/* 一言だけ出して数秒で消す。ステータス行の再描画に乗せるので、
   ここでは印を付けるだけ (描画は edit_task の上でしか起きない)。 */
/* 「別名で保存」。書き換えられない場所に当たったときの逃げ道でもある。 */
static bool s_pick_saving;
/* 書き換え不可で行き先を差し替えたか。無限に差し替えないための印。 */
static bool s_redirected;
/* SAVE モードは Ctrl+W から。以前ここで core 1 が固まったが、真因は
   **モーダルの上のタップが裏のこのタスクにも配られていたこと**だった
   (touch_observe が fs_picker のスクリムを見ておらず、カメラのだけ見て
   いた)。裏で全画面再描画が走り、UI タスクの lv_refr_now と同じ PPA を
   LVGL ロックの内側で奪い合う —— PPA の待ちには上限が無いので両方が
   止まる。ui_tab5 側でタッチを止めたので、この経路を戻した。 */
static void open_picker(bool for_save);

static void sys_note(const char *msg)
{
    snprintf(s_note, sizeof s_note, "%s", msg);
    s_note_until = esp_timer_get_time() + 4000000;
}

/*
 * fs_io からの返事。**fs_io タスクの上で呼ばれるので、投げ直すだけ。**
 * ここで edit_core を触ると、単一書き手 (§A.2) の前提が壊れる。
 * LOAD の buf は所有権ごとキューに載せ、edit_task が free する。
 */
/*
 * ピッカーからエディタへ渡す道。**これが無いと、開く口が 1 つも無い。**
 * fs.pick は JS 束縛で、呼んでいるのは dev スロットの probe だけだった ——
 * エディタは開けても、そこにファイルを持ち込む手段が存在しなかった。
 *
 * 仕様 §A の依存の向きは `edit_ui -> ... fs_io, fs_picker` なので、
 * エディタが自分でモーダルを出してよい。cb は **UI タスクの上**で呼ばれる
 * ので、ここでも投げ直すだけにする (§A.2 の単一書き手)。
 */
static void pick_done(void *ctx, const fs_pick_result_t *r)
{
    edit_cmd_t c;

    (void)ctx;
    if (!r->ok || !r->vpath[0]) {
        /* 取り消し。**描き直しは要る** —— モーダルの間こちらは描くのを
           やめており、スクリムが消えた跡は誰も塗らない。 */
        memset(&c, 0, sizeof c);
        c.kind = EDIT_CMD_REPAINT;
        post(&c);
        return;
    }
    /* **切り詰めない。** ピッカーの器は 512 B、こちらは 128 B。詰めて入れると
       別のファイルを開き、そのまま Ctrl+S で**別のファイルを上書きする**。
       入らない道は断って、断ったと言う。 */
    if (strlen(r->vpath) >= sizeof c.u.open.vpath) {
        sys_note("パスが長すぎます");
        return;
    }
    memset(&c, 0, sizeof c);
    c.kind = s_pick_saving ? EDIT_CMD_SAVE_AS : EDIT_CMD_OPEN;
    strcpy(c.u.open.vpath, r->vpath);
    post(&c);
}

static void open_picker(bool for_save)
{
    static const char *const exts[] = { ".js", ".txt", ".json", ".md" };
    fs_pick_req_t req;
    const char *base = s_vpath[0] ? strrchr(s_vpath, '/') : NULL;

    memset(&req, 0, sizeof req);
    s_pick_saving = for_save;
    req.mode = for_save ? FS_PICK_SAVE : FS_PICK_OPEN;
    req.title = for_save ? "別名で保存" : "開く";
    req.start_vpath = EDIT_DEFAULT_DIR;
    req.exts = exts;
    req.n_exts = (int)(sizeof exts / sizeof exts[0]);
    req.suggest_name = base ? base + 1 : "untitled.js";
    if (!fs_pick_begin(&req, pick_done, NULL))
        sys_note("ピッカーを出せません");
}

static void io_done(void *ctx, const fsio_result_t *r)
{
    edit_cmd_t c;

    (void)ctx;
    memset(&c, 0, sizeof c);
    c.kind = (r->kind == FSIO_LOAD) ? EDIT_CMD_LOADED : EDIT_CMD_SAVED;
    c.u.io.buf = r->buf;
    c.u.io.len = (uint32_t)r->len;
    c.u.io.err = (int32_t)r->err;
    post(&c);
    /* post が落としたら buf が漏れる。キューは 32 段あって、返事は
       打鍵より桁違いに少ないが、漏らさないことは保証しておく。 */
}

/* 本文を PSRAM へ写して fs_io へ渡す。**edit_core のバッファをそのまま
   渡さない** —— 書いている間に打鍵が来ると gap が動く。写した時点の
   スナップショットを保存するのが正しい (保存中の打鍵は次の保存に乗る)。 */
static void save_now(void)
{
    size_t len, got;
    char *buf;

    if (!s_ed)
        return;
    if (!s_vpath[0]) {
        /* 名前が無い原稿。§E-11 の既定の置き場へ、時刻ではなく固定名で。
           時刻を使うと「保存するたびに別ファイル」になって迷子になる。 */
        snprintf(s_vpath, sizeof s_vpath, "%s/untitled.js", EDIT_DEFAULT_DIR);
    }
    len = edit_text_len(s_ed);
    buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        sys_note("保存できません (メモリ不足)");
        return;
    }
    got = edit_copy_text(s_ed, 0, buf, len + 1);
    if (!fsio_submit(FSIO_SAVE, s_vpath, NULL, buf, got, io_done, NULL, NULL)) {
        free(buf);
        sys_note("保存できません (I/O 混雑)");
        return;
    }
    sys_note("保存中…");
}

static void render_status(void)
{
    edit_pos_t p = edit_cursor(s_ed);
    unsigned lines = (unsigned)edit_line_count(s_ed);
    const char *name = s_vpath[0] ? s_vpath : "(untitled)";
    /* 通知は期限を過ぎたら自分で消える。tick を待たないのは、tick が
       止まっていても正しく消えてほしいから (表示の真偽が時計だけで決まる)。 */
    if (s_note[0] && esp_timer_get_time() > s_note_until)
        s_note[0] = 0;
    int k = snprintf(s_status, sizeof(s_status), " %s%s  %u:%u  %u lines %s%s",
                     edit_modified(s_ed) ? "*" : "", name,
                     (unsigned)p.line1, (unsigned)p.col1, lines,
                     s_note[0] ? " — " : "", s_note);
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
#if EDIT_TRACE_PAINT
        s_tp_rects = 1; s_tp_rect_y = 0;
        s_tp_rect_h = (s_rows_text + 1) * s_cell_h;
#endif
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
#if EDIT_TRACE_PAINT
            if (!s_tp_rects) { s_tp_rect_y = r0 * s_cell_h;
                               s_tp_rect_h = (r - r0) * s_cell_h; }
            s_tp_rects++;
#endif
            ui_tab5_canvas_invalidate(0, r0 * s_cell_h, s_canvas_w,
                                      (r - r0) * s_cell_h);
        }
    }
    s_inval_rows = 0;
    s_inval_all = false;
}

/* 「描いて出す」を 1 単位にする。

   **バッチ全体を LVGL ロックの内側でやる。** そうしないと、行を描いて
   いる途中で LVGL タスクが起きてロックを取り、**描きかけのキャンバスを
   読んで** flush してしまう —— 半分は新しい白、半分はまだ古い黄色、
   という裂けたフレームが出る。SKK の確定で黄色い preedit の上端が
   残ったのはこれで、直接提示の順序を直しても閉じない窓だった
   (直接提示が正すのは「自分が撃つ瞬間」だけで、裂けた flush が後から
   着弾すれば古い画素が勝つ)。

   ロックは再帰なので、この下の ui_tab5_canvas_invalidate が内側で
   もう一度取っても問題ない。取れなくても描く —— 順序が緩むだけ。 */
/* 実機で 2026-08-27 に出た「改行/行削除で本文が消え、ステータスだけが
   画面中央に残る」を切り分けるための計測。edit_core はホストで無実が
   確定した (test_newline_view.c) ので、残るのはこの段から下。

   1 バッチ 1 行、4/s に絞る。**無条件に出してはいけない** —— 段ごとに
   吐いて 1 フレーム 113 ms にした前科がある (EDIT_TRACE_RUNS)。
   読みたいのは 3 つだけ:
     nonempty/rows  本文の段が描かれたか (0 なら view が空)
     rt / cv        ステータス行の位置と、キャンバスの実寸
     rect           実際に無効化した矩形 */
static void paint_dirty(void)
{
    /* **モーダルが出ている間は描かない。**
       スクリムで完全に隠れているので描く理由が無い。そして描くと実害が
       ある —— PPA SRM / DMA2D の使い手が 2 人になり、ピッカーの
       lv_refr_now と衝突して flush が完了しなくなる。
       検死役の 1 行がそれを名指しした (2026-08-27):
         CORONER: UI stalled 3s | lock owner=(none) depth=0 | flushing=1
       ロックの奪い合いではなく、**飛行中の flush が着地しない**形。
       モーダルが閉じたら pick_done が OPEN / REPAINT を投げるので、
       そこで描き直される。 */
    if (fs_pick_active())
        return;
    bool held = ui_tab5_canvas_batch_begin();
#if EDIT_TRACE_PAINT
    s_tp_rows = s_tp_nonempty = s_tp_neg = 0;
    s_tp_drawn = s_tp_failed = 0;
    uint32_t d0 = ui_tab5_direct_count();
    s_tp_rects = 0; s_tp_rect_y = s_tp_rect_h = -1;
    uint32_t fl = edit_dirty_flags(s_ed);
    uint64_t dr = edit_dirty_rows(s_ed);
#endif
    render_dirty();
    invalidate_pending();
#if EDIT_TRACE_PAINT
    uint16_t tp_crow = 0, tp_ccol = 0;
    bool tp_crow_vis = edit_cursor_view(s_ed, &tp_crow, &tp_ccol);
    (void)tp_ccol;
    /* **時間で間引くと、肝心の 1 フレームを取り逃す。** 4/s のサンプリングは
       タッチのドラッグが出す paint に埋もれて、改行した瞬間の paint を
       1 度も捉えられなかった。だから壊れた形そのものを条件にする:
       本文があるのに段が 1 つも描けなかった paint は**必ず**出す。
       それ以外は 1/s の生存確認に落とす。 */
    bool bad = edit_line_count(s_ed) > 1 && s_tp_rows > 1 && s_tp_nonempty <= 1;
    int64_t now = esp_timer_get_time();
    if (bad || now - s_tp_last > 1000000) {
        s_tp_last = now;
        ESP_LOGW(TAG, "paint fl=%u dr=%llx rt=%u ch=%d cv=%dx%d "
                 "rows=%d/%d neg=%d rect=%d+%d x%d blit=%d/%d gone=%d direct=%u "
                 "ln=%u cur=%u crow=%d top=%u scr=%dx%d gt=%dx%d%s",
                 (unsigned)fl, (unsigned long long)dr, (unsigned)s_rows_text,
                 s_cell_h, s_canvas_w, s_canvas_h,
                 s_tp_nonempty, s_tp_rows, s_tp_neg,
                 s_tp_rect_y, s_tp_rect_h, s_tp_rects,
                 s_tp_drawn, s_tp_drawn + s_tp_failed, s_canvas_gone ? 1 : 0,
                 (unsigned)(ui_tab5_direct_count() - d0),
                 (unsigned)edit_line_count(s_ed),
                 (unsigned)edit_cursor(s_ed).line1,
                 tp_crow_vis ? (int)tp_crow : -1,
                 (unsigned)edit_top_line(s_ed),
                 s_tp_scr, s_tp_scr_lines, s_tp_gt, s_tp_gt_line,
                 bad ? "  <<< BAD" : "");
        s_tp_scr = s_tp_gt = 0;
    }
#endif
    if (held)
        ui_tab5_canvas_batch_end();
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
        /* Ctrl+S = 保存。**保存を呼ぶ口がこれしか無い** —— 画面キーボード
           には保存キーが無く、ドックが刺さっていない板では保存できない。
           M5 のコマンド設計で操作バーに出すのが本筋で、それまでの入口。 */
        if (c == 0x13) { save_now(); return; }               /* Ctrl+S */
        if (c == 0x0F) { open_picker(false); return; }        /* Ctrl+O */
        /* Ctrl+W = 名前を付けて保存。Ctrl+Shift+S は使えない ——
           ドックは Ctrl+S と同じ 0x13 を送るので区別が付かない。 */
        if (c == 0x17) { open_picker(true); return; }         /* Ctrl+W */
#if CONFIG_MQJS_UI_GEOM_PROBE
        /* Ctrl+G = 幾何プローブ (E2、診断専用)。旗が無いビルドでは
           下の `c < 0x20` で捨てられる = 挙動は変わらない。 */
        if (c == 0x07) { ui_tab5_geom_probe(); return; }      /* Ctrl+G */
#endif
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

    /* モーダル (fs_picker) が出ている間、キーはピッカーの持ち物。
       **IME より前に**転送する —— ピッカーへ行くキーは IME を通さない
       (意図的な選択): SKK の変換 UI はこの原稿のステータス行に出るが、
       モーダル中それはスクリムの裏で見えないし、変換状態が原稿側に
       育つのはモーダル境界の混線そのもの。ファイル名は ASCII 素通し
       (M1 の lv_keyboard も日本語は打てなかった — kb_place の注記)。
       ここで返ることで、打鍵が裏の原稿を書き換える穴も同時に塞がる
       (描画は 4a640e0 でゲート済みだったが、edit_insert は素通りだった)。
       false = ピッカーは出ていない、いつもの経路へ。 */
    if (fs_pick_key(c->u.key.utf8, c->u.key.len))
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
    bool held = ui_tab5_canvas_batch_begin();
    render_dirty();
    const int64_t t3 = esp_timer_get_time();
    invalidate_pending();
    if (held)
        ui_tab5_canvas_batch_end();
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
#if EDIT_TRACE_PAINT
            s_tp_scr++; s_tp_scr_lines = -lines;
#endif
            edit_scroll(s_ed, -lines);
            paint_dirty();
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
#if EDIT_TRACE_PAINT
    s_tp_gt++; s_tp_gt_line = (int)line1;
#endif
    edit_goto(s_ed, (uint32_t)line1, (uint32_t)col + 1);
    paint_dirty();
}

/* ---- フォーカス ------------------------------------------------------ */

/* 画面まるごと 1 枚。冒頭の「落とし穴 1」のとおり、フォーカス直後は
   これを EDIT_FOCUS_REPAINTS 回くり返す —— RESET がいつ通るか知る口が
   無いので、通ってから描いたことを回数で担保する。 */
static void repaint_all(void)
{
    if (!s_fg || !s_ed || s_rows_text == 0)
        return;
    if (fs_pick_active())
        return;   /* paint_dirty と同じ理由。隠れている間は描かない */
    s_canvas_gone = false;
    bool held = ui_tab5_canvas_batch_begin();
    clear_canvas();
    render_all();
    render_status();
    edit_dirty_clear(s_ed);      /* 描いたので次の打鍵に持ち越さない */
    invalidate_pending();        /* この中で HIDDEN が落ちる */
    if (held)
        ui_tab5_canvas_batch_end();

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
            if (s_fg) paint_dirty();
            break;
        case EDIT_CMD_OPEN:
            /* §A.2 が edit_task から fs_* を呼ぶことを禁じているので、
               読むのは fs_io。ここでやるのはパスを覚えて依頼するまで。
               本文は「読み込み中」を出さずに空にしておく —— 失敗したときに
               古い原稿が残っていると、保存で上書きしてしまう。 */
            s_redirected = false;
            memcpy(s_vpath, c.u.open.vpath, sizeof(s_vpath));
            s_vpath[sizeof(s_vpath) - 1] = '\0';
            edit_set_text(s_ed, "", 0);
            if (s_fg) paint_dirty();
            if (!fsio_submit(FSIO_LOAD, s_vpath, NULL, NULL, 0,
                             io_done, NULL, NULL)) {
                ESP_LOGW(TAG, "open(%s): fs_io のキューが満杯", s_vpath);
                sys_note("開けません (I/O 混雑)");
            }
            break;
        case EDIT_CMD_SAVE:
            save_now();
            break;
        case EDIT_CMD_SAVE_AS:
            if (strlen(c.u.open.vpath) < sizeof s_vpath) {
                memcpy(s_vpath, c.u.open.vpath, sizeof s_vpath);
                s_vpath[sizeof s_vpath - 1] = 0;
                save_now();
            } else {
                sys_note("パスが長すぎます");
            }
            break;
        case EDIT_CMD_LOADED:
            if (c.u.io.err == 0 && c.u.io.buf) {
                edit_err_t e = edit_set_text(s_ed, (const char *)c.u.io.buf,
                                             c.u.io.len);
                if (e != EDIT_OK) {
                    ESP_LOGW(TAG, "set_text: %d", (int)e);
                    sys_note("読めません (中身が大きすぎる/不正)");
                }
            } else {
                ESP_LOGW(TAG, "load failed: %d", (int)c.u.io.err);
                sys_note("開けませんでした");
            }
            free(c.u.io.buf);          /* 所有権はここで終わる */
            if (s_fg) repaint_all();
            break;
        case EDIT_CMD_SAVED:
            if (c.u.io.err == 0) {
                s_redirected = false;
                edit_mark_saved(s_ed);
                sys_note("保存しました");
            } else if (c.u.io.err == ESP_ERR_NOT_ALLOWED && !s_redirected) {
                /* /internal/apps は「読めるが書き換えられない」領域
                   (fs_core §4)。棚から入れたアプリは署名付きで置かれて
                   いるので、ここを上書きできると信用の前提が崩れる。
                   断るだけでは行き止まりなので、**既定の置き場へ名前を
                   保って写す**。
                   **ここではモーダルを出さない。** 名前を選びたいなら
                   Ctrl+W があり、こちらは「断られた原稿を失わせない」ための
                   自動の逃げ道。行き先は毎回同じで、どこへ行ったかを出す。

                   (2026-08-27: ピッカーを edit_task から開くと core 1 が
                   止まることがある —— UI タスクと edit_task が沈黙し
                   core 0 は動き続ける。**SAVE 特有ではなく OPEN でも起き、
                   しかも間欠**。
                   2026-08-31 に真因判明: DMA2D の RX が FIFO 満杯のまま
                   idle で駐車し、PPA 回転の完了通知が永久に来ない。引き金は
                   回転後の 1 行が 14 バイト (出力幅 7) になるチャンクで、
                   これは端数チャンクとして構造的に出る。ui_tab5 側で
                   塞いだ (有界回転 + dma2d_force_end 回収、および 1 文字行
                   未満は PPA を使わない)。
                   それでもここでモーダルを出さない理由は変わらない ——
                   I/O の返事の途中で勝手に画面を奪うのが筋として悪い。
                   名前を選びたいなら Ctrl+W がある。) */
                const char *base = strrchr(s_vpath, '/');
                char alt[EDIT_VPATH_MAX];
                int n = snprintf(alt, sizeof alt, "%s/%s", EDIT_DEFAULT_DIR,
                                 base ? base + 1 : "untitled.js");
                if (n > 0 && (size_t)n < sizeof alt) {
                    memcpy(s_vpath, alt, sizeof s_vpath);
                    s_vpath[sizeof s_vpath - 1] = 0;
                    s_redirected = true;   /* 二度目は普通に失敗させる */
                    save_now();
                    sys_note("元の場所は書き換え不可 → scripts へ保存");
                } else {
                    sys_note("保存できません (パスが長すぎる)");
                }
            } else {
                ESP_LOGW(TAG, "save failed: %d", (int)c.u.io.err);
                sys_note("保存できませんでした");
            }
            if (s_fg) paint_dirty();
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
       取れなくても止めない —— cells_draw が CPU 経路へ落ちるだけ。

       **既定オフ (2026-08-26)。** 実機でスクロール中に LVGL が
       `wait_for_flushing` (lv_refr.c:1442) から永久に戻らなくなる。
       バックトレースで確定:
         wait_for_flushing <- refr_configured_layer <- refr_area
                           <- lv_display_refr_timer <- lvgl_port_task
       flush の完了 (esp_async_fbcpy -> DMA2D の ISR) が来ない。

       見立て: **DMA2D のチャネルは PPA と esp_async_fbcpy で共有**される
       (ppa_srm=tx1/rx1、fill=tx0/rx1、blend=tx2/rx1)。JS の ui.cells は
       UI タスク自身が叩くので flush と自然に直列化するが、**edit_task は
       LVGL の flush と別タスクから同時に PPA を使う最初の利用者**だった。
       スクロールでだけ出るのは、26 段を一度に描いて PPA を連射する唯一の
       場面だから。

       CPU 経路は遅い (1 段の blend が PPA なしでどれだけかかるかは未測) が、
       **固まらない方を取る**。戻すなら DMA2D の共有を理解してから ——
       非ブロッキング + 完了待ち、UI タスクへ描画を移す、flush と排他する、
       のどれかを決めること。 */
/* **1 に戻した。** CPU 描画は遅すぎて試す価値が無い (ユーザ判断)。
   代わりに ui_tab5 の flush 完了待ちへ上限を入れた —— 固まる代わりに
   1 フレーム崩れて先へ進む。原因 (DMA2D の共有) はまだ特定できていないが、
   **完了待ちに上限が無いこと自体が設計の穴**なので、そちらを塞ぐのが筋。 */
#ifndef EDIT_USE_PPA
#define EDIT_USE_PPA 1
#endif
#if EDIT_USE_PPA
    memset(&pc, 0, sizeof(pc));
    pc.oper_type = PPA_OPERATION_BLEND;
    if (ppa_register_client(&pc, &s_ppa) != ESP_OK) {
        s_ppa = NULL;
        ESP_LOGW(TAG, "PPA blend unavailable, cells stay on CPU");
    }
#else
    (void)pc;
    s_ppa = NULL;
    ESP_LOGW(TAG, "PPA disabled for edit_task (flush deadlock); cells on CPU");
#endif

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

/* 明示保存。任意のタスクから。実際の書き込みは fs_io がやる (§A.2)。 */
void edit_ui_save(void)
{
    edit_cmd_t c;

    memset(&c, 0, sizeof c);
    c.kind = EDIT_CMD_SAVE;
    post(&c);
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
