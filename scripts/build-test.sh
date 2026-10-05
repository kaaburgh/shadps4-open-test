#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TEST="${1:-gpu_solid_rt}"

if [[ ! "$TEST" =~ ^[A-Za-z0-9_]+$ || ! -f "$ROOT/tests/$TEST/Makefile" ]]; then
    printf 'unknown test: %s\n' "$TEST" >&2
    exit 2
fi

make -C "$ROOT/tests/$TEST" all

printf 'Built %s -> %s/out/%s\n' "$TEST" "$ROOT" "$TEST"
