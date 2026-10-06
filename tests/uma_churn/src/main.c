/*
 * uma_churn: Stage 10 of docs/memory-uma-test-roadmap.md.
 *
 * An integration workload built only from operations the earlier stages
 * validate on their own. A 4 MiB direct-memory pool (1024 pages) is
 * filled with a known pattern, then for each generation g = 1..16:
 *
 *   1. A deterministic plan picks 96 pages. Each page is CPU-only,
 *      GPU-only, or shared: a CPU run in one half and a GPU run in the other
 *      half of the same page. Runs are 1..64 words.
 *   2. The writes, in an order that alternates by generation:
 *        odd g:  the CPU writes its runs, then writer.comp the GPU runs;
 *        even g: writer.comp first, then the CPU, which reads nothing in
 *                between, so on shared pages it writes next to GPU data it
 *                has not read (Stage 3's transition).
 *      CPU runs hold cpu_value(w, g), GPU runs gpu_value(w, g).
 *   3. reader.comp reads 1024 words into out[]: a third of them words the
 *      CPU wrote in step 2, a third words the GPU wrote, a third random.
 *   4. The CPU checks out[], every word written in step 2, and 512 random
 *      words, all against a host-side model of the pool.
 *   5. In generations 4, 8 and 12 the pool is unmapped and the same
 *      direct memory is mapped again at the same address (Stage 9's
 *      remap_keep). This happens in step 2, between the GPU runs and the
 *      CPU runs, so the GPU's writes are still pending at the unmap and, as
 *      in Stage 9, the CPU writes first after the remap. 512 more random
 *      words are checked after step 4.
 *
 * Within a generation the CPU and GPU never write the same word, so every
 * result is defined; pages are shared freely. Every generation runs even
 * after a failure. At the end the whole pool is checked and its FNV-1a
 * digest is reported.
 *
 * Aliasing (Stage 6) is not mixed in: it currently fails on shadPS4 and
 * would hide everything else.
 */

#define SHADTEST_NAME "uma_churn"
#include "shadtest_guest.h"

enum {
    POOL_PAGES = 1024,
    PAGE_WORDS = ST_PAGE_BYTES / sizeof(uint32_t),
    POOL_WORDS = POOL_PAGES * PAGE_WORDS,
    POOL_BYTES = POOL_WORDS * sizeof(uint32_t),
    GENERATIONS = 16,
    REMAP_EVERY = 4,
    ACTIVE_PAGES = 96,
    MAX_RUN = 64,
    MAX_WRITES = ACTIVE_PAGES * MAX_RUN * 2,
    READ_COUNT = 1024,
    SAMPLE_COUNT = 512,
    WRITER_BINDINGS = 3,
    READER_BINDINGS = 4,
    PARAMS_WORDS = 3,
    /* SCE_KERNEL_MAP_FIXED */
    MAP_FIXED_FLAG = 0x10,
    ARENA_BYTES = 2 * 1024 * 1024,
};

_Static_assert(GENERATIONS < 256, "claims are stamped with a u8 generation");
_Static_assert(REMAP_EVERY % 2 == 0, "remaps must be in GPU-first generations");

static const uint32_t OUT_POISON = 0xcdcdcdcdu;

static uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static uint32_t init_value(uint32_t w) {
    return mix(w ^ 0x3C6EF372u);
}

/* Must match writer.comp. */
static uint32_t gpu_value(uint32_t w, uint32_t g) {
    return mix(w * 0x9E3779B1u + g * 0x85EBCA6Bu) ^ 0x6A09E667u;
}

static uint32_t cpu_value(uint32_t w, uint32_t g) {
    return mix(w * 0x9E3779B1u + g * 0x85EBCA6Bu) ^ 0xBB67AE85u;
}

/* Must match reader.comp. */
static uint32_t read_result(uint32_t value, uint32_t i) {
    return mix(value ^ (i * 0x165667B1u));
}

typedef struct {
    uint32_t state;
} Rng;

