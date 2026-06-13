# Tab5 音声出力パイプライン

M5Stack Tab5 (ESP32-P4) のスピーカー再生パス `components/audio_tab5` の
設計ドキュメント。**何をするか**だけでなく**なぜその設計か**を記録する。

- 状態・検証ログ・コミット履歴: [audio-tab5-status.md](audio-tab5-status.md)
- Opus デコーダ統合計画: [opus-decoder-plan.md](opus-decoder-plan.md)
- 複数 producer の所有権・cancel・overdub 設計:
  [audio-device-design.md](audio-device-design.md)

実機検証済み (2026-06-13, COM8): ブートビープ + WAV 自動再生を実聴、
MQTT テレメトリで再生中 frames が 48000/s ちょうど・バックプレッシャー
動作・途切れなしを確認。

---

## 1. スコープと前提

このコンポーネントは **PCM 再生のみ**を担う。コーデック (Opus 等) も
コンテナ demux も含まない。デコード済み signed 16-bit PCM を受け取り、
ES8388 経由で Tab5 内蔵スピーカーへ音切れなく流すのが責務。

将来、複数 producer は上位の `audio_device` を経由し、`audio_tab5` を直接
操作しない。`audio_device` は capability token と request queue を管理し、
必要な場合だけ mixer を有効化する。詳細は
[audio-device-design.md](audio-device-design.md) を参照。

- 対応レート: 8 / 12 / 16 / 24 / 44.1 / 48 kHz
- 対応チャネル: mono / stereo 入力 (出力は後述のとおり実質モノ)
- 非対象: エンコード、Ogg/MP3 demux、マイク入力/AEC、音量 UI

設計の上位原則は [opus-decoder-plan.md](opus-decoder-plan.md) §4-5 と整合。
audio_tab5 は codec にも Tab5 以外の構成にも依存せず、非 Tab5 ビルドでは
ヘッダがすべて no-op inline stub になる (`ui_tab5` と同じ流儀)。

---

## 2. ハードウェア

| 要素 | 値 | 備考 |
|---|---|---|
| コーデック | ES8388 | esp_codec_dev ~1.5、公式 m5stack_tab5 BSP と同系 |
| スピーカーアンプ | NS4150B (1W@8Ω) | **モノ・単入力** |
| 物理スピーカー | **1 個 (モノ)** | §5 のダウンミックス根拠 |
| マイク | ES7210 + 2 個 | 本コンポーネントは未使用 |
| ヘッドホン | 3.5mm ジャック | ES8388 のもう一系統 (将来の真ステレオ出力先) |
| I2S | MCLK=G30 BCLK=G27 WS=G29 DOUT=G26 | DIN=G28 はマイク、未使用 |
| I2S フォーマット | 16-bit Philips **stereo** 固定、MCLK=256·fs | `auto_clear` 有効 |
| コーデック I2C | 共有 `ui_tab5_i2c_bus()` (port 1, SDA=G31 SCL=G32) | 新規 master bus を作らない |
| SPK_EN | PI4IOE 0x43 expander **P1** (reg 0x05) | read-modify-write |

### なぜ共有 I2C バスか
ES8388 はタッチ/カメラ SCCB と同じ内部 I2C バスに載る。別の master bus を
作ると port を奪い合うので、`ui_tab5_start()` が立てた bus
(`ui_tab5_i2c_bus()`) を借りる。**前提: audio を使う前に UI を起動済みで
あること** (bus 未生成なら `audio_tab5_start` は `ESP_ERR_INVALID_STATE`)。

### なぜ SPK_EN は read-modify-write か
0x43 expander のレジスタ 0x05 には SPK_EN(P1) だけでなく LCD_RST(P4) /
TP_RST(P5) / CAM_RST(P6) が同居する。バイト全体を書き潰すと画面やカメラを
巻き込んでリセットしてしまうため、**該当ビットだけ** RMW する。

---

## 3. データフロー

```text
PCM ソース (Opus デコード / WAV / トーン生成 / 将来の JS)
  -> audio_tab5_write(pcm, frames, timeout)   ← バックプレッシャー境界
  -> PSRAM リングバッファ (既定 64KB, byte-buf)
  -> writer タスク (core 1, prio 10)
  -> i2s_channel_write()  (16-bit stereo, auto_clear)
  -> ES8388 DAC
  -> NS4150B (モノアンプ)
  -> スピーカー
```

