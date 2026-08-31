#!/bin/sh
#
# run_perf_tests.sh — build and run the perf_meter host tests
# (main/perf_meter.c, tools/tests/test_perf_hist.c).
#
# POSIX sh, same toolchain fallback as components/fs_core/pc_test/run_tests.sh:
# native gcc if there is one, otherwise `wsl gcc` (Windows dev boxes where
# the only toolchain lives inside WSL — wsl.exe maps the current directory
# to /mnt/<drive>/... automatically, so relative paths work and the Linux
# binaries are launched through wsl as well).
#
#   ./run_perf_tests.sh          # build + run
#   KEEP=1 ./run_perf_tests.sh   # leave the build tree behind
#
# Exit status: 0 when the suite passes.
#
# --------------------------------------------------------------------------
# Why only perf_meter.c
#
# main/perf_meter.{c,h} keeps ESP-IDF entirely behind #ifdef ESP_PLATFORM
# (native-editor-spec.md, perf_meter assignment): the histogram, the
# worst-16 ring, and the core-mismatch span guard are plain C99 and never
# defined here, so they compile and run on a host unmodified. Nothing else
# in main/ makes that claim (they all pull in FreeRTOS / esp_* headers),
# so this script only ever builds the one file.
# --------------------------------------------------------------------------
set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE" || exit 1

MAIN_DIR="../../main"
BUILD_DIR="build_perf"

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

TIMEOUT=""
if [ -n "$RUN" ]; then
    if wsl command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 60"; fi
else
    if command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 60"; fi
fi

CFLAGS="-std=c99 -O1 -g -Wall -Wextra -I$MAIN_DIR"

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

# ---------------------------------------------------------------- sources
CORE_SRCS="$MAIN_DIR/perf_meter.c"
for f in $CORE_SRCS; do
    if [ ! -f "$f" ]; then
        echo "ERROR: $f is missing." >&2
        exit 1
    fi
done

TESTS="test_perf_hist.c"
if [ ! -f "$TESTS" ]; then
    echo "ERROR: $TESTS is missing." >&2
    exit 1
fi

# -------------------------------------------------------------------- run
failed=""
name=$(basename "$TESTS" .c)
bin="$BUILD_DIR/$name"
printf '\n=== %s ===\n' "$name"

# shellcheck disable=SC2086
if ! $CC $CFLAGS $SAN "$TESTS" $CORE_SRCS -o "$bin"; then
    echo "FAIL $name: did not compile"
    failed="$failed $name(build)"
else
    # shellcheck disable=SC2086
    if $RUN $TIMEOUT "./$bin"; then
        printf 'PASS %s\n' "$name"
    else
        status=$?
        if [ "$status" -eq 124 ]; then
            echo "FAIL $name: TIMED OUT"
            failed="$failed $name(timeout)"
        else
            echo "FAIL $name: exit status $status"
            failed="$failed $name"
        fi
    fi
fi

printf '\n==================== summary ====================\n'
[ -n "${KEEP:-}" ] || rm -rf "$BUILD_DIR"
if [ -n "$failed" ]; then
    printf 'FAILED:%s\n' "$failed"
    exit 1
fi
printf 'ALL SUITES PASSED\n'
exit 0