static uint32_t rng_next(Rng* r) {
    uint32_t x = r->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->state = x;
    return x;
}

static uint32_t rng_below(Rng* r, uint32_t n) {
    return rng_next(r) % n;
}

typedef struct {
    uint32_t* model;  /* expected value of every pool word */
    uint8_t* claimed; /* generation that last claimed the word */
    uint32_t* cpu;    /* this generation's CPU-written words */
    uint32_t n_cpu;
    uint32_t* gpu;    /* this generation's GPU-written words */
    uint32_t n_gpu;
    unsigned shared_pages;
} Plan;

static void claim_run(Plan* p, uint32_t start, uint32_t len, unsigned g,
                      bool gpu) {
    for (uint32_t w = start; w < start + len; ++w) {
        if (p->claimed[w] == g) {
            /* A page picked twice in one generation: the first claim wins,
             * which keeps the lists disjoint; a rare collision only shortens
             * a run. */
            continue;
        }
        p->claimed[w] = (uint8_t)g;
        if (gpu) {
            p->gpu[p->n_gpu++] = w;
        } else {
            p->cpu[p->n_cpu++] = w;
        }
    }
}

static void build_plan(Plan* p, Rng* rng, unsigned g) {
    p->n_cpu = 0;
    p->n_gpu = 0;
    p->shared_pages = 0;
    for (unsigned k = 0; k < ACTIVE_PAGES; ++k) {
        const uint32_t base = rng_below(rng, POOL_PAGES) * PAGE_WORDS;
        const uint32_t role = rng_below(rng, 4);
        if (role < 2) {
            /* 0: CPU-only page, 1: GPU-only page */
            const uint32_t len = 1 + rng_below(rng, MAX_RUN);
            const uint32_t off = rng_below(rng, PAGE_WORDS - len + 1);
            claim_run(p, base + off, len, g, role == 1);
        } else {
            /* shared page: CPU run in one half, GPU run in the other */
            const uint32_t half = rng_below(rng, 2);
            const uint32_t half_words = PAGE_WORDS / 2;
            const uint32_t cpu_len = 1 + rng_below(rng, MAX_RUN);
            const uint32_t gpu_len = 1 + rng_below(rng, MAX_RUN);
            const uint32_t cpu_off =
                half * half_words + rng_below(rng, half_words - cpu_len + 1);
            const uint32_t gpu_off = (1 - half) * half_words +
                                     rng_below(rng, half_words - gpu_len + 1);
            claim_run(p, base + cpu_off, cpu_len, g, false);
            claim_run(p, base + gpu_off, gpu_len, g, true);
            p->shared_pages += 1;
        }
    }
}

typedef struct {
    const char* reason;
    const char* stage;
    unsigned generation;
    uint32_t word;
    uint32_t expected;
    uint32_t got;
    int got_generation;
} Failure;

static Failure g_first;
static unsigned g_failures;

/* Names the generation and side a wrong value came from, if any. `index` is
 * the out[] slot for GPU reads and UINT32_MAX for CPU reads. */
static const char* classify(uint32_t w, uint32_t got, uint32_t index,
                            unsigned upto, int* got_generation) {
    const bool via_out = index != UINT32_MAX;
#define AS_SEEN(v) (via_out ? read_result((v), index) : (v))
    if (via_out && got == OUT_POISON) {
        *got_generation = -1;
        return "out_unwritten";
    }
    if (got == AS_SEEN(init_value(w))) {
        *got_generation = 0;
        return "stale_init";
    }
    for (unsigned k = upto; k >= 1; --k) {
        if (got == AS_SEEN(cpu_value(w, k))) {
            *got_generation = (int)k;
            return "stale_cpu";
        }
        if (got == AS_SEEN(gpu_value(w, k))) {
            *got_generation = (int)k;
            return "stale_gpu";
        }
    }
#undef AS_SEEN
    *got_generation = -1;
    return "mismatch";
}

