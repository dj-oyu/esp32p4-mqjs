# Tab5 SSH ターミナル 設計書

Status: **Phase T3 完了・実機確認済み** (2026-06-11)。SSH ターミナルが実用速度で Tab5 で動作。
- T1 (素朴な行端末): 実機 SSH ログイン成功。
- T2 (VT100 パーサ+固定グリッド): SELFTEST の grid が PC 基準と完全一致、REPORT デモ目視確認。
- T3 (描画パイプライン再設計 + 等幅フォント + pty 動的リサイズ + 画面導出ジオメトリ):
  等幅セル描画 (HackGen 9x24、グリフ直接ブリット、バッファスクロール) で**実機が軽快に**。
  80x33 ジオメトリは画面から導出 (向き非依存)、pty も wolfSSH_ChangeTerminalSize で追従。
  **実機 SSH で `whoami`/`date` 実行・出力をユーザー確認** (2026-06-11)。
  タッチ検出リトライで暗転 (パネル誤判定) も解消。
- 残: 実機で `stty size`=33 と TUI (vim/htop) の最終目視は任意の追加確認。
対象: M5Stack Tab5 (ESP32-P4 + C6 WiFi)。ユーザー目標「JS でターミナルエミュレータ」の本命。

## 0. ゴール / 非ゴール

**ゴール**

- Tab5 が**本物の SSH クライアント**になり、LAN 上のサーバのシェルに繋がる
- JS だけでターミナル UI を実装できる (グリッド描画・キーボード入力・VT 解釈は JS、
  暗号と通信は C の `ssh.*`)
- Tab5 UI Phase 4a (`ui.keyboard` / `ui.onKey` / `ui.textSize`) の上に乗る
- Stamp-P4 ビルドへの影響ゼロ (SSH は `CONFIG_MQJS_SSH` オプトイン)

**非ゴール (v1)**

- 公開鍵認証 (v1 はパスワードのみ。鍵は後続)
- ホスト鍵の検証・TOFU 保存 (v1 は accept-any + fingerprint をログ)
- 複数同時セッション (1 本のみ)
- 完全な VT100/xterm エミュレーション (JS 側で段階的に。T1 は印字 + 改行 + BS)

## 1. 技術スタック (確定)

```
JS (examples/ssh_term.js)   端末本体: グリッド状態・VT パース・ui.* 描画・ui.onKey
  ssh.connect/write/resize/close + ssh.onData/onClose   ← device_stdlib.c に追加
───────────────── ここまで JS。下は C ─────────────────
components/sshc/sshc.c   wolfSSH セッションタスク (socket + WOLFSSH を専有)
wolfSSL/wolfSSH          レジストリ部品 wolfssl/wolfssh + wolfssl/wolfssl
LWIP                     TCP
```

### 選定理由

- **wolfSSH vs libssh2**: libssh2 の mbedTLS バックエンドは IDF 6 の **mbedTLS 4
  (PSA)** でレガシー crypto API が消えて動かない。wolfSSH は自前 crypto
  (wolfCrypt) を持ちレジストリ部品として配布されているので IDF6 に一番素直。
- **ホスト鍵 ECC/ed25519**: P4 では wolfSSL の `MY_USE_RSA=0` がデフォルト
  (user_settings.h)。host key は ECC/ECDSA/ed25519 のみ。RSA-only の古い
  サーバには現状繋がらない (必要なら RSA を有効化する)。

## 2. アーキテクチャ (既存の不変条件を踏襲)

```
js_task (Core0)                         ssh_task (専用, prio 5, 8KB stack)
  ssh.connect() ──spawn──────────────►  socket → wolfSSH handshake → pty+shell
  ssh.write(s) ──StreamBuffer(2KB)───►  wolfSSH_stream_send
  ssh.onData(fn) ◄─EV_SSH_DATA──heap──  wolfSSH_stream_read (50ms recv tmo)
  ssh.onClose(fn)◄─EV_SSH_CLOSED──────  終了時に必ず 1 回
  ssh.resize(c,r)─volatile flag──────►  wolfSSH_ChangeTerminalSize
```

**不変条件 (gpio/mqtt/touch と同型)**

