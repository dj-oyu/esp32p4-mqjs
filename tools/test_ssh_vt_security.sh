#!/bin/bash
# Build the PC runtime and run adversarial terminal-output tests.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MQJS="$ROOT/components/mqjs"
RUN_PC=${RUN_PC:-/tmp/run_pc}

gcc -O2 -I"$MQJS" -I"$MQJS/gen_pc" -I"$MQJS/mquickjs" -o "$RUN_PC" \
    "$MQJS/tools/run_pc.c" "$MQJS/mqjs_runtime.c" \
    "$MQJS/mquickjs/mquickjs.c" "$MQJS/mquickjs/cutils.c" \
    "$MQJS/mquickjs/dtoa.c" "$MQJS/mquickjs/libm.c" -lm

python3 "$ROOT/tools/test_ssh_vt_security.py" "$RUN_PC"
python3 "$ROOT/tools/test_vault_isolation.py" "$RUN_PC"
