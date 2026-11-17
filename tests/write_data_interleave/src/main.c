/*
 * write_data_interleave: a narrow companion to write_data_order (scenario A1
 * of docs/uma-e1c-e3-scenario-plan.md, kaaburgh/shadPS4#11).
 *
 * write_data_order has one dispatch before and one after the WRITE_DATA, both
 * short. An emulator that submits the recorded work and lets the CPU write
 * race it, or that happens to wait for the GPU somewhere, can pass it. This
 * test makes such a pass unlikely or visible:
 *
 *   interleave      ROUNDS times in one DCB: a slow dispatch copies X to
 *                   out[r], then WRITE_DATA changes 12 words of X to the
 *                   values of round r + 1; a last dispatch copies X to
 *                   out[ROUNDS]. Each dispatch spins in a dependent loop
 *                   before it reads X, so a CP write that is not ordered after
 *                   the dispatch lands while the dispatch still runs.
 *                   out[r] must hold exactly the values of round r.
 *   gpu_then_cp     a dispatch writes Y, then WRITE_DATA overwrites 4 words of
 *                   Y: the final words are the CP's (write-after-write).
 *   cp_then_gpu     WRITE_DATA writes Y2, then a dispatch overwrites all of
 *                   Y2: the final words are the dispatch's.
 *   cond_after_cp   WRITE_DATA sets a predicate that COND_EXEC reads in the
 *                   same DCB, for both values: the CP must read what it
 *                   wrote, even when the write is ordered on the GPU timeline.
 *   label_no_eop    a slow dispatch, then WRITE_DATA of a 64-bit label, no EOP
 *                   and no sceGnmSubmitDone: the CPU must see the label.
 *   x_values        final contents of X, the predicates and the markers.
 *
 * X, the predicates, the markers and the label share one 16 KiB block, so with
 * UMA shared backing all of these CP writes target memory that recorded work
 * reads through the shared backing. forbid-log.txt fails the run when shadPS4
 * waits for the GPU before a WRITE_DATA instead of ordering it.
 *
 * out[r], Y and Y2 are shader outputs: they need a readback (or shared
 * backing) to reach the guest, as in write_data_order.
 */

#define SHADTEST_NAME "write_data_interleave"
#include "shadtest_guest.h"

enum {
    ARENA_BYTES = 4 * 1024 * 1024,
    SLOT_BYTES = 64 * 1024,
    ROUNDS = 8,
    X_WORDS = 1024,
    SPOTS = 3,
    SPOT_WORDS = 4,
    /* Inside X's slot, behind X, in X's first 16 KiB. */
    PRED_A_WORD = 2048,
    PRED_B_WORD = 2049,
    MARKER_A_WORD = 2560,
    MARKER_B_WORD = 2561,
    SENTINEL_WORD = 2562,
    LABEL_WORD = 3072,
    Y_WORDS = ST_LOCAL_SIZE_X,
    BINDINGS = 2,
    /* Must match slow_copy.comp.glsl. */
    SLOW_ITERATIONS = 8192,
};

_Static_assert(X_WORDS % ST_LOCAL_SIZE_X == 0, "whole workgroups");
_Static_assert(LABEL_WORD % 2 == 0, "8-byte label");
_Static_assert((LABEL_WORD + 2) * 4 <= 16 * 1024, "one 16 KiB block");

static const uint32_t SLOW_MAGIC = 0x13579BDFu;

enum {
    CASE_INTERLEAVE,
    CASE_GPU_THEN_CP,
    CASE_CP_THEN_GPU,
    CASE_COND_AFTER_CP,
    CASE_LABEL_NO_EOP,
    CASE_X_VALUES,
    CASES,
};

static const uint32_t OUTPUT_POISON = 0xcdcdcdcdu;
static const uint32_t MARKER_OLD = 0x0BADC0DEu;
static const uint32_t MARKER_NEW = 7;
static const uint32_t SENTINEL = 9;

static uint32_t value(unsigned round, uint32_t i) {
    return (i * 0x9E3779B1u) ^ (round * 0x7F4A7C15u) ^ 0x0BADF00Du;
}

static uint32_t gpu_value(uint32_t i) {
    return 0xC0DE0000u | i;
}

static uint32_t cp_value(uint32_t i) {
    return 0xCF000000u | i;
}

static uint32_t spot_start(unsigned spot) {
    const uint32_t starts[SPOTS] = {0, X_WORDS / 2, X_WORDS - SPOT_WORDS};
    return starts[spot];
}

