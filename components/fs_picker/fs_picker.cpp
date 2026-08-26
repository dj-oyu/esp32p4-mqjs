/*
 * fs_picker — ネイティブのモーダル・ファイルピッカー / 同意ダイアログ。
 *
 * 仕様 docs/native-editor-spec.md §A.4、設計 docs/native-editor-design.md §4。
 *
 * ---- このファイルの構造 ------------------------------------------
 *
 * 住人は 1 タスクだけ: **UI タスク (esp_lvgl_port)**。
 *
 *   fs_pick_begin()   任意タスク。busy を 1 件だけ取り、要求文字列を
 *                     **同期的に複製**し、LVGL ロックを取って lv_timer を
 *                     resume する。LVGL オブジェクトには一切触らない。
 *   tick_cb()         UI タスク。lv_timer から呼ばれる = lv_timer_handler の
 *                     中 = LVGL ロック保持下。開く / 一覧を作る / 行を叩かれた /
 *                     確定 / 畳む、を 1 tick に 1 段だけ進める。
 *   LVGL イベント     UI タスク。「何をするか」を p->work に書いて timer を
 *                     起こすだけで、**その場では何も壊さない**。
 *
 * この分け方には理由がある。行のタップで一覧を作り直す = イベント配送中に
 * その行を lv_obj_clean() で消す、になり LVGL が落ちる。1 段の遅延を挟むと
 * 「削除するときイベントスタックは空」が構造で保証される。
 *
 * lv_timer を使い、ui_tab5_post_job() を使わないのも理由がある:
 * ui_run_frame_work() はキューを while で**空になるまで**回すので、
 * job から job を投げてもすべて同じフレームで走ってしまい、
 * 「1 フレームに N 行だけ作る」というページングにならない (= LVGL ロックを
 * 手放せない)。lv_timer なら period ごとに 1 回で、フレームを跨げる。
 *
 * ---- タイマの寿命 (ここを間違えるとモーダルが残る) ---------------
 *
 * **ピッカーが出ている間、lv_timer は止めない。** ユーザ待ちのときは
 * 周期を FS_PICK_TICK_MS から FS_PICK_IDLE_MS へ伸ばすだけにする。
 * 止めてしまうと fs_pick_cancel() が立てた印を読む者がいなくなり、
 * モーダルが画面に残ったまま s_busy が true で固まる —— 再起動まで
 * fs.pick も fs.request も false を返し続ける、静かで回復不能な壊れ方に
 * なる (敵対的レビュー S4)。止めてよいのは finish() の後だけ。
 *
 * 同じ理由で fs_pick_cancel() は LVGL ロックを取らない。取れなかった
 * ときに取り消しが届かない、が S4 の穴そのものだった。
 *
 * ---- 枠 (s_busy) を解く順序 --------------------------------------
 *
 * finish() は **cb を呼び終えてから** s_busy を解く。先に解くと、cb が
 * 答えを読んでいる最中に別アプリの fs_pick_begin() が通り、答えの置き場を
 * 上書きできてしまう (UI は core1、JS は core0 で本当に同時に走る)。
 * 代償は「cb の中から次の begin を呼べる」の撤回で、ヘッダに書いてある。
 *
 * ---- ロック予算 (spec §B.2: lvgl_port の保持 8 ms) ----------------
 *
 * ロックは LVGL タスクが自分のフレームのために握っているものを間借りする。
 * こちらが増やすのは「1 tick でする仕事」だけなので、行の生成を
 * FS_PICK_ROWS_PER_TICK 件で切る。**この数字は未測**で、142 列の実機で
 * 1 行あたり何 µs かかるかは分かっていない (openQuestions)。
 *
 * ---- 測るための穴 (spec §B.2 / §F #7) ------------------------------
 *
 * 実機で 1 回開けば FS_PICK_ROWS_PER_TICK / SCAN_PER_TICK / ROWS_MAX /
 * TICK_MS は決まる。そのための 1 行ログを 4 か所に置いてある。TAG は
 * すべて "fs_pick":
 *
 *   begin ...   呼び出し側 (JS タスク) が止まった時間: ロック待ち / 保持
 *   dir_open ...  件数 / truncated / 所要 µs。50 ms 超で行末に印
 *                 (spec §F #7 — 超えるなら M2 の fs_io へ逃がす)
 *   list ...    件数・行数・灰色の行数・チャンク数・合計 µs・
 *               **1 tick の最大ロック保持 µs**・そのときのつまみの値。
 *               8 ms 超で行末に印 (spec §B.2)
 *   open ...    ダイアログが出るまでの所要 = その tick のロック保持
 *
 * さらに 8 ms を超えた tick は "tick work=N held lvgl ... us" を WARN で
 * 1 行出す (毎 tick 出すと一覧の構築でログが溢れるので、超過分だけ)。
 */
#include "sdkconfig.h"

#include "fs_picker.h"

#if CONFIG_MQJS_TAB5_UI

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "fs_core.h"
#include "fs_grant.h" /* FS_OP_* — 同意画面が「何を求められたか」を書くため */
#include "ui_tab5.h"
/* ui_tab5 の非公開ヘッダ。ここから要るのは日本語グリフを持つ
   ui_tab5_jp_font() ただ 1 つ (既定の LVGL フォントでは全部豆腐になる)。
   宣言を写すのではなく本体を include するのは、C++ の名前修飾を
   取り違えないため — ui_tab5.cpp はこのヘッダを extern "C" の外で
   include しているので、この関数は C++ リンケージを持っている。 */
#include "ui_tab5_internal.h"

static const char *TAG = "fs_pick";

/* ---- 調整つまみ (どれも未測。openQuestions を参照) ---------------- */

/* 1 tick で作る行数の上限。LVGL ロックの保持を切る単位。 */
#define FS_PICK_ROWS_PER_TICK 24
/* 1 tick でスナップショットを走査するエントリ数の上限。フィルタで
   落ちる行は作らないので、行数だけで切ると 4096 件の DCIM を 1 tick で
   舐めてしまう。 */
#define FS_PICK_SCAN_PER_TICK 192
/* 一度に画面へ出す行の総数。FS_DIR_MAX (4096) 個の lv_button を作ると
   LVGL のプールも作成時間も持たないので、ここで切って「N 件中 M 件」と
   断る。スクロール駆動の本物のページングは M2 のファイラの仕事。 */
#define FS_PICK_ROWS_MAX 256
/* lv_timer の周期 [ms]。UI のフレームは 16 ms なので、実質フレーム毎。 */
#define FS_PICK_TICK_MS 8
/* ユーザ待ちの間の lv_timer の周期 [ms]。
   **ここで lv_timer_pause を使ってはいけない。** 止めると fs_pick_cancel()
   が立てた印を誰も読まなくなり、モーダルが画面に残ったまま s_busy が
   true で固まる = 再起動まで fs.pick / fs.request が false を返し続ける
   (敵対的レビュー S4)。周期を伸ばすだけにして、tick は必ず回す。
   1 フレームぶん (16 ms) にしてあるので、増える仕事は「フレームごとに
   switch を 1 回」だけ、取り消しの遅れも 1 フレームで済む。 */
#define FS_PICK_IDLE_MS 16
/* fs_dir_open がこれを超えたらログに印を付ける (spec §A.4 の閾値 50 ms)。 */
#define FS_PICK_DIR_WARN_US 50000
/* 1 tick で LVGL ロックに上乗せしてよい時間 (spec §B.2 の 8 ms)。
   超えたらログに印を付ける = FS_PICK_ROWS_PER_TICK を下げる根拠。 */
