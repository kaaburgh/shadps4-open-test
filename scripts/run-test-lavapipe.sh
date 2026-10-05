#!/usr/bin/env bash
# Run a test on shadPS4 with Mesa lavapipe (CPU Vulkan) under a private Xvfb.
# Usage: SHADPS4=/path/to/shadps4 scripts/run-test-lavapipe.sh <test> [run-test.py args...]
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LVP_ICD="${LVP_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}"
[[ -f "$LVP_ICD" ]] || { echo "lavapipe ICD not found: $LVP_ICD (install mesa-vulkan-drivers)" >&2; exit 2; }
command -v Xvfb >/dev/null || { echo "Xvfb not found (install xvfb)" >&2; exit 2; }

DISPLAY_NUM="${XVFB_DISPLAY:-:97}"
Xvfb "$DISPLAY_NUM" -screen 0 1280x720x24 -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
trap 'kill "$XVFB_PID" 2>/dev/null || true' EXIT
for _ in $(seq 50); do
    [[ -e "/tmp/.X11-unix/X${DISPLAY_NUM#:}" ]] && break
    sleep 0.1
done

export DISPLAY="$DISPLAY_NUM"
export VK_ICD_FILENAMES="$LVP_ICD"   # older loaders
export VK_DRIVER_FILES="$LVP_ICD"    # loader >= 1.3.207
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-dummy}"

python3 "$ROOT/scripts/run-test.py" "$@"
