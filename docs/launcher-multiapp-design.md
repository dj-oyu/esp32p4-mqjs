# Tab5 マルチアプリランタイム + ランチャー 設計書 (P4)

Status: **P4a + P4b 完了** (2026-06-11、タッチ操作までユーザー実機確認
済み)。§3 / §4 のとおり実装。P4a: AppSlot / slot+世代ルーティング /
前面切替 / sys.signal/onSignal / sys.onForeground/onBackground / 背景
ui.* ゲート / screen の構築後アニメ。P4b: 常駐ランチャー (slot 0、
examples/launcher.js = 唯一の組み込みアプリ) + ステータスバーのチップ
(直前アプリを「開く」) と長押し (武装プログレス → ランチャー) + 一覧の
行末 ✕ で即停止 (確認ページなし) + sys.setAppName/apps/installed/
launch/stop/notify (全アプリ開放 + C の 3 不変条件) + `// @app`
インストール + 全 examples の setAppName 宣言 + UI アプリの
onForeground 移行 (ssh_vt / mqtt_demo / p4_bg_app / launcher)。
**P4c も完了** (2026-06-11、実機検証済み — §4.5): retained メッセージ =
ストアの棚 (apps/+ 購読で接続毎に同期、バイト比較で冪等、install-only)、
tombstone = 空 retained でアンインストール、manifest ディレクティブ
(@app/@title/@icon/@perm — perm は表示のみ)、sys.uninstall + ランチャー
○ 行のゴミ箱ボタン (LV_SYMBOL_TRASH、停止の ✕ と区別)、sys.notices() +
ランチャー通知セクション + バー通知行タップで発信アプリを開く、
mqjs_push.py --retain/--delete。**Nerd Font 統合も完了** (HackGen
Console NF: 端末 17px に NF BMP 範囲、UI 20px を ui_font 連鎖 3 段目に;
ステータスバーの状態グリフ・チップのモードグリフ・通知ベル・@icon の
ランチャー表示 — 全部実機で表示確認済み 2026-06-11)。見送り
(発動条件 = 署名鍵以外の出所): 権限強制 / store 名前空間強制。
**P4d (typed clipboard IPC) も実装済み** (2026-06-11、§7 参照 —
clipboard.set/get/onChange、C 共有バッファ + NVS 永続、EV_CLIP は
setter 以外の全 onChange 登録アプリへ slot+世代で配送。PC 2 アプリ
スモーク green: tools/p4_clip_a.js + p4_clip_b.js)。同日 T3a
(ssh-terminal-design §7 のコントロールバー) も実装 — Paste が
clipboard.get() を消費する最初の利用者。

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
    uint16_t gen;             /* slot 再利用の stale イベント検出 */
    char name[32];            /* sys.setAppName / 起動元から */
    uint8_t *mem;             /* 固定アリーナ (下記 3.6) */
    JSContext *ctx;           /* アプリ生存中ずっと生きる */
    TimerSlot timers[16];
    GpioSlot  gpio_cb[8];
    MqttSub   mqtt_subs[8];  bool onconn_used; JSGCRef onconn;
    esp_mqtt_client_handle_t mqtt;  /* client_id="mqjs-app-<slot>" */
    SshCb     ssh_cbs[4];
    WidgetCb  widget_cbs[48]; /* 前面時のみ実体画面あり */
    JSGCRef   touch_cb, key_cb, fg_cb, bg_cb, signal_cb;  bool ..._used;
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
| EV_SIGNAL (新設) | 対象 slot を明示(sys.signal の app 間シグナル、§3.8) |

**所有権の不変条件**: イベントは `slot + 世代` を運び、ディスパッチャが
「スロット生存 + 世代一致」を検査して解決する。死んだ slot 宛の
in-flight イベントは payload を free して捨てるだけ — **app_stop は
キューを掃除しない**(FreeRTOS キューから選択的に抜く必要がない。
W1 ウィジェット世代・W3 セッション id と同じパターンの 3 度目の適用)。

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

### 3.6 メモリ管理 (確定詳細)

