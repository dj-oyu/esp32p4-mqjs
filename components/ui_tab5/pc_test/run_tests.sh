#!/bin/sh
#
# run_tests.sh — ui_tab5 のセル描画コアのホストテスト。
#
#   ./run_tests.sh          # 全部
#   KEEP=1 ./run_tests.sh   # build/ を残す
#
# 終了コード 0 = 全部緑。
#
# --------------------------------------------------------------------------
# ここで何が試せて、何が試せないか
#
# ui_tab5.cpp のほとんどは LVGL・mooncake・esp_lcd・PPA ドライバに
# べったりで、ホストでは意味を持たない。ホストで叩けるのは
# **セルの run を分割して描く算術** —— どの列に何を描くか、run を
# seg_max で切る場所、幅 2 のグリフをまたがせない条件、ポインタ+長さの
# 境界 —— で、それが ui_tab5_surf.inc に分けてある理由でもある。
#
# テストは ui_tab5_surf.inc を **そのまま** #include し、UTF-8 デコードと
# セル幅のヘルパも ui_tab5.cpp からマーカーで切り出して使う。写した
# コピーを検査すると「本体は直っていないのに緑」になるので、そうしない。
#
# 見ていないもの: 本物の PPA (設定フィールドはこのテストの偽物の定義に
# 合わせてある)、キャッシュ整合、LVGL への提示と invalidate、回転、
# 2 つのタスクが同時に描いたときのちらつき、実フォントのグリフ形状。
# すべて実機。
#
# --------------------------------------------------------------------------
# 通ったことにする前に壊して確かめた欠陥 (計画 §1 の規則)
#
# 2026-08-26、下の 25 通りを 1 つずつ注入して**全部が落ちること**を見た。
# 直したあとに緑になることも確認済み。ここを増やしたら同じことをやる。
#
#   ui_tab5_surf.inc / cells_utf8_next
#     1  cells_utf8_next の e 境界を外す      -> ASAN heap-buffer-overflow
#     2  CPU 経路の `c - col < n` を外す      -> t_ncells_shorter_than_text
#     3  分割の straddle rewind を外す        -> t_split_wide
#     4  ステージング無しの seg_max=n を外す  -> t_no_staging
#     5  A8 の 64B 整列チェックを外す         -> t_unaligned_staging
#     6  take==0 の早期 break を外す          -> ASAN global-buffer-overflow
#     7  PPA compose 経路の `c < n` を外す    -> t_ncells_shorter_than_text
#   提示メータ
#     P1 外れ値の上限を外す                   -> t_outlier_counted
#     P2 s_ppa_rot_us を提示ごとに戻さない    -> t_rot_per_presentation
#     P3 RENDER_START の per-refresh reset    -> t_worst_is_widest
#     P4 報告後に窓を空にしない               -> t_window
#     P5 canvas mark を消費しない             -> t_canvas_attribution
#     P6 最初の提示で窓の起点を置かない       -> t_window
#     P7 最悪を「最新」に取り違える           -> t_worst_is_widest
#     P8 イベントを 5 本しか登録しない        -> t_attach
#   native surface の入口
#     N1 get_content_coords -> get_coords     -> t_native_invalidate
#     N2 x2/y2 の -1 を落とす                 -> t_native_invalidate
#     N3 右下のクランプを外す                 -> t_native_invalidate_clamped
#     N4 lvgl_port_unlock を落とす            -> t_native_invalidate
#     N5 キャンバス未生成のガードを外す       -> t_native_no_canvas
#     N6 canvas mark を立てない               -> t_native_invalidate
#     N7 左上のクランプを外す                 -> t_native_invalidate_clamped
#   回転中の寸法 (ロックを取らない代金)
#     G1 surf_geom の確保サイズ判定を外す     -> ASAN heap-buffer-overflow
#     G2 surf_fill_rect の寸法スナップショット-> ASAN heap-buffer-overflow
#     G3 surf_blit_glyph の同上               -> ASAN heap-buffer-overflow
#   ヘッダ
#     H1 スタブに未定義の識別子を書く         -> header (C / C++, UI off)
#
# 2 と 7 は最初 **落ちなかった**: 画素だけ見ていると run_right / dst_w の
# clamp が余分なグリフを無音で捨ててしまう。clamp が働いた回数
# (cells_note_clip) を見るようにして初めて落ちるようになった。
# 3 と P6 も同じで、境目が CONT に落ちる文字列と 0 近くの時計では
# その欠陥を一度も通らなかった。**テストが通ることに意味を持たせたのは
# この 4 件の作り直しであって、最初に書いた assert ではない。**
# --------------------------------------------------------------------------
set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE" || exit 1

SRC="../ui_tab5.cpp"
BUILD_DIR="build"

