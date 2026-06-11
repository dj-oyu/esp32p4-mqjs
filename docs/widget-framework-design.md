# Tab5 ウィジェットフレームワーク + マルチセッション 設計書

Status: **W1〜W3 完了・実機検証済み** (2026-06-11)。実装ログ = §10 (W1) /
§11 (W2) / §12 (W3)。残りは P4(ランチャー/マルチアプリ)。

関連: `docs/ssh-terminal-design.md`(SSH 端末本体・§7 で入力/クリップボード/
ダイヤルパネルを設計)、[[tab5-platform-vision]](MQTT app-store + マルチタスク
構想)、[[test-new-modules-in-isolation]](新モジュールのテスト手順)。

## 0. 動機

今の JS UI は低レベル(`ui.cells/rect/text/keyboard/onTouch/onKey`、自前で全部
描画)。これだと「ホスト設定ページ(保存済み接続先のリスト + 新規入力フォーム)」
のような**フォーム/リスト/ナビ**を作るのが苦痛。LVGL は既にスクロールするリスト・
カーソル付きテキスト入力・フォーカスを持つので、**それを JS から使える
ウィジェット層**を被せる。さらに **SSH を複数同時にキープ**できるようにする。

これは [[tab5-platform-vision]] の「UI フレームワーク半分」。ページ遷移/タブ
切替の機構は、将来の mooncake ランチャー(アプリ切替)とそのまま同じ土台になる。

## 1. 2 つの UI モードを併存させる

- **キャンバスモード** (`ui.cells/scroll/rect/text`): 自前で高速描画。端末・
  グラフ可視化などカスタム描画向き。**ホットパス。既存のまま。**
- **ウィジェットモード** (`ui.screen/list/button/field/...`): LVGL ウィジェットの
  JS バインド。設定ページ・リスト・フォーム・ナビ向き。フォーム/入力を再発明しない。
- 1 アプリが両方使える。例: SSH クライアントページ = キャンバス端末 + キーボード +
  コントロールバーを内包する screen。

## 2. 先行調査(パイロット)結果 — 2026-06-11

### W1-0: smooth_ui_toolkit lvgl_cpp は LVGL 9.4 で通る ✅
- `components/smooth_ui_toolkit/src/lvgl/lvgl_cpp/` に `button/label/text_area/
  roller/slider/switch/table/screen/canvas/chart/qrcode/...` の C++ ラッパー
  (ヘッダオンリーのテンプレート、`Widget<lv_allocator>` パターン)。LVGL 9 命名
  (`lv_button_create`, `lv_textarea_*`, `lv_obj_del`, `lv_obj_add_event_cb`)。
- vendored LVGL は **9.4.0**。
- **コンパイルスパイク**(使い捨て `wtest_spike.cpp` を ui_tab5 に一時追加):
  `Screen / Button / Label / TextArea` を実体化 + `Screen::loadAnim(
  LV_SCR_LOAD_ANIM_MOVE_LEFT, ...)` を呼んで **Tab5 ビルドが通った**
  (`Building CXX ... wtest_spike.cpp.obj` → `Project build complete`)。
  → **採用決定**。設計書 §1 で懸念していた「lvgl_cpp の LVGL9 API ずれ」は杞憂。
- 副産物: **`Screen::loadAnim`(= `lv_screen_load_anim`)が使える** → ナビ遷移の
  スライド/フェードはこれで実現(自前アニメ不要)。
- 性能: ラッパーは薄く実行時オーバーヘッド無し。ウィジェットは設定/フォーム用で
  ホットパスでない。端末はキャンバスのまま。→ **性能影響なし**(採用条件クリア)。
- 注: lvgl_cpp は .cpp ゼロ(ヘッダオンリー)。現状ビルドされず(AnimateValue
  だけ使用中)。consumer がインスタンス化して初めてコンパイルされる。

