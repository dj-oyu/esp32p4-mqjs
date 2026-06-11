#!/bin/bash
# Shelve every store-manifested example: signed body -> <base>/apps/<name>,
# catalog row -> <base>/store/<name> (design sec 11). Usage (WSL):
#   bash tools/shelve_all.sh [HOST] [BASE_TOPIC]
cd "$(dirname "$0")/.." || exit 1
HOST=${1:-192.168.1.2}
BASE=${2:-esp32p4-mqjs/task/u7q3x9f2}
rc=0
for f in examples/*.js; do
    head -c 16 "$f" | grep -q '^// @app ' || continue  # launcher etc.
    python3 tools/mqjs_push.py "$HOST" "$BASE" "$f" --shelf || rc=1
done
exit $rc
