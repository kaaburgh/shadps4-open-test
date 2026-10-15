#!/usr/bin/env bash
# Runs present_transfer on a headless GNOME compositor (mutter) with one
# 3840x2160 virtual monitor at 100 % and 200 % scale, on lavapipe, and checks
# the size of the swapchain shadPS4 creates:
#
#   fullscreen-100  full screen, scale 1  -> 3840x2160
#   fullscreen-200  full screen, scale 2  -> 3840x2160 (the output's pixels,
#                                            not its 1920x1080 logical size)
#   windowed-200    1280x720 window, scale 2 -> 2560x1440
#
# Each run must create the swapchain once (no resize loop) and the guest must
# flip for its whole run.
#
# The scale is set with org.gnome.Mutter.DisplayConfig.ApplyMonitorsConfig
# over a private D-Bus session. The swapchain size is read with gdb from
# Vulkan::Swapchain::Create's arguments, so the shadPS4 binary needs its
# symbols (the default build has them). Needs mutter (46 or later, for
# --headless and --virtual-monitor), dbus-run-session, gdbus and gdb.
#
# usage: SHADPS4=/path/to/shadps4 tests/present_transfer/run-wayland-scale.sh
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/out/present_transfer"
REAL="${SHADPS4:?set SHADPS4 to the shadps4 binary}"
LVP_ICD="${LVP_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}"
for tool in mutter dbus-run-session gdbus gdb; do
    command -v "$tool" >/dev/null || { echo "$tool not found" >&2; exit 2; }
done
[[ -f $LVP_ICD ]] || { echo "lavapipe ICD not found: $LVP_ICD" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat >"$WORK/swapchain.gdb" <<'GDB'
set pagination off
set confirm off
handle SIGSEGV nostop noprint pass
handle SIGBUS nostop noprint pass
handle SIGILL nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGPIPE nostop noprint pass
handle SIG32 nostop noprint pass
handle SIG33 nostop noprint pass
handle SIG34 nostop noprint pass
handle SIG35 nostop noprint pass
break Vulkan::Swapchain::Create
commands
silent
printf "SWAPCHAIN %ux%u\n", $rsi, $rdx
continue
end
run
GDB
cat >"$WORK/shadps4" <<WRAP
#!/usr/bin/env bash
exec gdb -q -batch -nx -x "$WORK/swapchain.gdb" --args "$REAL" "\$@"
WRAP
chmod +x "$WORK/shadps4"

# One boot inside its own D-Bus session: start mutter, set the scale, run.
cat >"$WORK/session.sh" <<'SESSION'
#!/usr/bin/env bash
set -uo pipefail
scale=$1 full=$2 log=$3
mutter --wayland --headless --no-x11 --virtual-monitor 3840x2160 \
    --wayland-display wayland-shadtest >"$log.mutter" 2>&1 &
mutter_pid=$!
for _ in $(seq 100); do
    [[ -S $XDG_RUNTIME_DIR/wayland-shadtest ]] && break
    sleep 0.1
done
config=(--session --dest org.gnome.Mutter.DisplayConfig
        --object-path /org/gnome/Mutter/DisplayConfig)
state=""
for _ in $(seq 50); do
    state=$(gdbus call "${config[@]}" \
        --method org.gnome.Mutter.DisplayConfig.GetCurrentState 2>/dev/null) && break
    sleep 0.1
done
serial=$(sed -n 's/^(uint32 \([0-9]*\),.*/\1/p' <<<"$state")
monitor=$(grep -o "('Meta-0', '[0-9x@.]*'" <<<"$state" | head -1 | cut -d"'" -f4)
monitor=${monitor:-3840x2160@60.000}
gdbus call "${config[@]}" --method org.gnome.Mutter.DisplayConfig.ApplyMonitorsConfig \
    "$serial" 1 "[(0, 0, $scale.0, uint32 0, true, [('Meta-0', '$monitor', @a{sv} {})])]" \
    "@a{sv} {}" >>"$log.mutter" 2>&1
env -u DISPLAY WAYLAND_DISPLAY=wayland-shadtest SDL_VIDEO_DRIVER=wayland \
    python3 "$ROOT/scripts/run-test.py" present_transfer --shadps4 "$WORK/shadps4" \
    --timeout 90 --set "GPU.full_screen=$full" >"$log" 2>&1
kill "$mutter_pid" 2>/dev/null
wait "$mutter_pid" 2>/dev/null
SESSION

export ROOT WORK
export XDG_RUNTIME_DIR="$WORK/runtime" XDG_CONFIG_HOME="$WORK/config"
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-dummy}"
export VK_ICD_FILENAMES="$LVP_ICD" VK_DRIVER_FILES="$LVP_ICD"
mkdir -m 700 "$XDG_RUNTIME_DIR"
mkdir -p "$XDG_CONFIG_HOME"

status=0
run() { # <name> <scale> <full_screen> <expected swapchain>
    local name=$1 log="$OUT/run-wayland-$1.log"
    rm -f "$XDG_RUNTIME_DIR"/wayland-shadtest*
    dbus-run-session -- bash "$WORK/session.sh" "$2" "$3" "$log" >/dev/null 2>&1
    local result chains verdict=PASS
    result=$(grep -a -o 'HOST_RESULT.*' "$log" | tail -1)
    chains=$(grep -a -o 'SWAPCHAIN [0-9]*x[0-9]*' "$log" | cut -d' ' -f2 | tr '\n' ' ')
    [[ $result == *status=PASS* ]] || verdict=FAIL
    [[ $chains == "$4 " ]] || verdict=FAIL
    echo "WAYLAND_SCALE $name swapchains=[${chains% }] expected=$4 ${result:-no HOST_RESULT} -> $verdict"
    [[ $verdict == PASS ]] || status=1
}

run fullscreen-100 1 true 3840x2160
run fullscreen-200 2 true 3840x2160
run windowed-200 2 false 2560x1440
exit $status
