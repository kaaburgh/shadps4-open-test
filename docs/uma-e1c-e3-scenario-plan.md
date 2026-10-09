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
    `RELEASE_MEM`, `MEM_SEMAPHORE`, `PFP_SYNC_ME` and a full-cache `ACQUIRE_MEM`.
    `sceGnmDrawCmdWaitGraphicsWrite` emits `ACQUIRE_MEM` only for CB/DB targets plus the volatile
    actions; it does not invalidate K$ or vector L1. These need raw PM4 dwords;
    `sceGnmCmdAllocInside` or a direct `cmdptr` write works.
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

### Guest synchronization contract

Every scenario must be correct on GCN (Liverpool, GFX7) hardware exactly as written. A shadPS4
failure then points at shadPS4, not at an under-synchronized workload. If PS4 hardware becomes
available, every scenario should pass there; that run is the control for this contract.

The CP launches a dispatch and moves on to the next packet without waiting for the shader.
Shader stores reach L2 through the write-through vector L1, but vector L1 and K$ are not coherent
with other writers. CP DMA with `SRC_ADDR`/`DST_ADDR` (what `sceGnmDrawCmdFillMemory` and
`sceGnmDrawCmdCopyMemory` emit) and the CB/DB may bypass L2. So every edge between agents gets an
explicit barrier:

- **[B] full barrier:**
  - `EVENT_WRITE CS_PARTIAL_FLUSH` (after a draw, also `PS_PARTIAL_FLUSH`), so that earlier
    shaders have finished;
  - then `ACQUIRE_MEM` that writes back and invalidates L2 and invalidates vector L1 and K$
    (and CB/DB when an RT is involved).

  One conservative barrier is used for every edge instead of the minimal bits per edge, because
  minimal synchronization is not what these tests measure.
- **[P] `PFP_SYNC_ME`** goes after [B] and before `COND_EXEC`, so that a PFP-side read cannot run
  ahead of the barrier. It is harmless if the ME processes the packet.
- **Edges:**
  - shader → later CP packet or DMA: [B] (+ [P] for `COND_EXEC`);
  - CP write (`WRITE_DATA` with `wr_confirm`, `DMA_DATA` with `CP_SYNC`) → later shader: [B];
  - shader → shader through memory: [B];
  - result → CPU: the harness EOP label with `CACHE_FLUSH_AND_INV_TS_EVENT`, as in
    `st_dispatch_and_wait`, then the CPU waits on the label.
- **Packet choices:**
  - `WRITE_DATA` and `WAIT_REG_MEM` use ENGINE_SEL = ME;
  - barriers use `ACQUIRE_MEM`, not `SURFACE_SYNC`: shadPS4 has no `SURFACE_SYNC` case and hits
    UNREACHABLE on unknown opcodes, and OpenGNM itself emits `ACQUIRE_MEM` in the DCB;
  - the exact `CP_COHER_CNTL` bits come from Mesa radeonsi's GFX7 cache-flush path and OpenGNM
    `amdgfxregs.h`, and an M0 unit test pins the dwords, as `test_drawcmd.c` does.

**These barriers do not change the expected results.** shadPS4 at `8d07f08` treats them as
no-ops:
- `EVENT_WRITE` partial flushes and `ACQUIRE_MEM` on both queues (`liverpool.cpp:800`, `979`,
  `1210`, `1374`);
- `PFP_SYNC_ME` on the GFX queue (`liverpool.cpp:1040`). Only that queue has a PFP, and [P] is
  used only before `COND_EXEC`, which is GFX-only.

They are not submit points either, which B6 depends on.

Sequences below mark the barriers as **[B]** and **[P]**.

## Common infrastructure (milestone M0)

This is needed before most scenarios. Keep it as small as the first two tests require.

