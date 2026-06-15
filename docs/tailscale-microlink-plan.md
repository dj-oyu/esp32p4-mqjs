# Tailscale 統合 (microlink) Phase 3 計画

[`system-settings-design.md`](system-settings-design.md) Phase 3 の実装方針。Tab5
(ESP32-P4 + esp_hosted の C6 で Wi-Fi、lwIP スタック) 上で、System Vault に保管した
Tailscale auth key を実消費して端末を tailnet ノードにする。

本書は 2026-06-15 時点の選択肢調査と採用判断、de-risking 手順を定める。実機検証は
未着手。

## 制約

- ESP32-P4 (RISC-V)。ネイティブ Wi-Fi は無く esp_hosted 経由 (C6, SDIO→SPI 構成)。
  上位は標準 lwIP netif として見える。
- ESP-IDF 6.0.1 (`ESP_IDF_VERSION=6.0` override)。
- Tab5 は外部 PSRAM 大容量あり。内蔵 SRAM は逼迫気味
  (カメラ稼働時計測で static 269KB / 空き ~101KB / 最大連続 31KB)。
- local-first 運用。コーディネーションサーバ (PC broker 等) は 24/7 常駐を前提に
  できない。
- 既存 Phase 1 で `tskey-auth-...` を System Vault (`ts_auth`, ≤255B) に保管し、
  C 読み出し API `system_vault_tailscale_read(dst, cap)` を用意済み。consumer は未実装。

## 選択肢調査 (2026-06-15)

| 方式 | 端末が tailnet ノード | tskey/vault 再利用 | 外部常駐サーバ | P4/IDF6 | 判定 |
|---|---|---|---|---|---|
| **MicroLink** ネイティブ C 実装 | なる | そのまま | 不要 | 未検証 | **採用** |
| esp32-tailbridge (WG→Linux proxy) | 複雑構成のみ | 不可 (WGキー) | 必要 (常時) | - | 却下 |
| esp_wireguard + Headscale | 自前管理 | 不可 (WGキー) | Headscale 必要 | P4 非対応 | 却下 |
| libtailscale / tsnet | 不可 | - | - | - | 却下 (Go必須) |

### 却下理由

