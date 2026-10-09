#!/usr/bin/env bash
# Run one test in the six configurations of docs/uma-e1c-e3-scenario-plan.md:
# mirror and shared backing, each with readbacks Precise, Relaxed and Disabled.
#
# Usage: SHADPS4=/path/to/shadps4 scripts/run-matrix.sh <test> [run-test.py args...]
#
# RUNNER selects the launcher (default: scripts/run-test-lavapipe.sh). Each run's
# output is kept in out/<test>/matrix-<backing>-<readbacks>.log. The summary
# prints the runner's exit status, the HOST_RESULT line and how many times
# shadPS4 logged a shared-backing fallback ("falls back to the mirror", UMA E3
# research builds only).
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TEST="${1:?usage: run-matrix.sh <test> [run-test.py args...]}"
shift
RUNNER="${RUNNER:-$ROOT/scripts/run-test-lavapipe.sh}"
OUT="$ROOT/out/$TEST"
mkdir -p "$OUT"

status=0
for backing in mirror shared; do
    for readbacks in precise relaxed disabled; do
        log="$OUT/matrix-$backing-$readbacks.log"
        args=(--readbacks "$readbacks")
        [[ "$backing" == shared ]] && args+=(--shared-backing)
        bash "$RUNNER" "$TEST" "${args[@]}" "$@" >"$log" 2>&1
        rc=$?
        [[ $rc -eq 2 ]] && status=2
        result="$(grep -o 'HOST_RESULT.*' "$log" | tail -n 1 | cut -c1-160)"
        fallbacks="$(grep -c 'falls back to the mirror' "$log")"
        printf '%-6s %-8s rc=%d fallbacks=%-3s %s\n' "$backing" "$readbacks" "$rc" \
            "$fallbacks" "${result:-<no marker>}"
    done
done
exit "$status"