- **プロデューサ/コンシューマ分離**: 書き手 (デコードタスク等) はリングに
  積むだけ、I2S への吐き出しは専用 writer タスクが行う。これにより
  デコードのジッタが再生のリアルタイム性に波及しない。
- **writer タスクは core 1 / prio 10**: JS は core 0、LVGL は core 1。
  writer はほぼ DMA 待ちでブロックするので、UI を飢えさせずに core 1 に
  相乗りさせている (同時動作計測 = plan P4 で affinity を再評価する余地)。
- **リングは PSRAM・上限あり (`CONFIG_MQJS_TAB5_AUDIO_RING_KB`, 既定 64KB
  ≒ 340ms @48k stereo)**: 先読みは PSRAM で良いが、無制限にはしない。
- **writer 粒度 `AUD_CHUNK_BYTES`=2048 (~5.3ms @48k stereo)**: I2S への
  1 回の write 量。小さすぎると syscall 過多、大きすぎると停止応答が鈍る。

---

## 4. 主要な設計判断とその理由

### 4.1 I2S は常に 16-bit ステレオ、MCLK=256·fs
リンクを固定フォーマットにすることで、ソースが mono でも stereo でも
I2S/DMA 構成を切り替えずに済む。mono ソースはソフトで両レーンに複製する
(§5)。レート変更時だけ `i2s_channel_reconfig_std_clock` でクロックを
差し替える (チャネル再生成はしない)。

### 4.2 `auto_clear = true` (アンダーラン時は無音)
リングが枯れたら DMA はゼロを流す。これにより **古い DMA バッファの残骸が
ノイズとして再生されない**。アンダーラン = デジタル無音であって、データ
化けではない。

### 4.3 デコード済み PCM は C 側で完結、JS 境界を跨がせない
Opus 等のデコード出力は **C 内で直接 `audio_tab5_write()` に渡す**。
理由は 2 つ:
1. mquickjs エンジンに **TypedArray の生ポインタを取る公開 C API が無い**
   (JS 配列の反復は毎サンプル遅すぎてホットパスに使えない)。
2. そもそも音声のホットパスを JS に通す必要がない。JS は制御とテレメトリ
   だけ担えば十分。

→ JS API はスカラ制御 + テレメトリに限定 (§7)。`audio.play(pcm)` は
意図的に未実装。必要になったらエンジンに最小の TypedArray アクセサを
足してから対応する。

### 4.4 トーン/WAV は非ブロッキング (一発タスク)
`audio_tab5_tone()` / `audio_tab5_play_wav_mem()` は内部でブロッキングに
リングへ流すため、長い音は呼び出し元を数百 ms〜数秒拘束する。JS バインドや
ブート処理から直接呼ぶとイベントループ/ウォッチドッグを止めるので、
`*_async()` 版が **一発タスクを spawn** して即座に返す。トーンは再入ガード
(`s_tone_busy`)、WAV は再生中なら先行クリップをプリエンプト
(`s_wav_abort`)。

### 4.5 バックプレッシャー
`audio_tab5_write(pcm, frames, timeout_ms)` はリングに空きができるまで
`timeout_ms` を上限に待ち、**実際に積めたフレーム数を返す**。プロデューサは
戻り値で進捗を知り、リングが満杯なら自然に律速される (= 消費レート 48kHz に
吸い付く)。WAV ストリーマはこの戻り値でループする。

### 4.6 ステレオ→モノ ダウンミックスはソフトで、常時 ON
→ §5 で詳述。

### 4.7 大きい WAV はファーム埋め込み (LittleFS ではない)
→ §6 で詳述。

---

## 5. ステレオ→モノ ダウンミックス (設計の肝)

### 事実: コーデックは L+R を合成しない
Tab5 スピーカーは物理モノ (NS4150B 単入力)。さらに esp_codec_dev の es8388
初期化 (`managed_components/.../device/es8388/es8388.c`) は **ストレート
ステレオ結線**:

