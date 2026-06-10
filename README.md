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
| `i2c.setup(port, sda, scl[, hz])` | I2C マスター初期化 (port 0/1、デフォルト 400kHz) |
| `i2c.scan(port)` | 0x08〜0x77 を probe してアドレスの配列を返す |
| `i2c.readReg(port, addr, reg, n)` | レジスタ読み (1〜32 バイト)。バイト値の配列を返す |
| `i2c.writeReg(port, addr, reg, b0...)` | レジスタ書き (データ最大 8 バイト可変長) |
| `mqtt.connect("mqtt://host[:port]")` | esp-mqtt クライアント起動 (要 WiFi)。自動再接続あり |
| `mqtt.onConnect(fn)` | ブローカー接続確立ごとに `fn()` |
| `mqtt.subscribe(topic, fn)` | `fn(topic, payload)`。`+`/`#` ワイルドカード可。接続前に呼んでも良い |
| `mqtt.publish(topic, payload[, qos, retain])` | 戻り値は msg_id (qos0 は 0、失敗 -1) |
| `mqtt.connected()` / `mqtt.disconnect()` | 接続状態 (1/0) / セッション破棄 |
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
  tools/run_pc.c    … PC で任意の .js を実行するランナー (実機不要)
examples/           … JS タスクのサンプル集 (examples/README.md 参照)
main/app_main.c     … PSRAM に 256KB のコンテキストを作り JS タスクを実行
```

## ビルド (実機)

```bash
idf.py set-target esp32p4
idf.py build flash monitor
```

> **注意 (EIM 環境)**: EIM のアクティベーションは `ESP_IDF_VERSION=6.0.1` を
> 設定するが、esp_wifi_remote の Kconfig は major.minor (`6.0`) を期待する。
> ビルド・menuconfig の前に必ず `$env:ESP_IDF_VERSION='6.0'` で上書きする
> こと。忘れると WiFi 関連の Kconfig が丸ごと消えて
> `CONFIG_WIFI_RMT_* undeclared` でビルドが落ちる (またはスレーブが
> ESP32-H2 にフォールバックする)。

実行する JS タスクは `examples/` から 1 本選んでビルド時に埋め込む
(デフォルトは `blink_button.js`):

```bash
idf.py -DMQJS_SCRIPT=life.js build flash monitor
```

ライフゲーム / マンデルブロズーム / モールス信号 / 反射神経ゲーム /
ベンチマーク等が入っている。一覧と「mquickjs サブセットでスクリプトを
書くときのルール」は [examples/README.md](examples/README.md) を参照。

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

### M5Stack Tab5 でも動く (同じ ESP32-P4 + C6 構成)

Stamp の sdkconfig を汚さないよう別ビルドディレクトリ + 別 sdkconfig で:

```powershell
idf.py -B build_tab5 "-DSDKCONFIG=sdkconfig.tab5" "-DMQJS_SCRIPT=i2c_scan.js" -p <PORT> build flash
```

- 内蔵 I2C バスは port 0, SDA=G31, SCL=G32 (IMU/RTC/コーデック等がぶら下がる)
- チップリビジョンは Stamp と同じ v1.3 (既存の rev 設定がそのまま効く)
- **USB の DTR/RTS によるリセットが効かない** (ダウンロードモードに
  入るか無反応)。リセットは本体の電源ボタンで行うこと
- **WiFi も動く** (mqtt.* とタスク配信まで実機確認済み)。
  sdkconfig.tab5.defaults (ローカル・要 WiFi 認証情報) に以下を追加:
  ```
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1=12
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1=13
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1=11
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1=10
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D2_4BIT_BUS_SLOT_1=9
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D3_4BIT_BUS_SLOT_1=8
  CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=15
  CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
  CONFIG_MQJS_TAB5_POWER=y
  ```
  ハマりどころ 2 つ:
  1. **C6 は IO エキスパンダ (PI4IOE5V6408 @0x44) の P0 で電源ゲート**
     されている。`CONFIG_MQJS_TAB5_POWER=y` で main/board_tab5.c が
     起動時に通電する (これが無いと sdmmc_card_init が永遠に失敗)
  2. **esp-hosted 2.x はリセット極性設定の意味が 1.x と逆**。M5 公式
     UserDemo (hosted 1.4) の `RESET_ACTIVE_LOW` をそのまま写すと
     GPIO15 が常時 Low = C6 が永久リセットになる。2.x では
     `RESET_ACTIVE_HIGH` (= 通常 High・Low パルスでリセット) が正解

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

## WiFi (Stamp-AddOn C6・実機検証済み)

P4 自体に無線は無い。Stamp-AddOn C6 (SDIO コプロセッサ) +
`esp_wifi_remote` / `esp_hosted` で通常の `esp_wifi_*` API がそのまま使える。

- 結線は sdkconfig.defaults に固定済み: SDIO **Slot 1** (GPIO マトリクス)、
  CLK=43 / CMD=44 / D0..D3=45..48 / C6 リセット=42、4-bit 40MHz。
  (Slot 0 は IOMUX 固定ピンで Stamp の結線と合わないので使えない)
- C6 は出荷時に esp-hosted スレーブファーム書き込み済み。そのまま動く。
- SSID / パスワードは `idf.py menuconfig` → **mqjs platform** で設定
  (SSID 空なら WiFi はスキップされ、JS タスクだけ動く)。
- 起動シーケンス: `app_main` が `wifi_start_and_wait(30000)` で IP 取得まで
  最大 30 秒ブロック → JS タスク起動。切断時は自動再接続 (main/wifi.c)。

## MQTT 経由のタスク配信 (Ed25519 署名 + LittleFS 永続化・実機検証済み)

ホスト側 (main/task_source.c) が JS とは独立した esp-mqtt クライアントで
制御トピックを購読する。ペイロードは **Ed25519 署名(64B) ‖ JS ソース**。
署名がファームウェア埋め込みの公開鍵 (main/task_pubkey.h) で検証できた
タスクだけが受理され、LittleFS に永続化された上で実行中タスクを置き換える。

```bash
# 初回のみ: 鍵ペア生成 (秘密鍵 tools/task_signing_key.pem は gitignore 済み。
#           紛失したら --force で再生成 → 全デバイス再書き込み)
python3 tools/mqjs_keygen.py