### W1-1: LVGL ヒープを PSRAM 化(LVGL 独自プール)— 方式調査
- LVGL の mem ソース選択(Kconfig): `LV_USE_BUILTIN_MALLOC`(tlsf プール) /
  `LV_USE_CLIB_MALLOC`(標準 malloc) / `LV_USE_CUSTOM_MALLOC`(外部実装)。
  **現在 `CONFIG_LV_USE_CLIB_MALLOC=y`**(= 内部 RAM の malloc)。
- 決定(ユーザー): **LVGL 独自 LV_MEM プールを PSRAM に**(CLIB+SPIRAM 閾値では
  なく)。→ `LV_USE_BUILTIN_MALLOC`。
- **`LV_MEM_ADR`(コンパイル時 hex)は PSRAM(実行時マップ)に使えない**。なので:
  起動時に `heap_caps_malloc(N, MALLOC_CAP_SPIRAM)` した大バッファを
  **`lv_mem_add_pool(buf, N)`** で LVGL に渡す方式が要る(tlsf がそこを管理)。
- **プールは大きく**(2〜4MB)。過去、builtin 64KB プールが
  コンソールラベル(~200 個)で枯渇して **UI フリーズ**した履歴あり
  ([[esp32p4-build-flash-workflow]])。CLIB_MALLOC=y はその回避策だった。
  PSRAM の大プールに移せば根本解決。**W1-1 実装時に既存コンソール/端末の
  回帰確認必須**(枯渇しないこと、描画が遅くならないこと)。
- 未確定(実装時パイロット): builtin の主プール(`LV_MEM_SIZE_KILOBYTES`)を
  最小にして PSRAM プールを add するのか、主プール自体を PSRAM にできるのか。
  LVGL の lv_init/lv_mem 初期化順と esp_lvgl_port の絡みを確認する。

## 3. ウィジェット API(ハンドル式・確定)

宣言ツリーでなく **ハンドル式**(動的なリスト/フォームに向く)。コールバックは
**ウィジェット毎の個別関数**。

```js
// ホスト一覧ページ
var scr = ui.screen("SSH Hosts");           // 新スクリーン(前のはスタックへ退避)
var list = scr.list();
hosts.forEach(function (h) {
  list.add(h.user + "@" + h.host, function () { openSession(h); });  // タップで接続
});
scr.button("+ New", function () { showForm(); });

function showForm() {
  var s = ui.screen("New Host");
  var host = s.field("Host"), user = s.field("User"),
      pass = s.field("Password", { secret: true });
  s.button("Save", function () {
    store.set("hosts", hosts.concat([{ host: host.value(), user: user.value(),
                                       pass: pass.value() }]));
    ui.back();                               // 一覧へ(スタック pop)
  });
  s.button("Cancel", ui.back);
}
```

初期ウィジェットセット(確定・これで設定 UI は組める):
`ui.screen(title)` / `.list()`+`.add(text,onTap)` / `.button(text,onTap)` /
`.field(label,opts)`+`.value()` / `.label(text)` / `.toggle()` / `.slider()` /
`ui.navigate(builderFn)` / `ui.back()` / `.canvas()`(端末をこの中に置く)/
`ui.tabview()`(W3 の複数セッション切替)。

## 4. メモリ・スラッシング回避設計(核心)

「画面遷移で細かく解放、でもスラッシングさせない」を 4 つの仕掛けで。

### ① LVGL ヒープを PSRAM(最大の効き目) — W1-1
ウィジェットの確保/解放を 32MB PSRAM の LVGL 独自 tlsf プールで churn させる。
内部 RAM(~400KB・希少)を断片化させない。端末キャンバスバッファも既に PSRAM
なので一貫。(方式は §2 W1-1。)

### ② 有限リテインスタック(再構築しない、N=3 可変) — W1-3
`navigate(A→B)` は **A を破棄せず隠してスタックへ退避**、`back` は **A を
再表示するだけ**(再構築ゼロ=churn ゼロ)。破棄は「完全 pop」or「**深さ N=3
超過**」のみ。N は定数化して**後で調整可能**にする。超過分は破棄し、戻る時に
builder を再実行して再構築(稀)。**端末スクリーンは常駐扱い**(高コストな
キャンバスを作り直さない)。

