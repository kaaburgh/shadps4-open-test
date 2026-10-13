#!/usr/bin/env bash
# Runs present_transfer's 1280x720 frame through shadPS4's FSR upscaler on
# lavapipe, with Vulkan validation on, while tools/resize_window.py drags the
# window through several sizes, then checks what ends up on screen.
#
# The screen and window are 1920x1080 and GPU.fsr_enabled is set, so FSR
# upscales by 1.5. The window then goes through
#   1365x768  FSR, odd width
#   1601x900  FSR, odd width (the frame keeps the game's 16:9)
#   640x360   no FSR: the frame is smaller than the game's
#   1001x777  no FSR, letterboxed (frame 1001x563)
#   1920x1080 FSR again
# with every size held for 1.5 s, so FSR's images are recreated at new
# extents and FSR is switched off and on while the guest keeps flipping.
#
# Passes if
#   - the guest flips for its whole 15 s (no stuck flip, no crash);
#   - every resize was made;
#   - no Vulkan error is logged after the guest starts presenting (the
#     validation layer may complain about device creation);
#   - the patch centres at 1.5x scale match the default output within 1 LSB;
#   - negative control: the same frame checked at 1x scale fails;
#   - FSR is really used: a second run at 1920x1080 with GPU.fsr_enabled off
#     (the plain scaler) also matches at the patch centres, but its pixels
#     across the edge between two patches differ from the FSR run's by more
#     than EDGE_MIN. If FSR were ignored, the two strips would be the same.
#
# usage: SHADPS4=/path/to/shadps4 tests/present_transfer/run-fsr.sh
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$ROOT/tests/present_transfer"
OUT="$ROOT/out/present_transfer"
LOG="$OUT/run-fsr.log"
SIZES="1365x768 1601x900 640x360 1001x777 1920x1080"
EDGE_MIN=8

rm -f "$OUT/capture-fsr.json" "$OUT/capture-fsr-1x.json" "$OUT/capture-plain.json" \
    "$OUT/resize.log"
XVFB_SCREEN=1920x1080x24 \
XVFB_HOOK="python3 '$DIR/tools/resize_window.py' $SIZES >'$OUT/resize.log' 2>&1; \
python3 '$DIR/tools/capture.py' capture '$OUT/capture-fsr.json' 30 1.5; \
python3 '$DIR/tools/capture.py' capture '$OUT/capture-fsr-1x.json' 10 1" \
    bash "$ROOT/scripts/run-test-lavapipe.sh" present_transfer --timeout 60 \
    --set GPU.window_width=1920 --set GPU.window_height=1080 \
    --set GPU.fsr_enabled=true --set Vulkan.vkvalidation_enabled=true >"$LOG" 2>&1
rc=$?

# Control: the same window without FSR.
XVFB_SCREEN=1920x1080x24 \
XVFB_HOOK="python3 '$DIR/tools/capture.py' capture '$OUT/capture-plain.json' 30 1.5" \
    bash "$ROOT/scripts/run-test-lavapipe.sh" present_transfer --timeout 60 \
    --set GPU.window_width=1920 --set GPU.window_height=1080 \
    --set GPU.fsr_enabled=false >"$OUT/run-plain.log" 2>&1
plain_rc=$?

status=0
fail() {
    echo "FSR FAIL $*"
    status=1
}

result=$(grep -a 'HOST_RESULT' "$LOG" | tail -1)
echo "${result:-no HOST_RESULT} (exit $rc)"
[[ $rc -eq 0 && $result == *status=PASS* ]] || fail "guest did not pass"
plain=$(grep -a 'HOST_RESULT' "$OUT/run-plain.log" | tail -1)
echo "control without FSR: ${plain:-no HOST_RESULT} (exit $plain_rc)"
[[ $plain_rc -eq 0 && $plain == *status=PASS* ]] || fail "control run did not pass"

resized=$(grep -c '^RESIZE [0-9]' "$OUT/resize.log" 2>/dev/null)
echo "resizes: ${resized:-0} of 5"
[[ ${resized:-0} -eq 5 ]] || fail "resizes=${resized:-0}"

errors=$(sed 's/\x1b\[[0-9;]*m//g' "$LOG" | sed -n '/presenting 1280x720/,$p' |
    grep -a -c '\[Render\.Vulkan\] <\(Error\|Critical\)>')
echo "Vulkan errors while presenting: $errors"
[[ $errors -eq 0 ]] || fail "vulkan_errors=$errors"

python3 "$DIR/tools/capture.py" check "$OUT/capture-fsr.json" default || fail "pixels"
python3 "$DIR/tools/capture.py" check "$OUT/capture-plain.json" default || fail "control pixels"
python3 "$DIR/tools/capture.py" differ "$OUT/capture-fsr.json" "$OUT/capture-plain.json" \
    "$EDGE_MIN" || fail "edge: FSR and the plain scaler give the same pixels"
if python3 "$DIR/tools/capture.py" check "$OUT/capture-fsr-1x.json" default >/dev/null; then
    fail "negative control passed: a 1x check of the 1.5x frame should not"
else
    echo "negative control: 1x check of the 1.5x frame fails, as it should"
fi

[[ $status -eq 0 ]] && echo "FSR PASS"
exit $status