#define FS_PICK_HOLD_WARN_US 8000
/* fs_pick_begin が LVGL ロックを待つ上限。UI タスクが 1 フレーム
   (16 ms) 握っていることはあるので、その数倍。無限待ちにはしない —
   ピッカーを出せないより、出せなかったと返すほうがまし。
   (fs_pick_cancel はこのロックを取らない。取れなかったときに取り消しが
   届かなくなるため —— S4。) */
#define FS_PICK_LOCK_MS 200

/* 色は ui_widgets.cpp と同じ調色 (別コンポーネントなので値で持つ)。 */
#define PK_COL_BG     0x0B0E11
#define PK_COL_PANEL  0x1A222C
#define PK_COL_TEXT   0xC9D1D9
#define PK_COL_DIM    0x8B98A5
#define PK_COL_ACCENT 0x4FC3F7
#define PK_COL_WARN   0xE5A54B
#define PK_COL_PRESS  0x2E6BD6

/* ---- 状態 -------------------------------------------------------- */

enum {
    W_IDLE = 0,  /* ユーザ待ち。timer は止まっている */
    W_OPEN,      /* 出す */
    W_BUILD,     /* 一覧を 1 チャンク作る */
    W_ROW,       /* p->hit 行が叩かれた */
    W_UP,        /* 「上へ」 */
    W_CONFIRM,   /* 「開く/保存/このフォルダ/許可」 */
    W_ABORT,     /* 「キャンセル/拒否」 */
};

struct Pick {
    /* --- 要求 (fs_pick_begin が複製する。以後 read-only) --- */
    fs_pick_mode_t mode;
    char           title[FS_PICK_TITLE_MAX];
    char           start[FS_PATH_MAX];
    char           suggest[FS_PICK_NAME_MAX];
    char           scope[FS_PICK_SCOPE_MAX];
    char           reason[FS_PICK_REASON_MAX];
    uint8_t        ops;
    char           exts[FS_PICK_EXTS_MAX][FS_PICK_EXT_MAX];
    int            n_exts;
    fs_pick_cb_t   cb;
    void          *ctx;

    /* --- UI タスクだけが触る --- */
    uint8_t   work;
    bool      shown;    /* LVGL オブジェクトを表示済み */
    bool      done;     /* cb を呼び終えた (二重発火よけ) */
    char      cur[FS_PATH_MAX]; /* 今見ているディレクトリ。"" = ボリューム一覧 */
    bool      at_vols;
    fs_dir_t *dir;      /* cur のスナップショット (PSRAM、fs_core が確保) */
    int       n;        /* スナップショットの件数 (at_vols ならボリューム数) */
    int       scanned;  /* 走査済みのエントリ数 */
    int       rows;     /* 実際に作った行数 */
    int       blocked;  /* そのうち「長すぎて押せない」で灰色にした行数 */
    int       chunks;
    int64_t   build_us, build_max_us;
    int64_t   open_us;  /* begin から画面が出るまで (UI タスク側の所要) */
    int       hit;      /* 叩かれた行のスナップショット添字 */

    /* 組み立て用の器。**スタックに置かない**: この構造体を触るのは
       lv_timer のコールバック = LVGL タスクで、そのスタックは 7,168 B
       しかない (ESP_LVGL_PORT_INIT_CONFIG の既定値、ui_tab5 は上書き
       していない)。同じスタックで LVGL の描画も mooncake も term の
       フレームジョブも走るので、512 B の一時バッファを積む場所ではない。
       この構造体自体は PSRAM。 */
    char      scratch[FS_PATH_MAX];
    char      body[FS_PICK_SCOPE_MAX + FS_PICK_REASON_MAX + 320];
    char      msg[192];
    char      row[FS_NAME_MAX + 96]; /* 行のラベル (名前 + 断る理由) */

