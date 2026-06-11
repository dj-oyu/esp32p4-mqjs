#!/bin/bash
# PC smoke for examples: run each for a few seconds under run_pc and fail
# on any JS exception. Usage (WSL): bash tools/smoke_examples.sh [files...]
cd "$(dirname "$0")/../examples" || exit 1
RUN=${RUN_PC:-/tmp/run_pc}
files=${@:-touch_demo ui_demo i2c_scan mqtt_demo blink_button morse reaction bench ssh_term ssh_vt settings_demo}
rc=0
for f in $files; do
    out=$(timeout 4 "$RUN" "$f.js" 2>&1)
    if echo "$out" | grep -qiE "exception|SyntaxError|TypeError|ReferenceError"; then
        echo "=== $f.js FAIL ==="
        echo "$out" | head -8
        rc=1
    else
        echo "$f.js OK"
    fi
done
exit $rc
