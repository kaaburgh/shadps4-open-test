/*
 * dma_alias_order: scenario B1 of docs/uma-e1c-e3-scenario-plan.md.
 *
 * A DMA through VA B must be ordered against a dispatch that wrote the same
 * physical page P through VA A. In one DCB:
 *
 *   dispatch writes pattern 1 over P through A
 *   [B]   (the dispatch has finished; no dirty L2 line can later overwrite
 *          the DMA's write)
 *   fill: DMA fill over all of B with FILL   |   copy: DMA copy B -> D
 *   EOP
 *
 * Mappings (each case gets its own physical pages):
 *   contiguous     P mapped at A and at B (16 KiB each). B can be shared.
 *   noncontiguous  B is two adjacent 16 KiB virtual pages: B[0] maps P, as A
 *                  does, B[1] maps Q, which is not physically adjacent to P.
 *                  The DMA spans both, so shadPS4 cannot serve B from the
 *                  shared backing (a single-page alias would always be
 *                  contiguous with 16 KiB BufferCache blocks).
 *   single         control: B is A.
 *
 * Oracles:
 *   fill  A and all of B read FILL.
 *   copy  D holds pattern 1 for B[0] and Q's initial contents for B[1].
 *
 * shadPS4 tracks buffers per guest VA. With the mirror, the dispatch's write
 * through A lives in A's mirror while the DMA uses B (kaaburgh/shadPS4#5).
 * With shared backing, a shareable B goes to the GPU timeline (since
 * a393f8d); a B that cannot be shared is accessed while recording, before
 * the dispatch has run (kaaburgh/shadPS4#8): the fill takes the CPU fast
 * path, and the copy uploads B's mirror from guest memory. Only the copy
 * goes through BufferCache::ObtainBuffer, so only it logs a fallback.
 */

#define SHADTEST_NAME "dma_alias_order"
#include "shadtest_guest.h"

enum {
    ARENA_BYTES = 2 * 1024 * 1024,
    SLOT_BYTES = 64 * 1024,
    PAGE_WORDS = ST_DMEM_PAGE_BYTES / sizeof(uint32_t),
    /* Physical pages for the cases: P at the window start, Q 64 KiB later,
     * one 256 KiB window per case, far above the arena. */
    WINDOW_BASE = 16 * 1024 * 1024,
    WINDOW_BYTES = 256 * 1024,
    Q_OFFSET = 64 * 1024,
    BINDINGS = 2,
};

_Static_assert(PAGE_WORDS % ST_LOCAL_SIZE_X == 0, "whole workgroups");

typedef enum { MAP_CONTIGUOUS, MAP_NONCONTIGUOUS, MAP_SINGLE } Mapping;
typedef enum { OP_FILL, OP_COPY } Op;

enum {
    CASE_CONTIGUOUS_FILL,
    CASE_CONTIGUOUS_COPY,
    CASE_NONCONTIGUOUS_FILL,
    CASE_NONCONTIGUOUS_COPY,
    CASE_SINGLE_FILL,
    CASE_SINGLE_COPY,
    CASES,
};

static const uint32_t FILL = 0x22222222u;
static const uint32_t OUTPUT_POISON = 0xcdcdcdcdu;

static uint32_t pattern(uint32_t i) {
    return (i * 0x9E3779B1u) ^ 0x31313131u;
}
static uint32_t p_initial(uint32_t i) {
    return (i * 0x165667B1u) ^ 0x50505050u;
}
static uint32_t q_initial(uint32_t i) {
    return (i * 0x27D4EB2Fu) ^ 0x51515151u;
}

typedef struct {
    volatile uint32_t* a;
    volatile uint32_t* b;
    uint32_t b_words;
} Views;

static bool map_case(unsigned index, Mapping mapping, Views* v) {
    const off_t window = WINDOW_BASE + (off_t)index * WINDOW_BYTES;
    off_t phys[2];
    if (!st_dmem_alloc_in(window, window + Q_OFFSET, ST_DMEM_PAGE_BYTES,
                          ST_DMEM_PAGE_BYTES, &phys[0])) {
        return false;
    }
    void* a = NULL;
    if (!st_dmem_map(&a, ST_DMEM_PAGE_BYTES, phys[0], false)) {
        return false;
    }
    v->a = (volatile uint32_t*)a;

    if (mapping == MAP_SINGLE) {
        v->b = v->a;
        v->b_words = PAGE_WORDS;
    } else if (mapping == MAP_CONTIGUOUS) {
        void* b = NULL;
        if (!st_dmem_map(&b, ST_DMEM_PAGE_BYTES, phys[0], false)) {
            return false;
        }
        v->b = (volatile uint32_t*)b;
        v->b_words = PAGE_WORDS;
    } else {
        if (!st_dmem_alloc_in(window + Q_OFFSET, window + WINDOW_BYTES,
                              ST_DMEM_PAGE_BYTES, ST_DMEM_PAGE_BYTES,
                              &phys[1])) {
            return false;
        }
        uint8_t* b = NULL;
        if (!st_map_pages(phys, 2, &b)) {
            return false;
        }
        v->b = (volatile uint32_t*)b;
        v->b_words = 2 * PAGE_WORDS;
    }
    printf("case %u: P 0x%llx A %p B %p (%u words)\n", index,
           (unsigned long long)phys[0], (void*)v->a, (void*)v->b, v->b_words);
    return true;
}

