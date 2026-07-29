# 設計文書

2 段構えです。

- **`docs/` 直下** = 現役のリファレンス。いま動いているものの構造と、その形に
  した理由。実装を変えるときは、まずここを読んで、変えたらここを直す。
- **[`docs/history/`](history/)** = 決着済みキャンペーンの記録。実測値・採否の
  判断・検死記録が入っている。読むのは「なぜこの形なのか」「なぜあの案を
  採らなかったのか」を知りたいときだけでよく、**更新はしない**。

一次情報は [`../README.md`](../README.md)（使い方・ビルド）と
[`../examples/README.md`](../examples/README.md)（JS API とランタイム制約）です。

## 現役リファレンス

| 文書 | 何を決めているか |
|---|---|
| [launcher-multiapp-design.md](launcher-multiapp-design.md) | マルチアプリ実行モデルと MQTT App Store。1 本の `js_task` が複数 context を協調実行する前提 |
| [app-manager-migration.md](app-manager-migration.md) | 名前ベースの app API (`sys.start/open/focus/stop`)、App record と Worker の分離、policy profile、LRU eviction。Phase 0-4 実装済・実機 E2E 済 |
| [widget-framework-design.md](widget-framework-design.md) | mqjs app の UI。Widget (LVGL) と Canvas (`ui.cells` ほか) の使い分け、ライフサイクル |
| [tab5-ui-design.md](tab5-ui-design.md) | Tab5 UI の 4 層 (StatusBar / Console / Canvas / Widget) と描画キューの制約 |
| [ssh-terminal-design.md](ssh-terminal-design.md) | wolfSSH client (C) と terminal emulator (`examples/ssh_vt.js`) の責務分割 |
| [system-settings-design.md](system-settings-design.md) | System Vault、デバイス設定、プロビジョニング QR の wire format |
| [skk-ime-design.md](skk-ime-design.md) | SKK 日本語入力。`skk_core` (C) と mqjs フロントエンド、辞書 image の形式 |
| [audio-pipeline.md](audio-pipeline.md) | ES8388 + I2S + PCM ring のスピーカー再生パス |
| [power-states.md](power-states.md) | 4 ステート電源モデル。P0 (画面) は実装済、SUSPEND 以降が生きた計画 |
| [mquickjs-patches.md](mquickjs-patches.md) | vendored mquickjs (upstream `203d5bb`) に載せたローカルパッチの全量 |
| [ui-overlay-plan.md](ui-overlay-plan.md) | `ui.overlay` / `ui.cursor` — 浮かせるものを C 側へ寄せる。**未着手の設計** |

## history — 決着済みの記録

いずれも「やった・測った・こう決めた」で閉じています。再開する前に、まず
**やらないと決めた理由**が書いてあるかを確認してください。

| 記録 | 何が決着したか |
|---|---|
| [scanline-opt-plan.md](history/scanline-opt-plan.md) | バーコード走査の高速化。S1+S1.5 採用 (デコード成立コホート中央値 9→4 ms/f)、**S2 棄却**(検死記録あり)、S4 は寝かせ |
| [pie-tensor-asm-plan.md](history/pie-tensor-asm-plan.md) | `bc_locate` 構造テンソルの PIE 手書き融合カーネル。loc 13-15→6-7 ms/f、実機 PASS。QACC ではなく XACC・hwlp end ラベルは inclusive |
| [qr-read-performance.md](history/qr-read-performance.md) | QR 密度と光学条件の成立範囲。**本書の 400×300 解析面は後に棄却**され、640×640 native crop に着地 (経緯は camera-lifecycle §6)。付属コーパスは [qr-test/](history/qr-test/) |
| [camera-lifecycle-plan.md](history/camera-lifecycle-plan.md) | カメラの単一オーナータスク化と microlink との相互排他。**Phase 1 の設計は現行実装そのもの**。§8 の internal-SRAM quirc は実機で不可能と確定 |
| [tailscale-microlink-plan.md](history/tailscale-microlink-plan.md) | vendored microlink による Tailscale 統合。選択肢調査と、実機で潰した 4 つの根本原因 (SDIO DMA-OOM / NTP / tcpip_thread 自己デッドロック / DERP region) |
| [opus-decoder-plan.md](history/opus-decoder-plan.md) | float Opus デコーダと CELT カーネルの PIE 化。comb+denorm 採用 (-17.8%)、他は overhead 律速。§14 に PIE の適用可否の**訂正**あり |
| [audio-device-design.md](history/audio-device-design.md) | audio capability token と overdub mixer。P1+P3 実装済、P2 preempt は未 |
| [audio-tab5-status.md](history/audio-tab5-status.md) | `audio_tab5` の実装経緯と検証ログ (設計本体は [audio-pipeline.md](audio-pipeline.md)) |
| [runtime-hotspot-audit.md](history/runtime-hotspot-audit.md) | mqjs ランタイムの CPU ホットパス監査。**修正は未着手**のまま — 候補リストとして残す |
