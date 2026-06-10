# examples — mquickjs タスクのサンプル集

ビルド時に 1 本選んでファームウェアへ埋め込みます:

```bash
idf.py -DMQJS_SCRIPT=life.js build flash
```

(`-B <ASCII パス>` が必要な環境では他のコマンド同様に付ける。
 デフォルトは `blink_button.js`)

| スクリプト | 配線 | 内容 |
|---|---|---|
| `blink_button.js` | LED G2 / ボタン G5 | Lチカ + ボタンエッジ表示 (最初のマイルストーン) |
| `morse.js` | LED G2 | メッセージをモールス信号で点滅。setTimeout チェーン 1 本で再生 |
| `life.js` | 不要 | ライフゲームをシリアルコンソールに ANSI アニメーション表示 |
| `mandelbrot.js` | 不要 | マンデルブロ集合へ 40 フレームの ASCII ズーム。終了後イベントループが自然終了 → タスク再起動でリプレイ |
| `reaction.js` | LED G2 / ボタン G5 | 反射神経ゲーム。デバウンス・clearTimeout・ステートマシンの実例 |
| `bench.js` | 不要 | マイクロベンチマーク 7 種 (再帰 / 篩 / 文字列 / Array 高階関数 / libm / JSON / RegExp) |
| `mqtt_demo.js` | 不要 (要 WiFi) | test.mosquitto.org へ接続し publish→subscribe ループバック。mqtt.* API の実例 |
| `i2c_scan.js` | I2C デバイス (Tab5 は内蔵) | バススキャン + BMI270 の chip_id 読み出し。i2c.* API の実例 |
| `ui_console_test.js` | 不要 (Tab5 画面で確認) | 日本語+長行を 1 秒毎に print。Tab5 オンデバイスコンソールの動作確認用 |

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
