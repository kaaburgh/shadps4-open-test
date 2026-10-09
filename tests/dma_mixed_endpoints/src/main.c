/*
 * dma_mixed_endpoints: scenario B2 of docs/uma-e1c-e3-scenario-plan.md.
 *
 * A DMA copy from a source that shadPS4 could serve from the shared backing
 * into a destination it cannot must reach guest memory in every readbacks
 * mode. Nothing a shader writes is checked: the source is CPU-written and the
 * CPU reads the destination, so every configuration is diagnostic.
 *
 * Each case is one DCB:
 *   [dispatch reading D (read-only), [B]]
 *   DMA copy S -> D
 *   EOP
 * S is a fresh, physically contiguous 32 KiB range the CPU filled before the
 * submit. D is 32 KiB.
 *
 * Cases:
 *   mirrored_dst   D is two pages from separate physical allocations,
 *                  mapped back to back, and the dispatch binds it first, so
 *                  shadPS4 gives it arena (mirror) memory.
 *   unbound_dst    D is laid out the same way but never bound.
 *   shareable_dst  control: D is a fresh contiguous range too.
 *
 * Between a393f8d and 80eece2 shadPS4 copied on the GPU here when shared
 * backing was on, so the result stayed in D's mirror and never reached guest
 * memory in readbacks Relaxed/Disabled; 8d07f08 copies in guest memory.
 */

#define SHADTEST_NAME "dma_mixed_endpoints"
#include "shadtest_guest.h"

enum {
    ARENA_BYTES = 2 * 1024 * 1024,
    SLOT_BYTES = 64 * 1024,
    BYTES = 2 * ST_DMEM_PAGE_BYTES,
    WORDS = BYTES / sizeof(uint32_t),
    WINDOW_BASE = 16 * 1024 * 1024,
    WINDOW_BYTES = 256 * 1024,
    Q_OFFSET = 64 * 1024,
    BINDINGS = 2,
};

_Static_assert(WORDS % ST_LOCAL_SIZE_X == 0, "whole workgroups");

enum {
    CASE_MIRRORED_DST,
    CASE_UNBOUND_DST,
    CASE_SHAREABLE_DST,
    CASES,
};

static uint32_t source(unsigned seed, uint32_t i) {
    return (i * 0x9E3779B1u) ^ (seed * 0x7F4A7C15u) ^ 0x53525353u;
}
static uint32_t d_initial(uint32_t i) {
    return (i * 0x165667B1u) ^ 0x44444444u;
}

/* Two 16 KiB pages from physical offsets 64 KiB apart, mapped back to back. */
static volatile uint32_t* map_noncontiguous(unsigned index) {
    const off_t window = WINDOW_BASE + (off_t)index * WINDOW_BYTES;
    off_t phys[2];
    uint8_t* range = NULL;
    if (!st_dmem_alloc_in(window, window + Q_OFFSET, ST_DMEM_PAGE_BYTES,
                          ST_DMEM_PAGE_BYTES, &phys[0]) ||
        !st_dmem_alloc_in(window + Q_OFFSET, window + WINDOW_BYTES,
                          ST_DMEM_PAGE_BYTES, ST_DMEM_PAGE_BYTES, &phys[1]) ||
        !st_map_pages(phys, 2, &range)) {
        return NULL;
    }
    printf("case %u: D at %p from physical 0x%llx and 0x%llx\n", index,
           (void*)range, (unsigned long long)phys[0],
           (unsigned long long)phys[1]);
    return (volatile uint32_t*)range;
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

    GnmBuffer* table = (GnmBuffer*)st_arena_alloc(
        &arena, BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    volatile uint32_t* scratch =
        (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
    if (!table || !scratch) {
        return st_fail("reason=arena_memory");
    }

    StCase cases[CASES] = {
        [CASE_MIRRORED_DST] = {.name = "mirrored_dst"},
        [CASE_UNBOUND_DST] = {.name = "unbound_dst"},
        [CASE_SHAREABLE_DST] = {.name = "shareable_dst"},
    };

    for (unsigned k = 0; k < CASES; ++k) {
        volatile uint32_t* d = NULL;
        if (k == CASE_SHAREABLE_DST) {
            d = (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES,
                                                   SLOT_BYTES);
        } else {
            d = map_noncontiguous(k);
        }
        volatile uint32_t* s =
            (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
        if (!d || !s) {
            return st_fail("reason=placement case=%s", cases[k].name);
        }
        for (uint32_t i = 0; i < WORDS; ++i) {
            d[i] = d_initial(i);
            s[i] = source(k, i);
        }

        StDcb dcb;
        st_dcb_begin(&dcb, &queue);
        if (k == CASE_MIRRORED_DST) {
            table[0] = st_raw_buffer(d, BYTES);
            table[1] = st_raw_buffer(scratch, BYTES);
            st_dcb_dispatch(&dcb, &copy, table, WORDS / ST_LOCAL_SIZE_X);
            st_dcb_barrier(&dcb, 0);
        }
        st_dcb_copy(&dcb, d, s, BYTES);
        if (!st_dcb_submit_and_wait(&dcb)) {
            return st_fail("reason=submit_or_eop case=%s", cases[k].name);
        }

        for (uint32_t i = 0; i < WORDS; ++i) {
            const uint32_t got = d[i];
            if (got != source(k, i)) {
                st_case_note(&cases[k],
                             got == d_initial(i) ? "copy_not_in_guest_memory"
                                                 : "mismatch",
                             i, source(k, i), got);
            }
        }
    }

    return st_finish_cases(cases, CASES, "");
}
