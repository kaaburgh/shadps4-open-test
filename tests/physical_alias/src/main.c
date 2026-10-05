/*
 * physical_alias: Stage 6 of docs/memory-uma-test-roadmap.md.
 *
 * One 64 KiB block of direct memory is mapped twice, at alias A and alias B.
 * Both mappings are CPU+GPU read/write. On a PS4 they are the same physical
 * bytes, so a write through either alias is visible through the other once
 * the usual completion rules are met.
 *
 * A host that tracks CPU and GPU accesses per guest virtual address can end
 * up with two independent authorities for one backing: a CPU write through A
 * does not invalidate what the GPU cached through B, and a GPU write through
 * B is not downloaded when the CPU reads through A.
 *
 * Block layout (16 KiB each): R1 R2 R3 guard. Outputs and GPU-side sources
 * live in ordinary (unaliased) memory.
 *
 * Cases, in order:
 *   cpu_alias            CPU writes the block through A, reads through B.
 *   cpu_to_gpu_first     CPU writes R1 = P(1) through A; GPU reads R1 through
 *                        B for the first time.
 *   cpu_to_gpu_again     CPU writes R1 = P(2) through A; GPU reads R1 through
 *                        B again, after B's view was already used.
 *   cpu_to_gpu_same      control: CPU writes R1 = P(3) through B; GPU reads
 *                        through B.
 *   gpu_to_cpu           GPU writes R2 through B; CPU reads through A.
 *   gpu_to_cpu_same      control: GPU writes R3 through B; CPU reads through
 *                        B, then through A.
 *
 * Every case runs even after a failure.
 */

#define SHADTEST_NAME "physical_alias"
#include "shadtest_guest.h"

/* Declared by the SDK, not by the OpenOrbis headers; the OpenOrbis libkernel
 * stub exports it. */
int32_t sceKernelEnableDmemAliasing(void);

enum {
    REGION_BYTES = 16 * 1024,
    REGION_WORDS = REGION_BYTES / sizeof(uint32_t),
    BLOCK_BYTES = 4 * REGION_BYTES,
    BLOCK_WORDS = BLOCK_BYTES / sizeof(uint32_t),
    R1_OFFSET = 0 * REGION_BYTES,
    R2_OFFSET = 1 * REGION_BYTES,
    R3_OFFSET = 2 * REGION_BYTES,
    GUARD_OFFSET = 3 * REGION_BYTES,
    GROUPS_X = REGION_WORDS / ST_LOCAL_SIZE_X,
    BINDINGS = 2,
    TABLES = 5,
    ARENA_BYTES = 2 * 1024 * 1024,
};

_Static_assert(REGION_WORDS % ST_LOCAL_SIZE_X == 0,
               "regions must be whole workgroups");

static const uint32_t OUTPUT_POISON = 0xcdcdcdcdu;

static uint32_t pattern(unsigned generation, uint32_t i) {
    return (i * 0x9E3779B1u) ^ (generation * 0x7F4A7C15u) ^ 0x0BADF00Du;
}

static uint32_t block_fill(uint32_t word) {
    return 0x414C4941u ^ (word * 0x165667B1u);
}

/* Must match transform.comp. */
static uint32_t transform(uint32_t v, uint32_t i) {
    return ((v << 5) | (v >> 27)) ^ (i * 0x2545F491u) ^ 0x6A09E667u;
}

typedef struct {
    const char* name;
    const char* reason;
    unsigned bad_words;
    uint32_t index;
    uint32_t expected;
    uint32_t got;
} CaseResult;

static void note(CaseResult* r, const char* reason, uint32_t index,
                 uint32_t expected, uint32_t got) {
    if (!r->reason) {
        r->reason = reason;
        r->index = index;
        r->expected = expected;
        r->got = got;
    }
    r->bad_words += 1;
}

/* Checks out[i] == transform(pattern(generation, i)). A mismatch equal to
 * an older generation's value is stale_reason; equal to poison, unwritten. */
static void check_transformed(CaseResult* r, const volatile uint32_t* out,
                              unsigned generation, const char* stale_reason) {
    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        const uint32_t expected = transform(pattern(generation, i), i);
        const uint32_t got = out[i];
        if (got == expected) {
            continue;
        }
        const char* reason = "mismatch";
        if (got == OUTPUT_POISON) {
            reason = "unwritten";
        } else {
            for (unsigned g = 1; g < generation; ++g) {
                if (got == transform(pattern(g, i), i)) {
                    reason = stale_reason;
                    break;
                }
            }
        }
        note(r, reason, i, expected, got);
    }
}

/* Checks a GPU-written alias region read by the CPU: it must hold
 * transform(pattern(generation)); the old block fill means the GPU write was
 * not visible. */
static void check_gpu_written(CaseResult* r, const volatile uint32_t* region,
                              uint32_t block_word, unsigned generation,
                              const char* stale_reason) {
    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        const uint32_t expected = transform(pattern(generation, i), i);
        const uint32_t got = region[i];
        if (got != expected) {
            note(r,
                 got == block_fill(block_word + i) ? stale_reason : "mismatch",
                 i, expected, got);
        }
    }
}