```c
DACCONTROL17 = 0x90  // "only left DAC to left mixer enable 0db"
DACCONTROL20 = 0x90  // "only right DAC to right mixer enable 0db"
DACCONTROL24 = 0x1E  // LOUT1/ROUT1/LOUT2/ROUT2 を 0dB で有効
```

ES8388 のミキサーは「自側 DAC + 同側ライン入力バイパス」のみで、**反対側
DAC を引き込むクロス経路が無い**。つまりコーデックでは L+R 合成ができず、
未加工の 2ch をモノスピーカーへ出すと**繋がっている側の片 ch だけが鳴り、
もう片方は落ちる**。

### 対策: `(L+R)/2` をソフトで両レーンへ
`audio_tab5_write` のモノ経路で `m = (int16_t)(((int32_t)L + R) >> 1)` を
計算し、`[m, m]` を出す。

- int32 で和を取り `>>1` するので結果は **int16 範囲ちょうど** (最大
  32767+32767→32767、最小 -32768+-32768→-32768)。**クリップ不要**。
- 算術右シフト = -∞ 方向丸めだが、可聴バイアスは無い。

**既定 ON** (`s_downmix`)。駆動先が物理モノスピーカーのみのため
(ユーザー方針「スピーカー出力なら常に L+R」)。将来ヘッドホンへ真ステレオを
出すときだけ `audio.downmix(0)` で OFF にする。

### なぜ PIE/拡張命令を使わないか
ダウンミックスは 48kHz で毎サンプル ≒ 加算1+シフト1 = **約 0.04% CPU**。
再生は I2S が 48kHz でハードリアルタイム律速しており、プロデューサ側に余裕は
膨大。連続データなので PIE 向きではあるが:

1. インタリーブ `[L0 R0 L1 R1 ...]` から L+R を出すには deinterleave
   (ストライドロード/シャッフル) が要り、前処理コストが先行する。
2. そもそもボトルネックでないため、このプロジェクトの **計測ゲート方針**
   (実益が出た所だけ PIE 採用) を通らない。

→ スカラ C のまま。`audio_tab5_write` 内の独立ループ = カーネル境界として
切ってあるので、万一「ステレオ + Opus + カメラ + UI 同時」でオーディオ CPU
逼迫が**計測されたら**そこだけ差し替えられる。PIE 予算は Opus の CELT
カーネル (iMDCT/FFT/inner product, [opus-decoder-plan](opus-decoder-plan.md)
§5-6) に回す。そこは計算律速で実益が出る。

---

## 6. WAV 再生

### パーサ (`wav.c` / `wav.h`)
純 C の RIFF/WAVE パーサ。メモリ上の blob を走査して `fmt `/`data` を探し、
`LIST`/`INFO` 等は読み飛ばす。data サイズ過大は buffer 長に clamp、奇数
サイズの word パディングに耐性。16-bit 整数 PCM (1|2ch) のみ受理。

- ESP/IDF 非依存 = ホスト単体テスト可能。`tools/wav_test.c` を ASAN で
  ビルドし、実アセット + 合成エッジケースを検証 (新モジュールは統合前に
  PC で単体検証する方針)。

### ソースは埋め込み blob
テスト素材 `assets/audio/tab5-boot.wav` (48k/stereo/16-bit/6.3s, 1.21MB)。

- **なぜ LittleFS でないか**: `storage` パーティションは 1MB で、1.2MB の
  WAV が入らない。factory アプリ領域 (6MB) には埋め込み後でも ~1.1MB の
  余裕がある。
- `CONFIG_MQJS_TAB5_AUDIO_BOOT_WAV` で `EMBED_FILES` 埋め込み (**既定 OFF**
  — flash 毎に 1.2MB 肥大するため)。`_AUTOPLAY` でブート ~3.5s 後に自動
  再生。`audio.playWav()` / `audio_tab5_play_boot_wav()` でオンデマンド。
- 将来ファイル転送経路 (LittleFS への push、または partition 拡張) が
  できたら `audio_tab5_play_wav_file(path)` を足す。`audio.playWav()` が
  パス指定でないのはこのため。

---

## 7. API

### C API (`audio_tab5.h`)
非 Tab5 / `CONFIG_MQJS_TAB5_AUDIO` 無効時はすべて no-op inline stub。