3 層で戦略を分ける:
1. **JS コンテキスト = 固定アリーナ**(§2 の動的案から変更): 起動時に
   PSRAM へ `MQJS_MAX_APPS × 256KB = 1MB` を一括確保しスロット固定。
   app_stop でも返さない。32MB 中の 1MB は誤差で、(a) app_start が
   OOM で失敗するモードが構造的に消える(ランチャーの「起動」が常に
   成功)、(b) 断片化監視が不要になる。コンテキスト内部は
   コンパクション GC(断片化なし、finalizer は GC/teardown 遅延 —
   W1 実測どおり)。
2. **AppSlot(C 側 binding 状態)= 静的配列**: ~2KB/slot × 4 ≈ 8KB
   内部 RAM。GCRef は自 ctx 内を指し、app_stop の per-app reset で
   一括解放。
3. **共有資源の所有権**: ssh セッション・gpio ピン・mqtt クライアントは
   取得時に slot を記録。app_stop は**自分の分だけ**畳む(他アプリの
   セッションに触らない)。in-flight イベントは §3.2 の不変条件で解決。

### 3.7 スケジューラ (確定詳細)

```c
for (;;) {
    /* 1) 全スロットのタイマー走査; 期限到来分を所有 ctx で実行。
          戻り値 = 最近接期限までの ms (上限 50ms) */
    idle = run_all_timers();
    /* 2) イベント 1 個をオーナーへ直列ディスパッチ */
    if (xQueueReceive(q, &ev, idle)) {
        slot = resolve_owner(&ev);                 /* §3.2 */
        if (!alive(slot, ev.gen)) { free_payload(&ev); continue; }
        s_cur_app = &apps[slot];   /* ← 全 binding はこれを参照 */
        dispatch(apps[slot].ctx, &ev);             /* 5s watchdog */
    }
    /* 3) 刈り取り: pending なしスロットは app_stop
          (dev slot のみ 1s 後再起動を予約 = 既存挙動互換) */
    reap_idle_slots();
}
```
- **`s_cur_app` 1 本が協調モデル最大の配当**: ディスパッチが直列なので
  全 binding は「呼び出し元アプリ」をグローバルポインタ 1 個で知れる。
  TLS・ロック不要。binding 改修は「static 参照 → s_cur_app-> 参照」の
  機械的置換。
- 公平性 = イベント到着順 FIFO + タイマーは毎周全スロット走査。
  優先度・プリエンプションなし。1 ディスパッチ上限 = 5s watchdog
  (他アプリの最悪レイテンシ)。flood はキュー深さ 32 で bounded。
- ライフサイクル/シグナルも同じキューを通す → 他イベントと自然に
  直列化し、切替中の順序バグが構造的に起きない。
- **`delay(ms)` はマルチアプリ下で全アプリを止めるアンチパターン**
  (dev slot 互換で残すが README に明記。将来は警告 or slot 制限)。

### 3.8 sleep / シグナル / バックグラウンド通知

- **自然スリープはタダ**: イベントもタイマーも持たないアプリは CPU を
  一切食わない(ctx は PSRAM に眠るだけ)。`setTimeout(fn, 60000)` が
  そのまま「60 秒 sleep」。
- **`sys.signal(name, value)` / `sys.onSignal(fn)`**(P4a): EV_SIGNAL +
  slot 毎の GCRef + 名前→slot 解決だけの最小 app 間 IPC。typed
  clipboard(P4d)より先に手に入る。`onSignal` 登録は anything_pending
  を真にする = 「シグナル待ちで生存」が成立し、これが sleep からの
  復帰トリガーになる。
- **`sys.notify(text)`**(P4b): ステータスバーは layer_top のシステム
  クローム = どのアプリが前面でも見える通知面。実装は既存
  `ui_status_set_event` の公開 + 発信アプリ名の前置。拡張(P4b 内):
  アプリ毎最終 1 件をランチャー行に表示 / 通知ラベルのタップで発信
  アプリへ focus(バーの領域分けは実装時に調整)。
