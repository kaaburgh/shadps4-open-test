/*
 * pm4_controls: controls for the M0 infrastructure of
 * docs/uma-e1c-e3-scenario-plan.md.
 *
 * Every check is CP- or CPU-only: no shader writes anything that a case
 * reads. The results reach guest memory without a readback, so all six
 * configurations (mirror/shared x Precise/Relaxed/Disabled) must pass. A
 * failure here means the packets or helpers that the E1C/E3 scenarios build
 * on are broken, not that a scenario found a bug.
 *
 * Cases:
 *   barriers          [B] after a dispatch, [B] after a draw with CB, and
 *                     [P] in one DCB; the end-of-pipe label still arrives.
 *   write_data        WRITE_DATA of four dwords; the CPU reads them back.
 *   cond_exec_false   COND_EXEC on a CPU-written 0 skips exactly the guarded
 *                     WRITE_DATA: the marker keeps its old value and the
 *                     unconditional WRITE_DATA after it still runs.
 *   cond_exec_true    the same with a predicate of 1: the marker is written.
 *   discontiguous     two 16 KiB pages from physical offsets 64 KiB apart,
 *                     mapped back to back. A CPU write through a separate
 *                     mapping of each page shows through the combined range,
 *                     and a WRITE_DATA spanning both pages shows through the
 *                     separate mappings.
 *   chunk_straddle    32 KiB at physical [256 MiB - 16 KiB, 256 MiB + 16 KiB),
 *                     across shadPS4's 256 MiB shared-backing chunk boundary;
 *                     CPU and WRITE_DATA round trips across the boundary.
 */

#define SHADTEST_NAME "pm4_controls"
#include "shadtest_guest.h"

enum {
    ARENA_BYTES = 2 * 1024 * 1024,
    PAGE_WORDS = ST_DMEM_PAGE_BYTES / sizeof(uint32_t),
    CHUNK_BOUNDARY = 256 * 1024 * 1024,
    /* Placement search windows, far above the arena at the bottom of direct
     * memory. */
    PAGE_P_BASE = 16 * 1024 * 1024,
    PAGE_Q_BASE = PAGE_P_BASE + 64 * 1024,
    PAGE_SEARCH_END = 32 * 1024 * 1024,
};

enum {
    CASE_BARRIERS,
    CASE_WRITE_DATA,
    CASE_COND_EXEC_FALSE,
    CASE_COND_EXEC_TRUE,
    CASE_DISCONTIGUOUS,
    CASE_CHUNK_STRADDLE,
    CASES,
};

static const uint32_t MARKER_OLD = 0x0BADC0DEu;
static const uint32_t MARKER_NEW = 7;
static const uint32_t SENTINEL = 9;

static uint32_t fill(unsigned seed, uint32_t i) {
    return (i * 0x9E3779B1u) ^ (seed * 0x7F4A7C15u) ^ 0x434F4E54u;
}

/* COND_EXEC on *pred guarding WRITE_DATA marker = MARKER_NEW, then an
 * unconditional WRITE_DATA sentinel = SENTINEL. */
static bool run_cond_exec(StQueue* queue, volatile uint32_t* pred,
                          uint32_t pred_value, volatile uint32_t* marker,
                          volatile uint32_t* sentinel) {
    *pred = pred_value;
    *marker = MARKER_OLD;
    *sentinel = 0;

    StDcb dcb;
    st_dcb_begin(&dcb, queue);
    st_dcb_pfp_sync_me(&dcb);
    st_dcb_cond_exec(&dcb, pred, st_pm4_write_data_dwords(1));
    st_dcb_write_data(&dcb, marker, &MARKER_NEW, 1);
    st_dcb_write_data(&dcb, sentinel, &SENTINEL, 1);
    return st_dcb_submit_and_wait(&dcb);
}

