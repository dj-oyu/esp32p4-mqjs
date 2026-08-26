/* mqjs_native.h — ネイティブなものがフォアグラウンドになる仕組み
 *
 * docs/native-editor-spec.md §A.6 / §E 判断 #1。
 *
 * エディタもファイラも JS ワーカーではないが、**フォアグラウンド**にはなる
 * (キャンバスと打鍵を独占する)。今までの runtime は「fg = JS ワーカー 1 本」
 * (s_fg_worker) しか知らなかった。ここはその 2 本目の椅子で、置き場所を
 * runtime 側にしたのは §E #1 の判断 —— fg 切替の hygiene (UI_CMD_RESET) が
 * 1 か所に残るから。ランチャーの一覧・sys.open(name)・ステータスバーの
 * チップも、そのぶん既存の経路にそのまま乗る。
 *
 * 不変条件: **native surface が fg のとき、JS ワーカーは 1 本も fg ではない。**
 * s_fg_worker は「JS ワーカーの中でどれが前だったか」を憶えているだけになり、
 * native から戻るとその 1 本が sys.onForeground でやり直す。
 *
 * 実装は mqjs_runtime.c (「native surface」節)。別 TU にしないのは
 * s_fg_worker / switch_foreground / key_to_app が static で、そこを外へ
 * 出すと単一所有者が崩れるため。
 */
#ifndef MQJS_NATIVE_H
#define MQJS_NATIVE_H

#include <stdbool.h>
#include "skk_core.h" /* skk_dict_t は無名 struct の typedef */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 同時に登録できるネイティブ面。今の予定は editor と files の 2 枚で、
   2 枚ぶんの予備を持たせてある。表は起動時に埋めきる (下記の登録規約)。 */
#define MQJS_MAX_NATIVE 4

typedef struct {
    const char *name;    /* "editor" / "files" — ランチャー・sys.open の名前。
                            31 バイトまで (ステータスバーの chip_target が
                            char[32])。文字列は呼び出し側が永続で持つこと —
                            表は複製せずポインタを持つ。 */
    const char *title;   /* 表示名。NULL なら name が使われる */
    void (*focus)(void *ctx);   /* JS タスク上。直前に UI_CMD_RESET を通してある
                                   (下の「RESET は非同期」を読むこと)。ここでは
                                   自分のキューへ FOCUS を投げるだけにする。 */
    void (*blur)(void *ctx);    /* JS タスク上。**戻った時点で以後 key/touch は
                                   来ない** (fg フラグを先に落としてから呼ぶ)。 */
    void (*key)(void *ctx, const char *utf8, size_t len,
                uint32_t t_post, uint32_t t_isr);
                                /* poster のタスク上 (kbd_task / LVGL タスク /
                                   IME 所有タスク)。**キューへ投げるだけ**にする。
                                   utf8 はこの呼び出しの間だけ有効、len は 0 でない。
                                   t_post = mqjs_post_key が刻んだ time_us() の下位
                                   32bit、t_isr = 入力面が刻んだ ISR 時刻 (刻む口が
                                   無ければ 0)。どちらも µs、32bit の巻き戻りは
                                   引き算で消える。 */
    void (*touch)(void *ctx, int x, int y, int kind);
                                /* UI タスク上。kind 0=down 1=move 2=up、
                                   キャンバス座標。同じく投げるだけ。 */
    void *ctx;
} mqjs_native_surface_t;

/* 起動時に 1 回、**入力が動き出す前に** 呼ぶ。表はロックもバリアも
   持たない —— 打鍵の経路で mutex を取らないための設計で、成り立つ条件は
   「書くのは初期化の 1 タスクだけ、以後は読み取り専用」。走り出してから
   登録すると、他コアが半分だけ見える可能性を排除できない。
   返り値は id (>= 0)、表が満杯 / 引数不正 / 名前が重複なら -1。 */
int  mqjs_native_register(const mqjs_native_surface_t *s);