- **`sys.suspend()` は API 予約のみ**(実装はユースケース待ち)。
  実装する場合の決定済みセマンティクス: suspended スロット宛イベントは
  **捨てる**(共有 FIFO に留め置くと他アプリを頭詰まりさせるため)、
  復帰は EV_SIGNAL のみ通す。

## 4. P4b: ランチャー

- **ランチャー自体を組み込み JS アプリにする**(slot 0、起動時に
  embedded ソースから自動開始、停止不可)。ウィジェットフレームワークを
  そのまま使う(dogfooding): アプリ一覧 = `ui.screen` + list。
- **導線(2026-06-11 のユーザー協議で確定、当初案から変更)**:
  - **バーに「直前アプリ」チップ**を常設: アプリ名を書いたボタンで、
    タップ = そのアプリを**開く**(動作中なら focus、停止中なら
    launch → focus。停止中はディム表示)。スマホの通知/アイコンと同じ
    「タップしたらそのアプリが前面で動き出す」一貫意味論で、生死は
    ユーザーの関心事にしない。直前アプリが無いときは「アプリ一覧」
    チップになる(ランチャーへの可視導線を常に確保)。
  - **ホームへの導線 = バーの長押し**(layer_top のバーは全画面で
    見えている = どこからでも届く)。長押しには必ず視覚フィードバック:
    押下中にプログレスストリップが伸び、閾値で「離すとランチャー」の
    武装表示、コミットはリリース時、指を外せばキャンセル。
    閾値とアニメは StatusBar 内で自前駆動(LVGL の long_press_time に
    依存しない)。
  - チップの「開く」は C から直接 launch せず、**open 要求をランチャー
    (slot 0)へ signal 規約で投函**(差出人 "system"、
    `{"op":"open","app":<name>}`)。focus/launch の分岐とソース出所の
    解決はランチャーが持つ。通知タップ(sys.notify)も同じ経路に乗る
    ので、停止中アプリ宛の通知も自然に成立する。チップの識別子は
    slot ではなく**アプリ名**(再起動でスロットが変わるため)。
    生存判定はタップ時の整数比較のみ + 切替/停止イベントでのラベル
    追従(ポーリングなし)。
  - この結果、ランチャーの常駐・停止不可は単なる安全策ではなく
    **構造的要件**になる(クロームの open 経路が依存)。
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
  - `sys.setAppName(name)`(vision の T3 項目を取り込み。重複名は
    false を返して変更しない — 名前は signal/開く操作の識別子)。
    全 examples の先頭に追加する。
  - **権限はランチャー専用にしない**(2026-06-11 のユーザー協議で
    当初案から変更): 信頼境界は Ed25519 署名ゲートで、走るコードは
    全部オーナー署名 — 制限が防ぐのはバグだけで、stop の被害は
    「消える」止まり(状態破壊なし)。制限すると watchdog アプリ・
    ヘルパー起動・自己終了などの構成パターンを殺し、結局ランチャー
    宛 RPC を自作する迂回が生まれる。代わりに**不変条件を C で強制**:
    ① slot 0(ランチャー)は停止不可、② dev スロットの明示停止は
    次の push / 明示的な「開く」まで自動再実行を抑止、③ 停止時に
    `stopped by <name>` の帰属ログ。default-deny へ倒すのは P4c で
    署名鍵以外の出所が現れたとき(manifest の `permissions:["apps"]`
    にする — ランチャーは「apps 権限を持つ普通のアプリ」に着地)。

## 4.5 P4c: MQTT アプリレジストリ + manifest (設計 2026-06-11)

**レジストリ = broker の retained メッセージそのもの。** 別途のインデックス
やデータベースは作らない:

- 配布 topic: `<TASK_TOPIC>/apps/<name>`。署名付き JS (dev push と同じ
  Ed25519 封筒) を **retain=1** で publish したものが「ストアの棚」。
  デバイスは既存の task_source クライアントで `apps/+` を購読し、
  (再)接続のたびに棚全体が retained 配送で届く = それが同期。