    /* --- 他タスクから書かれる --- */
    volatile bool cancel;
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool  s_busy;         /* s_mux の下でだけ触る */
static Pick *s_p;            /* PSRAM。初回の begin で確保して以後使い回す */

/* tick_cb が自分で記録する UI タスク。fs_pick_cancel() が「自分は UI
   タスクか」を判定するのに使う (自分なら待たずにその場で畳む)。
   ui_tab5_is_ui_task() と二重に見るのは、あちらが最初のフレームが
   回るまで false を返すため。 */
static TaskHandle_t s_ui_task;

/* 結果は .bss に置く。s_p の中に置くと、cb が読んでいる最中に次の
   fs_pick_begin() の memset に巻き込まれうる。 */
static fs_pick_result_t s_res;

/* LVGL オブジェクトは一度作って使い回す (畳むときは隠すだけ)。
   毎回作り直すと、行のタップで自分を消すことになるのと、
   lv_layer_top の子の並びが picker ごとに変わるのを避けるため。 */
static lv_obj_t  *s_scrim, *s_dlg, *s_title, *s_path, *s_info, *s_note;
static lv_obj_t  *s_list, *s_name, *s_kb;
static lv_obj_t  *s_btn_up, *s_btn_ok, *s_lbl_ok, *s_lbl_cancel;
static lv_timer_t *s_tick;

/* ---- 小道具 ------------------------------------------------------ */

/* ここから下の塊は ESP-IDF にも LVGL にも触らない純粋な関数で、
   pc_test/run_tests.sh が**このマーカーの間をそのまま切り出して**
   ホストの g++ でコンパイルし、叩く。テスト用に書き写したコピーでは
   ないので、ここを直せばテストも同じものを見る。マーカーを消したり
   間に IDF/LVGL を触る関数を入れたりすると run_tests.sh が落ちる。 */
/* >>> host-testable helpers begin <<< */

static void sappend(char *dst, size_t cap, size_t *off, const char *s)
{
    if (*off >= cap)
        return;
    size_t n = strlen(s);
    if (n > cap - 1 - *off)
        n = cap - 1 - *off;
    memcpy(dst + *off, s, n);
    *off += n;
    dst[*off] = '\0';
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 表示用にバイト数で詰める。copy_str と違って UTF-8 の文字の途中では切らない
   —— パスには日本語のフォルダ名が普通に入るので、継続バイトの上で切ると
   壊れた字が 1 つ出る。最後の 1 文字が入りきらないときは、その文字ごと落とす。

   これが要るのは表示だけの都合ではない: 512 バイトのパスを 192 バイトの
   メッセージ器へ "%s" で流すと -Werror=format-truncation= が通らない
   (コンパイラは収まる証明ができない)。呼び出し側はここを通してから渡す。 */
static void clip_utf8(char *dst, size_t cap, const char *src)
{
    copy_str(dst, cap, src);
    size_t n = strlen(dst);
    if (n == 0 || !src || strlen(src) == n)
        return;                 /* 切っていない */

    size_t s = n;
    while (s > 0 && ((unsigned char)dst[s - 1] & 0xc0) == 0x80)
        s--;                    /* 継続バイトを遡って文字の先頭へ */
    if (s == 0) {               /* 先頭バイトが見つからない = 壊れた入力 */
        dst[0] = '\0';
        return;
    }
    s--;                        /* dst[s] が先頭バイト */

    unsigned char c    = (unsigned char)dst[s];
    size_t        need = (c < 0x80)          ? 1
                         : ((c & 0xe0) == 0xc0) ? 2
                         : ((c & 0xf0) == 0xe0) ? 3
                                                : 4;
    if (s + need > n)
        dst[s] = '\0';          /* 最後の 1 文字が欠けている */
}

/* 表示用に 1 行として複製する。制御文字は空白へ潰す。
   reason はアプリが書いた文字列なので、改行を詰めれば「許可 / 拒否」の
   ボタンを画面外へ押し出せてしまう —— 同意画面でそれをさせない。
   潰すのは 0x20 未満と 0x7f だけなので、UTF-8 の後続バイト (>= 0x80) は
   触らない。 */
static void copy_line(char *dst, size_t cap, const char *src)
{
    copy_str(dst, cap, src);
    for (size_t i = 0; dst[i]; i++) {
        unsigned char c = (unsigned char)dst[i];
        if (c < 0x20 || c == 0x7f)
            dst[i] = ' ';
    }
}

/* この道を選んで意味があるか = grant の root の器に収まるか。
 *
 * ピッカーの結果の器 (fs_pick_result_t.vpath = FS_PATH_MAX = 512) と
 * grant の root の器 (FS_PICK_SCOPE_MAX = FS_GRANT_ROOT_MAX = 128) は
 * 別で、runtime は入らなければ ok=false へ倒す (意図的)。倒すこと自体は
 * 正しいが、画面には何も出ないので「タップしたのにキャンセル扱い」に
 * なる —— /sd/DCIM/... で普通に届く長さで起きる (敵対的レビュー S6)。
 * だからピッカー側で、この判定に落ちる行は最初から押させない。
 *
 * strlen < cap は「NUL 込みで器に収まる」と同値 (runtime 側は
 * snprintf の戻り値が器以上なら捨てる、つまり 127 バイトまで)。 */
static bool fits_scope(const char *vpath)
{
    return vpath && vpath[0] && strlen(vpath) < (size_t)FS_PICK_SCOPE_MAX;
}

/* "/a/b" + "c" -> "/a/b/c"。dir の末尾の '/' は落とす。 */
static void join_path(char *dst, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/')
        n--;
    /* "/" だけ残ったら根。ここを 1 のままにすると "//name" を作ってしまい、
       fs_vol.c の path_shape_ok が "//" を拒むので開けないパスになる
       (ホストテストが見つけた。fsvol_resolve も同じ形で 1 を 0 に潰す)。 */
    if (n == 1 && dir[0] == '/')
        n = 0;
    snprintf(dst, cap, "%.*s/%s", (int)n, dir, name);
}

/* path を親へ 1 段上げる。"/internal" の親は "" (= ボリューム一覧)。 */
static void parent_path(char *path)
{
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    while (n > 0 && path[n - 1] != '/')
        n--;
    if (n <= 1) {
        path[0] = '\0'; /* ボリューム一覧へ */
        return;
    }
    path[n - 1] = '\0';
}

/* 名前がフィルタを通るか。exts が空なら全部通す。大文字小文字は無視。 */
static bool ext_ok(const Pick *p, const char *name)
{
    if (p->n_exts <= 0)
        return true;
    size_t nl = strlen(name);
    for (int i = 0; i < p->n_exts; i++) {
        size_t el = strlen(p->exts[i]);
        if (el == 0 || el > nl)
            continue;
        if (strcasecmp(name + nl - el, p->exts[i]) == 0)
            return true;
    }
    return false;
}

/* 保存名として通る形か。'/' を含まない、空でない、全部ドットでない、
   制御文字を含まない。fs_vol.c の path_shape_ok が 1 セグメントに課す
   条件と同じ (通らない名前を作らせて後で ENOENT にするより、ここで断る)。 */
static bool name_ok(const char *s)
{
    if (!s || !s[0])
        return false;
    bool all_dots = true;
    size_t n = 0;
    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        if (c == '/' || c < 0x20 || c == 0x7f)
            return false;
        if (c != '.')
            all_dots = false;
    }
    return !all_dots && n < FS_NAME_MAX;
}

static void ops_text(uint8_t ops, char *dst, size_t cap)
{
    static const struct {
        uint8_t     bit;
        const char *ja;
    } tbl[] = {
        { FS_OP_READ, "読み取り" },   { FS_OP_WRITE, "書き込み" },
        { FS_OP_CREATE, "作成" },     { FS_OP_DELETE, "削除" },
        { FS_OP_RENAME, "名前変更" },
    };
    size_t off = 0;
    dst[0] = '\0';
    for (unsigned i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
        if (!(ops & tbl[i].bit))
            continue;
        if (off)
            sappend(dst, cap, &off, " / ");
        sappend(dst, cap, &off, tbl[i].ja);
    }
    /* 未知のビットを黙って落とさない: 何を求められたか分からないまま
       「許可」を押させないため。 */
    if (ops & (uint8_t)~(uint8_t)FS_OP_ALL) {
        if (off)
            sappend(dst, cap, &off, " / ");
        sappend(dst, cap, &off, "不明な操作");
    }
    if (!off)
        sappend(dst, cap, &off, "(操作の指定なし)");
}

/* >>> host-testable helpers end <<< */

static const char *vol_state_ja(fsvol_state_t st)
{
    switch (st) {
    case FSVOL_ST_MOUNTED:    return "";
    case FSVOL_ST_ABSENT:     return "カードが入っていません";
    case FSVOL_ST_UNREADABLE: return "読めません (未フォーマット?)";
    case FSVOL_ST_UNKNOWN:
    default:                  return "状態不明";
    }
}

static void note(const char *msg)
{
    if (!s_note)
        return;
    if (msg && msg[0]) {
        lv_label_set_text(s_note, msg);
        lv_obj_remove_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 仕事がある。次の lv_timer_handler で必ず 1 回走らせる。
   lv_timer_set_period は last_run を触らないので、周期を変えてから
   lv_timer_ready を呼ぶ順でなければならない (ready が
   last_run = now - period - 1 を書くため)。 */
static void wake(void)
{
    if (s_tick) {
        lv_timer_set_period(s_tick, FS_PICK_TICK_MS);
        lv_timer_resume(s_tick);
        lv_timer_ready(s_tick);
    }
}

/* ユーザ待ちへ。**止めない** (S4)。周期を伸ばすだけにして、
   fs_pick_cancel() が立てた印を必ず次のフレームで読む。 */
static void go_idle(void)
{
    if (s_tick) {
        lv_timer_set_period(s_tick, FS_PICK_IDLE_MS);
        lv_timer_resume(s_tick);
    }
}

/* ---- LVGL イベント (何もせず、やることを書いて起こすだけ) --------- */

static void row_event(lv_event_t *e)
{
    Pick *p = s_p;
    if (!p || p->done || !p->shown)
        return;
    p->hit  = (int)(intptr_t)lv_event_get_user_data(e);
    p->work = W_ROW;
    wake();
}

static void btn_event(lv_event_t *e)
{
    Pick *p = s_p;
    if (!p || p->done || !p->shown)
        return;
    p->work = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    wake();
}

static void scrim_event(lv_event_t *e)
{
    (void)e;
    Pick *p = s_p;
    if (!p || p->done || !p->shown)
        return;
    /* 背後のタップは「取り消し」。ピッカーは何も返さない —— cam の
       viewfinder と同じ web モーダルの作法。 */
    p->work = W_ABORT;
    wake();
}

static void kb_place(bool on);

static void name_event(lv_event_t *e)
{
    (void)e;
    kb_place(true);
}

static void kb_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL)
        kb_place(false);
}

/* ---- 画面 -------------------------------------------------------- */

static lv_obj_t *mk_button(lv_obj_t *parent, const char *text, int work,
                           lv_obj_t **out_label)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 56);
    lv_obj_set_style_bg_color(b, lv_color_hex(PK_COL_PANEL), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(PK_COL_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, btn_event, LV_EVENT_CLICKED,
                        (void *)(intptr_t)work);
    if (out_label)
        *out_label = l;
    return b;
}

