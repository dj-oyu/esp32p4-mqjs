# ファイラとストレージ層の設計

内蔵ストレージ (LittleFS) と microSD (FAT) を 1 つのファイラから扱えるようにする。
Tab5 と Stamp-P4 の差は**コンパイル時の `#ifdef` ではなく、実行時の
「ボリュームが在るか無いか」に畳み込む**。

---

## 1. 出発点の事実

調査で確定した内容 (2026-08-25)。

### 1.1 このリポジトリの現状

microSD は完全に未実装。SD/SDMMC を触るコードはゼロ、FAT のマウントもゼロ。
内蔵側は 3 つあるが、いずれも汎用のファイル API を持たない。

| 実体 | パーティション | 既存の口 |
|---|---|---|
| LittleFS `/littlefs` | `storage` 0x610000 / 1 MB | `main/storage.c` の専用関数のみ (`storage_save_app` 等) |
| NVS | `nvs` 0x9000 / 24 KB | `store.get/set/del` |
| jisyo (mmap 専用) | `jisyo` 0x720000 / 8.5 MB | なし |

### 1.2 公式ファーム (M5Tab5-UserDemo BSP) の SD 実装

`platforms/tab5/components/m5stack_tab5/m5stack_tab5.c`:

```c
#define GPIO_SDMMC_CLK (GPIO_NUM_43)
#define GPIO_SDMMC_CMD (GPIO_NUM_44)
#define GPIO_SDMMC_D0  (GPIO_NUM_39)
#define GPIO_SDMMC_D1  (GPIO_NUM_40)
#define GPIO_SDMMC_D2  (GPIO_NUM_41)
#define GPIO_SDMMC_D3  (GPIO_NUM_42)
#define GPIO_SDMMC_DET (GPIO_NUM_NC)      /* カード検出ピンなし */
#define SDMMC_BUS_WIDTH (4)
/* SDMMC_HOST_SLOT_0 / SDMMC_FREQ_HIGHSPEED */
#define BSP_LDO_PROBE_SD_CHAN       4
#define BSP_LDO_PROBE_SD_VOLTAGE_MV 3300  /* sd_pwr_ctrl_new_on_chip_ldo() */
```

`format_if_mount_failed = false`、allocation unit 16 KB。電源スイッチピンは無く、
オンチップ LDO ch4 が SD の IO 電源。

### 1.3 移植したときの当たり判定

| 項目 | 結果 |
|---|---|
| SDMMC スロット | ピンは競合しない (C6 = slot 1、SD = slot 0)。**ただしコントローラは 1 個しかなく、そのままでは同時に上がらない → §1.5** |
| GPIO 39-44 | Tab5 ビルドで未使用 |
| LDO | ch1=flash / ch2=PSRAM / ch3=MIPI DSI PHY、**ch4 が空き** (公式と同じ) |
| FATFS | すでに sdkconfig に在る (`VOLUME_COUNT=2`, `LFN_HEAP`, `MAX_LFN=255`) |
| DMA | `SOC_SDMMC_PSRAM_DMA_CAPABLE=y` → 転送バッファを PSRAM に置ける |

### 1.4 Stamp-P4 には SD が挿せない

Stamp-P4 では C6 が **slot 1 = CLK43 / CMD44 / D0-D3 45-48 / RST42** を占有する
(`sdkconfig.defaults`)。Tab5 の SD が使う 39-44 と真正面からぶつかり、
そもそも Stamp-P4 モジュールに microSD スロットが無い。

**したがって「microSD は Tab5 だけの能力」であり、これが隠すべき差分の本体。**

### 1.5 スロットが別でも同時には上がらない (実機で判明)

配線を突き合わせた時点では「slot 0 と slot 1 で競合なし」と読んだが、
**実機ではブートループになった**。SD は正しくマウントされ、そのあと
esp_hosted が落ちる:

```
I (2861) sdcard: ... SD
I (2861) fsvol: volume 'sd' mounted
E (2850) SD_HOST: sd_host_create_sdmmc_controller(84): no available sd host controller
E (2850) sdmmc_periph: sdmmc_host_init(84): failed to create new SD controller
E (2860) H_SDIO_DRV: could not create sdio handle, exiting
rst:0xc (SW_CPU_RESET)
```

