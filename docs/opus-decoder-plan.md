# Opus デコーダ導入計画 — ESP32-P4 float + PIE asm

ステータス: **P1 実装・portable C baseline・CELT profile 実施済み (2026-06-13)**。
実装ブランチ: `codex/opus-float-plan`。

## 0. 結論

Opus コーデック本体は ESP Component Registry の `78/esp-opus` **1.0.5**
をベースにする。ただし Registry 版の `CMakeLists.txt` は次を強制しており、
そのままでは本計画の float 優先方針を満たさない。

- `FIXED_POINT=1`
- `DISABLE_FLOAT_API`
- SILK の固定小数点ソース (`silk/fixed/*.c`) のみをビルド
- `USE_ALLOCA`
- コンポーネント単位の `-O2`

したがって、初期実装では 1.0.5 のソースを追跡可能なローカルコンポーネント
として取り込み、CMake のソース選択と定義を変更する。最終到達点は、float
デコーダの主要カーネルを ESP32-P4 PIE asm へ段階的に置き換えることとする。

初期 C 実装の時点から asm 差し替え境界、portable C reference、dispatch、
比較テスト、C fallback を設ける。upstream 関数へ場当たり的に asm を埋め込まず、
計測で優先順位を決めながら、カーネル単位で安全に置換可能な構造にする。

成功条件は、Tab5 で Opus packet を継続的にデコードし、ES8388 経由で音切れ
なく再生できること、かつ UI・カメラ・MQTT の既存動作を阻害しないこと。

## 1. スコープ

### 対象

- raw Opus packet から signed 16-bit PCM へのデコード
- 8 / 12 / 16 / 24 / 48 kHz、mono / stereo
- Tab5 の ES8388 + I2S による PCM 再生
- float build と fixed-point build の比較測定
- float portable C カーネルから P4 PIE asm カーネルへの段階的置換
- デコード時間、CPU 使用率、スタック、内蔵 RAM、PSRAM 使用量の計測
- 将来の JavaScript API またはストリーミング入力を載せられる C API 境界

### 初期スコープ外

- Opus エンコード
- Ogg/Opus コンテナの demux
- HTTP/MQTT/RTP 固有のストリーミング実装
- マイク入力、AEC、録音
- 音量 UI やプレイヤー UI

Ogg ファイル再生が必要になった場合は、Opus デコーダとは別コンポーネント
として demuxer を追加する。libopus にコンテナ責務を持たせない。

## 2. ベースライブラリの管理方針

### 採用

- upstream: `78/esp-opus`
- baseline: ESP Component Registry `1.0.5`
- API: upstream の `opus_decoder_create()`, `opus_decode()` 等を維持

### ローカル化が必要な理由

Registry 版を `main/idf_component.yml` に追加するだけでは fixed-point build
になる。managed component を生成後に直接編集すると再取得で変更が失われ、
差分管理も困難になる。

初期実装では `components/esp_opus_float/` に 1.0.5 を取り込み、以下を明示する。

- upstream のバージョンと commit
- upstream から変更したファイル一覧
- ライセンスファイル
- float / fixed の選択を行う Kconfig と CMake

将来の upstream 更新は「新バージョンを取り込み、ローカル CMake 差分を再適用、
テストベクタを再実行」の手順に固定する。

## 3. float build 方針

### 原則

ESP32-P4 は単精度・倍精度 FPU を持つため、まず libopus の float 実装を評価する。
「float API を呼ぶ」だけではなく、内部処理も非 `FIXED_POINT` build にする。

ローカルコンポーネントの初期設定:

- `FIXED_POINT` を定義しない
- `DISABLE_FLOAT_API` を定義しない
- `silk/fixed/*.c` の代わりに `silk/float/*.c` を選択
- 共通 SILK / CELT / Opus ソースは upstream 1.0.5 の構成を維持
- 最初は Registry 版と同じ `-O2`
- `-ffast-math` は使用しない

`-ffast-math` は libopus の NaN/Inf 前提を壊し得るため、性能測定だけを理由に
有効化しない。

