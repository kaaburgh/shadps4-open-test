#!/usr/bin/env bash
# Run a test on shadPS4 with Mesa lavapipe (CPU Vulkan) under a private Xvfb.
# Usage: SHADPS4=/path/to/shadps4 scripts/run-test-lavapipe.sh <test> [run-test.py args...]
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LVP_ICD="${LVP_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}"
[[ -f "$LVP_ICD" ]] || { echo "lavapipe ICD not found: $LVP_ICD (install mesa-vulkan-drivers)" >&2; exit 2; }
command -v Xvfb >/dev/null || { echo "Xvfb not found (install xvfb)" >&2; exit 2; }

# Xvfb writes its display number to -displayfd once it accepts connections, and
# picks a free display unless XVFB_DISPLAY pins one. Polling for the socket file
# instead raced back-to-back runs: the previous Xvfb, killed but not yet exited,
# still owned the display, so the new one failed to start while its socket looked
# ready.
DISPLAY_FILE="$(mktemp)"
XVFB_PID=
cleanup() {
    if [[ -n "$XVFB_PID" ]]; then
        kill "$XVFB_PID" 2>/dev/null || true
        wait "$XVFB_PID" 2>/dev/null || true
    fi
    rm -f "$DISPLAY_FILE"
}
trap cleanup EXIT

# shellcheck disable=SC2086 # XVFB_DISPLAY is either empty or one ":N" word.
Xvfb ${XVFB_DISPLAY:-} -displayfd 3 -screen 0 1280x720x24 -nolisten tcp \
    3>"$DISPLAY_FILE" >/dev/null 2>&1 &
XVFB_PID=$!
for _ in $(seq 100); do
    [[ -s "$DISPLAY_FILE" ]] && break
    kill -0 "$XVFB_PID" 2>/dev/null || break
    sleep 0.1
done
DISPLAY_NUM="$(tr -d '[:space:]' <"$DISPLAY_FILE")"
if [[ -z "$DISPLAY_NUM" ]] || ! kill -0 "$XVFB_PID" 2>/dev/null; then
    echo "Xvfb did not start${XVFB_DISPLAY:+ on $XVFB_DISPLAY}" >&2
    exit 2
fi

export DISPLAY=":$DISPLAY_NUM"
export VK_ICD_FILENAMES="$LVP_ICD"   # older loaders
export VK_DRIVER_FILES="$LVP_ICD"    # loader >= 1.3.207
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-dummy}"

python3 "$ROOT/scripts/run-test.py" "$@"
