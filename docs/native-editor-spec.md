# ネイティブ・エディタ / ファイラ — プログラム仕様

`docs/native-editor-design.md` を実装に落とす。設計文書が「何を作るか」なら、
これは「どの関数を、どのタスクで、どの順に、何を測りながら」。

**推測の数字は書かない。** 数字は §0 の既存コード由来か、設計文書 §7 の実測か、
「測る必要がある」(§F) のどれか。設計判断で未決のものは §E に列挙し、勝手に決めていない。

読んだもの: `fs_core.h`, `term_core.h`, `term_port.h`, `ime_core.h`,
`ui_tab5.h`, `mqjs_runtime.h`, `mqjs_runtime.c` の grant 表 / IME 所有タスク /
dev スロット再実行 / `key_to_app` / `dump_error`, `ui_tab5.cpp` の
`cells_run`, `kbd_tab5.c` の ISR 経路, `panic_note.c`, `flash_suspend_test.c`,
`tools/tests/ime_diff.sh`, `fs_core/pc_test`, `examples/files.js`,
`tools/compile_task.c`。

---

## 0. 既存コードから確定した前提

| | 値 | 出所 |
|---|---|---|
| UI タスク | core 1, prio **4** | `ui_tab5.cpp:3821` |
| JS タスク | core 0, prio 5, stack 16K | `app_main.c:271` |
| ドック kbd タスク | core 1, prio 5 | `kbd_tab5.c:479` |
| IME 所有タスク | **affinity 無し**, prio 5 | `mqjs_runtime.c:7360` (`xTaskCreate`) |
| tcpip | affinity 無し | `CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY=y` |
| セル | 9 × 24 px | `UI_CELL_W/H` |
| キャンバス | 横 1280×632 → **142 列 × 26 行**。縦 720×(1280−88) → **80 列 × 49 行** | 実寸は設計文書 §7 |
| `s_cells_a8` | 720×24 = 17,280 B 内部 SRAM、JS の `ui.cells` 専用 | `ui_tab5.cpp:160` |
| PPA blend | `PPA_TRANS_MODE_BLOCKING`、6 セル未満は CPU | `cells_run` |
| キー経路 | ISR(GPIO50) → sem → kbd_task が I2C 読み → `mqjs_post_key(seq)` → (IME want なら `s_ime_q` → IME 所有タスク) → `key_to_app` → field or JS の EV_KEY | `kbd_tab5.c:22`, `mqjs_runtime.c:7395` |
| 既存の打鍵計測 | `t_post` を `mqjs_post_key` で刻み、所有タスクで hop を出す。avg/max のみ、p99 無し | `ImeMeter` |
| grant 表 | `FsGrant{token,worker,gen,write,epoch,vol,root}` ×4、**JS タスクだけが触る** (`dispatch_fsgrant`) | `mqjs_runtime.c:2997` |
| 同意 | ~~C が EV_SIGNAL → launcher が `sys.fsConsent` → EV_FSGRANT で JS タスクが mint~~ **M1 で `sys.fsConsent` を削除済み (design §5)。今はネイティブモーダル `fs_picker` (`FS_PICK_CONSENT`) が `fs_pick_cb` → EV_FSGRANT で JS タスクへ返す** | `mqjs_runtime.c:3567` (`js_fs_request`)、`:3217` (`fs_pick_cb`) |
| dev の再実行 | `s_dev_retry_at = s_stop_req ? 0 : now+1000`。**明示 stop が来ると `dev_held()` で止まる** | `mqjs_runtime.c:9020, 9234` |
| 例外 | `dump_error` は `JS_PrintValueF(e, JS_DUMP_LONG)` で文字列にするだけ。**行と桁はエンジンが持っている** (`get_pc2line(&line, &col, …)`、既定で桁入り — `JS_EVAL_STRIP_COL` は削るためのフラグ)。実機で `at mqtt-task:152:28` を観測済み。無いのは**数値として取り出す C の経路**だけ | `mqjs_runtime.c:714`, `mquickjs.c:3924`, `mquickjs.h:296` |
| 構文解析のみ | `JS_Parse(ctx, src, len, name, 0)` がある。ホストは 8MB のコンテキストで呼ぶ | `compile_task.c:63` |
| UI フレームフック | `ui_tab5_set_frame_cb` は **1 スロットで term が使用中** | `ui_tab5.h`, `term_port_freertos.c` |
| `--wrap` の前例 | `esp_hosted_init`, `lvgl_port_ppa_create`, `esp_panic_handler`, `sdmmc_host_init` | 各 CMakeLists |

---

## A. モジュール分割とヘッダ

```
components/edit_core/         純 C99。ESP-IDF ヘッダ 0。host_test/ に ASAN + fuzz
components/edit_ui/           device only。edit_task、プレゼンタ、ファイル入出力の依頼
components/fs_picker/         device only。ネイティブモーダル (ピッカー + 同意)
components/fs_filer/          device only。ピッカーの上の操作層
components/fs_io/             device only。I/O タスク (open/save/copy/format/swap)
main/perf_meter.{c,h}         計測。§C。エディタより先に入る
components/mqjs/mqjs_native.h 既存 runtime への追加: native surface、dev 投入、エラーシンク
```

依存の向き: `edit_ui → edit_core, ime_core, skk_core, ui_tab5, fs_io, fs_picker, perf_meter, mqjs_native`。
`edit_core` は何にも依存しない。`fs_picker` は `fs_core` と `ui_tab5` だけ。

### A.1 `edit_core.h`

規約 (`term_core.h` と同一): 呼び出し側が 1 ブロックを渡す、`malloc` 0 回、
内部ポインタを返す関数は**存在しない**、全公開関数は `(edit_t*, 整数, 呼び出し側バッファ)`。