### fixed-point 比較系

float 採用を推測で確定しないため、Kconfig で fixed-point build も選択可能にする。
同じ packet と同じ再生経路で比較し、次を記録する。

- 1 packet の decode 時間: median / p95 / max
- 20 ms 音声に対するリアルタイム係数
- decoder state サイズ
- タスクの stack high-water mark
- 内蔵 RAM / PSRAM の前後差分
- PCM 差分と聴感上の異常

float を既定にするゲート:

- 対象ワークロードすべてでリアルタイム期限を十分に満たす
- fixed より明確に遅くない、または遅くてもシステム余裕を損なわない
- UI、カメラ、ネットワーク同時動作で underrun が発生しない

## 4. コンポーネント構成

予定構成:

```text
components/
  esp_opus_float/       78/esp-opus 1.0.5 ベースの codec 本体
  opus_p4_kernels/      portable C reference + P4 PIE asm + dispatch
  audio_tab5/           Tab5 の ES8388、I2S、PCM queue、再生タスク
```

`esp_opus_float` は codec のみを提供し、ESP-IDF ドライバや Tab5 に依存しない。
upstream 差分は、選定した演算を `opus_p4_kernels` の内部 ABI 経由で呼ぶための
最小限の hook に留める。

`opus_p4_kernels` は次を所有する。

- libopus 内部型を外へ漏らさない、固定幅型と明示的 stride の kernel ABI
- portable C reference 実装
- ESP32-P4 PIE asm 実装
- compile-time / runtime dispatch
- asm と C reference の比較検証
- kernel ごとの cycle / call count テレメトリ

カーネル ABI は、連続バッファ、要素数、stride、アラインメント、alias 条件、
入出力範囲、丸め、飽和、NaN/Inf の扱いを明文化する。asm 化のためだけに
中間バッファを増やし、メモリ帯域で利益を失う設計は避ける。

`audio_tab5` は次を所有する。

- 共有 I2C バス上の ES8388 初期化
- I2S TX: MCLK GPIO30、BCLK GPIO27、WS GPIO29、DOUT GPIO26
- IO expander `0x43` P1 の `SPK_EN`
- DMA 対応 PCM バッファ
- デコード・再生キューと underrun/overflow カウンタ
- Stamp-P4 など非 Tab5 構成向けの no-op stub

既存の `ui_tab5_i2c_bus()` を共有し、音声側で別の I2C master bus を作らない。
IO expander のレジスタ全体を書き潰さず、既存状態を維持した read-modify-write
または共有 board abstraction を使う。

## 5. データフロー

```text
packet source
  -> bounded Opus packet queue
  -> audio decode task
  -> opus_decode()
  -> bounded PCM queue / ring buffer
  -> I2S DMA
  -> ES8388
  -> speaker
```

- packet queue と PCM queue は上限を持たせる。
- decoder state と頻繁に触る小さい作業領域は内蔵 RAM を優先する。
- 大きい先読みバッファは PSRAM 使用を許可する。
- I2S DMA バッファは DMA 対応内蔵 RAM に置く。
- UI の LVGL task が Core 1、JS task が Core 0 を使う現状を計測した上で、
  audio task の affinity と priority を決める。
- `USE_ALLOCA` は初期比較では維持するが、stack high-water mark が不足する場合は
  allocator 方針を見直す。

## 6. asm 差し替え可能な設計

### カーネル境界

初期実装時に libopus の演算を無差別に wrapper 化しない。プロファイル上有望で、
かつ独立した契約に切り出せる演算だけを kernel ABI にする。

初回 CELT profile 後の優先候補:

1. CELT `anti_collapse`
2. CELT `exp_rotation` / `normalise_residual`
3. CELT `denormalise_bands`
4. CELT inverse MDCT / FFT の butterfly と post-rotation
5. SILK float LPC synthesis / resampler（SILK fixture 計測後）
6. PCM gain、clip、mono/stereo 整形

