#!/usr/bin/env python3
"""Verify the patched standalone opengnm-psbc buffer-resource ABI on GFX7."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

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
layout(std430, set = 1, binding = 0) buffer Out { uint dst[]; };
void main() { dst[0] = 1u; }
""",
        "descriptor set 0 only",
    ),
    "descriptor-array": (
        r"""
#version 450
layout(local_size_x = 1) in;
layout(std430, set = 0, binding = 0) buffer Out { uint value; } outbuf[2];
void main() { outbuf[0].value = 1u; }
""",
        "descriptor arrays",
    ),
    "num-workgroups": (
        r"""
#version 450
layout(local_size_x = 1) in;
layout(std430, set = 0, binding = 0) buffer Out { uvec3 value; } outbuf;
void main() { outbuf.value = gl_NumWorkGroups; }
""",
        "gl_NumWorkGroups",
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


def compile_spirv(
    glslc: str,
    source: pathlib.Path,
    output: pathlib.Path,
    stage: str,
    *,
    vulkan11: bool = True,
) -> None:
    cmd = [glslc, f"-fshader-stage={stage}"]
    if vulkan11:
        cmd.append("--target-env=vulkan1.1")
    cmd.extend([str(source), "-o", str(output)])
    run(cmd)


def compile_psbc(psbc: str, spirv: pathlib.Path, output: pathlib.Path, stage: str) -> subprocess.CompletedProcess[str]:
    return run(
        [psbc, "-f", str(spirv), "-o", str(output), "-s", stage, "-4"],
        expect_ok=False,
    )


def parse_compute_sb(path: pathlib.Path) -> tuple[int, list[int]]:
    data = path.read_bytes()
    gnm = data.find(b"Shdr")
    if gnm < 0:
        raise RuntimeError("GNM Shdr header not found")
    if gnm + 16 + 0x24 > len(data):
        raise RuntimeError("truncated GNM compute shader")

    shader_type = data[gnm + 8]
    header_dwords = data[gnm + 9]
    if shader_type != 4:
        raise RuntimeError(f"expected compute shader type 4, got {shader_type}")

    cs = gnm + 16
    common = struct.unpack_from("<I", data, cs)[0]
    shader_size = common & 0x7FFFFF
    num_slots = common >> 24
    if shader_size < 0x1C:
        raise RuntimeError(f"invalid shader_size {shader_size}")

    slot_start = cs + 0x24
    slots: list[tuple[int, int, int, int]] = []
    for i in range(num_slots):
        off = slot_start + i * 4
        if off + 4 > len(data):
            raise RuntimeError("truncated input usage slot table")
        slots.append(tuple(data[off : off + 4]))

    descriptor_slots = [s for s in slots if s[0] == 0x1C]
    if len(descriptor_slots) != 1:
        raise RuntimeError(
            f"expected one PTR_INDIRECTRESOURCETABLE slot, got {len(descriptor_slots)}"
        )
    usage, api_slot, start_register, flags = descriptor_slots[0]
    del usage
    if api_slot != 0:
        raise RuntimeError(f"descriptor table api slot is {api_slot}, expected 0")
    if flags != 0:
        raise RuntimeError(f"descriptor table flag byte is 0x{flags:02x}, expected 0")

    code_start = gnm + 16 + header_dwords * 4
    code_size = shader_size - 0x1C  # GnmShaderBinaryInfo follows the ISA.
    code_end = code_start + code_size
    if code_end + 7 > len(data):
        raise RuntimeError("shader code extends beyond file")
    if data[code_end : code_end + 7] != b"OrbShdr":
        raise RuntimeError("OrbShdr footer is not at the computed code boundary")
    if code_size % 4:
        raise RuntimeError("GCN code size is not DWORD-aligned")

    words = list(struct.unpack_from(f"<{code_size // 4}I", data, code_start))
    return start_register, words


def decode_sop1_constant_move(word: int) -> tuple[int, int] | None:
    # Sea Islands / GCN 1.1 SOP1 encoding: [31:23]=101111101,
    # sdst[22:16], op[15:8], ssrc0[7:0]. On GCN 1.0/1.1,
    # s_mov_b32 is op 3 and s_mov_b64 is op 4. Inline scalar constants
    # use source encodings >= 128 (255 is a literal constant).
    if (word >> 23) != 0b101111101:
        return None
    op = (word >> 8) & 0xFF
    if op not in {3, 4}:
        return None
    ssrc0 = word & 0xFF
    if ssrc0 < 128:
        return None
    sdst = (word >> 16) & 0x7F
    width = 1 if op == 3 else 2
    return sdst, width


def decode_and_verify(words: list[int], table_sgpr: int) -> None:
    # Sea Islands SMRD encoding requested by the ABI contract:
    # [31:27]=11000, op[26:22], sdst[21:15], sbase[14:9]*2,
    # imm[8], offset[7:0] (DWORDs).
    descriptor_loads: dict[int, int] = {}
    for pc, word in enumerate(words):
        if (word >> 27) != 0b11000:
            continue
        op = (word >> 22) & 0x1F
        if op != 2:  # s_load_dwordx4
            continue
        sdst = (word >> 15) & 0x7F
        sbase = ((word >> 9) & 0x3F) * 2
        imm = (word >> 8) & 1
        offset = word & 0xFF
        if sbase == table_sgpr:
            if imm != 1:
                raise RuntimeError(
                    f"descriptor load at DWORD {pc} uses a register offset; expected immediate"
                )
            descriptor_loads[sdst] = offset

    offsets = set(descriptor_loads.values())
    if offsets != {0, 4}:
        raise RuntimeError(
            f"expected set-0 V# loads at DWORD offsets {{0, 4}}, got {sorted(offsets)}"
        )

    # MUBUF: dword0 [31:26]=111000, op[24:18]; dword1 srsrc[20:16]*4.
    # The positive shader uses GFX7 buffer_load_dword (0x0c) and
    # buffer_store_dword (0x1c). Restricting the scan to those opcodes avoids
    # treating literal/extension DWORDs from unrelated 64-bit instructions as
    # MUBUF headers.
    #
    # Also reject the original null-V# failure mode explicitly: an earlier
    # table load is not sufficient if a later s_mov_b32/b64 constant clobbers
    # any DWORD in the four-SGPR SRSRC before MUBUF consumes it.
    constant_moves: list[tuple[int, int, int]] = []
    for pc, word in enumerate(words):
        move = decode_sop1_constant_move(word)
        if move is not None:
            sdst, width = move
            constant_moves.append((pc, sdst, width))

    table_load_pc = {load["sdst"]: load["word_index"] for load in table_loads}
    mubuf_count = 0
    used_offsets: set[int] = set()
    for pc in range(len(words) - 1):
        word0 = words[pc]
        if (word0 >> 26) != 0b111000:
            continue
        op = (word0 >> 18) & 0x7F
        if op not in {0x0C, 0x1C}:
            continue
        word1 = words[pc + 1]
        srsrc = ((word1 >> 16) & 0x1F) * 4
        mubuf_count += 1
        if srsrc not in descriptor_loads:
            raise RuntimeError(
                f"MUBUF at DWORD {pc} uses s[{srsrc}:{srsrc + 3}], "
                "which is not produced by a set-0 s_load_dwordx4"
            )

        load_pc = table_load_pc[srsrc]
        for move_pc, move_dst, move_width in constant_moves:
            if not (load_pc < move_pc < pc):
                continue
            move_end = move_dst + move_width - 1
            if move_dst <= srsrc + 3 and move_end >= srsrc:
                raise RuntimeError(
                    f"MUBUF at DWORD {pc} uses s[{srsrc}:{srsrc + 3}], "
                    f"but a constant s_mov clobbers s[{move_dst}:{move_end}] "
                    f"after its table load at DWORD {load_pc}"
                )

        used_offsets.add(descriptor_loads[srsrc])

    if mubuf_count == 0:
        raise RuntimeError("no MUBUF instructions found in positive shader")
    if used_offsets != {0, 4}:
        raise RuntimeError(
            f"MUBUF instructions did not consume both bindings; used offsets {sorted(used_offsets)}"
        )


def verify_positive(psbc: str, glslc: str, tmp: pathlib.Path) -> None:
    source = tmp / "resource-abi.comp"
    spirv = tmp / "resource-abi.spv"
    sb = tmp / "resource-abi.sb"
    source.write_text(POSITIVE_SHADER)
    compile_spirv(glslc, source, spirv, "compute")
    proc = compile_psbc(psbc, spirv, sb, "compute")
    if proc.returncode != 0:
        raise RuntimeError(
            f"positive psbc compile failed ({proc.returncode})\n{proc.stderr}"
        )
    table_sgpr, words = parse_compute_sb(sb)
    decode_and_verify(words, table_sgpr)
    print(
        f"PASS positive: table pointer s[{table_sgpr}:{table_sgpr + 1}], "
        "V# offsets 0/4 DWORD, all MUBUF resources loaded through SMRD"
    )


def verify_negative(psbc: str, glslc: str, tmp: pathlib.Path) -> None:
    for name, (shader, expected) in NEGATIVE_SHADERS.items():
        source = tmp / f"{name}.comp"
        spirv = tmp / f"{name}.spv"
        sb = tmp / f"{name}.sb"
        source.write_text(shader)
        compile_spirv(glslc, source, spirv, "compute")
        proc = compile_psbc(psbc, spirv, sb, "compute")
        combined = proc.stdout + proc.stderr
        if proc.returncode == 0:
            raise RuntimeError(f"negative case {name} unexpectedly compiled")
        if expected not in combined:
            raise RuntimeError(
                f"negative case {name} failed without expected diagnostic {expected!r}\n"
                f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
            )
        print(f"PASS negative {name}: {expected}")


def compile_resource_free(
    psbc: str,
    glslc: str,
    source: pathlib.Path,
    stage: str,
    tmp: pathlib.Path,
    tag: str,
) -> bytes:
    spirv = tmp / f"{tag}.{stage}.spv"
    sb = tmp / f"{tag}.{stage}.sb"
    # Match tests/gpu_solid_rt/Makefile exactly: its glslc rules do not pass
    # an explicit --target-env.
    compile_spirv(glslc, source, spirv, stage, vulkan11=False)
    proc = compile_psbc(psbc, spirv, sb, stage)
    if proc.returncode != 0:
        raise RuntimeError(f"{tag} {stage} compile failed\n{proc.stderr}")
    return sb.read_bytes()


def verify_resource_free_identity(
    patched_psbc: str, baseline_psbc: str, glslc: str, tmp: pathlib.Path
) -> None:
    shaders = [
        (ROOT / "tests/gpu_solid_rt/assets/fullscreen.vert.glsl", "vertex"),
        (ROOT / "tests/gpu_solid_rt/assets/solid.frag.glsl", "fragment"),
    ]
    for source, stage in shaders:
        patched = compile_resource_free(
            patched_psbc, glslc, source, stage, tmp, f"patched-{source.stem}"
        )
        baseline = compile_resource_free(
            baseline_psbc, glslc, source, stage, tmp, f"baseline-{source.stem}"
        )
        if patched != baseline:
            raise RuntimeError(
                f"resource-free output changed for {source.relative_to(ROOT)}"
            )
        print(f"PASS byte identity: {source.relative_to(ROOT)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--psbc",
        default=str(ROOT / ".deps/opengnm-psbc/opengnm-psbc"),
        help="patched opengnm-psbc executable",
    )
    parser.add_argument("--glslc", default="glslc")
    parser.add_argument(
        "--baseline-psbc",
        help="optional unpatched psbc executable for byte-identical resource-free comparison",
    )
    args = parser.parse_args()

    for command in [args.psbc, args.glslc]:
        resolved = shutil.which(command) if "/" not in command else command
        if not resolved or not pathlib.Path(resolved).exists():
            raise RuntimeError(f"required executable not found: {command}")

    if args.baseline_psbc and not pathlib.Path(args.baseline_psbc).exists():
        raise RuntimeError(f"baseline psbc not found: {args.baseline_psbc}")

    with tempfile.TemporaryDirectory(prefix="psbc-resource-abi-") as td:
        tmp = pathlib.Path(td)
        verify_positive(args.psbc, args.glslc, tmp)
        verify_negative(args.psbc, args.glslc, tmp)
        if args.baseline_psbc:
            verify_resource_free_identity(
                args.psbc, args.baseline_psbc, args.glslc, tmp
            )

    print("PASS psbc resource ABI")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
