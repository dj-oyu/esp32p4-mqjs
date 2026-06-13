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

## 未決 / 次フェーズ

- JS バインディング (`audio.*`) は意図的に未実装 — device_stdlib.c と
  生成ヘッダ (Windows は pregen) に触るため、Opus 側と衝突しやすい。
  P3 統合時に合わせて追加する。
- writer task の affinity/priority は plan §5 のとおり同時動作計測
  (P4) で見直す。
- `audio_tab5_stop()` は HW を落とさない (再 start を速くするため)。
  省電力が必要になったら deinit を足す。
