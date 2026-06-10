# Tab5 ウィジェットフレームワーク + マルチセッション 設計書

Status: **設計フェーズ** (2026-06-11)。実装は別セッション。本書はその設計と、
この場で実施した先行調査(パイロット)の結果を記録する。

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
| **W2** | ホスト設定ページ + NVS 永続 + フォーム。既存 ssh_vt を「クライアントページ」に統合 | 小〜中 | — |
| **W3** | 複数 SSH セッション(sshc ハンドル化)+ tabview 切替 | 中 | — |
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
