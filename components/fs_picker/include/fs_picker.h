/*
 * fs_picker — ネイティブのモーダル・ファイルピッカー / 同意ダイアログ。
 *
 * 仕様: docs/native-editor-spec.md §A.4、設計: docs/native-editor-design.md §4。
 *
 * **ピッカーは grant を知らない。** 返すのは「ユーザがこのパスをこの意図で
 * 選んだ」という事実だけで、grant の mint は今までどおり JS タスク
 * (mqjs_runtime.c の dispatch_fsgrant) がやる。権限表の単一所有者を崩さない
 * ためで、ここに 2 つ目の権限概念を作らない (design §4.2)。
 *
 * **ネイティブモーダルである理由** (design §4.4): ランチャーは JS 協調タスク
 * 上のアプリなので、他アプリが長い C 呼び出しをしている間は同意画面を描け
 * ない (カタログ検証で 1.8 秒フリーズを観測済み)。このダイアログは UI タスク
 * (esp_lvgl_port) の上で独立に動くので、JS が詰まっていても出せる。
 *
 * 画面は ui_tab5_cam_canvas と同じ「スクリム + lv_layer_top に載る LVGL
 * オブジェクト」の作法で組む。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs_core.h"    /* FS_PATH_MAX */
#include "fs_grant.h"  /* FS_GRANT_ROOT_MAX — grant の器の**唯一の**持ち主 */

