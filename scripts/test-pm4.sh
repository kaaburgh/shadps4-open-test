#!/usr/bin/env bash
# Build and run the host self-test for tests/common/shadtest_pm4.h.
# Needs the pinned OpenGNM headers from ./scripts/bootstrap-deps.sh.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OPENGNM="$ROOT/.deps/opengnm"
[[ -f "$OPENGNM/include/pm4/sid.h" ]] || {
    echo "missing OpenGNM headers; run ./scripts/bootstrap-deps.sh" >&2
    exit 2
}

mkdir -p "$ROOT/build/pm4_selftest"
"${HOST_CC:-cc}" -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -I"$OPENGNM/include" -I"$ROOT/tests/common" \
    "$ROOT/tests/common/pm4_selftest.c" -o "$ROOT/build/pm4_selftest/pm4_selftest"
"$ROOT/build/pm4_selftest/pm4_selftest"
