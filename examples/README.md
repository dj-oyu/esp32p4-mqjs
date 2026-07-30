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
| 日本語入力 (SKK) をアプリに載せる | `ssh_vt.js`, `skk_test.js` |
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
- `ssh_vt.js`: 複数セッション対応の SSH ターミナル。日本語入力は
  プラットフォームの持ち物で、アプリが書くのは `ui.ime(1)` の opt-in と
  カーソルが動いたときの `ui.caret(x, y, h)` だけ
  (`docs/keyboard-ime-unification.md` §7)。制御バーの「あ」ボタンのトグルも、
  変換中の preedit と候補のフロートも、モード表示 (「あ」キーの面) も C 側が
  持ち、**確定した日本語は `ui.onKey` に普通の文字列として届く** ので、
  アプリは他の打鍵と同じく `ssh.write()` へ流すだけ。端末フォントは
  JIS 第1水準まで入っている (S4) ので、確定した日本語はそのままグリッドに出る。
  辞書は既定でファーム埋め込みの SKK-JISYO.M だが、`jisyo` パーティションへ
  大きいものを焼けば**アプリを変えずに**そちらが使われる (ルート README 3.5)。
  変換の学習は無い (2026-07-30 に削除)。候補は常に辞書順。
  ⚠️ **日本語入力は実機でしか試せない**。IME のフックは `mqjs_post_key()` の
  `#ifdef ESP_PLATFORM` の中にあり、run_pc では `ui.onKey` がそもそも発火
  しないので、ホストでは 1 打鍵も IME を通らない。ホストで見られるのは
  エンジンとセッション方針のほうで、`tools/tests/ime_diff.sh` (golden との
  回帰) と `tools/tests/test_ime_core.c` (▽ 中の矢印/ESC の飲み込み、
  トークンの順序) がそれを見る。かつて `tools/ssh_vt_imetest.sh` が
  ssh_vt のクロージャへ台本を注入して見張っていた「IME フックが TOKSEQ 展開
  より前にあること」は、`ime_core` では構造的に表現不可能になったので、
  この 2 つに引き継いで harness は廃止した
- `ssh_vt2.js`: 同じ SSH ターミナルの**ネイティブ `term.*` 版**
  (`docs/term-design.md` §11.4)。VT パーサ・grid・SGR・カーソル・
  スクロールバックは C の `term_core` にあり、受信バイトは
  `term.pipe(tid, sshId)` で **JS を一度も通らない**。アプリに残るのは
  タブ (= `term.show` の付け替え)、接続 UX、長押し選択 →
  `clipboard`(画面は `term.snapshot` で読む)、キー経路
  `ui.onKey → ssh.write`、回転時の `term.resize` + `ssh.resize`。
  `ui.caret` を呼ぶ場所も無い (§10.2: C の term が caret シンクへ直接
  push する)。ネイティブ path が実機で通るまで `ssh_vt.js` が現役の
  落とし所なので**両方入れておける** (アプリ名が別なので vault の
  パスワード/ホスト鍵は入れ直し、`store` のホスト一覧は共有)。
  `SELFTEST = true` に書き換えて run_pc で走らせると、`term.feed` した
  画面に対する選択/幅 2 文字の列展開・タブの割り付け・桁行の丸め・
  記録インジケータと `term.record` の契約を自己診断する。
  **アクティブなタブをもう一度タップするとタブメニュー**が出る
  (非アクティブのタップは今まで通り切り替え)。メニューには**記録モード**の
  入/切と「今の画面を記録する」がある — `docs/term-design.md` §4.4 の
  2026-07-30 例外で、**既定は切・セッション限り・どこにも記憶しない**。
  入れているあいだはタブが赤くなり `●` と `●REC` が出る。録ったものは
  `python3 tools/bb_pull.py <host> <topic> live --session` で PC から
  読める(= この機能の目的)。**秘密を表示するセッションでは入れない**:
  署名鍵の持ち主が平文で取り出せ、電源を切るまでリセットを跨いで残る
