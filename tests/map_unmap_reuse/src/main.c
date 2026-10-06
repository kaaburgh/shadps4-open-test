/*
 * map_unmap_reuse: Stage 9 of docs/memory-uma-test-roadmap.md.
 *
 * Memory lifetime rather than content: a 256 KiB direct-memory mapping is
 * used by the CPU and the GPU, unmapped, and mapped again at the same
 * virtual address. No CPU/GPU authority, page-tracking state or cached GPU
 * resource from an earlier lifetime may leak into the next one.
 *
 * Mapping layout (32 KiB each): X guard Y guard Z guard W guard.
 *
 * Each lifetime g = 1..3:
 *   1. CPU writes X = P(g), Z = Q(g), poisons Y and W, rewrites the guards.
 *   2. GPU: Y = transform(X), then W = transform(Z).
 *   3. CPU verifies Y, W, X, Z and the guards.
 *   4. GPU: Z = transform(Y). The CPU never reads this "dangling" write.
 *
 * Between lifetimes:
 *   remap_keep    (1 -> 2): unmap; map the same direct memory at the same
 *                 address. Direct memory keeps its contents, so before
 *                 lifetime 2 the CPU checks that X, Y, the guards and the
 *                 dangling GPU write to Z are all still there.
 *   release_reuse (2 -> 3): unmap; release the direct memory; allocate
 *                 again and map at the same address. The new memory's
 *                 contents are undefined, so lifetime 3 assumes nothing.
 *
 * In every lifetime, the CPU's writes to X and Z land on pages that an
 * earlier lifetime had uploaded or GPU-written, so a host that keeps that
 * state across the unmap hands the GPU stale data or downloads stale GPU
 * data over the CPU's writes.
 */

#define SHADTEST_NAME "map_unmap_reuse"
#include "shadtest_guest.h"

enum {
    REGION_BYTES = 32 * 1024,
    REGION_WORDS = REGION_BYTES / sizeof(uint32_t),
    MAPPING_BYTES = 8 * REGION_BYTES,
    X_OFFSET = 0 * REGION_BYTES,
    Y_OFFSET = 2 * REGION_BYTES,
    Z_OFFSET = 4 * REGION_BYTES,
    W_OFFSET = 6 * REGION_BYTES,
    GUARD_COUNT = 4,
    GROUPS_X = REGION_WORDS / ST_LOCAL_SIZE_X,
    LIFETIMES = 3,
    BINDINGS = 2,
    TABLE_X_TO_Y = 0,
    TABLE_Z_TO_W = 1,
    TABLE_Y_TO_Z = 2,
    TABLES = 3,
    /* SCE_KERNEL_MAP_FIXED */
    MAP_FIXED_FLAG = 0x10,
    ARENA_BYTES = 2 * 1024 * 1024,
};

_Static_assert(REGION_WORDS % ST_LOCAL_SIZE_X == 0,
               "regions must be whole workgroups");
/* shadPS4 copies read-only bindings of up to 16 KiB (STREAM_THRESHOLD) from
 * guest memory on every dispatch, so a stale cached copy of X or Z from an
 * earlier lifetime could never be read. */
_Static_assert(REGION_BYTES > 16 * 1024,
               "regions must be larger than shadPS4's stream threshold");

static const uint32_t OUTPUT_POISON = 0xcdcdcdcdu;

static uint32_t pattern_p(unsigned generation, uint32_t i) {
    return (i * 0x9E3779B1u) ^ (generation * 0x7F4A7C15u) ^ 0x0BADF00Du;
}

static uint32_t pattern_q(unsigned generation, uint32_t i) {
    return (i * 0x85EBCA77u) ^ (generation * 0xC2B2AE3Du) ^ 0x27D4EB2Fu;
}

static uint32_t guard_word(unsigned guard, uint32_t i) {
    return 0x47554152u ^ (i * 0x165667B1u) ^ (guard * 0x3C6EF372u);
}

/* Must match transform.comp. */
static uint32_t transform(uint32_t v, uint32_t i) {
    return ((v << 5) | (v >> 27)) ^ (i * 0x2545F491u) ^ 0x6A09E667u;
}

/* What the dangling write of lifetime g leaves in Z. */
static uint32_t dangling_z(unsigned generation, uint32_t i) {
    return transform(transform(pattern_p(generation, i), i), i);
}

