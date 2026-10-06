/*
 * buffer_cpu_rewrite: Stage 2 of docs/memory-uma-test-roadmap.md.
 *
 * The GPU reads a buffer, the CPU then rewrites whole pages of it, and the
 * GPU reads it again. The first GPU use can leave host-side cached/mirrored
 * state behind; a later CPU write has to invalidate or update it before the
 * next GPU access, every time, not just the first time.
 *
 *   pass 1: CPU writes generation 0 to all of X;  GPU: Y1 = transform(X)
 *   round 1: CPU rewrites pages {1, 6, 7, 15} of X to generation 1
 *   pass 2: GPU: Y2 = transform(X)
 *   round 2: CPU rewrites pages {0, 6, 9, 15} of X to generation 2
 *   pass 3: GPU: Y3 = transform(X)
 *
 * Pages 6 and 15 are rewritten in both rounds, so a host that stops tracking
 * a page after its first re-upload hands the GPU generation 1 in pass 3.
 * Each pass writes its own poisoned output buffer, so pages the CPU did not
 * rewrite still prove that the pass ran.
 *
 * After each pass the CPU checks the pass's output word by word against the
 * generation each page should have had, X against what the CPU wrote, and
 * every guard. A wrong output word is classified by the generation it was
 * actually computed from.
 *
 * Layout (one block, page aligned): guard X guard Y1 guard Y2 guard Y3 guard,
 * with 4 KiB guards and 64 KiB buffers.
 *
 * Passes 2 and 3 re-read X with no GPU cache invalidate in between. On a
 * PS4 that relies on the CACHE_COHERENT buffer type for CPU-written Garlic
 * memory, as the shared helpers do; it is validated on shadPS4 only.
 */

#define SHADTEST_NAME "buffer_cpu_rewrite"
#include "shadtest_guest.h"

enum {
    PAGE_WORDS = ST_PAGE_BYTES / sizeof(uint32_t),
    BUFFER_PAGES = 16,
    BUFFER_WORDS = BUFFER_PAGES * PAGE_WORDS,
    BUFFER_BYTES = BUFFER_WORDS * sizeof(uint32_t),
    GUARD_BYTES = ST_PAGE_BYTES,
    GUARD_WORDS = GUARD_BYTES / sizeof(uint32_t),
    PASSES = 3,
    GENERATIONS = PASSES,
    GROUPS_X = BUFFER_WORDS / ST_LOCAL_SIZE_X,
    BINDINGS = 2,
    /* guard X guard Y1 guard Y2 guard Y3 guard */
    BLOCK_BYTES = (PASSES + 2) * GUARD_BYTES + (PASSES + 1) * BUFFER_BYTES,
    ARENA_BYTES = 2 * 1024 * 1024,
};

_Static_assert(BUFFER_WORDS % ST_LOCAL_SIZE_X == 0,
               "buffers must be whole workgroups");

/* Pages the CPU rewrites before pass 2 and pass 3. */
static const uint8_t ROUND1_PAGES[] = {1, 6, 7, 15};
static const uint8_t ROUND2_PAGES[] = {0, 6, 9, 15};

static const uint32_t GUARD_VALUE = 0x5a17c3e9u;
static const uint32_t OUTPUT_POISON = 0xcdcdcdcdu;

/* Value of word i of X in a generation. Generations differ in every word. */
static uint32_t pattern(unsigned generation, uint32_t i) {
    return (i * 0x9E3779B1u) ^ (generation * 0x7F4A7C15u) ^ 0x0BADF00Du;
}

/* Must match transform.comp. */
static uint32_t transform(uint32_t v, uint32_t i) {
    return ((v << 5) | (v >> 27)) ^ (i * 0x2545F491u) ^ 0x6A09E667u;
}

typedef struct {
    const char* reason;
    unsigned pass;
    uint32_t index;
    uint32_t expected;
    uint32_t got;
    int got_generation;
} Failure;

