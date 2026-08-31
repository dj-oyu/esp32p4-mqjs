#!/bin/sh
#
# run_tests.sh — build and run the fs_core host tests.
#
# POSIX sh, same toolchain fallback as components/term_core/host_test:
# native gcc if there is one, otherwise `wsl gcc` (Windows dev boxes where
# the only toolchain lives inside WSL — wsl.exe maps the current directory
# to /mnt/<drive>/... automatically, so relative paths work and the Linux
# binaries are launched through wsl as well).
#
#   ./run_tests.sh              # everything
#   ./run_tests.sh reserved     # only test_<name>.c
#   KEEP=1 ./run_tests.sh       # leave the build tree behind
#
# Exit status: 0 when every suite passes.
#
# --------------------------------------------------------------------------
# Why only some of fs_core is testable here
#
# fs_vol.c pulls in FreeRTOS and esp_log, fs_ops.c the POSIX VFS on top of
# LittleFS/FAT — neither builds or means anything on a host. fs_reserved.c
# was split out precisely so the one piece that is pure logic (and whose
# boundary handling is easy to get subtly wrong) can be compiled by gcc and
# asserted on. Add a suite here only for code that stays free of ESP-IDF
# headers; everything else belongs on the device.
# --------------------------------------------------------------------------
set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE" || exit 1

SRC_DIR=".."
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

TIMEOUT=""
if [ -n "$RUN" ]; then
    if wsl command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 60"; fi
else
    if command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 60"; fi
fi

CFLAGS="-std=c99 -O1 -g -Wall -Wextra -I$SRC_DIR -I$SRC_DIR/include -I."

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
# Only the ESP-free translation units. Listed by name rather than globbed:
# adding a FreeRTOS-using file to fs_core must not silently break this.
CORE_SRCS="$SRC_DIR/fs_reserved.c $SRC_DIR/fs_grant.c"
for f in $CORE_SRCS; do
    if [ ! -f "$f" ]; then
        echo "ERROR: $f is missing." >&2
        exit 1
    fi
done

if [ "$#" -gt 0 ]; then
    TESTS=""
    for name in "$@"; do
        if [ -f "test_$name.c" ]; then
            TESTS="$TESTS test_$name.c"
        else
            echo "ERROR: no such suite: test_$name.c" >&2
            exit 1
        fi
    done
else
    TESTS=$(ls test_*.c 2>/dev/null)
    if [ -z "$TESTS" ]; then
        echo "ERROR: no test_*.c files here." >&2
        exit 1
    fi
fi

# -------------------------------------------------------------------- run
failed=""
built=0
for src in $TESTS; do
    name=$(basename "$src" .c)
    bin="$BUILD_DIR/$name"
    printf '\n=== %s ===\n' "$name"

    # shellcheck disable=SC2086
    if ! $CC $CFLAGS $SAN "$src" $CORE_SRCS -o "$bin"; then
        echo "FAIL $name: did not compile"
        failed="$failed $name(build)"
        continue
    fi
    built=$((built + 1))

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
done

printf '\n==================== summary ====================\n'
printf 'suites built: %d\n' "$built"
[ -n "${KEEP:-}" ] || rm -rf "$BUILD_DIR"
if [ -n "$failed" ]; then
    printf 'FAILED:%s\n' "$failed"
    exit 1
fi
printf 'ALL SUITES PASSED\n'
exit 0