既存の CELT float inner product hook は今回の CELT-only fixture の上位 30 関数に
入らなかった。asm 化を急がず、まず上位の連続 float 演算を portable C で整理し、
同じ kernel ABI を維持したまま PIE asm へ置換できる形にする。

entropy decode、分岐の多い制御処理、小さい単発ループは原則 portable C のまま
残す。PIE は連続データを十分長く処理できる演算へ集中する。

### ABI と dispatch

各カーネルは次の形を基本とする。

```c
typedef struct {
    void (*inner_prod_f32)(const float *a, const float *b, size_t n,
                           float *result);
    /* 実測で採用したカーネルだけを追加する。 */
} opus_kernel_ops_t;
```

- `*_c()` は常にビルドされる portable reference。
- `*_p4()` は P4 のみビルドする `.S` 実装。
- codec 初期化時に `opus_kernel_ops_t` を確定し、hot loop 内で target 判定しない。
- Kconfig で `portable C`、`P4 asm`、`P4 asm + verify` を選択可能にする。
- asm 未実装または検証失敗時は自動的に C へ fallback する。
- upstream hook は dispatch 呼び出しだけにし、upstream 更新時の競合を抑える。

関数ポインタ呼び出しのオーバーヘッドが短いカーネルで問題になる場合は、
codec 初期化時に上位粒度の関数を切り替えるか、P4 build 専用の compile-time
dispatch を使用する。ABI の都合で細切れ呼び出しを増やさない。

### 正しさ

- asm 実装前に、同じ ABI の C reference と独立テスト fixture を作る。
- 固定小数点カーネルは bit-exact を必須にする。
- float カーネルは演算順変更で丸め差が出るため、カーネル単体の ULP/絶対誤差、
  packet 単位の PCM 誤差、長時間 decode の drift をそれぞれ検証する。
- NaN、Inf、denormal、ゼロ長、非 16-byte aligned 入力、端数 lane を含める。
- `P4 asm + verify` では初期 packet または test fixture を C/asm 両方で実行し、
  不一致なら boot 中に asm を無効化する。

### asm 実装規約

- `.S` は P4 専用コンポーネントに置き、generic libopus ソースへ混在させない。
- P4 toolchain の既定 `-march` を使用し、独自 `-march` で他拡張を落とさない。
- ABI、使用 q register、XACC/QACC、alignment、hardware loop の終端規約を
  ファイル冒頭に記録する。
- C reference と同じ契約を保ち、asm 都合の公開 API 変更を行わない。
- 各 asm カーネルに単独 benchmark と before/after の cycle 値を残す。
- 改善しないカーネルは採用せず、C 実装へ戻す。

## 7. 実装ステップとゲート

### P1. codec 単体導入

- `78/esp-opus` 1.0.5 をローカルコンポーネント化
- float / fixed のビルド選択を追加
- decoder-only の利用例と最小 wrapper を追加
- raw packet をデコードするホストまたはデバイステストを追加
- `opus_p4_kernels` の ABI、portable C、dispatch、verify skeleton を追加

ゲート:

- float build が ESP32-P4 / ESP-IDF 6.0 でコンパイルできる
- 公式 Opus test vector または既知 packet の PCM 結果を検証できる
- upstream API への不要な変更がない
- codec が portable C dispatch 経由で動き、将来の asm 置換箇所が分離されている

### P2. Tab5 PCM 出力

- `audio_tab5` を追加
- ES8388、I2S TX、`SPK_EN` を初期化
- 生成した PCM tone または埋め込み PCM を再生

ゲート:

- codec を使わず、PCM 経路だけで安定再生できる
- I2C 共有による touch / camera 回帰がない
- underrun、I2S error がない

### P3. Opus decode + playback

- 埋め込み raw Opus packet 列をデコードして再生
- decode / queue / I2S のテレメトリを追加
- float と fixed を同一条件で比較

ゲート:

- 48 kHz stereo を含む対象条件でリアルタイム再生
- decode max 時間が packet duration を超えない
- 連続再生でメモリ増加、stack 不足、音切れがない

### P4. 既存機能との同時動作