typedef struct {
    Failure first;
    unsigned bad_output;
    unsigned bad_input;
    unsigned bad_guard;
    uint32_t bad_output_page_mask;
} PassResult;

static void note_failure(Failure* f, const char* reason, unsigned pass,
                         uint32_t index, uint32_t expected, uint32_t got,
                         int got_generation) {
    if (!f->reason) {
        f->reason = reason;
        f->pass = pass;
        f->index = index;
        f->expected = expected;
        f->got = got;
        f->got_generation = got_generation;
    }
}

static void rewrite_pages(volatile uint32_t* x, uint8_t* page_generation,
                          const uint8_t* pages, size_t count,
                          unsigned generation) {
    for (size_t k = 0; k < count; ++k) {
        const uint32_t first = (uint32_t)pages[k] * PAGE_WORDS;
        for (uint32_t i = first; i < first + PAGE_WORDS; ++i) {
            x[i] = pattern(generation, i);
        }
        page_generation[pages[k]] = (uint8_t)generation;
    }
}

/* For guard failures the reported index is the word offset in the block. */
static void check_guard(const volatile uint32_t* guard, unsigned guard_number,
                        unsigned pass, PassResult* result) {
    const uint32_t block_word = guard_number * (GUARD_WORDS + BUFFER_WORDS);
    for (uint32_t i = 0; i < GUARD_WORDS; ++i) {
        if (guard[i] != GUARD_VALUE) {
            note_failure(&result->first, "guard", pass, block_word + i,
                         GUARD_VALUE, guard[i], -1);
            result->bad_guard += 1;
        }
    }
}

static void verify_pass(unsigned pass, const volatile uint32_t* x,
                        const volatile uint32_t* y,
                        const volatile uint32_t* const* guards,
                        const uint8_t* page_generation, PassResult* result) {
    for (uint32_t i = 0; i < BUFFER_WORDS; ++i) {
        const unsigned page = i / PAGE_WORDS;
        const unsigned generation = page_generation[page];
        const uint32_t expected = transform(pattern(generation, i), i);
        const uint32_t got = y[i];
        if (got == expected) {
            continue;
        }

        const char* reason = "output_mismatch";
        int got_generation = -1;
        if (got == OUTPUT_POISON) {
            reason = "output_unwritten";
        } else {
            for (unsigned g = 0; g < GENERATIONS; ++g) {
                if (got == transform(pattern(g, i), i)) {
                    reason = "stale_generation";
                    got_generation = (int)g;
                    break;
                }
            }
        }
        note_failure(&result->first, reason, pass, i, expected, got,
                     got_generation);
        result->bad_output += 1;
        result->bad_output_page_mask |= 1u << page;
    }

    for (uint32_t i = 0; i < BUFFER_WORDS; ++i) {
        const unsigned generation = page_generation[i / PAGE_WORDS];
        const uint32_t expected = pattern(generation, i);
        const uint32_t got = x[i];
        if (got == expected) {
            continue;
        }
        int got_generation = -1;
        for (unsigned g = 0; g < GENERATIONS; ++g) {
            if (got == pattern(g, i)) {
                got_generation = (int)g;
                break;
            }
        }
        note_failure(&result->first, "input_changed", pass, i, expected, got,
                     got_generation);
        result->bad_input += 1;
    }

    for (unsigned g = 0; g < PASSES + 2; ++g) {
        check_guard(guards[g], g, pass, result);
    }
}

