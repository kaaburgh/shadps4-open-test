/*
 * write_data_order: scenario A1 of docs/uma-e1c-e3-scenario-plan.md.
 *
 * WRITE_DATA must take effect in CP order relative to the dispatches before
 * and after it. In one DCB:
 *
 *   dispatch A copies X to outA
 *   [B]   (A has finished reading X before the CP changes it)
 *   WRITE_DATA to 12 words of X (start, middle, end)
 *   [B]   (B cannot hit a cached X)
 *   dispatch B copies X to outB
 *   EOP
 *
 * so outA must hold X before the write and outB X after it. shadPS4 runs
 * WRITE_DATA as a memcpy into guest memory while parsing. With the mirror,
 * A has already uploaded X at that point; with shared backing, A reads guest
 * memory only when it executes, after the memcpy (kaaburgh/shadPS4#11).
 *
 * Cases:
 *   large_a / large_b   X is a 32 KiB read-only SSBO (tracked BufferCache
 *                       path in the mirror); outA / outB checked.
 *   small_a / small_b   X is a 4 KiB UBO (in the mirror, shadPS4 copies
 *                       read-only bindings up to 16 KiB into its stream buffer
 *                       while recording). With shared backing both X variants
 *                       are bound from the shared backing.
 *   cpu_control         control: the same words written by the CPU between
 *                       two submissions.
 *   x_values            the final contents of every X. WRITE_DATA writes guest
 *                       memory directly, so this is checkable in every
 *                       configuration; outA/outB are shader outputs and need a
 *                       readback (or shared backing) to reach the guest.
 */

#define SHADTEST_NAME "write_data_order"
#include "shadtest_guest.h"

enum {
    ARENA_BYTES = 2 * 1024 * 1024,
    /* Every buffer in its own 64 KiB, so no two share a BufferCache block. */
    SLOT_BYTES = 64 * 1024,
    LARGE_WORDS = 32 * 1024 / sizeof(uint32_t),
    SMALL_WORDS = 4 * 1024 / sizeof(uint32_t),
    SPOTS = 3,
    SPOT_WORDS = 4,
    BINDINGS = 2,
};

_Static_assert(LARGE_WORDS % ST_LOCAL_SIZE_X == 0, "whole workgroups");
_Static_assert(SMALL_WORDS % ST_LOCAL_SIZE_X == 0, "whole workgroups");

enum {
    CASE_LARGE_A,
    CASE_LARGE_B,
    CASE_SMALL_A,
    CASE_SMALL_B,
    CASE_CPU_CONTROL,
    CASE_X_VALUES,
    CASES,
};

static const uint32_t OUTPUT_POISON = 0xcdcdcdcdu;

static uint32_t before(unsigned variant, uint32_t i) {
    return (i * 0x9E3779B1u) ^ (variant * 0x7F4A7C15u) ^ 0x0BADF00Du;
}

static uint32_t after(unsigned variant, uint32_t i) {
    return before(variant, i) ^ 0xA5A5A5A5u;
}

/* The first word of each WRITE_DATA spot: start, middle and end of X. */
static uint32_t spot_start(uint32_t words, unsigned spot) {
    const uint32_t starts[SPOTS] = {0, words / 2, words - SPOT_WORDS};
    return starts[spot];
}

static bool is_written(uint32_t words, uint32_t i) {
    for (unsigned s = 0; s < SPOTS; ++s) {
        const uint32_t start = spot_start(words, s);
        if (i >= start && i < start + SPOT_WORDS) {
            return true;
        }
    }
    return false;
}

static uint32_t final_value(unsigned variant, uint32_t words, uint32_t i) {
    return is_written(words, i) ? after(variant, i) : before(variant, i);
}

typedef struct {
    unsigned variant;
    uint32_t words;
    const StComputeShader* shader;
    volatile uint32_t* x;
    volatile uint32_t* out_a;
    volatile uint32_t* out_b;
    GnmBuffer* table_a;
    GnmBuffer* table_b;
} Variant;

static void prepare(Variant* v) {
    for (uint32_t i = 0; i < v->words; ++i) {
        v->x[i] = before(v->variant, i);
    }
    st_fill_words(v->out_a, v->words, OUTPUT_POISON);
    st_fill_words(v->out_b, v->words, OUTPUT_POISON);
    v->table_a[0] = st_raw_buffer(v->x, v->words * sizeof(uint32_t));
    v->table_a[1] = st_raw_buffer(v->out_a, v->words * sizeof(uint32_t));
    v->table_b[0] = st_raw_buffer(v->x, v->words * sizeof(uint32_t));
    v->table_b[1] = st_raw_buffer(v->out_b, v->words * sizeof(uint32_t));
}