原因はハードではなく IDF 6 のドライバにある。legacy の `sdmmc_host_init()` は
**呼ばれるたびに無条件で新しいコントローラを作ろうとする**:

```c
esp_err_t sdmmc_host_init(void) {
    return sd_host_create_sdmmc_controller(&cfg, &s_ctlr);
}
```

P4 の SDMMC コントローラは 1 個なので、2 人目の `sd_host_claim_controller()`
は必ず落ちる。一方スロットを足す `sdmmc_host_init_slot()` は同じ静的な
`s_ctlr` に `s_slot0` / `s_slot1` を並べられる作りで、**足りないのは
「もう在るなら作らない」の一行だけ**だった。

直し方は 2 つあった。(a) 起動順を入れ替えて必ず esp_hosted に先に取らせる、
(b) 生成を冪等にする。(a) は「Wi-Fi を切ったビルドでは誰も作らない」
「先に呼んだ方が勝つ」という順序依存を残すので採らず、(b) を選んだ:
リンカの `--wrap=sdmmc_host_init` に参照カウントを噛ませ、どちらが先でも
2 人目は同じコントローラを共有する (`components/sdcard/sdmmc_share.c`)。
managed_components の esp_hosted には手を入れない —— `ui_tab5` が
`esp_hosted_init` に、`main` が `esp_panic_handler` にやっているのと同じ手。

アンマウントは既定の `deinit` (両スロットを畳む) ではなく
`SDMMC_HOST_FLAG_DEINIT_ARG` + `sdmmc_host_deinit_slot` を使う。
カードを取り出した拍子に C6 の SDIO リンクまで切らないため。

**教訓**: 「ピンが別だから同居できる」はハードの話でしかない。
共有されるのはピンだけでなく、**ドライバが握る単一のコントローラ**でもある。

---

## 2. 中心となる考え方

> **Stamp-P4 は「カードが永久に入っていない Tab5」として扱う。**

ファイラは Tab5 でも「カードが抜かれている」状態を必ず扱わねばならない。
その分岐を書いた時点で Stamp 対応は済んでいる ——
`if (board == stamp)` のような分岐は 1 行も書かない。ボード差は
「登録されたボリュームの集合」という**データ**になり、コードから消える。

この方針から自動的に決まること:

- JS からは `fs.volumes()` を回すだけ。ボード名も `#ifdef` も見えない。
- Stamp で `/sd/...` を開こうとすると「そんなボリュームは無い」というエラーになる。
  カードが抜けている Tab5 とは別のエラーコードにするが、**ファイラは無いボリューム
  を画面に出さないので、そもそもこの経路に入らない**。
- 将来 USB メモリや Tailscale 越しのリモート FS を足すときも、
  ボリュームを 1 つ登録するだけで済む。

---

## 3. 層の分け方

```
examples/files.js                     ボード非依存 (fs.volumes() を回すだけ)
─────────────────────────────────── JS API 境界 (fs.*)
mqjs_runtime.c の fs バインディング      ボード非依存 (fsvol_* しか呼ばない)
─────────────────────────────────── C API 境界 (fsvol_*)
components/fs_core                    ボリューム登録簿 + 仮想パス解決 + 共通 ops
       ↑ 登録                                    ↑ 登録
main/storage.c ("internal")            components/sdcard ("sd")
                                       Kconfig off なら「登録しない」を
                                       返すだけの実装に切り替わる
```

`components/sdcard` の TU は Kconfig が off でもコンパイルする。
中身が `#if CONFIG_MQJS_SDCARD` で丸ごと入れ替わり、`sdcard_init()` は
「登録しない」を返すだけになる —— こうしておくと **app_main 側に
`#ifdef` を書かなくて済む**。実体が無ければ `--gc-sections` が落とすので
Stamp の flash は増えない (ui_tab5 が LVGL に対してやっているのと同じ)。
REQUIRES は無条件でなければならない (依存の展開は kconfig より先に走る)。

**コンポーネント名に `tab5` を入れない。** SD ホストのコード自体は汎用で、
ボード固有なのはピン番号と LDO チャンネルだけ。それを Kconfig の既定値に
追い出すことで、ボード差は「設定データ」になりコードから消える。
Kconfig 既定値は Tab5 の配線 (§1.2)、`sdkconfig.tab5.defaults` で有効化する。

---

## 4. 仮想パス