typedef struct {
    off_t direct_memory;
    uint8_t* base;
    volatile uint32_t* x;
    volatile uint32_t* y;
    volatile uint32_t* z;
    volatile uint32_t* w;
    volatile uint32_t* guards[GUARD_COUNT];
} Mapping;

typedef struct {
    const char* reason;
    const char* stage;
    unsigned lifetime;
    uint32_t index;
    uint32_t expected;
    uint32_t got;
    int got_generation;
} Failure;

static Failure g_first;
static unsigned g_failures;

static void note_failure(const char* reason, const char* stage,
                         unsigned lifetime, uint32_t index, uint32_t expected,
                         uint32_t got, int got_generation) {
    if (!g_first.reason) {
        g_first.reason = reason;
        g_first.stage = stage;
        g_first.lifetime = lifetime;
        g_first.index = index;
        g_first.expected = expected;
        g_first.got = got;
        g_first.got_generation = got_generation;
    }
    g_failures += 1;
}

static const int MEMORY_PROT =
    ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW |
    ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE;

static bool allocate_direct(off_t* out) {
    const int res = sceKernelAllocateDirectMemory(
        0, (off_t)sceKernelGetDirectMemorySize(), MAPPING_BYTES, MAPPING_BYTES,
        ORBIS_KERNEL_WC_GARLIC, out);
    if (res != 0) {
        printf("sceKernelAllocateDirectMemory failed: 0x%x\n", res);
        return false;
    }
    return true;
}

/* Maps m->direct_memory, at `fixed` if it is non-NULL. */
static bool map_direct(Mapping* m, void* fixed) {
    void* addr = fixed;
    const int res = sceKernelMapDirectMemory(&addr, MAPPING_BYTES, MEMORY_PROT,
                                             fixed ? MAP_FIXED_FLAG : 0,
                                             m->direct_memory, MAPPING_BYTES);
    if (res != 0) {
        printf("sceKernelMapDirectMemory failed: 0x%x\n", res);
        return false;
    }
    if (fixed && addr != fixed) {
        printf("fixed mapping landed at %p instead of %p\n", addr, fixed);
        return false;
    }

    m->base = (uint8_t*)addr;
    m->x = (volatile uint32_t*)(m->base + X_OFFSET);
    m->y = (volatile uint32_t*)(m->base + Y_OFFSET);
    m->z = (volatile uint32_t*)(m->base + Z_OFFSET);
    m->w = (volatile uint32_t*)(m->base + W_OFFSET);
    for (unsigned g = 0; g < GUARD_COUNT; ++g) {
        m->guards[g] =
            (volatile uint32_t*)(m->base + (2 * g + 1) * REGION_BYTES);
    }
    return true;
}

static bool unmap(Mapping* m) {
    const int res = sceKernelMunmap(m->base, MAPPING_BYTES);
    if (res != 0) {
        printf("sceKernelMunmap failed: 0x%x\n", res);
        return false;
    }
    return true;
}

static bool release_direct(Mapping* m) {
    const int res =
        sceKernelReleaseDirectMemory(m->direct_memory, MAPPING_BYTES);
    if (res != 0) {
        printf("sceKernelReleaseDirectMemory failed: 0x%x\n", res);
        return false;
    }
    m->direct_memory = -1;
    return true;
}

/* Returns the generation in [1, below) whose value func(g, i) equals got, or
 * -1. */
static int find_generation(uint32_t (*func)(unsigned, uint32_t), uint32_t got,
                           uint32_t i, unsigned below) {
    for (unsigned g = 1; g < below; ++g) {
        if (func(g, i) == got) {
            return (int)g;
        }
    }
    return -1;
}

static uint32_t transformed_p(unsigned generation, uint32_t i) {
    return transform(pattern_p(generation, i), i);
}

static uint32_t transformed_q(unsigned generation, uint32_t i) {
    return transform(pattern_q(generation, i), i);
}

/* A GPU output word: classify a mismatch by the earlier lifetime whose input
 * it was computed from, or as the transform of an earlier dangling write. */
