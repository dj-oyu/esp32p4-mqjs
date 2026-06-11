# Tab5 マルチアプリランタイム + ランチャー 設計書 (P4)

Status: **設計フェーズ** (2026-06-11)。スコープは **P4a(ランタイムのマルチ
コンテキスト化)+ P4b(ランチャー)**。P4c(MQTT アプリレジストリ)と
P4d(typed clipboard IPC)は次弾。

関連: `docs/widget-framework-design.md`(W1〜W3 済 — 画面遷移・リテイン
スタック・マルチ SSH はこの土台)、`docs/ssh-terminal-design.md` §7
(クリップボード IPC の先行設計)、[[tab5-platform-vision]]。

## 0. 動機

現在はタスク 1 本だけ(`app_main.c` の js_task が `mqjs_run_script` を
ブロッキング実行、push で**置換**)。構想は「SSH 端末 + 電卓 + CSV ビューワを
**同時に**動かし、データを受け渡す」。そのための実行基盤(P4a)と、
アプリの起動/切替/停止を行うホーム画面(P4b)を作る。

## 1. 決定事項 (2026-06-11、ユーザー確定)

1. **実行モデル = 協調マルチコンテキスト**。単一 js_task(Core0)に
   N 個の mquickjs コンテキスト。イベント/タイマーを所有アプリの
   コンテキストへ直列ディスパッチ。ロック不要、既存 binding の
   「static → AppSlot 構造体化」だけで成立。1 ディスパッチの上限は
   既存の 5 秒ウォッチドッグ(= 他アプリの最悪待ち時間)。
2. **バックグラウンドも実行継続、UI だけ前面専有**。背景アプリの
   タイマー/mqtt/ssh は発火し続ける(ssh_vt の背景タブと同じ思想)。
   UI 系(キャンバス描画・ウィジェット・キーボード・タッチ/キー
   イベント)はフォアグラウンドアプリのみ。背景アプリの `ui.*` は
   **no-op**(戻り値はダミー)。
3. **画面は切替時に破棄、復帰時に再構築**。背景行きのアプリの
   ウィジェット画面は `ui_tab5_w_reset()` で全破棄し、キャンバスも
   クリア。前面復帰時に `sys.onForeground(fn)` でアプリが自前で
   作り直す(メモリ最小、ライフサイクル明示。キャンバスの
   「モデルから再描画」と同じ一本のルール)。
4. スコープ: 今回 P4a+P4b。アプリ配布(topic 毎レジストリ/manifest)と
   クリップボード IPC は次弾。

## 2. パイロット結果 (2026-06-11)

- **mquickjs エンジンは複数コンテキスト共存可**: `mquickjs.c` の可変
  static はデバッグ `#ifdef DUMP_PC2LINE_STATS` 内の 2 個だけ。全状態は
  コンテキストの mem_buf(256KB)に閉じている。単一スレッドで複数
  コンテキストを順番に触るぶんには共有状態が無い。
- メモリ予算: 4 アプリ × 256KB = 1MB PSRAM(32MB に対し余裕)。
  内部 RAM は per-app のコールバック表 ~1KB 程度 + 各アプリが使う
  リソース次第(SSH は別途上限 3 本で律速)。

## 3. P4a: ランタイムのマルチコンテキスト化

### 3.1 AppSlot

`mqjs_runtime.c` の static binding 状態を構造体に束ねる:

```c
#define MQJS_MAX_APPS 4   /* slot 0 = ランチャー(常駐)+ ユーザー 3 */

typedef struct {
    bool used;
    char name[32];            /* sys.setAppName / 起動元から */
    uint8_t *mem;             /* 256KB PSRAM、起動時確保・停止時解放 */
    JSContext *ctx;           /* アプリ生存中ずっと生きる */
    TimerSlot timers[16];
    GpioSlot  gpio_cb[8];
    MqttSub   mqtt_subs[8];  bool onconn_used; JSGCRef onconn;
    esp_mqtt_client_handle_t mqtt;  /* client_id="mqjs-app-<slot>" */
    SshCb     ssh_cbs[4];
    WidgetCb  widget_cbs[48]; /* 前面時のみ実体画面あり */
    JSGCRef   touch_cb, key_cb, fg_cb, bg_cb;  bool ..._used;
    /* print sink の行アセンブラもアプリ毎(行が混線しないように) */
} AppSlot;
```

