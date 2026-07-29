#!/bin/sh
# ssh_vt の IME 台本テストを run_pc で走らせる (docs/skk-ime-design.md S6)。
#
#   tools/ssh_vt_imetest.sh [/path/to/run_pc]
#
# examples/ssh_vt.js の "@imetest-inject" マーカー行を tools/ssh_vt_imetest.js.inc
# の中身に差し替えたものを一時ファイルに吐き、run_pc に食わせる。台本は ssh_vt
# のクロージャに届く必要があるので注入でしか成立しないが、この方式なら出荷
# アプリにはテストのバイトが 1 つも乗らない。
#
# run_pc の作り方は README の「PC だけでスクリプトを試す」を参照。cwd は
# run_pc の相対パス解決に効くので components/mqjs に移る (台本の既定辞書
# ../skk_core/skk_dict.bin がそこから解決される)。
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
app="$root/examples/ssh_vt.js"
inc="$root/tools/ssh_vt_imetest.js.inc"
run=${1:-/tmp/run_pc}
out=${TMPDIR:-/tmp}/ssh_vt_imetest.js

[ -x "$run" ] || { echo "run_pc がない: $run (README のレシピで作る)" >&2; exit 2; }

# マーカーはちょうど 1 個であること。2 個あると (説明文の中でマーカー名を
# 書いてしまう等) 最初の出現へ注入され、クロージャの外で台本が走って
# 意味不明な TypeError になる。実際にやらかしたのでここで止める。
n=$(grep -c '@imetest-inject' "$app" || true)
[ "$n" = 1 ] || {
    echo "マーカー @imetest-inject が $app に $n 個 (1 個であること)" >&2
    exit 2
}

# マーカーを含むコメントブロック (行頭 /* から */ まで) を台本で置き換える。
awk -v inc="$inc" '
    /@imetest-inject/ { skip = 1 }
    skip {
        if (/\*\//) { skip = 0; while ((getline line < inc) > 0) print line }
        next
    }
    { print }
' "$app" > "$out"

cd "$root/components/mqjs"
# タイムアウト: 台本は同期で終わるが、壊れると ssh_vt がイベントループに
# 居座るので必ず被せる。stdbuf は print を行単位で見るため。
timeout 30 stdbuf -o0 "$run" "$out" 2>&1 | tee "$out.log" | grep -E '^(FAIL |ssh_vt IME selftest)'
grep -q 'ssh_vt IME selftest: PASS' "$out.log"
