#!/usr/bin/env bash
# Boots game_update_overlay from base game folders with different names on
# lavapipe and checks which folder's overlay.txt the guest sees in /app0.
# Every folder gets its own overlay.txt; the base folder's says "base".
# Exits 0 only if every case passes.
#
#   plain        TEST00001      + TEST00001-patch                -> update
#   app_patch    TEST00001-app  + TEST00001-patch                -> update
#   app_update   TEST00001-app  + TEST00001-UPDATE               -> update
#   app_exact    TEST00001-app  + TEST00001-app-patch            -> update
#   exact_first  TEST00001-app  + TEST00001-app-patch ("exact")
#                               + TEST00001-patch ("title")      -> exact
#   other_title  TEST00001-app  + TEST00002-patch                -> base
#
# Extracted packages are often laid out as <title>-app next to
# <title>-patch (shadps4-emu/shadPS4#3651).
#
# usage: SHADPS4=/path/to/shadps4 tests/game_update_overlay/run.sh
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/out/game_update_overlay"
ELF="$OUT/game_update_overlay.elf"
GAMES="$OUT/games"

if [[ ! -f $ELF ]]; then
    echo "missing $ELF; run ./scripts/build-test.sh game_update_overlay" >&2
    exit 2
fi

status=0
# run <case> <expected overlay> <base folder> [<folder>=<overlay.txt text>]...
run() {
    local name=$1 expected=$2 base=$3
    shift 3
    rm -rf "$GAMES"
    mkdir -p "$GAMES/$base"
    cp "$ELF" "$GAMES/$base/eboot.bin"
    echo base >"$GAMES/$base/overlay.txt"
    for folder in "$@"; do
        mkdir -p "$GAMES/${folder%%=*}"
        echo "${folder#*=}" >"$GAMES/${folder%%=*}/overlay.txt"
    done
    bash "$ROOT/scripts/run-test-lavapipe.sh" game_update_overlay --timeout 60 \
        --elf "$GAMES/$base/eboot.bin" >"$OUT/run-$name.log" 2>&1
    local rc=$?
    local got
    got=$(grep -a -o 'SHADTEST name=game_update_overlay status=PASS overlay=[^[:cntrl:]]*' \
        "$OUT/run-$name.log" | tail -1 | sed 's/.*overlay=//')
    if [[ $rc -eq 0 && $got == "$expected" ]]; then
        echo "$name: PASS overlay=$got"
    else
        echo "$name: FAIL overlay=${got:-?} expected=$expected (exit $rc)"
        status=1
    fi
}

run plain update TEST00001 TEST00001-patch=update
run app_patch update TEST00001-app TEST00001-patch=update
run app_update update TEST00001-app TEST00001-UPDATE=update
run app_exact update TEST00001-app TEST00001-app-patch=update
run exact_first exact TEST00001-app TEST00001-app-patch=exact TEST00001-patch=title
run other_title base TEST00001-app TEST00002-patch=update

rm -rf "$GAMES"
exit $status
