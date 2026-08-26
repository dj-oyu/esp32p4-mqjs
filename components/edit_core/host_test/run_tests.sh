#!/bin/sh
#
# run_tests.sh — build and run the edit_core host tests.
#
# POSIX sh, same toolchain fallback as components/fs_core/pc_test and
# components/term_core/host_test: native gcc if there is one, otherwise
# `wsl gcc` (Windows dev boxes where the only toolchain lives inside WSL —
# wsl.exe maps the current directory to /mnt/<drive>/... automatically, so
# relative paths work and the Linux binaries are launched through wsl too).
#
#   ./run_tests.sh              # every suite
#   ./run_tests.sh buf lines    # only test_<name>.c
#   ./run_tests.sh --fuzz 60    # libFuzzer for 60 seconds (needs clang)
#   ./run_tests.sh --bench      # docs/native-editor-spec.md §C.3 timings
#   ./run_tests.sh --every-op   # edit_check() after every state-changing call
#   KEEP=1 ./run_tests.sh       # leave the build tree behind
#
# --every-op is the M3 gate wording ("edit_check 毎 op で緑", spec §C.6): the
# suites already call edit_check through ET_OK where they choose to, and this
# mode makes every public call that can change state go through it as well.
#
# Exit status: 0 when every suite passes.
#
# --------------------------------------------------------------------------
# Why edit_core can be tested here at all
#
# docs/native-editor-spec.md §A says edit_core is "純 C99。ESP-IDF ヘッダ 0"
# and that it depends on nothing. That promise is what makes this directory
# possible, so this script globs *every* .c in the component rather than
# listing them: if someone adds a file that pulls in freertos/ or esp_log.h,
# the build here breaks immediately instead of the rule rotting quietly.
#
# What this directory does NOT cover: everything above edit_core. The
# presenter, edit_task, the PPA blit path, the IME hop and the key route are
# device-only (§D row 7). A green run here means the core's contract holds on
# a host, not that the editor works.
#
# Each test_*.c starts with a comment naming what that suite does not look at
# (docs/native-editor-plan.md §1). Read it before trusting a green run.
# --------------------------------------------------------------------------
set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE" || exit 1

SRC_DIR=".."
BUILD_DIR="build"

MODE="test"
FUZZ_SECONDS=60
EVERY_OP=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --every-op)
            EVERY_OP="-DEDIT_TEST_CHECK_EVERY_OP"
            shift
            continue
            ;;
    esac
    break
done
if [ "$#" -gt 0 ]; then
    case "$1" in
        --fuzz)
            MODE="fuzz"
            shift
            if [ "$#" -gt 0 ]; then FUZZ_SECONDS="$1"; shift; fi
            ;;
        --bench)
            MODE="bench"
            shift
            ;;
        -h|--help)
            sed -n '2,30p' "$0"
            exit 0
            ;;
    esac
fi

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

CLANG=""
if command -v clang >/dev/null 2>&1; then
    CLANG="clang"
elif command -v wsl >/dev/null 2>&1 && wsl clang --version >/dev/null 2>&1; then
    CLANG="wsl clang"
fi

TIMEOUT=""
if [ -n "$RUN" ]; then
    if wsl command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 120"; fi
else
    if command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 120"; fi
fi

CFLAGS="-std=c99 -O1 -g -Wall -Wextra -I$SRC_DIR -I$SRC_DIR/include -I. $EVERY_OP"

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
# Globbed on purpose — see the header comment. edit_core is promised to be
# free of ESP-IDF headers (§A), so every .c in it must compile here.
CORE_SRCS=$(ls "$SRC_DIR"/*.c 2>/dev/null)
if [ -z "$CORE_SRCS" ]; then
    echo "ERROR: no .c files in $SRC_DIR — edit_core is not implemented yet." >&2
    echo "       (The suites here are written against docs/native-editor-spec.md" >&2
    echo "        §A.1 and will not link until edit_core.c exists.)" >&2
    exit 1
fi
if [ ! -f "$SRC_DIR/edit_core.h" ] && [ ! -f "$SRC_DIR/include/edit_core.h" ]; then
    echo "ERROR: edit_core.h not found in $SRC_DIR or $SRC_DIR/include." >&2
    exit 1
fi

# -------------------------------------------------------------------- fuzz
if [ "$MODE" = "fuzz" ]; then
    if [ -z "$CLANG" ]; then
        echo "ERROR: --fuzz needs clang (tried clang and 'wsl clang')." >&2
        exit 1
    fi
    CORPUS="$BUILD_DIR/corpus"
    mkdir -p "$CORPUS" || exit 1
    bin="$BUILD_DIR/fuzz_edit"
    echo "building fuzz_edit with $CLANG"
    # shellcheck disable=SC2086
    if ! $CLANG -std=c99 -O1 -g -Wall -Wextra -I"$SRC_DIR" -I"$SRC_DIR/include" -I. \
         -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
         fuzz_edit.c $CORE_SRCS -o "$bin"; then
        echo "FAIL fuzz_edit: did not compile"
        exit 1
    fi
    echo "running for ${FUZZ_SECONDS}s (corpus: $CORPUS)"
    # shellcheck disable=SC2086
    if $RUN "./$bin" "$CORPUS" -max_total_time="$FUZZ_SECONDS" -print_final_stats=1; then
        echo "FUZZ-OK  ${FUZZ_SECONDS}s, no crashes"
        [ -n "${KEEP:-}" ] || rm -f "$bin"
        exit 0
    fi
    echo "FUZZ-FAIL — a crash artefact was written next to this script"
    exit 1
fi

# ------------------------------------------------------------------- bench
if [ "$MODE" = "bench" ]; then
    bin="$BUILD_DIR/bench_edit"
    # ベンチはサニタイザ無し (-O2)。ASAN 付きの数字は回帰の比較に使えない。
    # shellcheck disable=SC2086
    if ! $CC -std=c99 -O2 -g -Wall -Wextra -I"$SRC_DIR" -I"$SRC_DIR/include" -I. \
         bench_edit.c $CORE_SRCS -o "$bin"; then
        echo "FAIL bench_edit: did not compile"
        exit 1
    fi
    # shellcheck disable=SC2086
    if $RUN $TIMEOUT "./$bin"; then
        echo
        echo "reminder: these are HOST numbers, for regression only."
        echo "          They are not the device numbers (§C.3, §F #8/#10)."
        [ -n "${KEEP:-}" ] || rm -rf "$BUILD_DIR"
        exit 0
    fi
    echo "FAIL bench_edit: non-zero exit"
    exit 1
fi

# ------------------------------------------------------------------- suites
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
[ -z "$EVERY_OP" ] || printf 'mode: edit_check() after every state-changing call\n'
[ -n "${KEEP:-}" ] || rm -rf "$BUILD_DIR"
if [ -n "$failed" ]; then
    printf 'FAILED:%s\n' "$failed"
    exit 1
fi
printf 'ALL SUITES PASSED\n'
exit 0
