# PIE 手書き融合カーネル計画 — bc_locate 構造テンソル

ステータス: **実機検証 PASS (2026-06-13)**。起動セルフチェック一致 →
`pie` パスで稼働、loc **13-15ms → 6-7ms/フレーム** (目標 4-7ms 達成)。
実本の ISBN (9784065419526) も PIE 経路でデコード完走。
初版から 2 つの実機修正が必要だった (どちらもセルフチェックが検出):

1. **QACC → XACC**: 初版は `esp.vmulas.s16.qacc` で QACC に積みながら
   `esp.srs.s.xacc` (XACC を読む) で抽出していた — 積む先と読む先が別。
   esp-dsp `dsps_dotprod_s16_arp4` の逆アセンブルが正解: reduce-sum
   内積は `esp.zero.xacc` + `esp.vmulas.s16.xacc` + `esp.srs.s.xacc` の
   XACC (スカラ 40bit) 系で揃える。
2. **hwlp 終端ラベルは「含む」規約**: `esp.lp.setupi` の終端ラベルは
   本体最後の命令の上に置く (esp-dsp f32 版 `.dotprod_loop` と同じ)。
   本体の直後に置くと次の命令 (行カウンタ addi) がループへ取り込まれ、
   30 行中 8 行しか処理されない。
関連: `components/cam_tab5/bc_locate.c` 冒頭の計測コメント、コミット 242665f。

## 0. §6 解決結果 (2026-06-13、xesppie ツールチェーンで確認)

着手前の未確認事項 (§6) はすべて解決した。使用ニーモニックは
`riscv32-esp-elf-gcc -march=rv32imafc_zicsr_zifencei_xesppie` で
アセンブル・逆アセンブル確認済み (IDF の P4 既定 march と完全一致、
よって `xesppie` は既定で有効 = ppa_bench の vld/vst が無フラグで通る理由)。

- **レーン別シフト命令**: 不要だった。`esp.andq` で 0x07E0 を全レーンに
  マスクし緑フィールドを `G<<5` (=32×) のまま使う §4 案を採用。勾配 32×・
  積 1024× を XACC に積み、抽出時 `esp.srs.s.xacc rd, 10` の算術右シフト
  で正確に復元 (整数和×2^10 なので丸めゼロ)。ホスト・エミュレーションで
  C 基準とビット一致を確認 (負の Sxy 含む 8 ブロック)。
- **アキュムレータと退避/抽出**: reduce-sum 内積は **XACC** (スカラ 40bit、
  実機検証で確定 — 冒頭ステータス参照)。1 本しかなく 3 和を共有できない
  ので **3 パス**構成。各パス頭で `esp.zero.xacc`、`esp.vmulas.s16.xacc q,q`
  (= dsps_dotprod_s16_arp4 と同じ意味論) で積和、末尾で
  `esp.srs.s.xacc t, t(=10)` により 32bit を GPR へ取り出して store。
  ブロック 2KB は 3 パス間 L2 常駐なので追加読みは安価。
- **アンアラインド `esp.vld.128`**: 可。cfg レジスタの bit1 を立てて有効化
  (`esp.movx.r.cfg` / `ori ,2` / `esp.movx.w.cfg`)。dsps_dotprod_s16_arp4
  と同一手法。±1px (偶数バイトだが非 16B) 窓を直接ロードできる。
- **Xhwlp ネスト**: 回避。`esp.lp.setupi 0,4,end` を**列 4 チャンクの 1 段
  のみ**に使用 (本体はマスク表のポインタ増分ロードで分岐ゼロ)。行ループ
  (30) と 3 パスは通常分岐。esp-dsp 自身も `esp.lp.setup` をコメントアウト
  して手動分岐へ退避していた → ネストは踏まない設計。
- **march 変更なしで通るか**: 不要。IDF の P4 march が既に `_xesppie` を
  含む。算術系 (`esp.vmulas.s16.qacc` / `esp.srs.s.xacc` / `esp.andq` 等)
  も同 march で通る。念のため CMake で `bc_tensor_p4.S` にファイルスコープ
  `-march=...xesppie` を APPEND してピン留め。