1. JS コンテキストに触れるのは js_task のみ。ssh_task → JS は既存の MqjsEvent
   キュー (EV_SSH_DATA / EV_SSH_CLOSED) 経由。
2. タスク間で渡す文字列は必ず heap コピー (受信データ) か値型 (StreamBuffer)。
3. ssh セッションが生きている間はイベントループが終了しない
   (`mqjs_ssh_active()` を `anything_pending()` が見る。mqtt と同じ作法)。
4. スクリプト入れ替え (`reset_slots`) で `mqjs_ssh_close()` を呼びセッション破棄。

**RX のバックプレッシャ**: 端末出力は落とせない (画面が壊れる) ので、
`mqjs_post_ssh_data()` は満杯時 100ms ブロックしてから諦める (TCP が上流を
止める)。touch/key の「満杯なら捨てる」とは逆の方針。諦めたらセッションを切る。

## 3. JS API (device_stdlib.c の `ssh` オブジェクト)

| API | 説明 |
|---|---|
| `ssh.connect(host, port, user, pass[, cols, rows])` | セッションタスク起動 (非ブロッキング)。二重接続は例外 |
| `ssh.write(str)` | キー入力をサーバへ。戻り値 1=投函成功 0=tx バッファ満杯 |
| `ssh.resize(cols, rows)` | pty サイズ変更 (非同期)。**組込み設定では現状 no-op** (下記 §5.7) |
| `ssh.close()` | セッション破棄 (タスク終了まで bounded ブロック) |
| `ssh.connected()` | シェル確立後 1、それ以外 0 |
| `ssh.onData(fn)` | `fn(chunk)` サーバ出力 (UTF-8 文字列、生バイト列) |
| `ssh.onClose(fn)` | `fn(reason)` 終了通知 ("remote closed" / "auth/handshake failed" 等) |

PC ビルドでは全て print スタブ (接続しない)。画面なし機体でも同じスクリプトが
走る (onData/onClose は登録のみ)。

## 4. フェーズ計画

| Phase | 内容 | 受け入れ基準 |
|---|---|---|
| **T0. スパイク** (T1 に統合) | wolfSSH × IDF6 ビルド検証 | wolfssl/wolfssh が IDF 6.0.1 で通る |
| **T1. 接続 + 素朴な端末 ✅** (2026-06-11 実機確認済み) | sshc セッションタスク、ssh.* バインディング、examples/ssh_term.js (印字+改行+BS の行バッファ) | JS だけで SSH シェルに繋ぎ、コマンド出力が画面に流れる (実機ログイン成功) |
| **T2. VT100 パーサ ✅** (2026-06-11 実機確認済み) | `examples/ssh_vt.js`: 80x24 セルグリッド + エスケープ状態機械 (チャンク跨ぎ)。CSI カーソル移動 (A/B/C/D/G/d/H/f)、消去 (J/K)、SGR 16 色+bold+inverse、スクロール領域 (r)、行/文字 挿入削除 (L/M/P/@)、ESC index/reverse-index/save-restore。固定セル幅でグリッド描画、行ダーティ + **コマンド予算制フラッシュ** (~40fps, 1 tick ≤110 コマンドで深さ 128 のキュー溢れ=描画ドロップを回避) | JS だけで色付きターミナルが動く (実機確認済み) |
| **T3. 仕上げ** | **横画面化 or 小フォントで 80 桁を全部出す** (実機実測で 1 桁 16px → 720px に 45 桁しか入らない、§4b)、制御キー行 (Esc/Tab/Ctrl/矢印) をキーボードに追加、ホスト鍵 TOFU 保存、公開鍵認証、接続情報の設定 UI | 実用的に使える |

### 4c. 描画パイプライン再設計 (Phase T3, 2026-06-11)

per-cell `ui.text` は重すぎた (グリフ毎に `lv_draw_label` レイヤを起こす →
全画面 ~1000 サイクル/フレーム + スクロール毎の全面再描画)。**等幅セルグリッド
パイプライン**に作り直した:

- **等幅フォント** `fonts/font_term_mono.c` = HackGen Console (size17 bpp4,
  ASCII + 罫線/ブロック)。**9x24 セル → 720px に 80 桁**ぴったり (クリップ無し)。
  proportional な Noto を縮小するのではなく、端末用の monospace を別途積んだ。
