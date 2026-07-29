# vendored mquickjs へのローカルパッチ

`components/mqjs/mquickjs/` は submodule ではなく展開コピー。upstream は
<https://github.com/bellard/mquickjs>（bellard.org 側には `mquickjs-extras.tar.xz`
しか無く、ソース tarball は配布されていない）。

**2026-07-29 以前はローカルパッチ 0 件**で、vendored の中身は upstream HEAD
`203d5bb`（2026-06-04）とバイト単位で一致していた。以後は本ドキュメントの
差分だけが乗る。同梱 `Changelog` は `2025-12-22: First public version` の
1 行しか無く更新されないので、**バージョン判定に使ってはいけない**。

## 現在のパッチ: computed-goto ディスパッチ

対象は `mquickjs.c` の `JS_Call` のみ、3 ハンク。すべて
`#ifdef JS_COMPUTED_GOTO` で囲ってあり、`JS_NO_COMPUTED_GOTO` を定義すれば
upstream と同じ switch にフォールバックする（GCC 以外でも自動でそうなる）。

| # | 位置 | 内容 |
|---|---|---|
| 1/3 | `JS_Call` のループ先頭 | `CASE`/`DEFAULT`/`BREAK` をラベル形式に切り替え、`dispatch_table[256]` を定義して最初の `DISPATCH()` を撃つ。`for(;;)` と `switch` の代わりにブロックを 2 つ開く |
| 2/3 | `default:` の直前 | switch に case が無い 6 個の opcode にラベル別名を張る |
| 3/3 | `restart:` | 「もう一周」を `DISPATCH()` にする。閉じ括弧 2 つはハンク 1 のブロックを閉じる |

### なぜ必要か

ESP-IDF は `-fno-jump-tables -fno-tree-switch-conversion` を**全 TU**に付ける
（`esp-idf/CMakeLists.txt`。flash の .rodata に落ちるジャンプテーブルを
IRAM 配置コードから触らせないため）。結果、約 200 分岐の opcode switch が
**二分探索の比較チェーン**にコンパイルされ、バイトコード 1 個あたり 8〜14 本の
条件分岐を通っていた。

`components/mqjs/CMakeLists.txt` でエンジンの 4 ファイルに限り
`-fjump-tables -ftree-switch-conversion` を戻している（IDF の
api-guides/memory-types が「IRAM 配置が不要なファイルでは個別に戻してよい」と
明記）。エンジンは JS タスク専用で ISR から呼ばれず、`JS_Call` は flash 常駐
なので条件を満たす。computed goto はその上に乗り、ディスパッチを
114 箇所に分散させて BTB を効かせる（ESP32-P4 の HP コアは 5 段 in-order +
BHT/BTB/RAS）。

### 実測（riscv32-esp-elf 15.2.0、-O2、mquickjs.c 単体）

| | JS_Call .text | JS_Call 条件分岐 | 間接ジャンプ | TU 全体の分岐 | flash 合計 |
|---|---|---|---|---|---|
| 変更前 | 10,968 B | 353 | 1（末尾呼出） | 3,839 | 114,095 B |
| 変更後 | 10,480 B | 223 | 115 | 3,410 | 117,955 B |

flash は +3.9KB。JS_Call は逆に 488 B 縮む。TU 全体の分岐が 3,839→3,410 と
JS_Call の分より大きく減っているのは、`-fjump-tables` がコンパイラ本体・
正規表現エンジン・JSON パーサの switch にも効いているため（JS_Call は
18,366 行のうち 1 割程度しかない）。

### 実行時間の実測（Tab5 実機 @ 360 MHz、2026-07-29）

`examples/bench.js` と同じベンチ本体を MQTT で報告させ、同一デバイス・同一
ビルド構成で before/after を取ったもの（3 パスの中央値）。

| ベンチ | 変更前 | 変更後 | |
|---|---:|---:|---:|
| fib(24) 再帰 | 307 ms | **188 ms** | **−39%** |
| sieve 50k | 380 ms | **219 ms** | **−42%** |
| array map+filter+reduce 5k | 29 ms | **20 ms** | **−31%** |
| regexp scan 2k | 27 ms | 25 ms | −7% |
| JSON 往復 200 | 39 ms | 41 ms | ±0 |
| string build 20k | 504 ms | 533 ms | ±0 |
| Math.sin/cos 50k | 1,570 ms | 1,474 ms | −6% |
| **合計** | **2,877 ms** | **2,511 ms** | **−13%** |

**VM のディスパッチが支配的なベンチだけが 30〜42% 速くなり、そうでないものは
動きません。** これが正しい形です — `string build` は O(n²) の memcpy 律速、
`Math.sin/cos` は mquickjs 同梱の移植版 libm 律速、`JSON` は割り当て律速で、
どれもバイトコード 1 個あたりの分岐数とは無関係。合計が −13% にとどまるのは
sincos だけで全体の半分を占めているからで、**JS のロジックそのものは実質
1.6 倍**です。

静的な分岐数（353 → 223、−37%）と fib/sieve の実測（−39%/−42%）がほぼ
一致しているので、効いている機構は狙いどおりディスパッチです。

### 安全側の性質

upstream が **case ラベルの無い opcode を追加**した場合、`DEF()` から生成される
テーブル要素が存在しないラベルを参照するので
`error: label 'OPL_OP_x' used but not defined` で**ビルドが落ちる**。
黙って `default:` に落ちたり未定義動作になったりはしない。落ちたらハンク 2/3 の
別名リストにその opcode を足す。

現在の別名リスト（HEAD `203d5bb` 時点）:
`invalid, nop, dup1, push_const8, fclosure8, push_empty_string`

### upstream を取り直す手順

1. `git clone https://github.com/bellard/mquickjs` して
   `diff -rq --exclude=tests <clone> components/mqjs/mquickjs` で差分を確認
   （本パッチ以外に差分が無いことの確認も兼ねる）
2. `mquickjs.c` 以外はそのまま上書きしてよい
3. `mquickjs.c` は上記 3 ハンクを当て直す。`git diff` で旧版のハンクを取り出し、
   3-way merge するのが早い
4. ビルドし、`error: label 'OPL_OP_x' used but not defined` が出たら別名リストを更新
5. `riscv32-esp-elf-objdump -d --section=.text.JS_Call` で間接ジャンプが
   100 本以上あることを確認（1 本だけならフォールバックに落ちている）

## 関連

- 調査の全体像と他の候補（XIP-from-PSRAM、flash QIO、GC）は
  [runtime-hotspot-audit.md](runtime-hotspot-audit.md) と併せて参照
- GC は今回スコープ外（体感のボトルネックが無いため）。将来、複雑で長寿命な
  アプリが出てきたら着手する。分かっていること:
  - GC は bump アロケータが尽きたときだけ発火し、`gc_compact_heap` は
    **確保済み領域を 3 回リニアスイープ**する（thread / memmove / rehash）。
    スイープ長は live set ではなくアリーナサイズ（~250KB、PSRAM）
  - 3 パス目の rehash は upstream 自身が冗長と認めている
    （`/* XXX: try to do it in the previous pass */`）
  - mark stack は heap_free と JS スタックの隙間に置かれ、GC 発動時点の
    隙間は `JS_MIN_FREE_SIZE`(512B) 未満＝ JSValue 100 個前後。溢れると
    `while (s->overflow)` でヒープ全体を再スイープする。深いデータ構造
    （連結リスト、深い JSON）で踏みうる