### ③ object_pool でリスト行を再利用 — W1-3
ホスト一覧やスクロールで行ウィジェットを **free+alloc せず recycle**
(`smooth_ui_toolkit/src/tools/object_pool`)。長いリストでも確保が増えない。

### ④ 「木をまとめて解放」+ GC を 1 回に集約 — W1-2/3
スクリーン破棄 = `lv_obj_del(root)` で**子ツリーを 1 回の再帰で解放**
(arena 解放に近い)。同時に、そのスクリーンが握る **JS コールバックの GCRef を
まとめて release** → mquickjs のコンパクション GC が **1 回で詰め直す**
(逐次解放で何度も走らせない)。

### 遷移タイミング
新スクリーン構築 → `Screen::loadAnim` でスライド → **アニメ完了後に旧を解放**。
小フォームなら一瞬の二重確保は許容(PSRAM なので無害)。大画面は free→build に
切替。

### 定量確認(W1-4、必須)
遷移を繰り返して `sys.heap()` が**安定**(増え続けない=リークもスラッシングも
ない)ことを計測してから先へ([[test-new-modules-in-isolation]] の精神)。

## 5. クロスタスク

LVGL は Core1。ウィジェット生成/変更は LVGL ロック下。JS の `ui.*` →
コマンドキュー → LVGL タスクが生成。イベント(tap/change/value)は既存の
MqjsEvent キューで JS へ。C 側に **id ↔ lv_obj/lvgl_cpp 対応表**、ハンドルは
**世代カウンタで stale 検出**。コールバックは GCRef 保持(gpio.onChange と同型)。

## 6. データモデル(ローカルファースト)

保存ホスト一覧は **NVS/LittleFS にローカル永続**(`store.get/set`、ブローカー
非依存。`docs/ssh-terminal-design.md` §7.1 の原則)。パスワードは現状平文保存に
なるので、将来は公開鍵認証 or デバイス鍵での暗号化へ(SSH 設計の非ゴール参照)。

## 7. 複数 SSH セッション(W3)

今の `sshc` は 1 セッション固定(static な s_task/s_tx)。**ハンドル式に refactor**:
```js
var id = ssh.connect(host, port, user, pass, cols, rows);  // → セッション id
ssh.write(id, "ls\n");  ssh.onData(id, fn);  ssh.close(id);
```
- C: セッション配列、各々が独立 socket/StreamBuffer/wolfSSH。`EV_SSH_DATA` に
  **session id を載せて** JS が正しい端末へ振り分け。
- **上限 2〜3 本**(1 本 = 8KB タスク + wolfSSH 状態 + crypto バッファ ≈ 数十 KB
  内部 RAM)。定数化して調整可能に。
- UI: SSH クライアントページを **tabview**(セッション毎にタブ)で「複数キープ + 切替」。

## 8. フェーズ計画

| | 内容 | 規模 | 依存パイロット |
|---|---|---|---|
| **W1-0** | lvgl_cpp × LVGL9.4 互換 ✅ **済** | — | 完了 |
| **W1-1** | LVGL ヒープ PSRAM 化(builtin malloc + lv_mem_add_pool)。既存 UI 回帰確認 | 小 | §2 W1-1 |
| **W1-2** | ウィジェットバインド基盤: id↔obj 表、コマンドキュー拡張、イベント routing、ハンドル世代管理 | 中 | — |
| **W1-3** | 初期ウィジェット: screen/navigate/back(リテインスタック N=3)+ button/label/field/list(object_pool)+ 一括解放(④) | 中 | — |
| **W1-4** | 設定ページ骨子を JS で組み、navigate/back を **heap 計測**してスラッシングしないことを定量化 | 小 | sys.heap() |
| **W2** | ホスト設定ページ + NVS 永続 + フォーム。既存 ssh_vt を「クライアントページ」に統合 ✅ **済** (§10.6) | 小〜中 | — |
| **W3** | 複数 SSH セッション(sshc ハンドル化)+ tabview 切替 ✅ **済** (§12) | 中 | — |
| **P4** | これを土台にランチャー/マルチアプリ([[tab5-platform-vision]]) | 大 | — |

