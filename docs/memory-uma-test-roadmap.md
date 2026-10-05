# Memory / UMA test roadmap

## Goal

Build a sequence of small, freely redistributable PS4 guest workloads that make CPU/GPU
memory interaction progressively harder while keeping each result deterministic and easy to
attribute.

The long-term target is not a generic memory benchmark. It is a correctness and profiling suite
for the gap between the PS4's unified-memory programming model and shadPS4's host-side memory
implementation, including future UMA/shared-backing paths.

The suite should help answer two separate questions:

1. **Correctness:** after a defined sequence of CPU and GPU accesses, which bytes must be visible
   to each side?
2. **Cost:** how much tracking, protection, upload, download, synchronization and staging work did
   shadPS4 need to preserve those bytes?

A test should establish correctness first. Performance/telemetry is secondary and must not change
the guest-visible oracle.

## Design rules

- Keep every workload small and independent. Add one new memory interaction at a time.
- Emit one machine-readable terminal result:
  `SHADTEST name=<test> status=PASS|FAIL ...`.
- Prefer exact integer patterns/checksums over visual inspection or floating-point tolerance.
- Use explicit guest-visible completion/order where the result depends on GPU completion.
- Do **not** use deliberately undefined CPU/GPU races as correctness tests. CPU and GPU may touch
  the same backing allocation, but overlapping unsynchronized writes are not a useful oracle.
- Use control layouts next to adversarial layouts so a failure localizes to memory ownership or
  granularity rather than shader/dispatch plumbing.
- Keep the guest binary/backend-neutral where possible. Optional shadPS4 instrumentation may add
  host-side counters, but must not be required to decide PASS/FAIL.
- Do not build a generalized test DSL or large framework until multiple implemented tests prove
  that the duplication is real.
- When a commercial game exposes a memory bug, the preferred permanent artifact is a minimized
  open workload reproducing the memory interaction, not a dependency on the game.

## Why this maps well to current shadPS4

As of shadPS4 `10393d2` (2026-10-05), the relevant host-side path already exposes clear
boundaries for synthetic testing:

- `PageManager` tracks CPU accesses at 4 KiB page granularity and can use userfaultfd or signal
  faults on Linux;
- `RegionManager` / `MemoryTracker` maintain CPU-modified and GPU-modified page state;
- `BufferCache` uploads CPU-modified ranges to Vulkan buffers and downloads GPU-modified ranges
  before CPU access;
- GPU-to-CPU reads can therefore exercise page protection, fault handling, dirty-state
  transitions, staging copies and synchronization;
- image/render-target state has a separate `TextureCache` path, so buffer-only tests should be
  established before image/buffer aliasing is introduced.

This roadmap intentionally starts with ordinary buffers. They isolate the memory machinery more
cleanly than `gpu_solid_rt`, whose oracle also depends on render-target and image-readback logic.

## Progression

| Stage | Test | Memory interaction added | Primary purpose |
|---|---|---|---|
| 0 | `gpu_solid_rt` | GPU render target -> CPU readback | Existing end-to-end baseline; proves guest GPU work and machine-readable readback oracle |
| 1 | `buffer_compute_roundtrip` | CPU buffer -> compute -> GPU-written buffer -> CPU | Establish the buffer/compute path without TextureCache |
| 2 | `buffer_cpu_rewrite` | CPU rewrites an already GPU-used buffer, then GPU consumes it again | CPU-dirty invalidation and repeated upload correctness |
| 3 | `buffer_page_false_sharing` | GPU and CPU modify disjoint ranges of the same tracked page | Detect loss of newer data caused by page-granularity ownership/authority decisions |
| 4 | `buffer_pingpong` | Repeated ordered CPU<->GPU ownership transitions | Turn one transition into a stable correctness stressor and optimization workload |
| 5 | `buffer_granularity_matrix` | Same operation at selected offsets/sizes/boundaries | Characterize amplification and boundary-sensitive bugs without changing semantics |
| 6 | `physical_alias` | Two guest virtual mappings refer to the same physical backing | Verify alias coherence and backing identity |
| 7 | `ordered_async_handoff` | CPU/GPU handoff using only guest-visible completion/order | Separate correct ordering from accidental host-global synchronization |
| 8 | `image_buffer_alias` | The same backing bytes move between image/RT and buffer usage | Exercise TextureCache <-> BufferCache ownership and alias transitions |
| 9 | `map_unmap_reuse` | Map/use/unmap/remap/reuse of backing memory | Detect stale tracking state, stale Vulkan resources and lifetime bugs |
| 10 | `uma_churn` | Many pages, sparse mixed accesses, many generations | Integration/stress workload for mirror vs UMA implementations |

Stages are ordered by diagnostic value, not by ambition. A later test should not be implemented
until the lower-level mechanism it depends on has a small passing test.

## Stage 0: `gpu_solid_rt` — existing baseline

Status: implemented.