- UI 操作、カメラ preview/scan、MQTT、JS apps と同時に再生
- CPU affinity、priority、queue サイズを調整

ゲート:

- UI latency と camera telemetry の明確な悪化がない
- audio underrun がない
- 内蔵 RAM の安全余裕を維持

### P5. P4 asm 優先順位決定

プロファイルで支配項を特定し、asm 化する順序を決める。

候補:

- CELT inverse MDCT / FFT
- CELT inner product / band operations
- SILK LPC synthesis / resampler
- PCM interleave、gain、clip

各 PIE カーネルの採用条件:

- 対象カーネルが decode 時間の十分大きな割合を占める
- 呼び出し境界や中間バッファの追加が利益を相殺しない
- end-to-end decode 時間または CPU 余裕が明確に改善する
- C reference との比較検証を通過する

### P6. P4 PIE asm への段階的置換

- 優先順位の高いカーネルから `*_p4.S` を実装
- C/asm 単体 benchmark と correctness test を実行
- `P4 asm + verify` で実機 decode
- 採用条件を満たしたカーネルのみ既定 dispatch に追加

ゲート:

- すべての asm カーネルに C reference と fallback がある
- float 誤差が定義した許容範囲内
- codec test vector と連続再生を通過
- end-to-end の改善値を記録

### P7. asm 統合後の同時動作検証

- P4 asm 有効状態で UI、カメラ、MQTT、JS apps と同時再生
- PIE register / hardware-loop 使用による割り込み・task 切替時の問題を確認
- portable C build と P4 asm build の回帰比較

ゲート:

- underrun、PCM corruption、例外がない
- portable C build よりシステム余裕が改善する
- asm 無効化で常に portable C 動作へ戻せる

## 8. 検証マトリクス

最低限の codec 条件:

| Rate | Channels | Mode |
|---|---:|---|
| 8 kHz | mono | SILK speech |
| 16 kHz | mono | SILK/hybrid speech |
| 24 kHz | mono | hybrid |
| 48 kHz | mono | CELT |
| 48 kHz | stereo | CELT music |

各条件で正常 packet、packet loss concealment (`data == NULL`)、不正 packet、
最大 frame duration を確認する。

システム条件:

- audio only
- audio + UI interaction
- audio + camera preview/scan
- audio + Wi-Fi/MQTT + JS app

実装方式:

- float portable C
- float P4 asm
- float P4 asm + verify
- fixed-point portable C（比較基準）

## 9. 競合回避と作業ルール

裏で別作業が行われているため、以下を毎回守る。

1. build、flash、monitor、COM 接続の直前に関連プロセスと COM 使用状況を確認する。
2. ユーザー確認なしに flash、monitor、COM 接続を行わない。
3. 専用 build directory を worktree 内または明示した別パスに置き、既存
   `build` / `build_tab5` を共有しない。
4. dependency lock 更新は本 worktree 内だけで行う。
5. 実機計測値は条件、sdkconfig、commit と一緒に文書化する。

## 10. 未決事項

- 最初に提供する packet source: 埋め込み fixture、LittleFS、または JS API
- Ogg demux が初期リリースに必要か
- speaker playback のみか、将来 microphone path も同じ abstraction に載せるか
- Registry component の更新追従を subtree、script、手動 vendor のどれで管理するか
- float 採用判定に必要な許容 CPU 使用率と最低内蔵 RAM 余裕
- float asm カーネルごとの許容誤差と packet 単位 PCM 誤差
- 最初に asm 化する CELT カーネルの粒度

## 11. Portable C float baseline

2026-06-13 に ESP32-P4 rev 1.3、CPU 360 MHz、ESP-IDF 6.0.1 で測定した。
commit は `dc62982` 以降の Opus benchmark 実装を含む dirty build。

条件:

- `CONFIG_MQJS_OPUS_BENCHMARK=y`
- portable C dispatch
- 48 kHz mono、20 ms、CELT-only、64 kbps
- 決定論的な float PCM から生成した 187-byte packet
- decoder warmup 10 frames、測定 500 frames