着手順は **W1-1(PSRAM ヒープ)→ W1-2(基盤)→ W1-3 → W1-4**。W1-0 は完了。

## 9. 確定した決定事項(2026-06-11)

- ウィジェットは **smooth_ui_toolkit lvgl_cpp**(LVGL 9.4 互換確認済み)。性能影響なし。
- API は **ハンドル式**、コールバックはウィジェット毎の個別関数。
- LVGL ヒープは **LVGL 独自プールを PSRAM**(builtin malloc + lv_mem_add_pool)。
- リテインスタック **深さ N=3**(定数化して調整可能)。
- 複数 SSH セッション **上限 2〜3 本**(定数化)。
- 初期ウィジェットセットは §3 のもので十分。
- 画面遷移アニメは `Screen::loadAnim`(lv_screen_load_anim)を使う。

## 10. 実装ログ (2026-06-11, W1 実装セッション)

### 10.1 W1-1: LVGL ヒープ PSRAM 化 — 方式は §2 の未確定案より単純化
§2 W1-1 の未確定だった「主プール自体を PSRAM にできるのか」は **YES**:
LVGL builtin malloc には `LV_MEM_POOL_ALLOC` フック(lv_mem_core_builtin.c)が
あり、`lv_mem_init()` が静的配列の代わりに
`lv_tlsf_create_with_pool(LV_MEM_POOL_ALLOC(LV_MEM_SIZE), LV_MEM_SIZE)` を呼ぶ。
→ **`lv_mem_add_pool` の二段構えは不要**。実装:
- `sdkconfig.defaults`: `CONFIG_LV_USE_BUILTIN_MALLOC=y` +
  `CONFIG_LV_MEM_SIZE_KILOBYTES=3072`(3MB)。
- `components/ui_tab5/CMakeLists.txt`: lvgl ターゲットに
  `LV_MEM_POOL_INCLUDE="esp_heap_caps.h"` /
  `LV_MEM_POOL_ALLOC(size)=heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` を定義。
  この define が無いと 3MB が .bss に置かれリンクできないので注意。
  **罠: CMake の `target_compile_definitions` は関数形式マクロを黙って
  捨てる** — `LV_MEM_POOL_ALLOC(size)=...` は `target_compile_options` で
  `-D` として渡すこと(初回ビルドは .bss 3MB あふれで
  "--enable-non-contiguous-regions discards section" の連発でリンク失敗
  した。原因はこれ)。
- Stamp ビルドは LVGL を初期化しないので無害(プール確保は実行時)。
- ビルド確認済み (2026-06-11): Tab5 (`build_tab5`) と Stamp (`build`) の
  両方グリーン。ついでに既存バグ修正: トップ CMakeLists の wolfSSH
  include-override が Stamp の INTERFACE な sshc ターゲットに PRIVATE を
  付けて configure が落ちていた(TYPE で分岐するよう修正)。

### 10.2 W1-2/3: 実装の形(設計からの意図的な差分 2 点)
実体: `components/ui_tab5/ui_widgets.cpp`(C コア)+ `mqjs_runtime.c`
(JS バインド・EV_WIDGET・コールバック表)+ `device_stdlib.c`(ROM クラス)。

1. **コマンドキューではなく lvgl_port_lock 下の同期呼び出し**(§5 の
   「LVGL ロック下」をキューなしで直接満たす)。理由: `field.value()` 等の
   同期読みが必須で、ウィジェットはホットパスでない。キャンバスの
   キューはホットパスなのでそのまま。