JS が見るパスは `/<volume-id>/<以下>`。実 VFS パス (`/littlefs`, `/sd`) は
C 層の内側に閉じ込める。

| 仮想パス | 実パス |
|---|---|
| `/internal` | `/littlefs` |
| `/internal/apps/foo.js` | `/littlefs/apps/foo.js` |
| `/sd/DCIM/a.jpg` | `/sd/DCIM/a.jpg` |

こうする理由:

1. **サンドボックス**。第一要素が登録済みボリューム id でなければ即エラー。
   `..` は解決前に拒否するので、`/` や他の VFS マウント (将来の spiffs など)
   には原理的に届かない。
2. **可搬性**。ファイラが記憶する「最後に開いていた場所」やブックマークが、
   内部のマウントポイントを変えても壊れない。
3. **表示名と分離**。`label` ("内蔵" / "microSD") は id と別に持つので、
   UI の文言を変えてもパスが変わらない。

---

## 5. C API (`components/fs_core`)

```c
typedef struct fsvol fsvol_t;

typedef struct {
    esp_err_t (*mount)(const fsvol_t *);
    esp_err_t (*unmount)(const fsvol_t *);
    bool      (*probe)(const fsvol_t *);   /* 媒体が今も在るか */
    esp_err_t (*usage)(const fsvol_t *, uint64_t *total, uint64_t *freeb);
} fsvol_ops_t;

struct fsvol {
    const char *id;      /* "internal" / "sd" — 仮想パスの第一要素 */
    const char *label;   /* "内蔵" / "microSD" — 表示専用 */
    const char *root;    /* 実 VFS パス */
    const char *fstype;  /* "littlefs" / "fat" — 表示専用 */
    uint8_t     flags;   /* FSVOL_REMOVABLE / FSVOL_SYSTEM */
    const fsvol_ops_t *ops;  /* NULL = 常時マウント済みの固定ボリューム */
};
```

`FSVOL_SYSTEM` は内蔵に付ける。ファイラは付いているボリュームで
「フォーマット」を出さず、`apps/` 以下の削除に追加の確認を挟む。

登録簿:

```c
esp_err_t      fsvol_register(const fsvol_t *v);  /* v は static 寿命 */
int            fsvol_count(void);
const fsvol_t *fsvol_at(int i);
const fsvol_t *fsvol_find(const char *id);
bool           fsvol_mounted(const fsvol_t *v);
esp_err_t      fsvol_mount(const fsvol_t *v);     /* 冪等 */
esp_err_t      fsvol_unmount(const fsvol_t *v);
uint32_t       fsvol_epoch(const fsvol_t *v);     /* マウント世代 */
```

`fsvol_epoch` はマウントのたびに増える番号。抜き差しをまたいだ権限や
ハンドルが「まだ有効か」を、**使うときに見比べるだけ**で判定できる
(アンマウント時にコールバックで回って無効化する必要がない)。

パス解決:

```c
/* "/sd/DCIM/a.jpg" -> *vol=sd, real="/sd/DCIM/a.jpg" */
esp_err_t fsvol_resolve(const char *vpath, const fsvol_t **vol,
                        char *real, size_t cap);
```

戻り値を分ける:

| 状況 | 戻り値 | JS 側の見え方 |
|---|---|---|
| ボリューム id が未登録 (Stamp の `/sd`) | `ESP_ERR_NOT_FOUND` | `"no such volume"` |
| 登録済みだが未マウント (カードなし) | `ESP_ERR_INVALID_STATE` | `"not mounted"` |
| `..` / 制御文字 / 長すぎ | `ESP_ERR_INVALID_ARG` | `"bad path"` |

共通 ops (POSIX VFS の上に載るだけなので、ボリューム種別を問わない):

```c
fs_list / fs_stat / fs_read / fs_write / fs_mkdir / fs_remove / fs_move / fs_copy
```

`fs_move` はボリュームをまたぐと `rename(2)` が使えないので、
自動で「コピーしてから元を消す」に落ちる。転送バッファは PSRAM
(SDMMC が PSRAM DMA 可なので内蔵 SRAM を食わない)。

---

## 6. 抜き挿し — 検出ピンが無い問題

Tab5 の SD 検出ピンは NC (§1.2)。割り込みは取れない。

