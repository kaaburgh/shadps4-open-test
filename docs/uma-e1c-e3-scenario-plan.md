# E1C and E3-gap scenario plan

Status: **plan only**. No test code has been written for any scenario below.

This plan extends the memory/UMA test roadmap (kaaburgh/shadps4-open-test#2, stages 0–10). It
adds guest workloads for the two areas the current suite does not reach:

- **A — E1C**: GDS and packet-specific visibility. The command processor (CP) reads or writes
  guest memory for these packets at a time that may not match GPU order.
- **B — E3 gaps**: paths in shared backing that the current suite never reaches. They involve
  fallback to the mirror, mixed ownership, the image-source guard and cross-queue waits.

E2 (baseline performance) is a measurement stage and is not part of this plan. See [Out of
scope](#out-of-scope).

## Context

The UMA research order is E0b → E0 → E1 → E2 → E3 → E4 → E5 → E6.

- **E1B** publishes guest completion (EOP/EOS/ReleaseMem labels) only after the host GPU has
  finished. It is done. The suite checks it indirectly: with shared backing and readbacks
  Disabled, a build that publishes labels at parse time fails 10/11 tests.
- **E1C** is the rest of E1 visibility: GDS and the packets that the CP executes against guest
  memory while parsing. The E1B notes end with: "Remaining work is exactly E1C … Do not start
  shared backing until its prerequisites are established".
- **E3** (shared backing) was started ahead of E1C by owner decision (kaaburgh/shadPS4#6). The
  existing suite tests guest-visible memory semantics, which is exactly what E3 changes: with
  readbacks Disabled it goes from 1/11 to 11/11.

The E3 review then showed what skipping E1C costs. With shared backing, recorded GPU work reads
guest memory when it executes, but the CP still accesses that memory at parse time. Example:
`WRITE_DATA` is now a regression against the mirror (kaaburgh/shadPS4#11). The review also left
E3 paths without coverage (kaaburgh/shadPS4#13).

Reference points:

- shadPS4: fork branch `research/uma-e3-shared-backing` at `8d07f08` (E1B + E3 slice).
- open-test: `main` at `5693622`.

## Facts that shape the scenarios

All line references are to shadPS4 `8d07f08`.

### How shadPS4 handles the relevant packets today

| Packet / path | Handling | Where |
|---|---|---|
| `WRITE_DATA` (gfx, compute) | `memcpy` into guest memory while parsing | `liverpool.cpp:930`, `1290` |
| `COND_EXEC` | Predicate read from guest memory while parsing; gfx only (no compute case) | `liverpool.cpp:1057` |
| `EVENT_WRITE` CS/PS partial flush | Ignored. Only streamout flush and `ZPASS_DONE` are handled | `liverpool.cpp:800`, `1374` |
| `WAIT_REG_MEM` | CPU compare on guest memory; yields. With shared backing, submits newly recorded work on every poll | `liverpool.cpp:993`, `1325`; `vk_rasterizer.cpp:403` |
| `DMA_DATA` | `Rasterizer::FillBuffer` / `CopyBuffer`: CPU fast path at parse time, or a GPU fill/copy. `cp_sync`/`raw_wait` not interpreted | `liverpool.cpp:893`, `1164`; `vk_rasterizer.cpp:1179`, `1204` |
| `EVENT_WRITE_EOS` GDS store | Synchronous: `Finish`, read GDS, store into guest memory | `liverpool.cpp:826` |
| `RELEASE_MEM` GDS → memory | GPU copy GDS → destination; IRQ signalled at parse time (explicit E1C gap) | `liverpool.cpp:1336` |
| `MEM_SEMAPHORE` | Signal/consume at parse time | `liverpool.cpp:959`, `1307` |
| `DUMP_CONST_RAM` (CE) | `memcpy` into guest memory while parsing | `liverpool.cpp:344` |
| `COPY_DATA` | Not implemented (warning only) | `liverpool.cpp:949` |

### Facts about readbacks and the BDA path

- **Precise mirror reads by the CP are correct.** A CP-thread read of a GPU-modified,
  read-protected page faults. The handler then downloads inline (`page_manager.cpp:449-462`,
  `assume_locks = is_gpu_thread`).
  - So in **mirror Precise**, parse-time reads of GPU-written memory (`COND_EXEC`,
    `WAIT_REG_MEM`, sharps) usually see the right value.
  - **Mirror Disabled/Relaxed** and **shared backing** have no such protection. The CP reads
    memory before the recorded producer has executed, or, in the mirror case, never sees the
    write at all.
- **BDA is narrower than the E3 review assumed.** shadPS4 uses the BDA page table only for
  scalar `ReadConst` loads from a dynamic address, which are reads only. It enables them only
  when `directMemoryAccess` is on (`shader_info_collection_pass.cpp:156-185`), and that setting
  defaults to `false` (`emulator_settings.h:406`).
  - Shaders never *write* guest memory through BDA. This removes the premise of review finding
    F4 (the host barrier is now per submit anyway).
  - Every BDA scenario needs `directMemoryAccess=true` and a shader that produces such loads.

### Tooling limits

- **OpenGNM emitters.**
  - Available: `DMA_DATA` with `CP_SYNC` (`sceGnmDrawCmdFillMemory`, `sceGnmDrawCmdCopyMemory`),
    `WAIT_REG_MEM` (`sceGnmDrawCmdWaitMem`) and EOP.
  - Missing: `WRITE_DATA`, `COND_EXEC`, `EVENT_WRITE` partial flush, `EVENT_WRITE_EOS`,
    `RELEASE_MEM` and `MEM_SEMAPHORE`. These need raw PM4 dwords; `sceGnmCmdAllocInside` or a
    direct `cmdptr` write works.
  - The packet opcodes are defined in OpenGNM `include/pm4/sid.h`. shadPS4's parser layout is in
    `pm4_cmds.h`.
- **psbc shaders** support set-0 UBO/SSBO only. There are no images, samplers, push constants or
  GDS instructions, and `buffer_reference` is untested. GDS producers must therefore be
  `DMA_DATA`, not shaders.
- **Compute queues.** No test uses an async compute (ASC) queue yet. OpenGNM has
  `sceGnmMapComputeQueue` and `sceGnmDingDong`. `RELEASE_MEM` exists only on compute queues in
  shadPS4.
- **Run modes.** Every scenario runs in six configurations: mirror and shared
  (`SHADPS4_UMA_SHARED_BACKING=1`), each with readbacks Precise, Relaxed and Disabled. The
  runner already inherits the environment, and Relaxed/Disabled use `--use-host-config`.

## Common infrastructure (milestone M0)

This is needed before most scenarios. Keep it as small as the first two tests require.

1. **Raw PM4 builders** in `tests/common` (or a new `tests/common/pm4.h`). Each builder returns
   `false` when the command buffer has no room. Dword layouts are cross-checked against shadPS4
   `pm4_cmds.h` and OpenGNM `sid.h`. Needed builders:
   - `WRITE_DATA` (memory dst, `wr_confirm`);
   - `COND_EXEC`;
   - `EVENT_WRITE` CS partial flush;
   - `EVENT_WRITE_EOS` GDS store;
   - raw `DMA_DATA` (GDS endpoints, `raw_wait`);
   - `RELEASE_MEM` (data / GDS);
   - `MEM_SEMAPHORE`.

   Whether these later become OpenGNM API functions is a separate decision.
2. **Placement helper.**
   - Allocate direct memory at a chosen physical offset through the `sceKernelAllocateDirectMemory`
     search range.
   - Map separately allocated pieces at adjacent fixed VAs. This makes a VA range that is *not*
     physically contiguous, which forces shadPS4's shared backing into mirror fallback.
   - Allocate a range that straddles the 256 MiB chunk boundary.
3. **ASC helper** (M2 only): map a compute queue, build a ring, `DingDong`.
4. **Runner.**
   - A switch for shared-backing mode next to the readbacks modes.
   - For the BDA spike only, a way to set `directMemoryAccess=true` in the isolated config.
5. **Hang policy.**
   - Some expected failures are CP hangs, for example `WAIT_REG_MEM` on a value the host never
     sees. Such cases run last in their ELF or in their own ELF.
   - Every wait is bounded and reports `FAIL reason=timeout ...` before the runner timeout kills
     the emulator.

## Scenario catalogue

Expected results are **hypotheses from code reading at `8d07f08`**, not runs. Each test PR
records the actual results in all six configurations, as stages 6 and 8 did.

- ✓: pass.
- ✗: fail; the tracking issue is given.
- ?: cannot be predicted from the code.
- "hang": the CP waits forever, so the guest reports a timeout.

| ID | Test | Phase | Exercises | Prio | Needs | mirror P | mirror D | shared P | shared D |
|---|---|---|---|---|---|---|---|---|---|
| A1 | `write_data_order` | E1C | `WRITE_DATA` between two readers | P1 | PM4 | ✓ | ✓ | ✗ #11 | ✗ #11 |
| A2 | `wait_gpu_written` | E1C | `WAIT_REG_MEM` on a shader-written flag, same queue | P2 | — | ✓ | hang | ✓ | ✓ |
| A3 | `cond_exec_gpu_predicate` | E1C | `COND_EXEC` on a shader-written predicate | P1 | PM4 | ✓ | ✗ | ✗ #11 | ✗ #11 |
| A4 | `dma_order` | E1C/E3 | `DMA_DATA` copy/fill around dispatches; fill + wait | P1 | — (+PM4 for `raw_wait`) | ✓ | ✗ (case a), hang (case d) | ✓ | ✓ |
| A5 | `gds_store` | E1C | GDS ← `DMA_DATA`; EOS GDS store (gfx); `RELEASE_MEM` GDS store (compute) | P2 | PM4, ASC | ✓ | ✗ (compute) | ✓ / ✗ early IRQ | ✓ / ✗ early IRQ |
| A6 | `gpu_written_descriptor` | E1C | A dispatch writes a V# that the next dispatch binds | P2 | — | ✓ | ✗ | ✗ #11 | ✗ #11 |
| A7 | `mem_semaphore_handoff` | E1C | `MEM_SEMAPHORE` producer/consumer across queues | P3 | PM4, ASC | ✓ | ✓ | ✓ | ✓ |
| A8 | `ce_dump_const_ram` | E1C | CE `DUMP_CONST_RAM` vs DE readers | P3 | CCB + PM4 | ? | ? | ? | ? |
| B1 | `dma_alias_order` | E3 | DMA through a second VA of memory a dispatch writes via the first | P1 | — | ✗ #5 | ✗ #5 | ✓ (shareable alias), ✗ #8 (non-contiguous alias) | same |
| B2 | `dma_mixed_endpoints` | E3 | DMA copy shareable source → mirror-only destination | P1 | placement | ✓ | ✓ | ✓ | ✓ |
| B3 | `demotion_divergence` | E3 | Two bindings of one block in one dispatch, the second forces mirror fallback | P2 | placement | ✓ | ✓ | ✗ #7 | ✗ #7 |
| B4 | `fallback_in_shared` | E3 | Chunk-crossing and non-contiguous ranges under shared backing | P1 | placement | ✓ | ✗ | ✓ | ✗ (new) |
| B5 | `image_source_same_submit` | E3 | Compute writes, then a draw initializes its RT from those bytes in the same submit | P1 | — | ✓ | ✓ | ✓ | ✓ |
| B6 | `cross_queue_wait` | E3/E1C | GFX `WAIT_REG_MEM` on a flag an ASC dispatch writes, producer parsed later | P2 | ASC | ✓ | hang | ✓ | ✓ |
| B7 | `bda_remap` | E3 | BDA read across unmap/remap | spike | `directMemoryAccess`, shader | ? | ? | ? | ? |

Some scenarios are expected to pass everywhere today: B2, B5, A7, and A2/B6 in shared mode.
They are regression and mutation-killing tests for guards the suite does not reach now. For
example, a mutant without the B5 guard still passes 11/11 today.

---

### A1 `write_data_order`

**Purpose.** `WRITE_DATA` must take effect in CP order relative to dispatches recorded before
and after it.

**Sequence.** All steps are in one DCB.
1. The CPU sets `X = 1`.
2. Dispatch A reads `X` into `outA`.
3. `WRITE_DATA X = 2` (memory, `wr_confirm`).
4. Dispatch B reads `X` into `outB`.
5. EOP label; the CPU waits.

**Oracle.** `outA == 1`, `outB == 2`, `X == 2`.

**Controls.**
- The same with a CPU write of `X` between two separate submits.
- `X` as a small UBO (stream path) and as an SSBO larger than the stream threshold.

**Expected.**
- Mirror: ✓. A uploads `X` at record time. The `memcpy` marks the page CPU-modified, and B
  uploads again.
- Shared: A reads `X` when it executes, after the `memcpy` → `outA == 2` ✗. Tracked in
  kaaburgh/shadPS4#11.

### A2 `wait_gpu_written`

**Purpose.** A `WAIT_REG_MEM` on memory that a shader writes must complete. On GCN the CP and
shaders share L2, so no extra flush is needed.

**Sequence.**
1. Dispatch P writes `flag = 0xC0FFEE` and `payload`.
2. `WAIT_REG_MEM flag == 0xC0FFEE`.
3. Dispatch C copies `payload` to `out`.
4. EOP label.

**Oracle.** `out == payload`, and the label arrives within a bounded wait.

**Expected.**
- Mirror Precise ✓: the CP read faults and downloads.
- Mirror Disabled/Relaxed: hang. The flag exists only in the arena.
- Shared ✓: `FlushForMemoryWait` submits P.

This is the known E1 gap "general GPU-written RAW under Disabled readbacks".

### A3 `cond_exec_gpu_predicate`

**Purpose.** `COND_EXEC` must evaluate a predicate written by preceding GPU work.

**Sequence.**
1. Dispatch P writes `pred` (0 in one case, 1 in another).
2. `EVENT_WRITE` CS partial flush.
3. `COND_EXEC(pred, n)` guarding `WRITE_DATA marker = 7`, or a small dispatch.
4. EOP label.

**Oracle.** `marker == 7` exactly when `pred == 1`.

**Control.** `pred` written by the CPU before submit.

**Expected.**
- Mirror Precise ✓ through the CP-thread fault download.
- Mirror Disabled ✗.
- Shared ✗: the predicate is read before P executes.

The fix belongs in E1C (kaaburgh/shadPS4#11). The CS partial flush is also ignored today.

### A4 `dma_order`

**Purpose.** `DMA_DATA` must be ordered against dispatches, and `CP_SYNC` must hold, without
relying on the CPU fast path. There are four cases:

- **a.** Dispatch writes `S`; CS partial flush; DMA copy `S → D`; EOP; the CPU reads `D`.
  - Mirror Precise ✓ (readback). Mirror Disabled ✗: `D` exists only in the arena.
  - Shared ✓.
- **b.** Dispatch A reads `R`; DMA fill `R = 5`; dispatch B reads `R`.
  - ✓ everywhere since `a393f8d`. Before that commit, shared failed.
- **c.** DMA fill `flag` into memory that is not GPU-modified, then `WAIT_REG_MEM flag`.
  - ✓ everywhere: CPU fill in the mirror, GPU fill + flush in shared.
- **d.** As **c**, but `flag` was GPU-written earlier.
  - Mirror Disabled: hang (GPU fill into the arena). Mirror Precise ?. Shared ✓.

**Optional.** A raw `DMA_DATA` with `raw_wait`, chaining DMA1 `X → Y` and DMA2 `Y → Z`.

### A5 `gds_store`

**Purpose.** GDS values reach guest memory in order. Producers are `DMA_DATA` into GDS, since
psbc cannot emit GDS instructions.

**gfx case.**
1. `DMA_DATA` memory → GDS.
2. `EVENT_WRITE_EOS` GDS store → `dst`.
3. EOP label.

Expected ✓ everywhere: the path is synchronous today.

**compute case (ASC).**
1. `DMA_DATA` memory → GDS.
2. `RELEASE_MEM` GDS → `dst`.
3. Then one of two waits:
   - **variant L:** `RELEASE_MEM` Data32 label, then the CPU waits on the label;
   - **variant I:** the CPU waits on the end-of-pipe IRQ/event.

Expected:
- Variant L: mirror Disabled ✗ (copy only into the arena); shared ✓.
- Variant I: the IRQ is signalled at parse time, so shared reads a stale `dst` ✗. This is the
  explicit E1C "early IRQ" gap.

### A6 `gpu_written_descriptor`

**Purpose.** Resource descriptors (sharps) that the GPU wrote must be read in GPU order.

**Sequence.**
1. Dispatch P writes a V# into table memory `T` as plain SSBO data.
2. Dispatch C uses `T` as its set-0 table and reads through that V#.
3. EOP label.

**Oracle.** C reads the buffer that P's V# names, not the one an older V# named.

**Expected.**
- Mirror Precise ✓: the record-time read faults and downloads.
- Mirror Disabled ✗.
- Shared ✗ (#11).

This is "Buffer-F2" in the E3 review.

### A7 `mem_semaphore_handoff` (P3)

A gfx dispatch is followed by a `MEM_SEMAPHORE` signal. An ASC queue waits on the semaphore, then
dispatches a reader.

Expected ✓ everywhere: shadPS4 records both queues into one host timeline. This is a regression
guard for the case where queues stop being serialized.

### A8 `ce_dump_const_ram` (P3)

`WRITE_CONST_RAM` / `DUMP_CONST_RAM` on the CE writes memory that a DE dispatch reads after a
counter wait.

This needs CCB submission support in the harness. Expected results cannot be predicted until the
CE/DE counter handling is traced.

---

### B1 `dma_alias_order`

**Purpose.** A DMA through VA `B` must be ordered against a dispatch that writes the same
physical page through VA `A` (P1 in the E3 review).

**Cases.**
- **contiguous:** `B` is physically contiguous, so it can be shared.
  - Shared ✓ since `a393f8d`.
  - Mirror ✗: aliases diverge (kaaburgh/shadPS4#5).
- **non-contiguous:** `B` cannot be shared. The CPU fast path runs at parse time.
  - Shared ✗ (kaaburgh/shadPS4#8).

### B2 `dma_mixed_endpoints`

**Purpose.** A DMA copy from a shareable source into a destination that cannot be shared must be
visible to the guest in every readbacks mode (fixed in `8d07f08`).

**Sequence.**
1. Make `D` non-contiguous.
2. Bind `D` read-only once, so it gets arena memory.
3. Fill a fresh contiguous `S`.
4. DMA copy `S → D`.
5. EOP; the CPU reads `D`.

**Expected.**
- ✓ everywhere at `8d07f08`.
- Shared Disabled ✗ at `a393f8d..80eece2`, so the test kills that regression.

### B3 `demotion_divergence`

**Purpose.** Expose kaaburgh/shadPS4#7 (demotion runs `Finish` inside `ObtainBuffer`) without
BDA or vertex input.

**Sequence.**
1. Bind block `K` shared: dispatch 1 writes through it.
2. Dispatch 2 has two SSBO bindings. Binding 0 is a range inside `K`. Binding 1 spans `K` and a
   non-contiguous neighbour, so binding it demotes `K`.
3. Dispatch 2 writes through binding 0 and reads the same addresses through binding 1.

**Oracle.** The values read equal the values written.

**Expected.**
- Shared ✗: binding 0 points into the chunk, binding 1 into the arena.
- Mirror ✓.

The vertex/index-state loss and the fault-path hang from #7 are not covered: they need draws
with vertex buffers and BDA respectively.

### B4 `fallback_in_shared`

**Purpose.** Ranges that fall back to the mirror under shared backing:

- a physically contiguous range straddling the 256 MiB chunk boundary (placement helper);
- a non-contiguous range.

**Sequence.**
1. Compute writes through each range.
2. EOP.
3. The CPU verifies.

**Expected.**
- Shared Precise ✓.
- Shared Disabled ✗. The fallback range behaves like mirror Disabled, so the GPU writes never
  reach the guest.

"Shared Disabled 11/11" holds today only because the suite never falls back. This limitation is
not recorded in any issue yet; the test would document it, and an issue should follow.

### B5 `image_source_same_submit`

**Purpose.** Kill the image-source guard mutant (`ObtainBufferForImage` copying shared memory on
the CPU at record time).

**Sequence.** Reuse `image_buffer_alias` shaders, with everything in one DCB:
1. Compute writes `M`.
2. An R-only draw goes to a linear RT over `M`.
3. EOP.

**Oracle.** G/B/A keep the compute values.

**Expected.** ✓ everywhere. The mutant fails in shared mode.

### B6 `cross_queue_wait`

**Purpose.** Cross-queue progress for `WAIT_REG_MEM` (P2 in the E3 review, fixed in `a393f8d`).

**Sequence.**
1. A GFX DCB starts with `WAIT_REG_MEM flag == v`, then a dispatch reads `payload`.
2. The ASC queue is submitted after the GFX DCB. Its dispatch writes `payload` and `flag`, and
   it has no EOP or `RELEASE_MEM`.

**Expected.**
- Shared ✓. Without `a393f8d` this hangs: the producer is recorded but never submitted.
- Mirror Disabled: hang (the flag exists only in the arena).
- Mirror Precise ✓.

### B7 `bda_remap` (spike)

**Purpose.** Cover the unmap BDA reset (kaaburgh/shadPS4#13) and the partial-unmap case
(kaaburgh/shadPS4#12).

**Precondition (spike).**
- `directMemoryAccess=true` in the runner config.
- A psbc shader whose loads shadPS4 compiles to `ReadConst` with a dynamic address. Candidate:
  a `buffer_reference` read with a uniform address, which ACO may emit as `s_load`.

**Decision.** If the spike fails, record that the BDA path is unreachable from open guest code
and drop B7.

The partial-unmap case needs 64 KiB BDA blocks, so it cannot be seen on lavapipe (16 KiB
blocks).

## Out of scope

| Topic | Why |
|---|---|
| E2 baseline performance | Measurement on real games and hardware, not a PASS/FAIL scenario; decision pending |
| `WRITE_DATA` as a CPU-visible completion signal | Undefined without a cache flush; correct guests use EOP/EOS |
| Occlusion queries (`ZPASS_DONE`) | shadPS4 writes synthetic counters; unimplemented feature, not memory visibility |
| `COPY_DATA` | Not implemented in shadPS4; a separate issue when a game needs it |
| GPU-written `INDIRECT_BUFFER`, `REWIND` | Rare in PS4 titles; revisit if a game shows them |
| NVIDIA pinning / RADV udmabuf | Hardware-specific (kaaburgh/shadPS4#9, #10); not observable on lavapipe |

## Milestones

Each test lands as its own PR, like stages 1–10. The PR title says when the test fails on the
reference shadPS4.

1. **M0** — raw PM4 builders, placement helper, runner shared-mode switch.
2. **M1 (P1)** — A1, A3, A4, B1, B2, B4, B5. All on the GFX queue, no new shader features.
3. **M2 (P2)** — A2, A6, A5 (gfx), B3. Then the ASC helper, A5 (compute) and B6.
4. **M3** — A7, A8 and the B7 spike.

## Mapping to shadPS4 issues

| Issue | Scenarios |
|---|---|
| kaaburgh/shadPS4#4 (image/buffer alias) | B5 (control) |
| kaaburgh/shadPS4#5 (physical aliases, mirror) | B1 |
| kaaburgh/shadPS4#7 (demotion) | B3 |
| kaaburgh/shadPS4#8 (per-VA tracking) | B1 non-contiguous |
| kaaburgh/shadPS4#11 (E1C parse-time access) | A1, A3, A6; A2/B6 under mirror Disabled |
| kaaburgh/shadPS4#12 (fallbacks, partial unmap) | B4, B7 |
| kaaburgh/shadPS4#13 (uncovered guards) | B2, B5, B6, B7, A4 |
| new: fallback under shared + Disabled | B4 |
