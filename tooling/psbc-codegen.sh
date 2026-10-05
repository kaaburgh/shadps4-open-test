#!/usr/bin/env bash
# Generate the Mesa codegen outputs that the pinned opengnm-psbc tree lists in
# its .gitignore but whose standalone Makefile does not produce. Commands mirror
# the custom_target() definitions in the vendored Mesa meson.build files.
set -euo pipefail

PSBC="${1:?usage: psbc-codegen.sh <opengnm-psbc dir>}"
PY="${PYTHON:-python3}"
cd "$PSBC"

gen() { # gen <output> <command...>: run command, capture stdout into output
    local out="$1"; shift
    [[ -s "$out" ]] && return 0
    "$@" >"$out.tmp" && mv -- "$out.tmp" "$out"
}

F=src/util/format
gen $F/u_format_gen.h   "$PY" $F/u_format_table.py $F/u_format.yaml --enums
gen $F/u_format_pack.h  "$PY" $F/u_format_table.py $F/u_format.yaml --header
gen $F/u_format_table.c "$PY" $F/u_format_table.py $F/u_format.yaml
gen src/util/format_srgb.c "$PY" src/util/format_srgb.py
gen src/util/shader_stats.h "$PY" src/util/process_shader_stats.py \
    src/util/shader_stats.rnc src/util/shader_stats.xml

[[ -s src/compiler/builtin_types.h ]] || "$PY" src/compiler/builtin_types_h.py src/compiler/builtin_types.h
[[ -s src/compiler/builtin_types.c ]] || "$PY" src/compiler/builtin_types_c.py src/compiler/builtin_types.c

P=src/amd/packets
for g in gfx11 gfx12; do
    gen src/amd/common/amd_cp_packets_$g.h "$PY" $P/parse_cp_pm4_table_data_json.py \
        $P/cp_pm4_table_data_gfx11.json $P/pm4_it_opcodes_gfx11.h \
        $P/cp_pm4_table_data_gfx12.json $P/pm4_it_opcodes_gfx12.h $g packets_h
done

R=src/amd/registers
gen src/amd/common/gfx10_format_table.c "$PY" src/amd/common/gfx10_format_table.py \
    $F/u_format.yaml $R/gfx10-rsrc.json $R/gfx11-rsrc.json

VKXML=../Vulkan-Headers/registry/vk.xml
[[ -s src/vulkan/util/vk_struct_type_cast.h ]] || "$PY" src/vulkan/util/vk_struct_type_cast_gen.py \
    --xml "$VKXML" --out src/vulkan/util/vk_struct_type_cast.h --beta false