- **esp32-tailbridge** ([pierrejay/esp32-tailbridge](https://github.com/pierrejay/esp32-tailbridge)):
  ESP32 は WireGuard クライアントとして Linux proxy (WireGuard+Tailscale 常駐) に
  繋ぐだけ。端末自体は tailnet ノードにならない (複雑な per-device namespace 構成を
  除く)。常時稼働の Linux box が必要で local-first に反し、auth key も WG キー方式で
  既存 vault 設計を再利用できない。
- **esp_wireguard** ([trombik/esp_wireguard](https://github.com/trombik/esp_wireguard) v0.9.0):
  対応ターゲットは esp32/s2/c3/esp8266 のみで **P4 非対応**。素の WireGuard で
  Tailscale コントロールプレーン (鍵交換/DERP/NAT越え) が無く、Headscale + 静的設定の
  自前運用になる。tskey-auth を使わないので Phase 1 設計と別物。
- **libtailscale / tsnet** ([tailscale/libtailscale](https://github.com/tailscale/libtailscale)):
  Go の userland netstack を C アーカイブ化したもの。Go ランタイム前提で MCU の素
  ESP-IDF では動かない。Tailscale 公式 FR (ESP32 #5220 / ESPHome #13561) も未対応で、
  コミュニティ C 実装が現実解。

## 採用: MicroLink 上流 (`CamM2325/microlink`)

[CamM2325/microlink](https://github.com/CamM2325/microlink) (MIT)。ts2021 プロトコルを
C でフル実装 (WireGuard 暗号 ChaCha20-Poly1305 / DERP relay / DISCO path discovery /
STUN)。端末が本物の tailnet ノードになる。**依存は上流を採用する** (フォーク
`dj-oyu/microlink` は未メンテのため依存しない)。

**重要: 同一ハード (Tab5 / ESP32-P4) での実績が prior-art として存在する。** ユーザーの
別プロジェクト [`dj-oyu/tab5-camera-viewer`](https://github.com/dj-oyu/tab5-camera-viewer)
が MicroLink を取り込み、**MJPEG ストリームを Tailscale 経由で 30fps 受信するところまで
実運用済み**。当時は Tab5 向け改修フォーク `dj-oyu/microlink` (未メンテ) を submodule に
していたが、上流はその後 v1.3.0→v2.1.0 と進み、当時フォークで当てていた改善
(symmetric NAT hole-punching、async/queue 化、mbedTLS 化) は上流に取り込まれている。
**よって上流 v2.x を採用し、フォーク／tab5-camera-viewer は「実証済みの統合テンプレートと
トラップ集」としてのみ参照する** (依存はしない)。上流に無い Tab5 固有パッチが見つかった
場合だけ、その差分を本プロジェクトへ取り込む。

### 適合点

- 認証が **`tskey-auth-...`** (config `ML_TAILSCALE_AUTH_KEY`)。vault `ts_auth` を
  `system_vault_tailscale_read()` で取り出してそのまま渡せる。Phase 1 設計が活きる。
- **netif 非依存** (任意 lwIP netif で動作。WiFi/PPP 実績)。P4 の esp_hosted も lwIP
  netif として見えるので適合見込み。
- Headscale/Ionscale 等の独自コーディネーションサーバ対応。local-first と相性可。
- API が素直で design doc の「adapter + lifecycle task + status API」にほぼ 1:1:
  ```c
  microlink_config_t cfg = { .auth_key = "...", .device_name = "...", ... };
  microlink_t *ml = microlink_init(&cfg);
  microlink_start(ml);
  uint32_t ip = microlink_get_vpn_ip(ml);        // 100.x.y.z
  microlink_state_t st = microlink_get_state(ml);
  bool up = microlink_is_connected(ml);
  // microlink_udp_* / microlink_tcp_* でソケット通信
  microlink_stop(ml); microlink_destroy(ml);
  ```
- 仕様メモ: 全 async / タスクベース、NVS peer キャッシュで再起動後即 DISCO 再開、
  Wi-Fi↔cellular のネットワーク rebind (~330ms)。

### リスク (採用前に潰す)

prior art により P4 とメモリは概ね解消。残る主リスクはビルド環境差分。

1. **ESP-IDF バージョン + ビルドシステム** (主リスク): tab5-camera-viewer は
   **PlatformIO の dual framework (arduino,espidf)** で、Arduino-ESP32 3.x 系 ≒ **IDF
   5.x** 上の実績。本機 mqjs は **純 ESP-IDF 6.0.1 + CMake + idf component manager**。
   上流 component を CMake ツリーへ載せ替え、IDF 6.0 でビルドを通すのが本丸。
   `ml_pio_config.h` 等の PlatformIO 専用設定を sdkconfig / CMake へ翻訳する。
2. **ESP32-P4 (RISC-V)**: **実機実証済み (resolved)**。同一 Tab5 で 30fps 達成。
3. **メモリ**: prior art で対処パターン確立済み。リニアバッファと MicroLink Coord
   Buffer (64KB) を **SPIRAM へ寄せて内蔵 SRAM を +94KB 解放**した実績あり
   (`SPIRAM_TRY_ALLOCATE`)。本機でも同方針。実空きは `heap_caps` で確認。
4. **NAT 越え / スループット**: symmetric NAT は直結 UDP 失敗 → DERP relay (低速)。
   prior art の計測では DERP 経由 7-17fps、STUN/DISCO で直結 hole-punch して 30fps。
   **本用途 (設定/制御トラフィック) では非問題**だが、DERP only だと体感遅延あり。

### 接続経路の方針 (ユーザー決定)

**直結 (STUN/DISCO hole-punching) を最優先、DERP リレーは直結不能時のフォールバックのみ**
(DERP は遅いので積極利用しない)。これは microlink/Tailscale の標準動作そのもの。
- 本番 config: `enable_stun = true` / `enable_disco = true` / `enable_derp = true` (全有効)。
  microlink は常に直結を試み、失敗時のみ DERP に落ちる。
- `enable_stun=false`/`disco=false` は **DERP 単体検証のテスト用トリック**であり本番設定ではない。
- 注力すべきは**直結成功率を上げて DERP に落ちにくくする**こと。罠: STUN/DISCO サーバ
  (`stun.l.google.com` / `derpN.tailscale.com`) への DNS/到達性 (prior art は DNS fallback IP を
  追加)。ユーザーのフォークが上流へ入れた adaptive symmetric NAT hole-punching も直結率改善が目的。
- 本用途は小さく低頻度なトラフィックなので、フォールバックで DERP (~300-600ms) に落ちても実用上可。

## 実績からの統合レシピ (tab5-camera-viewer `TailscaleTask.cpp`)

同一ハードで動いた実装をそのまま設計テンプレートにする。

### 初期化シーケンス (順序が重要)

1. Wi-Fi 接続待ち
2. **NTP 同期 (必須)**: `esp_sntp_*` で pool.ntp.org / time.google.com。
   理由コメント: *WireGuard の TAI64N timestamp は実時刻が必要。NTP 無しだと
   `gettimeofday()` が 1970 を返し、再起動後に peer の last-seen より古い timestamp に
   なって replay 攻撃として弾かれる*。→ Phase 1 で見つけた「SNTP 前は時刻未同期」問題と
   同根。**microlink_init より前に必ず NTP**。
3. `microlink_init(&config)`
4. `microlink_set_state_callback(ml, onStateChange, NULL)`
5. `microlink_start(ml)` — 以降フル async (ポーリングループ不要)

### config 構造体 (v2.x)

```c
microlink_config_t config = {
    .auth_key = <vault の ts_auth>,        // tskey-auth-...
    .device_name = "m5stack-tab5",
    .enable_derp = true,
    .enable_stun = true,
    .enable_disco = true,
    .max_peers = 4,
    .priority_peer_ip = microlink_parse_ip(<優先 peer>),
};
```

### 状態・診断 API

- `microlink_get_state(ml)` / `microlink_is_connected(ml)` (ポーリング)
- state callback `onStateChange` → `ML_STATE_CONNECTED`
- `microlink_get_vpn_ip(ml)` + `microlink_ip_to_str(ip, buf16)` → "100.x.y.z"
- `microlink_get_peer_count(ml)` / `microlink_get_peer_info(ml, i, &peer)` (`peer.online`)

### 確定済みトラップ (commit ログより)

- **暗号は mbedTLS 必須**: 純 C リファレンス (`WIREGUARD_CRYPTO_REFC=1`) は **Core 1 で
  スタックオーバーフロー (Guru Meditation)**。mbedTLS の ChaCha20-Poly1305 へ切替。v2.x の
  Noise handshake も mbedTLS ChaCha20-Poly1305 を要求。→ sdkconfig で mbedTLS ChaCha/Poly
  を有効化。
- **スタックサイズ**: WG 復号は tcpip コンテキストで走るため `tcpip_thread` を
  3072→16384。`ML_TASK_WG_MGR_STACK` 8→16KB。Tailscale ライフサイクルタスクも 16KB。
  `wireguardif` は `ip_input()` ではなく **`tcpip_input()`** を使う (32KB スタックを
  食い潰さないため)。
- **SDIO 非互換メモ**: PlatformIO 側で "Hybrid Compile 無効化 (SDIO ドライバ非互換)"。
  本機は esp_hosted SDIO→SPI 構成なので、esp_hosted の netif と MicroLink の UDP 出力経路の
  噛み合わせを実機で要確認。
- **DERP packet injection はスレッド安全に**: `tcpip_try_callback()` 経由 (src_ip=0 で
  渡さないと WG が peer endpoint を到達不能 VPN IP に更新して経路破壊)。

## de-risking スパイク (機能実装の前に)

起点は **上流 `CamM2325/microlink` v2.x**。主目的は IDF 6.0 + CMake への載せ替え。
tab5-camera-viewer の設定値・トラップを参照しながら進める。

1. 上流 `CamM2325/microlink` (v2.x) を `components/microlink` に追加 (idf component
   manager の managed component か git submodule)。**IDF 6.0 + P4 target で素ビルドが
   通るか**確認。PlatformIO 専用設定 (`ml_pio_config.h`, `lib_extra_dirs`,
   `library.json`) を CMake / sdkconfig へ翻訳。mbedTLS ChaCha20-Poly1305 を sdkconfig で
   有効化。**上流に無い Tab5 固有挙動が出たら tab5-camera-viewer の差分を参照して当てる。**
2. tcpip_thread / WG mgr / ライフサイクルタスクのスタックを prior art の値に設定
   (16KB 系)。`wireguardif` の `tcpip_input()` 経路を確認。
3. 最小 `app_main` から、**NTP 同期 → ハードコード auth key** で `microlink_init` →
   `microlink_set_state_callback` → `microlink_start` → `ML_STATE_CONNECTED` /
   `microlink_get_vpn_ip` まで、実機で一度 tailnet に繋ぐ。
4. `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` で内蔵 SRAM 実空きを測り、バッファの
   `MALLOC_CAP_SPIRAM` backing を確認。

上記が通って初めて本実装へ進む。**TRAP: フォークの managed-component kconfig が
fresh-worktree 初回 configure で落ちる既往 (esp_hosted SDIO→SPI) があるため、flash 前に
`CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE` 等を verify する。**

## de-risking スパイク 実行結果 (2026-06-15)

隔離した最小 esp32p4 プロジェクト (`C:\esp-build\mlspike-proj`、本体ファームと分離) で
上流 `CamM2325/microlink` (HEAD 2026-03-17) を IDF 6.0.1 / riscv GCC 15.2.0 でビルドし、
**IDF 6.0 移植ポイントを 3 件特定**した。configure は通過、コンパイル段階で 2 件の壁。

### 結論

**上流 microlink を IDF 6.0.1 + ESP32-P4 でビルド成功 (グリーン)。** 下記の移植差分で
`mlspike.bin` 生成・microlink 全 18 ソース compile/link OK・`error:` 0 件を確認
(2026-06-15)。当初の主障壁 mbedTLS 4 (PSA) は **PSA API へ移植して解決済み**。
移植は有界 (4 系統の差分) で、dead end ではないことが実証された。

### 移植ポイント

1. **cJSON が IDF6 コアから削除 (EASY・対応済み)**: `REQUIRES json` が解決不能。
   `espressif/cjson` を managed 依存に追加 (component 名は `cjson`)、microlink CMakeLists の
   `REQUIRES json` → `cjson` に変更。`#include "cJSON.h"` は managed cjson の INCLUDE_DIRS
   が `cJSON/` なのでそのまま通る。

2. **GCC 15 の新エラー (EASY)**: `-Werror=unterminated-string-initialization`。
   `wireguard_lwip/src/wireguard.c` と `microlink/src/nacl_box.c` の固定長
   `unsigned char` 配列を文字列リテラルで初期化している箇所 (Noise construction 文字列等、
   NUL を入れない意図) で発生。`wireguard_lwip` と `microlink` の両コンポーネントに
   `-Wno-error=unterminated-string-initialization` を付与して回避 (本リポジトリが wolfSSL に
   `-std=gnu17 -Wno-error=maybe-uninitialized` を当てているのと同じパターン)。

3. **mbedTLS 4.0 (PSA) で legacy crypto API 削除 (主障壁・PSA移植で解決済み)**:
   IDF 6.0 は **Mbed TLS 4.0.0 (PSA / TF-PSA-Crypto)** を同梱し、microlink が使う
   `mbedtls/entropy.h` `ctr_drbg.h` `chachapoly.h` `chacha20.h` 等の legacy ヘッダを削除した
   (sshc が mbedTLS を避け wolfCrypt を選んだのと同根)。利用面は 5 ファイル
   (`ml_noise.c` `ml_coord.c` `ml_derp.c` `ml_wg_mgr.c` `microlink_internal.h`) で、用途は 2 つ:
   - **RNG**: `mbedtls_entropy_*` + `mbedtls_ctr_drbg_*` (TLS の `conf_rng` と Noise 鍵/nonce)。
     mbedTLS 4 では RNG は PSA 大域。→ TLS は `psa_crypto_init()` のみで PSA RNG が効く。
     Noise 側は `esp_fill_random()` か `psa_generate_random()` に置換。
   - **Noise AEAD**: `mbedtls_chachapoly_*` (ts2021 Noise ハンドシェイクの ChaCha20-Poly1305)。
     → PSA `psa_aead_encrypt/decrypt` (`PSA_ALG_CHACHA20_POLY1305` は IDF6 で利用可) か、
     **in-tree の refc `chacha20poly1305.c`** (wireguard_lwip 同梱、本リポジトリの
     `components/tweetnacl` 隣にもある) で置換。ハンドシェイクは低頻度なので refc で十分。

   **重要 (誤解しやすい点)**: TLS プロトコル層 (`mbedtls/ssl.h` `net_sockets.h` `base64.h`
   `error.h`) は mbedTLS 4 に**残存**しており、`mbedtls_ssl_*` は使い続けられる。消えたのは
   低レベル crypto モジュールだけ。よって「TLS クライアント全面書き換え」ではなく
   「RNG 配線と AEAD 呼び出しの差し替え」で済む。

   - `esp_wifi`: 本体 main が既に `espressif/esp_wifi_remote` 依存のため P4 で解決可 (確認済み)。

   **採用方針 (ユーザー決定): PSA API へ全面移植。実装してグリーン確認済み。** 差分:
   - `microlink_internal.h`: `mbedtls/entropy.h` `ctr_drbg.h` → `psa/crypto.h`。DERP 構造体の
     `mbedtls_entropy_context` / `mbedtls_ctr_drbg_context` フィールド削除。
   - `ml_noise.c`: ChaCha20-Poly1305 を `mbedtls_chachapoly_*` → PSA one-shot
     `psa_aead_encrypt`/`psa_aead_decrypt` (`PSA_KEY_TYPE_CHACHA20` +
     `PSA_ALG_CHACHA20_POLY1305`、毎回 import_key→destroy_key、`psa_crypto_init()` 冪等呼び)。
     PSA の出力は ct‖tag 連続で旧レイアウトと一致するため temp バッファ不要。
   - `ml_derp.c`: TLS の `mbedtls_entropy_*`/`mbedtls_ctr_drbg_*`/`mbedtls_ssl_conf_rng()`
     (mbedTLS4 で削除) を撤去 → TLS setup 前に `psa_crypto_init()` のみ (RNG は PSA 大域)。
   - **runtime 注意 (未検証)**: PSA で ChaCha20-Poly1305 を有効にする sdkconfig
     (`CONFIG_MBEDTLS_CHACHAPOLY_C` 等 / `PSA_WANT_ALG_CHACHA20_POLY1305`) が必要。compile は
     通るが、無効だと `psa_aead_*` が実機で `PSA_ERROR_NOT_SUPPORTED`。実機段階で要確認。

4. **IDF 6.0 の driver 分割 (EASY)**: `driver/uart.h` `driver/gpio.h` が見つからない
   (cellular モデムの PPP/AT 用、`ml_cellular.c` `ml_at_socket.c`)。microlink の `REQUIRES` に
   `esp_driver_uart` `esp_driver_gpio` を追加して解決。**本実装では cellular 不要なので、これら
   2 ソースをビルドから除外して driver 依存ごと落とす案も可** (ただし `ml_net_switch.c` /
   `microlink.c` からの参照有無を要確認)。

### 検証済み移植レシピ (本体 repo へ vendor する時の手順)

1. `espressif/cjson` を依存に追加 (本体 `main/idf_component.yml` か microlink の manifest)。
2. microlink `CMakeLists.txt`: `REQUIRES` の `json`→`cjson`、`esp_driver_uart`
   `esp_driver_gpio` を追加、`target_compile_options(...
   -Wno-error=unterminated-string-initialization -Wno-error=stringop-truncation)`。
3. `wireguard_lwip CMakeLists.txt`: `-Wno-error=unterminated-string-initialization`。
4. mbedTLS4 PSA 移植 (上記 finding 3 の 3 ファイル差分)。
5. sdkconfig: PSA ChaCha20-Poly1305 有効化を確認。
6. component 配置: `components/microlink` と `components/wireguard_lwip` を**フラットな兄弟**として
   置く (上流の nested `components/` と symlink は Windows で問題。staging で flatten 済みのものを使う)。

### vendor → フルビルド: **GREEN (2026-06-15)**

`components/` へ vendor して本体 firmware (tab5) をビルド → **成功**
(`esp32p4-mqjs.bin` 3.9MB、microlink 全 18 ソース compile、`error:` 0 件)。
- 追加ポート: プロジェクトの厳格 warning 設定下で `-Werror=stringop-truncation`
  (`ml_peer_nvs.c`/`ml_wg_mgr.c` の hostname `strncpy`) が発火 → microlink に
  `-Wno-error=stringop-truncation` を追加 (隔離スパイクは非厳格で出なかった)。レシピ反映済み。
- **ハマった env トラップ (microlink 無関係・既知)**: ビルドが `CONFIG_WIFI_RMT_*` undeclared で
  `main/wifi.c` で落ち続けた。真因は [[esp32p4-build-flash-workflow]] 記載の **EIM の
  `ESP_IDF_VERSION` ミスマッチ**: activation profile が `6.0.1` を設定するが esp_wifi_remote の
  Kconfig は `orsource Kconfig.idf_v$ESP_IDF_VERSION.in` で `6.0` を期待 → silently skip →
  SLAVE_IDF_TARGET/WIFI_RMT_* が消え esp_hosted が H2 にフォールバック。**対処: profile source 後・
  idf.py 前に `$env:ESP_IDF_VERSION='6.0'`、かつ壊れた env で configure 済みの古い build dir は
  捨てて fresh build dir で configure**。これでグリーン。microlink 追加は reconfigure を強制する
  ため、必ず fresh dir + この env で実施する。

### スパイクで作った隔離環境 (再現用)

- 上流 clone: `C:\esp-build\microlink-src`、flatten 済み component: `C:\esp-build\mlcomps`
  (`microlink` と `wireguard_lwip`、symlink/nested を除去)
- 最小プロジェクト: `C:\esp-build\mlspike-proj` (EXTRA_COMPONENT_DIRS=mlcomps、
  main は `REQUIRES microlink` のみ、sdkconfig は esp32p4 + P4 rev + mbedTLS 設定)
- ビルド: IDF 6.0.1 activation → `idf.py -C <proj> set-target esp32p4 build`

### 次のステップ

ビルド可能性 (compile/link) は実証済み。本実装の de-risking は完了。残るは:
- 検証済みレシピで microlink を**本体 repo に vendor** し、フルファーム
  (`idf.py -B build_tab5 -DSDKCONFIG=sdkconfig.tab5 ...`) でビルドを通す。
- 最小 `app_main` から NTP → ハードコード auth key で**実機接続** (compile≠動作。PSA AEAD の
  runtime 有効化、esp_hosted netif と WG UDP 出力の噛み合わせ、スタックサイズはここで確認)。
- 通れば本実装 (vault 連携 lifecycle task、`tailscaleStatus()` 拡張) へ。

## 本実装 設計 (ユーザー確定 2026-06-15)

### 始動: Wi-Fi got-IP コールバックチェーン (ポーリング禁止)

prior-art の `while(WiFi.status()...)` ポーリングは使わない。この repo の非ブロッキング
`wifi_start(on_got_ip)` に連結する:

```
wifi_start(on_net_up)  [app_main.c]
  → IP_EVENT_STA_GOT_IP → on_net_up()  [既存: task_source_start + mqjs_notify_net_up]
      → tailscale_adapter_on_net_up()
          if (vault has key && enabled):
            状態=connecting/"時刻同期中"; SNTP 開始 + time-sync 通知 cb 登録
          → (NTP synced cb) tailscale_adapter_on_time_synced()
              vault_tailscale_read(buf) → microlink_init(cfg) →
              microlink_set_state_callback → microlink_start → buf ゼロクリア
              → (state cb) リトライ計数・状態写像・ウォッチドッグ
```
NTP は TAI64N のため microlink より前に必須。全段コールバックで非ブロッキング。

### 状態モデル (`tailscaleStatus().state` + `detail`)

| state | 条件 | detail 例 |
|---|---|---|
| `not-configured` | vault に key 無し | 未設定 |
| `disabled` | key 有り・ユーザー OFF | オフ |
| `connecting` | 接続試行中 | 接続中… 試行 N回 / 登録中 / 時刻同期中 |
| `connected` | 到達 | 接続済み 100.x.y.z / peers M |
| `error` | 試行上限で失敗 | 接続失敗（MAX回試行）auth key を確認 |

`tailscaleStatus()` の返却: `{configured, enabled, state, detail, retries, ip, peers}`
(秘密は一切含めない)。

### リトライ制御 (無限ループ禁止)

microlink は内部で自動再接続 (~5-10s) し、`ERROR`/`RECONNECTING` は auth 拒否と一時障害を
区別しない → **我々が境界を課す**:
- 初回接続: state cb が `RECONNECTING`/再 `CONNECTING` に入るたび `retries++` を**ライブ表示**。
- **`retries >= MAX (=5)` で `microlink_stop/destroy`** して自動再接続ごと停止 → `error`。
  自動リトライしない。再アームは明示操作 (key 入れ直し or 再試行ボタン) のみ。
- 一度 `connected` 到達後は retries リセットし、microlink 内部の自動再接続 (一時切断回復) を許可。

### オン/オフ (簡単に)

- vault に `enabled` フラグ永続化 (`system_vault_tailscale_enabled/_set_enabled`)。
- JS `system.tailscaleEnable()` / `tailscaleDisable()`。OFF → `microlink_stop/destroy`
  (key は保持) → `disabled`。
- **boot 時: key 有り かつ enabled の時だけ自動起動** (enabled 既定 = ON、key 設定時に繋ぎたい)。
- device_settings の Tailscale ページにトグルボタン + ライブ状況表示 (タイマ更新)。

### logout / re-auth

`tailscaleForget()` = `microlink_stop/destroy` + vault key 削除 + enabled 削除 →
`not-configured`。key 差し替え (`tailscaleSet`) は再 init。

### 実装単位

1. `system_vault` に `enabled` フラグ (+ host テスト) — **自己完結・先行実装**
2. `microlink_adapter.{c,h}` (上記チェーン + リトライ + 状態 struct、auth key ゼロクリア)
3. JS バインディング: `tailscaleStatus` 拡張 + `tailscaleEnable/Disable`、`tailscaleForget`
   更新 (ROM 再生成が要る — [[p4-multiapp-design]] の JS_NewString ネスト禁止等の罠注意)
4. `device_settings.js` UI: トグル + 状況表示
5. `on_net_up()` に `tailscale_adapter_on_net_up()` を連結

### 将来タスク (Phase 3 スコープ外)

- **Secure NVS**: auth key は現状 **平文 NVS**。製品 build で Flash Encryption + NVS
  Encryption (保存時暗号化)、Key Manager + HUK によるハード束縛鍵、Secure Boot を入れて
  固める。P4 の「拡張命令 (PIE/SIMD)」は演算用で**この用途には無関係**。
  cf. [`system-settings-design.md`](system-settings-design.md) の System Vault 不変条件。

## 検証基準 (Phase 3)

- 設定済み端末が再起動後に自動で tailnet に接続し、`tailscaleStatus()` が
  `connected` と Tailscale IP を返す。
- 秘密値 (auth key) が JS / status / ログに戻らない (Phase 1 不変条件を継続)。
- `tailscaleForget()` 後にセッションが切れ、再接続しない。
- 内蔵 SRAM が破綻しない (カメラ等の既存機能と同時稼働で OOM しない)。
- firmware build (IDF 6.0 / P4) と既存 host tests が通る。

## テスト計画

方針: 本リポジトリの「新プリミティブはまずホスト単体テスト → 最小デバイススモーク →
統合」([[test-new-modules-in-isolation]]) に従う。今回は crypto を PSA へ書き換えたため
**最大リスク=PSA AEAD 移植の正しさ**を最優先。

### レイヤ0: ビルド検証 (ゲート)
- 隔離 compile スパイク (IDF6/P4) — **済 (グリーン)**
- フル firmware ビルド (tab5, microlink vendored) — **済 (グリーン、`ESP_IDF_VERSION=6.0`
  + fresh build dir で解決)**
- Stamp ビルド回帰 (microlink が gc-drop され無影響) — 未

### レイヤ1: ホスト単体テスト (実機不要・最重要) — **DONE (19/19 pass)**
**ChaCha20-Poly1305 AEAD KAT** — PSA 移植の正しさ。オラクルは Python `cryptography`
(48.0.0, RFC 8439 ChaCha20Poly1305, 12byte nonce)。`tools/test_chacha20poly1305_kat.py`
(`python tools/test_chacha20poly1305_kat.py`、19/19 pass・golden ベクタ出力):
- 公式 RFC 8439 §2.8.2 ベクタでオラクル健全性を確認
- **Tailscale nonce 構築** (4byte 0 + 8byte **big-endian** counter) の検証
  (ml_noise.c のバイト順ロジックと一致するか)。WireGuard refc は LE なので不適、注意
- encrypt→decrypt ラウンドトリップ + tag 改ざんで decrypt 失敗 (auth 検出)
- **ct‖tag 連続レイアウト**の確認 (PSA 移植のバッファ仮定の根拠)
- golden ベクタ (key/counter/ad/pt → ct‖tag) を出力 → レイヤ2 のデバイステストが
  microlink の `chacha20poly1305_encrypt` 出力をこの golden と突き合わせる
- 限界: PSA の実呼び出し等価性は PSA ランタイムが要るためレイヤ2 (実機) で検証。本テストは
  アルゴリズム/nonce/レイアウトという **PSA 移植が依存する契約**を固める

### レイヤ2: デバイス最小スモーク (device-connect スパイク)
PSA runtime 有効 (`psa_aead_*` が `NOT_SUPPORTED` でない) / WiFi→NTP→`microlink_init`→
`microlink_start`→`ML_STATE_CONNECTED` / VPN IP 取得 / heap (内蔵SRAM・SPIRAM backing・OOM無) /
スタック high-water (prior-art の罠) / **golden ベクタとの AEAD 突合**。

### レイヤ3: 接続経路 (方針検証)
直結優先 (STUN/DISCO で DIRECT 到達・低RTT) / DERP フォールバック単体 (`enable_stun/disco=false`
で強制) / 昇格 (DERP→直結)。罠: STUN/DERP サーバの DNS 到達性。

### レイヤ4: E2E 機能
PC (tailnet) ↔ device UDP/TCP echo (port 9000)、双方向、直結/DERP の RTT 記録。

### レイヤ5: システム統合 (Phase 3 本実装)
vault→`microlink_init` (使用後ゼロクリア) / `tailscaleStatus().state` 拡張
(connecting/connected/IP/秘密なし lastError) / `tailscaleForget()`→`microlink_stop/destroy` /
秘密衛生 (log/status/JS に auth key が出ない) / 再起動→自動再接続 (NVS peer cache)。

### レイヤ6: 頑健性 / 共存
無効/期限切れ key→error・クラッシュ無 / WiFi 切断再接続 (network rebind ~330ms) /
NTP 不可→graceful fail / camera・SSH・MQTT と同時稼働で WDT・資源枯渇無。

### 自動化の切り分け
- 自動 (ホスト): レイヤ1 KAT、既存 host tests
- 半自動 (デバイス): MQTT devreport で heap/state/peer/RTT 吸い上げ (COM8 直開けより推奨)
- 手動: PC peer との E2E、NAT 経路実地

## 参考

- [CamM2325/microlink](https://github.com/CamM2325/microlink) — **採用 (上流本体, MIT, v2.x)**
- [dj-oyu/tab5-camera-viewer](https://github.com/dj-oyu/tab5-camera-viewer) — 同一ハードでの
  実運用 prior art (参照のみ)。`lib/AppLogic/TailscaleTask.cpp` が統合テンプレート、
  `sdkconfig.defaults` / `env_loader.py` にスタック・TCP 設定、
  `research/derp-throughput-analysis.md` に DERP 計測
- [dj-oyu/microlink](https://github.com/dj-oyu/microlink) — 当時のフォーク (**未メンテ。依存
  しない**。Tab5 固有差分が必要な時だけ参照)
- [MicroLink (OSRTOS)](https://osrtos.com/projects/microlink-tailscale-vpn-for-esp32/)
- [Csontikka/esphome-tailscale](https://github.com/Csontikka/esphome-tailscale) — MicroLink ラッパー実装例
- [alfs/tailscale-iot](https://github.com/alfs/tailscale-iot) — 別アプローチ (vendored noise-c)
- [pierrejay/esp32-tailbridge](https://github.com/pierrejay/esp32-tailbridge) — 却下した proxy 方式
- [trombik/esp_wireguard](https://github.com/trombik/esp_wireguard) — 却下した素 WireGuard
- [tailscale/libtailscale](https://github.com/tailscale/libtailscale) / [tsnet docs](https://tailscale.com/kb/1244/tsnet)
- Tailscale FR: [ESP32 library #5220](https://github.com/tailscale/tailscale/issues/5220),
  [ESPHome #13561](https://github.com/tailscale/tailscale/issues/13561)
