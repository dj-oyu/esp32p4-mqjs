#!/bin/sh
# Regression baseline for ime_core: run a fixed key script through the
# engine and diff the per-key trace against tools/tests/ime_golden.trace.
#
#   tools/tests/ime_diff.sh
#
# Each line is one keystroke: how it was disposed of (TEXT = committed
# text, PASS = the app's to handle, TAKEN = consumed silently), the
# preedit, the mode, the candidate count and any committed string.
#
# WHAT THE GOLDEN IS. Until the ssh_vt migration this script ran the same
# script through BOTH implementations — the hand-written JS glue inside
# examples/ssh_vt.js under run_pc, and ime_core — and diffed them live.
# That comparison reported IDENTICAL, and the golden is the JS side of it
# frozen on 2026-07-30 (see the commit that added it). ssh_vt no longer
# has that glue, so the old half is gone for good; what remains is a
# permanent baseline for ime_core that outlives any one app, and needs
# neither run_pc nor an app to run.
#
# It is a BASELINE, NOT AN ASPIRATION. A diff here means ime_core's
# behaviour changed — which may well be an improvement. Then re-record
# the golden in the same commit and say why. Do not bend the engine back
# to it, and do not "fix" the golden separately from the change.
#
# ⚠️ LEARNING IS STATE. skk learns on every commit out of v, so the
# candidate order on a second run is not the order on the first. The C
# driver attaches a learning dictionary of its own because the JS binding
# does (mqjs_runtime.c skk_attach_mru) and leaving it off reorders
# candidates — but it is a fresh in-process one, never persisted, so this
# script is repeatable where the old two-sided version was not.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
dict="$root/components/skk_core/skk_dict.bin"
gold="$root/tools/tests/ime_golden.trace"
out=${TMPDIR:-/tmp}

[ -f "$dict" ] || { echo "no dictionary: $dict (tools/skk_prep.py)" >&2; exit 2; }
[ -f "$gold" ] || { echo "no golden: $gold" >&2; exit 2; }

cc -O2 -std=c99 \
   -I "$root/components/skk_core/include" -I "$root/components/ime_core/include" \
   "$root/tools/tests/ime_diff_new.c" "$root/components/ime_core/ime_core.c" \
   "$root/components/skk_core/skk_kana.c" "$root/components/skk_core/skk_dict.c" \
   -o "$out/ime_diff_new"
"$out/ime_diff_new" "$dict" > "$out/ime_diff_new.trace"

if diff "$gold" "$out/ime_diff_new.trace"; then
    echo "ime_diff: IDENTICAL ($(grep -c '^T ' "$out/ime_diff_new.trace") keys)"
else
    echo "ime_diff: DIVERGED from $gold" >&2
    exit 1
fi
