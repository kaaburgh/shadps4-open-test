/*
 * dma_order: scenario A4 of docs/uma-e1c-e3-scenario-plan.md.
 *
 * DMA_DATA (OpenGNM sceGnmDrawCmdFillMemory / CopyMemory, CP_SYNC set) must
 * be ordered against dispatches. CP_SYNC only makes later packets wait for
 * the DMA, so every dispatch -> DMA edge has the plan's barrier [B] first,
 * and a DMA whose result a later dispatch reads has [B] after it too.
 *
 * Cases, each in one DCB:
 *   a_copy_after_write   dispatch writes S; [B]; DMA copy S -> D; the CPU
 *                        reads D.
 *   b_fill_between_reads dispatch A copies R to outA; [B]; DMA fill R = 5;
 *                        [B]; dispatch B copies R to outB. outA must hold the
 *                        old R, outB the fill.
 *   c_fill_then_wait     DMA fill flag (only the CPU wrote it before);
 *                        WAIT_REG_MEM flag == V; WRITE_DATA done. No shader:
 *                        CP-only, checkable in every configuration.
 *   d_fill_gpu_written   as c, but a dispatch wrote the flag earlier in the
 *                        DCB ([B] before the DMA). shadPS4 then fills on the
 *                        GPU, and the CP's wait needs that result. Without a
 *                        readback the wait never ends, so this case runs last;
 *                        the guest gives up after ST_EOP_WAIT_MS.
 */

#define SHADTEST_NAME "dma_order"
#include "shadtest_guest.h"

enum {
    ARENA_BYTES = 2 * 1024 * 1024,
    SLOT_BYTES = 64 * 1024,
    /* Above shadPS4's 16 KiB stream threshold: the tracked BufferCache path. */
    WORDS = 32 * 1024 / sizeof(uint32_t),
    BYTES = WORDS * sizeof(uint32_t),
    FLAG_WORDS = ST_LOCAL_SIZE_X,
    BINDINGS = 2,
};

_Static_assert(WORDS % ST_LOCAL_SIZE_X == 0, "whole workgroups");

enum {
    CASE_A,
    CASE_B,
    CASE_C,
    CASE_D,
    CASES,
};

static const uint32_t OUTPUT_POISON = 0xcdcdcdcdu;
static const uint32_t FILL_VALUE = 5;
static const uint32_t FLAG_VALUE = 0xC0FFEE01u;
static const uint32_t DONE = 1;

static uint32_t pattern(unsigned seed, uint32_t i) {
    return (i * 0x9E3779B1u) ^ (seed * 0x7F4A7C15u) ^ 0x0BADF00Du;
}

static volatile uint32_t* slot(StArena* arena) {
    return (volatile uint32_t*)st_arena_alloc(arena, SLOT_BYTES, SLOT_BYTES);
}

static const uint32_t S_INITIAL = 0x5EED5EEDu;

/* Notes every word of got that differs from expected(i): poison is
 * "unwritten", the value stale(i) is stale_reason, anything else
 * "mismatch". index_base keeps indices of several checks apart. */
static void check_words(StCase* c, const volatile uint32_t* got,
                        uint32_t count, uint32_t index_base,
                        uint32_t (*expected)(uint32_t),
                        uint32_t (*stale)(uint32_t),
                        const char* stale_reason) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t want = expected(i);
        const uint32_t v = got[i];
        if (v == want) {
            continue;
        }
        const char* reason = "mismatch";
        if (v == OUTPUT_POISON) {
            reason = "unwritten";
        } else if (v == stale(i)) {
            reason = stale_reason;
        }
        st_case_note(c, reason, index_base + i, want, v);
    }
}

static uint32_t source(uint32_t i) {
    return pattern(1, i);
}
static uint32_t s_initial(uint32_t i) {
    (void)i;
    return S_INITIAL;
}
static uint32_t r_before(uint32_t i) {
    return pattern(2, i);
}
static uint32_t fill_value(uint32_t i) {
    (void)i;
    return FILL_VALUE;
}

