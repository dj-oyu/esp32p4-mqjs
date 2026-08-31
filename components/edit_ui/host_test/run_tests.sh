#!/bin/sh
#
# run_tests.sh — build and run the edit_ui presenter test on a host.
#
# POSIX sh, same toolchain fallback as components/edit_core/host_test and
# components/fs_core/pc_test: native gcc if there is one, otherwise
# `wsl gcc` (Windows dev boxes where the only toolchain lives inside WSL).
#
#   ./run_tests.sh          # build + run
#   KEEP=1 ./run_tests.sh   # leave the build tree behind
#
# Exit status: 0 when every CHECK passes.
#
# --------------------------------------------------------------------------
# Why edit_ui can be tested here at all, when it is "device only"
#
# It cannot, in the sense that matters most — nothing here proves anything
# is on the screen, and every number in spec §C.1 needs the Tab5. What CAN
# be pinned down on a host is the part that is pure wiring: which rows the
# presenter repaints, whether adjacent ones are merged into one rectangle,
# whether the geometry is derived from ui_tab5_canvas_size/kb_reserved or
# quietly hard-coded, and whether edit_task lands on the core and priority
# spec §B.1 asks for. Those are exactly the mistakes that survive a glance
# at the screen — the editor looks fine while repainting 26 chunks.
#
# stubs/ holds a fake IDF (FreeRTOS, esp_timer, heap, PPA) and fake_dev.c a
# fake ui_tab5. The REAL edit_core, ime_core and skk_core are linked in, so
# the run also exercises the core's contract as the presenter uses it.
#
# Two headers are only stubs until their owners land them, and the script
# prefers the real thing the moment it appears:
#
#   mqjs_native.h    spec §A.6, owned by components/mqjs
#   ui_tab5_a3.h     the three §A.3 entry points added to ui_tab5.h
#
# Read the header of test_presenter.c for what a green run does NOT cover.
#
# --------------------------------------------------------------------------
# Sabotage record (docs/native-editor-plan.md §1: a check is unproven until
# the defect it names has been injected and seen to fail it).  On
# 2026-08-26 seven breakages of edit_ui.c were built and run here:
#
#   stop merging adjacent dirty rows   -> "old+new cursor rows merge" and
#                                         "the merged rect spans two rows"
#   ignore the "\0rotate" token        -> the three geometry checks
#   post UI_CMD_CLEAR at focus (the
#     racy design this file used to
#     have)                            -> "focus must NOT post a drawing
#                                          command"
#   drop the foreground guard in
#     on_key()                         -> "keys after blur must not draw"
#   leave the status row out of the
#     invalidate span (off by one)     -> "old+new cursor rows merge"
#   schedule only ONE paint at focus   -> six checks, led by "focus must
#                                         schedule more than one paint"
#   drop clear_canvas() from
#     repaint_all()                    -> "every focus paint must clear the
#                                          WHOLE canvas"
#
# Each failed only the checks that name it, and nothing else.
# --------------------------------------------------------------------------
set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE" || exit 1

SRC_DIR=".."
COMPONENTS="$SRC_DIR/.."
BUILD_DIR="build"

# ---------------------------------------------------------------- toolchain
CC=""
RUN=""
if command -v gcc >/dev/null 2>&1; then
    CC="gcc"
elif command -v wsl >/dev/null 2>&1 && wsl gcc --version >/dev/null 2>&1; then
    CC="wsl gcc"
    RUN="wsl"
    echo "note: no native gcc, using 'wsl gcc'"
else
    echo "ERROR: no gcc found (tried gcc and 'wsl gcc')." >&2
    exit 1
fi

mkdir -p "$BUILD_DIR" || exit 1

SAN=""
probe="$BUILD_DIR/.santest"
printf 'int main(void){return 0;}\n' > "$probe.c"
if $CC -fsanitize=address,undefined "$probe.c" -o "$probe.bin" >/dev/null 2>&1; then
    SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
    echo "note: sanitizers enabled"
else
    echo "note: sanitizers unavailable, running without"
fi
rm -f "$probe.c" "$probe.bin"

# ------------------------------------------------------- headers not yet ours
UI_H="$COMPONENTS/ui_tab5/include/ui_tab5.h"
if [ ! -f "$UI_H" ]; then
    echo "ERROR: $UI_H is missing." >&2
    exit 1
fi
A3=""
if grep -q 'ui_tab5_cells_draw' "$UI_H"; then
    echo "note: ui_tab5.h carries the §A.3 entry points; stubs/ui_tab5_a3.h unused"
else
    A3="-include stubs/ui_tab5_a3.h"
    echo "note: ui_tab5.h has no §A.3 entry points yet — using stubs/ui_tab5_a3.h"
fi

NATIVE_INC=""
if [ -f "$COMPONENTS/mqjs/mqjs_native.h" ]; then
    NATIVE_INC="-I$COMPONENTS/mqjs"
    echo "note: using the real components/mqjs/mqjs_native.h"
else
    echo "note: components/mqjs/mqjs_native.h does not exist yet — using stubs/"
fi

# ---------------------------------------------------------------- sources
CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter"
INCS="-Istubs $NATIVE_INC -I. -I$SRC_DIR/include \
      -I$COMPONENTS/edit_core/include -I$COMPONENTS/ime_core/include \
      -I$COMPONENTS/skk_core/include -I$COMPONENTS/ui_tab5/include"

SRCS="$SRC_DIR/edit_ui.c fake_dev.c test_presenter.c \
      $COMPONENTS/edit_core/edit_core.c $COMPONENTS/edit_core/edit_buf.c \
      $COMPONENTS/edit_core/edit_undo.c $COMPONENTS/edit_core/edit_lex.c \
      $COMPONENTS/edit_core/edit_view.c $COMPONENTS/edit_core/edit_check.c \
      $COMPONENTS/ime_core/ime_core.c \
      $COMPONENTS/skk_core/skk_kana.c $COMPONENTS/skk_core/skk_dict.c \
      $COMPONENTS/skk_core/skk_builtin.c"

BIN="$BUILD_DIR/test_presenter"
# shellcheck disable=SC2086
$CC $CFLAGS $SAN $A3 $INCS $SRCS -o "$BIN" || {
    echo "ERROR: build failed." >&2
    exit 1
}

if [ -n "$RUN" ]; then
    $RUN "./$BIN"
else
    "./$BIN"
fi
rc=$?

if [ "${KEEP:-}" = "" ]; then
    rm -rf "$BUILD_DIR"
fi
exit $rc