- **冪等な同期**: 受信ペイロードをインストール済みファイルと
  バイト比較し、同一なら黙ってスキップ (再接続のたびの retained 再配送で
  ストレージも通知も荒れない)。差分あり → 保存して
  「installed: / updated: <name>」を status + イベント表示。
  **自動実行は決してしない** (§6 の確定事項)。実行中アプリの新版も
  保存+通知のみ — 再起動はユーザー操作。
- 防御: ペイロード先頭の `// @app <name>` と topic の <name> が一致
  しないものは拒否 (棚の取り違え防止)。
- **tombstone = 空ペイロードの retained publish** (MQTT の retain 消去
  そのもの)。受信したらアンインストール。空は署名できないので
  ここだけ無署名を受けるが、LAN 限定 broker で被害は「消える」のみ
  (インストールは依然署名必須) — 脅威モデル §4 と整合。
- ツール: `mqjs_push.py ... --retain` (PUBLISH retain bit) /
  `--delete` (空 retained = tombstone)。

**manifest = JS ファイル先頭のディレクティブ行** (別ファイルにしない:
ソース自己記述で、署名が manifest ごと覆う):

- `// @app <name>` — 必須。識別子 (= setAppName / topic suffix)。
- `// @title <表示名>` — 任意。ランチャー一覧の人間向け表示。
- `// @icon <グリフ>` — 任意。Nerd Font の 1 文字 (ui_font 連鎖の NF
  フォールバックで描画、アセット不要)。ランチャー行頭に表示。
- `// @perm <カンマ区切り>` — 任意。宣言のみ parse して表示する。
  **強制 (default-deny) は引き続き「署名鍵以外の出所が現れたとき」**
  (P4b 権限議論の発動条件のまま)。
- `sys.installed()` は [{name, title}] を返すよう拡張 (title 無しは
  name にフォールバック)。

**アンインストール**: `sys.uninstall(name)` (littlefs の削除、全アプリ
開放 — 同じ信頼境界論)。ランチャーの ○ 行にも ✕。注意:
**レジストリ配布物のローカル ✕ は「次の同期まで」** — broker に retained
が残っていれば再接続で戻る。恒久削除は tombstone (それが
「ストア購読」の正しい意味論)。実行中インスタンスはどちらでも無傷。

**通知拡張 (P4b §3.8 から繰り越し)**:

- ランタイムがアプリ毎の最終通知 1 件を保持 (名前キーの小テーブル、
  アプリ停止後も残る)。`sys.notices()` → [{app, text}] (新しい順)。
- ランチャーに「通知」セクション: 行タップで発信アプリを開く
  (open 経路に乗るので停止済みアプリでも起動して開く)。
- ステータスバーのイベントラベルをタップ可能に: 表示中テキストの
  先頭 `[name] ` を parse して `mqjs_request_open(name)`。表示内容
  そのものを parse するので「古い通知の宛先に飛ぶ」事故が構造的に
  ない。ラベルは独立タップ対象なのでバーの長押し状態機械とは干渉
  しない。

**見送り (P4c 範囲外と明示)**: 権限強制 / store 名前空間強制 (NVS キー
15 字の予算と単一オーナー運用のため、発動条件は権限強制と同じ
「他人のコードが入るとき」)。アイコンは当初見送りだったが、Nerd Font
統合 (`@icon` = ただの 1 文字) でコストが消えたため P4c 内で実装済み。

## 5. フェーズ計画

| | 内容 | 規模 |
|---|---|---|
| **P4a** | AppSlot 構造体化 + イベントルーティング + 前面切替 + ライフサイクル + sys.signal/onSignal + 背景 ui.* ゲート + screen の「構築後アニメ」(§3.4) | 中〜大 |
| **P4b** | ランチャーアプリ(slot 0)+ ステータスバータップ + sys.apps/launch/stop/focus/notify + @app インストール | 小〜中 |
| P4c | topic 毎レジストリ + manifest(name/icon/permissions)+ store の名前空間強制 | 中 |
| **P4d** | typed clipboard IPC(C 共有バッファ、ssh-terminal-design §7 の設計を実装)— **済 2026-06-11、§7** | 小〜中 |

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
- ~~メモリ断片化~~: §3.6 で**固定アリーナに確定**したため解消
  (app_start/stop で PSRAM ヒープを churn しない)。
