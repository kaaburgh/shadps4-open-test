#!/usr/bin/env bash
# Boots loader_procparam with its PT_SCE_PROCPARAM header declaring several
# segment sizes and checks the SDK version the guest sees.
#
#   original   as built (0x50 with OpenOrbis): the reference version, not 0
#   0x18       just holds the SDK version          -> the reference version
#   0x17       too short to hold it                 -> any version (printed)
#   0x80       larger than the fields shadPS4 knows -> the reference version
#   0x1000     4 KiB                                 -> the reference version
#   0x100000   1 MiB, past the end of the file      -> the reference version
#
# Every boot must reach the guest and pass; the declared size only changes
# what shadPS4 reads when it looks up the SDK version.
#
# usage: SHADPS4=/path/to/shadps4 tests/loader_procparam/run.sh
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$ROOT/tests/loader_procparam"
OUT="$ROOT/out/loader_procparam"
ELF="$OUT/loader_procparam.elf"

[[ -f $ELF ]] || { echo "missing $ELF; run ./scripts/build-test.sh loader_procparam" >&2; exit 2; }
cp "$ELF" "$OUT/original.elf"
trap 'cp "$OUT/original.elf" "$ELF"' EXIT

status=0
reference=
run() { # <name> <size or -> <expected: ref|0>
    local name=$1 size=$2 want=$3
    cp "$OUT/original.elf" "$ELF"
    [[ $size != - ]] && python3 "$DIR/tools/set_procparam_size.py" "$OUT/original.elf" "$ELF" "$size" >/dev/null
    bash "$ROOT/scripts/run-test-lavapipe.sh" loader_procparam --timeout 30 >"$OUT/run-$name.log" 2>&1
    local result version
    result=$(grep -a -o 'HOST_RESULT.*' "$OUT/run-$name.log" | tail -1)
    version=$(sed -n 's/.*sdk_version=\(0x[0-9a-f]*\).*/\1/p' <<<"$result")
    if [[ $name == original ]]; then
        reference=$version
        want=$version
    fi
    [[ $want == ref ]] && want=$reference
    [[ $want == any ]] && want=$version
    local verdict=PASS
    [[ $result == *status=PASS* && -n $version && $version == "$want" ]] || verdict=FAIL
    [[ $name == original && ( -z $version || $version == 0x00000000 ) ]] && verdict=FAIL
    echo "PROCPARAM $name size=$size sdk_version=${version:-none} expected=$want ${result:-no HOST_RESULT} -> $verdict"
    [[ $verdict == PASS ]] || status=1
}

run original - ref
run min 0x18 ref
run short 0x17 any
run larger 0x80 ref
run page 0x1000 ref
run huge 0x100000 ref
exit $status
