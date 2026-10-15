#!/usr/bin/env bash
# Runs hle_audio_queue with SDL's disk audio driver and checks what was
# played with tools/analyze.py.
#
# SDL_AUDIO_DISK_TIMESCALE stretches the disk device's wait per buffer: at
# 1.0 it plays slightly faster than real time (its delay is rounded down to
# whole milliseconds), at 1.15 about 11 % slower, so the host queue grows and
# shadPS4 has to drop audio to keep latency bounded.
#
# usage: SHADPS4=/path/to/shadps4 tests/hle_audio_queue/run.sh [timescale [max gap]]...
#   default: "1.0 0" (nothing may be dropped) and "1.15 1024"
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$ROOT/tests/hle_audio_queue"
OUT="$ROOT/out/hle_audio_queue"
runs=("$@")
[[ ${#runs[@]} -eq 0 ]] && runs=("1.0 0" "1.15 1024")

status=0
for run in "${runs[@]}"; do
    read -r scale max_gap <<<"$run"
    raw="$OUT/played-$scale.f32"
    rm -f "$raw"
    SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE="$raw" SDL_AUDIO_DISK_TIMESCALE="$scale" \
        SDL_AUDIO_FORMAT=F32 SDL_AUDIO_CHANNELS=2 SDL_AUDIO_FREQUENCY=48000 \
        bash "$ROOT/scripts/run-test-lavapipe.sh" hle_audio_queue --timeout 60 \
        >"$OUT/run-$scale.log" 2>&1
    marker=$(grep -a -o 'SHADTEST name=hle_audio_queue status=[A-Z]* .*' "$OUT/run-$scale.log" | tail -1)
    echo "timescale=$scale: $marker"
    emitted=$(sed -n 's/.*frames=\([0-9]*\).*/\1/p' <<<"$marker")
    if [[ -z $emitted || ! -s $raw ]]; then
        echo "  no guest result or nothing played"
        status=1
        continue
    fi
    python3 "$DIR/tools/analyze.py" "$raw" "$emitted" --max-gap "$max_gap" | sed 's/^/  /' \
        || status=1
done
exit $status
