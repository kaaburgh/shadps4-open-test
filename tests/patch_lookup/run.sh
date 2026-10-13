#!/usr/bin/env bash
# Boots patch_lookup with "-p <name>" and a generated patch XML placed in
# the user's patches directory in several ways, and checks whether shadPS4
# applied it.
#
#   provider       patches/TestProvider/patch_lookup.xml, -p patch_lookup.xml
#                  -> applied (the case of upstream shadPS4 issue #4630)
#   direct         patches/patch_lookup.xml, -p patch_lookup.xml       -> applied
#   provider_path  patches/TestProvider/patch_lookup.xml,
#                  -p TestProvider/patch_lookup.xml                     -> applied
#   ambiguous      the same file in ProviderA and ProviderB,
#                  -p patch_lookup.xml                                  -> not applied
#   missing        no patch file, -p patch_lookup.xml                   -> not applied
#   traversal      patches/TestProvider/patch_lookup.xml,
#                  -p ../patch_lookup.xml                               -> not applied
#
# The patch is a "mask" patch: it searches the executable's memory for the
# guest's marker string, so it needs no addresses and applies to any app
# version.
#
# usage: SHADPS4=/path/to/shadps4 tests/patch_lookup/run.sh
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/out/patch_lookup"
PATTERN="53 48 41 44 54 45 53 54 2D 50 41 54 43 48 2D 4C 4F 4F 4B 55 50 2D 42 45 46 4F 52 45 2D 36 66 33 61 39 63"

[[ -f $OUT/patch_lookup.elf ]] || { echo "missing $OUT/patch_lookup.elf; run ./scripts/build-test.sh patch_lookup" >&2; exit 2; }

write_patch() { # <path under patches/>
    mkdir -p "$(dirname "$OUT/user/patches/$1")"
    cat >"$OUT/user/patches/$1" <<XML
<?xml version="1.0" encoding="utf-8"?>
<Patch>
    <Metadata Title="shadps4-open-test" Name="patch_lookup marker" Note="Generated test patch"
              Author="shadps4-open-test" PatchVer="1.0" AppVer="00.00"
              AppElf="patch_lookup.elf" isEnabled="true">
        <PatchList>
            <Line Type="mask" Address="$PATTERN" Value="414654455221" Offset="22"/>
        </PatchList>
    </Metadata>
</Patch>
XML
}

status=0
run() { # <name> <-p argument> <patched|unpatched> <patch path>...
    local name=$1 arg=$2 want=$3
    shift 3
    rm -rf "$OUT/user"
    mkdir -p "$OUT/user/patches"
    for path in "$@"; do
        write_patch "$path"
    done
    echo "$want" >"$OUT/expect.txt"
    bash "$ROOT/scripts/run-test-lavapipe.sh" patch_lookup --timeout 30 \
        "--emu-arg=-p" "--emu-arg=$arg" >"$OUT/run-$name.log" 2>&1
    local rc=$?
    local result
    result=$(grep -a -o 'HOST_RESULT.*' "$OUT/run-$name.log" | tail -1)
    echo "PATCH_LOOKUP $name -p $arg expected=$want ${result:-no HOST_RESULT} (exit $rc)"
    [[ $rc -eq 0 ]] || status=1
}

run provider patch_lookup.xml patched TestProvider/patch_lookup.xml
run direct patch_lookup.xml patched patch_lookup.xml
run provider_path TestProvider/patch_lookup.xml patched TestProvider/patch_lookup.xml
run ambiguous patch_lookup.xml unpatched ProviderA/patch_lookup.xml ProviderB/patch_lookup.xml
run missing patch_lookup.xml unpatched
run traversal ../patch_lookup.xml unpatched TestProvider/patch_lookup.xml

rm -rf "$OUT/user" "$OUT/expect.txt"
exit $status