This remains useful as an image/render-target baseline, but it should not become the foundation
for the memory suite. Its GPU -> CPU oracle deliberately includes linear render-target readback
and therefore exercises TextureCache/image behavior in addition to generic CPU/GPU visibility.

## Stage 1: `buffer_compute_roundtrip` — next test

### Purpose

Create the smallest deterministic workload that exercises shadPS4's ordinary buffer path in both
directions:

```text
CPU guest memory
    |
    | input upload / visibility
    v
compute shader
    |
    | GPU writes
    v
output buffer
    |
    | GPU -> CPU visibility
    v
CPU verifier
```

A failure here must be explainable without considering image layout, rasterization, physical
aliasing, partial-page ownership or asynchronous multi-queue behavior.

### Proposed layout

Use direct memory with page-aligned regions:

```text
guard
input      64 KiB
guard
output     64 KiB
guard
completion/status
```

The first implementation should use one fixed size. Do not add a size/offset matrix yet.

### Sequence

1. CPU fills `input[i]` with a deterministic integer pattern.
2. CPU fills `output` and guards with poison values.
3. Bind input/output as ordinary GNM buffer resources to a compute shader.
4. Dispatch enough workgroups to cover the buffer.
5. Each invocation computes an exact integer transform, for example conceptually:
   `out[i] = (in[i] ^ CONST_A) + i * CONST_B`.
6. Emit an explicit GPU completion marker/event.
7. CPU verifies every output element.
8. CPU verifies input and guard regions were not modified.
9. Emit PASS/FAIL with the first mismatching index and expected/actual values.

The exact constants are unimportant; they should simply produce a nontrivial, reproducible
32-bit pattern without floating point.

### What this establishes

- compute shader compilation/loading through the existing open toolchain;
- CS resource binding and direct dispatch;
- CPU-written guest bytes becoming visible to a GPU buffer read;
- GPU buffer writes becoming visible to guest CPU reads;
- a reusable buffer-oriented oracle independent of TextureCache.

### Negative control

At least one development-time negative control should prove the oracle can fail, for example a
shader variant that intentionally leaves the output poisoned or writes a different transform.
It does not need to become a permanent runtime mode if retaining it adds complexity.

### Deliberate non-goals for Stage 1

Do not add:

- random access patterns;
- atomics;
- multiple queues;
- image resources;
- physical aliases;
- CPU/GPU writes to the same page;
- timing-sensitive polling beyond the normal completion mechanism;
- a generalized parameterized benchmark framework.

Those belong to later stages.

## Stage 2: `buffer_cpu_rewrite`

### Sequence

1. CPU writes generation A into a buffer.
2. GPU reads A and writes a verified result.
3. After GPU completion, CPU modifies a small deterministic subset to generation B.
4. GPU consumes the same buffer again.
5. CPU verifies that the second result reflects B exactly.

Start with whole-page-separated updates. Partial-page adversarial layouts belong to Stage 3.

### Purpose

The first GPU use can create host-side cached/mirrored state. This test checks that a later CPU
write invalidates or updates that state before the next GPU access instead of leaving the GPU
with stale generation A.

## Stage 3: `buffer_page_false_sharing`

This is the first test aimed directly at the class of bugs that makes PS4-style unified memory
difficult to reproduce efficiently on a discrete-memory host.

### Core invariant

CPU and GPU modify **different bytes**. There is no data race.

For one 4 KiB tracked page:

```text
page
+------------------------------+
| region A: GPU-owned update   |
|                              |
| region B: CPU-owned update   |
+------------------------------+
```

Sequence:

1. CPU initializes the complete page.
2. GPU writes generation G into region A.
3. Wait for explicit GPU completion.
4. CPU writes generation C into disjoint region B and does not touch A.
5. GPU reads both A and B and writes a compact verdict/result buffer.
6. CPU verifies that A still contains G and B contains C.

A mirror implementation must not restore an older CPU copy of A while processing the CPU write
to B.

### Required controls

Implement the same semantic operation with at least:

- A and B on clearly separate pages — control;
- A and B on the same 4 KiB tracked page — adversarial case;
- a layout around a 4 KiB boundary — boundary case.

Only after those are stable should additional implementation-dependent boundaries be added.

## Stage 4: `buffer_pingpong`

Repeat a deterministic ordered handoff for many generations:

```text
CPU writes part B(n)
GPU validates A(n-1), B(n) and writes A(n)
completion
CPU validates A(n)
repeat
```

The final state can be predicted from the generation number, so every iteration remains a
correctness check rather than a timing benchmark.

This test is useful for both correctness and optimization because repeated ownership transitions
magnify unnecessary uploads, downloads, page faults and global synchronization.

## Stage 5: `buffer_granularity_matrix`

Parameterize the already-proven ping-pong/false-sharing operation across selected:

- allocation sizes;
- access sizes;
- alignments;
- offsets around 4 KiB page boundaries;
- sparse vs contiguous modified ranges.