static void note_failure(const char* stage, unsigned g, uint32_t w,
                         uint32_t index, uint32_t expected, uint32_t got) {
    if (!g_first.reason) {
        int got_generation = -1;
        g_first.reason = classify(w, got, index, g, &got_generation);
        g_first.stage = stage;
        g_first.generation = g;
        g_first.word = w;
        g_first.expected = expected;
        g_first.got = got;
        g_first.got_generation = got_generation;
    }
    g_failures += 1;
}

/* Checks one pool word against the model; returns 1 if it is wrong. */
static unsigned check_word(const volatile uint32_t* pool, const Plan* p,
                           const char* stage, unsigned g, uint32_t w) {
    const uint32_t got = pool[w];
    if (got == p->model[w]) {
        return 0;
    }
    note_failure(stage, g, w, UINT32_MAX, p->model[w], got);
    return 1;
}

static unsigned check_sample(const volatile uint32_t* pool, const Plan* p,
                             Rng* rng, const char* stage, unsigned g) {
    unsigned bad = 0;
    for (unsigned s = 0; s < SAMPLE_COUNT; ++s) {
        bad += check_word(pool, p, stage, g, rng_below(rng, POOL_WORDS));
    }
    return bad;
}

static void write_cpu_runs(volatile uint32_t* pool, Plan* p, unsigned g) {
    for (uint32_t k = 0; k < p->n_cpu; ++k) {
        const uint32_t w = p->cpu[k];
        p->model[w] = cpu_value(w, g);
        pool[w] = p->model[w];
    }
}

typedef struct {
    off_t direct_memory;
    void* base;
} Pool;

static const int POOL_PROT =
    ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW |
    ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE;

/* Maps the pool's direct memory, at `fixed` if it is non-NULL. */
static bool map_pool(Pool* pool, void* fixed) {
    void* addr = fixed;
    const int res = sceKernelMapDirectMemory(
        &addr, POOL_BYTES, POOL_PROT, fixed ? MAP_FIXED_FLAG : 0,
        pool->direct_memory, ST_DIRECT_MEMORY_ALIGNMENT);
    if (res != 0) {
        printf("sceKernelMapDirectMemory failed: 0x%x\n", res);
        return false;
    }
    if (fixed && addr != fixed) {
        printf("fixed mapping landed at %p instead of %p\n", addr, fixed);
        return false;
    }
    pool->base = addr;
    return true;
}

