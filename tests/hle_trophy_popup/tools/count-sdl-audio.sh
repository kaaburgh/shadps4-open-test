#!/usr/bin/env bash
# Runs hle_trophy_popup with shadPS4 under gdb and checks that the trophy
# popups release every SDL audio resource they take: each opened audio device
# is closed, each audio stream destroyed, and each SDL audio subsystem
# reference returned. shadPS4's window takes one audio reference at startup
# and keeps it, so one more init than quit is expected. The breakpoints sit on
# SDL's public entry points, so SDL's internal calls are not counted.
#
# usage: SHADPS4=/path/to/shadps4 tests/hle_trophy_popup/tools/count-sdl-audio.sh \
#            [run-test-lavapipe.sh args, e.g. --set Audio.audio_backend=1]
# Needs gdb and a shadPS4 binary with symbols for the SDL3 functions (the
# default build has them). Exits 0 when balanced, 1 when not, 2 when the run
# itself failed.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
REAL="${SHADPS4:?set SHADPS4 to the shadps4 binary}"
command -v gdb >/dev/null || { echo "gdb not found" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat >"$WORK/count.gdb" <<'GDB'
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
break SDL_OpenAudioDevice
commands
silent
printf "SDLCOUNT open_device\n"
continue
end
break SDL_CloseAudioDevice
commands
silent
printf "SDLCOUNT close_device\n"
continue
end
break SDL_CreateAudioStream
commands
silent
printf "SDLCOUNT create_stream\n"
continue
end
break SDL_DestroyAudioStream
commands
silent
printf "SDLCOUNT destroy_stream\n"
continue
end
break SDL_Init
commands
silent
printf "SDLCOUNT init 0x%x\n", $rdi
continue
end
break SDL_InitSubSystem
commands
silent
printf "SDLCOUNT init 0x%x\n", $rdi
continue
end
break SDL_QuitSubSystem
commands
silent
printf "SDLCOUNT quit 0x%x\n", $rdi
continue
end
run
GDB

cat >"$WORK/shadps4" <<WRAP
#!/usr/bin/env bash
exec gdb -q -batch -nx -x "$WORK/count.gdb" --args "$REAL" "\$@"
WRAP
chmod +x "$WORK/shadps4"

LOG="$ROOT/out/hle_trophy_popup/count-sdl-audio.log"
set +e
SHADPS4="$WORK/shadps4" bash "$ROOT/scripts/run-test-lavapipe.sh" hle_trophy_popup \
    --timeout 180 "$@" >"$LOG" 2>&1
status=$?
set -e
grep -a "HOST_RESULT" "$LOG" || true
if [[ $status -ne 0 ]]; then
    echo "run failed ($status); see $LOG" >&2
    exit 2
fi

python3 - "$LOG" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
count = lambda name: len(re.findall(rf"SDLCOUNT {name}\b", text))
SDL_INIT_AUDIO = 0x10
audio = lambda op: sum(1 for v in re.findall(rf"SDLCOUNT {op} (0x[0-9a-f]+)", text)
                       if int(v, 16) & SDL_INIT_AUDIO)
opened, closed = count("open_device"), count("close_device")
created, destroyed = count("create_stream"), count("destroy_stream")
inits, quits = audio("init"), audio("quit")
print(f"audio devices opened {opened}, closed {closed}")
print(f"audio streams created {created}, destroyed {destroyed}")
print(f"audio subsystem inits {inits}, quits {quits} (the window keeps one)")
ok = opened == closed and created == destroyed and inits - 1 == quits and opened > 0
print("SDL_AUDIO_BALANCE " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
PY