ライフサイクル: `app_start(slot, src, len, name)` = mem 確保 →
`JS_NewContext` → トップレベル eval。`app_stop(slot)` = そのアプリの
binding 後始末(現 `reset_slots` 相当: GCRef 解放、gpio ISR 解除、
mqtt destroy、**自分が開いた ssh セッションだけ** close、前面なら
ui_w_reset)→ `JS_FreeContext` → mem 解放。

### 3.2 イベントルーティング

イベントキューは**共有 1 本のまま**、各イベントの所有アプリを決めて
そのコンテキストにディスパッチする:

| イベント | 所有の決め方 |
|---|---|
| タイマー | per-slot の timers[] を全スロット走査(現 run_timers の外側ループ化) |
| EV_GPIO | pin → slot の登録表 |
| EV_MQTT_* | esp-mqtt の handler 引数 = slot |
| EV_SSH_DATA/CLOSED | session id → slot 対応表(connect した時に記録) |
| EV_TOUCH / EV_KEY | **常に前面アプリ** |
| EV_WIDGET | **常に前面アプリ**(画面は前面しか持てない) |
| EV_LIFECYCLE (新設) | 対象 slot を明示(foreground/background 通知) |

ループ終了条件: 「全スロットが pending なし」→ 従来の
タスク再実行ループへ戻る…のではなく、**ランチャー(slot 0)が常駐**する
ので js_task は永久ループになる。アプリの自然終了(pending なし)=
そのスロットだけ `app_stop`。

### 3.3 前面切替プロトコル

```
switch_foreground(new_slot):
  旧前面: EV_LIFECYCLE(background) → JS の sys.onBackground(fn)
          → C: ui_tab5_w_reset() + キャンバスクリア + キーボード hide
  新前面: EV_LIFECYCLE(foreground) → JS の sys.onForeground(fn)
          → アプリが画面/キャンバスを再構築 (決定事項 3)
```
- `sys.onForeground/onBackground` 未登録のアプリは、復帰しても何も
  描かないだけ(console 系はそれで十分)。
- 背景アプリの `ui.*` は C 側ゲート(`s_foreground_slot` 比較)で no-op。
  クエリ系(ui.size/cellSize/textSize)は背景でも答える(無害)。
- キーボード・タッチ・EV_WIDGET は前面にだけ届くので、背景アプリの
  入力系コールバックは登録されたまま眠る。

### 3.4 再構築コストの実測 (2026-06-11, p4_rebuild_probe.js)

決定事項 3(破棄→onForeground 再構築)の裏付け実測(タスクスイッチ機構
なし、W1-W3 プリミティブで計測):
- ウィジェットページ破棄 = **avg 0.4ms**(静止時)。
- settings 級ページ(16 ウィジェット+リスト 6 行)の構築 = **純粋
  ≈30ms**、壁時計 avg ≈215ms。差は `ui.screen()` が遷移アニメ(200ms)を
  先に開始するため、後続の create がアニメ描画とロックを取り合うから。
  つまり体感の復帰遅延 ≈ 遷移アニメ時間で、「保持して隠す」にしても
  節約は ~100ms 程度 → 破棄方式で問題なし。
- キャンバス全再描画(80×33)の発行はほぼ 0ms、実ピクセル完了は
  Core1 の数フレーム(50〜100ms)。
- **P4a 改善項目**: `ui.screen()` を「子の構築完了後にアニメ開始」へ
  (イベントディスパッチ終了時に load_anim をコミット)。スライド中の
  ウィジェットのポップイン解消とロック競合削減が同時に得られる。

### 3.5 既存挙動の互換

- **単発タスクの開発フロー(push → 置換 → 自動再実行)は維持**:
  既存の task topic に来たタスクは「dev スロット」(slot 1 固定)で
  実行。終了時の 1 秒後再実行も dev スロットだけ従来どおり。
  → 全 examples が無変更で動く。