/* 任意タスク。EV_FOCUS を投げるだけで、実際の切替は JS タスクが
   mqjs_focus と同じ順序規約 (blur -> RESET -> focus) で行う。
   イベントキューが満杯なら黙って落ちる (sys.focus と同じ)。 */
void mqjs_native_focus(int id);

/* 自分が今 fg か。任意タスクから読める (int 1 個の読み)。 */
bool mqjs_native_is_fg(int id);

/* いま何らかの native 面が fg か (id を持たない側から聞く口)。
   ui_tab5 の track_task_switch が「タスクが替わったら画面を消す」を
   native 面にも適用してキャンバスを消してしまうため。他の 3 つの
   reset 経路には既に s_fg_native ガードが入っている。 */
bool mqjs_native_fg_active(void);

/* SKK 辞書を 1 本確保して返す。NULL = 焼かれていない。
   読み取り専用で共有可能 (個人辞書は存在しない)。edit_ui がこれを
   edit_ui_attach_dict() へ渡す。 */
const skk_dict_t *mqjs_skk_dict_acquire(void);

/* 名前引き。見つからなければ -1。mqjs_request_open / sys.open / sys.focus は
   これを **JS ワーカーより先に** 引く。 */
int  mqjs_native_find(const char *name);

/* 打鍵の入口 (mqjs_post_key) に ISR 時刻を添えられる版。
   mqjs_post_key(u, n) == mqjs_post_key_ts(u, n, 0)。
   入力面 (kbd_tab5 / ui_tab5) が ISR 時刻を刻むようになったら、そちらの
   呼び出しをこれに替えるだけで surface->key の t_isr が生きる。
   ここに置いてあるのは、この引数を欲しがっているのが native surface
   だけだから (JS 側の EV_KEY は時刻を運ばない)。 */
void mqjs_post_key_ts(const char *utf8, size_t len, uint32_t t_isr);

/* ---- M5 でここに足すもの (Phase 1 では実装しない) --------------------
 *
 * 仕様 §A.6 の後半は M5 (構文チェック + 行ジャンプ + dev スロット実行) の
 * 範囲なので、**宣言も置いていない** —— 宣言だけあると呼べてしまい、
 * リンクの段で初めて分かることになるため。M5 で足すのは 3 本:
 *
 *   bool mqjs_dev_run_local(const char *src, size_t len, const char *name);
 *   bool mqjs_parse_request(const char *src, size_t len, const char *name,
 *                           mqjs_parse_cb_t cb, void *ctx);
 *   void mqjs_set_error_sink(void (*fn)(int worker, const char *app,
 *                                       const char *msg, int line1, int col1));
 */

/* ---- 呼び出し側が知っておくべき 3 つの落とし穴 ----------------------
 *
 * 1. **UI_CMD_RESET は非同期。** 切替では focus() の直前に UI_CMD_RESET を
 *    UI キューへ *post* するが、キューを掃くのは UI タスクで、focus() が
 *    返る瞬間にキャンバスが空になっている保証は無い (ui_tab5_cmd は
 *    投げっぱなし・ノンブロッキングで、待つ口が無い)。UI キューを通さずに
 *    自分のタスクから直接キャンバスへ描く面 (spec §A.3 の
 *    ui_tab5_cells_draw) は、**最初の 1 枚が RESET に消される**ことを
 *    前提に組むこと。RESET はキャンバスを **HIDDEN にもする**ので、
 *    直接描画する面は自分でキャンバスを出す必要がある。
 *
 * 2. **IME は通らない。** native fg の間、mqjs_post_key は IME 所有タスクを
 *    迂回する (spec §E-2: エディタは自前の ime_t を持つ)。これを外すと打鍵は
 *    所有タスクへ吸われ、面には 1 バイトも届かない。逆に、面は自分で
 *    ime_feed を呼ばない限り かな漢字変換を得られない。
 *
 * 3. **停止できない。** native surface に sys.stop(name) は効かない
 *    (false を返す)。ランチャーの行末 ✕ を押しても何も起きない。
 *    面のライフサイクルは登録した側が持つ。
 */

#ifdef __cplusplus
}
#endif

#endif /* MQJS_NATIVE_H */
