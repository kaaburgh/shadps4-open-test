#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TEST="${1:-gpu_solid_rt}"

case "$TEST" in
    gpu_solid_rt)
        make -C "$ROOT/tests/gpu_solid_rt" all
        ;;
    *)
        printf 'unknown test: %s\n' "$TEST" >&2
        exit 2
        ;;
esac

printf 'Built %s -> %s/out/%s\n' "$TEST" "$ROOT" "$TEST"