static unsigned check_output(const char* stage, unsigned lifetime,
                             const volatile uint32_t* out,
                             uint32_t (*expected_func)(unsigned, uint32_t)) {
    const unsigned before = g_failures;
    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        const uint32_t expected = expected_func(lifetime, i);
        const uint32_t got = out[i];
        if (got == expected) {
            continue;
        }
        const char* reason = "output_mismatch";
        int got_generation = find_generation(expected_func, got, i, lifetime);
        if (got == OUTPUT_POISON) {
            reason = "output_unwritten";
        } else if (got_generation >= 0) {
            reason = "gpu_stale_lifetime";
        } else {
            for (unsigned g = 1; g < lifetime; ++g) {
                if (got == transform(dangling_z(g, i), i)) {
                    reason = "gpu_stale_dangling";
                    got_generation = (int)g;
                    break;
                }
            }
        }
        note_failure(reason, stage, lifetime, i, expected, got, got_generation);
    }
    return g_failures - before;
}

/* A CPU-written word that nothing may change. */
static unsigned check_kept(const char* reason, const char* stage,
                           unsigned lifetime, const volatile uint32_t* ptr,
                           uint32_t (*expected_func)(unsigned, uint32_t),
                           unsigned generation) {
    const unsigned before = g_failures;
    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        const uint32_t expected = expected_func(generation, i);
        const uint32_t got = ptr[i];
        if (got != expected) {
            note_failure(reason, stage, lifetime, i, expected, got, -1);
        }
    }
    return g_failures - before;
}

static unsigned check_guards(const char* stage, unsigned lifetime,
                             const Mapping* m) {
    const unsigned before = g_failures;
    for (unsigned g = 0; g < GUARD_COUNT; ++g) {
        for (uint32_t i = 0; i < REGION_WORDS; ++i) {
            const uint32_t expected = guard_word(g, i);
            const uint32_t got = m->guards[g][i];
            if (got != expected) {
                note_failure("guard", stage, lifetime, g * REGION_WORDS + i,
                             expected, got, -1);
            }
        }
    }
    return g_failures - before;
}

static void run_lifetime(StQueue* queue, const StComputeShader* shader,
                         GnmBuffer* tables, Mapping* m, unsigned lifetime,
                         bool* infra_ok) {
    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        m->x[i] = pattern_p(lifetime, i);
        m->z[i] = pattern_q(lifetime, i);
    }
    st_fill_words(m->y, REGION_WORDS, OUTPUT_POISON);
    st_fill_words(m->w, REGION_WORDS, OUTPUT_POISON);
    for (unsigned g = 0; g < GUARD_COUNT; ++g) {
        for (uint32_t i = 0; i < REGION_WORDS; ++i) {
            m->guards[g][i] = guard_word(g, i);
        }
    }

    if (!st_dispatch_and_wait(queue, shader, &tables[TABLE_X_TO_Y * BINDINGS],
                              GROUPS_X) ||
        !st_dispatch_and_wait(queue, shader, &tables[TABLE_Z_TO_W * BINDINGS],
                              GROUPS_X)) {
        *infra_ok = false;
        return;
    }

    const unsigned bad_y = check_output("y", lifetime, m->y, transformed_p);
    const unsigned bad_w = check_output("w", lifetime, m->w, transformed_q);
    const unsigned bad_x =
        check_kept("cpu_reverted", "x", lifetime, m->x, pattern_p, lifetime);
    const unsigned bad_z =
        check_kept("cpu_reverted", "z", lifetime, m->z, pattern_q, lifetime);
    const unsigned bad_guard = check_guards("guard", lifetime, m);
    printf(
        "lifetime %u at %p (direct 0x%llx): %s bad_y=%u bad_w=%u bad_x=%u "
        "bad_z=%u bad_guard=%u\n",
        lifetime, (void*)m->base, (unsigned long long)m->direct_memory,
        bad_y + bad_w + bad_x + bad_z + bad_guard ? "FAIL" : "ok", bad_y, bad_w,
        bad_x, bad_z, bad_guard);

    /* Dangling GPU write: Z = transform(Y), never read back here. */
    if (!st_dispatch_and_wait(queue, shader, &tables[TABLE_Y_TO_Z * BINDINGS],
                              GROUPS_X)) {
        *infra_ok = false;
    }
}

/* After remap_keep, everything the previous lifetime left must still be in
 * the direct memory, including the GPU write the CPU never read. */