static bool run_in_one_dcb(StQueue* queue, const Variant* v) {
    StDcb dcb;
    st_dcb_begin(&dcb, queue);
    st_dcb_dispatch(&dcb, v->shader, v->table_a, v->words / ST_LOCAL_SIZE_X);
    st_dcb_barrier(&dcb, 0);
    for (unsigned s = 0; s < SPOTS; ++s) {
        const uint32_t start = spot_start(v->words, s);
        uint32_t data[SPOT_WORDS];
        for (uint32_t k = 0; k < SPOT_WORDS; ++k) {
            data[k] = after(v->variant, start + k);
        }
        st_dcb_write_data(&dcb, v->x + start, data, SPOT_WORDS);
    }
    st_dcb_barrier(&dcb, 0);
    st_dcb_dispatch(&dcb, v->shader, v->table_b, v->words / ST_LOCAL_SIZE_X);
    return st_dcb_submit_and_wait(&dcb);
}

static void check_out_a(StCase* c, const Variant* v) {
    for (uint32_t i = 0; i < v->words; ++i) {
        const uint32_t expected = before(v->variant, i);
        const uint32_t got = v->out_a[i];
        if (got == expected) {
            continue;
        }
        const char* reason = "mismatch";
        if (got == OUTPUT_POISON) {
            reason = "unwritten";
        } else if (is_written(v->words, i) && got == after(v->variant, i)) {
            reason = "a_saw_later_write";
        }
        st_case_note(c, reason, i, expected, got);
    }
}

static void check_out_b(StCase* c, const Variant* v) {
    for (uint32_t i = 0; i < v->words; ++i) {
        const uint32_t expected = final_value(v->variant, v->words, i);
        const uint32_t got = v->out_b[i];
        if (got == expected) {
            continue;
        }
        const char* reason = "mismatch";
        if (got == OUTPUT_POISON) {
            reason = "unwritten";
        } else if (is_written(v->words, i) && got == before(v->variant, i)) {
            reason = "b_missed_write";
        }
        st_case_note(c, reason, i, expected, got);
    }
}

static void check_x(StCase* c, const Variant* v) {
    for (uint32_t i = 0; i < v->words; ++i) {
        st_case_expect(c, "x_final", i, final_value(v->variant, v->words, i),
                       v->x[i]);
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
    StComputeShader copy_ubo;
    if (!st_load_compute_shader(&arena, "/app0/assets/copy.comp.sb", &copy) ||
        !st_load_compute_shader(&arena, "/app0/assets/copy_ubo.comp.sb",
                                &copy_ubo)) {
        return st_fail("reason=shader_load");
    }

    GnmBuffer* tables = (GnmBuffer*)st_arena_alloc(
        &arena, 6 * BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    if (!tables) {
        return st_fail("reason=arena_memory");
    }

    Variant variants[3] = {
        {.variant = 1, .words = LARGE_WORDS, .shader = &copy},
        {.variant = 2, .words = SMALL_WORDS, .shader = &copy_ubo},
        {.variant = 3, .words = LARGE_WORDS, .shader = &copy},
    };
    for (unsigned k = 0; k < 3; ++k) {
        Variant* v = &variants[k];
        v->x = (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
        v->out_a =
            (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
        v->out_b =
            (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
        v->table_a = &tables[(2 * k + 0) * BINDINGS];
        v->table_b = &tables[(2 * k + 1) * BINDINGS];
        if (!v->x || !v->out_a || !v->out_b) {
            return st_fail("reason=arena_memory");
        }
        prepare(v);
    }
    Variant* const large = &variants[0];
    Variant* const small = &variants[1];
    Variant* const control = &variants[2];

    StCase cases[CASES] = {
        [CASE_LARGE_A] = {.name = "large_a"},
        [CASE_LARGE_B] = {.name = "large_b"},
        [CASE_SMALL_A] = {.name = "small_a"},
        [CASE_SMALL_B] = {.name = "small_b"},
        [CASE_CPU_CONTROL] = {.name = "cpu_control"},
        [CASE_X_VALUES] = {.name = "x_values"},
    };

    if (!run_in_one_dcb(&queue, large)) {
        return st_fail("reason=submit_or_eop case=large");
    }
    check_out_a(&cases[CASE_LARGE_A], large);
    check_out_b(&cases[CASE_LARGE_B], large);

    if (!run_in_one_dcb(&queue, small)) {
        return st_fail("reason=submit_or_eop case=small");
    }
    check_out_a(&cases[CASE_SMALL_A], small);
    check_out_b(&cases[CASE_SMALL_B], small);

    /* cpu_control: A, then the CPU writes the spots, then B. */
    if (!st_dispatch_and_wait(&queue, control->shader, control->table_a,
                              control->words / ST_LOCAL_SIZE_X)) {
        return st_fail("reason=submit_or_eop case=cpu_control");
    }
    for (uint32_t i = 0; i < control->words; ++i) {
        if (is_written(control->words, i)) {
            control->x[i] = after(control->variant, i);
        }
    }
    if (!st_dispatch_and_wait(&queue, control->shader, control->table_b,
                              control->words / ST_LOCAL_SIZE_X)) {
        return st_fail("reason=submit_or_eop case=cpu_control");
    }
    check_out_a(&cases[CASE_CPU_CONTROL], control);
    check_out_b(&cases[CASE_CPU_CONTROL], control);

    for (unsigned k = 0; k < 3; ++k) {
        check_x(&cases[CASE_X_VALUES], &variants[k]);
    }

    return st_finish_cases(cases, CASES, "");
}