```c
/* ---- 上限 (呼び出し側が init で決める。既定は MQJS_SCRIPT_MAX に揃える) ---- */
typedef struct {
    uint32_t max_bytes;   /* 本文の上限。既定 128*1024 (= MQJS_SCRIPT_MAX) */
    uint32_t max_lines;   /* 行インデックスの上限。既定 8192 */
    uint32_t undo_bytes;  /* undo リング。既定 64*1024 */
    uint16_t max_cols, max_rows;  /* 表示の最大。既定 142 / 49 (両向きの最大) */
} edit_config_t;

typedef struct edit edit_t;                       /* 不透明。ブロックの先頭に置かれる */

typedef enum {
    EDIT_OK = 0,
    EDIT_E_ARG,     /* NULL / 範囲外の引数 */
    EDIT_E_FULL,    /* max_bytes 超過。本文は変更されていない */
    EDIT_E_LINES,   /* max_lines 超過。同上 */
    EDIT_E_UTF8,    /* 不正な UTF-8。同上 */
    EDIT_E_STATE,   /* その状態では不可 (選択が無いのに delete_selection 等) */
} edit_err_t;

size_t     edit_mem_size(const edit_config_t *cfg);
edit_err_t edit_init(void *block, size_t block_len, const edit_config_t *cfg,
                     edit_t **out);              /* block は 8B 整列、寿命は core と同じ */

/* ---- 本文の出し入れ (バイトが境界を越える唯一の場所) ---- */
edit_err_t edit_set_text(edit_t *e, const char *utf8, size_t len);   /* 置換、undo 消去、カーソル 0 */
size_t     edit_text_len(const edit_t *e);
size_t     edit_copy_text(const edit_t *e, size_t off, char *dst, size_t cap);
           /* off から cap まで複製。保存 (チャンク) と構文チェックの入力用。gap を跨いでも連続 */

/* ---- 編集 (全部カーソル位置、全部 undo 可) ---- */
edit_err_t edit_insert(edit_t *e, const char *utf8, size_t len);     /* 選択があれば置換 */
edit_err_t edit_delete(edit_t *e, int32_t n);   /* n<0: backspace |n| 文字、n>0: 前方 n 文字。端で clamp、OK */
edit_err_t edit_undo(edit_t *e);                /* 何も無ければ EDIT_E_STATE */
edit_err_t edit_redo(edit_t *e);

/* ---- カーソル・選択 ---- */
typedef enum { EDIT_M_LEFT, EDIT_M_RIGHT, EDIT_M_UP, EDIT_M_DOWN, EDIT_M_HOME, EDIT_M_END,
               EDIT_M_PGUP, EDIT_M_PGDN, EDIT_M_DOC_HOME, EDIT_M_DOC_END,
               EDIT_M_WORD_LEFT, EDIT_M_WORD_RIGHT } edit_motion_t;
edit_err_t edit_move(edit_t *e, edit_motion_t m, uint32_t n);         /* 端で clamp、OK */
edit_err_t edit_goto(edit_t *e, uint32_t line1, uint32_t col1);        /* 1 始まり (JS のエラー位置と同じ)。clamp、OK。line1==0 は E_ARG */
void       edit_select_begin(edit_t *e);        /* アンカー = カーソル */
void       edit_select_end(edit_t *e);          /* 選択解除 */
bool       edit_selection(const edit_t *e, size_t *from_byte, size_t *to_byte);
size_t     edit_copy_selection(const edit_t *e, char *dst, size_t cap);
edit_err_t edit_delete_selection(edit_t *e);

typedef struct { uint32_t line1, col1; size_t byte; } edit_pos_t;    /* col1 はセル列 (CJK=2) */
edit_pos_t edit_cursor(const edit_t *e);
uint32_t   edit_line_count(const edit_t *e);

/* ---- IME の合成文字列 (バッファには入れない。カーソル位置に重ねて描くだけ) ---- */
edit_err_t edit_set_preedit(edit_t *e, const char *utf8, size_t len);  /* len 0 = 消す。上限 128 B */

/* ---- エラー位置のマーク (実行時エラーの行を色で示す) ---- */
edit_err_t edit_set_mark(edit_t *e, uint32_t line1, uint32_t col1);
void       edit_clear_mark(edit_t *e);

/* ---- 表示 ---- */
void       edit_set_view(edit_t *e, uint16_t cols, uint16_t rows);   /* 回転・キーボード表示で呼ぶ。全行 dirty */
edit_err_t edit_scroll(edit_t *e, int32_t lines);                    /* タッチのドラッグ用。カーソルは動かさない */
/* カーソルが画面外へ出る操作は core が自動で追従スクロールする (ソフトラップ無し、横も同様) */

/* 何を描き直すか。行 = 画面の段 (ソフトラップが無いので 1 対 1) */
#define EDIT_DIRTY_ALL     (1u << 0)   /* スクロール・set_view・set_text の後 */
#define EDIT_DIRTY_CURSOR  (1u << 1)   /* カーソル行が変わった (旧行も rows に入っている) */
#define EDIT_DIRTY_STATUS  (1u << 2)   /* 行数・変更フラグ・位置 */
uint32_t   edit_dirty_flags(const edit_t *e);
uint64_t   edit_dirty_rows(const edit_t *e);    /* bit i = 画面 i 段目。rows ≤ 49 < 64 */
void       edit_dirty_clear(edit_t *e);

/* 1 段ぶんを「同じ色の連続 (run)」に切って返す。プレゼンタは run ごとに
   1 回 cells blit を呼ぶ。utf8 は呼び出し側のバッファに複製される —— core の
   内側を指すポインタは出さない。返り値は run 数、utf8_cap 不足なら負。 */
typedef enum { EDIT_CLS_PLAIN = 0, EDIT_CLS_KEYWORD, EDIT_CLS_IDENT, EDIT_CLS_NUMBER,
               EDIT_CLS_STRING, EDIT_CLS_COMMENT, EDIT_CLS_PUNCT, EDIT_CLS_PREEDIT,
               EDIT_CLS_N } edit_cls_t;
#define EDIT_RUN_SELECTED (1u << 0)
#define EDIT_RUN_CURSOR   (1u << 1)   /* この run の先頭セルがカーソル */
#define EDIT_RUN_MARK     (1u << 2)   /* エラー位置 */
typedef struct {
    uint16_t col, ncells;             /* 画面上の列と幅 (CJK は filler 込み、ui.cells の CONT 契約どおり) */
    uint8_t  cls, flags;
    uint16_t utf8_off, utf8_len;      /* utf8 バッファ内の位置 */
} edit_run_t;
int        edit_view_row(const edit_t *e, uint16_t row, edit_run_t *runs, int runs_cap,
                         char *utf8, size_t utf8_cap);
bool       edit_cursor_view(const edit_t *e, uint16_t *row, uint16_t *col);  /* 画面外なら false */

/* ---- 状態 ---- */
bool       edit_modified(const edit_t *e);
void       edit_mark_saved(edit_t *e);
uint32_t   edit_edit_count(const edit_t *e);    /* オートセーブの「100 編集」用。単調増加 */

/* ---- 検査と統計 (打鍵経路では呼ばない) ---- */
bool       edit_check(const edit_t *e);         /* 全不変条件を再計数で検査。ホストは毎 op、実機は Kconfig */
typedef struct {
    uint32_t gap_moves, gap_bytes_moved;        /* Q3 #2 の閾値 (piece table へ倒す判断) の材料 */
    uint32_t relex_lines, relex_cascades;       /* コメント開始で後続へ波及した回数 */
    uint32_t newline_index_moves, undo_evictions;
} edit_stats_t;
void       edit_stats(const edit_t *e, edit_stats_t *out);
```

**データ構造 (決定)**

- **gap buffer** 1 本、`max_bytes + 1` バイト。UTF-8 のまま。
- **行インデックス** `uint32_t line_start[max_lines]` は **gap 込みの物理オフセット**で持つ。
  gap 内への挿入で後続行が動かない (O(1)/打鍵)。改行の挿入/削除だけ末尾の memmove。
  `line_state[max_lines]` 1 バイト: 行頭の字句状態 (0 素, 1 ブロックコメント内, 2 テンプレート文字列内) + 要再計算ビット。
- **undo** はバイト上限のリング: `{kind, byte_off, len, text...}` の可変長レコード。
  連続する 1 文字挿入は 1 レコードに結合。満杯なら最古を捨てて `undo_evictions++`。
- **preedit** は core が持つ 128 B の小バッファ。`edit_view_row` がカーソル行に重ねて出す。**本文には入らない**。
- **字句解析**は行単位、行頭状態から。編集行を再字句し、行末状態が前回と違う間だけ次行へ波及。波及は**可視域の下端で打ち切り**、残りは次に `edit_view_row` が呼ばれたとき遅延で解く (要再計算ビット)。
- 正規表現リテラルは検出しない (`/` は PUNCT)。理由: `/` の文脈判定は行内で閉じないので行単位字句の前提を壊す。誤彩色は害が無い。

**不変条件 (`edit_check` が全部見る)**

1. `0 ≤ gap_begin ≤ gap_end ≤ cap`、本文長 = `cap − (gap_end − gap_begin)`。
2. `line_start[0] == 0`、各 `line_start[i]` は改行の直後、`line_count` 個で本文を尽くす。gap 内を指す `line_start` は無い。
3. カーソルの byte は文字境界、`line1/col1` は byte から再導出した値と一致。
4. `line_state[i]` は `line_state[i-1]` から行 i−1 を字句解析した結果と一致 (要再計算ビットの立った行は除く)。
5. undo リングのレコード境界が整合し、redo は最後の undo から連続。
6. gap の両端 8 バイトのカナリアが無傷。

**エラーの返し方**: 本文を変える関数は失敗時に**何も変えない** (E_FULL/E_LINES/E_UTF8 は先に検査してから書く)。clamp で済むものは clamp して OK を返す (呼び出し側に「端に当たった」を判断させない)。

### A.2 `edit_ui.h` (device only)

```c
typedef struct {
    edit_config_t core;
    uint32_t autosave_idle_ms;      /* 既定 2000 */
    uint32_t autosave_edits;        /* 既定 100 */
    uint32_t syntax_idle_ms;        /* 既定 1000 */
} edit_ui_config_t;

bool edit_ui_start(const edit_ui_config_t *cfg);   /* 起動時 1 回。ブロックを PSRAM に確保、edit_task 生成、native surface 登録 */
void edit_ui_open(const char *vpath);              /* 任意タスク。ピッカー無しで開く (ランチャー / ファイラの「編集」から) */
void edit_ui_new(void);
```

edit_task へのコマンド (内部、`edit_cmd_t`、キュー長 32、投げっぱなし):

| kind | 誰が | 中身 |
|---|---|---|
| `KEY` | native surface の key シンク (kbd_task / LVGL タスク) | utf8[24], len, **t_post, t_isr** |
| `TOUCH` | 同 | x, y, kind |
| `FOCUS` / `BLUR` | JS タスク (fg 切替) | — |
| `FILE_LOADED` | fs_io | buf (PSRAM, 所有権移転), len, err |
| `FILE_SAVED` | fs_io | err, was_swap |
| `PARSE_RESULT` | JS タスク | ok, line1, col1, msg[96] |
| `RUN_ERROR` | JS タスク (エラーシンク) | line1, col1, msg[96] |
| `PICKED` | fs_picker | vpath, mode |
| `TICK` | esp_timer 500ms | オートセーブ / 構文チェックのアイドル判定 |
| `SNAP` | JS タスク (`sys.perf`) | 計測スナップショットを撮って ack |