| 関数 | 役割 |
|---|---|
| `audio_tab5_start(rate, ch)` | 再生パス起動 / レート・ch 再設定 |
| `audio_tab5_write(pcm, frames, timeout_ms)` | s16 PCM 投入、戻り値=積めた frames (バックプレッシャー) |
| `audio_tab5_stop()` | ストリーム停止 + リングをドレイン (HW は維持) |
| `audio_tab5_set_volume(pct)` / `audio_tab5_volume()` | 音量 0..100 |
| `audio_tab5_set_downmix(on)` / `audio_tab5_downmix()` | ステレオ→モノ fold (既定 ON) |
| `audio_tab5_get_stats(&st)` | running/rate/ch/queued/underruns/frames_written |
| `audio_tab5_tone(hz, ms)` / `audio_tab5_tone_async(hz, ms)` | 正弦トーン (同期/タスク) |
| `audio_tab5_play_wav_mem(buf, len)` / `_async(buf, len)` | メモリ WAV 再生 (同期/タスク、buf は再生中存続必須) |
| `audio_tab5_wav_playing()` | WAV 再生中か |
| `audio_tab5_play_boot_wav()` | 埋め込み WAV 再生 |
| `audio_tab5_selftest_async()` | ブート自己テスト (ビープ + 任意で WAV autoplay) |

### JS API (`audio.*`)
スカラ制御 + テレメトリのみ (§4.3)。

| メソッド | 戻り値 | 役割 |
|---|---|---|
| `audio.start(rate=48000, ch=2)` | bool | 起動 |
| `audio.stop()` | undefined | 停止 |
| `audio.tone(hz, ms)` | bool | 非ブロッキングのビープ |
| `audio.volume([pct])` | 0..100 | 取得 / 設定 |
| `audio.downmix([on])` | bool | fold 取得 / 設定 |
| `audio.playWav()` | bool | 埋め込み WAV 再生 |
| `audio.stats()` | JSON 文字列 | テレメトリ (COM 不要の遠隔読み出し) |

**意図的な未実装** (スコープ外、必要時に追加):
- `audio.play(pcm)` — JS から任意 PCM 投入。エンジンに TypedArray リーダが
  無く、Opus path は C 完結のため不要 (§4.3)。
- `audio.playWav(path)` — ファイル指定再生。ファイル転送経路が未整備 (§6)。

---

## 8. Kconfig

| シンボル | 既定 | 説明 |
|---|---|---|
| `MQJS_TAB5_AUDIO` | n | 本コンポーネント有効化 (Tab5 ビルドで y) |
| `MQJS_TAB5_AUDIO_RING_KB` | 64 | PCM リング容量 (KB, PSRAM) |
| `MQJS_TAB5_AUDIO_SELFTEST` | n | ブート ~3s 後にビープ ×2 (P2 ゲート) |
| `MQJS_TAB5_AUDIO_BOOT_WAV` | n | `assets/audio/tab5-boot.wav` を埋め込み (~1.2MB) |
| `MQJS_TAB5_AUDIO_BOOT_WAV_AUTOPLAY` | n | ブート後に埋め込み WAV を自動再生 |

---

## 9. 検証

### P2 ゲート (PCM 経路、コーデック非依存)
1. `_SELFTEST=y`: ブート後ビープ ×2 (880/1319Hz)。**実聴 OK**。
2. `_BOOT_WAV_AUTOPLAY=y`: 続けて WAV 自動再生。**実聴 OK**。
3. `tools/probe_audio.js` を dev タスクに push → `<topic>/proberep` に
   `audio.stats()` を時系列 publish (COM 不要):
   - 再生中 frames が **48000/s ちょうど**で増加 (I2S が正レートで消費)。
   - queued ≈ 64KB で頭打ち (バックプレッシャー)。
   - underruns は意図ギャップのみ (途切れなし)。
   - probe 後は `tools/dev_idle.js` を push して dev スロット復元。

### フラッシュ手順 (Tab5 の DTR/RTS リセット罠回避)
```text
idf.py -B build_tab5 -p COM8 flash
python -m esptool --chip esp32p4 --port COM8 --after watchdog-reset run
```
flash 直後は Tab5 がダウンロードモードで沈黙するので watchdog-reset で
明示リブート (COM8 が一瞬消えて esptool が exit 1 = 成功)。検証は MQTT で
行い COM8 は開かない (開くとアプリがリセットされる)。