static bool is_written(uint32_t i) {
    for (unsigned s = 0; s < SPOTS; ++s) {
        if (i >= spot_start(s) && i < spot_start(s) + SPOT_WORDS) {
            return true;
        }
    }
    return false;
}

/* X after round r's WRITE_DATA (round 0: before any). */
static uint32_t x_after(unsigned round, uint32_t i) {
    return is_written(i) ? value(round, i) : value(0, i);
}

static uint32_t slow_loop(uint32_t acc) {
    for (unsigned k = 0; k < SLOW_ITERATIONS; ++k) {
        acc = acc * 1664525u + 1013904223u;
    }
    return acc;
}

static void put_write_data(StDcb* dcb, volatile uint32_t* dst,
                           uint32_t (*fn)(unsigned, uint32_t), unsigned round,
                           uint32_t first, uint32_t count) {
    uint32_t data[SPOT_WORDS];
    for (uint32_t k = 0; k < count; ++k) {
        data[k] = fn(round, first + k);
    }
    st_dcb_write_data(dcb, dst + first, data, count);
}

static void check_out(StCase* c, unsigned round, const volatile uint32_t* out) {
    for (uint32_t i = 0; i < X_WORDS; ++i) {
        const uint32_t expected = x_after(round, i);
        const uint32_t got = out[i];
        if (got == expected) {
            continue;
        }
        const char* reason = "mismatch";
        if (got == OUTPUT_POISON) {
            reason = "unwritten";
        } else if (is_written(i)) {
            for (unsigned q = 0; q <= ROUNDS; ++q) {
                if (q != round && got == value(q, i)) {
                    reason = q > round ? "saw_later_write" : "missed_write";
                    break;
                }
            }
        }
        /* index: round in the high 16 bits, word in the low. */
        st_case_note(c, reason, (round << 16) | i, expected, got);
    }
}

/* Submits without EOP and without sceGnmSubmitDone. */
static bool submit_only(StDcb* dcb) {
    if (!dcb->ok || st_gnm_errors != 0) {
        return false;
    }
    void* addrs[1] = {dcb->cmd.beginptr};
    uint32_t sizes[1] = {
        (uint32_t)((uintptr_t)dcb->cmd.cmdptr - (uintptr_t)dcb->cmd.beginptr),
    };
    st_store_fence();
    const int32_t res = sceGnmSubmitCommandBuffers(1, addrs, sizes, NULL, NULL);
    if (res < 0) {
        printf("sceGnmSubmitCommandBuffers failed: 0x%x\n", (unsigned)res);
        return false;
    }
    return true;
}