static void check_cond_exec(StCase* c, volatile uint32_t* marker,
                            volatile uint32_t* sentinel, uint32_t expected) {
    st_case_expect(c, "marker", 0, expected, *marker);
    st_case_expect(c, "sentinel", 1, SENTINEL, *sentinel);
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

    volatile uint32_t* words =
        (volatile uint32_t*)st_arena_alloc(&arena, ST_PAGE_BYTES, ST_PAGE_BYTES);
    if (!words) {
        return st_fail("reason=arena_memory");
    }
    volatile uint32_t* const data_dst = words;       /* 4 words */
    volatile uint32_t* const pred = words + 8;
    volatile uint32_t* const marker = words + 9;
    volatile uint32_t* const sentinel = words + 10;

    StCase cases[CASES] = {
        [CASE_BARRIERS] = {.name = "barriers"},
        [CASE_WRITE_DATA] = {.name = "write_data"},
        [CASE_COND_EXEC_FALSE] = {.name = "cond_exec_false"},
        [CASE_COND_EXEC_TRUE] = {.name = "cond_exec_true"},
        [CASE_DISCONTIGUOUS] = {.name = "discontiguous"},
        [CASE_CHUNK_STRADDLE] = {.name = "chunk_straddle"},
    };

    /* barriers: a failed submission or a timed-out label is the failure. */
    {
        StDcb dcb;
        st_dcb_begin(&dcb, &queue);
        st_dcb_barrier(&dcb, 0);
        st_dcb_barrier(&dcb, ST_PM4_BARRIER_AFTER_DRAW | ST_PM4_BARRIER_CB);
        st_dcb_pfp_sync_me(&dcb);
        if (!st_dcb_submit_and_wait(&dcb)) {
            st_case_note(&cases[CASE_BARRIERS], "submit_or_eop", 0, 0, 0);
        }
    }

    /* write_data */
    {
        const uint32_t data[4] = {fill(1, 0), fill(1, 1), fill(1, 2),
                                  fill(1, 3)};
        st_fill_words(data_dst, 4, 0);
        StDcb dcb;
        st_dcb_begin(&dcb, &queue);
        st_dcb_write_data(&dcb, data_dst, data, 4);
        if (!st_dcb_submit_and_wait(&dcb)) {
            return st_fail("reason=submit_or_eop case=write_data");
        }
        for (uint32_t i = 0; i < 4; ++i) {
            st_case_expect(&cases[CASE_WRITE_DATA], "write_data", i, data[i],
                           data_dst[i]);
        }
    }

    /* cond_exec_false / cond_exec_true */
    if (!run_cond_exec(&queue, pred, 0, marker, sentinel)) {
        return st_fail("reason=submit_or_eop case=cond_exec_false");
    }
    check_cond_exec(&cases[CASE_COND_EXEC_FALSE], marker, sentinel,
                    MARKER_OLD);
    if (!run_cond_exec(&queue, pred, 1, marker, sentinel)) {
        return st_fail("reason=submit_or_eop case=cond_exec_true");
    }
    check_cond_exec(&cases[CASE_COND_EXEC_TRUE], marker, sentinel, MARKER_NEW);

    /* discontiguous */
    {
        const int alias_res = sceKernelEnableDmemAliasing();
        if (alias_res != 0) {
            return st_fail("reason=enable_dmem_aliasing res=0x%x", alias_res);
        }
        off_t phys[2];
        if (!st_dmem_alloc_in(PAGE_P_BASE, PAGE_SEARCH_END, ST_DMEM_PAGE_BYTES,
                              ST_DMEM_PAGE_BYTES, &phys[0]) ||
            !st_dmem_alloc_in(PAGE_Q_BASE, PAGE_SEARCH_END, ST_DMEM_PAGE_BYTES,
                              ST_DMEM_PAGE_BYTES, &phys[1])) {
            return st_fail("reason=placement case=discontiguous");
        }
        printf("discontiguous pages at physical 0x%llx and 0x%llx\n",
               (unsigned long long)phys[0], (unsigned long long)phys[1]);
        if (phys[1] == phys[0] + ST_DMEM_PAGE_BYTES) {
            return st_fail("reason=pages_adjacent case=discontiguous");
        }

        uint8_t* combined = NULL;
        void* single[2] = {NULL, NULL};
        if (!st_map_pages(phys, 2, &combined) ||
            !st_dmem_map(&single[0], ST_DMEM_PAGE_BYTES, phys[0], false) ||
            !st_dmem_map(&single[1], ST_DMEM_PAGE_BYTES, phys[1], false)) {
            return st_fail("reason=map case=discontiguous");
        }
        printf("combined range at %p, pages also at %p and %p\n",
               (void*)combined, single[0], single[1]);

        volatile uint32_t* const range = (volatile uint32_t*)combined;
        volatile uint32_t* const page[2] = {(volatile uint32_t*)single[0],
                                            (volatile uint32_t*)single[1]};
        StCase* c = &cases[CASE_DISCONTIGUOUS];

        for (unsigned p = 0; p < 2; ++p) {
            for (uint32_t i = 0; i < PAGE_WORDS; ++i) {
                page[p][i] = fill(10 + p, i);
            }
        }
        st_compiler_barrier();
        for (unsigned p = 0; p < 2; ++p) {
            for (uint32_t i = 0; i < PAGE_WORDS; ++i) {
                st_case_expect(c, "cpu_range", p * PAGE_WORDS + i,
                               fill(10 + p, i), range[p * PAGE_WORDS + i]);
            }
        }

        /* Four dwords across the page boundary, written through the range. */
        const uint32_t span[4] = {fill(12, 0), fill(12, 1), fill(12, 2),
                                  fill(12, 3)};
        StDcb dcb;
        st_dcb_begin(&dcb, &queue);
        st_dcb_write_data(&dcb, range + PAGE_WORDS - 2, span, 4);
        if (!st_dcb_submit_and_wait(&dcb)) {
            return st_fail("reason=submit_or_eop case=discontiguous");
        }
        st_case_expect(c, "write_data_page0", PAGE_WORDS - 2, span[0],
                       page[0][PAGE_WORDS - 2]);
        st_case_expect(c, "write_data_page0", PAGE_WORDS - 1, span[1],
                       page[0][PAGE_WORDS - 1]);
        st_case_expect(c, "write_data_page1", PAGE_WORDS, span[2], page[1][0]);
        st_case_expect(c, "write_data_page1", PAGE_WORDS + 1, span[3],
                       page[1][1]);
    }

    /* chunk_straddle */
    {
        off_t phys = -1;
        if (!st_dmem_alloc_in(CHUNK_BOUNDARY - ST_DMEM_PAGE_BYTES,
                              CHUNK_BOUNDARY + ST_DMEM_PAGE_BYTES,
                              2 * ST_DMEM_PAGE_BYTES, ST_DMEM_PAGE_BYTES,
                              &phys)) {
            return st_fail("reason=placement case=chunk_straddle");
        }
        if (phys != CHUNK_BOUNDARY - ST_DMEM_PAGE_BYTES) {
            return st_fail("reason=placement case=chunk_straddle phys=0x%llx",
                           (unsigned long long)phys);
        }
        void* mapped = NULL;
        if (!st_dmem_map(&mapped, 2 * ST_DMEM_PAGE_BYTES, phys, false)) {
            return st_fail("reason=map case=chunk_straddle");
        }
        printf("chunk_straddle physical 0x%llx mapped at %p\n",
               (unsigned long long)phys, mapped);

        volatile uint32_t* const range = (volatile uint32_t*)mapped;
        StCase* c = &cases[CASE_CHUNK_STRADDLE];
        for (uint32_t i = 0; i < 2 * PAGE_WORDS; ++i) {
            range[i] = fill(20, i);
        }
        st_compiler_barrier();
        for (uint32_t i = 0; i < 2 * PAGE_WORDS; ++i) {
            st_case_expect(c, "cpu", i, fill(20, i), range[i]);
        }

        const uint32_t span[4] = {fill(21, 0), fill(21, 1), fill(21, 2),
                                  fill(21, 3)};
        StDcb dcb;
        st_dcb_begin(&dcb, &queue);
        st_dcb_write_data(&dcb, range + PAGE_WORDS - 2, span, 4);
        if (!st_dcb_submit_and_wait(&dcb)) {
            return st_fail("reason=submit_or_eop case=chunk_straddle");
        }
        for (uint32_t i = 0; i < 4; ++i) {
            st_case_expect(c, "write_data", PAGE_WORDS - 2 + i, span[i],
                           range[PAGE_WORDS - 2 + i]);
        }
    }

    return st_finish_cases(cases, CASES, "");
}
