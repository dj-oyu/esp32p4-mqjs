# esp32p4-mqjs — Stamp-P4 (ESP32-P4) で mquickjs を動かす最小プラットフォーム

JS タスクを mquickjs (MicroQuickJS) で実行する ESP-IDF プロジェクト。
「JS から Lチカ + ボタン割り込み」に加え、**MQTT 経由での JS スクリプト配布**
(受信 → NVS 永続化 → 実行中タスクの差し替え) を実装した状態です。

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
main/
  app_main.c        … JS ランナータスク (スクリプトの実行・停止・差し替え)
  net_mqtt.c        … WiFi (esp_wifi_remote) + MQTT クライアント。配布の受け口
  script_store.c    … 受信スクリプトの NVS 永続化 (再起動後も復元)
  Kconfig.projbuild … WiFi SSID / MQTT ブローカー等の設定
tools/mqjs_push.py  … ホスト側の配布ツール (paho-mqtt)
partitions.csv      … NVS を 256KB に拡張したパーティションテーブル
```

## ビルド (実機)

```bash
idf.py set-target esp32p4
idf.py menuconfig   # "mqjs script distribution" で WiFi SSID / ブローカー URI を設定
idf.py build flash monitor
```

ESP32-P4 自体は無線を持たないため、WiFi は **Stamp-AddOn C6 (esp-hosted
スレーブ)** 経由です。`main/idf_component.yml` の `esp_wifi_remote` /
`esp_hosted` が managed component として自動取得されます。
C6 とのトランスポート (SDIO/SPI) とピン割り当ては menuconfig の
**Component config → ESP-Hosted** を実配線に合わせてください。

`MQJS_WIFI_SSID` が空のままならネットワークは無効化され、従来どおり
保存済み/内蔵スクリプトだけが動きます (ビルド・起動は通る)。

ビルド中に device_stdlib.c が **ホスト側 gcc** でコンパイルされ、
`mquickjs_atom.h` / `device_stdlib.h` (ROM 化 stdlib) が `-m32`
(P4 = 32bit RISC-V) で自動生成されます。生成時の
`Too many properties, consider increasing ATOM_ALIGN` 警告は無害です
(グローバルのハッシュサイズが上限で頭打ちになるだけ)。

## MQTT 経由のスクリプト配布

起動時にシリアルへ `device id: aabbccddeeff` (efuse MAC) が出ます。
トピックは `mqjs/<device-id>/...` (プレフィックスは Kconfig で変更可):

| トピック | 向き | 内容 |
|---|---|---|
| `mqjs/<id>/script` | → デバイス | JS ソース本文 (最大 64KB)。受信すると NVS へ保存し、実行中タスクを中断して即差し替え。**空ペイロード**で保存スクリプトを消去 |
| `mqjs/<id>/cmd` | → デバイス | `restart` / `stop` / `clear` |
| `mqjs/<id>/status` | ← デバイス | retained JSON: `{"state":"running","rc":0,"heap":...}`。LWT で `offline` |

retain 付きで publish しておくと、デバイスは再起動・再接続のたびに
ブローカーから最新スクリプトを受け取れます (NVS 保存と二重の保険)。

```bash
# 付属ツール (pip install paho-mqtt)
./tools/mqjs_push.py -b 192.168.1.10 -d aabbccddeeff blink.js
./tools/mqjs_push.py -b 192.168.1.10 -d aabbccddeeff --cmd restart

# mosquitto クライアントでも同じこと
mosquitto_pub -h 192.168.1.10 -t 'mqjs/aabbccddeeff/script' -r -f blink.js
mosquitto_sub -h 192.168.1.10 -t 'mqjs/+/status' -v
```

動作メモ:
- スクリプトは「実行 → タイマー/ハンドラが無くなったら終了 → 1 秒後に再実行」
  のループ。`stop` でアイドル化、`restart` で先頭から再実行。
- 大きいペイロードは esp-mqtt がフラグメント配送するため、
  `net_mqtt.c` で `total_data_len` ベースに再組み立てしてから適用。
- 実行中スクリプトの中断は `mqjs_runtime_stop()` (sticky フラグ)。
  イベントループの脱出に加え、interrupt handler 経由で実行中の JS
  コードも `InternalError: interrupted` で中断できる。
- **署名検証は未実装**。現状はブローカーに publish できる者が任意コードを
  実行できるため、信頼できる閉じたネットワーク + ブローカー認証
  (`MQJS_MQTT_USERNAME`) での運用を前提とすること。Ed25519 署名は次の
  マイルストーン。

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
- 配布スクリプトの署名検証 (Ed25519) — 現状は閉域ネットワーク前提
- 複数タスク (トピック `mqjs/<id>/script/<task-name>` 化 + タスク毎コンテキスト)
- `mqjs -o -m32` で事前コンパイルしたバイトコード配信 (信頼済みソース限定)

## ライセンス

mquickjs は MIT License (Fabrice Bellard / Charlie Gordon)。
`components/mqjs/mquickjs/LICENSE` を参照。
