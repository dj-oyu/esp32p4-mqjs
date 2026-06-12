# mqjs アプリのサンプル

このディレクトリには、ESP32-P4 上の mquickjs ランタイムで動くアプリと、
API の最小例があります。最初は `blink_button.js`、画面付きの Tab5 では
`settings_demo.js`、マルチアプリ動作は `p4_bg_app.js` から読むと全体像を
つかみやすくなります。

## 実行方法

### ビルドへ埋め込む

```bash
idf.py -DMQJS_SCRIPT=blink_button.js build flash
```

指定しない場合も `blink_button.js` が使われます。

### 開発中のアプリを push する

ルート README の手順で署名鍵と MQTT 接続を設定し、`tools/mqjs_push.py` から
dev topic へ送ります。dev topic のアプリは実行中の dev app を置き換えます。

先頭に `// @app` があるソースは、dev app の置換ではなくインストール対象として
扱われます。ストアへ掲載する場合は `--shelf` を使います。

```js
// @app hello
// @title Hello
// @icon H
// @desc 最小の mqjs アプリ
// @perm ui

sys.setAppName("hello");
print("hello");
```

## 目的別の入口

| やりたいこと | 最初に読むサンプル |
|---|---|
| GPIO とタイマーを使う | `blink_button.js`, `morse.js`, `reaction.js` |
| MQTT を publish / subscribe する | `mqtt_demo.js`, `clip_mirror.js` |
| フォーム、設定画面、リストを作る | `settings_demo.js`, `i2c_scan.js` |
| キャンバスへ高速描画する | `cells_test.js`, `ui_demo.js`, `touch_demo.js` |
| バックグラウンドで動くサービスを作る | `p4_bg_app.js`, `clip_mirror.js` |
| 永続データを保存する | `settings_demo.js`, `reading.js`, `circuit.js` |
| カメラで ISBN を読む | `cam_demo.js`, `reading.js` |
| SSH クライアントを作る | `ssh_vt.js` |
| CPU 負荷や長い処理の分割を学ぶ | `bench.js`, `mandelbrot.js` |

## サンプル一覧

### 入門とデバイス API

- `blink_button.js`: GPIO、タイマー、ボタン、簡単なウィジェット
- `morse.js`: `setTimeout` チェーンで処理を分割
- `reaction.js`: 小さな状態機械と入力処理
- `i2c_scan.js`: I2C スキャンと動的な結果一覧
- `mqtt_demo.js`: MQTT の接続、購読、publish
- `cam_demo.js`: カメラのコードスキャン

### UI

- `settings_demo.js`: screen、field、button、list、toggle、slider
- `ui_demo.js`: キャンバス描画と設定画面の併用
- `touch_demo.js`: タッチ入力とキャンバス描画
- `kbd_demo.js`: オンスクリーンキーボードとキー入力
- `cells_test.js`: 等幅セル描画とスクロールの最小例
- `ui_console_test.js`: UTF-8、ANSI 色、長いログの表示確認

### アプリとサービス

- `p4_bg_app.js`: foreground / background と通知
- `clip_mirror.js`: クリップボードを MQTT へ日和見同期する常駐サービス
- `ssh_vt.js`: 複数セッション対応の SSH ターミナル
- `reading.js`: NVS 永続化、一覧 UI、ISBN 入力
- `circuit.js`: キャンバス UI、式評価、永続化

### アルゴリズムと描画

- `life.js`: ANSI コンソール上のライフゲーム
- `mandelbrot.js`: 重い処理を小分けにする描画例
- `bench.js`: mquickjs の簡易ベンチマーク

## UI の選び方

mqjs アプリでは、用途に応じて 3 種類の作り方を使い分けます。

1. **ウィジェット**: 設定、フォーム、一覧には `ui.screen()` を使う。
2. **キャンバス**: 端末、グラフ、ゲームには `ui.cells()`、`ui.rect()`、
   `ui.text()` などを使う。
3. **ハイブリッド**: 高頻度描画はキャンバス、設定だけウィジェットにする。

画面はアプリが background へ移ると破棄されます。表示用データは JS 側へ保持し、
foreground 復帰時に再構築してください。

```js
function build() {
    var s = ui.screen("Counter");
    s.label("count = " + count);
}

var count = 0;
sys.onForeground(build);
build();
```

Tab5 と Stamp-P4 の両方で動かすアプリは、画面サイズを確認して UI を省略できます。

```js
var size = ui.size();
if (size[0] !== 0) {
    ui.text(8, 8, "hello", 0xffffff);
}
```

## バックグラウンドサービス

バックグラウンド中もタイマー、MQTT、SSH、clipboard のイベントは動き続けます。
一方、UI と入力は foreground app だけが所有します。

クリップボード同期のようなサービスは、次のように UI を持たず、イベント登録に
よって生存できます。

```js
sys.setAppName("service");
clipboard.onChange(function (data, type) {
    print("clipboard:", type, data.length);
});
```

実行枠 (worker) は 4 本固定です。空きが無いときは背面の evictable な
アプリが LRU で自動停止して枠を譲ります。停止直前には `sys.onStop(reason)`
が呼ばれるので、状態は `store.set()` へ保存して次回起動時に復元してください。
詳細は [`docs/app-manager-migration.md`](../docs/app-manager-migration.md) を参照してください。

## mquickjs で書くときの注意

mquickjs は ES5 ベースの stricter mode です。

- `var` と `function` を使う。`let`、`const`、`class`、アロー関数、
  Promise、`async` は使えない。
- 配列の末尾を越えて要素を代入できない。必要なら `new Array(n)` を使う。
- 一般的な新しい標準 API の一部はない。処理を書く前に既存サンプルを確認する。
- `delay(ms)` は全アプリを止めるため、短い初期化以外では使わない。
- 1 回の JS 実行は 5 秒以内に終える。重い処理は `setTimeout` で分割する。

## 主な上限

| リソース | 上限 |
|---|---:|
| JS メモリ | 256 KiB / app |
| タイマー | 16 / app |
| GPIO ハンドラ | 8 / app |
| MQTT 購読 | 8 / app |
| ウィジェット callback | 48 / app |
| SSH セッション | 全 app 合計 3 |
| 1 回の JS 実行 | 5 秒 |

API とアプリライフサイクルの詳細は、ルートの
[`README.md`](../README.md)、[`docs/widget-framework-design.md`](../docs/widget-framework-design.md)、
[`docs/launcher-multiapp-design.md`](../docs/launcher-multiapp-design.md) を参照してください。
