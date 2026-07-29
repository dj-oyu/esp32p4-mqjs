#!/bin/bash
# Build the PC runtime and run adversarial terminal-output tests.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MQJS="$ROOT/components/mqjs"
RUN_PC=${RUN_PC:-/tmp/run_pc}

# Keep this in sync with the run_pc recipe in README.md: mqjs_runtime.c
# includes skk_core.h and ui_cell_width.h unconditionally (both are pure
# logic shared with the firmware), so their include dirs and the skk_core
# sources are not optional on the host either.
gcc -O2 -I"$MQJS" -I"$MQJS/gen_pc" -I"$MQJS/mquickjs" \
    -I"$ROOT/components/skk_core/include" \
    -I"$ROOT/components/ui_tab5/include" -o "$RUN_PC" \
    "$MQJS/tools/run_pc.c" "$MQJS/mqjs_runtime.c" "$MQJS/system_vault.c" \
    "$MQJS/tailscale_adapter.c" "$MQJS/app/mqjs_app_manager.c" \
    "$ROOT/components/skk_core/skk_kana.c" \
    "$ROOT/components/skk_core/skk_dict.c" \
    "$ROOT/components/skk_core/skk_builtin.c" \
    "$MQJS/mquickjs/mquickjs.c" "$MQJS/mquickjs/cutils.c" \
    "$MQJS/mquickjs/dtoa.c" "$MQJS/mquickjs/libm.c" -lm

python3 "$ROOT/tools/test_ssh_vt_security.py" "$RUN_PC"
python3 "$ROOT/tools/test_vault_isolation.py" "$RUN_PC"