- **256KB/アプリの妥当性**: ssh_vt(最大級)が 3 セッション分のモデルを
  持っても収まる実績あり。アプリ毎に可変させるのは manifest(P4c)待ち。
- **「publish → 即置換」は dev スロット限りの開発挙動**(ユーザー指摘
  2026-06-11): P4c で MQTT がアプリ配布(app store)化するとき、この
  意味論をレジストリ topic に持ち込まないこと。配布 topic の受信は
  **インストール(保存)まで**で、実行開始はユーザー操作(ランチャー /
  チップの「開く」)経由 — P4b-lite の `// @app` ディレクティブ
  (保存のみ、置換実行しない)がその先行形。実行中アプリの新版が
  届いた場合も勝手に差し替えず「更新あり」を通知してユーザーが再起動、
  が基本線(常駐アプリの状態を黙って破壊しない)。dev topic の即置換は
  開発体験のための仕様としてそのまま残す。

## 7. P4d: typed clipboard IPC (実装 2026-06-11)

`docs/ssh-terminal-design.md` §7 の先行設計を実装したもの。**型付きの
システム共有値 1 個**を C が所有する — 全 JS コンテキストの外なので
アプリ停止・前面切替・(NVS 永続により) 再起動を跨いで生存する。
「電卓の結果を SSH 端末に貼る」型のアプリ間データ受け渡しの第一歩。

- **JS API**: `clipboard.set(data[, type])` → bool /
  `clipboard.get()` → `{data, type}` | undefined /
  `clipboard.onChange(fn(data, type))`。type は自由形式の MIME 風
  文字列 (`text/plain` 既定、`text/csv` `application/json` `number`
  …) で受け手が解釈を決める。データ上限 4000B (NVS blob 1 個 =
  80×33 画面の全選択が収まる)、型 31B。
- **通知 = EV_CLIP**: set したアプリ**以外**の onChange 登録アプリへ
  slot+世代でポスト (setter 除外は将来の MQTT ミラーアプリが自分の
  書き込みをブローカーへ echo し返さないための構え)。イベントは
  ペイロードを運ばず、ハンドラはディスパッチ時点の現在値を受ける =
  latest wins。onChange 登録は anything_pending を真にする
  (純粋なクリップボードリスナーとして生存できる)。
- **永続**: NVS namespace `mqjsclip` (store.* の `mqjs` とキー衝突
  しない)。§7.1 のローカルファースト — retained MQTT には頼らない。
  ブローカー同期 (層2) は API に焼き込まず外付けミラーアプリで。
- **検証**: PC 2 アプリスモーク (tools/p4_clip_a.js + p4_clip_b.js、
  run_pc) — 相互 onChange / setter 除外 / get() 往復 green。
- 最初の消費者: T3a コントロールバーの Paste
  (= `ssh.write(clipboard.get().data)`)。Copy は選択 UI (T3b) 待ち
  → T3b で実装済み (ssh-terminal-design §7 T3b、実機確認 2026-06-11)。

## 8. `// @autostart` ディレクティブ (実装 2026-06-12)

manifest に `// @autostart` の 1 行があるアプリを、**opt-in 済みなら**
ブート時に自動 launch する。動機 = clip_mirror のような常駐ブリッジが
再起動のたびに手動起動になるのを解消。

- **opt-in 方式で「配布物は自動実行しない」(§6) を再起動にも延長**:
  棚から @autostart 付きアプリが届いただけでは何も起きない。
  **ローカルで一度 launch した瞬間** (ランチャーのタップ / sys.launch
  の littlefs 解決、その時点のファイルが @autostart を宣言している
  こと) に opt-in 名簿へ載る。名簿 = NVS `mqjsauto`/`optin` の
  カンマ区切り文字列 (上限 480B)。`sys.uninstall` で名簿からも外れる。
