#!/bin/sh
# Differential gate for the IME migration: run the SAME key script through
# the old path (the real examples/ssh_vt.js under run_pc, with the real
# dictionary) and through ime_core, and diff the per-key traces.
#
#   tools/tests/ime_diff.sh [/path/to/run_pc]
#
# Each line is one keystroke: how it was disposed of (TEXT = committed
# text, PASS = the app's to handle, TAKEN = consumed silently), the
# preedit, the mode, the candidate count and any committed string. If the
# two agree on all of that for every key, the layer did not change what
# the user sees.
#
# ⚠️ LEARNING IS STATE. skk learns on every commit out of v and run_pc
# PERSISTS it (components/mqjs/skk/mru.txt), so the old path answers
# differently on its second run — the previously chosen candidate comes
# first. This was caught the hard way: the first comparison "passed"
# only because both sides happened to be fresh. The file is removed
# before each old-path run, and the C driver attaches a learning
# dictionary of its own because the JS binding does (mqjs_runtime.c
# skk_attach_mru) and leaving it off reorders candidates.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
run=${1:-/tmp/run_pc}
app="$root/examples/ssh_vt.js"
inc="$root/tools/tests/ime_diff_old.js.inc"
dict="$root/components/skk_core/skk_dict.bin"
out=${TMPDIR:-/tmp}

[ -x "$run" ] || { echo "run_pc not found: $run (README recipe)" >&2; exit 2; }
[ -f "$dict" ] || { echo "no dictionary: $dict (tools/skk_prep.py)" >&2; exit 2; }

n=$(grep -c '@imetest-inject' "$app" || true)
[ "$n" = 1 ] || { echo "marker @imetest-inject appears $n times in $app" >&2; exit 2; }

# ---- old path: the real app, injected at the marker ----
awk -v inc="$inc" '
    /@imetest-inject/ { skip = 1 }
    skip { if (/\*\//) { skip = 0; while ((getline line < inc) > 0) print line } next }
    { print }
' "$app" > "$out/ime_diff_old.js"

rm -f "$root/components/mqjs/skk/mru.txt"
(cd "$root/components/mqjs" && timeout 60 stdbuf -o0 "$run" "$out/ime_diff_old.js" 2>&1) \
    | grep -E '^(T |DICT|READY|TRACE)' > "$out/ime_diff_old.trace"
rm -f "$root/components/mqjs/skk/mru.txt"

# ---- new path: ime_core, same dictionary, its own fresh learning ----
cc -O2 -std=c99 \
   -I "$root/components/skk_core/include" -I "$root/components/ime_core/include" \
   "$root/tools/tests/ime_diff_new.c" "$root/components/ime_core/ime_core.c" \
   "$root/components/skk_core/skk_kana.c" "$root/components/skk_core/skk_dict.c" \
   -o "$out/ime_diff_new"
"$out/ime_diff_new" "$dict" > "$out/ime_diff_new.trace"

if diff "$out/ime_diff_old.trace" "$out/ime_diff_new.trace"; then
    echo "ime_diff: IDENTICAL ($(grep -c '^T ' "$out/ime_diff_new.trace") keys)"
else
    echo "ime_diff: DIVERGED" >&2
    exit 1
fi