/* 呼び出し側は LVGL ロックを保持していること (= UI タスクの上)。 */
static bool build_ui(void)
{
    if (s_dlg)
        return true;

    lv_obj_t *top = lv_layer_top();
    if (!top)
        return false;

    /* 全画面のスクリム。背後の LVGL ウィジェットへタップを通さない
       (ui_tab5_cam_canvas と同じ作法)。 */
    s_scrim = lv_obj_create(top);
    lv_obj_set_size(s_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_set_style_radius(s_scrim, 0, 0);
    lv_obj_set_style_border_width(s_scrim, 0, 0);
    lv_obj_set_style_pad_all(s_scrim, 0, 0);
    lv_obj_set_style_bg_color(s_scrim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_60, 0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_scrim, scrim_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);

    s_dlg = lv_obj_create(top);
    lv_obj_set_size(s_dlg, LV_PCT(92), LV_PCT(86));
    lv_obj_center(s_dlg);
    lv_obj_set_style_radius(s_dlg, 12, 0);
    lv_obj_set_style_bg_color(s_dlg, lv_color_hex(PK_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dlg, 1, 0);
    lv_obj_set_style_border_color(s_dlg, lv_color_hex(PK_COL_DIM), 0);
    lv_obj_set_style_pad_all(s_dlg, 12, 0);
    lv_obj_set_style_pad_row(s_dlg, 8, 0);
    lv_obj_set_style_text_color(s_dlg, lv_color_hex(PK_COL_TEXT), 0);
    /* 日本語グリフ。これを入れないとラベルが全部豆腐になる。 */
    lv_obj_set_style_text_font(s_dlg, ui_tab5_jp_font(), 0);
    lv_obj_set_flex_flow(s_dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_dlg, LV_OBJ_FLAG_SCROLLABLE);
    /* モーダルの中身: タップを吸う (スクリムへ抜けさせない)。 */
    lv_obj_add_flag(s_dlg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dlg, LV_OBJ_FLAG_HIDDEN);

    s_title = lv_label_create(s_dlg);
    lv_obj_set_width(s_title, LV_PCT(100));
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(s_title, "");

    s_path = lv_label_create(s_dlg);
    lv_obj_set_width(s_path, LV_PCT(100));
    lv_label_set_long_mode(s_path, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(s_path, lv_color_hex(PK_COL_DIM), 0);
    lv_label_set_text(s_path, "");

    s_info = lv_label_create(s_dlg);
    lv_obj_set_width(s_info, LV_PCT(100));
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_info, "");
    lv_obj_add_flag(s_info, LV_OBJ_FLAG_HIDDEN);

    s_note = lv_label_create(s_dlg);
    lv_obj_set_width(s_note, LV_PCT(100));
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_note, lv_color_hex(PK_COL_WARN), 0);
    lv_label_set_text(s_note, "");
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

    s_list = lv_list_create(s_dlg);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(PK_COL_PANEL), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);

    s_name = lv_textarea_create(s_dlg);
    lv_textarea_set_one_line(s_name, true);
    lv_textarea_set_max_length(s_name, FS_PICK_NAME_MAX - 1);
    lv_textarea_set_placeholder_text(s_name, "ファイル名");
    lv_obj_set_width(s_name, LV_PCT(100));
    lv_obj_set_style_bg_color(s_name, lv_color_hex(PK_COL_PANEL), 0);
    lv_obj_set_style_border_color(s_name, lv_color_hex(PK_COL_ACCENT),
                                  LV_STATE_FOCUSED);
    lv_obj_add_event_cb(s_name, name_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_name, name_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_flag(s_name, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *foot = lv_obj_create(s_dlg);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(foot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(foot, 8, 0);

    mk_button(foot, "キャンセル", W_ABORT, &s_lbl_cancel);
    s_btn_up = mk_button(foot, LV_SYMBOL_LEFT " 上へ", W_UP, NULL);
    s_btn_ok = mk_button(foot, "開く", W_CONFIRM, &s_lbl_ok);
    lv_obj_set_style_bg_color(s_btn_ok, lv_color_hex(PK_COL_ACCENT), 0);
    lv_obj_set_style_text_color(s_btn_ok, lv_color_hex(0x08131A), 0);

    return true;
}

/* SAVE の名前入力のためのソフトキーボード。**プラットフォーム鍵盤
   (ui_tab5 の on-screen keyboard / ドック / SKK) には載っていない** ——
   あの経路の入口 (ui_tab5_kb_field) は ui_tab5 の非公開 API で、
   fs_picker からは開けられない。M1 は LVGL 素の lv_keyboard で凌ぐので
   日本語のファイル名は打てない (openQuestions)。 */
static void kb_place(bool on)
{
    Pick *p = s_p;
    if (!p || p->mode != FS_PICK_SAVE)
        on = false;
    if (on) {
        if (!s_kb) {
            s_kb = lv_keyboard_create(lv_layer_top());
            lv_obj_set_size(s_kb, LV_PCT(100), LV_PCT(40));
            lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_add_event_cb(s_kb, kb_event, LV_EVENT_READY, NULL);
            lv_obj_add_event_cb(s_kb, kb_event, LV_EVENT_CANCEL, NULL);
        }
        lv_keyboard_set_textarea(s_kb, s_name);
        lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_kb);
        /* ダイアログを上へ寄せて鍵盤に隠れないようにする */
        lv_obj_set_height(s_dlg, LV_PCT(50));
        lv_obj_align(s_dlg, LV_ALIGN_TOP_MID, 0, 96);
    } else {
        if (s_kb) {
            lv_keyboard_set_textarea(s_kb, NULL);
            lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_dlg) {
            lv_obj_set_height(s_dlg, LV_PCT(86));
            lv_obj_center(s_dlg);
        }
    }
}

/* ---- 一覧 -------------------------------------------------------- */

static void close_dir(Pick *p)
{
    if (p->dir) {
        fs_dir_close(p->dir);
        p->dir = NULL;
    }
}

/* vpath ("" ならボリューム一覧) を開き、一覧の作り直しを予約する。 */
static void enter_dir(Pick *p, const char *vpath)
{
    close_dir(p);
    copy_str(p->cur, sizeof p->cur, vpath);
    p->at_vols = (p->cur[0] == '\0');
    p->n = p->scanned = p->rows = p->blocked = p->chunks = 0;
    p->build_us = p->build_max_us = 0;

    if (p->at_vols) {
        p->n = fsvol_count();
        lv_label_set_text(s_path, "ボリューム");
    } else {
        int64_t   t0  = esp_timer_get_time();
        fs_dir_t *d   = NULL;
        esp_err_t err = fs_dir_open(p->cur, &d);
        int64_t   dt  = esp_timer_get_time() - t0;
        int       n   = fs_dir_count(d);
        /* spec §F #7: この 1 行が「UI タスクで開いてよいか」の判断材料。 */
        ESP_LOGI(TAG, "dir_open %s -> %s n=%d trunc=%d %lld us%s", p->cur,
                 err == ESP_OK ? "ok" : fs_err_str(err), n,
                 fs_dir_truncated(d) ? 1 : 0, (long long)dt,
                 dt > FS_PICK_DIR_WARN_US
                     ? "  ** >50ms: move to fs_io (spec A.4/F#7) **"
                     : "");
        if (err != ESP_OK) {
            char shown[128];
            clip_utf8(shown, sizeof shown, p->cur);
            snprintf(p->msg, sizeof p->msg, "%s を開けません: %s", shown,
                     fs_err_str(err));
            note(p->msg);
            /* 開けない場所に留まっても出口が無い。ボリューム一覧へ戻す。 */
            copy_str(p->cur, sizeof p->cur, "");
            p->at_vols = true;
            p->n       = fsvol_count();
            lv_label_set_text(s_path, "ボリューム");
        } else {
            p->dir = d;
            p->n   = n;
            lv_label_set_text(s_path, p->cur);
            if (fs_dir_truncated(d))
                note("このフォルダは大きすぎるので途中までしか読んでいません");
        }
    }

    lv_obj_clean(s_list);
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    p->work = W_BUILD;
}

static void add_row(Pick *p, int snap_idx, const char *icon, const char *text,
                    bool enabled)
{
    lv_obj_t *b = lv_list_add_button(s_list, icon, text);
    if (!b)
        return;
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(PK_COL_PANEL), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(PK_COL_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, lv_color_hex(enabled ? PK_COL_TEXT
                                                        : PK_COL_DIM), 0);
    lv_obj_set_style_transform_width(b, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(b, 0, LV_STATE_PRESSED);
    if (enabled)
        lv_obj_add_event_cb(b, row_event, LV_EVENT_CLICKED,
                            (void *)(intptr_t)snap_idx);
    else
        lv_obj_add_state(b, LV_STATE_DISABLED);
    p->rows++;
}

/* 「選んでも runtime に捨てられる」行を、断る理由を付けて灰色で置く
   (S6)。理由は行そのものに書く —— 押しても何も起きない行の理由は、
   その行の隣にしか置き場が無い。 */
static void add_blocked_row(Pick *p, const char *icon, const char *name)
{
    snprintf(p->row, sizeof p->row,
             "%s  — パスが長すぎて選べません (%d 文字まで)", name,
             (int)FS_PICK_SCOPE_MAX - 1);
    add_row(p, -1, icon, p->row, false);
    p->blocked++;
}

static void build_chunk(Pick *p)
{
    int64_t t0    = esp_timer_get_time();
    int     rows0 = p->rows;
    int     scan0 = p->scanned;

    while (p->scanned < p->n && p->rows < FS_PICK_ROWS_MAX &&
           p->rows - rows0 < FS_PICK_ROWS_PER_TICK &&
           p->scanned - scan0 < FS_PICK_SCAN_PER_TICK) {
        int i = p->scanned++;
        if (p->at_vols) {
            const fsvol_t *v = fsvol_at(i);
            if (!v)
                continue;
            fsvol_state_t st = fsvol_state(v);
            char         *line = p->msg;   /* スタックに 160 B 積まない */
            if (st == FSVOL_ST_MOUNTED) {
                uint64_t  total = 0, freeb = 0;
                esp_err_t ue = fs_usage(v->id, &total, &freeb);
                if (ue == ESP_OK && total)
                    snprintf(line, sizeof p->msg, "%s  (%s, 空き %llu/%llu MB)",
                             v->label ? v->label : v->id,
                             v->fstype ? v->fstype : "?",
                             (unsigned long long)(freeb >> 20),
                             (unsigned long long)(total >> 20));
                else
                    snprintf(line, sizeof p->msg, "%s  (%s)",
                             v->label ? v->label : v->id,
                             v->fstype ? v->fstype : "?");
            } else {
                snprintf(line, sizeof p->msg, "%s  — %s",
                         v->label ? v->label : v->id, vol_state_ja(st));
            }
            /* 未マウントの行は押せない。ここでマウントを試すと
               fsvol_mount() の 150 ms 級のブロッキング呼び出しが UI タスクを
               止める (design §5 が fs.mount を JS から削ったのと同じ理由)。
               M2 の fs_io がマウントを持つまでは、状態を出すだけにする。 */
            add_row(p, i,
                    (v->flags & FSVOL_REMOVABLE) ? LV_SYMBOL_SD_CARD
                                                 : LV_SYMBOL_DRIVE,
                    line, st == FSVOL_ST_MOUNTED);
        } else {
            fs_entry_t e;
            if (!fs_dir_get(p->dir, i, &e))
                continue;
            if (!e.is_dir) {
                if (p->mode == FS_PICK_DIR)
                    continue;           /* フォルダを選ぶ画面に file は出さない */
                if (!ext_ok(p, e.name))
                    continue;
            }
            const char *icon = e.is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
            /* この行を選んだら答えは何になるか、を先に組み立てて器に
               入るか見る (S6)。フォルダは「入るための行」でもあるので、
               フォルダそのものが答えになる DIR モードでだけ塞ぐ ——
               OPEN/SAVE では長いフォルダにも入れる (中身の名前が短ければ
               答えは器に収まる)。 */
            join_path(p->scratch, sizeof p->scratch, p->cur, e.name);
            bool nav_only = e.is_dir && p->mode != FS_PICK_DIR;
            if (!nav_only && !fits_scope(p->scratch))
                add_blocked_row(p, icon, e.name);
            else
                add_row(p, i, icon, e.name, true);
        }
    }

    int64_t dt = esp_timer_get_time() - t0;
    p->build_us += dt;
    if (dt > p->build_max_us)
        p->build_max_us = dt;
    p->chunks++;

    bool more = (p->scanned < p->n) && (p->rows < FS_PICK_ROWS_MAX);
    if (more) {
        p->work = W_BUILD;
        return;
    }
    if (p->rows >= FS_PICK_ROWS_MAX && p->scanned < p->n) {
        snprintf(p->msg, sizeof p->msg,
                 "%d 件のうち先頭 %d 件だけ表示しています", p->n, p->rows);
        note(p->msg);
    }
    /* spec §B.2: 1 チャンクの所要が、この tick で LVGL ロックに上乗せした
       時間そのもの。監督が実機でこの 1 行を見て
       FS_PICK_ROWS_PER_TICK / FS_PICK_SCAN_PER_TICK / FS_PICK_ROWS_MAX /
       FS_PICK_TICK_MS を決める。件数・所要 ms・ロック保持 ms が 1 行に
       揃っていること。 */
    ESP_LOGI(TAG,
             "list %s n=%d rows=%d blocked=%d scanned=%d chunks=%d "
             "build=%lld us hold_max=%lld us "
             "[rows/tick=%d scan/tick=%d rows_max=%d tick=%d ms]%s",
             p->at_vols ? "(vols)" : p->cur, p->n, p->rows, p->blocked,
             p->scanned, p->chunks, (long long)p->build_us,
             (long long)p->build_max_us, FS_PICK_ROWS_PER_TICK,
             FS_PICK_SCAN_PER_TICK, FS_PICK_ROWS_MAX, FS_PICK_TICK_MS,
             p->build_max_us > FS_PICK_HOLD_WARN_US
                 ? "  ** >8ms lock hold: lower FS_PICK_ROWS_PER_TICK "
                   "(spec B.2) **"
                 : "");
    p->work = W_IDLE;
}

/* ---- モードごとの見た目 ------------------------------------------ */

static void apply_mode(Pick *p)
{
    const char *ok_text = "開く";
    bool        want_list = true, want_name = false, want_info = false;
    bool        want_ok = true, want_up = true;
    const char *cancel_text = "キャンセル";

    switch (p->mode) {
    case FS_PICK_OPEN:
        /* 確定はファイルのタップそのもの。OK ボタンは出さない
           (「選んでから OK」の 2 段にするかは実機の手応え待ち)。 */
        want_ok = false;
        break;
    case FS_PICK_SAVE:
        ok_text   = "保存";
        want_name = true;
        break;
    case FS_PICK_DIR:
        ok_text = "このフォルダ";
        break;
    case FS_PICK_CONSENT:
    default:
        ok_text     = "許可";
        cancel_text = "拒否";
        want_list   = false;
        want_up     = false;
        want_info   = true;
        break;
    }

    lv_label_set_text(s_lbl_ok, ok_text);
    lv_label_set_text(s_lbl_cancel, cancel_text);
    if (want_ok)
        lv_obj_remove_flag(s_btn_ok, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_btn_ok, LV_OBJ_FLAG_HIDDEN);
    if (want_up)
        lv_obj_remove_flag(s_btn_up, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_btn_up, LV_OBJ_FLAG_HIDDEN);
    if (want_list)
        lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    if (want_name) {
        lv_obj_remove_flag(s_name, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(s_name, p->suggest);
    } else {
        lv_obj_add_flag(s_name, LV_OBJ_FLAG_HIDDEN);
    }
    if (want_info)
        lv_obj_remove_flag(s_info, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_info, LV_OBJ_FLAG_HIDDEN);
}

static void do_open(Pick *p)
{
    int64_t t0 = esp_timer_get_time();

    if (!build_ui()) {
        p->work = W_ABORT;
        return;
    }
    note(NULL);
    apply_mode(p);
    kb_place(false);

    if (p->mode == FS_PICK_CONSENT) {
        ops_text(p->ops, p->msg, sizeof p->msg);
        /* 理由 (fs.request({reason})) を先に出す。旧ランチャーの同意画面が
           出していたもので、ネイティブ化のときに落ちていた。理由が無ければ
           欄ごと出さない —— 空の見出しは「理由が無い」を「理由が空」に
           見せかけるだけなので。 */
        snprintf(p->body, sizeof p->body,
                 "%s%s%sこのアプリが求めている範囲:\n  %s\n\n"
                 "できるようになる操作:\n  %s",
                 p->reason[0] ? "理由:\n  " : "",
                 p->reason[0] ? p->reason : "",
                 p->reason[0] ? "\n\n" : "",
                 p->scope[0] ? p->scope : "(範囲の指定なし)", p->msg);
        lv_label_set_text(s_info, p->body); /* ラベルは複製を持つ */
        snprintf(p->body, sizeof p->body,
                 "「%s」がファイルへの許可を求めています",
                 p->title[0] ? p->title : "(名前のないアプリ)");
        lv_label_set_text(s_title, p->body);
        lv_label_set_text(s_path, "");
        /* 予約サブツリーは grant より外側の不変条件 (design §4.6)。
           そこに許可を出しても fs_core が跳ね返すので、先に言う。 */
        if (p->scope[0] && fs_path_reserved(p->scope))
            note("この範囲は書き換えできません (署名済みアプリの置き場)");
        p->work = W_IDLE;
    } else {
        lv_label_set_text(s_title, p->title[0] ? p->title
                                               : (p->mode == FS_PICK_SAVE
                                                      ? "保存先を選ぶ"
                                                      : "ファイルを選ぶ"));
        enter_dir(p, p->start);
    }

    lv_obj_remove_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_dlg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_scrim);
    lv_obj_move_foreground(s_dlg);
    p->shown  = true;
    p->open_us = esp_timer_get_time() - t0;

    /* 「起動時」の 1 行 (spec §B.2 / §F #7)。この tick で LVGL ロックに
       上乗せした時間 = そのまま open の所要。fs_dir_open の内訳は直前の
       dir_open 行にある。 */
    ESP_LOGI(TAG,
             "open mode=%d n=%d open=%lld us (lock hold) [idle=%d ms "
             "tick=%d ms budget=%d us]%s",
             (int)p->mode, p->n, (long long)p->open_us, FS_PICK_IDLE_MS,
             FS_PICK_TICK_MS, FS_PICK_HOLD_WARN_US,
             p->open_us > FS_PICK_HOLD_WARN_US
                 ? "  ** >8ms lock hold at open (spec B.2) **"
                 : "");
}

/* ---- 確定と後始末 ------------------------------------------------ */

/* **finish() から戻ったあと p を触ってはいけない。**
 *
 * この関数は最後に busy を解く。解いた瞬間から、別のタスクの
 * fs_pick_begin() が *p を memset して次の要求で埋める権利を持つ
 * (そのタスクは LVGL ロックで待たされるが、memset はロックの外でやる)。
 * cb と ctx はスタックへ写してあり、結果は静的な s_res なので
 * cb 側は無事だが、p は無事ではない。
 *
 * 現在の呼び出し元 (tick_step / do_row / do_confirm / fs_pick_cancel) は
 * いずれも finish の直後に return / break していて、これを守っている。
 */
static void finish(Pick *p, bool ok, const char *vpath)
{
    if (p->done)
        return;
    p->done = true;

    /* ここで止めてよい: 畳み終わっているので、以後 cancel の印を読む
       相手がいなくても困らない (次の begin が wake() で起こす)。
       ピッカーが**出ている間**は止めないこと —— S4。 */
    if (s_tick)
        lv_timer_pause(s_tick);
    kb_place(false);
    if (s_dlg)
        lv_obj_add_flag(s_dlg, LV_OBJ_FLAG_HIDDEN);
    if (s_scrim)
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    if (s_list)
        lv_obj_clean(s_list);
    close_dir(p);
    p->shown = false;
    p->work  = W_IDLE;

    s_res.ok = ok;
    copy_str(s_res.vpath, sizeof s_res.vpath, ok && vpath ? vpath : "");

    fs_pick_cb_t cb  = p->cb;
    void        *ctx = p->ctx;
    p->cb            = NULL;

    if (cb)
        cb(ctx, &s_res);

    /* **busy は cb の後に解く** (敵対的レビュー S3)。
       先に解くと、cb が答えを読んでいる最中に別アプリの fs.pick が
       通ってしまい、その begin が答えの置き場 (runtime 側の landing) を
       上書きできる = 答えが別のワーカーへ配送されうる。UI(core1) と
       JS(core0) は本当に同時に走るので、これは机上の窓ではない。
       代償はヘッダに書いてあった「cb の中から次の begin を呼べる」の
       撤回で、そちらは呼び出し側が自分のキューを 1 段挟めば済む。 */
    taskENTER_CRITICAL(&s_mux);
    s_busy = false;
    taskEXIT_CRITICAL(&s_mux);
}

/* 「選べない、なぜなら長すぎる」を画面に出す。黙って取り消し扱いに
   しないための一行 (S6)。 */
static void too_long_note(Pick *p)
{
    snprintf(p->msg, sizeof p->msg,
             "パスが長すぎて選べません (%d 文字まで)。"
             "上のフォルダか短い名前にしてください",
             (int)FS_PICK_SCOPE_MAX - 1);
    note(p->msg);
}

static void do_row(Pick *p)
{
    p->work = W_IDLE;
    if (p->at_vols) {
        const fsvol_t *v = fsvol_at(p->hit);
        if (!v)
            return;
        snprintf(p->scratch, sizeof p->scratch, "/%s", v->id);
        note(NULL);
        enter_dir(p, p->scratch);
        return;
    }
    fs_entry_t e;
    if (!p->dir || !fs_dir_get(p->dir, p->hit, &e))
        return;

    join_path(p->scratch, sizeof p->scratch, p->cur, e.name);
    if (e.is_dir) {
        note(NULL);
        enter_dir(p, p->scratch);
        return;
    }
    switch (p->mode) {
    case FS_PICK_OPEN:
        /* 器に入らない行はそもそも押せない (build_chunk が灰色にする)。
           ここでも見るのは、一覧の作り方と確定の判断のどちらか片方に
           頼らないため —— 片方が緩んだときに黙って「タップしたのに
           取り消し」へ戻らないように。 */
        if (!fits_scope(p->scratch)) {
            too_long_note(p);
            break;
        }
        finish(p, true, p->scratch);
        break;
    case FS_PICK_SAVE:
        /* 既存ファイルのタップは「そこへ上書き保存」の候補。確定は
           「保存」ボタン —— 1 タップで上書きが決まる作りにはしない。 */
        lv_textarea_set_text(s_name, e.name);
        note("同じ名前のファイルがあります (保存すると置き換わります)");
        break;
    default:
        break;
    }
}

static void do_confirm(Pick *p)
{
    p->work = W_IDLE;

    switch (p->mode) {
    case FS_PICK_CONSENT:
        finish(p, true, p->scope);
        return;
    case FS_PICK_DIR:
        if (p->at_vols) {
            note("フォルダを開いてから選んでください");
            return;
        }
        if (!fits_scope(p->cur)) {
            too_long_note(p);
            return;
        }
        finish(p, true, p->cur);
        return;
    case FS_PICK_SAVE: {
        if (p->at_vols) {
            note("保存するフォルダを開いてください");
            return;
        }
        const char *name = lv_textarea_get_text(s_name);
        if (!name_ok(name)) {
            note("その名前では保存できません ('/' や '.' だけの名前は不可)");
            return;
        }
        join_path(p->scratch, sizeof p->scratch, p->cur, name);
        /* 予約サブツリー (design §4.6) は grant の外側の不変条件。
           ここで止めなくても fs_core が ESP_ERR_NOT_ALLOWED を返すが、
           「保存できたつもり」で先へ進ませないために先に断る。 */
        if (fs_path_reserved(p->scratch)) {
            note("この場所へは書き込めません (署名済みアプリの置き場)");
            return;
        }
        /* 名前は打ち込めるので、一覧を灰色にするだけでは塞ぎ切れない
           (S6)。ここで断らないと「保存」を押しても黙って取り消しになる。 */
        if (!fits_scope(p->scratch)) {
            too_long_note(p);
            return;
        }
        finish(p, true, p->scratch);
        return;
    }
    case FS_PICK_OPEN:
    default:
        /* OPEN はファイルのタップで確定する。ここへは来ない
           (OK ボタンを隠してある) が、来ても何もしない。 */
        return;
    }
}

/* 1 tick ぶんを進める。**戻ったあと p を触ってはいけない** (finish が
   走っていれば p はもう次の要求のものかもしれない)。 */
static void tick_step(lv_timer_t *t)
{
    Pick *p = s_p;
    /* cb を持たない = 生きている要求ではない (未使用、畳み終わり、または
       begin がロック待ちで諦めた跡)。出ていないときだけ止めてよい —— この
       条件を緩めると W_IDLE で止まる形が復活して S4 に戻る。 */
    if (!p || p->done || !p->cb) {
        lv_timer_pause(t);
        return;
    }
    /* 取り消しは他のどの仕事よりも先に見る。ここへ来られない状態
       (= timer が止まっている) を作らないことが S4 の核心で、
       W_IDLE でも止めずに周期を伸ばすだけにしてある。 */
    if (p->cancel) {
        p->cancel = false;
        finish(p, false, NULL);
        return;
    }
    switch (p->work) {
    case W_OPEN:
        p->work = W_IDLE;
        do_open(p);
        break;
    case W_BUILD:
        p->work = W_IDLE;
        build_chunk(p);
        break;
    case W_ROW:
        do_row(p);
        break;
    case W_UP:
        p->work = W_IDLE;
        if (!p->at_vols) {
            copy_str(p->scratch, sizeof p->scratch, p->cur);
            parent_path(p->scratch);
            note(NULL);
            enter_dir(p, p->scratch);
        }
        break;
    case W_CONFIRM:
        do_confirm(p);
        break;
    case W_ABORT:
        finish(p, false, NULL);
        break;
    case W_IDLE:
    default:
        /* ユーザ待ち。**止めない** —— 止めると fs_pick_cancel() の印を
           読む者がいなくなり、モーダルが残ったまま二度と pick が通らなく
           なる (S4)。周期を伸ばすだけにして、イベントは wake() で縮める。 */
        go_idle();
        break;
    }
}

static void tick_cb(lv_timer_t *t)
{
    /* この関数の所要が、そのまま LVGL ロックへの上乗せ (spec §B.2 の
       8 ms 予算)。実機で FS_PICK_* を決めるために、超えた tick だけ
       1 行残す —— 毎 tick 出すとログが一覧の構築で溢れる。 */
    s_ui_task = xTaskGetCurrentTaskHandle();
    int64_t t0 = esp_timer_get_time();
    uint8_t w0 = s_p ? s_p->work : (uint8_t)W_IDLE;

    tick_step(t);   /* この後 s_p を読まないこと */

    int64_t dt = esp_timer_get_time() - t0;
    if (dt > FS_PICK_HOLD_WARN_US)
        ESP_LOGW(TAG, "tick work=%d held lvgl %lld us (budget %d us)",
                 (int)w0, (long long)dt, FS_PICK_HOLD_WARN_US);
}

/* ---- 公開 API ---------------------------------------------------- */

bool fs_pick_begin(const fs_pick_req_t *req, fs_pick_cb_t cb, void *ctx)
{
    if (!req || !cb)
        return false;
    if (req->mode != FS_PICK_OPEN && req->mode != FS_PICK_SAVE &&
        req->mode != FS_PICK_DIR && req->mode != FS_PICK_CONSENT)
        return false;
    /* CONSENT の範囲は切り詰めない。切り詰めると "/sd/long/dir/x" が
       "/sd/long/dir/" になって、**求められたより広い**範囲を画面に出し、
       その広い範囲で許可を取ることになる。器に入らないなら断る。 */
    if (req->mode == FS_PICK_CONSENT && !fits_scope(req->scope_vpath))
        return false;

    /* UI が上がっていなければ出せない。lvgl_port_lock() は
       lvgl_port_init 前に呼ぶと assert で落ちるので、その前に断る
       (ui_tab5_canvas_size は表示が上がってから非 0 になる)。 */
    int cw = 0, ch = 0;
    ui_tab5_canvas_size(&cw, &ch);
    if (cw <= 0 || ch <= 0)
        return false;

    taskENTER_CRITICAL(&s_mux);
    bool mine = !s_busy;
    if (mine)
        s_busy = true;
    taskEXIT_CRITICAL(&s_mux);
    if (!mine)
        return false; /* モーダルは同時に 1 件 */

    Pick *p = s_p;
    if (!p) {
        p = (Pick *)heap_caps_calloc(1, sizeof *p, MALLOC_CAP_SPIRAM);
        if (!p)
            p = (Pick *)calloc(1, sizeof *p);
        if (!p) {
            taskENTER_CRITICAL(&s_mux);
            s_busy = false;
            taskEXIT_CRITICAL(&s_mux);
            return false;
        }
        s_p = p;
    }
    memset(p, 0, sizeof *p);

    /* 要求の文字列は呼び出し側の寿命なので、ここで複製し切る。 */
    p->mode = req->mode;
    p->ops  = req->ops;
    copy_str(p->title, sizeof p->title, req->title);
    copy_str(p->start, sizeof p->start, req->start_vpath);
    copy_str(p->suggest, sizeof p->suggest, req->suggest_name);
    copy_str(p->scope, sizeof p->scope, req->scope_vpath);
    /* reason はアプリが書いた文字列なので 1 行へ潰して複製する
       (改行でボタンを画面外へ押し出させない)。 */
    copy_line(p->reason, sizeof p->reason, req->reason);
    if (req->exts && req->n_exts > 0) {
        int n = req->n_exts;
        if (n > FS_PICK_EXTS_MAX)
            n = FS_PICK_EXTS_MAX;
        for (int i = 0; i < n; i++) {
            /* 空文字列のフィルタは「全部通す」と区別が付かないので捨てる
               (ext_ok が長さ 0 を弾くのと二重に見えるが、ここで捨てて
               おかないと n_exts>0 なのに何も通らない一覧になる) */
            if (!req->exts[i] || !req->exts[i][0])
                continue;
            copy_str(p->exts[p->n_exts], sizeof p->exts[0], req->exts[i]);
            p->n_exts++;
        }
    }
    p->cb  = cb;
    p->ctx = ctx;

    int64_t t_req = esp_timer_get_time();
    bool    got   = lvgl_port_lock(FS_PICK_LOCK_MS);
    int64_t t_got = esp_timer_get_time();
    if (!got) {
        ESP_LOGW(TAG, "lvgl lock timeout (%d ms); picker not shown",
                 FS_PICK_LOCK_MS);
        p->cb = NULL;
        taskENTER_CRITICAL(&s_mux);
        s_busy = false;
        taskEXIT_CRITICAL(&s_mux);
        return false;
    }
    if (!s_tick)
        s_tick = lv_timer_create(tick_cb, FS_PICK_TICK_MS, NULL);
    if (!s_tick) {
        lvgl_port_unlock();
        p->cb = NULL;
        taskENTER_CRITICAL(&s_mux);
        s_busy = false;
        taskEXIT_CRITICAL(&s_mux);
        return false;
    }
    p->work = W_OPEN;
    wake();
    lvgl_port_unlock();

    /* 呼び出し側 (たいてい JS タスク) がここで止まった時間。ロック待ちが
       効いてくるなら FS_PICK_LOCK_MS を下げる根拠になる。 */
    ESP_LOGI(TAG, "begin mode=%d lock_wait=%lld us held=%lld us",
             (int)req->mode, (long long)(t_got - t_req),
             (long long)(esp_timer_get_time() - t_got));
    return true;
}

/* 自分は UI タスクか。tick_cb が自分で覚えた手掛かりと ui_tab5 の
   公開判定を両方見る (前者は最初の tick まで、後者は最初のフレームまで
   埋まらないので、片方だけでは早い時期に取りこぼす)。 */
static bool on_ui_task(void)
{
    if (ui_tab5_is_ui_task())
        return true;
    return s_ui_task && xTaskGetCurrentTaskHandle() == s_ui_task;
}

void fs_pick_cancel(void)
{
    /* 印を立てるところまでは s_busy と同じ排他の下でやる。読んでから
       書くまでの隙に別の begin が枠を取ると、そちらを畳んでしまう。 */
    taskENTER_CRITICAL(&s_mux);
    bool  active = s_busy && s_p;
    Pick *p      = s_p;
    if (active)
        p->cancel = true;
    taskEXIT_CRITICAL(&s_mux);
    if (!active)
        return;

    /* UI タスクの上なら (cb の中を含む) その場で畳む。ここで待ちに
       入ると、畳む相手＝自分なので永久に待つことになる。
       UI タスクの上ということは lv_timer_handler / ui_tab5 のジョブの
       中ということで、LVGL ロックは既に自分が握っている。 */
    if (on_ui_task()) {
        p->cancel = false;
        finish(p, false, NULL);   /* この後 p を触らない */
        return;
    }

    /* 他タスク。**LVGL ロックは取らない** —— 取れなかったときに
       取り消しが誰にも届かなくなるのが S4 の穴そのものだった。印は
       もう立っていて、ピッカーが出ている間 tick は止まらない
       (FS_PICK_IDLE_MS) ので、UI タスクが回れば必ず読まれる。
       ここで待つのは「戻った時点で次の begin が通る」を成り立たせる
       ため (S5)。待ちは 1〜2 フレームで終わるのが普通。 */
    int64_t    t0   = esp_timer_get_time();
    TickType_t step = pdMS_TO_TICKS(2);
    if (step == 0)
        step = 1;   /* CONFIG_FREERTOS_HZ=100 では 2 ms は 0 tick に潰れる */
    TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(FS_PICK_CANCEL_WAIT_MS);
    for (;;) {
        taskENTER_CRITICAL(&s_mux);
        bool busy = s_busy;
        taskEXIT_CRITICAL(&s_mux);
        if (!busy) {
            ESP_LOGI(TAG, "cancel: folded in %lld us",
                     (long long)(esp_timer_get_time() - t0));
            return;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            /* UI タスクが回っていない。印は立ったままなので、回り出した
               時点で畳まれて cb も撃たれる —— 残るのは「その窓の間だけ
               次の begin が false」で、以前のような回復不能な固まりでは
               ない。 */
            ESP_LOGW(TAG,
                     "cancel: UI task did not fold within %d ms; the picker "
                     "will fold as soon as it runs (next begin returns false "
                     "until then)",
                     FS_PICK_CANCEL_WAIT_MS);
            return;
        }
        vTaskDelay(step);
    }
}

#else /* CONFIG_MQJS_TAB5_UI = n (Stamp など画面の無いボード) */

/* 画面が無い = 誰にも尋ねられない。既定は「尋ねられないなら通さない」
   (mqjs_runtime.c の fs.request がランチャー不在のとき拒否に倒すのと
   同じ判断)。cb は呼ばれない — begin が false を返すので。 */
bool fs_pick_begin(const fs_pick_req_t *req, fs_pick_cb_t cb, void *ctx)
{
    (void)req;
    (void)cb;
    (void)ctx;
    return false;
}

void fs_pick_cancel(void) {}

#endif /* CONFIG_MQJS_TAB5_UI */
