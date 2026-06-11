# examples — mquickjs タスクのサンプル集

ビルド時に 1 本選んでファームウェアへ埋め込みます:

```bash
idf.py -DMQJS_SCRIPT=life.js build flash
```

(`-B <ASCII パス>` が必要な環境では他のコマンド同様に付ける。
 デフォルトは `blink_button.js`)

## UI の 3 形態と標準イディオム (W1 ウィジェットフレームワーク)

サンプルは UI の使い方で 3 形態に分かれる
(docs/widget-framework-design.md §1):

1. **ウィジェットモード** — 設定・フォーム・リストは `ui.screen()` で作る。
   スクロール・フォーカス・オンスクリーンキーボードは LVGL 任せの標準路。
   例: `settings_demo.js` `i2c_scan.js` `mqtt_demo.js`
2. **キャンバスモード (性能バイパス)** — 高頻度描画は `ui.cells / rect /
   line / scroll` でコマンドキュー直行。ウィジェットを経由しないので
   端末・グラフ・お絵かきはこちら。例: `ssh_vt.js` の描画部、`cells_test.js`
3. **ハイブリッド** — ホットパスはキャンバスのまま、設定・フォームだけ
   ウィジェット画面を重ねる。例: `touch_demo.js` `ui_demo.js`
   `ssh_vt.js`/`ssh_term.js` (接続フォーム)

どのサンプルも共通で使う 3 つのイディオム:

```js
// (1) 画面なし機 (Stamp) ではウィジェットを作らない。コールバックを
//     登録するとイベントループが終了しなくなる (タスクの自動再実行が
//     止まる) ので、ヘッドレス動作を保つスクリプトは必ずこのゲートを通す。
var HAS_UI = ui.size()[0] !== 0;

// (2) ウィジェット画面が開いている間はキャンバスの onTouch を遮断する。
//     タッチはウィジェット画面の上でも mqjs に届くため。
var inSettings = false;
ui.onTouch(function (x, y, kind) {
    if (inSettings) return;          // ウィジェットに任せる
    /* ... キャンバスのホットパス ... */
});

// (3) 動的リストは「画面ごと作り直す」。行の個別削除 API は無い。
//     ui.screen() を作り直せばリテインスタック (N=3) が古い画面を回収する。
```

ウィジェット API の使用例は `settings_demo.js`、設計と実測値は
docs/widget-framework-design.md §3/§10 を参照。