static uint32_t groups_for(uint32_t invocations) {
    return (invocations + ST_LOCAL_SIZE_X - 1) / ST_LOCAL_SIZE_X;
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
    StComputeShader writer;
    StComputeShader reader;
    if (!st_load_compute_shader(&arena, "/app0/assets/writer.comp.sb",
                                &writer) ||
        !st_load_compute_shader(&arena, "/app0/assets/reader.comp.sb",
                                &reader)) {
        return st_fail("reason=shader_load");
    }

    Pool pool_map = {.direct_memory = -1};
    if (sceKernelAllocateDirectMemory(
            0, (off_t)sceKernelGetDirectMemorySize(), POOL_BYTES,
            ST_DIRECT_MEMORY_ALIGNMENT, ORBIS_KERNEL_WC_GARLIC,
            &pool_map.direct_memory) != 0 ||
        !map_pool(&pool_map, NULL)) {
        return st_fail("reason=pool_memory");
    }
    volatile uint32_t* const pool = (volatile uint32_t*)pool_map.base;

    uint32_t* widx = (uint32_t*)st_arena_alloc(
        &arena, MAX_WRITES * sizeof(uint32_t), ST_PAGE_BYTES);
    uint32_t* ridx = (uint32_t*)st_arena_alloc(
        &arena, READ_COUNT * sizeof(uint32_t), ST_PAGE_BYTES);
    volatile uint32_t* out = (volatile uint32_t*)st_arena_alloc(
        &arena, READ_COUNT * sizeof(uint32_t), ST_PAGE_BYTES);
    volatile uint32_t* params = (volatile uint32_t*)st_arena_alloc(
        &arena, PARAMS_WORDS * sizeof(uint32_t), ST_PAGE_BYTES);
    GnmBuffer* tables = (GnmBuffer*)st_arena_alloc(
        &arena, (WRITER_BINDINGS + READER_BINDINGS) * sizeof(GnmBuffer),
        ST_PAGE_BYTES);
    Plan plan = {
        .model = (uint32_t*)malloc(POOL_BYTES),
        .claimed = (uint8_t*)calloc(POOL_WORDS, 1),
        .cpu = (uint32_t*)malloc(MAX_WRITES * sizeof(uint32_t)),
        .gpu = (uint32_t*)malloc(MAX_WRITES * sizeof(uint32_t)),
    };
    if (!widx || !ridx || !out || !params || !tables || !plan.model ||
        !plan.claimed || !plan.cpu || !plan.gpu) {
        return st_fail("reason=memory");
    }

    GnmBuffer* writer_table = tables;
    GnmBuffer* reader_table = tables + WRITER_BINDINGS;
    writer_table[0] = st_raw_buffer(pool, POOL_BYTES);
    writer_table[1] = st_raw_buffer(widx, MAX_WRITES * sizeof(uint32_t));
    writer_table[2] = st_raw_buffer(params, PARAMS_WORDS * sizeof(uint32_t));
    reader_table[0] = st_raw_buffer(pool, POOL_BYTES);
    reader_table[1] = st_raw_buffer(ridx, READ_COUNT * sizeof(uint32_t));
    reader_table[2] = st_raw_buffer(out, READ_COUNT * sizeof(uint32_t));
    reader_table[3] = st_raw_buffer(params, PARAMS_WORDS * sizeof(uint32_t));

    for (uint32_t w = 0; w < POOL_WORDS; ++w) {
        plan.model[w] = init_value(w);
        pool[w] = plan.model[w];
    }

    Rng rng = {.state = 0x2545F491u};
    unsigned failed_generations = 0;
    unsigned remaps = 0;
    for (unsigned g = 1; g <= GENERATIONS; ++g) {
        build_plan(&plan, &rng, g);

        /* 2. CPU and GPU runs; odd generations write from the CPU first. */
        const bool cpu_first = g % 2 == 1;
        if (cpu_first) {
            write_cpu_runs(pool, &plan, g);
        }
        for (uint32_t k = 0; k < plan.n_gpu; ++k) {
            widx[k] = plan.gpu[k];
        }
        params[0] = g;
        params[1] = READ_COUNT;
        params[2] = plan.n_gpu;
        if (plan.n_gpu &&
            !st_dispatch_and_wait(&queue, &writer, writer_table,
                                  groups_for(plan.n_gpu))) {
            return st_fail("reason=dispatch stage=writer gen=%u", g);
        }
        for (uint32_t k = 0; k < plan.n_gpu; ++k) {
            const uint32_t w = plan.gpu[k];
            plan.model[w] = gpu_value(w, g);
        }
        /* 5. Lifetime transition with the contents kept, while the GPU's
         * writes are still pending. The address stays the same, so `pool`
         * and the descriptor tables stay valid. */
        const bool remap = g % REMAP_EVERY == 0 && g < GENERATIONS;
        if (remap) {
            void* const base = pool_map.base;
            if (sceKernelMunmap(base, POOL_BYTES) != 0 ||
                !map_pool(&pool_map, base)) {
                return st_fail("reason=remap gen=%u", g);
            }
            remaps += 1;
        }
        if (!cpu_first) {
            /* Nothing in the pool has been read since the GPU runs. */
            write_cpu_runs(pool, &plan, g);
        }

        /* 3. GPU reads: this generation's CPU words, its GPU words, and
         * random words. */
        for (uint32_t i = 0; i < READ_COUNT; ++i) {
            const uint32_t third = i * 3 / READ_COUNT;
            uint32_t w;
            if (third == 0 && plan.n_cpu) {
                w = plan.cpu[rng_below(&rng, plan.n_cpu)];
            } else if (third == 1 && plan.n_gpu) {
                w = plan.gpu[rng_below(&rng, plan.n_gpu)];
            } else {
                w = rng_below(&rng, POOL_WORDS);
            }
            ridx[i] = w;
        }
        st_fill_words(out, READ_COUNT, OUT_POISON);
        if (!st_dispatch_and_wait(&queue, &reader, reader_table,
                                  groups_for(READ_COUNT))) {
            return st_fail("reason=dispatch stage=reader gen=%u", g);
        }

        /* 4. Checks: what the GPU read, then what the CPU sees. */
        const unsigned before = g_failures;
        unsigned bad_out = 0;
        for (uint32_t i = 0; i < READ_COUNT; ++i) {
            const uint32_t w = ridx[i];
            const uint32_t expected = read_result(plan.model[w], i);
            if (out[i] != expected) {
                note_failure("gpu_read", g, w, i, expected, out[i]);
                bad_out += 1;
            }
        }
        unsigned bad_gpu = 0;
        for (uint32_t k = 0; k < plan.n_gpu; ++k) {
            bad_gpu += check_word(pool, &plan, "gpu_words", g, plan.gpu[k]);
        }
        unsigned bad_cpu = 0;
        for (uint32_t k = 0; k < plan.n_cpu; ++k) {
            bad_cpu += check_word(pool, &plan, "cpu_words", g, plan.cpu[k]);
        }
        const unsigned bad_sample =
            check_sample(pool, &plan, &rng, "sample", g);

        unsigned bad_remap = 0;
        if (remap) {
            bad_remap = check_sample(pool, &plan, &rng, "after_remap", g);
        }

        const bool ok = g_failures == before;
        printf("gen %2u: cpu_words=%u gpu_words=%u shared_pages=%u%s: %s "
               "bad_out=%u bad_gpu=%u bad_cpu=%u bad_sample=%u "
               "bad_remap=%u\n",
               g, plan.n_cpu, plan.n_gpu, plan.shared_pages,
               remap ? " remap" : "", ok ? "ok" : "FAIL", bad_out, bad_gpu,
               bad_cpu, bad_sample, bad_remap);
        if (!ok) {
            failed_generations += 1;
        }
    }

    /* Final: every word, then the digest of what the CPU sees. */
    unsigned bad_final = 0;
    uint32_t digest = 0x811c9dc5u;
    for (uint32_t w = 0; w < POOL_WORDS; ++w) {
        bad_final += check_word(pool, &plan, "final", GENERATIONS, w);
        uint32_t v = pool[w];
        for (int byte = 0; byte < 4; ++byte) {
            digest ^= v & 0xffu;
            digest *= 0x01000193u;
            v >>= 8;
        }
    }
    printf("final: %s bad_final=%u\n", bad_final ? "FAIL" : "ok", bad_final);

    if (st_gnm_errors != 0) {
        return st_fail("reason=gnm_error count=%u", st_gnm_errors);
    }
    if (g_first.reason) {
        return st_fail(
            "reason=%s stage=%s gen=%u page=%u word=%u expected=%08x "
            "got=%08x got_gen=%d failed_generations=%u bad_final=%u "
            "bad_words=%u",
            g_first.reason, g_first.stage, g_first.generation,
            g_first.word / PAGE_WORDS, g_first.word % PAGE_WORDS,
            g_first.expected, g_first.got, g_first.got_generation,
            failed_generations, bad_final, g_failures);
    }

    st_emit_marker("PASS",
                   "generations=%u pages=%u remaps=%u digest=%08x",
                   (unsigned)GENERATIONS, (unsigned)POOL_PAGES, remaps,
                   digest);

    sceKernelMunmap(pool_map.base, POOL_BYTES);
    sceKernelReleaseDirectMemory(pool_map.direct_memory, POOL_BYTES);
    free(plan.gpu);
    free(plan.cpu);
    free(plan.claimed);
    free(plan.model);
    st_unload_compute_shader(&reader);
    st_unload_compute_shader(&writer);
    st_arena_destroy(&arena);
    return 0;
}