static void report(const CaseResult* r) {
    if (r->reason) {
        printf(
            "case %s: FAIL bad_words=%u first %s index=%u expected=%08x "
            "got=%08x\n",
            r->name, r->bad_words, r->reason, r->index, r->expected, r->got);
    } else {
        printf("case %s: ok\n", r->name);
    }
}

static bool map_alias(off_t direct_memory, uint8_t** out) {
    void* addr = NULL;
    const int prot = ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW |
                     ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE;
    const int res = sceKernelMapDirectMemory(&addr, BLOCK_BYTES, prot, 0,
                                             direct_memory, BLOCK_BYTES);
    if (res != 0) {
        printf("sceKernelMapDirectMemory failed: 0x%x\n", res);
        return false;
    }
    *out = (uint8_t*)addr;
    return true;
}

int main(void) {
    st_begin();

    StArena arena;
    if (!st_arena_init(&arena, ARENA_BYTES)) {
        return st_fail("reason=direct_memory");
    }

    StQueue queue;
    if (!st_queue_init(&arena, &queue)) {
        return st_fail("reason=command_memory");
    }

    StComputeShader shader;
    if (!st_load_compute_shader(&arena, "/app0/assets/transform.comp.sb",
                                &shader)) {
        return st_fail("reason=shader_load");
    }

    GnmBuffer* tables = (GnmBuffer*)st_arena_alloc(
        &arena, TABLES * BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    volatile uint32_t* outputs[3];
    volatile uint32_t* sources[2];
    for (unsigned k = 0; k < 3; ++k) {
        outputs[k] = (volatile uint32_t*)st_arena_alloc(&arena, REGION_BYTES,
                                                        ST_PAGE_BYTES);
    }
    for (unsigned k = 0; k < 2; ++k) {
        sources[k] = (volatile uint32_t*)st_arena_alloc(&arena, REGION_BYTES,
                                                        ST_PAGE_BYTES);
    }
    if (!tables || !outputs[2] || !sources[1]) {
        return st_fail("reason=arena_memory");
    }

    const int alias_res = sceKernelEnableDmemAliasing();
    if (alias_res != 0) {
        return st_fail("reason=enable_dmem_aliasing res=0x%x", alias_res);
    }

    off_t direct_memory = -1;
    const int alloc_res = sceKernelAllocateDirectMemory(
        0, (off_t)sceKernelGetDirectMemorySize(), BLOCK_BYTES, BLOCK_BYTES,
        ORBIS_KERNEL_WC_GARLIC, &direct_memory);
    if (alloc_res != 0) {
        return st_fail("reason=allocate res=0x%x", alloc_res);
    }

    uint8_t* alias_a = NULL;
    uint8_t* alias_b = NULL;
    if (!map_alias(direct_memory, &alias_a) ||
        !map_alias(direct_memory, &alias_b)) {
        return st_fail("reason=map_alias");
    }
    if (alias_a == alias_b) {
        return st_fail("reason=same_address");
    }
    printf("direct 0x%llx mapped at A=%p B=%p\n",
           (unsigned long long)direct_memory, (void*)alias_a, (void*)alias_b);

    volatile uint32_t* const a_block = (volatile uint32_t*)alias_a;
    volatile uint32_t* const b_block = (volatile uint32_t*)alias_b;
    volatile uint32_t* const a_r1 = (volatile uint32_t*)(alias_a + R1_OFFSET);
    volatile uint32_t* const b_r1 = (volatile uint32_t*)(alias_b + R1_OFFSET);
    volatile uint32_t* const a_r2 = (volatile uint32_t*)(alias_a + R2_OFFSET);
    volatile uint32_t* const b_r2 = (volatile uint32_t*)(alias_b + R2_OFFSET);
    volatile uint32_t* const a_r3 = (volatile uint32_t*)(alias_a + R3_OFFSET);
    volatile uint32_t* const b_r3 = (volatile uint32_t*)(alias_b + R3_OFFSET);
    const uint32_t r2_word = R2_OFFSET / sizeof(uint32_t);
    const uint32_t r3_word = R3_OFFSET / sizeof(uint32_t);

    for (unsigned k = 0; k < 3; ++k) {
        st_fill_words(outputs[k], REGION_WORDS, OUTPUT_POISON);
    }
    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        sources[0][i] = pattern(4, i);
        sources[1][i] = pattern(5, i);
    }

    /* Tables: R1(B) -> outputs[0..2]; sources[0] -> R2(B); sources[1] ->
     * R3(B). */
    for (unsigned k = 0; k < 3; ++k) {
        tables[k * BINDINGS + 0] = st_raw_buffer(b_r1, REGION_BYTES);
        tables[k * BINDINGS + 1] = st_raw_buffer(outputs[k], REGION_BYTES);
    }
    tables[3 * BINDINGS + 0] = st_raw_buffer(sources[0], REGION_BYTES);
    tables[3 * BINDINGS + 1] = st_raw_buffer(b_r2, REGION_BYTES);
    tables[4 * BINDINGS + 0] = st_raw_buffer(sources[1], REGION_BYTES);
    tables[4 * BINDINGS + 1] = st_raw_buffer(b_r3, REGION_BYTES);

    CaseResult cases[6] = {
        {.name = "cpu_alias"},        {.name = "cpu_to_gpu_first"},
        {.name = "cpu_to_gpu_again"}, {.name = "cpu_to_gpu_same"},
        {.name = "gpu_to_cpu"},       {.name = "gpu_to_cpu_same"},
    };

    /* cpu_alias */
    for (uint32_t j = 0; j < BLOCK_WORDS; ++j) {
        a_block[j] = block_fill(j);
    }
    st_compiler_barrier();
    for (uint32_t j = 0; j < BLOCK_WORDS; ++j) {
        if (b_block[j] != block_fill(j)) {
            note(&cases[0], "cpu_alias_mismatch", j, block_fill(j), b_block[j]);
        }
    }

    /* cpu_to_gpu_first */
    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        a_r1[i] = pattern(1, i);
    }
    if (!st_dispatch_and_wait(&queue, &shader, &tables[0 * BINDINGS],
                              GROUPS_X)) {
        return st_fail("reason=dispatch case=cpu_to_gpu_first");
    }
    check_transformed(&cases[1], outputs[0], 1, "gpu_stale_alias");

    /* cpu_to_gpu_again */
    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        a_r1[i] = pattern(2, i);
    }
    if (!st_dispatch_and_wait(&queue, &shader, &tables[1 * BINDINGS],
                              GROUPS_X)) {
        return st_fail("reason=dispatch case=cpu_to_gpu_again");
    }
    check_transformed(&cases[2], outputs[1], 2, "gpu_stale_alias");

    /* cpu_to_gpu_same (control) */
    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        b_r1[i] = pattern(3, i);
    }
    if (!st_dispatch_and_wait(&queue, &shader, &tables[2 * BINDINGS],
                              GROUPS_X)) {
        return st_fail("reason=dispatch case=cpu_to_gpu_same");
    }
    check_transformed(&cases[3], outputs[2], 3, "gpu_stale");

    /* gpu_to_cpu: GPU writes R2 through B, CPU reads through A. */
    if (!st_dispatch_and_wait(&queue, &shader, &tables[3 * BINDINGS],
                              GROUPS_X)) {
        return st_fail("reason=dispatch case=gpu_to_cpu");
    }
    check_gpu_written(&cases[4], a_r2, r2_word, 4, "cpu_stale_alias");

    /* gpu_to_cpu_same (control): read through B first, then through A. */
    if (!st_dispatch_and_wait(&queue, &shader, &tables[4 * BINDINGS],
                              GROUPS_X)) {
        return st_fail("reason=dispatch case=gpu_to_cpu_same");
    }
    check_gpu_written(&cases[5], b_r3, r3_word, 5, "cpu_stale");
    check_gpu_written(&cases[5], a_r3, r3_word, 5, "cpu_stale_alias");

    /* The guard region is never written after cpu_alias. */
    const uint32_t guard_word0 = GUARD_OFFSET / sizeof(uint32_t);
    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        const uint32_t expected = block_fill(guard_word0 + i);
        if (a_block[guard_word0 + i] != expected) {
            note(&cases[0], "guard", guard_word0 + i, expected,
                 a_block[guard_word0 + i]);
        }
    }

    unsigned failed = 0;
    const CaseResult* first = NULL;
    char failed_names[160] = "";
    for (unsigned k = 0; k < 6; ++k) {
        report(&cases[k]);
        if (cases[k].reason) {
            failed += 1;
            if (!first) {
                first = &cases[k];
            }
            const size_t used = strlen(failed_names);
            snprintf(failed_names + used, sizeof(failed_names) - used, "%s%s",
                     used ? "," : "", cases[k].name);
        }
    }

    if (st_gnm_errors != 0) {
        return st_fail("reason=gnm_error count=%u", st_gnm_errors);
    }

    if (first) {
        return st_fail(
            "reason=%s case=%s index=%u expected=%08x got=%08x failed_cases=%u "
            "failed=%s",
            first->reason, first->name, first->index, first->expected,
            first->got, failed, failed_names);
    }

    st_emit_marker("PASS", "cases=6 region_words=%u", (unsigned)REGION_WORDS);

    sceKernelMunmap(alias_b, BLOCK_BYTES);
    sceKernelMunmap(alias_a, BLOCK_BYTES);
    sceKernelReleaseDirectMemory(direct_memory, BLOCK_BYTES);
    st_unload_compute_shader(&shader);
    st_arena_destroy(&arena);
    return 0;
}
