#!/bin/sh
#
# run_pc_tests.sh — build run_pc with term_core in it and run the JS suites.
#
# The phase-2 JS surface (§8 minus pipe/onReply) is exercised the way every
# other mqjs script is smoke-tested before it goes near hardware: through
# run_pc, judged on stdout. Each suite prints "TERM PC SELFTEST: ALL PASS"
# or "... N FAILED", and any interpreter exception is a failure regardless
# of what the suite thought (§8: a terminal error must never throw).
#
# POSIX sh, same toolchain fallback as host_test/run_tests.sh: native gcc if
# there is one, otherwise `wsl gcc`.
#
#   ./run_pc_tests.sh                # everything
#   ./run_pc_tests.sh basic errors   # only pc_term_<name>.js
#   KEEP=1 ./run_pc_tests.sh         # leave the build tree behind
#
# Exit status: 0 when every suite passes.
#
# --------------------------------------------------------------------------
# Why there is a C file in a directory of JS tests
#
# run_pc.c has no boot path, so nothing installs a term_port or calls
# term_registry_init(), and nothing drains on a frame tick. This script
# therefore builds the plain binary first and asks it (pc_term_probe.js)
# whether term works. Only if it does not does it rebuild with
# pc_port_shim.c linked in. When the runtime grows its own host port the
# shim silently stops being used.
# --------------------------------------------------------------------------
set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../.." && pwd)
MQJS="$ROOT/components/mqjs"
TERM_DIR="$ROOT/components/term_core"
BUILD="$HERE/build"

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
    if wsl command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 20"; fi
else
    if command -v timeout >/dev/null 2>&1; then TIMEOUT="timeout 20"; fi
fi

mkdir -p "$BUILD" || exit 1

# ------------------------------------------------------------ ROM headers
# Same sequence as README "PC だけでスクリプトを試す": the atom and stdlib
# headers are generated from device_stdlib.c, so a `term` added to the prop
# table only reaches the PC build after this regen (§8: "追加には ROM ヘッダ
# regen が必要").
echo "--- generating ROM headers"
cd "$MQJS" || exit 1

# Everything from here on is built relative to $MQJS (our new cwd), not via
# the absolute $HERE/$ROOT/$MQJS/$TERM_DIR variables above, on purpose: when
# $CC is 'wsl gcc', git bash's MSYS layer rewrites POSIX-looking absolute
# arguments (leading /d/...) into Windows paths before wsl.exe ever sees
# them, and the Linux-side gcc/ld then cannot open a "D:/..." path (this is
# exactly the failure this script used to hit: "cannot open output file
# D:/...: No such file or directory"). host_test/run_tests.sh never hits
# this because it stays relative throughout; do the same here.
TERM_REL="../term_core"
PCTEST_REL="../term_core/pc_test"
BUILD_REL="$PCTEST_REL/build"

if ! $CC -O2 -I mquickjs -o "$BUILD_REL/stdlib_tool" \
        device_stdlib.c mquickjs/mquickjs_build.c; then
    echo "ERROR: stdlib_tool did not build" >&2
    exit 1
fi
mkdir -p gen_pc || exit 1
$RUN "$BUILD_REL/stdlib_tool" -a -m64 > gen_pc/mquickjs_atom.h || exit 1
$RUN "$BUILD_REL/stdlib_tool"    -m64 > gen_pc/device_stdlib.h || exit 1

