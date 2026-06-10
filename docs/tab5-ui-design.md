# Tab5 UI 設計書 — mqjs プラットフォームの「顔」

Status: **Phase 4a 実装完了** (2026-06-10、ターミナル布石: textSize/keyboard/onKey。実機確認はユーザー操作待ち)
対象: M5Stack Tab5 (ESP32-P4 rev v1.3 + ESP32-C6, 5" 1280x720 MIPI-DSI, GT911 タッチ)

## 0. ゴール / 非ゴール

**ゴール**

- push した JS タスクの挙動が**画面で即見える**こと (print 出力・受理通知・実行状態)
- 将来、**JS タスク自身が画面とタッチを使える**こと (`ui.*` バインディング)
- 日本語表示 (コンソール・ラベルとも)
- Stamp-P4 ビルドへの影響ゼロ (UI は Tab5 専用のオプトイン)

**非ゴール**

- 汎用 GUI ビルダー的なリッチウィジェットを JS に公開すること (v1 は描画プリミティブまで)
- 動画・カメラ・オーディオ (Tab5 にはあるが本プラットフォームの範囲外)
- Stamp-P4 での画面対応 (ハードが無い)

## 1. 技術スタック (確定)

```
mooncake             画面/App ライフサイクル管理のみ (= 抑制的利用)
smooth_ui_toolkit    アニメーション (AnimateValue/spring) + LVGL C++ ラッパー
LVGL 9 + esp_lvgl_port   描画・入力。espressif 管理部品
esp_lcd (DSI/ILI9881C) + esp_lcd_touch_gt911   パネル/タッチ (UserDemo と同部品)
jp_font              Noto Sans CJK JP サブセット (lv_font_conv 生成、後述)
─────────── ここまで C++。components/ui_tab5 に完全封印 ───────────
公開 C API (ui_tab5.h)  ←  main / mqjs ランタイムは C のまま
```

### 選定理由 (要点のみ)

- **LVGL vs M5GFX**: Tab5 は MIPI-DSI でフレームバッファ常時走査のため、M5GFX の
  伝統的なメモリ優位が消える。720p では「変わった部分だけ描く」が支配的で、
  LVGL のダーティ領域差分描画が実効効率で勝つ。smooth_ui_toolkit の
  ウィジェット層 (lvgl_cpp) と jp_font の前例 (StackChan-dazo) も LVGL 側。
  Phase 2 の即時描画 API は LVGL canvas バッファへの自前描画で実現する
  (描画ライブラリの二重搭載はしない)。
- **mooncake**: ライフサイクル糊 (~500 行・依存ゼロ・MIT)。採用コストほぼゼロで、
  Phase 4 のマルチタスクスロット+ランチャー構想に構造がそのまま合う。
  **使い方は抑制的**: 画面遷移管理のみ。描画・状態・キューは本体コードに置き、
  Ability は「いつ描くか」だけ知る。いつでも剥がせる状態を保つ。
- **mooncake / smooth_ui_toolkit は vendoring** (mquickjs/TweetNaCl と同じ流儀。
  コミット ID を README に記録)。LVGL 系はレジストリ部品。

## 2. アーキテクチャ

```
┌──────────────────────────── Tab5 ────────────────────────────┐
│ ui_task (Core1, C++, prio 低)        js_task (Core0, 既存)    │
│  LVGL tick/timer + Mooncake::update() mqjs イベントループ      │
│   ├─ StatusBar (UIAbility)            │                      │
│   ├─ ConsoleApp (AppAbility)          │                      │
│   └─ CanvasApp (AppAbility, Phase2)   │                      │
│                                       │                      │
│  [状態] ui_status 構造体 ◄─ mutex ─── wifi.c / task_source.c  │
│  [ログ] 行リングバッファ ◄─ mutex ─── print sink (js_task から)│
│  [描画] UiCmd キュー     ◄─ queue ─── JS: ui.* (Phase2)       │
│  [入力] GT911 ─► mqjs_post_touch() ─► 既存 MqjsEvent キュー    │
│                                        └► JS: ui.onTouch(fn) │
└──────────────────────────────────────────────────────────────┘
```

**不変条件 (これまでと同じ設計原則)**

1. JS コンテキストに触れるのは js_task のみ。UI→JS は既存の MqjsEvent
   キュー経由 (gpio/mqtt と同型)。JS→UI は専用キュー/リングバッファ経由。
2. JSValue・JS ヒープへのポインタをタスク間で渡さない。文字列は必ずコピー。
3. mqjs ランタイムのウォッチドッグ・GC・使い捨てコンテキスト設計に変更なし。

**コア配置**: js_task を Core0、ui_task を Core1 に明示ピン留め
(現在 affinity 未指定なので、このタイミングで固定する)。

## 3. モジュール境界

### components/ui_tab5 (新規, C++17)

公開ヘッダ `ui_tab5.h` は C リンケージのみ:

```c
void ui_tab5_start(void);                     // 画面初期化 + ui_task 起動
void ui_tab5_log(const char *line, size_t n); // コンソール 1 行 (UTF-8)
void ui_tab5_set_status(const ui_status_t *); // 状態スナップショット更新
// Phase 2:
bool ui_tab5_cmd(const ui_cmd_t *);           // 描画コマンド投函 (満杯なら false)
```

`CONFIG_MQJS_TAB5_UI=n` (デフォルト) のとき: コンポーネントは空登録になり、
ヘッダはすべて no-op の inline スタブを提供する (board_tab5.h と同じ手法)。
→ main 側に `#ifdef` を散らさない。Stamp ビルドはフラッシュ 1 バイトも増えない。

### mqjs ランタイムへの変更 (最小)

- `mqjs_set_print_sink(void (*fn)(const char *, size_t))` を追加。
  js_print / dump_error の出力を stdout と sink の両方に流す (tee)。
  → ConsoleApp はこれだけで JS の print を取得できる。Phase 1 では
  ランタイム変更はこの 1 点のみ。
- Phase 2 で `ui` オブジェクトを device_stdlib.c に追加 (ヘッダ再生成、
  WSL。手順は README 既出)。
- Phase 3 で MqjsEvent に EV_TOUCH を追加し、公開 API
  `mqjs_post_touch(x, y, kind)` を生やす (ISR 不要、GT911 ポーリングは
  ui_task 側で実施して投函)。

### 状態フィード (キュー不要の設計)

```c
typedef struct {
    char task_name[32];      // "task" / "mqtt-task" 等
    char task_origin[16];    // embedded / mqtt / persisted
    char ip[16];
    bool wifi_up, mqtt_up;
    char last_event[48];     // "accepted (3644B)" / "bad signature" 等
} ui_status_t;
```

書き手 (wifi.c / task_source.c / app_main.c) は変化時に
`ui_tab5_set_status()` を呼ぶだけ。UI 側は mutex 付きスナップショットを
フレーム毎に読む。イベントの取りこぼしという概念自体を持たない。

### コンソール

- 行リングバッファ: 200 行 × 256 バイト入力 (+recolor マークアップ余白、
  固定長、mutex 保護、古い行から破棄)。sink の行分割幅 = 画面上の
  1 レコード長なので、96B だと日本語 ~32 文字 (幅の 8〜9 割) で
  見かけ上の改行が入ってしまう (実機で発覚) — 256B に拡大
- producer: print sink (js_task 上で呼ばれる → コピーのみで即 return)。
  コピー時に変換: **ANSI SGR カラー → LVGL recolor マークアップ**
  (16 色パレット、色状態は行を跨いで持続)、\t → スペース 4 個、
  その他の CSI シーケンス/制御文字は除去、'#' はエスケープ
- consumer: ConsoleApp がフレーム毎に新着行を recolor 付き lv_label へ反映
- スクロールはタッチでフリック (LVGL 標準のスクロール)、最下部付近に
  いるときだけ tail-follow

### 描画コマンドキュー (Phase 2)

```c
typedef struct {
    uint8_t op;            // CLEAR / FILL / RECT / LINE / TEXT / PIXEL
    int16_t x, y, w, h;
    uint32_t color;        // 0xRRGGBB
    char *text;            // TEXT のみ。heap コピー、消費側が free
} ui_cmd_t;
```

- 深さ 64 の FreeRTOS キュー。**満杯時は非ブロッキングで破棄**し、ドロップ
  カウンタを画面に出す (JS を待たせない・背圧を可視化する)。
- CanvasApp はフレーム毎に全件消化 → lv_canvas バッファへ自前プリミティブで
  描画 → invalidate はフレーム 1 回に合体。
- JS API 案: `ui.size()` → `[w,h]`、`ui.clear(c)` `ui.fill(c)`
  `ui.rect(x,y,w,h,c)` `ui.line(x0,y0,x1,y1,c)` `ui.text(x,y,str,c)`
  `ui.pixel(x,y,c)`。色は 0xRRGGBB の整数。
- 論理解像度はステータスバーを除いた canvas 領域 (実装時に確定し
  `ui.size()` で照会可能にする)。

### タッチ (Phase 3)

- GT911 を ui_task でポーリング (esp_lcd_touch_gt911)
- down/move/up を `mqjs_post_touch()` で MqjsEvent キューへ
- JS: `ui.onTouch(function(x, y, kind) {...})` — gpio.onChange と同じ
  GCRef 保持パターン。レート制限 (~50Hz) は投函側で行う。

## 4. 日本語フォント

- **レシピ (StackChan-dazo で実証済み)**: hiz8 の `Noto-Sans-CJK-JP.min`
  (常用漢字サブセット TTF) を `lv_font_conv --size 20 --bpp 4 --no-compress
  --format lvgl --range 0x20-0xFFFF` で C ソース化し、
  `components/ui_tab5/fonts/` にコミット。
- フラッシュ実測見込み ~1.5MB → **v1 はサイズ 20 の 1 本のみ**。見出しは
  transform 拡大で代用し、必要になってから 2 本目を検討。
- **mquickjs の UTF-8 パススルーは検証済み** (2026-06-10, run_pc):
  `print("こんにちは世界")` は無傷で出力され、`"あいう".length === 3`
  (コードポイント単位)。JS からの日本語コンソール出力は問題なし。
- LVGL は UTF-8 ネイティブ。ラベルにそのまま渡せる。

## 5. パーティション変更 (要 erase-flash)

フォント+LVGL でアプリが 3MB を超えるため:

```
# 変更前                          # 変更後
factory  0x10000  0x300000 (3MB)  factory  0x10000  0x600000 (6MB)
storage  0x310000 0x100000 (1MB)  storage  0x610000 0x100000 (1MB)
```

16MB フラッシュなので余裕。partitions.csv は Stamp と共通のまま変更する
(Stamp 側は空き領域が増えるだけ)。**切替時に一度 erase-flash が必要**で、
永続化済みタスクと NVS は消える (WiFi 認証情報は sdkconfig 由来なので無傷)。

## 6. フェーズ計画と受け入れ基準

| Phase | 内容 | 受け入れ基準 |
|---|---|---|
| **0. スパイク ✅** (2026-06-10) | IDF 6.0.1 + DSI + esp_lvgl_port + LVGL9 で Hello World、jp_font で日本語ラベル、パーティション拡張 | Tab5 に「こんにちは世界」が表示される |
| **1. コンソール ✅** (2026-06-10) | mooncake/smooth_ui_toolkit 導入、StatusBar + ConsoleApp、print sink、状態フィード | Web UI から push → 画面に accepted 通知と print 出力 (日本語込み) が流れる |
| **2. ui.\* 描画 ✅** (2026-06-10) | UiCmd キュー、CanvasApp、stdlib に `ui` 追加 (ヘッダ再生成)、examples/ui_demo.js | push した JS だけで時計/グラフが画面に描ける。PC 版はスタブ print |
| **3. タッチ ✅** (2026-06-10) | ST7123/GT911 → EV_TOUCH → `ui.onTouch`、examples/touch_demo.js | JS だけでタッチ反応するデモが動く |
| **4a. ターミナル布石 ✅** (2026-06-10) | `ui.textSize` (同期メトリクス)、`ui.keyboard` + `ui.onKey` (LVGL オンスクリーンキーボード → EV_KEY)、examples/kbd_demo.js | JS だけで行エディタ (入力・BS・Enter・履歴) が動く |
| **4. 構想 (未確定)** | 永続化タスクの複数スロット化 + mooncake ランチャーでタップ切替。Stack-chan (CoreS3) との MQTT 連携デモ。**JS によるターミナルエミュレータ実装** (ユーザー目標 2026-06-10: ANSI カラー対応・4a の keyboard/metrics はその布石) | — |

各 Phase 完了ごとにコミット。Phase 0/1 は JS API 変更なしなので
ヘッダ再生成不要。

## 7. リスクと対策

| リスク | 影響 | 対策 |
|---|---|---|
| LVGL/esp_lvgl_port/DSI ドライバの IDF 6.0 非対応 | Phase 0 が進まない | 最初に検証 (だから Phase 0)。ダメなら部品バージョン固定 or 小パッチ (dazo に IDF6 パッチ前例あり)。最終フォールバックは UserDemo の BSP 移植 |
| smooth_ui_toolkit lvgl_cpp と LVGL 9 最新の API ずれ | ウィジェット層が使えない | コアのアニメーションだけ使い、ウィジェットは素の LVGL で書く (依存を一段弱める) |
| 720p の sw_rotate (UserDemo は 90° 回転で運用) が重い | フレームレート低下 | まず横持ちネイティブで試す。回転が必要なら PPA 利用 or 縦 UI レイアウトに変更 |
| ui_task と esp-hosted/SDIO の Core1 競合 | WiFi スループット低下 | ui_task は低優先度・16ms 周期。問題が出たら優先度/コア再配置 |
| フォント追加でフラッシュ肥大 | ビルド不能 | factory 6MB 化 (§5)。さらに必要なら esp_mmap_assets でフォントを別パーティション化 (dazo 方式) |

## 8. 未決事項 (実装時に決める)

- 画面の向き: 横 (1280x720) を第一候補。パネルがネイティブ縦なら回転コストを Phase 0 で実測してから確定
- バックライト制御・スクリーンセーバ (IO エキスパンダ P4? UserDemo 参照)
- StatusBar と Console/Canvas の画面分割比率
- mooncake / smooth_ui_toolkit の固定コミット (vendoring 時に記録)

## 9. Phase 0 実装メモ (2026-06-10 完了)

実装は計画どおり components/ui_tab5 (C++, `CONFIG_MQJS_TAB5_UI`) に封印。
ハマりどころが 2 つあった:

1. **このリポジトリの Tab5 は ST7123 パネル個体** (新しめのロット)。
   UserDemo と同じ方法でタッチコントローラから変種判定する:
   GT911 応答 → ILI9881C、0x55 応答 → fw レジスタ 0x0000 が 1 なら
   ST7121 / 3 なら ST7123。3 変種とも実装済み (ILI9881C/GT911 個体は
   未実機検証)。ST7123/ST7121 ドライバは UserDemo から vendoring
   (`components/ui_tab5/vendor/`, Apache-2.0)。パネル init テーブルは
   `ili9881_init_data.inc` / `st7123_init_data.inc` (M5Stack MIT)。
   **Phase 3 への影響: タッチも GT911 ではなく ST7123 (I2C 0x55)。**
   esp_lcd_touch_gt911 ではなく ST7123 タッチドライバが必要
   (UserDemo の bsp_touch_new 参照)。
2. **esp-hosted 2.x は pre-scheduler のコンストラクタで全トランス
   ポートを初期化する** (`port_esp_hosted_host_init.c` の
   `__attribute__((constructor))`)。その時点のヒープは PSRAM 登録前・
   内部 DMA RAM ~110KB しかなく、SDIO mempool 2 本で ~90KB 食う。
   UI の .bss (~50KB) を足しただけで枯渇し
   `assert failed: sdio_mempool_create (buf_mp_g)` のブートループに
   なった。対策: ui_tab5 が `-Wl,--wrap=esp_hosted_init` でコンスト
   ラクタ時の呼び出しを no-op 化し、wifi.c が起動後に本物を呼ぶ
   (esp_hosted_init は冪等)。C6 の電源ゲート (board_tab5_power_init)
   より後に初期化される副次効果もあり、こちらの方が本来正しい順序。

その他: IDF 6 で esp_lcd の DPI 設定が `pixel_format` →
`in_color_format`/`out_color_format` に、dma2d がフラグ →
`esp_lcd_dpi_panel_enable_dma2d()` に変わった以外、LVGL9 +
esp_lvgl_port 2.8 + esp_lcd_ili9881c 1.1 は IDF 6.0.1 でそのまま通った
(§7 の最大リスクは杞憂だった)。jp_font は StackChan-dazo で生成済みの
font_noto_jp_20_4.c (サイズ 20 / bpp4 / 約 1.5MB, OFL) をそのまま
コミット。パーティション拡張 + erase-flash 済み (永続タスクと NVS は
消えた。WiFi は sdkconfig 由来なので無傷)。画面は横持ちではなく
**ネイティブ縦 720x1280 のまま** (回転は Phase 1 のレイアウト時に判断、
§8 のとおり)。

## 9b. Phase 1 実装メモ (2026-06-10 完了)

計画どおり 2 コミットに分割:
part 1 (`7e7ecb9`) = print sink / 状態フィード / vendoring、
part 2 = ui_tab5.cpp の画面実装。設計からの逸脱なし。要点:

- **画面の向きはネイティブ縦 720x1280 で確定** (§8 の未決事項)。
  回転コストゼロで、コンソールには縦が向いている。esp_lvgl_port が
  起動時に swap_xy 非対応エラーを 1 行出すが無害。
- レイアウト: ステータスバー 88px (上段 44px = WiFi/MQTT ドット +
  タスク名、下段 = last_event)、残り全面がコンソール。
- mooncake は計画どおり抑制的利用: StatusBar (UIAbility, extension
  manager 側) + ConsoleApp (AppAbility)。`Mooncake::update()` は
  16ms 周期の lv_timer から呼ぶので、全 LVGL 操作が esp_lvgl_port の
  ロック内で完結する。
- smooth_ui_toolkit は AnimateValue を 1 箇所 (新イベント受信時の
  ハイライトのスプリング減衰) のみ。デフォルトの chrono ティックで
  そのまま動く。
- producer 側の不変条件: ui_tab5_log は 20ms タイムアウト付き mutex +
  memcpy のみ (取れなければ行を捨てる)。js_task が UI に待たされる
  経路は存在しない。consumer はフレーム毎に最大 16 行をコピーして
  ロック外で描画。リングに 200 行超溜まったら古い方へ追い付く。
- コンソールは行毎 lv_label の flex column (上限 200 子、古い行から
  削除)。最下部 ±24px にいるときだけ追従スクロール。
- 実機 E2E 確認済み: mqjs_push.py で署名付き push → status トピックに
  "accepted"、print 出力 (日本語込み) がシリアルに tee され、画面側
  リングにも流れる (examples/ui_console_test.js)。

## 9c. Phase 2 実装メモ (2026-06-10 完了)

設計 (§3) からの差分・確定事項:

- **キュー深さは 64 → 128** (16B/件)。静的シーン (時計の文字盤 60 本など)
  を一括描画すると 1 フレームのドレイン間隔の間に 64 を超えるため。
  それでも溢れる場合の作法は ui_demo.js のとおり `delay()` で小分けに
  流す。ドロップはステータスバーに `drop N` (赤) で出る。
- **論理解像度は 720x1192** (1280 − ステータスバー 88px)。`ui.size()`
  は js_task 起動前に確定しているので常に有効。画面なし (Stamp /
  パネル初期化失敗) では `[0, 0]` を返し、描画系は全て silent no-op
  — 同じスクリプトが両機体で走る。
- **CanvasApp の表示切替**: 起動時は隠れていて、最初の ui.* コマンドで
  コンソールの上に現れる。**別タスクへの切替を状態フィードの世代で検知
  したらクリアして隠す** (print 専用タスクに戻ったときコンソールが
  復帰する)。タッチでの手動切替は Phase 3/4。
- 描画は RGB565 バッファ (PSRAM, 1.68MB) へ自前プリミティブ
  (rect/line/pixel は直書き、Bresenham)。TEXT のみ LVGL の
  canvas layer (`lv_canvas_init_layer` → `lv_draw_label` →
  `finish_layer`) で jp_font レンダリング。invalidate はドレイン
  1 バッチにつき 1 回。
- TEXT 文字列の所有権: js 側で heap コピー → キュー投函成功で UI 側が
  free、失敗 (満杯/画面なし) は投函側が即 free。
- ヘッダ再生成は WSL で実施 (gen/ = -m32, gen_pc/ = -m64、手順は
  README)。PC スモーク: run_pc で ui.* がスタブ print されること、
  ui_demo.js が rc=0 で走ることを確認済み。
- 実機 E2E: ui_demo.js を署名 push → status "accepted"、例外なし。
  (時計+サイン波の見た目の最終確認はユーザー目視)

## 9d. Phase 3 実装メモ (2026-06-10)

設計 (§3 タッチ) からの差分・確定事項:

- このリポジトリの Tab5 はタッチも ST7123 (§9 のとおり)。ドライバは
  レジストリ部品 **espressif/esp_lcd_touch_st7123**。GT911 個体も
  esp_lcd_touch_gt911 で実装済み (未実機検証)。両者とも 16bit レジスタ
  アドレス・I2C 0x55/0x5D(0x14)、INT=GPIO23、RST はパネルと共通の
  IO エキスパンダ線 (detect で解放済み)。
- **タッチ用 I2C バスは port 1 で常時保持** (SDA31/SCL32)。port 0 は
  JS の `i2c.setup(0, ...)` 用に空けたまま。注意: JS が port 0 で
  ピン 31/32 を claim するとタッチが死ぬ (GPIO マトリクスを奪う)。
- **ポーリングは esp_lvgl_port に任せた** (lvgl_port_add_touch)。これで
  LVGL のジェスチャが生きて Phase 1 で約束したコンソールのフリック
  スクロールが本当に動くようになった。JS への投函は 16ms の mooncake
  タイマー内で indev の状態 (lv_indev_get_state/point) を観測して
  down/move/up 遷移を `mqjs_post_touch()` へ — 追加の I2C トラフィック
  ゼロ、レート上限は実質 60Hz。
- 座標は **キャンバス座標系** (ステータスバー分 y-88、負は 0 に
  クランプ)。`ui.onTouch(fn)` → `fn(x, y, kind)`、kind 0=down 1=move
  2=up。onTouch 登録はイベントループを生かし続ける (タイマー無しの
  純タッチタスクが書ける)。再登録は置き換え。
- ui_tab5 → mqjs は extern 宣言 1 本 (mqjs → ui_tab5 が既にあるため
  REQUIRES だと循環。wifi.c の esp_hosted_init と同じ流儀)。
- E2E: touch_demo.js (お絵かき + クリアボタン) を署名 push →
  "accepted"。PC では onTouch は登録のみ (発火しないスタブ)。

## 9e. Phase 4a 実装メモ (2026-06-10)

ターミナルエミュレータ (Phase 4 のユーザー目標) の布石 3 点:

- **`ui.textSize(str)` は同期クエリ** (キュー経由ではない)。js_task から
  `lv_text_get_size` を直接呼ぶが、fmt_txt フォントのグリフ dsc 参照は
  const テーブルの読みだけ (LVGL 9 にキャッシュなし、ビットマップ
  デコードまで到達しない) なので LVGL タスクと競合しない。改行を含むと
  複数行サイズになる。画面なしは `[0,0]` (ui.size と同じ作法)、PC は
  近似スタブ (半角 10px/全角 20px/行高 25px)。
- **オンスクリーンキーボードは lv_keyboard** をそのまま画面下 400px に
  オーバーレイ (4 段 × 100px)。デフォルトの VALUE_CHANGED ハンドラは
  textarea 前提でモード切替がこちらのコールバックより先にマップを
  差し替えてしまう (切替後のマップでボタンテキストを読むと誤キーを
  拾う) ため、`lv_obj_remove_event_cb` で外して自前ハンドラに置換。
  モード切替 (abc/ABC/1#) は `lv_keyboard_set_mode` で再実装。
  ×/⌨ キーは JS に通知せず非表示にするだけ (kbd_demo はタップで再表示)。
- **キーは EV_KEY (8 バイト UTF-8 値型) で既存キューへ**。
  `mqjs_post_key()` は touch と同型 (ハンドラ未登録なら投函しない、
  満杯なら捨てる)。JS への引数は文字列 1 個: 通常キーはそのまま、
  Enter/OK = "\n"、BS = "\b"、←/→ = "\x1b[D"/"\x1b[C" (ターミナルの
  カーソルシーケンスをそのまま流せるようにする意図)。`ui.onKey` 登録は
  onTouch 同様イベントループを生かし続ける。
- ui.keyboard は UI_CMD_KEYBOARD として描画キューに相乗り (描画 op では
  ないので CanvasApp を unhide しない)。タスク切替検知でキーボードも
  自動的に閉じる。ヘッダ再生成 (gen/ + gen_pc/) は WSL で実施済み。

## 10. 次セッションの着手手順 (Phase 0 当時のメモ)

1. `components/ui_tab5` 雛形 + Kconfig (`MQJS_TAB5_UI`) + 空登録の確認 (Stamp ビルドが無傷であること)
2. ui_tab5/idf_component.yml に lvgl / esp_lvgl_port / esp_lcd_ili9881c / esp_lcd_touch_gt911 を追加 → Tab5 ビルドで取得・コンパイル確認 (**ここが IDF 6 関門**)
3. UserDemo の BSP 初期化 (DSI パネル・LDO・バックライト) を参考に最小の表示初期化を書く
4. partitions.csv 拡張 + erase-flash + Hello World 表示
5. jp_font 生成・コミット → 日本語ラベル表示
6. 完了したらこの文書の Phase 0 を ✅ に更新してコミット
