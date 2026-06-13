# audio_tab5 — Tab5 スピーカー再生パス (opus-decoder-plan P2)

ステータス: **実装済み・実機未検証 (2026-06-13)**。flash はユーザーのタイミング
確認待ち。Opus デコーダ (`codex/opus-float-plan` worktree) と並行作業のため、
本件は `feat/audio-tab5` ブランチ / 専用 worktree / 専用 `build_tab5` で行う。

## 実装

`docs/opus-decoder-plan.md` §4 の `audio_tab5` 仕様に準拠:

- `components/audio_tab5/` — ES8388 を esp_codec_dev (~1.5, 公式
  m5stack_tab5 BSP と同系) で駆動。共有 I2C バス `ui_tab5_i2c_bus()`
  (port 1) を使用し、新しい master bus は作らない。
- I2S master TX: MCLK=G30 BCLK=G27 WS=G29 DOUT=G26、16-bit Philips
  stereo 固定、MCLK=256*fs、`auto_clear` (underrun は無音)。
  mono 入力はソフトで両チャネル複製。
- SPK_EN = PI4IOE 0x43 の P1。レジスタ 0x05 を read-modify-write
  (P4 LCD_RST / P5 TP_RST / P6 CAM_RST が同じバイトに同居)。
- PCM ring buffer (PSRAM, `CONFIG_MQJS_TAB5_AUDIO_RING_KB`, 既定 64KB
  ≒ 340ms @48k stereo) → writer task (core 1, prio 10) → I2S DMA。
- API: `audio_tab5_start(rate, ch)` / `audio_tab5_write(pcm, frames,
  timeout)` (backpressure 付き) / `stop` / `set_volume` / `get_stats`
  (underruns, frames_written, queued_bytes) / `tone`。
- 非 Tab5 build は inline stub (`ui_tab5.h` と同パターン)、Kconfig
  `CONFIG_MQJS_TAB5_AUDIO` で sources ごと外れる。

## P2 ゲートの検証手順 (flash 後)

1. `CONFIG_MQJS_TAB5_AUDIO_SELFTEST=y` でブート ~3 秒後に 880Hz /
   1319Hz の 300ms ビープ ×2 がスピーカーから鳴る。
2. ログ `audio5: SELFTEST done: frames=... underruns=...` —
   underruns はトーン間ギャップの 1 のみが期待値。
3. タッチ / カメラの回帰がないこと (I2C 共有の確認)。

## JS バインディング `audio.*` (commit af2c98a, PC 検証済み)

実装済み。デコード済み PCM は C 側 (Opus パス) が `audio_tab5_write` に
直接流すので、JS はスカラ制御 + テレメトリのみ (エンジンに TypedArray
の公開リーダが無いことも確認 — 余計な複雑さを避けた)。

- `audio.start(rate=48000, ch=2)` -> bool
- `audio.stop()`
- `audio.tone(hz, ms)` -> bool — 非ブロッキング (一発タスク、再入ガード)。
  これが MQTT 越しに叩ける P2 検証トリガ。
- `audio.volume([pct])` -> 0..100
- `audio.stats()` -> JSON 文字列 (running/rate/ch/queued/underruns/
  frames) — COM8 不要のリモート読み出し。

`examples/audio_test.js`: Tab5 はボタン付きパネル、画面なし機はビープ列 +
MQTT へ stats publish。run_pc スモークテスト通過、Tab5 ビルドもクリーン。

実機 P2 検証 (flash 後) の遠隔手順:
1. `audio_test` を push (`tools/mqjs_push.py` または webui)。
2. UI 機ならボタンでビープ。画面なし機なら自動でビープ列。
3. `audio.stats()` の JSON を MQTT で受けて underruns/frames を確認。
   COM8 を開かずに P2 を判定できる。

## WAV 再生パイプライン (commit 1ec13d3, host + build 検証済み)

Opus デコーダより先に再生パスを実体化。テスト素材は実 WAV
`assets/audio/tab5-boot.wav` (48k / stereo / 16-bit / 6.3s)。

データフロー: WAV ソース → `wav_parse_mem` → PCM フレーム →
`audio_tab5_write` → リング → I2S → ES8388。

- `components/audio_tab5/wav.c/.h`: 純 C の RIFF/WAVE パーサ。チャンクを
  走査して fmt+data を探し LIST/INFO 等はスキップ、data サイズ過大は
  clamp、奇数サイズの word パディングに耐性。`tools/wav_test.c` で
  ASAN ホストテスト (実アセット + 合成エッジケース) 全合格。
- `audio_tab5_play_wav_mem(blob, len)`: 16-bit PCM (1|2ch) を解析して
  バックプレッシャー付きでストリーム。`stop()` / 新規再生で中断可。
  `_async()` はタスク化 + 再生中クリップをプリエンプト。mono は既存
  write パスがステレオ複製。
- 埋め込み boot WAV: 1.2MB の WAV は 1MB の littlefs storage に入らない
  ので **EMBED_FILES でファーム埋め込み** (Kconfig
  `MQJS_TAB5_AUDIO_BOOT_WAV`、既定オフ — flash 毎に肥大するため)。
  `_AUTOPLAY` でブート ~3.5s 後に自動再生。`audio.playWav()` /
  `audio_tab5_play_boot_wav()` でオンデマンド再生 (MQTT から実音検証)。
- ビルド: bin 5.17MB (factory 6MB に収まる)、SDIO/C6 無傷。

将来のファイル転送パス (LittleFS への WAV 転送、あるいは partition 拡張)
が出来たら `audio_tab5_play_wav_file(path)` を足す。

## 未決 / 次フェーズ

- `audio.play(pcm)` (JS からの任意 PCM 投入) は未実装。エンジンに
  TypedArray の生ポインタ取得 API が無く、Array 反復は遅い。Opus path
  は C 内完結なので P3 では不要。必要になったらエンジンに最小の
  TypedArray アクセサを足してから対応。
- writer task の affinity/priority は plan §5 のとおり同時動作計測
  (P4) で見直す。
- `audio_tab5_stop()` は HW を落とさない (再 start を速くするため)。
  省電力が必要になったら deinit を足す。
