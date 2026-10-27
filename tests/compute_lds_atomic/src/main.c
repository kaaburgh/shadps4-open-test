/*
 * compute_lds_atomic: workgroup shared memory (LDS) and buffer atomics in one
 * GFX7 compute dispatch.
 *
 * lds_atomic.comp runs 64 workgroups of 256 invocations. Each invocation
 * writes its source word to LDS, waits on the barrier and reads back the word
 * of the invocation at the mirrored position of its workgroup (255 - local
 * index), which on GCN belongs to another wave. It writes that word v, mixed
 * with its own index, to dst and adds to two sets of buffer counters:
 *
 *   counters[v & 63]                          += 1   histogram of v
 *   counters[64 + workgroup * 4 + (lid & 3)]  += v   partial workgroup sums
 *
 * Every expected word is computed on the CPU. The counters start from
 * non-zero values, so an atomic that stores instead of adding fails too, and
 * the sums wrap modulo 2^32 like the GPU's.
 *
 * Each atomic address differs between lanes. One sum per workgroup would
 * give the whole wave one address, which the compiler turns into a
 * whole-wave reduction (s_or_saveexec_b64 ..., -1, unimplemented in shadPS4
 * at the time of writing) and one lane's atomic.
 *
 * Before dispatching, the test checks that the shader binary allocates the
 * LDS it uses: lds[256] of uint is 1 KiB, COMPUTE_PGM_RSRC2.LDS_SIZE = 2
 * (512-byte blocks). shadPS4 sizes the shader's shared memory from that
 * field. opengnm-psbc before kaaburgh/opengnm-psbc d545621 aborted on any
 * shader with shared memory instead of producing a binary.
 *
 * Layout (one block, page aligned): guard src guard dst guard counters guard,
 * with 4 KiB guards and 64 KiB src/dst.
 */

#define SHADTEST_NAME "compute_lds_atomic"
#include "shadtest_guest.h"

enum {
    WORKGROUP_SIZE = 256,
    GROUPS_X = 64,
    WORD_COUNT = WORKGROUP_SIZE * GROUPS_X,
    BUFFER_BYTES = WORD_COUNT * sizeof(uint32_t),
    BUCKETS = 64,
    SUM_SLOTS = 4,
    SUM_WORDS = GROUPS_X * SUM_SLOTS,
    COUNTER_WORDS = BUCKETS + SUM_WORDS,
    COUNTER_BYTES = COUNTER_WORDS * sizeof(uint32_t),
    GUARD_BYTES = ST_PAGE_BYTES,
    GUARD_WORDS = GUARD_BYTES / sizeof(uint32_t),
    GUARDS = 4,
    BLOCK_BYTES = GUARDS * GUARD_BYTES + 2 * BUFFER_BYTES + COUNTER_BYTES,
    /* COMPUTE_PGM_RSRC2.LDS_SIZE counts 128-dword (512-byte) blocks on GFX7. */
    LDS_BLOCKS = WORKGROUP_SIZE * sizeof(uint32_t) / 512,
    BINDINGS = 3,
    ARENA_BYTES = 2 * 1024 * 1024,
};

_Static_assert((BUCKETS & (BUCKETS - 1)) == 0, "buckets are v & (BUCKETS - 1)");
_Static_assert((SUM_SLOTS & (SUM_SLOTS - 1)) == 0,
               "sum slots are lid & (SUM_SLOTS - 1)");
_Static_assert(WORKGROUP_SIZE * sizeof(uint32_t) % 512 == 0,
               "the shared array is a whole number of LDS blocks");

static const uint32_t GUARD_VALUE = 0x5a17c3e9u;
static const uint32_t OUTPUT_POISON = 0xcdcdcdcdu;

static uint32_t input_value(uint32_t i) {
    uint32_t x = i * 0x9E3779B1u + 0x7F4A7C15u;
    x ^= x >> 15;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    return x;
}

/* Must match lds_atomic.comp. */
static uint32_t mix(uint32_t v, uint32_t i) {
    return v ^ (i * 0x2545F491u);
}

/* The index whose source word invocation i reads back from LDS. */
static uint32_t exchanged_index(uint32_t i) {
    const uint32_t lid = i % WORKGROUP_SIZE;
    return i - lid + (WORKGROUP_SIZE - 1 - lid);
}

/* The partial-sum slot invocation i adds to, relative to the first sum. */
static uint32_t sum_slot(uint32_t i) {
    return (i / WORKGROUP_SIZE) * SUM_SLOTS +
           (i % WORKGROUP_SIZE) % SUM_SLOTS;
}