- **C プリミティブ** (`ui_tab5` の CanvasApp):
  - `ui.cells(col,row,str,fg,bg)` — 等幅グリフを **生 4bpp ビットマップから
    直接 RGB565 へ fg/bg ブレンド** (lv_draw_label を通さない)。セルにクリップ
    するので罫線のはみ出し (box_w 最大 11) もタイリングで繋がる。
  - `ui.scroll(top,bot,n,bg)` — バッファを memmove (**O(1)、全面再描画が消える**)。
  - `ui.cellSize()` — セル寸法を JS に公開。
- **JS** はダーティ行を同色ランに分けて `ui.cells` (白文字行なら 1 行 = 1 命令)、
  scrollUp/Down を `ui.scroll` に。**全画面 ~24-50 命令 / スクロール 1 命令**
  (旧 ~1000)。実機でスムーズになったとユーザー確認。
- **グリフ取得の罠**: 公開 `lv_font_get_glyph_bitmap()` は呼ぶ前に
  `req_raw_bitmap` を 0 に戻し、A8 デコードのため渡した NULL draw_buf を
  参照 → load fault。`resolved_font->get_glyph_bitmap()` を直接呼ぶ。
  4bpp は行パディング無しの連続ビットストリーム・MSB ニブル先 (ホスト検証
  `fonts/decode_test.py`)。
- **テスト手順** (重要、[[test-new-modules-in-isolation]]): 新プリミティブは
  ① ホストでビット詰め検証 → ② `examples/cells_test.js` で最小デバイススモーク
  (端末抜き) → ③ 全統合、の順。いきなり UI 経由で実機テストして crash-loop
  させたのが教訓。

### 4d. 桁数/行数は画面から導出 (向き非依存, T3)

桁数・行数をハードコードせず `ui.size()` / `ui.cellSize()` から導出する
(`COLS = W/CW`, `ROWS = (H - キーボード高)/LH`)。**pty もその値に
`wolfSSH_ChangeTerminalSize` で合わせる**ので、サーバが画面ぴったりの行数を使う。
向き・解像度・キーボード高が変わっても、また将来 横画面化 (1280幅 → 142桁) しても
グリッドと pty が自動追従する。現状: ポートレート + キーボード常時表示で 80x33。

**wolfSSH_ChangeTerminalSize の有効化** (§5.8): `SendChannelTerminalResize` は
ウィンドウ変更パケットを送るだけだが `!NO_FILESYSTEM` でゲートされ未コンパイル
だった。`components/sshc/wolfssl_override/user_settings.h` が managed user_settings
を `#include_next` してから `NO_FILESYSTEM` を undef (+ termios コードを切るため
`NO_TERMIOS` を define)、root CMakeLists が wolfssl/wolfssh/sshc の include パス
先頭に差し込む。`-Wno-error=maybe-uninitialized` も追加 (有効化で露出する upstream 警告)。

### 4b. 旧メモ (Phase T2, per-cell ui.text。T3 で置換済み)

- per-cell `ui.text` 時代は実機 45 桁・スクロール全面再描画でコマンド予算制が
  必須だった。T3 (§4c) でセルグリッド化して解消。
- **検証/デモ用フラグはコメントを 1 行に**。`var REPORT = false; /* ... */` の
  コメントを複数行にすると、push 時に `sed` でフラグを書き換えた瞬間に
  2 行目以降が宙に浮いて `SyntaxError` → 永続タスクが毎秒リスタートする
  ループになった (フリーズではない。シリアルログで一発判明)。
- **シリアル接続自体がリブートを起こす**。Tab5 の COM8 を開くと
  `CHIP_USB_UART_RESET` で再起動する (= 画面が一瞬暗転)。デバッグ中の
  「暗転」「ソフトリセット」はこれが原因で、JS タスクのクラッシュではない。
  実行中の表示を見たいときはシリアルを開かない。
- **JS の `mqtt.*` はタスク配信クライアントと client-id が衝突**しうる
  (両方デフォルト ID)。自己検証の結果報告に JS mqtt を使うと publish が
  不安定。代わりに SELFTEST モードの `print()` を COM8 シリアルで読むのが
  確実 (実機エンジンの grid を PC 基準と突き合わせられる)。

