# esp32p4-mqjs — Stamp-P4 (ESP32-P4) で mquickjs を動かす最小プラットフォーム

JS タスクを mquickjs (MicroQuickJS) で実行する ESP-IDF プロジェクト。
最初のマイルストーン「JS から Lチカ + ボタン割り込み」を実装した状態です。

## JS から見える API

| API | 説明 |
|---|---|
| `print(...)` / `console.log(...)` | UART/USB-CDC コンソールへ出力 |
| `gpio.setMode(pin, gpio.IN \| gpio.OUT \| gpio.IN_PULLUP \| gpio.IN_PULLDOWN)` | ピンモード設定 |
| `gpio.write(pin, level)` / `gpio.read(pin)` | 同期 I/O |
| `gpio.onChange(pin, fn)` | 両エッジ割り込み。`fn(level)` がイベントループから呼ばれる |
| `setTimeout(fn, ms)` / `setInterval(fn, ms)` | 戻り値の id を `clearTimeout/clearInterval(id)` へ |
| `delay(ms)` | ブロッキング待ち (vTaskDelay)。短時間専用 |
| `Date` / `Date.now()` / `performance.now()` | 後者は起動からの単調時間 (ms) |

これに加えて mquickjs 標準の String / Array / Math / JSON / RegExp /
TypedArray 等が使えます (ES5 サブセット・stricter mode。Promise/async は無し)。

## ディレクトリ構成

```
components/mqjs/
  mquickjs/         … エンジン本体 (bellard/mquickjs, MIT) を vendoring
  device_stdlib.c   … stdlib 定義 (mqjs_stdlib.c 派生)。★API を増やすときはここ
  mqjs_runtime.c    … バインディング実装 + イベントループ + ウォッチドッグ
  mqjs_runtime.h    … コンポーネント公開 API
  tools/test_pc.c   … PC スモークテスト (実機不要)
main/app_main.c     … デモ: PSRAM に 256KB のコンテキストを作り JS を実行
```

## ビルド (実機)

```bash
idf.py set-target esp32p4
idf.py build flash monitor
```

ビルド中に device_stdlib.c が **ホスト側 gcc** でコンパイルされ、
`mquickjs_atom.h` / `device_stdlib.h` (ROM 化 stdlib) が `-m32`
(P4 = 32bit RISC-V) で自動生成されます。生成時の
`Too many properties, consider increasing ATOM_ALIGN` 警告は無害です
(グローバルのハッシュサイズが上限で頭打ちになるだけ)。

### チップリビジョン (重要)

M5Stamp-P4 のチップは **rev v1.3**。一方 ESP-IDF v6.0 のデフォルトは
最小リビジョン v3.01 (ECO5) なので、そのままビルドすると flash 時に
`requires chip revision in range [v3.1 - v3.99]` で蹴られる。
sdkconfig.defaults で以下を固定済み (消さないこと):

```
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```

### Windows での注意

- プロジェクトパスに非 ASCII 文字 (日本語ディレクトリ名等) が
  含まれると、コンパイルは通るが **binutils (objdump) がビルド
  成果物を開けず ldgen で失敗する**。ビルドディレクトリだけ `-B` で
  ASCII パスに逃がせば回避できる:

  ```powershell
  . $env:IDF_PATH\export.ps1
  idf.py -B C:\esp-build\esp32p4-mqjs -p <PORT> build flash monitor
  ```

  以降の flash / monitor も毎回同じ `-B` を付けること。
- Stamp-P4 は内蔵 USB-Serial/JTAG (VID 303A / PID 1001) として
  COM ポートに見える。C6 アドオンは SDIO 接続のコプロセッサなので
  PC からは P4 だけが見える。
- `idf.py monitor` は TTY 必須。リダイレクトされたシェルから
  ログだけ取りたい場合は SerialPort (115200) で COM を開き、
  RTS を ON→OFF とパルスしてリセット後に読み出せばよい。
- ROM 化 stdlib のヘッダは Windows では生成せず、コミット済みの
  `components/mqjs/gen/` のものを使う (CMakeLists が自動で切替)。
  **device_stdlib.c を変更したら Linux/WSL でヘッダを再生成して
  コミットし直すこと。**

## PC でのテスト (実機不要・検証済み)

```bash
cd components/mqjs
gcc -O2 -I mquickjs -o /tmp/stdlib_tool device_stdlib.c mquickjs/mquickjs_build.c
mkdir -p gen_pc
/tmp/stdlib_tool -a -m64 > gen_pc/mquickjs_atom.h   # PC は 64bit
/tmp/stdlib_tool -m64    > gen_pc/device_stdlib.h
gcc -O2 -I. -Igen_pc -Imquickjs -o /tmp/test_pc tools/test_pc.c \
    mqjs_runtime.c mquickjs/mquickjs.c mquickjs/cutils.c \
    mquickjs/dtoa.c mquickjs/libm.c -lm
/tmp/test_pc
```

確認済みの動作:
- setInterval ×4 + clearInterval + setTimeout が実時間どおり発火し、
  タイマー消化後にイベントループが自然終了 (606ms)
- `while(true){}` の暴走スクリプトが JS_SetInterruptHandler 製の
  ウォッチドッグにより 5 秒で `InternalError: interrupted` で中断され、
  ホストは生存 (rc=-1)

## 設計メモ (重要)

- **コールバック保持**: コンパクション GC がオブジェクトを動かすため、
  生の JSValue を C に保持してはいけない。`JS_AddGCRef` で永続 GC 参照を
  取り、`ref.val` を使う (mqjs.c 本家の setTimeout と同じパターン)。
- **ISR から JS を呼ばない**: GPIO ISR は FreeRTOS キューへ投函するだけ。
  コンテキストを所有するタスク (イベントループ) だけが JS に触れる。
- **呼び出し規約**: `JS_StackCheck(ctx, nargs+2)` → 引数を push →
  関数を push → this を push → `JS_Call(ctx, nargs)`。
- **ウォッチドッグ**: JS_Eval / コールバック実行前に 5 秒のデッドラインを
  セット。インタプリタが定期的に interrupt handler を呼ぶので、
  ホールトしたスクリプトを安全に中断できる。
- **コンテキスト使い捨て**: stdlib は ROM 常駐なので JS_NewContext は
  非常に軽い。タスク毎に新品のコンテキストで実行し、状態汚染を防ぐ。

## API を追加する手順

1. `device_stdlib.c` の該当テーブルにエントリ追加
   (例: `JS_CFUNC_DEF("readReg", 3, js_i2c_readReg)` を持つ `i2c` オブジェクト)
2. `mqjs_runtime.c` に同名の C 関数を実装 (ESP-IDF ドライバを呼ぶ)
3. リビルド (ヘッダは自動再生成)

µs 精度が要るプロトコル (DHT22, WS2812 等) は JS に出さず、
RMT などのペリフェラルを使う高レベル関数として公開すること。

## 次のステップ

- I2C バインディング (`i2c.readReg` / `i2c.writeReg`) — 同期で十分
- Stamp-AddOn C6 + esp_wifi_remote でネットワーク導入
- LittleFS パーティション + タスクのダウンロード/署名検証 (Ed25519)
- `mqjs -o -m32` で事前コンパイルしたバイトコード配信 (信頼済みソース限定)

## ライセンス

mquickjs は MIT License (Fabrice Bellard / Charlie Gordon)。
`components/mqjs/mquickjs/LICENSE` を参照。