edit_task 本体 (core 1, prio **5** = UI+1, stack 8 KB 内部 SRAM, `xTaskCreatePinnedToCore`):

```
loop:
  cmd = receive()
  KEY:   T1 = now
         d = ime_feed(&own_ime, key)            <- §E-2 (自前 ime_t)
         PASS  -> motion/edit を edit_core へ
         TAKEN -> edit_set_preedit(ime_preedit())、ui_tab5_ime_face()
         TEXT  -> edit_insert(ime_text())
         T2 = now
         render_dirty()                          <- ロック無し。§A.3
         T3 = now
         ui_tab5_canvas_invalidate(rows)         <- ロックはこの中だけ
         T4 = now
         perf_edit_sample(T0=t_post, t_isr, T1, T2, T3, T4)
```

**自前 `ime_t` が安全である条件 (§E-2 で決定。条件付きなので根拠ごと残す)**

`skk_core` がロックを持たないのは、`docs/skk-ime-design.md` が言うとおり
**「mqjs のワーカーが全部 1 つの `mqjs` タスクの上で動くから」**である。`edit_task` は
別タスクなので、**その前提はここでは成立していない**。それでも今は安全で、理由は別にある:

1. 共有される `skk_dict_t` は**読み取り専用** (mmap した image を指すだけ)。
2. 可変状態は `skk_t` (と、それを包む `ime_t`) のインスタンスごとに独立している。
   edit_task の `ime_t` と IME 所有タスクの `s_ime` は 1 バイトも共有しない。
3. **個人辞書 (学習の書き戻し) は 2026-07-30 に削除済み**で、書き換わる共有状態が存在しない
   (`skk_core.h:16` "There is no personal dictionary")。

**つまり、個人辞書が復活したらこの判断は破綻する。** `skk-ime-design.md` §S7 は明示登録型の
再検討の入口を残しているので、これは仮定の話ではない。**個人辞書を再導入するなら、
コードを書く前に (a) `skk_core` のロック方針と (b) `edit_task` からのアクセスを設計し直すこと。**
「edit_task だけ辞書を読めなくする」「所有タスク経由に戻す (§E-2 の (b))」「書き手を 1 タスクに
限定して読み手は世代番号で見る」のどれかになるが、どれを選ぶかはそのとき決める。
この段落を消さずに残すのは、後でこれを踏む人が根拠を復元できるようにするため。

**edit_task の禁止事項** (設計文書 §6.5 をコードの規則に): `fs_*` を呼ばない、
`esp_partition_*`/NVS を呼ばない、`lvgl_port_lock` を `ui_tab5_canvas_invalidate` 以外で取らない、
`malloc` しない、JS タスクを待たない、`ppa_do_blend` 以外でブロックしない。
デバッグビルドでは `perf_meter` の flash カウンタが **edit_task 名の flash 操作 = 0** を assert する (§C.2)。

### A.3 プレゼンタ — `ui_tab5.h` への追加

`cells_run` は `CanvasApp` の内側で `_buf` と `s_cells_a8` を握っている。
native surface が同じ経路を**自分のタスクから、自分のステージングバッファで**使うための入口を足す:

```c
/* native surface 用のキャンバス直接描画。呼び出し側のタスクで走り、LVGL ロックを取らない。
   a8 は呼び出し側の 64B 整列ステージング (内部 SRAM、UI_CELL_H × ncells×UI_CELL_W 以上)。
   ppa は ppa_register_client で呼び出し側が持つ blend クライアント (エンジンは共有、キューは別)。 */
typedef struct {
    int col, row, ncells;
    const char *utf8; size_t len;
    uint32_t fg, bg; unsigned attrs;
    uint8_t *a8; size_t a8_len;
    void *ppa;                       /* ppa_client_handle_t */
} ui_cells_draw_t;
bool ui_tab5_cells_draw(const ui_cells_draw_t *d);          /* false = キャンバス無し */
void ui_tab5_canvas_fill(int x, int y, int w, int h, uint32_t rgb); /* 段の右端の余白用。ロック無し */
void ui_tab5_canvas_invalidate(int x, int y, int w, int h); /* lvgl_port_lock -> lv_obj_invalidate_area -> unlock。保持は µs */
```

**なぜ `ui_tab5_canvas_invalidate` が面積を取るのか (2026-08-26、ピッカーの実測から)。**
今の `ui.cells` は描画バッチの末尾で `lv_obj_invalidate(_canvas)` を呼んでいる
(`ui_tab5.cpp:3197`) —— **キャンバス全体**。`term_core` は dirty-row set を計算して
いるのに、その情報は LVGL の境界で捨てられている。`ssh_vt` は 1 文字来るたびに
1280×632 の再描画を払っている。