結果:

| Target | Time |
|---|---:|
| inner product, 120 floats | 2.119 us/call |
| inner product, 240 floats | 4.208 us/call |
| inner product, 480 floats | 8.491 us/call |
| inner product, 960 floats | 17.571 us/call |
| CELT float decode, 20 ms frame | 1,790.7 us/frame |
| CELT float decode realtime load | 8.95% |

初回測定では `main` task の既定 stack 上で encoder/decoder を実行して stack
protection fault になったため、benchmark は 32 KB の専用 task で実行する。

## 12. CELT float function profile

2026-06-13 に COM8 へ専用 profile build を flash し、baseline と同じ 48 kHz
mono、20 ms、CELT-only packet を 100 frames decode して測定した。
`-finstrument-functions` による関数単位の approximate self cycle であり、
instrumentation overhead を含むため絶対時間ではなく優先順位の判断に使う。

上位関数:

| Rank | Function | Self cycles | Calls |
|---:|---|---:|---:|
| 1 | `anti_collapse` | 14,754,002 | 100 |
| 2 | `exp_rotation` | 9,363,687 | 5,900 |
| 3 | `normalise_residual` | 7,485,034 | 5,900 |
| 4 | `denormalise_bands` | 6,858,367 | 100 |
| 5 | `quant_band` | 3,958,682 | 2,100 |
| 6 | `quant_partition` | 3,470,721 | 9,700 |
| 7 | `cwrsi` | 3,328,754 | 5,900 |
| 8 | `compute_theta` | 2,768,382 | 3,800 |
| 9 | `clt_mdct_backward_c` | 2,617,032 | 800 |
| 10 | `ec_dec_uint` | 2,194,914 | 8,600 |

- `dropped=0` で、複数回の測定でも順位は概ね再現した。
- profile build の decode は約 2,012 us/frame、10.06% realtime まで増えた。
  非 profile baseline は 1,790.7 us/frame、8.95% realtime を採用する。
- `celt_inner_prod` は上位 30 件に入らず、この fixture では初期最適化対象にしない。
- entropy decode 系は call 数が多いが分岐中心のため、初期の SIMD/PIE asm 対象から
  外す。
- 最初の portable C 最適化は `anti_collapse`、`exp_rotation`、
  `normalise_residual`、`denormalise_bands`、MDCT の順に調査する。
- stereo CELT、SILK、hybrid の profile を追加するまでは全 Opus workload に
  一般化しない。

benchmark 完了後に既存 startup 経路の `tcpip_send_msg_wait_sem` assert が発生した。
profile 出力後の事象で計測値には影響しないが、audio 統合前に別途切り分ける。

### Stereo fixture

実音源を使う stereo CELT profile 用に次を追加した。

- source: `assets/audio/tab5-boot.wav`
  - 48 kHz、stereo、signed 16-bit PCM、6.32 秒
- encoded: `assets/audio/tab5-boot-48k.opus`
  - libopus、audio application、20 ms frame、CBR 48 kbps
  - Ogg Opus、38,683 bytes、コンテナ込み実効 48.9 kbps

生成コマンド:

```powershell
ffmpeg -i assets/audio/tab5-boot.wav -map_metadata -1 -c:a libopus `
  -application audio -b:a 48k -vbr off -frame_duration 20 `
  assets/audio/tab5-boot-48k.opus
```

codec benchmark は Ogg を直接扱わないため、次の実装では Ogg page から Opus packet
を抽出した決定論的 fixture を生成する。48 kbps は低帯域 stereo の負荷確認に使い、
音質比較と高複雑度側の profile には 96 kbps fixture も追加する。

### Initial playback implementation

`components/opus_player/` に、埋め込みまたはメモリ上の Ogg Opus を既存
`audio_tab5` PCM pipeline へ流す初期実装を追加した。

```text
Ogg page/lacing parser
  -> Opus packet
  -> opus_decode() (48 kHz signed 16-bit PCM)
  -> audio_tab5_write()
  -> PCM ring / I2S / ES8388
```