2. **lvgl_cpp ラッパーではなく LVGL C API 直叩き**。lvgl_cpp の Widget は
   RAII(デストラクタで自分の lv_obj を delete)で、§4④の
   「`lv_obj_del(root)` で木をまとめて解放」と所有権が衝突する。
   遷移アニメは `lv_screen_load_anim`(= Screen::loadAnim の中身)を使用。
   back 時は auto_del=true で「アニメ完了後に旧を解放」をそのまま実現。

機構(設計どおり): id↔obj 表 160 slot + 世代カウンタ(handle =
gen<<8|slot、screen 破棄で全所属 entry の世代を bump → stale ハンドルは
無害な no-op)。イベントは LVGL タスク → `mqjs_post_widget(handle,value)`
→ EV_WIDGET → JS。コールバックは GCRef 48 slot、**screen handle 単位で
一括 release**(§4④。back / 深さ超過 evict / タスク終了の 3 箇所)。
タスク終了時は `ui_tab5_w_reset()` が全スクリーンを破棄してコンソール
画面へ戻す(キャンバスのタスク切替クリアと同型)。

JS API は §3 の形をそのまま実装(mquickjs の user class
`UiScreen`/`UiWidget`、opaque に handle を持つ。ROM stdlib の制約で
クラス定義はグローバル直下に置く必要があった)。`sys.heap()` →
`[internal, psram, lvgl_pool]` も追加(gen/ + gen_pc/ ヘッダ再生成済み、
`JS_CLASS_COUNT` は mqjs_classes.h で定義)。

W1 で意図的に積み残したもの:
- `ui.navigate(builderFn)` は「即時実行」のみ(builder 保持なし)。
  深さ超過で evict されたスクリーンへ back すると**コンソールまで
  フォールスルー**する。builder 再実行による再構築は W2。
- §4③ object_pool の行リサイクルは未実装(行はスクリーンの木と一緒に
  死ぬ。リスト在位のまま再 populate する消費者が現れる W2 で導入)。
- `.canvas()` / `ui.tabview()` は W3。
- FIELD はタップでスクリーン内 lv_keyboard(共有 1 枚)が出る。端末用
  キーボード(ui.keyboard)とは別物。

### 10.3 W1-4: 計測ハーネス
`examples/settings_demo.js`: 設定ページ骨子(label/field×3(secret 含む)/
toggle/slider/list 6 行/Save/Cancel)を 5 枚 push(N=3 超え → evict 発生)
→ コンソールまで pop、を 8 周し、各周で `sys.heap()` を出力。
**PC ビルドで全周回 OK**(evict・フォールスルー・コールバック解放が設計
どおり動作、48 slot を超えない)。

### 10.4 実機検証結果 (2026-06-11)
1. **W1-4 定量ゲート合格**: `settings_demo.js` 41 画面完走(クラッシュなし)。
   `widget_leak_probe.js`(print を排した 100 画面 churn)を 2 周実行:
   **lvgl d=0 / psram d=0**。settings_demo で見えた lvgl 減少
   (-284B/cycle)はコンソール行ラベルの保持(200 行リング、設計どおり)で、
   ウィジェットのリークではない。
2. internal RAM は周回中 -21KB 減るが、これは **JsUiHandle opaque の
   finalizer が GC 実行まで遅延する**ため(mquickjs の仕様)。タスク終了の
   context 破棄で全回収され、次タスクの開始値は完全に回復(238051 →
   239611)。有界・回収確認済み。watch 項目: 1 タスク内で数千ウィジェットを
   GC なしで作る場合のみ内部 RAM を圧迫し得る(必要になれば back() 時に
   JS_GC を蹴る)。
3. SSH 回帰 OK: 新ファーム起動時に永続化済み ssh_vt タスクが自動実行され
   `shell up on sunrise@192.168.1.33:22` を確認(PSRAM プール下で wolfSSH
   正常)。