- `fsvol_ops_t::probe()` は SD に対して軽いカードコマンド (CMD13) を打ち、
  失敗したら「抜かれた」と判定して自動的にアンマウントする。
- **タイマーは置かなかった。** `fsvol_resolve` が毎回 `probe()` を通すので、
  ファイルを触ろうとした瞬間に必ず最新の在/不在が分かる。周期ポーリングは
  「触っていない間も回る」ぶん電源方針 (`docs/power-states.md`) に反する上、
  最悪 2 秒古い答えしか出せない。一覧の各行の stat は解決済みの実パスを
  使うので、1 行ごとに CMD13 を打つことにはならない。
- 書き込みは `fopen`/`fwrite`/`fclose` + `fsync` で閉じ切る。最悪でも
  書いている最中の 1 ファイルしか失わない。
- UI に必ず「取り出し」を出す。押されたら `fsvol_unmount`。

---

## 7. 権限 — capability トークン

読み取りは全アプリに開放、**書き込み・削除・リネーム・マウント操作はトークン制**。

読み取りを開放してよい理由: 登録されるボリュームに秘密が無い。
System Vault と Wi-Fi 資格情報は NVS 側 (`vault.*` / Kconfig) で、
LittleFS に載っているのはアプリの JS と SKK 辞書だけ。
—— 秘密を LittleFS に置くようになったらこの判断をやり直すこと。

```js
fs.request({ path: "/sd", write: true, reason: "写真の整理" }, function (grant) {
    if (!grant) return;                  // 拒否された
    fs.mkdir(grant, "/sd/DCIM/2026");
    fs.write(grant, "/sd/DCIM/2026/note.txt", "hi");
    fs.release(grant);                   // 明示解放 (省略可)
});
```

`grant` は `net.onReady` のトークンと同じく**不透明な整数**。
理由 (net トークンと同じ): クラスも GC も要らず、`JS_ToInt32` 一発で検証でき、
「トップレベルでは絶対に手に入らない」形になる。

トークンが死ぬ条件 (どれも**使うときに見比べるだけ**で判定する。
失効させて回るコールバックも、期限を数えるタイマーも無い):
- `fs.release(grant)` — 明示的に返す
- 発行時の worker/世代が今の呼び出し元と一致しない = そのアプリは
  もう別物 (停止して入れ替わった)
- 発行時の `fsvol_epoch` とボリュームの現在の世代が違う = カードが
  抜き差しされた。**カードを抜いた瞬間に、そのカードへの権限が消える**

### 7.1 同意画面は誰が描くか

**ランチャーが描く。** 当初は C 側にプロバイダ構造体
(`mqjs_set_fs_consent_provider`) を置き、Tab5 では `ui_status.c` の
LVGL モーダルを注入する形を考えたが、実装してみると

- ランチャーは**停止不可の組み込みシステムアプリ**で、要求元とは別物
  という要件を最初から満たしている、
- 既存の `ui.screen` / `sys.signal` / `sys.focus` にそのまま乗るので
  新しい LVGL の C++ を 1 行も書かずに済む、
- UI の無いボードでは「ランチャーが居ない」= 尋ねる相手が居ない、が
  そのまま自動的な既定 (拒否) になる、

ので、C のプロバイダ層は要らなくなった。差分を隠す層をもう 1 枚
足すより、**もともと差分の外にある常駐アプリに任せる**方が薄い。

経路:

1. `fs.request()` が要求を保留表に積み、`{"op":"fs-consent", id, app,
   path, write, vol, reason}` をランチャーへ `EV_SIGNAL` で送る。
   差出人は `"system"` —— この名前は `sys.setAppName` が拒否するので、
   アプリは偽の許可要求を送れない。JSON の各文字列はエスケープして
   埋める (`reason` はアプリが自由に書ける)。
2. ランチャーが許可/拒否の画面を出し、`sys.fsConsent(id, ok)` で返す
   (`system_api_allowed` ゲート = 組み込みシステムアプリのみ)。
3. C が `EV_FSGRANT` を要求元へ送り、**JS タスクの上で** grant を発行して
   `cb(grant)` を呼ぶ。権限表を触るのが 1 タスクだけになるのでロックが要らない。

例外はひとつ、**dev スロットは同意を経ずに自動許可**する。
`camera.scanQr` / `sys.blackbox` / `system.*` と同じ最高権限を既に
持っており、ここだけ締めても新しい安全性は生まれない一方、MQTT で
押し込む probe が画面を触れずに止まってしまう。

