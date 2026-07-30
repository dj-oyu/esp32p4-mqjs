#!/bin/sh
#
# run_tests.sh — build and run the term_core phase-1 host tests.
#
# POSIX sh. Uses gcc if it is on PATH; otherwise falls back to `wsl gcc`
# (Windows dev boxes where the only toolchain is inside WSL — wsl.exe maps the
# current directory to /mnt/<drive>/... automatically, so relative paths work
# and the Linux binaries are launched through wsl as well).
#
# Exit status: 0 when every suite passes, 1 otherwise.
#
#   ./run_tests.sh              # everything
#   ./run_tests.sh init sgr     # only the named suites (test_<name>.c)
#   FUZZ_MB=64 ./run_tests.sh fuzz   # longer soak
#
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
    RUN=""
elif command -v wsl >/dev/null 2>&1 && wsl gcc --version >/dev/null 2>&1; then
    CC="wsl gcc"
    RUN="wsl"
    echo "note: no native gcc, using 'wsl gcc'"
else
    echo "ERROR: no gcc found (tried gcc and 'wsl gcc')." >&2
    exit 1
fi

# `timeout` turns a parser that hangs into a failing test instead of a hung
# terminal — exactly the failure mode §5's hard bounds exist to prevent.
TIMEOUT=""
if [ -n "$RUN" ]; then
    if wsl command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 300"; fi
else
    if command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 300"; fi
fi

# -I../../tweetnacl is for test_bb_sig only: term_bb_pull.h leaves the Ed25519
# verifier abstract (a callback with crypto_sign_open's contract), so ONE suite
# links the vendored verify-only TweetNaCl and drives term_bb_serve over the
# fixed vectors in bb_vectors.h. Nothing else includes it.
CFLAGS="-std=c99 -O1 -g -Wall -Wextra -I$SRC_DIR -I. -I$SRC_DIR/../tweetnacl"
LDFLAGS=""

mkdir -p "$BUILD_DIR" || exit 1

# ASan/UBSan when available: the memory contract (§4.2, B6) deserves a real
# checker behind the hand-rolled red zones. The probe lives in the build dir
# on purpose — with the wsl fallback a Windows-side mktemp path would not be
# visible to the compiler.
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
CORE_SRCS=""
for f in "$SRC_DIR"/*.c; do
    [ -e "$f" ] || continue
    CORE_SRCS="$CORE_SRCS $f"
done
if [ -z "$CORE_SRCS" ]; then
    echo "ERROR: no implementation sources found in $SRC_DIR (expected term_core.c)." >&2
    echo "       The tests are written against term_core.h only; build them once" >&2
    echo "       the implementation lands." >&2
    exit 1
fi

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

# ---------------------------------------------------------------- run
failed=""
built=0
for src in $TESTS; do
    name=$(basename "$src" .c)
    bin="$BUILD_DIR/$name"
    printf '\n=== %s ===\n' "$name"

    # Per-suite extra sources. Only test_bb_sig has one: the real Ed25519
    # verifier it drives term_bb_serve's `verify` callback with. TweetNaCl is
    # public-domain reference code that shifts negative values deliberately, so
    # it is compiled UNSANITISED into an object and linked — UBSan is right about
    # it and it is not this project's code to fix.
    EXTRA=""
    if [ "$name" = "test_bb_sig" ]; then
        tn="$SRC_DIR/../tweetnacl/tweetnacl.c"
        if [ ! -f "$tn" ]; then
            echo "FAIL $name: $tn is missing (the real Ed25519 vectors need it)"
            failed="$failed $name(no-tweetnacl)"
            continue
        fi
        # shellcheck disable=SC2086
        if ! $CC $CFLAGS -c "$tn" -o "$BUILD_DIR/tweetnacl.o"; then
            echo "FAIL $name: tweetnacl.c did not compile"
            failed="$failed $name(tweetnacl)"
            continue
        fi
        EXTRA="$BUILD_DIR/tweetnacl.o"
    fi

    # shellcheck disable=SC2086
    if ! $CC $CFLAGS $SAN "$src" $CORE_SRCS $EXTRA -o "$bin" $LDFLAGS; then
        echo "FAIL $name: did not compile"
        failed="$failed $name(build)"
        continue
    fi
    built=$((built + 1))

    args=""
    if [ "$name" = "test_fuzz" ] && [ -n "${FUZZ_MB:-}" ]; then
        args="$FUZZ_MB"
    fi

    # shellcheck disable=SC2086
    if $RUN $TIMEOUT "./$bin" $args; then
        printf 'PASS %s\n' "$name"
    else
        status=$?
        if [ "$status" -eq 124 ]; then
            echo "FAIL $name: TIMED OUT (a hang is a bound violation, see §5/B1)"
            failed="$failed $name(timeout)"
        else
            echo "FAIL $name: exit status $status"
            failed="$failed $name"
        fi
    fi
done

printf '\n==================== summary ====================\n'
printf 'suites built: %d\n' "$built"
if [ -n "$failed" ]; then
    printf 'FAILED:%s\n' "$failed"
    exit 1
fi
printf 'ALL SUITES PASSED\n'
exit 0
