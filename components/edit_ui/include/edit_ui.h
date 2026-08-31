/*
 * edit_ui — エディタを画面に出す層。`edit_task` とプレゼンタ。
 *
 * 仕様 docs/native-editor-spec.md §A.2 / §A.3、設計 docs/native-editor-design.md。
 *
 * ここに居るもの / 居ないもの:
 *
 *   居る   edit_task (core 1, prio 5)、コマンドキュー、自前の ime_t、
 *          edit_core の dirty 段を ui_tab5 のセル描画へ落とすプレゼンタ、
 *          打鍵 → invalidate の段別計測。
 *   居ない ファイルの読み書き。**仕様 §A.2 が「edit_task は fs_* を呼ばない」と
 *          定めている**ので、開く / 保存するは fs_io (M2) に投げる形になる。
 *          その fs_io がまだ無いため、Phase 1 の edit_ui_open() は
 *          「頼まれたことを覚えてステータス行に出す」だけで、1 バイトも読まない。
 *
 * ---- 公開面が 3 本しかない理由 --------------------------------------
 *
 * エディタは JS のワーカーではなく **native surface** (§A.6) なので、
 * 起動・入力・描画の入口はすべて runtime 側から来る。外から呼ぶ必要が
 * あるのは「起動時に 1 回組み立てる」「このパスを開け」「新規」の 3 つだけで、
 * キーもタッチもフォーカスも mqjs_native_surface_t のコールバック経由。
 * ここに 4 本目を足したくなったら、それは native surface の口が足りて
 * いないという意味なので、まず §A.6 を疑うこと。
 *
 * 例外が edit_ui_attach_dict() 1 本ある。理由はその宣言の上に書いた。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "edit_core.h"
#include "skk_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 仕様 §A.2 の edit_ui_config_t。core の上限はそのまま edit_core へ渡り、
   残りの 3 つは edit_task のアイドル判定 (オートセーブ / 構文チェック) に
   使う —— どちらも M2 / M5 の仕事なので、Phase 1 では受け取って保持する
   だけで、まだ誰も読まない。既定値を消さずに置いておくのは、後から
   「いくつだったか」を探し直さないため。 */
typedef struct {
    edit_config_t core;
    uint32_t autosave_idle_ms;      /* 既定 2000 */
    uint32_t autosave_edits;        /* 既定 100 */
    uint32_t syntax_idle_ms;        /* 既定 1000 */
} edit_ui_config_t;

/* 起動時 1 回。ブロックを PSRAM に確保、edit_task 生成、native surface 登録。
 *
 * cfg が NULL なら仕様 §A.1 / §A.2 の既定値を使う (max_bytes 128 KB,
 * max_lines 8192, undo 64 KB, max_cols/rows 142/49)。既定を呼び出し側に
 * 書き写させないための NULL であって、設定を省ける口ではない。
 *
 * false = 何も起きていない (メモリが取れなかった / タスクが作れなかった /
 * 画面の無いビルド)。部分的に立ち上がった状態は残さない。 */
bool edit_ui_start(const edit_ui_config_t *cfg);

/* 任意タスク。ピッカー無しで開く (ランチャー / ファイラの「編集」から)。
 *
 * **Phase 1 では読み込まない。** fs_io (§A.7) がまだ無く、edit_task から
 * fs_* を呼ぶのは設計違反 (§A.2) なので、パスを覚えてフォアグラウンドに
 * 出るところまでで止まる。M2 で fsio_submit(FSIO_LOAD, ...) を挟めば、
 * この関数の外形は変わらない。 */
void edit_ui_open(const char *vpath);

/* 任意タスク。空バッファで開いてフォアグラウンドへ。 */
void edit_ui_new(void);
/* 今の本文を s_vpath へ保存する (名前が無ければ /internal/scripts へ)。
   書き込みは fs_io タスクの上。呼んだ側は待たない。 */
void edit_ui_save(void);

/* ---- 仕様 §A.2 に無い 1 本。足した理由をここに残す -------------------
 *
 * §E-2 は「エディタは自前の ime_t を持ち、辞書 (skk_dict_t) は読み取り専用の
 * 共有」と決めた。ところが辞書の実体を持っているのは mqjs_runtime.c の
 * static な SkkImage 表で、**§A.6 の native surface にはそれを取り出す口が
 * 無い**。edit_ui が自分で skk_dict_open() する手は取らなかった: 開くには
 * esp_partition_mmap が要り、参照カウントも二重になり、「同じ image を
 * 2 度開かない」という runtime 側の約束を壊す。
 *
 * したがって当面は「持っている側から渡してもらう」形にする。呼ぶのは
 * runtime か app_main で、edit_ui_start() の後ならいつでもよい (コマンド
 * キュー経由で edit_task に届くので、attach するのは常に ime_t の所有
 * タスク自身)。dict は edit_ui より長生きすること。
 *
 * 辞書が付くまで ime_feed() は全部 IME_PASS を返す (ime_core.h の約束) ので、
 * この関数を一度も呼ばなくても ASCII は打てる。日本語が打てないだけ。
 *
 * **これは仕様との食い違いとして報告済み。** §A.6 に辞書アクセサが入ったら、
 * この 1 本は消して差し替えること。 */
void edit_ui_attach_dict(const skk_dict_t *dict);

#ifdef __cplusplus
}
#endif