int main(void) {
    st_begin();

    if (slow_loop(OUTPUT_POISON) == SLOW_MAGIC) {
        return st_fail("reason=poison_hits_magic");
    }

    StArena arena;
    if (!st_arena_init(&arena, ARENA_BYTES)) {
        return st_fail("reason=direct_memory");
    }
    StQueue queue;
    if (!st_queue_init(&arena, &queue)) {
        return st_fail("reason=command_memory");
    }
    StComputeShader copy;
    StComputeShader slow;
    if (!st_load_compute_shader(&arena, "/app0/assets/copy.comp.sb", &copy) ||
        !st_load_compute_shader(&arena, "/app0/assets/slow_copy.comp.sb",
                                &slow)) {
        return st_fail("reason=shader_load");
    }

    enum { TABLES = ROUNDS + 1 + 3 };
    GnmBuffer* tables = (GnmBuffer*)st_arena_alloc(
        &arena, TABLES * BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    volatile uint32_t* x =
        (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
    volatile uint32_t* s =
        (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
    volatile uint32_t* y =
        (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
    volatile uint32_t* y2 =
        (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
    volatile uint32_t* scratch =
        (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
    volatile uint32_t* out[ROUNDS + 1];
    for (unsigned r = 0; r <= ROUNDS; ++r) {
        out[r] = (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
        if (!out[r]) {
            return st_fail("reason=arena_memory");
        }
    }
    if (!tables || !x || !s || !y || !y2 || !scratch) {
        return st_fail("reason=arena_memory");
    }

    for (uint32_t i = 0; i < X_WORDS; ++i) {
        x[i] = value(0, i);
    }
    for (uint32_t i = 0; i < Y_WORDS; ++i) {
        s[i] = gpu_value(i);
    }
    st_fill_words(y, Y_WORDS, OUTPUT_POISON);
    st_fill_words(y2, Y_WORDS, OUTPUT_POISON);
    st_fill_words(scratch, X_WORDS, OUTPUT_POISON);
    volatile uint32_t* const pred_a = x + PRED_A_WORD;
    volatile uint32_t* const pred_b = x + PRED_B_WORD;
    volatile uint32_t* const marker_a = x + MARKER_A_WORD;
    volatile uint32_t* const marker_b = x + MARKER_B_WORD;
    volatile uint32_t* const sentinel = x + SENTINEL_WORD;
    volatile uint64_t* const label = (volatile uint64_t*)(x + LABEL_WORD);
    *pred_a = 1;
    *pred_b = 0;
    *marker_a = MARKER_OLD;
    *marker_b = MARKER_OLD;
    *sentinel = 0;
    *label = 0;

    GnmBuffer* out_table[ROUNDS + 1];
    for (unsigned r = 0; r <= ROUNDS; ++r) {
        st_fill_words(out[r], X_WORDS, OUTPUT_POISON);
        out_table[r] = &tables[r * BINDINGS];
        out_table[r][0] = st_raw_buffer(x, X_WORDS * sizeof(uint32_t));
        out_table[r][1] = st_raw_buffer(out[r], X_WORDS * sizeof(uint32_t));
    }
    GnmBuffer* const y_table = &tables[(ROUNDS + 1) * BINDINGS];
    y_table[0] = st_raw_buffer(s, Y_WORDS * sizeof(uint32_t));
    y_table[1] = st_raw_buffer(y, Y_WORDS * sizeof(uint32_t));
    GnmBuffer* const y2_table = &tables[(ROUNDS + 2) * BINDINGS];
    y2_table[0] = st_raw_buffer(s, Y_WORDS * sizeof(uint32_t));
    y2_table[1] = st_raw_buffer(y2, Y_WORDS * sizeof(uint32_t));
    GnmBuffer* const scratch_table = &tables[(ROUNDS + 3) * BINDINGS];
    scratch_table[0] = st_raw_buffer(x, X_WORDS * sizeof(uint32_t));
    scratch_table[1] = st_raw_buffer(scratch, X_WORDS * sizeof(uint32_t));

    StCase cases[CASES] = {
        [CASE_INTERLEAVE] = {.name = "interleave"},
        [CASE_GPU_THEN_CP] = {.name = "gpu_then_cp"},
        [CASE_CP_THEN_GPU] = {.name = "cp_then_gpu"},
        [CASE_COND_AFTER_CP] = {.name = "cond_after_cp"},
        [CASE_LABEL_NO_EOP] = {.name = "label_no_eop"},
        [CASE_X_VALUES] = {.name = "x_values"},
    };
    const uint32_t groups = X_WORDS / ST_LOCAL_SIZE_X;
    StDcb dcb;

    /* interleave */
    st_dcb_begin(&dcb, &queue);
    for (unsigned r = 0; r < ROUNDS; ++r) {
        st_dcb_dispatch(&dcb, &slow, out_table[r], groups);
        st_dcb_barrier(&dcb, 0);
        for (unsigned sp = 0; sp < SPOTS; ++sp) {
            put_write_data(&dcb, x, value, r + 1, spot_start(sp), SPOT_WORDS);
        }
        st_dcb_barrier(&dcb, 0);
    }
    st_dcb_dispatch(&dcb, &slow, out_table[ROUNDS], groups);
    if (!st_dcb_submit_and_wait(&dcb)) {
        return st_fail("reason=submit_or_eop case=interleave");
    }
    for (unsigned r = 0; r <= ROUNDS; ++r) {
        check_out(&cases[CASE_INTERLEAVE], r, out[r]);
    }

    /* gpu_then_cp and cp_then_gpu */
    st_dcb_begin(&dcb, &queue);
    st_dcb_dispatch(&dcb, &copy, y_table, 1);
    st_dcb_barrier(&dcb, 0);
    {
        uint32_t data[SPOT_WORDS];
        for (uint32_t k = 0; k < SPOT_WORDS; ++k) {
            data[k] = cp_value(k);
        }
        st_dcb_write_data(&dcb, y, data, SPOT_WORDS);
        st_dcb_write_data(&dcb, y2, data, SPOT_WORDS);
    }
    st_dcb_barrier(&dcb, 0);
    st_dcb_dispatch(&dcb, &copy, y2_table, 1);
    if (!st_dcb_submit_and_wait(&dcb)) {
        return st_fail("reason=submit_or_eop case=gpu_cp");
    }
    for (uint32_t i = 0; i < Y_WORDS; ++i) {
        const uint32_t expected = i < SPOT_WORDS ? cp_value(i) : gpu_value(i);
        const uint32_t got = y[i];
        if (got != expected) {
            st_case_note(&cases[CASE_GPU_THEN_CP],
                         got == gpu_value(i)      ? "gpu_write_after_cp"
                         : got == OUTPUT_POISON ? "unwritten"
                                                : "mismatch",
                         i, expected, got);
        }
        const uint32_t got2 = y2[i];
        if (got2 != gpu_value(i)) {
            st_case_note(&cases[CASE_CP_THEN_GPU],
                         got2 == cp_value(i)      ? "cp_write_after_gpu"
                         : got2 == OUTPUT_POISON ? "unwritten"
                                                 : "mismatch",
                         i, gpu_value(i), got2);
        }
    }

    /* cond_after_cp: a dispatch reads X's block first, so with shared
     * backing it is in use by recorded work when the predicates change. */
    st_dcb_begin(&dcb, &queue);
    st_dcb_dispatch(&dcb, &copy, scratch_table, groups);
    st_dcb_barrier(&dcb, 0);
    {
        const uint32_t zero = 0;
        const uint32_t one = 1;
        const uint32_t marker = MARKER_NEW;
        const uint32_t sentinel_value = SENTINEL;
        const uint32_t guarded = ST_PM4_WRITE_DATA_BASE_DWORDS + 1;
        st_dcb_write_data(&dcb, pred_a, &zero, 1);
        st_dcb_write_data(&dcb, pred_b, &one, 1);
        st_dcb_cond_exec(&dcb, pred_a, guarded);
        st_dcb_write_data(&dcb, marker_a, &marker, 1);
        st_dcb_cond_exec(&dcb, pred_b, guarded);
        st_dcb_write_data(&dcb, marker_b, &marker, 1);
        st_dcb_write_data(&dcb, sentinel, &sentinel_value, 1);
    }
    if (!st_dcb_submit_and_wait(&dcb)) {
        return st_fail("reason=submit_or_eop case=cond_after_cp");
    }
    st_case_expect(&cases[CASE_COND_AFTER_CP], "pred_0_ran_guarded", 0,
                   MARKER_OLD, *marker_a);
    st_case_expect(&cases[CASE_COND_AFTER_CP], "pred_1_skipped_guarded", 1,
                   MARKER_NEW, *marker_b);
    st_case_expect(&cases[CASE_COND_AFTER_CP], "sentinel", 2, SENTINEL,
                   *sentinel);

    /* label_no_eop */
    {
        const uint64_t label_value = 0x4C4142454C000001ULL; /* "LABEL" */
        const uint32_t words[2] = {(uint32_t)label_value,
                                   (uint32_t)(label_value >> 32)};
        st_dcb_begin(&dcb, &queue);
        st_dcb_dispatch(&dcb, &slow, scratch_table, groups);
        st_dcb_barrier(&dcb, 0);
        st_dcb_write_data(&dcb, label, words, 2);
        if (!submit_only(&dcb)) {
            return st_fail("reason=submit case=label_no_eop");
        }
        if (!st_wait_label(label, label_value)) {
            st_case_note(&cases[CASE_LABEL_NO_EOP], "label_not_seen", 0,
                         (uint32_t)label_value, (uint32_t)*label);
        }
        /* Drain the queue before its command memory is reused. */
        st_dcb_begin(&dcb, &queue);
        if (!st_dcb_submit_and_wait(&dcb)) {
            return st_fail("reason=submit_or_eop case=label_no_eop");
        }
    }

    for (uint32_t i = 0; i < X_WORDS; ++i) {
        st_case_expect(&cases[CASE_X_VALUES], "x_final", i,
                       x_after(ROUNDS, i), x[i]);
    }
    st_case_expect(&cases[CASE_X_VALUES], "pred_a", PRED_A_WORD, 0, *pred_a);
    st_case_expect(&cases[CASE_X_VALUES], "pred_b", PRED_B_WORD, 1, *pred_b);

    return st_finish_cases(cases, CASES, "");
}