/* c and d: [optional dispatch writing the flag, B]; fill; wait; done. */
static bool fill_then_wait(StQueue* queue, const StComputeShader* shader,
                           GnmBuffer* table, volatile uint32_t* flag,
                           volatile uint32_t* done) {
    StDcb dcb;
    st_dcb_begin(&dcb, queue);
    if (shader) {
        st_dcb_dispatch(&dcb, shader, table, 1);
        st_dcb_barrier(&dcb, 0);
    }
    st_dcb_fill(&dcb, flag, FLAG_WORDS * sizeof(uint32_t), FLAG_VALUE);
    sceGnmDrawCmdWaitMem(&dcb.cmd, GNM_WAIT_REG_MEM_FUNC_EQUAL,
                         (uint64_t)(uintptr_t)flag, FLAG_VALUE, 0xffffffffu);
    st_dcb_write_data(&dcb, done, &DONE, 1);
    return st_dcb_submit_and_wait(&dcb);
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
        &arena, 4 * BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    volatile uint32_t* const a_src = slot(&arena);
    volatile uint32_t* const a_s = slot(&arena);
    volatile uint32_t* const a_d = slot(&arena);
    volatile uint32_t* const b_r = slot(&arena);
    volatile uint32_t* const b_out_a = slot(&arena);
    volatile uint32_t* const b_out_b = slot(&arena);
    volatile uint32_t* const cd_words = slot(&arena);
    volatile uint32_t* const d_src = slot(&arena);
    if (!tables || !a_src || !a_s || !a_d || !b_r || !b_out_a || !b_out_b ||
        !cd_words || !d_src) {
        return st_fail("reason=arena_memory");
    }
    /* c and d each get their own 16 KiB page for the flag. */
    volatile uint32_t* const c_flag = cd_words;
    volatile uint32_t* const c_done = cd_words + FLAG_WORDS;
    volatile uint32_t* const d_flag = cd_words + 4096;
    volatile uint32_t* const d_done = cd_words + 4096 + FLAG_WORDS;

    GnmBuffer* const table_a = &tables[0 * BINDINGS];
    GnmBuffer* const table_b_a = &tables[1 * BINDINGS];
    GnmBuffer* const table_b_b = &tables[2 * BINDINGS];
    GnmBuffer* const table_d = &tables[3 * BINDINGS];
    table_a[0] = st_raw_buffer(a_src, BYTES);
    table_a[1] = st_raw_buffer(a_s, BYTES);
    table_b_a[0] = st_raw_buffer(b_r, BYTES);
    table_b_a[1] = st_raw_buffer(b_out_a, BYTES);
    table_b_b[0] = st_raw_buffer(b_r, BYTES);
    table_b_b[1] = st_raw_buffer(b_out_b, BYTES);
    table_d[0] = st_raw_buffer(d_src, FLAG_WORDS * sizeof(uint32_t));
    table_d[1] = st_raw_buffer(d_flag, FLAG_WORDS * sizeof(uint32_t));

    for (uint32_t i = 0; i < WORDS; ++i) {
        a_src[i] = pattern(1, i);
        b_r[i] = pattern(2, i);
    }
    st_fill_words(a_s, WORDS, S_INITIAL);
    st_fill_words(a_d, WORDS, OUTPUT_POISON);
    st_fill_words(b_out_a, WORDS, OUTPUT_POISON);
    st_fill_words(b_out_b, WORDS, OUTPUT_POISON);
    st_fill_words(c_flag, FLAG_WORDS, 0);
    st_fill_words(d_flag, FLAG_WORDS, 0);
    st_fill_words(d_src, FLAG_WORDS, 0);
    *c_done = 0;
    *d_done = 0;

    StCase cases[CASES] = {
        [CASE_A] = {.name = "a_copy_after_write"},
        [CASE_B] = {.name = "b_fill_between_reads"},
        [CASE_C] = {.name = "c_fill_then_wait"},
        [CASE_D] = {.name = "d_fill_gpu_written"},
    };

    /* a */
    {
        StDcb dcb;
        st_dcb_begin(&dcb, &queue);
        st_dcb_dispatch(&dcb, &copy, table_a, WORDS / ST_LOCAL_SIZE_X);
        st_dcb_barrier(&dcb, 0);
        st_dcb_copy(&dcb, a_d, a_s, BYTES);
        if (!st_dcb_submit_and_wait(&dcb)) {
            return st_fail("reason=submit_or_eop case=a_copy_after_write");
        }
        check_words(&cases[CASE_A], a_d, WORDS, 0, source, s_initial,
                    "d_copied_before_write");
    }

    /* b */
    {
        StDcb dcb;
        st_dcb_begin(&dcb, &queue);
        st_dcb_dispatch(&dcb, &copy, table_b_a, WORDS / ST_LOCAL_SIZE_X);
        st_dcb_barrier(&dcb, 0);
        st_dcb_fill(&dcb, b_r, BYTES, FILL_VALUE);
        st_dcb_barrier(&dcb, 0);
        st_dcb_dispatch(&dcb, &copy, table_b_b, WORDS / ST_LOCAL_SIZE_X);
        if (!st_dcb_submit_and_wait(&dcb)) {
            return st_fail("reason=submit_or_eop case=b_fill_between_reads");
        }
        check_words(&cases[CASE_B], b_out_a, WORDS, 0, r_before, fill_value,
                    "a_saw_fill");
        check_words(&cases[CASE_B], b_out_b, WORDS, WORDS, fill_value,
                    r_before, "b_missed_fill");
        /* R itself is written by the DMA: checkable in every mode. */
        check_words(&cases[CASE_B], b_r, WORDS, 2 * WORDS, fill_value,
                    r_before, "r_not_filled");
    }

    /* c */
    if (!fill_then_wait(&queue, NULL, NULL, c_flag, c_done)) {
        st_case_note(&cases[CASE_C], "timeout", 0, DONE, *c_done);
    } else {
        st_case_expect(&cases[CASE_C], "done", 0, DONE, *c_done);
        st_case_expect(&cases[CASE_C], "flag", 1, FLAG_VALUE, *c_flag);
    }

    /* d, last: it can leave the CP waiting. */
    if (!fill_then_wait(&queue, &copy, table_d, d_flag, d_done)) {
        st_case_note(&cases[CASE_D], "timeout", 0, DONE, *d_done);
    } else {
        st_case_expect(&cases[CASE_D], "done", 0, DONE, *d_done);
    }

    return st_finish_cases(cases, CASES, "");
}