/* B's expected contents before the dispatch: P through B[0], Q after it. */
static uint32_t b_initial(uint32_t i) {
    return i < PAGE_WORDS ? p_initial(i) : q_initial(i - PAGE_WORDS);
}

static void check_fill(StCase* c, const Views* v) {
    for (uint32_t i = 0; i < PAGE_WORDS; ++i) {
        const uint32_t got = v->a[i];
        if (got != FILL) {
            st_case_note(c, got == pattern(i) ? "a_dispatch_after_fill" : "a",
                         i, FILL, got);
        }
    }
    for (uint32_t i = 0; i < v->b_words; ++i) {
        const uint32_t got = v->b[i];
        if (got != FILL) {
            st_case_note(c, got == pattern(i) ? "b_dispatch_after_fill" : "b",
                         PAGE_WORDS + i, FILL, got);
        }
    }
}

static void check_copy(StCase* c, const Views* v, const volatile uint32_t* d) {
    for (uint32_t i = 0; i < v->b_words; ++i) {
        const uint32_t expected = i < PAGE_WORDS ? pattern(i) : b_initial(i);
        const uint32_t got = d[i];
        if (got == expected) {
            continue;
        }
        const char* reason = "mismatch";
        if (got == OUTPUT_POISON) {
            reason = "unwritten";
        } else if (i < PAGE_WORDS && got == p_initial(i)) {
            reason = "copied_before_dispatch";
        }
        st_case_note(c, reason, i, expected, got);
    }
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

    StComputeShader copy;
    if (!st_load_compute_shader(&arena, "/app0/assets/copy.comp.sb", &copy)) {
        return st_fail("reason=shader_load");
    }

    const int alias_res = sceKernelEnableDmemAliasing();
    if (alias_res != 0) {
        return st_fail("reason=enable_dmem_aliasing res=0x%x", alias_res);
    }

    GnmBuffer* tables = (GnmBuffer*)st_arena_alloc(
        &arena, CASES * BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    volatile uint32_t* src =
        (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
    if (!tables || !src) {
        return st_fail("reason=arena_memory");
    }
    for (uint32_t i = 0; i < PAGE_WORDS; ++i) {
        src[i] = pattern(i);
    }

    StCase cases[CASES] = {
        [CASE_CONTIGUOUS_FILL] = {.name = "contiguous_fill"},
        [CASE_CONTIGUOUS_COPY] = {.name = "contiguous_copy"},
        [CASE_NONCONTIGUOUS_FILL] = {.name = "noncontiguous_fill"},
        [CASE_NONCONTIGUOUS_COPY] = {.name = "noncontiguous_copy"},
        [CASE_SINGLE_FILL] = {.name = "single_fill"},
        [CASE_SINGLE_COPY] = {.name = "single_copy"},
    };
    const struct {
        Mapping mapping;
        Op op;
    } runs[CASES] = {
        [CASE_CONTIGUOUS_FILL] = {MAP_CONTIGUOUS, OP_FILL},
        [CASE_CONTIGUOUS_COPY] = {MAP_CONTIGUOUS, OP_COPY},
        [CASE_NONCONTIGUOUS_FILL] = {MAP_NONCONTIGUOUS, OP_FILL},
        [CASE_NONCONTIGUOUS_COPY] = {MAP_NONCONTIGUOUS, OP_COPY},
        [CASE_SINGLE_FILL] = {MAP_SINGLE, OP_FILL},
        [CASE_SINGLE_COPY] = {MAP_SINGLE, OP_COPY},
    };

    for (unsigned k = 0; k < CASES; ++k) {
        Views v;
        if (!map_case(k, runs[k].mapping, &v)) {
            return st_fail("reason=placement case=%s", cases[k].name);
        }
        for (uint32_t i = 0; i < v.b_words; ++i) {
            v.b[i] = b_initial(i);
        }
        volatile uint32_t* d = NULL;
        if (runs[k].op == OP_COPY) {
            d = (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES,
                                                   SLOT_BYTES);
            if (!d) {
                return st_fail("reason=arena_memory");
            }
            st_fill_words(d, v.b_words, OUTPUT_POISON);
        }

        GnmBuffer* table = &tables[k * BINDINGS];
        table[0] = st_raw_buffer(src, ST_DMEM_PAGE_BYTES);
        table[1] = st_raw_buffer(v.a, ST_DMEM_PAGE_BYTES);

        StDcb dcb;
        st_dcb_begin(&dcb, &queue);
        st_dcb_dispatch(&dcb, &copy, table, PAGE_WORDS / ST_LOCAL_SIZE_X);
        st_dcb_barrier(&dcb, 0);
        if (runs[k].op == OP_FILL) {
            st_dcb_fill(&dcb, v.b, v.b_words * sizeof(uint32_t), FILL);
        } else {
            st_dcb_copy(&dcb, d, v.b, v.b_words * sizeof(uint32_t));
        }
        if (!st_dcb_submit_and_wait(&dcb)) {
            return st_fail("reason=submit_or_eop case=%s", cases[k].name);
        }

        if (runs[k].op == OP_FILL) {
            check_fill(&cases[k], &v);
        } else {
            check_copy(&cases[k], &v, d);
        }
    }

    return st_finish_cases(cases, CASES, "");
}