---

## 10. 既知の制約 / 今後

- **クリック/ポップ**: トーン/WAV が非ゼロ振幅で終わると段差クリックが出る。
  対策方針 = コンテンツへのフェードではなく **(a) writer の「実音↔無音
  継ぎ目」デクリックランプ + (b) トーンのゼロ交差合成**
  ([audio-tab5-status.md](audio-tab5-status.md) 参照)。アイドルのヒスは
  アンプ常時通電とのトレードオフ (コーデックのソフトミュートで対処可)。
  **未実装。**
- **JS からの PCM 投入** (`audio.play`) と**ファイル名指定再生**
  (`audio.playWav(path)`) は未実装 (§7)。
- **ヘッドホン真ステレオ出力**: ルーティング切替は未対応。出すなら
  `audio.downmix(0)` + 出力先選択ロジックが要る。
- **Opus 統合 (P3)**: デコード出力を `audio_tab5_write()` に繋ぐ。CELT
  カーネルが PIE の本命 ([opus-decoder-plan](opus-decoder-plan.md))。
- **同時動作 (P4)**: UI/カメラ/MQTT と同時再生時の writer affinity/priority・
  PSRAM バス帯域・アンダーランを計測して調整。

---

## 11. 未解決: 再生時ノイズ (2026-06-13, 保留)

起動音 / トーン / WAV のいずれも、**再生中だけ**「破裂音 / ノイズまじり」に
聞こえる。無音時は出ない。音量に依らない。原因切り分けは尽くしたが解決せず、
一旦保留。調査結果のみ記録(再開時の出発点)。

> 注: 切り分けに使った **16-band FFT アナライザ・レゾナンス HPF・peak/clip
> メータは、未使用のため調査後に削除**(内部 RAM 約8KB 回収)。よって
> `audio.hpf()` / `audio.stats()` の `spec`/`peak`/`clip` は**現存しない**
> (`audio.stats()` は running/rate/ch/queued/underruns/frames の6項目に復帰)。
> tone のデクリック包絡のみ残置。下記の findings(原因切り分け)は有効。

**ソフトは無実と確定:**

- デジタル信号は FFT でクリーン(純音トーンに倍音なし＝高バンド 0、
  `audio.stats()` の `spec` で確認)。
- ダウンミックス `(L+R)/2` は実測 `clip=0` / `peak ≤ -5.6 dBFS`(railing なし)。
- レゾナンス付き HPF で低域を削っても消えない(HPF 出力もピーク正規化で clip 解消済)。
- 音量 70→50→30→15 で不変(音量は ES8388 DAC 後段、デジタルは同一)。
- クリーンな物理電源断後も再発(watchdog リセット連打による劣化ではない)。
- リング/チャンクはフレーム整列(`AUD_CHUNK_BYTES=2048`, `AUD_RING_BYTES=65536`
  ＝ともに 4 の倍数)。
- **ES8388 / I2S 設定は M5Tab5-UserDemo(動作実績・クリーン)と完全一致**:
  ピン MCLK=30/BCLK=27/WS=29/DOUT=26、`es8388_codec_new` DAC、MCLK×256、
  `auto_clear`、16bit/stereo、hw_gain pa5.0/dac3.3。M5 式「再生前 close+open」も効果なし
  (M5 がそれをやるのは I2S を es7210 マイク RX/TDM4ch と共有し録音↔再生でバス
  再設定が要るため。本機はスピーカ専用で不要)。

**最有力仮説:** M5(軽量デモ)はクリーン、本機(MIPI-DSI 表示 + WiFi/SDIO +
カメラ並行の重環境)はノイズ → **P4 共有クロックドメインが並行負荷で乱れ I2S
MCLK がジッタ**、or アナログ/電源。測定器(オシロ/ロジアナ)領域。

**再開時の次手:** ① MCLK(G30)・DOUT・電源レールをスコープ観測。
② `clk_cfg.clk_src` を XTAL/別系統に固定して PLL 競合を切り分け。
③ es7210 含む共有 I2S 初期化を M5 流に揃える。
④ 表示/WiFi を止めて再生し負荷依存か確認。
