#!/usr/bin/env bash
# Boots kernel_memory_size on lavapipe under several memory configurations
# and compares the direct and flexible sizes the guest reports with the
# default run. Every run must also pass the guest's own checks: it maps all
# the flexible memory it is told is available. Exits 0 only if every run
# passes.
#
#   default         nothing changed                     -> D0, F0
#   extra_fmem      General.extra_fmem_in_mbytes=256    -> D0,           F0 + 256 MiB
#   extra_dmem      General.extra_dmem_in_mbytes=256    -> D0 + 256 MiB, F0
#   title_flex_768  the title asks for 768 MiB flexible -> D0 - 320 MiB, F0 + 320 MiB
#   title_flex_256  the title asks for 256 MiB flexible -> D0 + 192 MiB, F0 - 192 MiB
#   pro             General.neo_mode=true and a param.sfo
#                   whose ATTRIBUTE supports Pro mode   -> neo=1, D > D0
#   devkit          General.dev_kit_mode=true           -> D > D0
#
# Without a title request the flexible size is 448 MiB. A title asks for
# another size through its process parameters' mem_param, made here by
# tools/set_flexible_size.py; shadPS4 takes the difference out of direct
# memory. The exact Pro and devkit sizes are shadPS4's own constants, so
# only their direction is checked.
#
# usage: SHADPS4=/path/to/shadps4 tests/kernel_memory_size/run.sh
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$ROOT/tests/kernel_memory_size"
OUT="$ROOT/out/kernel_memory_size"
ELF="$OUT/kernel_memory_size.elf"
RAW="$ROOT/build/kernel_memory_size/kernel_memory_size.raw.elf"
TOOLCHAIN="$ROOT/.deps/openorbis"
MIB=$((1024 * 1024))

if [[ ! -f $ELF || ! -f $RAW ]]; then
    echo "missing $ELF or $RAW; run ./scripts/build-test.sh kernel_memory_size" >&2
    exit 2
fi
cp "$ELF" "$OUT/default.elf"

# Writes $ELF as a copy of the guest whose title asks for $1 bytes of
# flexible memory.
title_flexible() {
    python3 "$DIR/tools/set_flexible_size.py" "$RAW" "$OUT/title.raw.elf" "$1" &&
        OO_PS4_TOOLCHAIN="$TOOLCHAIN" "$TOOLCHAIN/bin/linux/create-fself" \
            -in="$OUT/title.raw.elf" -out="$OUT/title.oelf" --eboot "$ELF" \
            --paid 0x3800000000000011 >/dev/null
}

status=0
D0=
F0=
D=
F=
NEO=

# run <case> [run-test.py args...]: boots the guest, sets D, F and NEO.
run() {
    local name=$1
    shift
    bash "$ROOT/scripts/run-test-lavapipe.sh" kernel_memory_size --timeout 60 "$@" \
        >"$OUT/run-$name.log" 2>&1
    local rc=$?
    local marker
    marker=$(grep -a -o 'SHADTEST name=kernel_memory_size status=[A-Z]* [^[:cntrl:]]*' \
        "$OUT/run-$name.log" | tail -1)
    D=$(sed -n 's/.* direct=\(0x[0-9a-f]*\).*/\1/p' <<<"$marker")
    F=$(sed -n 's/.* flexible=\(0x[0-9a-f]*\).*/\1/p' <<<"$marker")
    NEO=$(sed -n 's/.* neo=\([0-9]*\).*/\1/p' <<<"$marker")
    if [[ $rc -ne 0 || $marker != *status=PASS* ]]; then
        local why=${marker:-no result marker}
        if [[ -z $marker ]]; then
            why+=", $(grep -a -o 'Assertion Failed!\|Failed to map physical memory' \
                "$OUT/run-$name.log" | tr '\n' ' ')"
        fi
        echo "$name: FAIL ($why, exit $rc)"
        status=1
        return 1
    fi
    return 0
}

# expect <case> <direct MiB delta or "more"> <flexible MiB delta>
expect() {
    local name=$1 ddelta=$2 fdelta=$3 ok=1
    if [[ $ddelta == more ]]; then
        ((D > D0)) || ok=0
    else
        ((D == D0 + ddelta * MIB)) || ok=0
    fi
    ((F == F0 + fdelta * MIB)) || ok=0
    local got="direct=$D ($(((D - D0) / MIB)) MiB) flexible=$F ($(((F - F0) / MIB)) MiB) neo=$NEO"
    if ((ok)); then
        echo "$name: PASS $got"
    else
        echo "$name: FAIL $got, expected direct $ddelta MiB, flexible $fdelta MiB"
        status=1
    fi
}

rm -rf "$OUT/sce_sys"
if run default; then
    D0=$D
    F0=$F
    echo "default: PASS direct=$D0 flexible=$F0 neo=$NEO"
else
    cp "$OUT/default.elf" "$ELF"
    exit 1
fi

run extra_fmem --set General.extra_fmem_in_mbytes=256 && expect extra_fmem 0 256
run extra_dmem --set General.extra_dmem_in_mbytes=256 && expect extra_dmem 256 0

for case in "768 -320 320" "256 192 -192"; do
    read -r mib ddelta fdelta <<<"$case"
    if title_flexible $((mib * MIB)); then
        run "title_flex_$mib" && expect "title_flex_$mib" "$ddelta" "$fdelta"
    else
        echo "title_flex_$mib: FAIL (could not make the executable)"
        status=1
    fi
done
cp "$OUT/default.elf" "$ELF"

python3 "$DIR/tools/make_sfo.py" "$OUT/sce_sys/param.sfo"
if run pro --set General.neo_mode=true; then
    if [[ $NEO == 1 ]]; then
        expect pro more 0
    else
        echo "pro: FAIL neo=$NEO"
        status=1
    fi
fi
rm -rf "$OUT/sce_sys"

run devkit --set General.dev_kit_mode=true && expect devkit more 0

rm -f "$OUT/default.elf" "$OUT/title.raw.elf" "$OUT/title.oelf"
exit $status
