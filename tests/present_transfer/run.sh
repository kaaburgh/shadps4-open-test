#!/usr/bin/env bash
# Runs present_transfer twice on lavapipe, with shadPS4's default output and
# with GPU.rec709_output, captures the screen while the frame is up and checks
# the patch colours against tools/patches.py. Exits 0 only if both pass.
#
# usage: SHADPS4=/path/to/shadps4 tests/present_transfer/run.sh [default|rec709]...
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$ROOT/tests/present_transfer"
OUT="$ROOT/out/present_transfer"
modes=("$@")
[[ ${#modes[@]} -eq 0 ]] && modes=(default rec709)

status=0
for mode in "${modes[@]}"; do
    extra=()
    [[ $mode == rec709 ]] && extra=(--set GPU.rec709_output=true)
    rm -f "$OUT/capture-$mode.json"
    XVFB_HOOK="python3 '$DIR/tools/capture.py' capture '$OUT/capture-$mode.json' 60" \
        bash "$ROOT/scripts/run-test-lavapipe.sh" present_transfer --timeout 60 "${extra[@]}" \
        >"$OUT/run-$mode.log" 2>&1
    echo "mode=$mode $(grep -a 'HOST_RESULT' "$OUT/run-$mode.log" | tail -1)"
    python3 "$DIR/tools/capture.py" check "$OUT/capture-$mode.json" "$mode" || status=1
done
exit $status