ピッカーで測った単位がこれを値段にする: **コストは面積で決まり、描画バッファの
720 × 50 段に量子化される** (ピッカーの実測で 3.2 ms/チャンク。ただしあれは角丸と
スクリムを含む合成で、キャンバスの 1:1 転送はもっと安いはず —— **エディタでの
1 チャンクの値は測る必要がある**、§F #13)。セルは 9×24 px なので:

| 操作 | dirty 段 | チャンク |
|---|---:|---:|
| 1 文字入力 | 1 | 1〜2 |
| カーソル移動 (旧行 + 新行) | 2 | 2〜3 |
| **スクロール (`EDIT_DIRTY_ALL`)** | **26** | **13〜26** |

**スクロールが予算を割る。** 打鍵の次に多い操作なのに、全面再描画になる。
対策は古典的な端末の手で、**PPA でキャンバスを段ぶん上下にブリットし、新しく
現れた段だけを描く**。`edit_scroll()` が API にあるのはこのため —— プレゼンタが
「全部描き直す」に倒してはいけない。

**隣接する dirty 段は 1 つの矩形にまとめてから渡すこと。** 24 px の段に対して
チャンクは 50 px なので、離れた 2 段は 2 チャンク、隣接した 2 段は 1 チャンクで
済むことがある。マージは無料。

`render_dirty()`: `edit_dirty_rows()` の各段について `edit_view_row` → run ごとに `ui_tab5_cells_draw`。
`EDIT_DIRTY_ALL` なら全段。段の背景は run の bg で塗られる (cells の既存契約)。段の右端の余白は
`ui_tab5_canvas_fill` 1 回。

**向き (§E-9: 両対応)**。`edit_core` は向きを知らない — `edit_set_view(cols, rows)` に数字を渡すだけで、
`edit_view_row` はその数字の中で描く。向きの検出と数字の算出はプレゼンタの仕事:
既存の自動回転 (`ui_tab5_set_landscape` / `ui_tab5_landscape()`、ドック装着で切替) に乗り、
fg アプリへ届く `"\0rotate"` トークンを native surface の key シンクが受けたら edit_task が
`ui_tab5_canvas_size()` と `ui_tab5_kb_reserved(mode)` から `cols = w / UI_CELL_W`、
`rows = (h − reserved) / UI_CELL_H` を出して `edit_set_view` を呼ぶ (全段 dirty)。
画面キーボードの出し入れも同じ経路。横 142×26 / 縦 80×49 は `max_cols/max_rows` の初期値であって、
コードのどこにも決め打ちを置かない。

ステージングバッファ: **自前 17,280 B 内部 SRAM** (`static uint8_t __attribute__((aligned(64)))`)。
確保後の `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` を起動ログに出し、
**40 KB 未満なら** `s_cells_a8` 共有 + 専用ミューテックスに切り替える (設計文書 §3.2)。
判定はビルド時ではなく**起動ログの数字で**。

### A.4 `fs_picker.h` (device only)

ピッカーは **grant を知らない**。「ユーザがこのパスをこの意図で選んだ」だけを返し、
grant の mint は今までどおり JS タスク (`dispatch_fsgrant`) がやる。権限表の単一所有者を崩さない。

```c
typedef enum { FS_PICK_OPEN, FS_PICK_SAVE, FS_PICK_DIR, FS_PICK_CONSENT } fs_pick_mode_t;
typedef struct {
    fs_pick_mode_t mode;
    const char *title;              /* 表示。CONSENT ではアプリ名 */
    const char *start_vpath;        /* NULL = ボリューム一覧 */
    const char *const *exts; int n_exts;   /* OPEN/SAVE のフィルタ。NULL = 全部 */
    const char *suggest_name;       /* SAVE の初期名 */
    const char *scope_vpath; uint8_t ops;  /* CONSENT: 求められた範囲と操作 */
} fs_pick_req_t;
typedef struct { bool ok; char vpath[FS_PATH_MAX]; } fs_pick_result_t;
typedef void (*fs_pick_cb_t)(void *ctx, const fs_pick_result_t *r);

/* 任意タスク。モーダルなので同時に 1 件、2 件目は false。cb は UI タスク上で呼ばれる —
   受け取った側は自分のキューへ投げるだけにする (JS へは既存の EV_FSGRANT、edit_task へは PICKED)。 */
bool fs_pick_begin(const fs_pick_req_t *req, fs_pick_cb_t cb, void *ctx);
void fs_pick_cancel(void);
```

画面は `ui_tab5_cam_canvas` と同じ「スクリム + 上に載る LVGL オブジェクト」の作法で組む
(モーダル、背後のタッチを吸う、`UI_CMD_RESET` で消える)。
一覧は `fs_dir_open` のスナップショット (PSRAM) を UI タスクで開く — `fs_dir_open` の所要時間は
**測る必要がある** (SD の DCIM 数千枚で UI タスクを何 ms 止めるか。閾値 50 ms、超えれば fs_io へ)。

### A.5 grant 表への追加 (`mqjs_runtime.c`)

```c
enum { FS_SCOPE_SUBTREE = 0, FS_SCOPE_FILE = 1, FS_SCOPE_APPDIR = 2 };
enum { FS_OP_READ = 1, FS_OP_WRITE = 2, FS_OP_CREATE = 4, FS_OP_DELETE = 8, FS_OP_RENAME = 16 };
typedef struct {
    uint32_t token; uint8_t worker; uint16_t gen;
    uint8_t  kind;                  /* NEW */
    uint8_t  ops;                   /* NEW: bool write を置換 */
    uint32_t epoch; const fsvol_t *vol; char root[MQJS_FS_SCOPE_MAX];
} FsGrant;
```

- `fs_in_scope`: SUBTREE は前置一致 (現状)、FILE は**完全一致**、APPDIR は前置一致。
- `fs_grant_for(ctx, gv, path, need_ops)`: `need_write` を op ビットに置換。各 `js_fs_*` が自分の op を渡す
  (`write` 既存→WRITE、`write` 新規→CREATE|WRITE、`mkdir`→CREATE、`remove`→DELETE、
  `rename`/`move`→**両端**を RENAME で検査、`copy`→**先だけ** CREATE|WRITE を検査)。
- **`copy` は元に `READ` を課さない (訂正、2026-08-26)。** 当初案は「元に READ、
  先に CREATE|WRITE」だったが、`fs.read` は design §5 で全アプリに開放済みなので、
  元に READ を要求してもアプリは `fs.read` + `fs.write` で迂回でき安全性は増えず、
  一方で `examples/files.js` のボリュームまたぎコピー (grant は貼り付け先の
  ボリュームにしか出ない) は元の READ 判定で必ず落ちていた。**`rename`/`move` は
  両端とも書き換えるので両端の検査を残す** ── こちらは迂回しても安全性が変わらない
  話ではない (RENAME は元を消す)。
- **暗黙 grant**: `gv` が 0 で `path` が `/internal/data/<appname>/` 配下なら APPDIR とみなして通す。
  `<appname>` は App record の名前 (`sys.setAppName` 済み、サニタイズ済み)。表には入れない (mint 不要)。
- `fs.pick(opts, cb)`: `fs_pick_begin` を呼び、cb で `EV_FSGRANT` を kind=FILE、ops = mode に応じて
  (OPEN→READ、SAVE→CREATE|WRITE) で投げる。`dispatch_fsgrant` は `p->kind/ops` を写す。
- `sys.fsConsent` と `FsPending` の EV_SIGNAL 経路: `fs.request` が `fs_pick_begin(CONSENT)` を呼ぶ形に置換。
  ランチャー側の同意 JS は**同じコミットで削除**。
- 削除: `js_fs_mount`, `js_fs_unmount`, `js_fs_format` (設計文書 §5)。`fs_format` の専用タスクは fs_io に統合 (§A.7)。
- **grant 判定 (`fs_in_scope` + op ビット) は純 C の 1 ファイルに切り出し**、`fs_core/pc_test` で叩く (§D の `test_grant_scope.c`)。

### A.6 `mqjs_native.h` — runtime への追加

エディタとファイラは JS ワーカーではないが、**フォアグラウンド**にはなる (キャンバスとキーを独占する)。
今の runtime は fg = JS ワーカー 1 本 (`s_fg_worker`) しか知らない。§E-1 の判断待ちだが、
仕様として必要な最小の形を書いておく:

```c
typedef struct {
    const char *name;                            /* "editor" / "files" — ランチャー・sys.open の名前 */
    const char *title;                           /* 表示名 */
    void (*focus)(void *ctx);                    /* JS タスク上。UI_CMD_RESET 済み、キャンバスは空 */
    void (*blur)(void *ctx);                     /* JS タスク上。以後 key/touch は来ない */
    void (*key)(void *ctx, const char *utf8, size_t len, uint32_t t_post, uint32_t t_isr);
                                                 /* poster のタスク上 (kbd_task / LVGL / IME 所有)。キューへ投げるだけ */
    void (*touch)(void *ctx, int x, int y, int kind);
    void *ctx;
} mqjs_native_surface_t;
int  mqjs_native_register(const mqjs_native_surface_t *s);    /* 起動時。id を返す */
void mqjs_native_focus(int id);                                /* 任意タスク。mqjs_focus と同じ経路で JS タスクが切替 */
bool mqjs_native_is_fg(int id);

/* dev スロットへローカルソースを投入 (任意タスク)。走り終わっても・落ちても再実行しない
   (dev_held と同じ「明示 stop」扱い)。src は停止まで呼び出し側が保持。 */
bool mqjs_dev_run_local(const char *src, size_t len, const char *name);

/* 構文チェックの依頼 (任意タスク)。JS タスクで JS_Parse し、結果を cb で返す (JS タスク上)。 */
typedef void (*mqjs_parse_cb_t)(void *ctx, bool ok, int line1, int col1, const char *msg);
bool mqjs_parse_request(const char *src, size_t len, const char *name, mqjs_parse_cb_t cb, void *ctx);

/* 未捕捉例外のシンク。dump_error から呼ばれる (JS タスク上)。line1/col1 は取れなければ 0。 */
void mqjs_set_error_sink(void (*fn)(int worker, const char *app, const char *msg, int line1, int col1));
```

`key_to_app` の分岐は 3 段になる: field → **native fg** → JS fg。
`mqjs_post_key` の IME 判定は native fg のとき **want=false** (エディタは自前の `ime_t` を持つ、§E-2)。
`mqjs_request_open(name)` は native の名前を先に引く。

**行/列の取得 (決着済み)**: 構文エラーも実行時例外も、エンジンは行と桁の両方を持っている
(`mquickjs.c:3924` の `get_pc2line(&line_num, &col_num, …)`。桁は既定で入り、`JS_EVAL_STRIP_COL` は
それを**削る**ためのフラグ)。実機の例外文字列 `at mqtt-task:152:28` がその出力。
残る作業は `dump_error` の横に**数値として取り出す経路**を 1 本作ることだけ:
`JS_Parse` 失敗直後と未捕捉例外時に、例外オブジェクトから `(line1, col1)` を取って
`mqjs_parse_cb_t` / エラーシンクへ渡す。`get_pc2line` は static なので、runtime 側に同じ
入口を出す薄い関数を 1 つ足す (mquickjs 本体の変更ではなく、既存 API の呼び出し側の追加)。
vendor パッチは**不要**。文字列を正規表現で拾う経路は作らない。

**構文チェックのコンテキスト**: dev ワーカーの arena は使えない (走っている最中に依頼が来る)。
**専用 arena 1 本 (256 KB PSRAM, `MQJS_APP_MEM_SIZE`)** を初回依頼時に確保し常駐させる。
**128 KB のソースを 256 KB で JS_Parse できるかは測る必要がある** (ホストは 8 MB で呼んでいる。
足りなければ arena を大きくするだけだが、数字が無いと決められない → §F #6)。

### A.7 `fs_io.h` (device only)

```c
typedef enum { FSIO_LOAD, FSIO_SAVE, FSIO_COPY, FSIO_MOVE, FSIO_REMOVE, FSIO_FORMAT } fsio_kind_t;
typedef struct { bool ok; esp_err_t err; void *buf; size_t len; } fsio_result_t;
typedef void (*fsio_cb_t)(void *ctx, const fsio_result_t *r);              /* fs_io タスク上。投げ直すだけ */
typedef void (*fsio_progress_t)(void *ctx, uint64_t done, uint64_t total);
bool fsio_submit(fsio_kind_t k, const char *from, const char *to, const void *buf, size_t len,
                 fsio_cb_t cb, fsio_progress_t prog, void *ctx);            /* 任意タスク、キュー 8、満杯で false */
```

タスク: **core 0, prio 3** (§E-14), stack 8 KB。既存の `fs.format` 専用タスクをここへ統合。
LOAD は PSRAM に `MQJS_SCRIPT_MAX + 1` を確保して読み、所有権を cb へ渡す。
SAVE は `<vpath>.tmp` へ書いて rename (途中で落ちても元が残る)。

**保存先は最初から `/internal/scripts/` に固定** (§E-11)。新規ファイルの既定の置き場であり、
ピッカーの SAVE も `start_vpath` をここにする。後から動かすとユーザの原稿が迷子になるので、
昇格 UI が無い間もパスだけは決めておく。

**swap (オートセーブ) は内蔵 littlefs** (§E-8)。`/internal/scripts/.swp/<basename>.<crc32(vpath)>.swp`
— 原本が SD 上にあっても内蔵に置く (同名ファイルの衝突は vpath の CRC で避ける)。保存成功で削除。
アイドル駆動 (2 s / 100 編集) なので打鍵と重ならず、内蔵の電断安全性 (littlefs) を取る。
SD に置く案は捨てた: 「SD ならキャッシュ停止を起こさない」という利点は、アイドル駆動にした時点で
買う理由が消える。

### A.8 `fs_filer.h`

`void fs_filer_open(const char *start_vpath);` — native surface "files"。
画面はピッカーと同じ部品 + 操作バー (copy/move/rename/delete/新規フォルダ/取り出し/format/編集/プレビュー)。
全操作は `fsio_submit`、進捗は同じ画面のバー。`fs_path_reserved` が真の行には削除ボタンを出さない
(既存の事前判定用 API どおり)。「編集」は `edit_ui_open(vpath)`。
ゲート用に `fs_filer_replay(const char *ops, size_t len)` (デバッグ Kconfig 配下) を持つ (§C.6)。

---

## B. タスク・優先度・ロック

### B.1 住人表 (変更後)

| タスク | core | prio | 変更 | 持ってよいロック |
|---|---|---|---|---|
| UI (esp_lvgl_port) | 1 | 4 | — | lvgl (自分のフレーム) |
| **edit_task** | 1 | **5** | 新規 | lvgl を `ui_tab5_canvas_invalidate` 内だけ。PPA client (自前) |
| kbd_tab5 | 1 | 5 | — | I2C0 バス (読みの間) |
| IME 所有 | **1** | 5 | **affinity を 1 に固定** (今は無し。core 0 に居ると JS の長い C 呼び出しに hop を食われる) | lvgl (float 更新) |
| JS (mqjs) | 0 | 5 | — | lvgl (ui.* の 1 コマンド分)、grant 表 (所有者) |
| **fs_io** | 0 | **3** | 新規 (format タスクを統合) | flash ドライバ (littlefs 経由)、SDMMC |
| http_get | 0 | 4 | — | — |
| tcpip | **0** | 18 | **`CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y`** | — |
| esp-hosted | 要確認 | 22-23 | affinity を確認し、無ければ 0 へ | — |
| microlink lifecycle | 要確認 | ? | 同上 | — |
| fsusp_crc / fsusp_wr | — | — | 試験専用、既定 off | — |

**core 1 の住人は UI・edit_task・kbd・IME 所有・ISR のみ**。これが「アプリを横断して安定」の構造的根拠。
確認方法: 起動直後に `uxTaskGetSystemState` を 1 回ログに出す (`perf_meter` の boot dump、§F #4)。

### B.2 ロック保持の予算

| ロック | 保持者 | 上限 | 超えたら |
|---|---|---|---|
| lvgl_port | JS の ui.* 1 コマンド | **8 ms** | コマンドをチャンク化 (cells の 80 セル分割は既にある) |
| lvgl_port | edit_task (invalidate) | **100 µs** | 設計違反 |
| lvgl_port | fs_picker (一覧の再構築) | **例外: 上限を置かない** | 下記 |
| lvgl_port | IME 所有 (float) | 8 ms | — |
| PPA エンジン | edit_task 1 段 blend / JS cells / LVGL 回転 / カメラ | 各操作は HW 時間。待ちは **測る** | 3 ms 超で回転を部分領域に |
| edit_core | edit_task のみ (単一所有者、ロック無し) | — | — |
| grant 表 | JS タスクのみ (単一所有者、ロック無し) | — | — |
| flash ドライバ | fs_io、NVS commit、インストール | — | **edit_task からは 0 回** (§C.2 が assert) |

**fs_picker がこの予算の外に居る理由 (2026-08-26、実機で決めた)。** ピッカーは
**モーダル**で、開いている間フォアグラウンドのアプリは入力を奪われている。この表の
8 ms が守っているのは JS の `ui.*`、`edit_task` の invalidate、IME の float ——
モーダルの下では前二者は動いておらず、IME はピッカー自身のものしか無い。
**予算が守るべき相手が居ない。**

そして実機で測ると、守っても得が無いどころか損だった。初回の実装は
`enter_dir` が `lv_obj_clean` して**次の tick で**行を作る 2 段構成で、
保持を刻む代わりに (a) その隙間に空の一覧が 1 フレーム描かれ、(b) 描画が
`LV_DEF_REFR_PERIOD=33` と `FREERTOS_HZ=100` (esp_lvgl_port の loop は
1 tick 待ち + 末尾 `vTaskDelay(1)` で 1 周 10〜20 ms) の組み合わせで
**実効 40 ms 周期に量子化**され、tap→pixel が 40〜100 ms になった。
ユーザは実機で「ディレクトリ移動が重い」と言った。

採った形: **新しい行が揃ってから差し替え、揃った tick の末尾で
`lv_refr_now()`** (`ui_tab5.cpp:3015` の `KB_BENCH_STEP` が UI タスク上・
ロック下で使っている前例)。保持にその描画時間が乗るが、**総保持は後で
refresh timer が同じ描画をするのと同じで、1 回に固まるだけ**。
「1 手に 1 回 8 ms を超える」と「40 ms 遅れて出る」の取引で、前者を取った。

**黙って破らないこと。** ピッカーは保持時間を "held lvgl" のログに出し続ける。
`LV_DEF_REFR_PERIOD` と `FREERTOS_HZ` は**触らない** —— 前者は全画面共通で
毎フレーム invalidate するアプリの再描画が倍になり、後者は
`pdMS_TO_TICKS` の丸めが system 全体で変わる。ピッカーのために動かす変数ではない。

---

## C. 測定の設計

### C.0 規則

1. **打鍵経路の計測は「時計を読んで内部 SRAM の整数に足す」だけ。** 1 打鍵あたり時計読み **≤ 6 回**、
   ロック 0、malloc 0、ログ 0、書式化 0、flash 0。`esp_timer_get_time` の 1 回のコストは **測る**
   (§F #0)。それが 1 µs 超なら回数を減らす。
2. メータは**単一書き手**。読む側は所有タスクに `SNAP` を投げて複製を受け取る (`ime_cmd_sync` と同じ)。
   例外は ISR が書く `t_isr` と poster が数える drops (既存の `s_ime_drops` と同じ理由)。
3. ヒストグラムは **log2 バケット固定 12 本** (`<1ms, <2, <4, <8, <16, <32, <64, <128, <256, <512, <1024, ≥1024 ms`
   相当を µs で)。p50/p99 はバケットから読む側が出す。挿入は `31 - clz(us)` の 1 命令。
   分布 1 本 = 48 B。加えて **最悪 16 件のリング** (各段の内訳付き) で「最悪のときに何が起きていたか」を残す。
4. 取り出しは **`sys.perf()` 1 本** (新 binding、ROM ヘッダ再生成が要る — 記憶の罠)。
   `{flash:{...}, edit:{...}, lock:{...}, ime:{...}, tasks:[...]}` を返し、既存の probe (`tools/probe_*.js`) が
   MQTT で流す。`ui.imeStats()` はそのまま残す (互換)。
5. 全メータは `CONFIG_MQJS_PERF_METER` (既定 y) 配下。off でマクロが空になる。
6. **停止 (キャッシュ停止) の入口と出口の時刻は、コア別に持つ** (`esp_cpu_get_core_id()` で添字)。
   2 つのコアの cycle-count CSR は同期していない。M0a の初版はコアをまたいで引き算し、
   `max=10,177,430us` (10.18 秒) という偽の最大値を出した。入口を書いたコアと出口を読むコアが
   同じであることを、構造で保証する。
7. **計測器は妥当性の上限を持ち、外れ値は捨てた数を報告する**。停止なら 100 ms 超を外れ値とし、
   ヒストグラムには入れず `discarded` に数える。黙って捨てると「計測器は正しい」と誤読される
   (規則 6 の修正直後の計測で 1 件出ている — 出所は未特定、数だけ残す)。
   edit_task の段別計測 (C.1) も同じ: 1 段 > 1 秒は外れ値として数える。

### C.1 打鍵 → 表示の端対端

**刻む場所**

| 点 | どこ | 誰が |
|---|---|---|
| **T_isr** | `kb_isr` (GPIO50) | ISR。`s_kbd_isr_us` (volatile, DRAM) に `esp_timer_get_time` を書くだけ。画面キーボードは 0 |
| **T0** | `mqjs_post_key` 入口 | poster。既存の `t_post` を流用 |
| **T1** | edit_task が `KEY` を取り出した直後 | edit_task |
| **T2** | `edit_*` (IME feed 込み) が終わった直後 | edit_task |
| **T3** | `render_dirty` が終わった直後 (PPA blocking 戻り) | edit_task |
| **T4** | `ui_tab5_canvas_invalidate` が戻った直後 | edit_task |
| **T5** | その段を含む flush が終わった時刻 | LVGL の flush 経路 (**新フック**、下記) |

**T5 の取り方**: `ui_tab5_set_frame_cb` は 1 スロットで term が使っている。別に
`ui_tab5_set_flush_cb(void (*fn)(void*, int x1, int y1, int x2, int y2, uint32_t seq), void*)` を足す。
esp_lvgl_port の flush_cb (DPI フレームバッファへの転写完了) から呼ぶ (§E-4)。edit_task は invalidate 時に
`seq_at_invalidate = ui_tab5_frame_seq()` を控え、flush_cb で「seq ≥ 控え かつ 領域が dirty 段と交差」
なら T5 を記録する。**ガラスに出るまでの走査 (≤ 16.7 ms) はソフトでは測れない**ので、
端対端は「T5 + 最大 16.7 ms」を上限として読む。

**保持**: `perf_edit_t { hist[6][12], max[6], ring[16]{T_isr..T5} }` — 段は
`isr→post`, `post→edit_task (hop)`, `edit`, `render`, `invalidate (ロック待ち込み)`, `flush`。

**取り出し**: `sys.perf().edit = {hopP50, hopP99, editP99, renderP99, lockP99, flushP99, e2eP99, worst:[...]}`。

### C.2 打鍵中のフラッシュ操作回数 — **実装済み (M0)**

`main/flash_stall_meter.c`、`CONFIG_MQJS_FLASH_STALL_METER`。
`--wrap=spi_flash_disable_interrupts_caches_and_other_cpu` と `--wrap=spi_flash_enable_interrupts_caches_and_other_cpu`
(`cache_utils.c` 定義、`spi_flash_os_func_app.c` から呼ばれる = TU が違うので wrap が効く)。

実装で確定した規則 (設計から変えた点を含む):

- **wrap は停止の中で走る**ので `IRAM_ATTR`、触る変数は `DRAM_ATTR` のみ、ログ無し。
- **時計は `esp_cpu_get_cycle_count()` (CSR 読み)**。`esp_timer` はタイマ周辺機器を触るので停止中に使わない。
  → C.0 規則 1 の「時計」は、**キャッシュ停止の内側では CSR、外側 (edit_task) では `esp_timer`** と読み分ける。
- 集計は停止が明けてから: 回数 / 打鍵に重なった回数 / 合計 µs / 最大 µs / **log2 ヒストグラム 10 本**
  (「30 ms が 1 発」と「1 ms が 30 発」を区別するため)。
- **「打鍵中」= 停止開始が最終打鍵から 1,500 ms 以内**。オートセーブのアイドル 2 s より短いので、
  オートセーブ自身の書き込みは定義上「打鍵中」に入らない。
- **`s_last_key_us` の記録位置は `mqjs_post_key` の「電源ゲートの後、IME ルートの前」**。
  画面を起こしただけのキーは打鍵として数えず、IME に吸われた打鍵は数える。
  `mqjs_last_key_us()` / `mqjs_now_us()` で公開。エディタが無くても動く。
- 報告は 30 秒ごと、数が動いたときだけ (ログ行)。

**ベースライン (実機、2026-08-26、30 秒、通常起動 + Wi-Fi + Tailscale、打鍵なし)**:

```
stalls=3605  typing=0  total=213ms  max=5244us  avg=59us
hist us  <64:2990  <128:552  <256:48  <512:11  <1k:1  <2k:0  <4k:0  <8k:3  <16k:0  16k+:0
```

**読み方の訂正 (2026-08-26)。** 初版はここで「3,605 / 30s = 120 回/秒、合計 213ms /
30,000ms = 壁時計の 0.7%」と読み、そう書いていた。これは誤り。`meter_task` は
30 秒おきに**ブートからの累計**を報告するだけで `flash_stall_meter_reset()` は
どこからも呼ばれておらず (`main/flash_stall_meter.c:136,151-156`)、上のログの
`stalls=3605` は「起動後この報告が出るまでの累計」であって「直前 30 秒間の
回数」ではない ── ログ行に付く `30s:` は報告の間隔を表すだけで集計窓ではない。
**正しく読むには連続する 2 行の差分を取る必要がある。** このベースライン行
1 本だけからは「回/秒」も「壁時計に対する割合」も出せない。読み取れるのは
「起動直後の littlefs マウント・棚の一覧・NVS 読み出し・Tailscale の鍵読みで
少なくとも 3,605 回、合計 213ms 分の停止が起きた」という起動時の事実だけ。

回数が多いのは、**auto-suspend 無しでは書き込みだけでなく読みを含む全ての SPI1 ドライバ操作が
キャッシュを落とす**から (IDF: 短い読みはキャッシュが完了を待つ)。3,605 回の大半は littlefs / NVS の読み。
**尾は 5.2 ms** (4〜8 ms のバケツに 3 件)。打鍵に重なれば見えるが、予算 30 ms に対して致命ではない。

**設計文書 §6.1 の「core 1 スループット −38%」とは条件が違う。** あれは 150 回連続の littlefs 書き込み
= 意図的な酷使での値。「−38% を XIP の根拠に使ってはいけない」のは初版どおりだが、
代わりに置いた「0.7%」も誤読だったので根拠にはならない。**XIP の根拠になり得るのは
実使用の実測 (§F #2、design §6.4) だけ**: ssh_vt2 19 分 + skk_test の ASCII 4 分 +
日本語 SKK 変換 4 分半、合計 約 27.5 分の打鍵で**打鍵に重なった停止は 0 回**。停止の
出所はアプリのロード (1 回あたり 1,000〜2,000 発) で打鍵とは重ならず、**アイドル
47 分でも停止は 0 回**だった。design §6.4 はこれで XIP をやらないと決着している。

設計 (§C.2 旧版) にあって報告に無いもの — **未実装として扱い、M0b へ** (lead 確認済み):
`byTask` (タスク別 8 枠)、最悪 16 件のリング、そこから出す `keysStalled` (M4、edit_task が自分の T0..T4 と突き合わせる)、
`sys.perf()` への接続 (§C.0 規則 4)。M0 のゲートは「ログで `opsTyping` が取れる」で足りる。
- **直接の指標**: edit_task が 1 打鍵ごとに `ring` を見て、自分の T0..T4 と重なった停止があれば
  `keys_stalled++`。これが「打鍵が実際に食らった回数」。設計文書 §6.4 はこの 2 つ (`ops_typing`, `keys_stalled`) で判断する。
- **assert**: デバッグビルドで `slot(edit_task).ops != 0` なら `ESP_LOGE` + panic note 1 行 (打鍵経路の flash 書き込み)。
- 報告: `sys.perf().flash = {ops, opsTyping, usTyping, maxUs, hist, byTask:[{name, ops, us, opsTyping}], keysStalled}`。
- **読みも数える**: `esp_flash_read` (littlefs/NVS のドライバ読み) も同じ guard を通るので ops に入る。
  `byTask` で書き手が分かるので分離は不要。

エディタが無い M0 の時点で **ssh_vt で打ちながら** 取れば、「今の使い方で打鍵中に flash 操作が何回起きるか」が
§6.4 の初期値になる。

### C.3 `edit_core` の内部コスト

T1→T2 を `ime_feed` / `edit_*` (apply) の 2 つに割る (境界で時計 1 回)。`render_dirty` の中では
`edit_view_row` 全段の合計と `ui_tab5_cells_draw` 全段の合計を、段ループの外で 2 回の読みで出す
(**段ごとには読まない**: 26 段で 52 回になる)。1 打鍵の時計読みは T0..T4 + 境界 1 + render 内 2 = **≤ 8 回**、
C.0 の「6 回」を超えるので、**render 内の 2 回は Kconfig `PERF_METER_DETAIL` 配下**にして既定 off。

ホスト側は `host_test/bench_edit.c` で同じ関数を `clock_gettime` で回す: 1 文字挿入、改行挿入、
300 KB の gap 移動、コメント開始の波及、`view_row` 142 列。**ホストの数値は回帰検出用**であって実機の値ではない。

### C.4 ロック保持 p99 と違反者

`--wrap=lvgl_port_lock` / `--wrap=lvgl_port_unlock` (esp_lvgl_port の別 TU から呼ぶ側だけが wrap される —
LVGL タスク自身の内部呼び出しは同一 TU なので**見えない**。それでよい: 見たいのは外部の保持者)。

```c
lock:   t_req = now; ok = __real_lvgl_port_lock(t);
        if (ok && depth[task]++ == 0) { t_acq[task] = now; wait_hist[log2(t_acq - t_req)]++; }
unlock: if (--depth[task] == 0) { hold = now - t_acq[task]; hold_hist[log2(hold)]++;
                                  per_task[task].max = max; if (hold > 8000) per_task[task].violations++; }
```
depth はタスク別 (再帰ロック)。8 枠のタスク別表に `{name, holds, maxUs, violations}`。
報告: `sys.perf().lock = {waitP99, holdP99, byTask:[...]}`。**edit_task の maxUs が 100 µs を超えたら設計違反**。

### C.5 SKK 変換の p99

`ImeMeter` に `key_hist[12], conv_hist[12], hop_hist[12]` を足す (既存の avg/max は残す)。
`ui.imeStats()` に `keyHist/convHist/hopHist` を追加、`sys.perf().ime` は同じものを指す。
松で `probe` を回して p99 を取る。閾値 5 ms (Q3) は avg 77 µs から見て余裕があるはずだが、**p99 は未測**。

### C.6 マイルストーンと「いつ測れるか」

| M | 内容 | ここで**可能になる**測定 | ゲート (次へ進む条件) |
|---|---|---|---|
| **M0a** (実装済み) | `flash_stall_meter` (C.2)。**エディタは無い** | C.2 の `opsTyping` とヒストグラム (ssh_vt で打ちながら)、§F #1 #2 | 実機: 30 秒報告に数字が出る。littlefs 書き込みで ops が増える |
| **M0b** | lock の wrap (C.4) + IME ヒストグラム (C.5) + `sys.perf()` + affinity 掃除 + boot のタスク一覧ダンプ + `mqjs_parse_request` (arena 計測のため先行) | C.4、C.5、§F #0 #3 #4 #5 #6 | 実機: `sys.perf()` が MQTT で取れる。**core 1 に prio>4 の非住人が 0 本** |
| **M1** (進行中) | `fs_picker` (OPEN/SAVE/CONSENT) + grant の kind/ops + `fs.pick` + `sys.fsConsent` 削除 | ピッカーの `fs_dir_open` 所要 (UI タスク上、閾値 50 ms)、ロック保持 (C.4 に picker が出る) | ホスト: `test_reserved` 継続 + `test_grant_scope.c`。実機: 既存 `probe_fs.js` が `fs.pick` 経由で通る |
| **M2** | `fs_io` + `fs_filer` + `fs.mount/unmount/format` 削除 | コピー中の UI 応答 (C.4 の wait、IME hop)、fs_io のスループット | **`files.js` ゲート** (下記)。実機: 大容量カードのコピーと format が UI を止めない (lock waitP99 ≤ 8 ms) |
| **M3** (完了) | `edit_core` (ホストのみ) | C.3 のホストベンチ、fuzz | ホスト: `run_tests.sh` 全緑、fuzz 1 時間で 0 クラッシュ、`edit_check` 毎 op で緑、ASAN/UBSAN 緑 |
| **M4** | `edit_ui` (edit_task、プレゼンタ、native surface、自前 ime_t、open/save/swap) | **C.1 全段**、C.2 の `keysStalled`、C.3 実機、ステージング後の内部 SRAM largest | 実機: e2e p99 (T0→T5) が idle で **≤ 33 ms**、ssh 通信中・コピー中・Tailscale 再接続中の 3 負荷で **≤ 60 ms**。edit_task の flash ops = **0**。largest ≥ 40 KB (未満なら共有へ) |
| **M5** | 構文チェック + dev 投入 + エラー位置ジャンプ + エラーシンク | parse 時間 vs サイズ (16/64/128 KB)、JS タスクの停止時間 | 実機: 構文エラーで dev が **再起動ループしない**、行/列に飛ぶ。parse が 128 KB で 50 ms 超なら「明示のみ」へ |
| **M6** | 実使用 1 週間 | C.2 の `opsTyping`/`keysStalled` の蓄積 | **§6.4 の判断**: `keysStalled / keys` を見て XIP をやるか決める |

**`files.js` ゲート** (`tools/tests/filer_diff.sh`、`ime_diff.sh` 方式):
操作列を固定 (`tools/tests/filer_ops.txt`: mkdir/write/copy/move/rename/remove/list を内蔵と SD で)。
(1) 既存 `files.js` を dev スロットで走らせて各操作後の `fs.list` を MQTT で吐かせた記録を **golden** に凍結、
(2) ネイティブファイラを同じ操作列で `fs_filer_replay` 駆動して同じ形で吐く、
(3) diff。`ime_diff.sh` と同じく「基準であって理想ではない」— 差が出たら理由を書いて golden を更新する。

---

## D. 実装順とゲート (ホスト / 実機の別)

| 順 | 段 | ホストで満たせる | 実機が要る |
|---|---|---|---|
| 1 | M0a flash_stall_meter (済) | — | wrap が呼ばれる (littlefs 書き込みで ops が増える) |
| 2 | M0b lock wrap / IME hist / `sys.perf()` / affinity | `log2` バケットと p99 算出の単体テスト | `sys.perf()` 取得、boot ダンプで core 1 の住人確認 |
| 3 | M1 grant 判定の純 C 化 + テスト (進行中) | `test_grant_scope.c` | — |
| 4 | M1 picker (進行中) | — | OPEN/SAVE/CONSENT の 3 モードが返る、`probe_fs.js` |
| 5 | M2 fs_io + filer | — | `filer_diff.sh` IDENTICAL、format/コピーで lock waitP99 ≤ 8 ms |
| 6 | M3 edit_core (完了) | 全部 (ASAN/UBSAN/fuzz/bench/`edit_check`) | — |
| 7 | M4 edit_ui | プレゼンタは無理。`edit_view_row` の run 分割はホストで検証済み | C.1 p99、flash ops=0、SRAM largest |
| 8 | M5 parse/run/jump | `JS_Parse` の位置取得は `run_pc` でホスト検証可 | dev 投入と再起動抑止 |
| 9 | M6 実使用 | — | `keysStalled` |

**ブランチ**: `feat/native-editor` を `feat/filer-storage` から切る (grant 変更はマージ前に、設計文書 §5)。
M0 だけは独立に `main` へ入れてよい (エディタ非依存、他の計測にも使える)。

---

## E. 設計判断の台帳

番号は据え置き (他文書とメッセージが番号で参照している)。状態を各項目の先頭に付ける。

- **決定 (推奨どおり)**: #1 runtime 側、#3 借りる、#5 新 binding、#12 非彩色、#13 64 KB、#14 prio 3、#15 既定 off
- **決定 (条件付き)**: #2 自前 `ime_t` — 条件は §A.2 に明記
- **決着**: #6 — エンジンが行と桁を持っている。§A.6
- **M0 で測って決める**: #4、#7
- **決定 (ユーザ)**: #8 内蔵 swap、#9 両向き、#10 128 KB、#11 昇格 UI は後回し・保存先は固定

1. **[決定]** **native surface の置き場**。runtime に `mqjs_native_surface_t` (§A.6) を足して JS ワーカーと同列の
   fg にする案を書いたが、代替は「term_ui_tab5 のコンソール画面と同じ流儀で ui_tab5 側に持つ」。
   ランチャーの一覧・`sys.open("editor")`・ステータスバーのチップに出るかどうかが変わる。
   **推奨: runtime 側** (fg 切替の hygiene = UI_CMD_RESET が 1 箇所に残る)。
2. **[決定・条件付き]** **エディタの IME**。(a) 自前の `ime_t` を edit_task が持つ (辞書は `skk_dict_t` を共有、読み取り専用)、
   (b) 既存の IME 所有タスクを通す。(a) は hop が 1 段減り JS タスクと無関係になるが、preedit の
   float は edit_task が自分で描く (本仕様は canvas 上のオーバーレイ)。(b) は既存の float と `ui.imeStats` を
   そのまま使えるが、所有タスクの affinity を固定してもキュー 1 段が残る。**(a) で決定。
   安全である 3 条件と、個人辞書を再導入するときの手順は §A.2 に書いた — そこを消さないこと。**
3. **[決定]** **キャンバス**。JS の `s_js_canvas_buf` (PSRAM 1.6 MB) を native fg の間だけ借りるか、
   エディタ用にもう 1 枚持つか。借りる案は UI_CMD_RESET で消える前提と整合し PSRAM を増やさない。
   **推奨: 借りる**。
4. **[M0 で測る]** **`ui_tab5_set_flush_cb` の追加** (T5 のため)。esp_lvgl_port の flush_cb に手を入れる = managed component
   への変更か、`lvgl_port_ppa_create` と同じ wrap か。**要確認: flush 完了を外へ出す既存の口があるか**。
5. **[決定]** **`sys.perf()` の形**。新 binding (ROM 再生成) か、`ui.imeStats()` に相乗りか、ログ行か。
   **新 binding で決定**。相乗りは意味が違うものを 1 つの名前に入れる。
6. **[決着]** **エラー位置**。エンジンが行と桁を両方持っている (`get_pc2line`、桁は既定で入る、
   実機で `at mqtt-task:152:28` を観測)。vendor パッチは不要。残る作業は数値で取り出す経路 1 本 (§A.6)。
   M5 の範囲は「構文エラーも実行時エラーも行/桁へ飛ぶ」で確定。
7. **[M0 で測る]** **構文チェックの arena**。専用 256 KB を常駐させる案。**128 KB の JS_Parse が 256 KB で足りるか未測** (§F #6)。
   足りなければ 512 KB か、dev arena をアイドル時に借りるか。
8. **[決定・ユーザ]** **swap の置き場 → 内蔵 littlefs**。アイドル駆動なら打鍵と重ならないので、
   電断安全性のある側を取る。SD 優先はやめ、記述も §A.7 から削った。
9. **[決定・ユーザ]** **縦向き → 両対応**。横 1280×632 決め打ちにしない。既存の自動回転に乗る。
   `edit_core` は列数・行数を引数で受け取るだけで向きを知らない (§A.3)。
10. **[決定・ユーザ]** **最大ファイルサイズ = `MQJS_SCRIPT_MAX` (128 KB)**。押し込める上限と編集できる上限が
    同じなので「編集して push できないファイル」が生まれない。超えるファイルは読み取り専用でプレビュー。
11. **[決定・ユーザ]** **`/internal/scripts` と `apps/` 昇格 UI → 後回し (M1〜M6 の外)**。
    ただし**保存先のパスだけは最初から `/internal/scripts` で固定** (§A.7)。予約サブツリーには入れない。
    昇格 UI が無い間は「エディタで開いて dev スロットで走らせる」だけで完結する。
12. **[決定]** **正規表現リテラルの彩色**をやらない (§A.1)。
13. **[決定]** **undo の上限** 64 KB (Kconfig)。
14. **[決定]** **fs_io の優先度 3** (JS の 5 より下)。JS のビジーループで I/O が飢える方向。逆 (JS より上) は
    I/O が JS を止める。どちらも「協調 JS が長い C 呼び出しをしない」前提で問題にならない。3 で決定。
15. **[決定]** **C.3 の render 内 2 回の時計読み** (規則 C.0 の 6 回を超える) は `PERF_METER_DETAIL` 配下、既定 off。

---

## F. 実測が要るもの (推測で埋めない)

| # | 何を | いつ (M) | 使う判断 |
|---|---|---|---|
| 0 | `esp_timer_get_time` 1 回のコスト | M0 | C.0 の「時計 6 回」が妥当か |
| 1 | キャッシュ停止の分布 (通常動作) | **済 (M0a)**: 起動直後 30 秒で累計 stalls=3605、avg 59 µs、max 5.2 ms (§C.2 のログ)。**「120 回/秒」「壁時計の 0.7%」は 2026-08-26 訂正**: カウンタがブートからの累計でリセットされないので 1 行を 30 秒窓として割った値は過大評価。実際の背景率は §F #2 のとおり実質ゼロ | C.2 のヒストグラム下限 (64 µs のバケツが要る — 確認済み) |
| 2 | ssh_vt で 10 分打った間の `opsTyping` (`byTask` は M0b) | **済**: 実使用 約 27.5 分 (ssh_vt2 19分 + skk_test の ASCII 4分 + 日本語 SKK 変換 4分半、M3 = `edit_core` 完了と同日、2026-08-26)、打鍵中の停止 **0 回**。観測された停止の出所はアプリのロード (1 回あたり 1,000〜2,000 発) で、打鍵とは重ならなかった。**アイドル 47 分でも停止 0 回** | §6.4 の判断材料。design §6.4 はこれで XIP をやらないと決着した。まだ 1 週間 (§F #11) の分ではないが、0 回はここまでで最も強い材料 |
| 3 | lvgl lock の holdP99 と違反者 (ランチャー idle / ssh_vt / viewfinder) | M0 | B.2 の 8 ms が現実的か |
| 4 | boot 時のタスク一覧 (core/prio) | M0 | B.1 |
| 5 | SKK convP99 (松) | M0 | Q3 #8 |
| 6 | `JS_Parse` 16/64/128 KB の時間と arena 消費 | M0 | E-7、M5 のアイドル起動可否 |
| 7 | `fs_dir_open` の所要。**内蔵は測った (2026-08-26): 0 件 2.2ms / 4 件 9.7ms / 7 件 10.3ms。所要はほぼそのまま両コア停止時間で (31 秒窓の停止差分 +502 回 ≈ dir_open 33.5ms ÷ 78µs)、犯人は littlefs のメタデータ走査 (`lfs_dir_getinfo` が 1 件あたり 2 回、`READ_SIZE=128` で 1 走査 = ⌈使用量/128⌉ read)。`LITTLEFS_MMAP_PARTITION=y` で消えるはず (要検証)。残るのは SD (FAT、停止せず O(件数) の別モデル)** | M1 済 / SD は M2 | **50ms 閾値は撤回。** コストは件数ではなく `O(件数 × メタデータブロック使用量)` で、使用量は compaction 前の commit 履歴に依存する = 同じ件数でも数倍違う。線形外挿は成り立たない |
| 8 | 1 段 (142 セル) の `ui_tab5_cells_draw` 時間、26 段の合計、PPA 待ち | M4 | Q3 #3/#9 |
| 9 | ステージング確保後の内部 SRAM largest | M4 | A.3 の共有判定 |
| 10 | e2e 各段の p99 (idle / 3 負荷) | M4 | M4 ゲート |
| 11 | `keysStalled / keys` 1 週間 | M6。**§F #2 の 27 分サンプル (0 回) は先行シグナルで、この行の代わりにはならない** — 1 週間分はまだ無い | XIP をやるか。今のところ「やらない」側に倒れている |
| 12 | **オートセーブ 1 回の停止数・合計・最大 (16 / 64 KB)** | M4 | 設計文書 §6.5 は「オートセーブは 2 秒アイドル駆動なので打鍵と重ならない」とするが、それは**セーブ中に打ち始める**場合を防がない。littlefs は 4KB ごとに erase・512B ごとに prog を出し、**書き込みの停止は MMAP でも消えない** (消えるのは読みだけ)。実測の最大停止は 2026-08-26 に 5.2ms → **17.4ms** へ更新され、出所は probe を push した = littlefs へ書いた瞬間だった。erase 1 発が 17ms 級なら打鍵 p99 ≤ 33ms は 1 発で割れる。**対策は測ってから決める** —— 推測で作り込むと要らない代金を払う (§6.4 の XIP で一度やりかけた) |
| 13 | **キャンバス 1 チャンク (720×50) の描画 + PPA 回転 + DSI 転送**。ピッカー実測の 3.2ms は角丸とスクリムの合成込みで、キャンバスの 1:1 転送は別物 | M4 | §A.3 の段別 invalidate の値段。打鍵 1〜2 チャンク・スクロール 13〜26 チャンクに掛ける単価 |