- Ogg parser は IDF 非依存で、pageをまたぐpacket、`OpusHead`、`OpusTags`、
  mono/stereo mapping family 0 を扱う。
- `pre_skip` を適用し、audio ring の backpressure に従ってblocking writeする。
- `CONFIG_OPUS_PLAYER_BOOT_OPUS` で48 kbps fixtureを埋め込み、
  `CONFIG_OPUS_PLAYER_BOOT_AUTOPLAY` で実機確認できる。
- 初期版はメモリ上の完全なOgg blobのみ対象。filesystem/streaming input、
  chained Ogg、CRC検証、最終page granuleによる末尾padding除去は後続実装とする。
- WAV/selftestとOpus autoplayの同時producer起動は禁止する。将来はaudio session
  ownershipを共通化してpreemptionを実装する。

2026-06-13 verification:

- host parser test: 48 kbps fixtureを317 audio packetsとして走査、
  stereo、`pre_skip=312`。capture破損と末尾欠損を拒否。
- ESP-IDF 6.0.1 dedicated build: `build_opus_play`
  - `CONFIG_MQJS_TAB5_AUDIO=y`
  - `CONFIG_OPUS_PLAYER=y`
  - `CONFIG_OPUS_PLAYER_BOOT_OPUS=y`
  - binary `0x13d9f0`、app partition空き79%
- flashと実機autoplayは未実施。

## 13. 実音源 decoder-only ベンチ

最適化の end-to-end 判定用として、`components/opus_player/opus_bench.c` に
実機ベンチを置く。boot直後、UI・Wi-Fi・audio出力を起動する前に、埋め込み
48 kbps stereo fixture全体を複数周デコードする。

測定範囲は `opus_decode()` のみとし、Ogg parser、PCM hash、I2S、speakerは
packet時間から除外する。各周でdecoderを再生成し、実際のpacket順序と状態遷移を
維持する。

出力:

- 全packetのdecode合計時間とaudio durationに対するrealtime load
- decode cycle合計とcycles/frame
- packet latencyのmin / p50 / p95 / p99 / max
- packet数、圧縮bytes、decoded frames
- 周回ごとのPCM FNV-1a checksum
- 全周のthroughput (`x realtime`)

専用build:

```powershell
. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1
$env:ESP_IDF_VERSION='6.0'
idf.py -B build_opus_bench `
  -D SDKCONFIG=build_opus_bench/sdkconfig `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.tab5.defaults.example;sdkconfig.opus-bench.defaults" `
  build
```

通常buildの`sdkconfig`を変更せず、UI・カメラ・Wi-Fiも有効化しない静かな
decoder-only構成とする。計測前はbuild / flash / monitor / COM8の競合を確認する。
関数profileが必要な場合だけ
`CONFIG_OPUS_P4_FUNCTION_PROFILE=y`を追加し、profile値は絶対時間baselineと
分離して扱う。

### 2026-06-13 stereo fixture baseline

COM8へ`build_opus_bench`をflashし、ESP32-P4 rev 1.3、CPU 360 MHz、
ESP-IDF 6.0.1で測定した。計測firmwareは`2b7185b-dirty`で、本節の
real-fixture benchmark実装を含む。function profile、audio出力は無効。

条件:

- fixture: `assets/audio/tab5-boot-48k.opus`、38,683 bytes
- 48 kHz stereo、48 kbps CBR、20 ms packet
- warmup 1周、測定5周
- 各周でdecoderを再生成
- `opus_decode()`のみpacket時間として計測

結果:

| Metric | Result |
|---|---:|
| Audio packets / pass | 317 |
| Compressed audio bytes / pass | 38,040 |
| Decoded frames / pass | 304,320 |
| Decode time / pass | 714,040–714,357 us |
| Decode cycles / frame | 845.0–845.4 |
| Realtime load | 11.26–11.27% |
| Throughput | 8.88x realtime |
| Packet p50 | 2,099–2,108 us |
| Packet p95 | 2,819–2,828 us |
| Packet p99 | 2,889–2,899 us |
| Packet max | 3,335–3,360 us |
| PCM FNV-1a | `37e8c5b59e9a40c6`、全5周一致 |

5周合計はdecode `3,570,866 us`、`1,285,973,195 cycles`、
`845.1 cycles/frame`。周回差は小さく、以後のportable C / PIE asm比較の
end-to-end baselineとして使用できる。

### anti-collapse signed-noise kernel候補

`anti_collapse()`全体はband/channel/block制御、collapse判定、seed更新、
renormalizeを含み、最初のASM境界としては広すぎる。最初のportable C候補は、
collapsed short-MDCT blockへ決定的な符号付きnoiseを書き込むループだけとする。

ABI:

```c
uint32_t opus_p4_anti_collapse_noise_f32(
    float *x, int n, int stride, float r, uint32_t seed);