- **ブートパス** (`autostart_boot`、スケジューラループ開始前に 1 回):
  /littlefs/apps/ を走査し「名簿にいる ∧ 現ファイルがまだ @autostart
  を宣言」のものを空きユーザースロット (2-3) に起動。更新で
  ディレクティブが消えたアプリは起動しない (名簿はそのまま)。
  順序 = ディレクトリ順。スロット溢れ・起動失敗はログ+通知のみで
  ブートは止めない。前面は奪わない (起動だけ)。
- **可視化**: 起動できた一覧を `[system] autostart: <names>` として
  通知テーブル (ランチャー通知セクション) + ステータスバーに 1 行。
- 適用第 1 号: examples/clip_mirror.js が @autostart 宣言。

## 9. ランチャーの app store サブ UI (実装 + 実機確認済み 2026-06-12)

動機 (ユーザー): MQTT 経由のスクリプト取得は自由度に乏しい —
アプリの一覧と説明を確認できる UI が欲しい。**不変条件: メイン画面 =
アプリスイッチャー (● 実行中 + 通知 + open 経路) の役割は不可侵**。
ストアは `ui.screen` のサブページとして増築する。

- **メイン画面の再編 (ユーザー確定)**: ○ (インストール済み・停止中) の
  行をメインから外し、メインは「● 実行中 (タップ=切替、✕=停止) +
  通知 + [ストア] ボタン」だけにする。スイッチャーとしての一覧性を
  最優先。停止中アプリの起動はストア経由 (1 タップ深くなる) か
  チップで。
- **新ディレクティブ `// @desc <一行説明>`**: ソース自己記述 manifest
  の延長 (署名が覆う)。`sys.installed()` を
  `{name, title, icon, perm, desc, autostart, optin}` に拡張
  (installed_push に manifest_field/@desc + manifest_has/@autostart +
  opt-in 名簿照会を追加。新 binding 不要 = stdlib 再生成不要)。
- **ストアページ**: 棚 (= littlefs、retained 同期でインストール済み) の
  全アプリを `@icon @title` で一覧。実行中は ● を前置。行タップで
  詳細ページへ。
- **詳細ページ**: タイトル / name / @desc / @perm (表示のみ) /
  ファイルサイズ / 実行状態 / @autostart 宣言と opt-in 状態。
  アクション: [開く] (既存 openApp = focus-or-launch 経路に乗せる) /
  [アンインストール] (sys.uninstall。棚に retained が残っていれば
  次の同期で戻る旨を表示 — 恒久削除は tombstone、という P4c の
  意味論をそのまま案内する)。
- **範囲外と明記**: 選択インストール (棚の全自動同期をやめて
  「ストアで選んだものだけ入れる」) はレジストリのプロトコル変更
  (インデックス topic 等) が要るため次弾。現状は 棚 = インストール済み
  の同期意味論を維持する。

## 10. TODO (次弾候補)

- 選択インストール (§9 範囲外項目): インデックス topic + 本体の
  オンデマンド取得。棚の「全部入り」をやめるとき。→ **§11 で設計済み**。

## 11. ストアカタログ + 選択インストール (設計 2026-06-12)

動機 (ユーザー): ランチャーから「インストール前のアプリ」が見えない。
app store には必須の機能。

### 11.1 現状分析 — なぜ一部しか見えないか

- ランチャー一覧 = `sys.installed()` = デバイスの `/littlefs/apps/`。
  リポジトリ examples/ は PC 側ソースであり、デバイスには
  `// @app` manifest 付きで署名 push したものしか入らない
  (2026-06-12 時点で @app 持ちは clip_mirror / p4_bg_app の 2 本のみ。
  残りは dev タスク世代のスクリプトで、ランチャーには構造的に出ない)。
- §4.5 の同期意味論は「棚 (retained) = 接続のたび全量自動インストール」。
  **「棚にあるが未インストール」という状態がそもそも存在しない** —
  「見る→選ぶ→入れる」というストア体験の前提が欠けている。

### 11.2 プロトコル: カタログ topic の追加

- **カタログ**: `<TASK_TOPIC>/store/<name>` に retained で publish する
  **スクリプト先頭の manifest コメントブロックそのもの** (+ ツールが
  `// @size <bytes>` を追記)。新しいフォーマットを発明しない:
  デバイスには manifest_field パーサが既にある。1 エントリ数百バイト。