static void check_kept_contents(const Mapping* m, unsigned previous) {
    const unsigned bad_x =
        check_kept("lost_cpu_write", "x", previous, m->x, pattern_p, previous);
    const unsigned bad_y = check_kept("lost_gpu_write", "y", previous, m->y,
                                      transformed_p, previous);
    const unsigned bad_z = check_kept("lost_dangling_gpu_write", "z", previous,
                                      m->z, dangling_z, previous);
    const unsigned bad_w = check_kept("lost_gpu_write", "w", previous, m->w,
                                      transformed_q, previous);
    const unsigned bad_guard = check_guards("guard_after_remap", previous, m);
    printf(
        "remap_keep contents of lifetime %u: %s bad_x=%u bad_y=%u bad_z=%u "
        "bad_w=%u bad_guard=%u\n",
        previous, bad_x + bad_y + bad_z + bad_w + bad_guard ? "FAIL" : "ok",
        bad_x, bad_y, bad_z, bad_w, bad_guard);
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
    if (!tables) {
        return st_fail("reason=descriptor_memory");
    }

    Mapping m;
    memset(&m, 0, sizeof(m));
    if (!allocate_direct(&m.direct_memory) || !map_direct(&m, NULL)) {
        return st_fail("reason=map stage=initial");
    }
    void* const address = m.base;
    const off_t first_direct_memory = m.direct_memory;

    /* The address never changes, so neither do the descriptor tables. */
    tables[TABLE_X_TO_Y * BINDINGS + 0] = st_raw_buffer(m.x, REGION_BYTES);
    tables[TABLE_X_TO_Y * BINDINGS + 1] = st_raw_buffer(m.y, REGION_BYTES);
    tables[TABLE_Z_TO_W * BINDINGS + 0] = st_raw_buffer(m.z, REGION_BYTES);
    tables[TABLE_Z_TO_W * BINDINGS + 1] = st_raw_buffer(m.w, REGION_BYTES);
    tables[TABLE_Y_TO_Z * BINDINGS + 0] = st_raw_buffer(m.y, REGION_BYTES);
    tables[TABLE_Y_TO_Z * BINDINGS + 1] = st_raw_buffer(m.z, REGION_BYTES);

    bool infra_ok = true;

    run_lifetime(&queue, &shader, tables, &m, 1, &infra_ok);
    if (!infra_ok) {
        return st_fail("reason=dispatch lifetime=1");
    }

    /* remap_keep */
    if (!unmap(&m) || !map_direct(&m, address)) {
        return st_fail("reason=map stage=remap_keep");
    }
    check_kept_contents(&m, 1);

    run_lifetime(&queue, &shader, tables, &m, 2, &infra_ok);
    if (!infra_ok) {
        return st_fail("reason=dispatch lifetime=2");
    }

    /* release_reuse */
    if (!unmap(&m) || !release_direct(&m) ||
        !allocate_direct(&m.direct_memory) || !map_direct(&m, address)) {
        return st_fail("reason=map stage=release_reuse");
    }
    const bool same_direct_memory = m.direct_memory == first_direct_memory;

    run_lifetime(&queue, &shader, tables, &m, 3, &infra_ok);
    if (!infra_ok) {
        return st_fail("reason=dispatch lifetime=3");
    }

    if (st_gnm_errors != 0) {
        return st_fail("reason=gnm_error count=%u", st_gnm_errors);
    }

    if (g_first.reason) {
        return st_fail(
            "reason=%s region=%s lifetime=%u index=%u expected=%08x got=%08x "
            "got_generation=%d bad_words=%u",
            g_first.reason, g_first.stage, g_first.lifetime, g_first.index,
            g_first.expected, g_first.got, g_first.got_generation, g_failures);
    }

    uint32_t checksum = 0x811c9dc5u;
    const volatile uint32_t* outputs[2] = {m.y, m.w};
    for (unsigned k = 0; k < 2; ++k) {
        for (uint32_t i = 0; i < REGION_WORDS; ++i) {
            uint32_t v = outputs[k][i];
            for (int byte = 0; byte < 4; ++byte) {
                checksum ^= v & 0xffu;
                checksum *= 0x01000193u;
                v >>= 8;
            }
        }
    }

    st_emit_marker("PASS",
                   "lifetimes=%u transitions=remap_keep,release_reuse "
                   "same_direct_memory=%u checksum=%08x",
                   (unsigned)LIFETIMES, same_direct_memory ? 1u : 0u, checksum);

    unmap(&m);
    release_direct(&m);
    st_unload_compute_shader(&shader);
    st_arena_destroy(&arena);
    return 0;
}