# ----------------------------------------------------------------- build
# term_core's device-only translation units are ESP_PLATFORM-guarded, so the
# whole directory can go on the command line unfiltered — the same rule
# host_test/run_tests.sh relies on.
TERM_SRCS=""
for f in "$TERM_REL"/*.c; do
    [ -e "$f" ] || continue
    TERM_SRCS="$TERM_SRCS $f"
done
if [ -z "$TERM_SRCS" ]; then
    echo "ERROR: no sources in $TERM_DIR" >&2
    exit 1
fi

MQJS_SRCS="tools/run_pc.c mqjs_runtime.c system_vault.c tailscale_adapter.c
           app/mqjs_app_manager.c
           ../skk_core/skk_kana.c ../skk_core/skk_dict.c ../skk_core/skk_builtin.c
           mquickjs/mquickjs.c mquickjs/cutils.c mquickjs/dtoa.c mquickjs/libm.c"

INCS="-I. -Igen_pc -Imquickjs -I../skk_core/include -I../ui_tab5/include -I$TERM_REL"

build_run_pc () {
    extra="$1"
    # shellcheck disable=SC2086
    $CC -O1 -g $INCS -o "$BUILD_REL/run_pc" $MQJS_SRCS $TERM_SRCS $extra -lm -lpthread
}

echo "--- building run_pc (no shim)"
if ! build_run_pc ""; then
    echo "ERROR: run_pc did not build. If the link failed on term_* symbols the"
    echo "       bindings are not in mqjs_runtime.c yet; if it failed on"
    echo "       js_term_* the ROM regen (§8) has not been run." >&2
    exit 1
fi

probe_state () {
    out=$($RUN $TIMEOUT "$BUILD_REL/run_pc" "$PCTEST_REL/pc_term_probe.js" 2>&1)
    echo "$out" | sed -n 's/^TERMPROBE: //p' | head -1
}

state=$(probe_state)
[ -n "$state" ] || state="missing"
echo "--- probe says: $state"

case "$state" in
missing)
    echo "ERROR: this run_pc has no \`term\` object at all." >&2
    echo "       The §8 bindings and/or the ROM header regen are still pending;" >&2
    echo "       there is nothing for these suites to test yet." >&2
    exit 1
    ;;
ready)
    ;;
*)
    echo "--- rebuilding with pc_port_shim.c (run_pc installs no term_port)"
    if ! build_run_pc "$PCTEST_REL/pc_port_shim.c"; then
        echo "ERROR: the shim build failed" >&2
        exit 1
    fi
    state=$(probe_state)
    echo "--- probe says: $state"
    if [ "$state" != "ready" ]; then
        echo "ERROR: term is still not usable ($state) even with the host port." >&2
        exit 1
    fi
    ;;
esac

# ------------------------------------------------------------------- run
if [ "$#" -gt 0 ]; then
    SUITES=""
    for n in "$@"; do
        if [ -f "$HERE/pc_term_$n.js" ]; then
            SUITES="$SUITES pc_term_$n.js"
        else
            echo "ERROR: no such suite: pc_term_$n.js" >&2
            exit 1
        fi
    done
else
    SUITES=""
    for f in "$HERE"/pc_term_*.js; do
        base=$(basename "$f")
        [ "$base" = "pc_term_probe.js" ] && continue
        SUITES="$SUITES $base"
    done
fi

failed=""
for s in $SUITES; do
    printf '\n=== %s ===\n' "$s"
    out=$($RUN $TIMEOUT "$BUILD_REL/run_pc" "$PCTEST_REL/$s" 2>&1)
    status=$?
    echo "$out"
    if [ "$status" -eq 124 ]; then
        echo "FAIL $s: TIMED OUT"
        failed="$failed $s(timeout)"
        continue
    fi
    # §8: errors are return values. An exception reaching the runtime is a
    # failure even if the script's own tally came out clean.
    if echo "$out" | grep -qiE "exception|SyntaxError|TypeError|ReferenceError|RangeError"; then
        echo "FAIL $s: an exception escaped (§8 forbids it)"
        failed="$failed $s(exception)"
        continue
    fi
    if echo "$out" | grep -q "TERM PC SELFTEST: ALL PASS"; then
        echo "PASS $s"
    else
        echo "FAIL $s: no ALL PASS line"
        failed="$failed $s"
    fi
done

printf '\n==================== summary ====================\n'
[ -n "${KEEP:-}" ] || rm -f "$BUILD/stdlib_tool"
if [ -n "$failed" ]; then
    printf 'FAILED:%s\n' "$failed"
    exit 1
fi
printf 'ALL PC SUITES PASSED\n'
exit 0
