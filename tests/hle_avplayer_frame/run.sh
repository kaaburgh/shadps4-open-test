#!/usr/bin/env bash
# Plays a losslessly encoded test pattern through AvPlayer at several sizes
# on lavapipe; the guest compares the first frame byte for byte. Exits 0
# only if every size passes. Needs ffmpeg with libx264.
#
#   1920x1080  width a multiple of 64, 8 rows of height padding
#   1280x720   no padding
#   854x480    width neither a multiple of 16 nor of 64
#   720x406    height padding with an odd number of chroma rows (203)
#
# usage: SHADPS4=/path/to/shadps4 tests/hle_avplayer_frame/run.sh [WxH]...
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$ROOT/tests/hle_avplayer_frame"
OUT="$ROOT/out/hle_avplayer_frame"
sizes=("$@")
[[ ${#sizes[@]} -eq 0 ]] && sizes=(1920x1080 1280x720 854x480 720x406)

status=0
for size in "${sizes[@]}"; do
    w=${size%x*}
    h=${size#*x}
    if ! python3 "$DIR/tools/make_video.py" "$OUT/video.mp4" "$w" "$h"; then
        echo "$size: FAIL (could not encode the video)"
        status=1
        continue
    fi
    echo "$w $h" >"$OUT/video.txt"
    bash "$ROOT/scripts/run-test-lavapipe.sh" hle_avplayer_frame --timeout 60 \
        >"$OUT/run-$size.log" 2>&1
    rc=$?
    marker=$(grep -a -o 'SHADTEST name=hle_avplayer_frame status=[A-Z]* [^[:cntrl:]]*' \
        "$OUT/run-$size.log" | tail -1)
    echo "$size: ${marker#SHADTEST name=hle_avplayer_frame } (exit $rc)"
    [[ $rc -eq 0 ]] || status=1
done
rm -f "$OUT/video.mp4" "$OUT/video.txt"
exit $status