static uint32_t fnv1a_words(uint32_t hash, const volatile uint32_t* words,
                            size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t v = words[i];
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= v & 0xffu;
            hash *= 0x01000193u;
            v >>= 8;
        }
    }
    return hash;
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
        &arena, PASSES * BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    uint8_t* block =
        (uint8_t*)st_arena_alloc(&arena, BLOCK_BYTES, ST_PAGE_BYTES);
    if (!tables || !block) {
        return st_fail("reason=buffer_memory");
    }

    const volatile uint32_t* guards[PASSES + 2];
    volatile uint32_t* x = NULL;
    volatile uint32_t* y[PASSES];
    uint8_t* cursor = block;
    for (unsigned g = 0; g < PASSES + 2; ++g) {
        volatile uint32_t* guard = (volatile uint32_t*)cursor;
        st_fill_words(guard, GUARD_WORDS, GUARD_VALUE);
        guards[g] = guard;
        cursor += GUARD_BYTES;
        if (g == PASSES + 1) {
            break;
        }

        volatile uint32_t* buffer = (volatile uint32_t*)cursor;
        if (g == 0) {
            x = buffer;
        } else {
            y[g - 1] = buffer;
            st_fill_words(buffer, BUFFER_WORDS, OUTPUT_POISON);
        }
        cursor += BUFFER_BYTES;
    }

    uint8_t page_generation[BUFFER_PAGES];
    memset(page_generation, 0, sizeof(page_generation));
    for (uint32_t i = 0; i < BUFFER_WORDS; ++i) {
        x[i] = pattern(0, i);
    }

    for (unsigned p = 0; p < PASSES; ++p) {
        tables[p * BINDINGS + 0] = st_raw_buffer(x, BUFFER_BYTES);
        tables[p * BINDINGS + 1] = st_raw_buffer(y[p], BUFFER_BYTES);
    }

    unsigned failed_passes = 0;
    Failure first = {0};
    uint32_t checksum = 0x811c9dc5u;

    for (unsigned p = 0; p < PASSES; ++p) {
        const unsigned pass = p + 1;
        if (pass == 2) {
            rewrite_pages(x, page_generation, ROUND1_PAGES,
                          sizeof(ROUND1_PAGES), 1);
        } else if (pass == 3) {
            rewrite_pages(x, page_generation, ROUND2_PAGES,
                          sizeof(ROUND2_PAGES), 2);
        }

        if (!st_dispatch_and_wait(&queue, &shader, &tables[p * BINDINGS],
                                  GROUPS_X)) {
            return st_fail("reason=dispatch pass=%u", pass);
        }

        PassResult result = {0};
        verify_pass(pass, x, y[p], guards, page_generation, &result);

        char generations[BUFFER_PAGES + 1];
        for (unsigned page = 0; page < BUFFER_PAGES; ++page) {
            generations[page] = (char)('0' + page_generation[page]);
        }
        generations[BUFFER_PAGES] = '\0';

        if (result.first.reason) {
            printf(
                "pass %u page_generations=%s: FAIL bad_output=%u "
                "bad_input=%u bad_guard=%u bad_output_pages=%04x first %s "
                "index=%u expected=%08x got=%08x got_generation=%d\n",
                pass, generations, result.bad_output, result.bad_input,
                result.bad_guard, result.bad_output_page_mask,
                result.first.reason, result.first.index, result.first.expected,
                result.first.got, result.first.got_generation);
            if (!first.reason) {
                first = result.first;
            }
            failed_passes += 1;
        } else {
            printf("pass %u page_generations=%s: ok\n", pass, generations);
            checksum = fnv1a_words(checksum, y[p], BUFFER_WORDS);
        }
    }

    if (st_gnm_errors != 0) {
        return st_fail("reason=gnm_error count=%u", st_gnm_errors);
    }

    if (first.reason) {
        return st_fail(
            "reason=%s pass=%u index=%u expected=%08x got=%08x "
            "got_generation=%d failed_passes=%u",
            first.reason, first.pass, first.index, first.expected, first.got,
            first.got_generation, failed_passes);
    }

    st_emit_marker("PASS", "passes=%u pages=%u rewritten=%u+%u checksum=%08x",
                   (unsigned)PASSES, (unsigned)BUFFER_PAGES,
                   (unsigned)sizeof(ROUND1_PAGES),
                   (unsigned)sizeof(ROUND2_PAGES), checksum);

    st_unload_compute_shader(&shader);
    st_arena_destroy(&arena);
    return 0;
}