## 5. wolfSSH × IDF 6 のハマりどころ (T1 で解決)

1. **wolfSSL 5.8.2 の `esp_sdk_mem_lib.c` が C23 予約語 `thread_local` を
   enum メンバ名に使う**。IDF 6 は `-std=gnu23` でコンパイルするため
   `expected identifier before 'thread_local'` で落ちる。対策: プロジェクト
   ルートの CMakeLists.txt で wolfssl/wolfssh コンポーネントだけ
   `-std=gnu17` を追記 (GCC は最後の -std が勝つ。C17 では `thread_local`
   は予約語でない)。Stamp/Tab5 両方に効く。
2. **wolfssh の REQUIRES が `wolfssl` (素の名前) を要求**。idf_component.yml に
   `wolfssl/wolfssl` を明示依存として足す (wolfssh だけだと
   「unknown component 'wolfssl'」)。
3. **user_settings.h は managed のものを使う**。`components/wolfssl/` を作ると
   wolfssl の CMakeLists が「managed と両方ある」とエラーにする。プロジェクト
   コピーは置かない。wolfSSH の有効化は `CONFIG_ESP_ENABLE_WOLFSSH=y`
   (WOLFSSH_TERM 等を user_settings が有効化)。
4. **RSA スタック #warning が -Werror=cpp で fatal**。SSH は専用 8KB タスクで
   走り main タスクスタック (3.5KB) は無関係なので
   `CONFIG_ESP_WOLFSSL_NO_STACK_SIZE_BUILD_WARNING=y` で黙らせる。
5. **コンシューマ側 (sshc.c) は `WOLFSSL_USER_SETTINGS` を define** してから
   `<wolfssh/ssh.h>` を include する。これが無いと ssh.h が
   `wolfssl/options.h` (生成されない) を探して fatal。
6. **mqjs ↔ sshc の循環依存回避**: mqjs が sshc.h に依存するので、sshc は
   mqjs を REQUIRES せず `mqjs_post_ssh_data/_closed` を extern 宣言で呼ぶ
   (ui_tab5 / wifi.c と同じ流儀)。

7. **pty サイズは 80x24 固定 (T1 の制約)**。`NO_FILESYSTEM` 設定では
   `wolfSSH_ChangeTerminalSize` がコンパイルされず、クライアントの pty-req
   も `GetTerminalInfo()` が termios を持たないため既定の 80x24 を送る。
   よって `ssh.resize()` は現状 no-op、`ssh.connect()` の cols/rows も
   プロトコル上は無視される。JS 端末はグリッドを 80x24 に固定して
   サーバの stty と合わせる (examples/ssh_term.js)。可変サイズは T3 で
   wolfSSL 設定を見直す (filesystem スタブ or 別 API)。

これらの設定はマシンローカルの sdkconfig.tab5.defaults と、コミットされる
ルート CMakeLists.txt / components/sshc/ に入っている。

## 6. セキュリティ注記

- v1 はホスト鍵を**検証しない** (中間者攻撃に無防備)。信頼できる LAN 内専用。
  fingerprint はシリアルログに出る。T3 で TOFU 保存を入れる。
- パスワードは ssh.connect の引数として JS ソースに書く想定。タスクは署名付きで
  push される (Ed25519 ゲート) ので配送経路は保護されるが、LittleFS 上の
  task.js は平文。本番運用では公開鍵認証 (T3) に移行すべき。

## 7. 入力・クリップボード・システムパネル設計 (T3a-c + P4、2026-06-11 合意)

ユーザーと詰めた次フェーズの設計。**方針: キーボード/パネルは汎用キートークン源 +
システム UI(C)、端末や app の意味付けは JS(push 可能)。**

### キートークン規約 (キーボード → JS `ui.onKey`)
- 印字キー → そのままのバイト ("a", "あ")
- 特殊キー → `"\x00name"` センチネル (NUL 始まり)。
  `esc tab ctrl alt fn f1..f12 up down left right home end pgup pgdn del ins copy paste`
