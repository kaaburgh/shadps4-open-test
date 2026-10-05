# opengnm-psbc resource ABI

This repository applies a small patch set to the pinned
`PS4-OpenGNM/opengnm-psbc` revision from `deps.lock`. The patch gives the
standalone compiler an explicit resource ABI for ordinary UBO and SSBO
buffers. It exists so synthetic PS4 tests can exercise buffer memory without
silently compiling resource accesses against a null V#.

This is a local compatibility contract for `shadps4-open-test`, not an
upstream Sony SDK ABI document.

## Scope

The ABI currently supports:

- descriptor set 0 only;
- `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` and
  `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`;
- one descriptor per binding;
- all shader stages accepted by the standalone compiler, subject to the
  unsupported-feature checks below.

The primary consumer is the compute-buffer test path.

## Set 0 resource table

Set 0 is one contiguous table of 16-byte GNM buffer descriptors (V#).

For binding `b`:

```text
descriptor_address = table_address + 16 * b
```

Each entry is four DWORDs and has the ordinary `GnmBuffer` / V# layout.
Binding-number gaps are allowed; callers may leave unused 16-byte entries
between live bindings.

For example:

```text
table + 0x00   binding 0 V#
table + 0x10   binding 1 V#
table + 0x20   binding 2 V#
...
```

The patched standalone compiler creates a synthetic
`radv_descriptor_set_layout` with `offset = 16 * binding`, `size = 16`,
and `array_size = 1` for each used UBO/SSBO binding. That layout is supplied
to both shader-info analysis and descriptor lowering through
`radv_shader_stage::layout`.

## Table pointer user data

A shader that uses set 0 emits one
`GNM_SHINPUTUSAGE_PTR_INDIRECTRESOURCETABLE` input-usage slot.

- `apislot = 0`;
- `startregister` is the first user SGPR containing the table pointer;
- the pointer occupies two consecutive SGPRs, low DWORD followed by high
  DWORD;
- the guest binds it with
  `sceGnmDrawCmdSetPointerUserData(cmd, stage, startregister, table)`.

OpenGNM's `sceGnmDrawCmdSetPointerUserData` writes
`sizeof(void *) / sizeof(uint32_t)` DWORDs. On the PS4 target this is two
DWORDs, matching this ABI.

The descriptor-table pointer is deliberately not reconstructed from
`compiler_info.hw.address32_hi`. Descriptor lowering takes the high 32 bits
from the second user SGPR. This permits tables in normal PS4 direct-memory
mappings above 4 GiB.

The expected GFX7 shape for a buffer binding is therefore:

```text
s_load_dwordx4  <vsharp_sgprs>, s[table_lo:table_hi], binding * 4
...
buffer_*        ..., <vsharp_sgprs>, ...
```

The SMRD offset is in DWORDs, hence binding 0 uses offset 0 and binding 1
uses offset 4.

## Input-usage slot flag byte

For `PTR_INDIRECTRESOURCETABLE`, the patch emits
`registercount = 0`, `resourcetype = 0`, and `chunkmask = 0`.

Publicly available GNM-compatible structures describe `registercount` as
selecting 4-DWORD versus 8-DWORD widths for resource usages, while noting
that other usage types define their own sizes. No public authoritative Sony
specification was used here. The resource-table entry is a 4-DWORD V#, but
the input-usage item itself denotes a pointer; its actual user-data width in
this ABI is the two DWORDs written by `sceGnmDrawCmdSetPointerUserData`.
Keeping the flag byte zero avoids assigning unrelated resource-width or
internal chunk semantics to the pointer slot.

## Unsupported features

The standalone compiler fails explicitly instead of lowering to a null or
otherwise incomplete descriptor ABI for:

- descriptor sets other than set 0;
- descriptor arrays;
- images, samplers, and texel buffers;
- push constants;
- dynamic-buffer descriptor machinery;
- `gl_NumWorkGroups` / the GFX7 grid-size pointer user data;
- shaders that require scratch memory.

These checks are intentionally conservative. A feature should be added only
when its guest-visible user-data contract is represented in the shader
binary and can be verified independently.

## Resource-free shaders

A shader with no supported or unsupported resource ABI inputs does not
create a synthetic descriptor layout and does not enable the two-SGPR
descriptor-pointer path. This is intentional so existing resource-free
shaders retain their previous code-generation path.

`tooling/verify-psbc-resource-abi.py` checks the positive SSBO case and
negative unsupported cases without an emulator. It can also compare the
resource-free `gpu_solid_rt` shader binaries against an unpatched baseline
compiler when `--baseline-psbc` is supplied.

## Known limitation: VS vertex-buffer table

This patch does not change RADV/OpenGNM's vertex-input path. The
`vertex_buffers` pointer used by VS input lowering remains a one-SGPR
low-address argument whose high 32 bits come from `address32_hi` (zero in
the current standalone PS4 compiler setup).

Consequently this ABI makes the set-0 UBO/SSBO resource table fully
64-bit-addressable, but it does not make the VS vertex-buffer descriptor
table above 4 GiB safe. That is a separate problem and is intentionally out
of scope for this patch.