4. **コンソール flood 回帰 OK**(`console_flood.js`): 300 行連続出力後も
   `sys.heap()` が返る(= LVGL ポートロックが取れる = UI タスク生存。旧
   64KB プール枯渇はここでフリーズした)。200 行キャップ到達後の lvgl
   プールはフラット(±100B)。複数周回で安定。
5. **目視確認(ユーザー実施)**: FIELD キーボード・入力・パスワード
   マスク OK、toggle/slider OK、リスト行タップはコールバック発火していた
   が見た目の反応が薄く「仕切り線の長さが変わる」ように見えた(= LVGL
   デフォルトテーマの押下 transform の副作用。意図した効果ではない)。

### 10.5 目視フィードバック対応 (2026-06-11)
- FIELD: フォーカス中はアクセント色(#4FC3F7)の太ボーダー + 背景明色化
  (`LV_STATE_FOCUSED` スタイル)。非フォーカスは細い dim ボーダー。
- リスト行: 背景を不透明化し、**押下中は青背景(#2E6BD6)+ 白文字**。
  デフォルトテーマの押下 transform(width/height)を 0 に上書きして
  「仕切り線が伸び縮みする」錯視を排除。
- `UiWidget.setText(str)` を追加(LABEL/BUTTON/ITEM の表示文字列、FIELD は
  内容を置換)。デモはタップ/Save の結果を画面内ステータスラベルに表示する
  ように変更(コールバック往復が console なしで見える)。Save は back
  しなくなった(値の確認がしやすいように)。
- **ステータスバーは `lv_layer_top()` のシステムクロームに**(08db8f5)。
  ウィジェット画面でも常時表示・遷移アニメで動かない。ウィジェット画面は
  UI_STATUSBAR_H(ui_tab5_internal.h で共有)の top padding を確保。
  端末キーボード(ui.keyboard)の親もコンソール画面に固定(ウィジェット
  画面上で作られると破棄時に dangling になるため)。
- examples は全面的に 3 形態(ウィジェット/キャンバス/ハイブリッド)へ
  対応済み(5856abd、examples/README.md に標準イディオム集)。ssh_vt /
  ssh_term は接続フォーム入力になり認証情報がファイルから消えた。

## 11. W2 実装ログ (2026-06-11)

- **`store.get/set/del`**(mqjs_runtime.c / device_stdlib.c): NVS
  namespace "mqjs"、キー 1〜15 文字(NVS 制限)、値は文字列 ≤3.9KB
  (JS 側で JSON.stringify/parse)。set/del は即 commit。NVS 初期化は
  wifi.c 任せ + 遅延フォールバック(WiFi 無し構成でも動く)。PC ビルドは
  セッション内テーブル(フロー検証用)。mqjs コンポーネントに
  nvs_flash 依存を追加。§6 のローカルファースト原則どおりブローカー非依存。
- **ssh_vt クライアントページ**: 起動 → 「SSH ホスト」一覧(store の
  "ssh_hosts" JSON)。行タップ → 接続/編集/削除のアクションページ。
  「+ 新規接続」→ フォーム(Host/Port/User/Password + 保存トグル)。
  切断 → 一覧に復帰。リスト変更後は `while(ui.back()){}` でコンソール
  まで巻き戻して作り直す(動的リストの標準イディオム)。端末コア
  (VT100 パーサ/セル描画)は無変更、SELFTEST 出力同一を確認。
- パスワードは平文で NVS 保存(§6 で宣言済みのトレードオフ。公開鍵認証/
  デバイス鍵暗号化が将来課題)。
- 実機確認(自動プローブ): `w2_store_probe.js` で set/get/del とタスク
  再実行・**ウォッチドッグ再起動跨ぎの永続**を MQTT レポートで実証。
  UI から保存したホスト(ssh_hosts JSON)も再起動を生き延びた。

### 11.1 実機で踏んだバグ 2 件(修正・実証済み)

1. **unwind 競合**: `while(ui.back()){}` が画面遷移アニメと競合。LVGL は
   アニメ完了まで `lv_screen_active()` が旧画面を返す(lv_display.c)ため、
   連続 back の 2 回目が orphan 済み画面を見て 0 を返し巻き戻しが止まる。
   タイミング依存(LVGL タスクは Core1 並走)で、症状は「接続後に
   キーボードが効かない/タップ再表示しない」(実は hosts ページが前面の
   まま)。**修正**: ui_widgets.cpp がアクティブ画面スロットを自前追跡
   (`s_cur`)し、`lv_screen_active()` への問い合わせを廃止。連続 back の
   重なりは LVGL 側が in-flight ロードを強制完了 + del_prev で安全。
   **実機実証**: `w2_unwind_probe.js`(3 画面積んで 1 tick 内で全 unwind)
   → burst_pops=3 OK / rebuild_pops OK。
2. **MQTT クライアント ID 衝突**: JS の `mqtt.connect` と task_source.c が
   両方 esp-mqtt デフォルト ID(MAC 由来で同一)→ 同一ブローカーに繋ぐと
   蹴り合いになりタスク配信がフラッピング(status バー MQTT グレー、
   push 喪失)。**修正**: JS 接続に固有 ID `mqjs-js-task` を付与
   (js_mqtt_connect)。プローブの publish とタスク配信の共存で実証。

教訓: 「MQTT グレー + コンソール初期表示のまま」は C6 wedge とは限らず、
「画面を描かないタスクが永続化されている + 配信チャネル不調」のことも
ある。シリアルの `loaded persisted task (NNN bytes)` のサイズでどの
タスクが走っているか判別できる。

## 12. W3 実装ログ (2026-06-11) — 複数 SSH セッション + タブ切替

- **sshc ハンドル化**(components/sshc): static 単一セッション →
  `SSHC_MAX_SESSIONS = 3` のセッション配列。各セッションが独立の
  task(8KB)/socket/wolfSSH/tx StreamBuffer(2KB) を持つ。id は
  `(gen<<2 | slot) + 1` で stale な JS ハンドルはどの API でも無害な
  no-op。`mqjs_ssh_close_all()`(タスク切替クリーンアップ用)を追加。
  wolfSSH_Init は一度だけ(複数タスクの Init/Cleanup 競合を回避)。
- **ssh.\* JS API は §7 のとおりハンドル式**: `connect` が id を返し
  (空きなしは TypeError)、`write/resize/close/connected/onData/onClose`
  は第1引数に id。コールバックはセッション毎(4 slot、EV_SSH_CLOSED の
  ディスパッチ後に data/close をまとめて release)。EV_SSH_DATA/CLOSED は
  id を運び、JS が正しい端末へ振り分ける。
- **ssh_vt マルチセッション化**: 端末状態+VT100 パーサ+描画を
  `makeTerm()` ファクトリに分離(パーサロジックは無変更、SELFTEST 出力
  同一を確認)。グリッド最上段 1 行(TAB_ROWS)がキャンバス直描きの
  タブバー: タップで切替、緑の `+` で新規接続(ホストページへ)。
  pty 行数は ROWS-1 に追従。
- **背景タブのリソース設計**(「ピクセルは捨てる、モデルと回線は保つ」):
  描画資源はタブ毎に持たない(キャンバスは全タブ共有の 1 枚、背景 Term
  は `act` ゲートで ui.* を一切発行しない。背景中のスクロールも
  ui.scroll でなく dirty マークに置換)。保持するのはグリッドモデル
  (~10KB/本、固定 256KB の JS ヒープ内)と SSH セッション資源
  (数十 KB 内部 RAM/本 — これが上限 3 本の根拠)。切替時は
  markAll + 予算付き flush でモデルから全再描画。背景受信は継続
  (サーバ出力を取りこぼさない)。PSRAM にタブ数比例の確保はない。
- 実機テスト済み(ユーザー確認): タブ切替、複数セッション同時キープ。