- C 側は「ボタン → トークン」対応を持つだけ。端末の意味は全部 JS で解釈。

### T3a: コントロールバー + 特殊/修飾キー (価値最大、Tab 補完解禁)
- テキストキーボードの上に端末専用 control bar (`lv_buttonmatrix`):
  `[Esc][Tab][Ctrl][Alt][Fn][←][↓][↑][→][Copy][Paste]`、Fn で数字↔F1-F12。
- **修飾キーは one-shot sticky** (押した次の 1 キーだけ効く)。JS の `ui.onKey`
  状態機械で変換: Ctrl → `c & 0x1F`、Alt → `"\x1b" + c`。Fn キーは xterm
  シーケンス表を JS が持つ。
- **Tab 補完はこれだけで使える** (Tab=`\t` をサーバへ送るだけ、補完はサーバ側
  readline、結果は既存 VT パーサが描画)。受信 `\t` のタブストップ8展開を
  `feed()` に足すかは任意。

### T3b: コピー&ペースト (ほぼ JS のみ)
- **クリップボードは型付き・システム共有の C プリミティブ** (層1):
  `clipboard.set(data, type)` / `clipboard.get()` → `{data, type}` /
  `clipboard.onChange(fn)`。JS コンテキスト外の C バッファなのでタスク切替を
  跨いで生存、将来のマルチタスクで全 app が共有 = **アプリ間 IPC の最初の一手**。
  型 (`text/plain` `text/csv` `application/json` `number` …) で受け手が判別。
- 選択: `ui.onTouch` に選択モード (ロングプレスでドラッグ開始)。ドラッグ →
  セル座標、選択セルをハイライト、離したらグリッドモデルから抽出 → clipboard。
- Paste = `ssh.write(clipboard.get().data)` (対応サーバはブラケットペースト
  `\x1b[200~`…`\x1b[201~` で囲む)。
- **MQTT ミラー (層2) は API に焼き込まず外付け** (`clipboard.onChange→mqtt.publish`
  と `mqtt.subscribe→clipboard.set` を橋渡しする小 app/ランチャー)。retained
  トピックで永続化 + クロスデバイス共有。echo は sender ID で無視。Phase 4。

### T3c: システム制御/stats パネル (キーボード非表示時に出る、C 所有)
- ステータスバーと同じシステム UI 層。明るさ/音量はデバイス機能なので C 所有の
  LVGL パネルが直接ハードを叩く。アプリ (端末) はその上で走る。
- 内容:
  - **stats**: `sys.setAppName()` 自己申告のアプリ名 (MQTT トピック = 一意 ID、
    appName = 人間向け表示名)、`sys.heap()` `sys.psram()` `sys.ssid()`
    `sys.rssi()` `sys.uptime()` (uptime は performance.now で代用可)。
  - **クリップボードプレビュー** (型付き、ペースト前確認)。
  - **明るさダイヤル**: `lv_arc` + 中央 % 表示 + 5% スナップ + `[−][+]` nudge →
    `backlight_set()` 直結。**実装可** (backlight_set 既存)。
  - **音量ダイヤル**: 同じ `lv_arc`。ただしオーディオコーデック未駆動なので枠だけ
    先行、駆動は後続。
- タッチ精度: 相対ドラッグ + ステップ吸着 + 中央数値 + ± nudge (バーの絶対位置
  ジャンプを回避)。ダイヤルスタイルは `lv_arc` で確定。

### Phase 4: マルチタスク + ランチャー + app-store (別スコープ・大改修)
ビジョン: MQTT broker を app-store 化し、複数 mquickjs app をマルチタスク
(例: SSH + 関数電卓 + CSV 可視化 を同時起動、電卓の結果を SSH 編集に使い、
SSH で取った CSV を可視化に渡す)。要素:
- 複数 JS コンテキスト/タスクの同時実行 (今は 1 タスクずつ。メモリ予算 256KB×1
  を分割 or 動的)。
- mooncake ランチャーで画面スロット切替 (設計書 §6 の構想)。
- MQTT トピック = アプリ ID。app-store 化したら topic → manifest(name/icon/権限)。
- IPC バス = 型付きクリップボード (層2 ミラー) を皮切りにローカル MQTT トピック。