ランチャーが居ないビルド (Stamp / ヘッドレス / 起動直後) では、
dev スロット以外は**黙って拒否**。尋ねる相手が居ないなら書かせない。

---

## 8. FATFS の文字コード

現状は `CODEPAGE_437` + `API_ENCODING_ANSI_OEM`。**API を UTF-8 に変える**
(`CONFIG_FATFS_API_ENCODING_UTF_8=y`)。今 FAT を使っている場所がゼロなので
切り替えの破壊リスクは無い。

コードページ (SFN 用) は別問題。LFN が有効なら長い名前は UTF-16 で
記録され、UTF-8 API で正しく読み書きできる。コードページが効くのは
**LFN エントリを持たない純 8.3 名**だけで、現代のカードではまれ。
一方 CP932 のテーブルは数十 KB 単位で flash を食う
(size diet 方針と正面衝突する)。

→ **まず UTF-8 API + CP437 で行く**。実機で化けるカードが出たら
`CONFIG_FATFS_CODEPAGE_932=y` に 1 行で切り替える。フラッシュ増分は
そのとき実測して判断する。

設定を置く場所に注意: **`sdkconfig.tab5*` は gitignore**。追跡される
`sdkconfig.defaults` (両ボード共通、Stamp では FATFS 未使用なので無害) と
`sdkconfig.tab5.defaults.example` の両方に入れる。片方だけだと新しい
worktree で消える (SDIO 設定で前科あり)。

---

## 9. 段階

| Phase | 内容 | 状態 |
|---|---|---|
| 1 | `components/fs_core` (登録簿 + パス解決 + 共通 ops)、`components/sdcard`、`main/storage.c` から内蔵を登録 | 実装済 (ビルド確認のみ) |
| 2 | `fs.*` バインディング + grant テーブル + 同意経路 + ROM 再生成 | 実装済 (ビルド確認のみ) |
| 3 | `examples/files.js` + ランチャーの同意画面 | 実装済 (run_pc で構文と「ボリューム 0 本」経路のみ確認) |
| 4 (未着手) | 画像プレビュー、SD からのアプリ実行、USB MSC、カメラ保存先としての SD、フォルダ丸ごとコピー |  |

**実機検証 (2026-08-25, Tab5 COM8)** — `tools/probe_fs.js` を dev スロットへ push。

確認できたこと:

| | |
|---|---|
| SD と C6 の同居 | `sdmmc_share: sharing the SDMMC controller (2 users)` → `H_SDIO_DRV: Card init success` → `wifi: got ip`。§1.5 の修正後、**両方が同時に上がる** |
| SD のマウント | slot 0 / 4-bit / HIGHSPEED で実カードがマウント (`fsvol: volume 'sd' mounted`) |
| 一覧 | `/internal/apps` 7 件を 50 ms |
| サンドボックス | `..` を含む 3 本 → `bad path`、`/littlefs/apps` と `/nosuchvol` → `no such volume`。**素通りゼロ** |
| grant 強制 | grant 無しの `fs.write` → `expected a grant from fs.request(...)` |
| 書き込み往復 | microSD へ 9 ms、読み戻し一致、削除まで |
| **日本語ファイル名** | `テスト.txt` を書いて一覧に同じ名前で出た。**§8 の判断 (UTF-8 API だけでよい / CP932 不要) が実機で裏取りできた** |

まだ確認していないこと (画面のタップが要る):

- ランチャーの同意画面 (dev スロットは自動許可なので probe では通らない経路)
- ファイラ本体の操作感、`sys.focus` の非同期との競合
- **カードを物理的に抜いたときの** probe → 自動アンマウント → grant 失効

ファイラは棚に載せてある (`<base>/store/files`)。ランチャー → ストア →
入手可能 → files → インストール、で入る。

バイナリは +15.4 KB (SDMMC/FATFS ドライバ込み)。

## 10. やらないと決めたこと

- **カード検出ピンの追加**: ハードが出していない。ポーリングで足りる。
- **フォーマット機能**: 押し間違いの被害が回復不能。PC でやればよい。
- **常駐の SD 監視タスク**: 電源方針に反する。前面にいる間だけ見る。
