#!/bin/sh
#
# run_tests.sh — fs_picker のホストテスト。
#
#   ./run_tests.sh          # 全部
#   KEEP=1 ./run_tests.sh   # build/ を残す (helpers.inc を見たいとき)
#
# 終了コード 0 = 全部緑。
#
# --------------------------------------------------------------------------
# ここで何が試せて、何が試せないか
#
# fs_picker.cpp のほとんどは LVGL と ui_tab5 と fs_core にべったりで、ホスト
# では意味を持たない。ホストで叩けるのは「静かに間違えるほう」——
# パスの連結・親への 1 段・拡張子の後方一致・保存名の形・同意画面の操作文言
# ——だけで、そこだけをファイル内のマーカーで囲んである。
#
# テストは**そのマーカーの間を切り出して**コンパイルする。写したコピーを
# 検査すると「本体は直っていないのに緑」になるので、そうしない。
#
# 見ていないもの: LVGL のオブジェクト構築、モーダルの重なり、タッチ、
# lv_timer の駆動、fs_dir_open の所要 (spec §F #7)、cb が UI タスクで
# 呼ばれること。すべて実機。
# --------------------------------------------------------------------------
set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE" || exit 1

SRC="../fs_picker.cpp"
FS_CORE_INC="../../fs_core/include"
BUILD_DIR="build"

[ -f "$SRC" ] || { echo "ERROR: $SRC が無い" >&2; exit 1; }

# ---------------------------------------------------------------- toolchain
# fs_core/pc_test と同じ流儀: native g++ が無ければ wsl。
CXX=""
RUN=""
if command -v g++ >/dev/null 2>&1; then
    CXX="g++"
elif command -v wsl >/dev/null 2>&1 && wsl g++ --version >/dev/null 2>&1; then
    CXX="wsl g++"
    RUN="wsl"
    echo "note: no native g++, using 'wsl g++'"
else
    echo "ERROR: g++ が見つからない (g++ と 'wsl g++' を試した)" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR" || exit 1

SAN=""
probe="$BUILD_DIR/.santest"
printf 'int main(void){return 0;}\n' > "$probe.cpp"
if $CXX -fsanitize=address,undefined "$probe.cpp" -o "$probe.bin" >/dev/null 2>&1; then
    SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
    echo "note: sanitizers enabled"
else
    echo "note: sanitizers unavailable, running without"
fi
rm -f "$probe.cpp" "$probe.bin"

# ------------------------------------------------------------- 切り出し
BEGIN='>>> host-testable helpers begin <<<'
END='>>> host-testable helpers end <<<'
INC="$BUILD_DIR/helpers.inc"

awk -v b="$BEGIN" -v e="$END" '
    index($0, b) { on = 1; next }
    index($0, e) { on = 0; done = 1; next }
    on           { print }
    END          { exit done ? 0 : 1 }
' "$SRC" > "$INC" || {
    echo "ERROR: $SRC にマーカーが無い ('$BEGIN' / '$END')。" >&2
    echo "       関数を動かしたなら、マーカーも一緒に動かすこと。" >&2
    exit 1
}

if [ ! -s "$INC" ]; then
    echo "ERROR: 切り出した塊が空。マーカーの順序が逆かもしれない。" >&2
    exit 1
fi

# 切り出した塊が本当に「純粋」か。IDF/LVGL の型が紛れ込んだら、この
# テストは意味を失うのでその場で落とす (静かに範囲外へ滑るのを防ぐ)。
# (コメント行は落としてから見る。判定は「その名前で **呼んでいる**」——
#  識別子のうしろに '(' が来る形——に絞る。コメントで名前に言及しただけで
#  落ちると、由来を書き残せなくなる。)
if grep -vE '^[[:space:]]*(/\*|\*|//)' "$INC" \
       | grep -E '(lv_|esp_|ESP_LOG|fsvol_|fs_dir_|LV_)[A-Za-z0-9_]*[[:space:]]*\(' >&2; then
    echo "ERROR: マーカーの内側に LVGL/IDF を呼ぶ行がある (上の行)。" >&2
    echo "       純粋な関数だけをマーカーの内側に置くこと。" >&2
    exit 1
fi
echo "note: helpers.inc = $(wc -l < "$INC") 行"

# -------------------------------------------------------------------- run
CXXFLAGS="-std=gnu++17 -O1 -g -Wall -Wextra -fno-exceptions -fno-rtti"
CXXFLAGS="$CXXFLAGS -I$FS_CORE_INC -I$BUILD_DIR -I."

failed=""
for src in test_*.cpp; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .cpp)
    bin="$BUILD_DIR/$name"
    printf '\n=== %s ===\n' "$name"
    # shellcheck disable=SC2086
    if ! $CXX $CXXFLAGS $SAN "$src" -o "$bin"; then
        echo "FAIL $name: コンパイルできない"
        failed="$failed $name(build)"
        continue
    fi
    # shellcheck disable=SC2086
    if $RUN "./$bin"; then
        printf 'PASS %s\n' "$name"
    else
        echo "FAIL $name: exit $?"
        failed="$failed $name"
    fi
done

# ------------------------------------------------- 2 本目: 構文と型の検査
#
# fs_picker.cpp の**全体**を、本物の LVGL 9.4 / fs_core / fs_grant /
# ui_tab5 のヘッダに突き合わせて -fsyntax-only に通す。ESP-IDF のヘッダ
# だけ hoststub/ の薄い偽物で置き換える (詳細と限界は hoststub/README.md)。
#
# これが無いと、デバイスを持たない実装者には「lv_obj_set_size の引数を
# 1 本取り違えた」を見つける手段が無い。走らせてはいないので、これは
# 「動く」の証拠ではなく「綴りと型が合っている」の証拠でしかない。
LVGL_DIR="../../../managed_components/lvgl__lvgl"
if [ -d "$LVGL_DIR" ] && [ -f "$LVGL_DIR/lv_conf_template.h" ]; then
    printf '\n=== syntax (fs_picker.cpp vs real LVGL headers) ===\n'
    mkdir -p "$BUILD_DIR"
    # LVGL 同梱テンプレートの 15 行目 "#if 0 /* Set this to 1 ... */" を
    # 1 にするだけ。デバイスの sdkconfig とは別物 (README 参照)。
    sed '15s|.*|#if 1|' "$LVGL_DIR/lv_conf_template.h" > "$BUILD_DIR/lv_conf.h"
    # shellcheck disable=SC2086
    if $CXX -fsyntax-only -std=gnu++17 -fno-exceptions -fno-rtti \
            -Wall -Wextra \
            -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
            -I "$BUILD_DIR" -I hoststub -I "$LVGL_DIR" \
            -I "$FS_CORE_INC" -I ../../ui_tab5/include -I ../../ui_tab5 \
            -I ../include ../fs_picker.cpp; then
        echo 'PASS syntax'
    else
        echo 'FAIL syntax'
        failed="$failed syntax"
    fi
else
    echo 'note: managed_components/lvgl__lvgl が無いので syntax スイートは飛ばす'
fi

printf '\n==================== summary ====================\n'
[ -n "${KEEP:-}" ] || rm -rf "$BUILD_DIR"
if [ -n "$failed" ]; then
    printf 'FAILED:%s\n' "$failed"
    exit 1
fi
printf 'ALL SUITES PASSED\n'
exit 0
