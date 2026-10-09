/*
 * cond_exec_gpu_predicate: scenario A3 of docs/uma-e1c-e3-scenario-plan.md.
 *
 * COND_EXEC must evaluate a predicate that preceding GPU work wrote. In one
 * DCB:
 *
 *   dispatch P copies the new predicate value into pred
 *   [B]  (P has finished, its write is out of the shader caches)
 *   [P]  (the PFP does not read ahead of the ME)
 *   COND_EXEC(pred) guarding WRITE_DATA marker = 7
 *   WRITE_DATA sentinel = 9 (always runs; shows the skip length is exact)
 *   EOP
 *
 * pred starts at the opposite value, so reading it before P has run picks
 * the wrong branch. shadPS4 evaluates COND_EXEC while parsing. In mirror
 * Precise that read of a GPU-modified page downloads inline; with readbacks
 * Relaxed/Disabled it sees guest memory without P's write; with shared
 * backing it reads guest memory before P has executed (kaaburgh/shadPS4#11).
 *
 * Cases:
 *   gpu_pred_1 / gpu_pred_0   P writes 1 (marker expected) or 0 (skipped).
 *   cpu_pred_1 / cpu_pred_0   controls: the CPU writes the predicate before
 *                             submitting, no dispatch.
 *
 * marker and sentinel are CP-written, so every case is checkable without a
 * readback; the GPU cases still depend on the CP seeing P's write.
 */

#define SHADTEST_NAME "cond_exec_gpu_predicate"
#include "shadtest_guest.h"

enum {
    ARENA_BYTES = 2 * 1024 * 1024,
    SLOT_BYTES = 64 * 1024,
    /* P copies one workgroup of words; the predicate is word 0. */
    PRED_WORDS = ST_LOCAL_SIZE_X,
    BINDINGS = 2,
};

enum {
    CASE_GPU_PRED_1,
    CASE_GPU_PRED_0,
    CASE_CPU_PRED_1,
    CASE_CPU_PRED_0,
    CASES,
};

static const uint32_t MARKER_OLD = 0x0BADC0DEu;
static const uint32_t MARKER_NEW = 7;
static const uint32_t SENTINEL = 9;

typedef struct {
    volatile uint32_t* src;  /* P's source: the new predicate value */
    volatile uint32_t* pred; /* P's destination; COND_EXEC reads word 0 */
    volatile uint32_t* marker;
    volatile uint32_t* sentinel;
    GnmBuffer* table;
} Slot;

static bool alloc_slot(StArena* arena, GnmBuffer* table, Slot* s) {
    s->src = (volatile uint32_t*)st_arena_alloc(arena, SLOT_BYTES, SLOT_BYTES);
    s->pred = (volatile uint32_t*)st_arena_alloc(arena, SLOT_BYTES, SLOT_BYTES);
    volatile uint32_t* words =
        (volatile uint32_t*)st_arena_alloc(arena, SLOT_BYTES, SLOT_BYTES);
    if (!s->src || !s->pred || !words) {
        return false;
    }
    s->marker = words;
    s->sentinel = words + 1;
    s->table = table;
    s->table[0] = st_raw_buffer(s->src, PRED_WORDS * sizeof(uint32_t));
    s->table[1] = st_raw_buffer(s->pred, PRED_WORDS * sizeof(uint32_t));
    return true;
}

/* Runs one case: with a shader, P writes value over the opposite initial
 * predicate; without one, the CPU writes value before submitting. */
static bool run_case(StQueue* queue, const StComputeShader* shader,
                     const Slot* s, uint32_t value) {
    st_fill_words(s->src, PRED_WORDS, value);
    st_fill_words(s->pred, PRED_WORDS, shader ? !value : value);
    *s->marker = MARKER_OLD;
    *s->sentinel = 0;

    StDcb dcb;
    st_dcb_begin(&dcb, queue);
    if (shader) {
        st_dcb_dispatch(&dcb, shader, s->table, 1);
        st_dcb_barrier(&dcb, 0);
    }
    st_dcb_pfp_sync_me(&dcb);
    st_dcb_cond_exec(&dcb, s->pred, st_pm4_write_data_dwords(1));
    st_dcb_write_data(&dcb, s->marker, &MARKER_NEW, 1);
    st_dcb_write_data(&dcb, s->sentinel, &SENTINEL, 1);
    return st_dcb_submit_and_wait(&dcb);
}

static void check_case(StCase* c, const Slot* s, uint32_t value) {
    const uint32_t expected = value ? MARKER_NEW : MARKER_OLD;
    const uint32_t got = *s->marker;
    if (got != expected) {
        st_case_note(c, value ? "branch_skipped" : "branch_taken", 0, expected,
                     got);
    }
    st_case_expect(c, "sentinel", 1, SENTINEL, *s->sentinel);
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

    Slot slots[CASES];
    for (unsigned k = 0; k < CASES; ++k) {
        if (!alloc_slot(&arena, &tables[k * BINDINGS], &slots[k])) {
            return st_fail("reason=arena_memory");
        }
    }

    StCase cases[CASES] = {
        [CASE_GPU_PRED_1] = {.name = "gpu_pred_1"},
        [CASE_GPU_PRED_0] = {.name = "gpu_pred_0"},
        [CASE_CPU_PRED_1] = {.name = "cpu_pred_1"},
        [CASE_CPU_PRED_0] = {.name = "cpu_pred_0"},
    };
    const struct {
        bool gpu;
        uint32_t value;
    } runs[CASES] = {
        [CASE_GPU_PRED_1] = {true, 1},
        [CASE_GPU_PRED_0] = {true, 0},
        [CASE_CPU_PRED_1] = {false, 1},
        [CASE_CPU_PRED_0] = {false, 0},
    };

    for (unsigned k = 0; k < CASES; ++k) {
        if (!run_case(&queue, runs[k].gpu ? &copy : NULL, &slots[k],
                      runs[k].value)) {
            return st_fail("reason=submit_or_eop case=%s", cases[k].name);
        }
        check_case(&cases[k], &slots[k], runs[k].value);
    }

    return st_finish_cases(cases, CASES, "");
}
