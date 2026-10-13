#!/usr/bin/env bash
# Runs hle_language once per scenario on lavapipe. Each scenario sets the
# title's param.sfo, shadPS4's global console_language and per-title
# custom_configs/<serial>.json files, and the language the guest must see.
# Exits 0 only if every scenario passes.
#
# Global language 3; per-title overrides TEST00001 -> 4, TEST00002 -> 5;
# TEST00009 has no per-title file.
#
#   no_param_sfo        no sce_sys/param.sfo, so no serial          -> 3
#   unknown_title       TEST00009                                   -> 3
#   title_a             TEST00001                                   -> 4
#   title_b             TEST00002                                   -> 5
#   title_a_relaunch    relaunch on title_a's user directory after
#                       changing TEST00001's override to 6          -> 6
#   unknown_relaunch    relaunch on that user directory, TEST00009  -> 3
#
# The relaunch scenarios start from the user directory (config.json,
# custom_configs, ...) the previous run left behind, so they also check that a
# title's override is not saved into the global config.
#
# usage: SHADPS4=/path/to/shadps4 tests/hle_language/run.sh
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$ROOT/tests/hle_language"
OUT="$ROOT/out/hle_language"
GLOBAL=3

fresh_user() {
    rm -rf "$OUT/user"
    mkdir -p "$OUT/user/custom_configs"
    override TEST00001 4
    override TEST00002 5
}

# Continue from the user directory the last run left behind.
carry_user() {
    rm -rf "$OUT/user"
    cp -a "$OUT/xdg-data/shadPS4" "$OUT/user"
}

override() {
    printf '{"General": {"console_language": %d}}\n' "$2" \
        >"$OUT/user/custom_configs/$1.json"
}

status=0
run() { # <case> <TITLE_ID or -> <expected language>
    rm -rf "$OUT/sce_sys"
    [[ $2 != - ]] && python3 "$DIR/tools/make_sfo.py" "$OUT/sce_sys/param.sfo" "$2"
    echo "$1 $3" >"$OUT/expect.txt"
    bash "$ROOT/scripts/run-test-lavapipe.sh" hle_language \
        --set "General.console_language=$GLOBAL" >"$OUT/run-$1.log" 2>&1
    local rc=$?
    echo "$1: $(grep -a -o 'SHADTEST name=hle_language.*' "$OUT/run-$1.log" | tail -1) (exit $rc)"
    [[ $rc -eq 0 ]] || status=1
}

fresh_user
run no_param_sfo - "$GLOBAL"
fresh_user
run unknown_title TEST00009 "$GLOBAL"
fresh_user
run title_b TEST00002 5
fresh_user
run title_a TEST00001 4
carry_user
override TEST00001 6
run title_a_relaunch TEST00001 6
carry_user
run unknown_relaunch TEST00009 "$GLOBAL"

rm -rf "$OUT/sce_sys" "$OUT/expect.txt"
exit $status
