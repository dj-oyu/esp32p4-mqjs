# tools/

3 段構えです。

- **`tools/` 直下** = 日々使う道具。鍵、配信、辞書・フォント生成、シリアル捕捉。
- **[`probes/`](probes/)** = 実機 dev スロットへ push して MQTT で結果を読む
  使い捨ての計測スクリプト。**それぞれ特定のキャンペーン専用**で、汎用テスト
  ではない。
- **[`tests/`](tests/)** = ホストで走る自動テスト。実機もシリアルも要らない。

`tools/agents/skills/` は AI エージェント向けスキルの正本で、
`sync-claude-skills.sh` が `.claude/skills/` へ複製します（後者は手編集しない）。

## 日々使う道具

| ツール | 用途 |
|---|---|
| `mqjs_keygen.py` | タスク署名用の Ed25519 鍵ペアを作る。秘密鍵→`task_signing_key.pem` (gitignore)、公開鍵→`main/task_pubkey.h` |
| `mqjs_push.py` | JS を署名して MQTT で配信。`--shelf` でストアへ、`--delete` で tombstone |
| `mqjs_webui.py` | ブラウザから push するためのローカル Web UI (127.0.0.1 のみ) |
| `mqtt_pub.py` | 依存なしの MQTT 3.1.1 publisher (QoS 0)。署名の要らない生 publish 用 |
| `shelve_all.sh` | `examples/*.js` のうち `// @app` を持つもの全部をストアへ並べる |
| `mqjs_provision_qr.py` | MQJSP1 プロビジョニング QR の生成と検査 (検査は秘匿値をマスク) |
| `skk_prep.py` | SKK-JISYO を skk_core 用に変換・再ソート・索引付け・検証。`inspect` で中身を引ける |
| `font_term_mono.py` | `components/ui_tab5/fonts/font_term_mono.c` を再生成 |
| `ndl_bridge.py` | 国会図書館 Search API のブリッジ (`reading.js` の ISBN 照会をこの PC へ委譲) |
| `qr_read_bench.py` | 合成 QR の可読性ベンチ (光学マージンとホストデコーダ時間。実機 CPU 時間は予測しない) |
| `capture_com8.py` | DTR/RTS を触らない受動シリアル捕捉。動いているアプリを再起動させずに覗ける |
| `cap_boot.py` | ポートの再出現を待ってブート窓を捕らえる |
| `smoke_examples.sh` | `run_pc` で examples を数秒ずつ走らせ、例外が出たら落とす |
| `sync-claude-skills.sh` | `agents/skills/` → `.claude/skills/` を再生成 |
| `dev_idle.js` | dev スロットの常駐スタブ。毎ブート devreport を 1 行出して黙る。**probe の後はこれに戻す** |

## probes/ — 実機計測スクリプト

使い方はどれも同じです。`mqjs_push.py` で dev スロットへ送り、
`<base>/proberep` を購読して結果を読み、**終わったら `dev_idle.js` に戻す**。
書かれた数値は当時の実機のもので、再実行すれば変わります。

| probe | 何を測ったか |
|---|---|
| `probe_sram.js` | 実負荷下の SRAM テレメトリ。idle → WireGuard → カメラと負荷段階を歩く |
| `probe_frag.js` | WireGuard 往復を繰り返すと内部フラグメンテーションが**累積するか**。6 ラウンド約 15 分 |
| `probe_wg.js` | tailnet ホストへの WireGuard 経路負荷。1 fetch ごとの実時間と L2 の増減 |
| `probe_arena_bench.js` | PSRAM アリーナ税。同じスクリプトを 2 ビルド (PSRAM / L2 SRAM) で走らせて比較する。**アリーナサイズは揃えること** |
| `probe_scan_once.js` | バーコードスキャン 1 回だけ。前後の L2 を見て「初回スキャンが固定する block」を名指しする |
| `probe_qr_repro.js` | QR スキャン終了コールバックのハング再現 (2026-07-29)。単独 `scanQr` と `scan`→`scanQr` 連鎖の 2 本立て |
| `probe_cam.js` | `camera.scan` パイプラインの疎通 (init+capture+イベント配送) |
| `probe_audio.js` | 音声再生パスの疎通。tone と WAV をまたいで `audio.stats()` を報告 |
| `probe_http.js` | `http.get` の疎通。実 HTTPS GET |
| `probe_skk_dict.js` | `skk.open()` がどの辞書を選び、引くのに何 µs かかるか (ホストテストの実機側半分) |
| `probe_skk_learn.js` | 個人辞書が**再起動をまたぐか** (S7)。push → 再起動 → もう一度 push の 3 手 |
| `probe_store_apps.js` | ストアカタログからの install → launch |
| `probe_run_apps.js` | user worker を空けて installed app を順に起動し、生存を確認して戻す |

## tests/ — ホストテスト

`run_pc` を使うものは、先に README の「PC だけでスクリプトを試す」で
`run_pc` を作ってください。

| テスト | 対象 |
|---|---|
| `test_skk_kana.c` | ローマ字→かな変換と SKK ステートマシン |
| `test_skk_dict.c` | 辞書検索。全辞書の全見出し + 各見出しの「必ず存在しない版」を突く |
| `test_skk_e2e.c` | 実辞書 image を通したステートマシンの端から端まで |
| `ssh_vt_imetest.sh` (+ `.js.inc`) | `ssh_vt.js` の IME キー経路。`@imetest-inject` マーカーへ台本を注入して `run_pc` で走らせる (出荷アプリにテストのバイトは乗らない) |
| `test_ssh_vt_security.sh` | `run_pc` をビルドして下 2 つを走らせる入口 |
| `test_ssh_vt_security.py` | 悪意ある SSH サーバが送れるエスケープ列に端末アプリが壊されないか |
| `test_vault_isolation.py` | vault が不変な app identity で隔離されているか |
| `test_provision_parse.mjs` | `device_settings.js` の MQJSP1 パーサ／バリデータ (Python 生成の wire text と突き合わせ) |
| `test_provision_qr.py` | MQJSP1 wire format そのもの |
| `test_chacha20poly1305_kat.py` | microlink の ChaCha20-Poly1305 PSA 移植 KAT。`cryptography` を信頼オラクルにする |
| `test_opus_comb.c` / `test_opus_denorm.c` / `test_opus_bfly3.c` / `test_opus_normres.c` | Opus PIE カーネルの portable C reference を独立 int64 golden とビット一致で照合 |
| `test_opus_p4_kernels.c` | カーネル dispatch が C reference と一致するか |
| `test_phase1.js` (+ `_peer.js`) | 名前ベース app API (`sys.start/open/focus/stop`) の `run_pc` スモーク |
