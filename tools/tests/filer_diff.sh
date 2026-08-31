#!/bin/sh
# Migration gate for the native filer (docs/native-editor-spec.md §C.6),
# written the way tools/tests/ime_diff.sh was: freeze a fixed operation
# script (tools/tests/filer_ops.txt), diff a live run's trace against a
# golden. See that script's comment header for the format both instructions
# and traces use, and for why VOL sd may legitimately be a single SKIP line
# on a board with no card in (Stamp-P4, or a cardless Tab5 run).
#
#   tools/tests/filer_diff.sh
#
# WHAT THIS SCRIPT ACTUALLY DOES RIGHT NOW: checks that the golden exists,
# and refuses to report success if it does not. THAT IS THE WHOLE
# IMPLEMENTATION TODAY. It is not a stub out of laziness — every step past
# that check needs the real device (there is no host-buildable stand-in
# for fs_core/fs_filer the way ime_diff.sh has one for ime_core), and the
# agent writing this file is expressly forbidden from touching the device
# or components/**. Filling in the rest is real work for whoever has both;
# read HOW TO CAPTURE THE GOLDEN below before doing that.
#
# ---------------------------------------------------------------------
# HOW TO CAPTURE THE GOLDEN (needs the real device; nobody has done this
# yet — do not invent numbers, run it)
# ---------------------------------------------------------------------
#
# 1. Write a dev-slot driver probe (does not exist yet — a sibling of
#    tools/probe_fs.js) that:
#      - reads tools/tests/filer_ops.txt's instruction list. JS on the
#        device cannot open a host file, so either (a) hardcode the same
#        sequence into the probe in the same order — and keep the two
#        files in sync by hand, same as ime_diff_new.c's KEYS[] is kept in
#        sync with its own comment's claims about the golden — or
#        (b) have a host-side script (tools/mqtt_pub.py is the existing
#        pattern) publish each instruction line as an MQTT command that
#        the probe subscribes to and executes one at a time. (b) is the
#        one worth building: it means filer_ops.txt stays the single
#        source of truth instead of two files that can drift.
#      - for VOL sd, checks fs.volumes() for an "sd" entry with
#        mounted:true before running that block; if absent, emits exactly
#        one SKIP record for the whole block and moves on (see
#        filer_ops.txt's header — this is not optional).
#      - after every instruction, calls fs.list() on the directory named
#        in filer_ops.txt's comment header and reports the sorted
#        {name, dir, size} triples as one JSON line over MQTT
#        (net.topic("proberep"), same as every other tools/probe_*.js).
#      - grants: this all runs in the dev slot, so fs.request() resolves
#        without a consent screen (design §7 / spec §A.5 — the dev slot
#        already carries full trust). A driver built to run outside the
#        dev slot would need fs.pick()/fs.request() plumbing this script
#        does not attempt to describe.
#
# 2. Run that probe on real hardware (Tab5 with a card in, so VOL sd
#    actually executes at least once somewhere in the golden — a
#    cardless-only capture would never prove the SKIP path is reachable,
#    only that it is the default). Capture its MQTT trace to
#    tools/tests/filer_golden.trace, one JSON line per record, in the
#    instruction order from filer_ops.txt.
#
# 3. Freeze that file, commit it, and say in the commit message what ran
#    and what firmware it ran against — same discipline as
#    ime_golden.trace's commit.
#
# WHAT REPLACES THE GOLDEN SIDE ONCE fs_filer EXISTS (M2, not built yet):
# the design calls the native replay driver `fs_filer_replay`
# (spec §C.6) — presumably a component that walks the same instruction
# list natively and reports through the same MQTT shape, so this script's
# job becomes: run fs_filer_replay, capture its trace, diff against
# filer_golden.trace. That diff step is not written below because there
# is nothing on either side to point it at yet; do not stub it with an
# empty success path — an empty diff against a golden that also does not
# exist would report green while proving nothing (see this repo's rule:
# a red suite on arrival is suspect, but so is a green one with an empty
# input on both sides).
#
# ime_diff.sh's caution applies here even harder: this is a BASELINE, NOT
# AN ASPIRATION. A diff between native and JS behaviour may mean the
# native filer improved on an old JS quirk — re-record the golden in the
# same commit and say why, do not bend the native side back to match
# JS-era behaviour reflexively.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
ops="$root/tools/tests/filer_ops.txt"
gold="$root/tools/tests/filer_golden.trace"

[ -f "$ops" ] || { echo "no ops script: $ops" >&2; exit 2; }

if [ ! -f "$gold" ]; then
    echo "filer_diff: 未取得 — golden が無い ($gold)" >&2
    echo "  capture it from real hardware first; see the HOW TO CAPTURE" >&2
    echo "  THE GOLDEN comment at the top of this script." >&2
    exit 1
fi

# Everything past this line is unreachable until both a device-side
# capture driver and a native fs_filer_replay exist. Left unwritten
# rather than faked — see the big comment above.
echo "filer_diff: golden present but no native-side driver exists yet" >&2
echo "  (fs_filer_replay, spec §C.6, M2 — not built). Nothing to diff" >&2
echo "  against. This is expected until M2 lands; it is not a pass." >&2
exit 1