[ -f "$SRC" ] || { echo "ERROR: $SRC が無い" >&2; exit 1; }
[ -f "../ui_tab5_surf.inc" ] || { echo "ERROR: ../ui_tab5_surf.inc が無い" >&2; exit 1; }

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
# ui_tab5.cpp のマーカーの間を切り出す。写しではなく本体を試すため。
cut_between() { # $1=begin $2=end $3=out
    awk -v b="$1" -v e="$2" '
        index($0, b) { on = 1; next }
        index($0, e) { on = 0; done = 1; next }
        on           { print }
        END          { exit done ? 0 : 1 }
    ' "$SRC" > "$3" || {
        echo "ERROR: $SRC にマーカーが無い ('$1' / '$2')。" >&2
        echo "       関数を動かしたなら、マーカーも一緒に動かすこと。" >&2
        return 1
    }
    [ -s "$3" ] || { echo "ERROR: 切り出した塊が空: $3" >&2; return 1; }
    echo "note: $(basename "$3") = $(wc -l < "$3") 行"
}

cut_between '>>> host-testable cell helpers begin <<<' \
            '>>> host-testable cell helpers end <<<' \
            "$BUILD_DIR/cellhelpers.inc" || exit 1
cut_between '>>> host-testable presentation meter begin <<<' \
            '>>> host-testable presentation meter end <<<' \
            "$BUILD_DIR/profmeter.inc" || exit 1
cut_between '>>> host-testable native surface begin <<<' \
            '>>> host-testable native surface end <<<' \
            "$BUILD_DIR/native.inc" || exit 1

# セルのヘルパは本当に「純粋」か。IDF/LVGL の型が紛れ込んだら、この
# テストは意味を失うのでその場で落とす。(コメント行は落としてから見る。)
# 提示メータのほうは LVGL のイベントと esp_timer を**わざと**使うので、
# この検査は掛けない (test_prof.cpp がその 2 つを偽物で置き換える)。
if grep -vE '^[[:space:]]*(/\*|\*|//)' "$BUILD_DIR/cellhelpers.inc" \
       | grep -E '(lv_|esp_|ESP_LOG|ppa_)[A-Za-z0-9_]*[[:space:]]*\(' >&2; then
    echo "ERROR: マーカーの内側に LVGL/IDF/PPA を呼ぶ行がある (上の行)。" >&2
    exit 1
fi

# -------------------------------------------------------------------- run
CXXFLAGS="-std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter"
CXXFLAGS="$CXXFLAGS -fno-exceptions -fno-rtti"
CXXFLAGS="$CXXFLAGS -DCONFIG_MQJS_TAB5_UI=1"
CXXFLAGS="$CXXFLAGS -I$BUILD_DIR -I. -I../include -I.. -Ihoststub"

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

# ------------------------------------------------- 2 本目: ヘッダの検査
#
# ui_tab5.h の**両方の枝**をコンパイルする。CONFIG_MQJS_TAB5_UI=n の
# 枝はデバイスのビルドでは一度も通らないので (Stamp ビルドでしか通らず、
# そちらは手元で焼かない)、宣言とスタブの食い違いはここでしか出ない。
# C と C++ の両方から使われるヘッダなので両方で見る。
printf '\n=== header (ui_tab5.h, UI on/off, C and C++) ===\n'
hdr_ok=1
printf '#include "ui_tab5.h"\nint main(void){return 0;}\n' > "$BUILD_DIR/hdr.c"
cp "$BUILD_DIR/hdr.c" "$BUILD_DIR/hdr.cpp"
CC_FOR_H=$(printf '%s' "$CXX" | sed 's/g++/gcc/')
for def in "-DCONFIG_MQJS_TAB5_UI=1" ""; do
    # shellcheck disable=SC2086
    if ! $CC_FOR_H -fsyntax-only -std=gnu11 -Wall -Wextra $def \
            -I ../include -I hoststub "$BUILD_DIR/hdr.c"; then
        echo "FAIL header (C, ${def:-UI off})"; hdr_ok=0
    fi
    # shellcheck disable=SC2086
    if ! $CXX -fsyntax-only -std=gnu++17 -Wall -Wextra $def \
            -I ../include -I hoststub "$BUILD_DIR/hdr.cpp"; then
        echo "FAIL header (C++, ${def:-UI off})"; hdr_ok=0
    fi
done
if [ "$hdr_ok" = 1 ]; then
    echo 'PASS header'
else
    failed="$failed header"
fi

printf '\n==================== summary ====================\n'
[ -n "${KEEP:-}" ] || rm -rf "$BUILD_DIR"
if [ -n "$failed" ]; then
    printf 'FAILED:%s\n' "$failed"
    exit 1
fi
printf 'ALL SUITES PASSED\n'
exit 0