- 不採用ニーモニック: `esp.vand.128` (未定義 → `esp.andq` を使用)、
  `esp.movx.r.xacc.l/.h` (operand 形式不一致 → 抽出は `esp.srs.s.xacc`)。

## 1. 背景と実測

バーコード領域検出 (bc_locate) はスキャン中 3 フレームに 1 回、
400x300 の解析画像(PPA がハードウェアで生成する中央クロップの 1/2
縮小)全面に 32px ブロックの構造テンソル (Sxx/Syy/Sxy) を計算する。
`camera.status()` 末尾の `[pv loc scan ms/f]` テレメトリでの実測:

| 実装 | loc (1 実行あたり) |
|---|---|
| 融合 C ループ(現行) | **11-20ms** |
| esp-dsp `dsps_dotprod_s16` (PIE _arp4、非アライン) | 23-50ms |
| 同上(16B アライン済み) | 22-23ms |

ライブラリ PIE が負けた理由: ブロックあたり 3 回 × 108 ブロック =
324 回の短い呼び出し + 勾配配列の実体化(書いて読み直す追加メモリ
パス)が、ベクトル MAC の節約を上回る。融合ループは変換・差分・積和
をレジスタ内で完結させ、画素を 1 回しか読まない。

**結論: PIE で勝つ唯一の形は、ロード+変換+差分+積和を 1 パスに
融合した手書きアセンブリ**(中間値をメモリに置かない)。

## 2. 現状バイナリの事実(2026-06-13 調査)

- 実効 `-march=rv32imafdc_zicsr_zifencei` — **PIE (xesppie) も
  Xhwlp (ハードウェアループ) も含まれない**。
- `bc_locate.c.obj` 逆アセンブル(620 命令)に `esp.*` / `lp.*` は
  ゼロ件。**GCC はどちらの拡張も自動生成しない**(自動ベクトル化
  なし、ゼロオーバーヘッドループ変換なし)。
- ただしアセンブラ (gas) は `esp.` ニーモニックをインラインアセンブリ
  で受け付ける(前例: `main/ppa_bench.c` の `esp.vld.128.ip` /
  `esp.vst.128.ip`、拡張名 xespv2p1 とコメントされている)。
  → .S ファイルか asm volatile で書けば march 変更は不要(要再確認)。

## 3. ターゲットカーネル仕様

入力: `s_mid` (RGB565, 400x300, 64B アライン, PSRAM)。
ブロック: 32x32 px、12x9 = 108 個。
出力: ブロックごとに int32 の Sxx, Syy, Sxy。

数学(現行 C と完全一致させること):
- ルマ: 6bit 緑 `l = (v >> 5) & 0x3F`
- 中心差分: `gx = l[y][x+1] - l[y][x-1]`, `gy = l[y+1][x] - l[y-1][x]`
  (内側 30x30 = 900 点)
- `Sxx = Σgx²`, `Syy = Σgy²`, `Sxy = Σgx·gy`

値域: |gx| ≤ 63、積 ≤ 3969、和 ≤ 3.57M → 32bit 累積で安全。

## 4. レジスタ・命令計画

8 レーン × 16bit (q レジスタ 128bit) を基本とする。1 ブロック行
32px = 4 ベクトル。