- `skk_test.js`: 日本語入力の試験台。入力は ssh_vt と同じく `ui.ime(1)` +
  `ui.caret()` で、**計測は `ui.imeStats()`** — 辞書の引き方 (lookups/probes)、
  変換の µs、そして**打鍵がコマンドキューで待った時間 (hop)** を C 側が数えて
  返す。私設セッションを開いて JS で時間を挟んでも、それは実際の打鍵が通る道
  ではない (`docs/keyboard-ime-unification.md` §7.1)。桁のモデル (`ui.cellWidth`
  と CONT セル) と `ui.cells` / `ui.text` の描き分けの見本でもある
- `reading.js`: NVS 永続化、一覧 UI、ISBN 入力。タイトルと著者は
  `s.field(名前, "ja")` と書くだけで日本語入力になり、ISBN やページ数の欄は
  既定のまま = ASCII のみ。**どの欄で日本語を許すかは C 側が強制する**ので、
  アプリの書き忘れで ISBN にかなが混ざることはない
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

## 画面の回転 (横向き)

Tab5 はキーボードドックを着けると横向き 1280x720 になります。**向きとキーボードの
予約高さは一緒に変わります** — ドックに物理キーがあるのでオンスクリーンキーボードは
出さず、`ui.keyboard(2)` の予約は 480px から制御バーぶんの 80px 程度まで縮みます。

ステータスバーやウィジェット画面は C 側が付いてきますが、キャンバスは寸法を差し替えた
うえで**アプリが描き直すまで隠されます**。桁数・行数・当たり判定を引き直して描くのは
アプリの仕事で、何もしないとコンソールが見えたままになります。

回転時、C は foreground のアプリ**だけ**に `"\x00rotate"` キーを送ります。背面にいる間の
回転は届かず、取りこぼした一回がそのままズレっぱなしになるので、トークンだけに頼らず
既存の周期タイマーで `ui.size()` を突き合わせてください (副作用の無いクエリです)。
`ui.keyboard()` も mode を負数にすると表示を変えない問い合わせになりますが、戻り値 0 は
「キーボードが無い」ではなく「まだ答えられない」(キャンバス未生成) のことがあり、
信じると本文がキーの裏に潜り込みます。ただし `ui.keyboard(-1)` (mode 1) では
ドック装着中の 0 が正解なので、`> 0` ではなく `ui.size()[0]` で「答えられるか」を
見分けてください (`circuit.js` の `curKb()`)。

背面にいる間は描画コマンドが捨てられます。ポーリングだけだと裏で回転を拾って寸法だけ
新しくなり、前面に戻っても突き合わせが一致して二度と描き直さないので、
`sys.onForeground()` からも `relayout()` を呼んでください。

```js
function relayout() {
    var s = ui.size();
    W = s[0] || W;                    /* [0,0] = 画面なし: 前の値を保つ */
    H = s[1] || H;
    var kb = ui.keyboard(-2);
    if (kb > 0) KB_H = kb;            /* 0 は「まだ答えられない」 */
    COLS = (W / CELL_W) | 0;
    ROWS = ((H - KB_H) / CELL_H) | 0;
    redrawAll();                      /* 描くまでキャンバスは隠れたまま */
}

ui.onKey(function (k) {
    /* レイアウト変更は他のガードより先に置く。下の return に飲まれると直らない */
    if (k.charCodeAt(0) === 0 && k.slice(1) === "rotate") { relayout(); return; }
    /* ...モード判定やセッションガード... */
});

setInterval(function () {
    var s = ui.size();
    if (s[0] !== W || s[1] !== H) relayout();
}, 500);
```

実例は `circuit.js` (横向きで表を右の帯へ移す)、`ssh_vt.js`、`skk_test.js` にあります。

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