```

- CELTのLCG seed進行と符号選択をbit-exactに維持する。
- `stride`をABIに含め、`LM`ごとのstrided writeをcodec外へ切り出す。
- portable C referenceとdispatch wrapperを用意し、後続のPIE ASMは同じABIで
  置換する。
- 生成コードではstatic dispatchが解決され、wrapper内に70-byteの独立ループが
  生成された。hot loop内の関数ポインタdispatchは残っていない。

2026-06-13にESP-IDF 6.0.1でbenchmark build、ASM-verify build、通常buildが
成功した。portable C構成をCOM8へflashし、baselineと同じ条件で実測した。

| Metric | Baseline | Signed-noise kernel boundary |
|---|---:|---:|
| Decode total / 5 passes | 3,570,866 us | 3,566,822 us |
| Cycles / frame | 845.1 | 844.2 |
| Realtime load | 11.26% | 11.25% |
| Throughput | 8.88x | 8.89x |
| PCM FNV-1a | `37e8c5b59e9a40c6` | 同値、全5周一致 |

`cycles/frame`は約0.11%改善したが、周回揺らぎに近く、この境界切り出し単体の
明確な高速化とは判断しない。bit-exact性と動作回帰がないことは確認できたため、
後続ASMの置換境界として維持する。ASM採用判断では引き続き`845.1 cycles/frame`
をend-to-end baselineとして使用する。

### ASM差し替え下準備

実ASM最適化へ入る前に、`anti-collapse signed-noise`カーネルで次の経路を
実装する。

- `opus_p4_anti_collapse_noise_f32_p4`をP4用`.S`シンボルとして分離する。
- 現在の`.S`本体はportable C referenceへのtail callだけを行うABI scaffold。
  PIE命令による最適化はまだ行わない。
- portable、asm scaffold、asm scaffold + verifyをKconfigで選択する。
- verifyは最初の`CONFIG_OPUS_P4_VERIFY_CALLS`回で、出力floatのbit patternと
  最終seedをC契約から検証する。
- 1回でも不一致なら、その呼び出しをCで再実行して出力を修復し、以後は
  permanent C fallbackとする。
- benchmark終了時に最終kernel状態とverify failure数を出力する。

verify専用buildは通常buildと分離する。

```powershell
. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1
$env:ESP_IDF_VERSION='6.0'
idf.py -B build_opus_asm_verify `
  -D SDKCONFIG=build_opus_asm_verify/sdkconfig `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.tab5.defaults.example;sdkconfig.opus-bench.defaults;sdkconfig.opus-asm-verify.defaults" `
  build
```

この段階で確認するのは、P4 assembler、RISC-V hard-float ABI、symbol link、
compile-time dispatch、verify/fallback経路。実機速度の改善は期待せず、
ASM本体へ置換した後にportable baselineと比較する。

## 14. 参考

- ESP Component Registry: `78/esp-opus` 1.0.5
  - https://components.espressif.com/components/78/esp-opus/versions/1.0.5/readme
- upstream repository
  - https://github.com/78/esp-opus
- Xiph libopus
  - https://gitlab.xiph.org/xiph/opus
- Tab5 audio implementation reference
  - https://github.com/m5stack/M5Tab5-UserDemo
