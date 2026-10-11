/*
 * fallback_in_shared: scenario B4 of docs/uma-e1c-e3-scenario-plan.md.
 *
 * Ranges that shadPS4's shared backing cannot serve fall back to the mirror.
 * One DCB writes three 32 KiB ranges with a dispatch each, then the CPU
 * reads them:
 *
 *   chunk_straddle  physically contiguous, but across the 256 MiB boundary
 *                   between two imported chunks (physical
 *                   [256 MiB - 16 KiB, 256 MiB + 16 KiB)).
 *   noncontiguous   two 16 KiB pages from separate physical allocations,
 *                   mapped back to back.
 *   control         a fresh contiguous range inside one chunk; shared.
 *
 * This does not test ordering. It documents that the shared backing does
 * not lift the readback limitation for ranges that fall back: with readbacks
 * Relaxed/Disabled their GPU writes stay in the mirror, as with the mirror
 * alone, while the control range reaches guest memory (kaaburgh/shadPS4#12).
 */

#define SHADTEST_NAME "fallback_in_shared"
#include "shadtest_guest.h"

enum {
    ARENA_BYTES = 2 * 1024 * 1024,
    SLOT_BYTES = 64 * 1024,
    BYTES = 2 * ST_DMEM_PAGE_BYTES,
    WORDS = BYTES / sizeof(uint32_t),
    CHUNK_BOUNDARY = 256 * 1024 * 1024,
    P_BASE = 16 * 1024 * 1024,
    Q_BASE = P_BASE + 64 * 1024,
    PAGE_SEARCH_END = 32 * 1024 * 1024,
    BINDINGS = 2,
};

_Static_assert(WORDS % ST_LOCAL_SIZE_X == 0, "whole workgroups");

enum {
    CASE_CHUNK_STRADDLE,
    CASE_NONCONTIGUOUS,
    CASE_CONTROL,
    CASES,
};

static uint32_t written(unsigned range, uint32_t i) {
    return (i * 0x9E3779B1u) ^ (range * 0x7F4A7C15u) ^ 0x46424B21u;
}
static uint32_t initial(uint32_t i) {
    return (i * 0x165667B1u) ^ 0x49494949u;
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

    GnmBuffer* tables = (GnmBuffer*)st_arena_alloc(
        &arena, CASES * BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    if (!tables) {
        return st_fail("reason=arena_memory");
    }

    volatile uint32_t* ranges[CASES];

    off_t straddle_phys = -1;
    void* straddle = NULL;
    if (!st_dmem_alloc_in(CHUNK_BOUNDARY - ST_DMEM_PAGE_BYTES,
                          CHUNK_BOUNDARY + ST_DMEM_PAGE_BYTES, BYTES,
                          ST_DMEM_PAGE_BYTES, &straddle_phys) ||
        straddle_phys != CHUNK_BOUNDARY - ST_DMEM_PAGE_BYTES ||
        !st_dmem_map(&straddle, BYTES, straddle_phys, false)) {
        return st_fail("reason=placement case=chunk_straddle");
    }
    ranges[CASE_CHUNK_STRADDLE] = (volatile uint32_t*)straddle;

    off_t phys[2];
    uint8_t* noncontiguous = NULL;
    if (!st_dmem_alloc_in(P_BASE, Q_BASE, ST_DMEM_PAGE_BYTES,
                          ST_DMEM_PAGE_BYTES, &phys[0]) ||
        !st_dmem_alloc_in(Q_BASE, PAGE_SEARCH_END, ST_DMEM_PAGE_BYTES,
                          ST_DMEM_PAGE_BYTES, &phys[1]) ||
        !st_map_pages(phys, 2, &noncontiguous)) {
        return st_fail("reason=placement case=noncontiguous");
    }
    ranges[CASE_NONCONTIGUOUS] = (volatile uint32_t*)noncontiguous;

    ranges[CASE_CONTROL] =
        (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
    if (!ranges[CASE_CONTROL]) {
        return st_fail("reason=arena_memory");
    }

    printf("chunk_straddle %p (physical 0x%llx), noncontiguous %p (physical "
           "0x%llx, 0x%llx), control %p\n",
           straddle, (unsigned long long)straddle_phys, (void*)noncontiguous,
           (unsigned long long)phys[0], (unsigned long long)phys[1],
           (void*)ranges[CASE_CONTROL]);

    StDcb dcb;
    st_dcb_begin(&dcb, &queue);
    for (unsigned k = 0; k < CASES; ++k) {
        volatile uint32_t* src =
            (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
        if (!src) {
            return st_fail("reason=arena_memory");
        }
        for (uint32_t i = 0; i < WORDS; ++i) {
            src[i] = written(k, i);
            ranges[k][i] = initial(i);
        }
        GnmBuffer* table = &tables[k * BINDINGS];
        table[0] = st_raw_buffer(src, BYTES);
        table[1] = st_raw_buffer(ranges[k], BYTES);
        st_dcb_dispatch(&dcb, &copy, table, WORDS / ST_LOCAL_SIZE_X);
    }
    if (!st_dcb_submit_and_wait(&dcb)) {
        return st_fail("reason=submit_or_eop");
    }

    StCase cases[CASES] = {
        [CASE_CHUNK_STRADDLE] = {.name = "chunk_straddle"},
        [CASE_NONCONTIGUOUS] = {.name = "noncontiguous"},
        [CASE_CONTROL] = {.name = "control"},
    };
    for (unsigned k = 0; k < CASES; ++k) {
        for (uint32_t i = 0; i < WORDS; ++i) {
            const uint32_t got = ranges[k][i];
            if (got != written(k, i)) {
                st_case_note(&cases[k],
                             got == initial(i) ? "gpu_write_not_in_guest_memory"
                                               : "mismatch",
                             i, written(k, i), got);
            }
        }
    }

    return st_finish_cases(cases, CASES, "");
}