1. **Raw PM4 builders** in `tests/common` (or a new `tests/common/pm4.h`). Each builder returns
   `false` when the command buffer has no room. Dword layouts are cross-checked against shadPS4
   `pm4_cmds.h` and OpenGNM `sid.h`. Needed builders:
   - barriers: `EVENT_WRITE` CS/PS partial flush, full-cache `ACQUIRE_MEM`, `PFP_SYNC_ME`
     (see the [contract](#guest-synchronization-contract));
   - `WRITE_DATA` (memory dst, ENGINE_SEL ME, `wr_confirm`);
   - `COND_EXEC`;
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
- ✗ #N: fail; N is the tracking issue.
- **R: not diagnostic.** The oracle needs a GPU write to reach guest memory, either for the CPU
  check or for a parse-time CP read. The mirror in readbacks Relaxed/Disabled never returns GPU
  writes, and the existing suite passes 1/11 there for that reason. The test still runs and
  records the result, but an R failure says nothing about the packet under test and is not a
  regression signal.
- "R hang": the CP waits for such a value; the guest reports a bounded timeout.
- ?: cannot be predicted from the code.

**Oracle via** says how the checked values reach the guest:
- **CP**: a packet or the CPU-side DMA path writes guest memory directly, so the value is
  checkable in every mode;
- **shader** or **RT**: a shader or render-target output, checkable only where GPU writes reach
  the guest (mirror Precise, shared backing).

Relaxed is expected to match Disabled: the current suite gives the same results in both modes
for mirror and for shared. **Mirror Precise is the reference baseline.** Where it fails for a
known issue, the scenario's control without that feature is the reference: a single VA for B1
(#5), a CPU-written `M` for B5 (#4).

Every scenario also needs the M0 barrier builders. **Needs** lists only the extra
infrastructure.

| ID | Test | Phase | Exercises | Oracle via | Prio | Needs | mirror P | mirror R/D | shared P | shared R/D |
|---|---|---|---|---|---|---|---|---|---|---|
| A1 | `write_data_order` | E1C | `WRITE_DATA` between two readers | shader (`X`: CP) | P1 | PM4 | ✓ | R (`X` ✓) | ✗ #11 | ✗ #11 |
| A2 | `wait_gpu_written` | E1C | `WAIT_REG_MEM` on a shader-written flag, same queue | CP label + shader | P2 | — | ✓ | R hang | ✓ | ✓ |
| A3 | `cond_exec_gpu_predicate` | E1C | `COND_EXEC` on a shader-written predicate | CP | P1 | PM4 | ✓ | R | ✗ #11 | ✗ #11 |
| A4 | `dma_order` | E1C/E3 | `DMA_DATA` copy/fill around dispatches; fill + wait | per case | P1 | — (+PM4 for `raw_wait`) | ✓ (d: ?) | R (a, b), ✓ (c), R hang (d) | ✓ | ✓ |
| A5 | `gds_store` | E1C | GDS ← `DMA_DATA`; EOS GDS store (gfx); `RELEASE_MEM` GDS store (compute) | CP (gfx); GPU copy in shadPS4 (compute) | P2 | PM4, ASC | ✓ | ✓ (gfx), R (compute) | ✓ / ✗ early IRQ | ✓ / ✗ early IRQ |
| A6 | `gpu_written_descriptor` | E1C | A dispatch writes a V# that the next dispatch reads its data through | shader | P2 | — | ✓ | R | ✗ #11 | ✗ #11 |
| A7 | `mem_semaphore_handoff` | E1C | `MEM_SEMAPHORE` producer/consumer across queues | shader | P3 | PM4, ASC | ✓ | R | ✓ | ✓ |
| A8 | `ce_dump_const_ram` | E1C | CE `DUMP_CONST_RAM` vs DE readers | shader | P3 | CCB + PM4 | ? | R | ? | ? |
| B1 | `dma_alias_order` | E3 | DMA through a second VA of memory a dispatch writes via the first | shader / CP | P1 | placement | ✗ #5 | R | ✓ (shareable alias), ✗ #8 (non-contiguous alias) | same |
| B2 | `dma_mixed_endpoints` | E3 | DMA copy shareable source → mirror-only destination | CP | P1 | placement | ✓ | ✓ | ✓ | ✓ |
| B3 | `demotion_divergence` | E3 | A block bound shared, then demoted while the same dispatch still writes it through the chunk | shader | P2 | placement | ✓ | R | ✗ #7 | ✗ #7 |
| B4 | `fallback_in_shared` | E3 | Chunk-crossing and non-contiguous ranges under shared backing | shader | P1 | placement | ✓ | R | ✓ | R (fallback ranges; control ✓) |
| B5 | `image_source_same_submit` | E3 | Compute writes, then a draw initializes its RT from those bytes in the same submit | RT | P1 | — | ✗ #4 | R | ✓ | ✓ |
| B6 | `cross_queue_wait` | E3/E1C | GFX `WAIT_REG_MEM` on a flag an ASC dispatch writes, producer parsed later | CP label + shader | P2 | ASC | ✓ | R hang | ✓ | ✓ |
| B7 | `bda_remap` | E3 | BDA read across unmap/remap | shader | spike | `directMemoryAccess`, shader | ? | R | ? | ? |

Some scenarios are expected to pass in every diagnostic configuration today: B2, A4 case c,
A5 gfx, A7, and B5/A2/B6 in shared mode. They are regression and mutation-killing tests for
guards the suite does not reach now. For example, a mutant without the B5 guard still passes
11/11 today.

---

### A1 `write_data_order`

**Purpose.** `WRITE_DATA` must take effect in CP order relative to dispatches recorded before
and after it.

**Sequence.** All steps are in one DCB.
1. The CPU sets `X = 1`.
2. Dispatch A reads `X` into `outA`.
3. **[B]**: A has finished reading `X` before the CP changes it. `wr_confirm` alone confirms
   only the write; it does not retire earlier shader reads.
4. `WRITE_DATA X = 2` (memory, ENGINE_SEL ME, `wr_confirm`).
5. **[B]**: B cannot hit a cached `X` from A.
6. Dispatch B reads `X` into `outB`.
7. EOP label; the CPU waits.

**Oracle.**
- `outA == 1` and `outB == 2` (shader outputs);
- `X == 2` (CP-written, so checkable in every mode).

**Controls.**
- The same with a CPU write of `X` between two separate submits.
- `X` as a small UBO (stream path) and as an SSBO larger than the stream threshold.

**Expected.**
- Mirror Precise ✓. A uploads `X` at record time. The `memcpy` marks the page CPU-modified, and B
  uploads again.
- Mirror Relaxed/Disabled: R for `outA`/`outB`; `X == 2` ✓.
- Shared ✗: A reads `X` when it executes, after the `memcpy`, so `outA == 2`. The barriers do
  not change this: shadPS4 ignores them and still runs the `memcpy` at parse time. Tracked in
  kaaburgh/shadPS4#11.

### A2 `wait_gpu_written`

**Purpose.** A `WAIT_REG_MEM` on memory that a shader writes must complete.

**Sequence.**
1. Dispatch P writes `payload`, then `flag = 0xC0FFEE`.
2. **[B]**.
3. `WAIT_REG_MEM flag == 0xC0FFEE` (ENGINE_SEL ME).
4. Dispatch C copies `payload` to `out`. The barrier at step 2 already invalidated C's caches.
5. EOP label.

On hardware the wait is already satisfied when the CP reaches it. The test still targets the
emulator: shadPS4 evaluates the wait on the CP thread before P has executed on the host.

**Oracle.**
- The label arrives within a bounded wait (CP-written).
- `out == payload` (shader output).

**Expected.**
- Mirror Precise ✓: the CP read faults and downloads.
- Mirror Relaxed/Disabled: R hang. The flag exists only in the arena.
- Shared ✓: `FlushForMemoryWait` submits P.

The mirror R hang is the known E1 gap "general GPU-written RAW under Disabled readbacks".

### A3 `cond_exec_gpu_predicate`

**Purpose.** `COND_EXEC` must evaluate a predicate written by preceding GPU work.

**Sequence.**
1. Dispatch P writes `pred` (0 in one case, 1 in the other).
2. **[B]**, then **[P]**.
3. `COND_EXEC(pred, n)` guarding `WRITE_DATA marker = 7`.
4. EOP label.

**Oracle.** `marker == 7` exactly when `pred == 1`. `marker` is CP-written, but the CP decision
depends on a shader-written `pred`.

**Control.** `pred` written by the CPU before submit. It is CP-only, so it is diagnostic in every
mode.

**Expected.**
- Mirror Precise ✓ through the CP-thread fault download.
- Mirror Relaxed/Disabled: R. The CP never sees P's `pred`.
- Shared ✗: the predicate is read before P executes.

The fix belongs in E1C (kaaburgh/shadPS4#11). shadPS4 ignores the barriers today.

### A4 `dma_order`

**Purpose.** `DMA_DATA` must be ordered against dispatches, and `CP_SYNC` must hold, without
relying on the CPU fast path.

`CP_SYNC` makes later CP packets wait for the DMA. It does not retire earlier shaders, so every
shader → DMA edge gets **[B]** first. A DMA that writes memory a later shader reads also gets
**[B]** after it: the DMA may bypass L2, and L2 or L1 can still hold the old line.

There are four cases:

- **a.** Dispatch writes `S`; **[B]**; DMA copy `S → D`; EOP; the CPU reads `D`.
  - Mirror Precise ✓ (readback).
  - Mirror Relaxed/Disabled: R. `S` is GPU-modified, so the copy runs on the GPU and `D` exists
    only in the arena.
  - Shared ✓.
- **b.** Dispatch A reads `R` into `outA`; **[B]**; DMA fill `R = 5`; **[B]**; dispatch B reads
  `R` into `outB`; EOP.
  - Oracle: `outA` = the initial value, `outB == 5` (shader outputs).
  - Mirror Precise ✓: A uploads at record time, the CPU fill marks the page CPU-modified, and B
    uploads again.
  - Mirror Relaxed/Disabled: R.
  - Shared ✓ since `a393f8d`, where the fill moved to the GPU timeline. Before that commit the
    fill ran on the CPU at parse time and A read 5 (✗).
- **c.** DMA fill `flag` into memory that is not GPU-modified, then `WAIT_REG_MEM flag`.
  - No shader is involved and the oracle is the CP-written label, so this case is diagnostic in
    every mode.
  - ✓ everywhere: CPU fill in the mirror, GPU fill + flush in shared.
- **d.** As **c**, but a dispatch wrote `flag` earlier; **[B]** before the DMA, so that a dirty
  L2 line cannot later overwrite the DMA write.
  - Mirror Relaxed/Disabled: R hang. `flag` is GPU-modified, so the fill runs on the GPU into the
    arena.
  - Mirror Precise ?.
  - Shared ✓.

**Optional.** A raw `DMA_DATA` with `raw_wait`, chaining DMA1 `X → Y` and DMA2 `Y → Z`.

### A5 `gds_store`

**Purpose.** GDS values reach guest memory in order. Producers are `DMA_DATA` into GDS, since
psbc cannot emit GDS instructions. No shader is involved, so no **[B]** is needed: `CP_SYNC`
orders the DMA before the store.

**gfx case.**
1. `DMA_DATA` memory → GDS (`CP_SYNC`), from memory the CPU initialized.
2. `EVENT_WRITE_EOS` GDS store → `dst`.
3. EOP label.

Expected ✓ everywhere, mirror Relaxed/Disabled included. The path is synchronous today: shadPS4
waits for the GPU, reads GDS and stores into guest memory on the CP thread.

**compute case (ASC).**
1. `DMA_DATA` memory → GDS.
2. `RELEASE_MEM` GDS → `dst`.
3. Then one of two waits:
   - **variant L:** `RELEASE_MEM` Data32 label, then the CPU waits on the label;
   - **variant I:** the CPU waits on the end-of-pipe IRQ/event.

Expected:
- shadPS4 copies GDS → `dst` on the GPU, so mirror Relaxed/Disabled is R for both variants.
- Variant L: mirror Precise ✓; shared ✓.
- Variant I: the IRQ is signalled at parse time, so shared reads a stale `dst` ✗. This is the
  explicit E1C "early IRQ" gap.

### A6 `gpu_written_descriptor`

**Purpose.** Resource descriptors (sharps) that the GPU wrote must be read in GPU order.

**Sequence.**
1. The CPU writes `V#_old` (naming buffer `O`) into table `T`, and fills `O` and `N` with
   different patterns.
2. Dispatch P writes `V#_new` (naming `N`) into `T` as plain SSBO data.
3. **[B]**: P's stores have completed, and K$ cannot return the old V#.
4. Dispatch C uses `T` as its set-0 table and copies through that V# into `out`.
5. EOP label.

On GCN the V# is read by C's scalar loads through K$; the CP only passes `T`'s address in user
data (`sceGnmDrawCmdSetPointerUserData`). So the edge is shader → shader, and [B] covers it; no
[P] is needed. shadPS4 instead reads `T` on the CP thread while recording C, and that read is
what the test exercises.

**Oracle.** `out` holds `N`'s pattern (shader output).

**Controls.**
- The CPU writes `V#_new` before submit and P is skipped. This shows that C reads through the V#
  in the table at all; it is diagnostic wherever shader outputs are.
- P writes `V#_old` again: `out` holds `O`'s pattern.

**Expected.**
- Mirror Precise ✓: the record-time read faults and downloads.
- Mirror Relaxed/Disabled: R. `out` is shader-written, and the record-time read of `T` sees
  `V#_old`.
- Shared ✗ (#11).

This is "Buffer-F2" in the E3 review.

### A7 `mem_semaphore_handoff` (P3)

**Sequence.**
1. A gfx dispatch writes `data`.
2. **[B]**.
3. `MEM_SEMAPHORE` signal.
4. An ASC queue waits on the semaphore.
5. **[B]** on the ASC.
6. A reader dispatch copies `data` to `out`.
7. `RELEASE_MEM` label.

Expected:
- ✓ in mirror Precise and shared. shadPS4 records both queues into one host timeline.
- R in mirror Relaxed/Disabled (`out` is shader-written).

This is a regression guard for the case where queues stop being serialized.

### A8 `ce_dump_const_ram` (P3)

`WRITE_CONST_RAM` / `DUMP_CONST_RAM` on the CE writes memory that a DE dispatch reads after a
counter wait.

This needs CCB submission support in the harness. Expected results cannot be predicted until the
CE/DE counter handling is traced.

---

### B1 `dma_alias_order`

**Purpose.** A DMA through VA `B` must be ordered against a dispatch that writes the same
physical page through VA `A` (P1 in the E3 review; its counterexample already had a CS partial
flush).

**Mappings** (placement helper):
- **contiguous:** one 16 KiB physical page `P` is mapped at VA `A` and at VA `B`.
- **non-contiguous:** `B` covers two adjacent 16 KiB virtual pages. `B[0]` aliases `P`, as `A`
  does. `B[1]` maps a separate allocation `Q` that is not physically adjacent to `P`. The DMA
  range spans both pages, so `LookupSharedBlocks` fails for it.

  A single-page alias cannot reach #8 on lavapipe: a BufferCache block there is 16 KiB, the
  same as a backing page, so such an alias is always contiguous and stays shareable. The
  two-page layout fails the lookup at any block size, because the range always includes the
  discontinuity.

**Sequence.**
1. A dispatch writes `v = 1` through `A`.
2. **[B]**: the dispatch has finished, and no dirty L2 line can later overwrite the DMA write.
3. One of two operations, over the whole of `B`:
   - **fill:** DMA fill through `B` with 2;
   - **copy:** DMA copy `B → D`.
4. EOP; the CPU reads through `A` and `B` (fill) or reads `D` (copy).

**Oracle.**
- fill: 2 through `A` and through all of `B`;
- copy: `D` holds 1 where `B` aliases `P`, and `Q`'s initial contents for `B[1]`.

**Shared/fallback decision.** Each run records from the shadPS4 log whether `B` took the shared
path or the mirror fallback. A non-contiguous case that stayed shareable does not count as
covering #8.

**Control.** The same sequence through a single VA (`B == A`). It passes in mirror Precise and
is the reference for the mirror columns.

**Cases.**
- **contiguous:** `B` can be shared.
  - Shared ✓ since `a393f8d`.
  - Mirror Precise ✗: aliases diverge (kaaburgh/shadPS4#5). Mirror Relaxed/Disabled: R.
- **non-contiguous:** `B` cannot be shared. shadPS4 accesses `P` through `B` while recording,
  before the dispatch through `A` has run: the fill takes the CPU fast path, and the copy
  uploads `B`'s mirror from guest memory.
  - Shared ✗ (kaaburgh/shadPS4#8).

### B2 `dma_mixed_endpoints`

**Purpose.** A DMA copy from a shareable source into a destination that cannot be shared must be
visible to the guest in every readbacks mode (fixed in `8d07f08`).

**Sequence.**
1. Make `D` non-contiguous.
2. A dispatch binds `D` read-only once, so `D` gets arena memory. Its output is not checked.
3. **[B]**.
4. The CPU fills a fresh contiguous `S`.
5. DMA copy `S → D`.
6. EOP; the CPU reads `D`.

No shader writes `S` or `D`, so the oracle reaches the guest through the CP path and is
diagnostic in every mode.

**Expected.**
- ✓ everywhere at `8d07f08`.
- Shared Disabled ✗ at `a393f8d..80eece2`, so the test kills that regression.

### B3 `demotion_divergence`

**Purpose.** Expose kaaburgh/shadPS4#7 (demotion runs `Finish` inside `ObtainBuffer`) without
BDA or vertex input. The case: a descriptor obtained earlier in a dispatch points into the
chunk, and a later binding demotes the same block.

**Sequence.**
1. Dispatch 1 writes pattern `p1` into block `K`. `K` is fresh and shareable, so it is served
   shared.
2. **[B]**.
3. Dispatch 2 has two SSBO bindings:
   - binding 0 is a range inside `K`;
   - binding 1 spans `K` and a non-contiguous neighbour `Nb`, which forces mirror fallback and
     demotes `K` after binding 0 already points into the chunk.

   Dispatch 2 writes `p2` through binding 0 and reads only `Nb` through binding 1. No address is
   accessed through both bindings within the dispatch, which would be ill-defined on hardware.
4. **[B]**.
5. Dispatch 3 reads `K` into `out`. `out` is a fresh shareable buffer, so shared Disabled can
   observe it.
6. EOP label.

**Oracle.** `out == p2` (shader output).

**Expected.**
- Mirror Precise ✓.
- Mirror Relaxed/Disabled: R.
- Shared ✗ (#7):
  1. Demotion uploads `K` into the arena from guest memory before dispatch 2 runs.
  2. Dispatch 2 writes `p2` through the chunk into guest memory, which the arena never sees.
  3. Dispatch 3 reads `K` from the arena, so `out == p1`.
- **Binding order.** The failure depends on shadPS4 obtaining binding 0 before binding 1. The
  test also runs the reversed order, which is expected to pass because both bindings land in
  the arena, and records which order shadPS4 uses.

The vertex/index-state loss and the fault-path hang from #7 are not covered: they need draws
with vertex buffers and BDA respectively.

### B4 `fallback_in_shared`

**Purpose.** Ranges that fall back to the mirror under shared backing:

- a physically contiguous range straddling the 256 MiB chunk boundary (placement helper);
- a non-contiguous range;
- a shareable control range.

**Sequence.**
1. Compute writes through each range.
2. EOP.
3. The CPU verifies.

**Expected.**
- Mirror Precise and shared Precise ✓.
- Mirror Relaxed/Disabled: R.
- Shared Relaxed/Disabled:
  - the fallback ranges: R. They behave like mirror Disabled, so the GPU writes never reach the
    guest;
  - the control range ✓, which shows the failure comes from the fallback.

B4 does not test ordering. It documents that shared backing does not lift the readback
limitation for ranges that fall back. It becomes a regression test once fallback ranges stop
losing writes (kaaburgh/shadPS4#12).

"Shared Disabled 11/11" holds today only because the suite never falls back. This limitation is
not recorded in any issue yet; the test would document it, and an issue should follow.

### B5 `image_source_same_submit`

**Purpose.** Kill the image-source guard mutant (`ObtainBufferForImage` copying shared memory on
the CPU at record time).

**Sequence.** Reuse `image_buffer_alias` shaders, with everything in one DCB:
1. Compute writes `M`.
2. **[B]** with a CB invalidate. On GFX7 the CB does not access memory through L2, so the
   compute data must be written back before the partial-mask draw reads `M`.
3. An R-only draw goes to a linear RT over `M`.
4. EOP with a CB flush (`FLUSH_AND_INV_CB_DATA_TS`, as `image_buffer_alias` uses).

**Oracle.** R = 90, and G/B/A keep the compute values (RT output).

**Control.** The CPU writes `M` instead of compute, as `image_buffer_alias` case
`rt_masked_after_cpu_write` does. It passes in mirror Precise.

**Expected.**
- Mirror Precise ✗ #4. This is `image_buffer_alias` case `rt_masked_after_buffer_write` in one
  DCB, and that case fails in mirror Precise at `8d07f08` (suite run for kaaburgh/shadPS4#6).
- Mirror Relaxed/Disabled: R.
- Shared ✓: the multi-submit case passes in all three readbacks modes.
- The guard mutant fails in shared mode.

### B6 `cross_queue_wait`

**Purpose.** Cross-queue progress for `WAIT_REG_MEM` (P2 in the E3 review, fixed in `a393f8d`).

**Sequence.**
1. A GFX DCB:
   1. `WAIT_REG_MEM flag == v`;
   2. **[B]**;
   3. a dispatch copies `payload` to `out`;
   4. EOP label.
2. The ASC queue is submitted after the GFX DCB:
   1. a dispatch writes `payload`, then, after a buffer memory barrier in the shader, `flag`;
   2. `EVENT_WRITE CS_PARTIAL_FLUSH`;
   3. `ACQUIRE_MEM` with L2 write-back.

   There is no EOP and no `RELEASE_MEM`. shadPS4 treats the compute `EVENT_WRITE` and
   `ACQUIRE_MEM` as no-ops, so the producer still has no submit point. That is the condition
   for the original hang.

**Expected.**
- Shared ✓. Without `a393f8d` this hangs: the producer is recorded but never submitted.
- Mirror Precise ✓.
- Mirror Relaxed/Disabled: R hang. The flag exists only in the arena.

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
| kaaburgh/shadPS4#4 (image/buffer alias) | B5 (mirror Precise) |
| kaaburgh/shadPS4#5 (physical aliases, mirror) | B1 |
| kaaburgh/shadPS4#7 (demotion) | B3 |
| kaaburgh/shadPS4#8 (per-VA tracking) | B1 non-contiguous |
| kaaburgh/shadPS4#11 (E1C parse-time access) | A1, A3, A6 |
| kaaburgh/shadPS4#12 (fallbacks, partial unmap) | B4, B7 |
| kaaburgh/shadPS4#13 (uncovered guards) | B2, B5, B6, B7, A4 |
| new: fallback under shared + Disabled | B4 |