- ssh セッション上限 3 は**全アプリ合計**(sshc は関知しない。
  app_stop が自アプリの分だけ閉じる)。
- `store.*` は全アプリ共有の flat namespace。P4 では**キー prefix の
  規約**(`<app>.key`)で運用し、強制は P4c の manifest で検討。
- print はコンソールに全アプリ混在で流れる。行アセンブラをアプリ毎に
  分離して行の混線だけ防ぐ(行頭に `[name]` を付けるかは実装時に判断)。

## 4. P4b: ランチャー

- **ランチャー自体を組み込み JS アプリにする**(slot 0、起動時に
  embedded ソースから自動開始、停止不可)。ウィジェットフレームワークを
  そのまま使う(dogfooding): アプリ一覧 = `ui.screen` + list。
- **ホームへの導線 = ステータスバーをタップ**(layer_top のバーは全画面で
  見えている = どこからでも届く)。C 側: バーに CLICKED ハンドラ →
  `switch_foreground(0)`。
- ランチャー画面(JS):
  - 実行中アプリのリスト(name、slot、稼働状態)→ タップで前面切替
  - 「停止」アクション(行タップ → 切替/停止の選択ページ、W2 ホスト
    ページと同型)
  - 起動できるもの: dev タスク(persisted/embedded)+ /littlefs/apps/
    以下のスクリプト(後述 P4b-lite)
- **P4b-lite インストール**(レジストリの先行最小形): push payload の
  先頭行に `// @app <name>` ディレクティブがあれば task_source が
  `/littlefs/apps/<name>.js` に保存(置換実行はしない)。ランチャーが
  一覧して起動できる。topic 毎レジストリ/manifest/アイコンは P4c。
- ランチャー用 JS API 追加:
  - `sys.apps()` → [{slot, name, running}]
  - `sys.launch(nameOrPath)` / `sys.stop(slot)` / `sys.focus(slot)`
  - `sys.setAppName(name)`(vision の T3 項目を取り込み)
  - これらは**ランチャー slot からのみ許可**(他アプリが呼ぶと
    TypeError — 権限は P4c の manifest までの暫定ルール)。

## 5. フェーズ計画

| | 内容 | 規模 |
|---|---|---|
| **P4a** | AppSlot 構造体化 + イベントルーティング + 前面切替 + ライフサイクルイベント + 背景 ui.* ゲート | 中〜大 |
| **P4b** | ランチャーアプリ(slot 0)+ ステータスバータップ + sys.apps/launch/stop/focus + @app インストール | 小〜中 |
| P4c | topic 毎レジストリ + manifest(name/icon/permissions)+ store の名前空間強制 | 中 |
| P4d | typed clipboard IPC(C 共有バッファ、ssh-terminal-design §7 の設計を実装) | 小〜中 |

実装順: P4a を**固定 2 アプリのハードコード**で先に検証(ランチャー抜き、
[[test-new-modules-in-isolation]] の精神: 例えば dev スロットの ssh_vt +
slot2 の mqtt_demo を同時に動かし、SSH を張ったまま裏で mqtt ループバックが
続くこと・切替で画面が復元することを確認)→ それから P4b。

## 6. リスクと割り切り

- **協調モデルの公平性**: 重い 1 ディスパッチ(最悪 5 秒 watchdog)が
  他アプリの応答を遅らせる。P4 では割り切る(対策するなら将来
  「アプリ毎の watchdog 予算短縮」や eval の分割)。
- **イベントキュー共有**: 暴走アプリの大量イベントが他アプリを遅らせ得る
  (W3 の rx 背圧と同じ構図で bounded)。キュー深さ 32 は据え置きで観察。
- **メモリ断片化**: app_start/stop の 256KB 確保/解放は PSRAM ヒープ。
  断片化が見えたら slot 毎に**固定事前確保**へ切替(定数化しておく)。
- **256KB/アプリの妥当性**: ssh_vt(最大級)が 3 セッション分のモデルを
  持っても収まる実績あり。アプリ毎に可変させるのは manifest(P4c)待ち。