#ifdef __cplusplus
extern "C" {
#endif

/* 要求の中の文字列は fs_pick_begin() の中で**同期的に複製**される
   (begin から実際の描画までは UI タスクの次フレームまで遅れるので、
   呼び出し側のバッファを覚えておくわけにはいかない)。器を超える分は
   切り捨てる。 */
#define FS_PICK_TITLE_MAX  64    /* title の器 (バイト、NUL 込み) */
#define FS_PICK_NAME_MAX   128   /* suggest_name の器 */
#define FS_PICK_EXTS_MAX   6     /* 覚えられる拡張子の本数 */
#define FS_PICK_EXT_MAX    12    /* 拡張子 1 本の器 (".json" など) */
/* reason の器。runtime 側の MQJS_FS_REASON_MAX (96) より大きいので、
   fs.request({reason}) の文字列がここで切り詰められることはない。 */
#define FS_PICK_REASON_MAX 128

/* 選んだ結果が最後に収まる grant の root の器。
   **この数値をここに書かない。** 持ち主は fs_grant.h の FS_GRANT_ROOT_MAX
   ただ 1 つで、mqjs_runtime.c の MQJS_FS_SCOPE_MAX も _Static_assert で
   そこに縛られている。
   ピッカーがこれより長い道を返すと runtime は黙って ok=false に倒す
   (= タップしたのに取り消し扱い) ので、ピッカー側はその長さになる行を
   最初から押せなくする —— 灰色表示 + 行に理由を 1 行。 */
#define FS_PICK_SCOPE_MAX  FS_GRANT_ROOT_MAX

typedef enum { FS_PICK_OPEN, FS_PICK_SAVE, FS_PICK_DIR, FS_PICK_CONSENT } fs_pick_mode_t;
typedef struct {
    fs_pick_mode_t mode;
    const char *title;              /* 表示。CONSENT ではアプリ名 */
    const char *start_vpath;        /* NULL = ボリューム一覧 */
    const char *const *exts; int n_exts;   /* OPEN/SAVE のフィルタ。NULL = 全部 */
    const char *suggest_name;       /* SAVE の初期名 */
    const char *scope_vpath; uint8_t ops;  /* CONSENT: 求められた範囲と操作 */
    /* CONSENT で「なぜ要るのか」として出す、**アプリが書いた 1 行**。
       fs.request({path, write, reason}) の reason をそのまま入れる欄で、
       旧ランチャー同意画面が出していたものと同じ意味。
       NULL か "" なら理由の欄ごと出さない (欄が消えるだけで、範囲と
       操作の表示は変わらない)。他のモードでは読まれない。
       他の文字列と同じく fs_pick_begin() の中で同期的に複製する。
       複製のときに制御文字は空白へ潰す —— アプリが改行を詰めて
       「許可 / 拒否」を画面外へ押し出せないようにするため。 */
    const char *reason;
} fs_pick_req_t;
typedef struct { bool ok; char vpath[FS_PATH_MAX]; } fs_pick_result_t;
typedef void (*fs_pick_cb_t)(void *ctx, const fs_pick_result_t *r);

/* 任意タスク。モーダルなので同時に 1 件、2 件目は false。cb は UI タスク上で呼ばれる —
   受け取った側は自分のキューへ投げるだけにする (JS へは既存の EV_FSGRANT、edit_task へは PICKED)。

   false を返す条件 (cb は 1 回も呼ばれない):
     - req / cb が NULL、mode が未定義
     - 画面がまだ上がっていない
     - 先客のモーダルが出ている (s_busy)
     - **CONSENT で scope_vpath が空か、FS_PICK_SCOPE_MAX に収まらない**
       —— 切り詰めて出すと "/sd/long/dir/x" が "/sd/long/dir/" になり、
       求められたより**広い**範囲で許可を取ることになるので、断る
     - LVGL ロックが FS_PICK_LOCK_MS 以内に取れなかった
     - PSRAM が取れなかった / lv_timer を作れなかった */
bool fs_pick_begin(const fs_pick_req_t *req, fs_pick_cb_t cb, void *ctx);

/*
 * 出ているピッカーを畳む。いつ呼んでも安全 (出ていなければ何もしない)。
 * 畳むと cb が r->ok=false で**ちょうど 1 回**呼ばれる。
 *
 * **戻った時点で、次の fs_pick_begin() は通る。** これは仕様であって
 * 努力目標ではない: 「前の要求を捨てて次を出す」(mqjs_runtime.c の
 * fs_pending_take) が、非同期な取り消しだと 1 本目は殺したのに 2 本目が
 * 出ない、という形で必ず壊れるため。
 *
 * 待ちの構造 (JS タスクを長く止めないため):
 *
 *   - UI タスクから呼んだとき (= cb の中を含む) は、その場で同期に畳む。
 *     待ちは無い。
 *   - 他のタスクから呼んだときは印を立てて UI タスクの次の tick を待つ。
 *     待ちの上限は FS_PICK_CANCEL_WAIT_MS (下記)。ピッカーが出ている間
 *     内部のタイマは**止まらない**ので、UI タスクが動いてさえいれば
 *     1〜2 フレーム (16〜32 ms) で畳み終わる。
 *   - 上限に達したとき: WARN を 1 行出して戻る。**畳む印は立ったまま**
 *     なので、UI タスクが再び回った時点で畳まれて cb も撃たれる。
 *     つまりこの窓の間だけ次の fs_pick_begin() が false を返す
 *     (= 呼び出し側は「今は出せない」を返せばよい) 一方、
 *     モーダルが画面に残り続けることも、以後の pick が死に続けることも
 *     ない。上限に達するのは UI タスク自体が止まっているときだけ。
 *
 * 注意 (仕様 §A.4 のシグネチャに引数が無いことの帰結): この API は
 * 「今出ているピッカー」を指す手掛かりを取らない。自分のピッカーが
 * 畳まれた直後に呼ぶと、その瞬間に別のタスクが開いたピッカーを畳む。
 */
void fs_pick_cancel(void);

/* fs_pick_cancel() が他タスクから呼ばれたときに待つ上限 [ms]。 */
#define FS_PICK_CANCEL_WAIT_MS 64

/*
 * cb の契約 (呼び出し側が守るもの):
 *
 *   - **cb は UI タスク (esp_lvgl_port のタスク) の上で、LVGL ロックを
 *     保持したまま呼ばれる。** ブロックしない。fs_* を呼ばない。JS タスクを
 *     待たない。自分のキューへ 1 件投げて帰ること。ここで 8 ms 使うと
 *     spec §B.2 のロック予算 (lvgl_port の保持 8 ms) を破る。
 *   - fs_pick_begin() が true を返したら、cb は**ちょうど 1 回**呼ばれる
 *     (確定・取り消し・fs_pick_cancel()・要求が壊れていた場合を含む)。
 *     false を返したときは 1 回も呼ばれない。
 *   - r->ok が false のとき r->vpath は空文字列。
 *   - r とその中身は cb が戻るまでしか生きていない。使うなら複製する。
 *   - **cb の中から fs_pick_begin() を呼んではいけない。** 以前ここには
 *     「呼べる」と書いてあったが、その契約は捨てた: 呼べるようにするには
 *     cb より先に枠を空けるしかなく、そうすると cb が走っている最中に
 *     別のタスクの fs_pick_begin() が答えの置き場を上書きできてしまう
 *     (UI(core1) と JS(core0) は本当に同時に走る)。今は枠は cb が戻って
 *     から空く。cb の中で次のピッカーを出したい呼び出し側は、**自分の
 *     キューへ 1 件投げてから、そちらで** fs_pick_begin() を呼ぶこと。
 *     cb の中から呼ぶと、その begin は false を返す。
 */

#ifdef __cplusplus
}
#endif