- **本体**: 既存どおり `<TASK_TOPIC>/apps/<name>` retained (Ed25519 封筒)。
- **カタログは無署名を許す** (tombstone と同じ理屈): 偽装されても
  起きるのは「ストアに嘘の行が並ぶ」まで。インストール = 本体の
  署名検証は不変なので、コードの信頼境界は §4.5 から動かない。
- tombstone: `--delete` は store/ と apps/ の両方に空 retained。

### 11.3 デバイス: 購読モデルの変更 (task_source.c)

現行の `apps/+` ワイルドカード購読 (= 全量同期) をやめ、

1. 接続時: `store/+` を購読 (カタログ常時同期、RAM 上の小テーブル
   — 24 エントリ × name 25B + head 224B ≈ 6KB 静的)。
2. 接続時: `/littlefs/apps/*.js` を列挙し、**インストール済みの分だけ**
   `apps/<name>` を個別購読 — 更新と tombstone は従来どおり届く
   (= 既存インストール分の挙動は不変。clip_mirror 等は何も変わらない)。
3. `sys.install(name)`: `apps/<name>` を購読 → retained 本体が届く →
   既存 registry_rx の検証+保存パスへ (コード再利用、自動実行なしも
   §6 のまま)。以後は購読継続 = 更新も流れる。
4. `sys.uninstall(name)`: runtime に uninstall hook を追加し
   (print sink と同じ登録パターン、mqjs→main の依存逆転を回避)、
   main 側で `apps/<name>` を unsubscribe。

**意味論の改善が副産物**: §4.5 の注意書き「ローカル ✕ は次の同期で
戻る」が消える。アンインストール = 購読解除なので戻らず、ストアの
「入手可能」に並び直す — ユーザーの自然な期待と一致する。

### 11.4 JS API (binding は store provider 経由)

- `sys.store()` → `[{name, title, icon, desc, size, installed}]`
  (カタログ ∪ インストール済み。installed はファイル存在で判定)。
- `sys.install(name)` → bool (購読要求の成否。完了は非同期:
  既存の「installed: <name>」status/イベント通知が完了報)。
- runtime は `mqjs_set_store_provider(count/get/install の関数表)` を
  公開し main が登録する。未登録 (PC ビルド) では store() = 空配列。

### 11.5 ランチャー UI (§9 ストアページの増築)

- ストアページを 2 セクション化: 「インストール済み」(現行) +
  「**入手可能**」(installed=false のカタログ行、@icon @title)。
- 入手可能の詳細ページ: @desc / @perm / サイズ表示 +
  [インストール] → `sys.install(name)` → 「インストール中…」表示
  → 完了通知後の build() 再描画で済み側に移る。

### 11.6 ツール (mqjs_push.py)

- `--shelf` フラグ新設: `apps/<name>` へ署名本体 + `store/<name>` へ
  manifest ヘッダ (+@size) を、どちらも retain=1 で publish。
  name はファイルの `// @app` 行から取る (topic の手書きミス防止)。
- `--delete` を両 topic tombstone に拡張。

### 11.7 互換と移行

- 旧ファーム共存: 旧デバイスは `apps/+` 購読のままなので、棚に本体が
  retained で残っている限り従来どおり全量同期する (カタログ topic は
  無視される)。新ファームだけが選択インストールになる。
- カタログ未掲載のインストール済みアプリ (旧経路で入れたもの) は
  installed 側の一覧に出るだけ — 欠落しない。
- examples のストア掲載は別作業: 各ファイルに `// @app @title @icon
  @desc` を付与して `--shelf` で publish (掲載候補の選定はユーザー)。

### 11.8 見送り (この弾の範囲外)

- バージョン表記 / 更新差分表示 (`// @ver` は将来ディレクティブ)。
- カタログの署名 (LAN 限定 broker の脅威モデルでは過剰、§4.5 踏襲)。
- インストール進捗 UI (retained 配送は実測ほぼ即時、通知で足りる)。