Do not encode shadPS4's current internal cache/block sizes into the guest contract unless a
specific implementation bug needs that reproducer. Host instrumentation can still correlate
results with current internal boundaries.

The purpose is to locate cliffs and amplification, not to create a synthetic bandwidth suite.

## Stage 6: `physical_alias`

Map the same physical/direct-memory backing at two guest virtual addresses when the available
open APIs/toolchain permit it.

Test both directions:

- CPU writes through alias A, GPU reads alias B;
- GPU writes through alias B, CPU reads alias A.

Then combine aliasing with ordered partial writes.

This is important for a true shared-backing/UMA implementation: tracking only guest virtual
ranges must not accidentally create two independent authorities for one physical backing.

If the required mapping API cannot be exercised cleanly with the current open stack, defer this
stage rather than adding a large compatibility layer just for the test.

## Stage 7: `ordered_async_handoff`

Earlier tests may use conservative completion to keep diagnosis simple. This stage removes
accidental host-global synchronization from the contract.

Use only the guest-visible completion primitive/order that a PS4 program is entitled to rely on:

```text
CPU generation N
    ->
GPU consumes N / produces N+1
    ->
guest-visible completion
    ->
CPU consumes N+1
```

Correctness must not depend on an unrelated host `scheduler.Finish()` or queue-idle operation.

This stage should be paired with optional host telemetry so a future UMA mode can demonstrate
that it preserves visibility without falling back to a full GPU stall/readback.

## Stage 8: `image_buffer_alias`

Only after buffer ownership is well understood, use the same backing bytes as different GPU
resource classes across ordered phases, for example:

1. image/render-target write;
2. completion;
3. buffer/texel-buffer read;
4. CPU verification;

and the reverse direction.

This targets transitions between TextureCache and BufferCache, which are deliberately excluded
from the earlier buffer tests.

## Stage 9: `map_unmap_reuse`

Exercise memory lifetime rather than just content:

1. allocate/map;
2. CPU/GPU use;
3. unmap/release;
4. remap or reuse the backing/address range;
5. write a new generation;
6. verify that no old GPU/CPU authority, page watcher or cached resource leaks into the new
   lifetime.

Keep this test deterministic and short. Long reload loops belong to Stage 10.

## Stage 10: `uma_churn`

Build an integration workload from operations already validated independently:

- a moderately large direct-memory pool;
- many 4 KiB pages;
- deterministic sparse CPU writes;
- deterministic sparse GPU writes;
- repeated generations;
- mixed same-page and separate-page ownership;
- periodic alias/lifetime transitions only if those earlier stages exist.

The workload should output a final digest plus enough failure metadata to identify the first bad
generation/page/range.

This is the stage intended for sustained comparison of the current mirror model with a future
UMA/shared-backing implementation.

## Optional host-side telemetry

The same guest ELF should remain usable without instrumentation. A development build of shadPS4
may additionally report counters such as:

- CPU write faults / CPU read faults;
- pages protected/unprotected;
- upload bytes and upload ranges;
- download bytes and download ranges;
- staging bytes;
- number of forced scheduler finishes / queue waits;
- GPU-modified and CPU-modified state transitions.

Useful comparison dimensions are:

```text
correctness result
+
faults / transitions
+
bytes uploaded
+
bytes downloaded
+
synchronization events
```

This allows the suite to demonstrate not only that a UMA path is correct, but that it actually
removes work that the mirror path needs.

Telemetry must never turn an implementation-specific counter value into the guest PASS/FAIL
criterion.

## Backend / validation matrix

Use different hosts for different purposes:

- **lavapipe:** cheap deterministic bring-up and command/shader plumbing; not a performance or
  physical-memory model;
- **discrete AMD/NVIDIA:** primary stress target for the current mirror/copy model;
- **integrated/host-coherent Vulkan implementations:** useful comparison point for future shared
  backing;
- **future shadPS4 UMA mode:** same guest workloads, same semantic oracle;
- **real PS4:** strongest reference when available, especially before declaring subtle cache/
  alias semantics to be hardware-faithful.

A test does not need every backend before landing. The minimum validation should match the new
mechanism introduced by that stage.

## Recommended implementation order

The next three implementation PRs should be:

1. **`buffer_compute_roundtrip`** — establish compute + ordinary buffer CPU->GPU->CPU plumbing.
2. **`buffer_page_false_sharing`** — directly exercise page-granularity mixed CPU/GPU
   authority with deterministic disjoint writes.
3. **`buffer_pingpong`** — repeat that ownership transition and add optional telemetry for
   mirror-vs-UMA work amplification.

`buffer_cpu_rewrite` is logically Stage 2 and should either be implemented as a small standalone
test between 1 and 3, or included as a very small control mode while implementing Stage 3 if doing
so is materially cheaper. Do not let that choice delay the page-false-sharing test.

The immediate next test is therefore **`buffer_compute_roundtrip`**.