```
行バッファ戦略 (ブロックごと):
  q0..q3 : 行 y-1 のルマ (32px)
  q4..q7 : 行 y+1 のルマ
  行 y のルマは gx 用に ±1px ずらし窓が必要 → アンアラインド
  esp.vld.128 を 2 回 (offset -2B / +2B) して直接「ずれた」ベクトル
  を作る方式が最少命令 (シャッフル命令への依存を避ける)

1 反復 (8px) の流れ:
  1. esp.vld.128      : RGB565 ロード ×(必要本数)
  2. G6 抽出          : 右シフト 5 + AND 0x3F を全レーンに
                        → P4 PIE のレーン別シフト命令名は要確認 (§6)。
                        代替案: AND 0x07E0 のみで G<<5 スケールのまま
                        進め、E_MIN / コヒーレンス比較を ×1024 側に
                        合わせる (比率は不変)。シフト命令が見つから
                        なくても AND だけで成立する。
  3. 差分             : esp.vsub.s16 (gx = 右窓 - 左窓, gy = 下 - 上)
  4. 積和             : esp.vmulas.s16.qacc
                        ※ QACC は 1 系統しかない前提で設計する:
                        Sxx/Syy/Sxy は「ブロック内 3 パス」または
                        「QACC 退避 (esp.srcmb 系) を挟む」のどちらか。
                        3 パス案は行データをレジスタに保持したまま
                        回せれば追加ロードなしで済む (32x32 ルマは
                        2KB = レジスタには乗らないので、行ペア単位で
                        3 累積を回す行内 3 パスが現実的)
  5. QACC 抽出        : esp.srcmb.s16.qacc 等で 32bit 和を取り出し
                        (正確な抽出列は esp-dsp の _arp4 実装を写経)

Xhwlp (ゼロオーバーヘッドループ):
  - esp.lp.setup / esp.lp.setupi で内側 8px ループと行ループを包む。
    分岐 + カウンタ更新 (~2-3 cycle/iter) が消える。
  - C ループへの後付けは不可 (コンパイラがループレジスタを知らない)。
    必ずこの asm カーネル内で使う。
  - ネスト可否 (2 段) は TRM で要確認 (§6)。不可なら外側ループのみ C。
```

## 5. 検証計画(isolation-first)

1. **ホスト C リファレンスとの厳密一致**: カーネルの入出力仕様を
   `bc_tensor_block(const uint16_t *base, int stride, int32_t out[3])`
   に切り出し、C 版(現行ループ)を host/device 両方でビルド可能に
   しておく。デバイス起動後の初回スキャン前に、PRNG で埋めた合成
   ブロック ×16 で asm 版と C 版を照合し、不一致なら
   `camera.status()` に "tensor asm MISMATCH" を出して C 版へ
   フォールバック(シリアルなし運用での安全網)。
2. 既存ホストスイート(回転 7 角度 + 弱コントラスト + 背景棄却)は
   C 版で従来どおり回る(アルゴリズム回帰の検出)。
3. テレメトリ前後比較(loc ms)+ 実本スキャン。

## 6. 着手前に TRM / esp-dsp ソースで確認すること(すべて解決 → §0)

- [x] P4 PIE のレーン別シフト命令の正確な名前(s16 右シフト)。
      → 不要。AND-only (0x07E0) + 末尾 `>>10` で確定 (§0)。
- [x] QACC の本数と退避/復帰命令。→ 1 系統、3 パス、
      `esp.vmulas.s16.qacc` 積和 + `esp.srs.s.xacc` 抽出 (§0)。
- [x] アンアラインド esp.vld.128 の可否。→ 可。cfg bit1 で有効化 (§0)。
- [x] Xhwlp のネスト可否。→ ネスト回避、列 1 段のみ `esp.lp.setupi`(§0)。
- [x] march 変更なしに通るか。→ IDF P4 march が既に `_xesppie` 含む。
      算術系も通る。CMake でファイルスコープ pin (§0)。

## 7. 期待効果と着手条件

- 見込み: loc 11-20ms → **4-7ms**(変換+差分+MAC の 3-4 倍化、
  メモリ読みはそのまま)。3 フレームに 1 回なので**フレーム平均では
  約 2-4ms の短縮 = 全体の ~5%**。
- 現在の支配項は PPA のピクセルレート(pv ~36ms、§: PPA は入力画素数
  律速 ~17.5Mpx/s)であり、この最適化の体感効果は小さい。
- **着手条件(いずれか)**:
  - 走査線/領域候補を大幅に増やして CPU 項が再び支配的になったとき
  - bc_locate を毎フレーム実行したくなったとき(追従性向上)
  - PPA 依存を外す構成変更(クロップ拡大等)で pv が縮んだとき

## 8. 参考

- PIE 入門 (Espressif blog):
  https://developer.espressif.com/blog/2024/12/pie-introduction/
- esp-dsp P4 アセンブリ実例: modules/dotprod/fixed/dsps_dotprod_s16_arp4.S
- リポジトリ内の PIE 前例: main/ppa_bench.c (vld/vst + 16B アライン)
- 計測手段: camera.status() 末尾の "[pv loc scan ms/f]"