# 署名して配信 (要 WSL + python3-cryptography)
python3 tools/mqjs_push.py <ブローカー> <タスクトピック> examples/bench.js
python3 tools/mqjs_push.py <ブローカー> <タスクトピック> x.js --raw  # 未署名→拒否される
```

- 検証 OK → `/littlefs/task.js` に保存 → `mqjs_runtime_stop()` で中断 →
  新品のコンテキストで即実行。**リセット後も永続化タスクが優先される**
  (埋め込みスクリプトに戻すには `idf.py erase-flash` か署名付きで上書き)。
- 検証 NG → `error: bad signature` を `<トピック>/status` に publish して破棄。
- ブローカーとトピックは `menuconfig` → **mqjs platform** で設定。
  署名込みで 1 MQTT パケット (32KB) まで。
- 検証は vendoring した TweetNaCl (`components/tweetnacl/`, public domain) の
  `crypto_sign_open`。PC ラウンドトリップテストは `test_verify.c` 参照。
- **バイトコードも配信できる** (実機検証済み)。PC で事前コンパイルすると
  デバイス側はパーサーを通らず即実行になる:
  ```bash
  cd components/mqjs   # WSL
  gcc -O2 -I. -Igen_pc -Imquickjs -o /tmp/mqjs_compile tools/compile_task.c \
      mqjs_runtime.c mquickjs/mquickjs.c mquickjs/cutils.c mquickjs/dtoa.c mquickjs/libm.c -lm
  /tmp/mqjs_compile ../../examples/bench.js /tmp/bench.bin   # 32-bit (P4 用)
  python3 ../../tools/mqjs_push.py <broker> <topic> /tmp/bench.bin  # 署名・配信は JS と同一
  ```
  `-m64` を付ければホスト幅で出力され `run_pc` でスモークテストできる。
  ローダはバイトコードを検証しない (mquickjs の仕様) ので、署名ゲートが
  実質の信頼境界。デバイス側は magic で自動判別 (`JS_IsBytecode` →
  relocate → `JS_LoadBytecode` → `JS_Run`)。
- 開発はローカルの Mosquitto が便利 (Windows: `listener 1883` +
  `allow_anonymous true` を追記し、ファイアウォールで LAN に限定)。

## PC でのテスト (実機不要・検証済み)

Linux / WSL 上で (Windows ネイティブには gcc が無い前提):

```bash
cd components/mqjs
gcc -O2 -I mquickjs -o /tmp/stdlib_tool device_stdlib.c mquickjs/mquickjs_build.c
mkdir -p gen_pc
/tmp/stdlib_tool -a -m64 > gen_pc/mquickjs_atom.h   # PC は 64bit
/tmp/stdlib_tool -m64    > gen_pc/device_stdlib.h
gcc -O2 -I. -Igen_pc -Imquickjs -o /tmp/run_pc tools/run_pc.c \
    mqjs_runtime.c mquickjs/mquickjs.c mquickjs/cutils.c \
    mquickjs/dtoa.c mquickjs/libm.c -lm

/tmp/run_pc ../../examples/bench.js          # 終わるスクリプトはそのまま
timeout 3 /tmp/run_pc ../../examples/life.js # 終わらないものは timeout で
```

gpio.* はスタブ (print するだけ)、タイマーは実時間で動く。
`tools/test_pc.c` は固定スクリプトの最小スモークテスト版。

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
- **呼び出し規約**: `JS_StackCheck(ctx, nargs+2)` → 引数を**逆順に** push
  (最後に push したものが arg0) → 関数を push → this を push →
  `JS_Call(ctx, nargs)`。
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

- ~~I2C バインディング (`i2c.readReg` / `i2c.writeReg`) — 同期で十分~~
  ✅ 完了 (M5Stack Tab5 の内蔵バスで実機検証: 7 デバイス検出、
  BMI270 chip_id=0x24 読み出し OK。examples/i2c_scan.js 参照)
- ~~Stamp-AddOn C6 + esp_wifi_remote でネットワーク導入~~ ✅ 完了 (上記 WiFi 節)
- ~~MQTT バインディング (esp-mqtt を JS の `mqtt.*` として公開)~~ ✅ 完了
  (examples/mqtt_demo.js で実機検証済み。IDF 6.0 では esp-mqtt は
  レジストリ部品 `espressif/mqtt` になった点に注意)
- ~~LittleFS パーティション + タスクのダウンロード/署名検証 (Ed25519)~~
  ✅ 完了 (上記タスク配信節)
- ~~`mqjs -o -m32` で事前コンパイルしたバイトコード配信 (信頼済みソース限定)~~
  ✅ 完了 (tools/compile_task.c + 上記タスク配信節。Tab5 実機で検証済み)

## ライセンス

mquickjs は MIT License (Fabrice Bellard / Charlie Gordon)。
`components/mqjs/mquickjs/LICENSE` を参照。