static uint32_t counter_initial(uint32_t k) {
    return 0x10000000u + k * 0x00010001u;
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
    if (!st_load_compute_shader(&arena, "/app0/assets/lds_atomic.comp.sb",
                                &shader)) {
        return st_fail("reason=shader_load");
    }

    /* A binary without the LDS allocation would run with no shared memory;
     * report that instead of dispatching it. */
    const uint32_t rsrc2 = shader.shader->registers.computepgmrsrc2;
    if (G_00B84C_LDS_SIZE(rsrc2) != LDS_BLOCKS) {
        return st_fail("reason=lds_size got=%u expected=%u rsrc2=%08x",
                       G_00B84C_LDS_SIZE(rsrc2), (unsigned)LDS_BLOCKS, rsrc2);
    }

    GnmBuffer* table = (GnmBuffer*)st_arena_alloc(
        &arena, BINDINGS * sizeof(GnmBuffer), 16);
    uint8_t* block =
        (uint8_t*)st_arena_alloc(&arena, BLOCK_BYTES, ST_PAGE_BYTES);
    if (!table || !block) {
        return st_fail("reason=buffer_memory");
    }

    volatile uint32_t* guards[GUARDS];
    uint8_t* cursor = block;
    guards[0] = (volatile uint32_t*)cursor;
    cursor += GUARD_BYTES;
    volatile uint32_t* src = (volatile uint32_t*)cursor;
    cursor += BUFFER_BYTES;
    guards[1] = (volatile uint32_t*)cursor;
    cursor += GUARD_BYTES;
    volatile uint32_t* dst = (volatile uint32_t*)cursor;
    cursor += BUFFER_BYTES;
    guards[2] = (volatile uint32_t*)cursor;
    cursor += GUARD_BYTES;
    volatile uint32_t* counters = (volatile uint32_t*)cursor;
    cursor += COUNTER_BYTES;
    guards[3] = (volatile uint32_t*)cursor;

    for (unsigned g = 0; g < GUARDS; ++g) {
        st_fill_words(guards[g], GUARD_WORDS, GUARD_VALUE);
    }
    for (uint32_t i = 0; i < WORD_COUNT; ++i) {
        src[i] = input_value(i);
    }
    st_fill_words(dst, WORD_COUNT, OUTPUT_POISON);
    for (uint32_t k = 0; k < COUNTER_WORDS; ++k) {
        counters[k] = counter_initial(k);
    }

    table[0] = st_raw_buffer(src, BUFFER_BYTES);
    table[1] = st_raw_buffer(dst, BUFFER_BYTES);
    table[2] = st_raw_buffer(counters, COUNTER_BYTES);

    if (!st_dispatch_and_wait(&queue, &shader, table, GROUPS_X)) {
        return st_fail("reason=dispatch");
    }

    StCase cases[] = {
        {.name = "lds_exchange"},
        {.name = "atomic_histogram"},
        {.name = "atomic_group_sum"},
        {.name = "src_unchanged"},
        {.name = "guards"},
    };
    StCase* exchange = &cases[0];
    StCase* histogram = &cases[1];
    StCase* group_sum = &cases[2];
    StCase* src_case = &cases[3];
    StCase* guard_case = &cases[4];

    uint32_t expected_histogram[BUCKETS];
    uint32_t expected_sum[SUM_WORDS];
    for (uint32_t b = 0; b < BUCKETS; ++b) {
        expected_histogram[b] = counter_initial(b);
    }
    for (uint32_t s = 0; s < SUM_WORDS; ++s) {
        expected_sum[s] = counter_initial(BUCKETS + s);
    }

    uint32_t checksum = 0x811c9dc5u;
    for (uint32_t i = 0; i < WORD_COUNT; ++i) {
        const uint32_t v = input_value(exchanged_index(i));
        expected_histogram[v & (BUCKETS - 1)] += 1;
        expected_sum[sum_slot(i)] += v;

        const uint32_t expected = mix(v, i);
        const uint32_t got = dst[i];
        checksum = (checksum ^ got) * 0x01000193u;
        if (got == expected) {
            continue;
        }

        /* Name the likely failure: the word was not written, the LDS read
         * returned the invocation's own word (no exchange) or zero. */
        const char* reason = "dst_mismatch";
        if (got == OUTPUT_POISON) {
            reason = "dst_unwritten";
        } else if (got == mix(input_value(i), i)) {
            reason = "lds_own_word";
        } else if (got == mix(0, i)) {
            reason = "lds_zero";
        }
        st_case_note(exchange, reason, i, expected, got);
    }

    for (uint32_t b = 0; b < BUCKETS; ++b) {
        const uint32_t got = counters[b];
        if (got != expected_histogram[b]) {
            st_case_note(histogram,
                         got == counter_initial(b) ? "counter_untouched"
                                                   : "histogram_mismatch",
                         b, expected_histogram[b], got);
        }
    }
    for (uint32_t s = 0; s < SUM_WORDS; ++s) {
        const uint32_t k = BUCKETS + s;
        const uint32_t got = counters[k];
        if (got != expected_sum[s]) {
            st_case_note(group_sum,
                         got == counter_initial(k) ? "counter_untouched"
                                                   : "group_sum_mismatch",
                         k, expected_sum[s], got);
        }
    }

    for (uint32_t i = 0; i < WORD_COUNT; ++i) {
        st_case_expect(src_case, "src_changed", i, input_value(i), src[i]);
    }

    /* For guard failures the reported index is the word offset in the
     * guard area (guard number * GUARD_WORDS + word). */
    for (unsigned g = 0; g < GUARDS; ++g) {
        for (uint32_t w = 0; w < GUARD_WORDS; ++w) {
            st_case_expect(guard_case, "guard", g * GUARD_WORDS + w,
                           GUARD_VALUE, guards[g][w]);
        }
    }

    char detail[128];
    snprintf(detail, sizeof(detail),
             "groups=%u workgroup=%u lds_blocks=%u dst_checksum=%08x",
             (unsigned)GROUPS_X, (unsigned)WORKGROUP_SIZE,
             (unsigned)LDS_BLOCKS, checksum);
    const int result =
        st_finish_cases(cases, sizeof(cases) / sizeof(cases[0]), detail);

    st_unload_compute_shader(&shader);
    st_arena_destroy(&arena);
    return result;
}