| スクリプト | 配線 | UI 形態 | 内容 |
|---|---|---|---|
| `blink_button.js` | LED G2 / ボタン G5 | ウィジェット (任意) | Lチカ + ボタンエッジ表示 (最初のマイルストーン)。Tab5 では点滅トグル/周期スライダー/エッジ表示のパネル付き |
| `morse.js` | LED G2 | ウィジェット (任意) | メッセージをモールス信号で点滅。setTimeout チェーン 1 本で再生。Tab5 では FIELD でメッセージ差し替え |
| `life.js` | 不要 | コンソール | ライフゲームをシリアルコンソールに ANSI アニメーション表示 |
| `mandelbrot.js` | 不要 | コンソール | マンデルブロ集合へ 40 フレームの ASCII ズーム。終了後イベントループが自然終了 → タスク再起動でリプレイ |
| `reaction.js` | LED G2 / ボタン G5 (無くても可) | ウィジェット (任意) | 反射神経ゲーム。デバウンス・clearTimeout・ステートマシン。Tab5 では画面の PRESS ボタンでも遊べる |
| `bench.js` | 不要 | ウィジェット (表示のみ) | マイクロベンチマーク 7 種。Tab5 では結果をラベルに setText (ボタン無し = 自動リピート維持の実例) |
| `mqtt_demo.js` | 不要 (要 WiFi) | ウィジェット | publish→subscribe ループバック + コントロールパネル (状態/最終受信/自動 publish トグル) |
| `i2c_scan.js` | I2C デバイス (Tab5 は内蔵) | ウィジェット | バススキャン結果をリスト表示、行タップで reg0 読み出し、再スキャンは画面作り直し (動的リストの標準パターン) |
| `settings_demo.js` | 不要 (Tab5 画面で操作) | ウィジェット | W1 ウィジェット API 一式のショーケース + ヒープ安定性計測 (navigate/back 周回) |
| `ui_console_test.js` | 不要 (Tab5 画面で確認) | コンソール | コンソール表示試験: 日本語/長行/タブ/記号/**ANSI カラー** (SGR 16 色、行跨ぎの色持続) |
| `ui_demo.js` | 不要 (Tab5 画面で確認) | ハイブリッド | アナログ時計 + サイン波 (キャンバス)。右上 □ で波形設定 (ウィジェット) |
| `touch_demo.js` | 不要 (Tab5 画面で操作) | ハイブリッド | お絵かき (キャンバス直描き)。右上 □ でペン色/太さ/クリアの設定画面 |
| `kbd_demo.js` | 不要 (Tab5 画面で操作) | キャンバス | ui.keyboard + ui.onKey + ui.textSize の行エディタ (端末キーボード経路のサンプル)。Enter で履歴に確定、タップでキーボード再表示 |
| `cells_test.js` | 不要 (Tab5 画面で確認) | キャンバス | ui.cells/ui.scroll (グリフ直接ブリット) の最小スモーク |
| `ssh_term.js` | 要 WiFi + sshd (Tab5 画面で操作) | ハイブリッド | SSH 最小端末 (Phase T1)。接続フォーム (ウィジェット) → 端末 (キャンバス)。認証情報はフォーム入力 |
| `ssh_vt.js` | 要 WiFi + sshd (Tab5 画面で操作) | ハイブリッド | SSH ターミナルエミュレータ (Phase T2/T3)。VT100 パーサ + セルグリッド + 16 色。ls --color / vi / top 対応。接続フォーム入力、切断でフォームに復帰。`SELFTEST=true` で PC パーサ検証 |

`life.js` / `mandelbrot.js` は ANSI エスケープを使うので、対応した
ターミナル (`idf.py monitor`、TeraTerm 等) で見ること。

## スクリプトを書くときのルール (mquickjs サブセット)

エンジンは ES5 ベースの **stricter mode**。普通の JS のつもりで書くと
落ちる代表例:

- **`var` と `function` だけ。** `let` / `const` / `class` / アロー関数 /
  テンプレートリテラル / 分割代入 / デフォルト引数は構文エラー。
  Promise / async も無い。
- **配列に穴を開けられない。** `a[10] = x` は末尾 (`a.length`) を超えると
  TypeError。`new Array(n)` は OK (全要素 undefined で長さ n)。
  穴あき配列リテラル `[1, , 3]` は構文エラー。
- `for of` は **配列のみ**。`for in` は own property のみ列挙。
- `eval` は間接形 `(1, eval)(...)` のみ。
- `new Number(1)` 等の値ボクシングは不可。
- `Date` は `Date.now()` / `new Date(ms)` / `valueOf` だけ
  (`getHours` 等は無い)。壁時計は SNTP 同期するまで無意味。
- `toLowerCase` / `toUpperCase` / RegExp の大文字小文字無視は ASCII のみ。
- プロパティキーは文字列か 31bit 正整数。数値文字列キーの暗黙変換に
  頼らないこと (`obj['0']` と `obj[0]` を混用しない)。
- 無い標準関数に注意: `String.prototype.includes/startsWith/padStart`、
  `Array.prototype.find/includes/fill`、`Object.assign` など。
  あるもの: `replaceAll` / `trimStart` / `trimEnd` / `repeat` /
  `codePointAt`、Array の `map/filter/reduce/sort/...`、TypedArray、
  JSON、RegExp (`s`/`y`/`u` フラグ)、Math ほぼ全部 + `imul/clz32/...`。

## ランタイムの制約 (mqjs_runtime.c)

- **ウォッチドッグ 5 秒**: トップレベル実行も各コールバックも、1 回の
  JS 実行が 5 秒を超えると `InternalError: interrupted` で中断される。
  重い処理はフレーム分割して setTimeout でつなぐ (mandelbrot.js 方式)。
- **タイマーは最大 16 本**、GPIO ハンドラは最大 8 本 (1 ピンにつき 1 個)、
  MQTT 購読は最大 8 本 (トピックフィルタ 95 文字、ペイロード 4KB まで。
  フィルタ重複は TypeError)。
- **mqtt.connect() するとセッションが生きている間イベントループは
  終了しない** (mqtt.disconnect() で破棄すれば終了要因が消える)。
  subscribe コールバックは esp-mqtt タスクからキュー経由で JS イベント
  ループに渡るので、他のコールバックと同じスレッドで直列に走る。
- **時間粒度は 10 ms** (FreeRTOS 100 Hz tick)。`setInterval(fn, 2)` は
  実質 10 ms になる。ソフト PWM のような µs/ms 精度の用途には使えない
  (そういうものは C 側で RMT 等を使って高レベル API として公開する)。
- **イベントループはタイマーも GPIO ハンドラも無くなると終了**し、
  ホスト (app_main) が 1 秒後にまっさらなコンテキストでタスクを
  再実行する。「一度だけ実行して終わる」スクリプトはこの性質で
  自動リピートになる。
- `delay(ms)` はイベントループごとブロックするので短時間専用。
- `print` / `console.log` は引数をスペース区切りで出力し改行を付ける。
  文字列はそのまま出るので ANSI エスケープが使える。
- メモリはタスクあたり 256 KB (PSRAM 上、app_main.c の `JS_MEM_SIZE`)。
  コンパクション GC なので断片化はしない。
