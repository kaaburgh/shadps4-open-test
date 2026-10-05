#!/usr/bin/env python3
"""Compile and validate the standalone psbc set-0 buffer descriptor ABI.

This intentionally decodes the small GFX7 instruction subset needed by the
contract instead of relying on llvm-objdump, which does not disassemble this
subtarget in LLVM 19.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile


PSSL_HEADER_SIZE = 0x24
GNM_FILE_HEADER_SIZE = 0x10
ORB_MAGIC = b"OrbShdr"
PTR_INDIRECTRESOURCETABLE = 0x1C

POSITIVE_SHADER = r"""
#version 450
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) readonly buffer In { uint src[]; };
layout(std430, set = 0, binding = 1) writeonly buffer Out { uint dst[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    dst[i] = (src[i] ^ 0xA5A5A5A5u) + i * 0x9E3779B1u;
}
"""

NEGATIVE_SHADERS = {
    "set1": (
        r"""
#version 450
layout(local_size_x = 1) in;
layout(std430, set = 1, binding = 0) buffer B { uint x; } b;
void main() { b.x = 1u; }
""",
        "only descriptor set 0 is supported",
    ),
    "descriptor-array": (
        r"""
#version 450
layout(local_size_x = 1) in;
layout(std430, set = 0, binding = 0) buffer B { uint x; } b[2];
void main() { b[0].x = 1u; }
""",
        "descriptor arrays are not supported",
    ),
    "num-workgroups": (
        r"""
#version 450
layout(local_size_x = 1) in;
layout(std430, set = 0, binding = 0) buffer B { uint x; } b;
void main() { b.x = gl_NumWorkGroups.x; }
""",
        "gl_NumWorkGroups is not supported",
    ),
}


def run(cmd: list[str], *, expect_ok: bool = True) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if expect_ok and proc.returncode != 0:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc


def compile_glsl(glslc: str, source: pathlib.Path, spv: pathlib.Path) -> None:
    run([
        glslc,
        "-fshader-stage=compute",
        "--target-env=vulkan1.1",
        str(source),
        "-o",
        str(spv),
    ])


def compile_psbc(psbc: str, spv: pathlib.Path, sb: pathlib.Path, *, expect_ok: bool) -> subprocess.CompletedProcess[str]:
    return run([
        psbc,
        "-f",
        str(spv),
        "-o",
        str(sb),
        "-s",
        "compute",
        "-4",
    ], expect_ok=expect_ok)


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_shader_binary(path: pathlib.Path) -> tuple[list[int], int]:
    data = path.read_bytes()
    orb = data.find(ORB_MAGIC)
    if orb < 0:
        raise AssertionError("OrbShdr metadata not found")

    length_word = u32(data, orb + 8)
    code_size = (length_word >> 8) & 0xFFFFFF
    code_start = orb - code_size
    if code_start < PSSL_HEADER_SIZE + GNM_FILE_HEADER_SIZE:
        raise AssertionError("invalid GCN code range")
    if code_size % 4:
        raise AssertionError(f"GCN code size is not dword aligned: {code_size}")

    chunk_offset_dw = data[orb + 12]
    num_slots = data[orb + 13]
    slots_start = orb - chunk_offset_dw * 4

    resource_slots: list[tuple[int, int, int, int]] = []
    for index in range(num_slots):
        off = slots_start + index * 4
        if off + 4 > len(data):
            raise AssertionError("input usage slot lies outside shader binary")
        usage, api_slot, startregister, flags = data[off:off + 4]
        if usage == PTR_INDIRECTRESOURCETABLE:
            resource_slots.append((usage, api_slot, startregister, flags))

    if len(resource_slots) != 1:
        raise AssertionError(
            f"expected exactly one PTR_INDIRECTRESOURCETABLE slot, got {len(resource_slots)}"
        )

    _, api_slot, startregister, flags = resource_slots[0]
    if api_slot != 0:
        raise AssertionError(f"resource table api_slot must be 0, got {api_slot}")
    if flags != 0:
        raise AssertionError(
            "resource-table pointer slot must leave registercount/resource/chunk fields zero, "
            f"got 0x{flags:02x}"
        )

    words = list(struct.unpack_from(f"<{code_size // 4}I", data, code_start))
    return words, startregister


def decode_smrd_dwordx4(words: list[int]) -> list[dict[str, int]]:
    loads: list[dict[str, int]] = []
    for index, word in enumerate(words):
        if (word >> 27) != 0b11000:
            continue
        op = (word >> 22) & 0x1F
        if op != 2:  # s_load_dwordx4
            continue
        loads.append({
            "word_index": index,
            "sdst": (word >> 15) & 0x7F,
            "sbase": ((word >> 9) & 0x3F) * 2,
            "imm": (word >> 8) & 1,
            "offset_dw": word & 0xFF,
        })
    return loads


def decode_mubuf_srsrc(words: list[int]) -> list[dict[str, int]]:
    result: list[dict[str, int]] = []
    for index in range(len(words) - 1):
        word0 = words[index]
        if (word0 >> 26) != 0b111000:
            continue
        word1 = words[index + 1]
        result.append({
            "word_index": index,
            "op": (word0 >> 18) & 0x7F,
            "srsrc": ((word1 >> 16) & 0x1F) * 4,
        })
    return result


def verify_positive(sb: pathlib.Path) -> None:
    words, table_sgpr = parse_shader_binary(sb)
    loads = decode_smrd_dwordx4(words)
    table_loads = [
        load for load in loads
        if load["sbase"] == table_sgpr and load["imm"] == 1
    ]

    offsets = {load["offset_dw"] for load in table_loads}
    missing = {0, 4} - offsets
    if missing:
        raise AssertionError(
            f"missing s_load_dwordx4 table offsets {sorted(missing)}; "
            f"decoded loads={table_loads}"
        )

    loaded_srsrc = {load["sdst"] for load in table_loads}
    mubufs = decode_mubuf_srsrc(words)
    if len(mubufs) < 2:
        raise AssertionError(f"expected at least two MUBUF instructions, got {mubufs}")

    bad = [inst for inst in mubufs if inst["srsrc"] not in loaded_srsrc]
    if bad:
        raise AssertionError(
            "MUBUF srsrc is not produced by an s_load_dwordx4 from the "
            f"resource-table pointer: {bad}; loads={table_loads}"
        )

    used_offsets = {
        load["offset_dw"]
        for load in table_loads
        if load["sdst"] in {inst["srsrc"] for inst in mubufs}
    }
    if not {0, 4}.issubset(used_offsets):
        raise AssertionError(
            "both binding 0 (offset 0 dword) and binding 1 (offset 4 dwords) "
            f"must feed MUBUF srsrc; used offsets={sorted(used_offsets)}"
        )

    print(
        "PASS positive ABI: "
        f"table=s[{table_sgpr}:{table_sgpr + 1}] "
        f"loads={[(x['offset_dw'], x['sdst']) for x in table_loads]} "
        f"mubuf_srsrc={[x['srsrc'] for x in mubufs]}"
    )


def verify_negative(psbc: str, glslc: str, directory: pathlib.Path) -> None:
    for name, (source_text, expected) in NEGATIVE_SHADERS.items():
        source = directory / f"{name}.comp"
        spv = directory / f"{name}.spv"
        sb = directory / f"{name}.sb"
        source.write_text(source_text)
        compile_glsl(glslc, source, spv)
        proc = compile_psbc(psbc, spv, sb, expect_ok=False)
        if proc.returncode == 0:
            raise AssertionError(f"{name}: psbc unexpectedly succeeded")
        if expected not in proc.stderr:
            raise AssertionError(
                f"{name}: expected stderr to contain {expected!r}\n"
                f"actual stderr:\n{proc.stderr}"
            )
        print(f"PASS negative ABI: {name}: {expected}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--psbc",
        default=".deps/opengnm-psbc/opengnm-psbc",
        help="path to patched opengnm-psbc",
    )
    parser.add_argument("--glslc", default="glslc")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="psbc-resource-abi-") as tmp:
        directory = pathlib.Path(tmp)
        source = directory / "roundtrip.comp"
        spv = directory / "roundtrip.spv"
        sb = directory / "roundtrip.sb"
        source.write_text(POSITIVE_SHADER)
        compile_glsl(args.glslc, source, spv)
        compile_psbc(args.psbc, spv, sb, expect_ok=True)
        verify_positive(sb)
        verify_negative(args.psbc, args.glslc, directory)

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
