/*
 * buffer_page_false_sharing: Stage 3 of docs/memory-uma-test-roadmap.md.
 *
 * The CPU and the GPU modify disjoint byte ranges, so there is no data race,
 * but in the adversarial layouts the ranges share a 4 KiB page, which is the
 * granularity at which shadPS4 tracks CPU- and GPU-modified memory.
 *
 * Each layout runs in its own fresh pages:
 *
 *   1. CPU fills the whole window (data pages, result page, params page) with
 *      a per-word guard pattern and writes the layout's salt to params.
 *   2. writer.comp writes region A = G(i). Wait for end of pipe.
 *   3. CPU writes region B = C(i) and does not touch A.
 *   4. checker.comp reads A and B and writes R = combine(A, B, i). Wait.
 *   5. CPU verifies R, A, B, the guard words and params.
 *
 * A host that keeps a per-page copy must not let the CPU write to B restore
 * an older copy of A, in either direction: the GPU must see G in step 4 and
 * the CPU must see G in step 5.
 *
 * Window layout (offsets from the window start, 6 pages):
 *
 *   0x0000-0x3fff  data pages: A and B at the layout's offsets, guard elsewhere
 *   0x4000-0x43ff  R (rest of the page is guard)
 *   0x5000         params: salt (rest of the page is guard)
 */

#define SHADTEST_NAME "buffer_page_false_sharing"
#include "shadtest_guest.h"

enum {
    REGION_WORDS = 256,
    REGION_BYTES = REGION_WORDS * sizeof(uint32_t),
    GROUPS_X = REGION_WORDS / ST_LOCAL_SIZE_X,
    WINDOW_PAGES = 6,
    WINDOW_BYTES = WINDOW_PAGES * ST_PAGE_BYTES,
    WINDOW_WORDS = WINDOW_BYTES / sizeof(uint32_t),
    RESULT_OFFSET = 4 * ST_PAGE_BYTES,
    PARAMS_OFFSET = 5 * ST_PAGE_BYTES,
    PARAMS_BYTES = sizeof(uint32_t),
    WRITER_BINDINGS = 2,
    CHECKER_BINDINGS = 3,
    ARENA_BYTES = 2 * 1024 * 1024,
};

typedef struct {
    const char* name;
    uint32_t a_offset;
    uint32_t b_offset;
} Layout;

/* Offsets into the window; A and B are REGION_BYTES (0x400) each and pages
 * are 0x1000. */
static const Layout LAYOUTS[] = {
    /* Control: A and B on different pages, with a guard page between. */
    {"separate_pages", 0x0000, 0x2000},
    /* A and B in disjoint parts of one page, with guard words between. */
    {"same_page", 0x1000, 0x1800},
    /* A ends exactly at a page boundary and B starts there. */
    {"adjacent_boundary", 0x1c00, 0x2000},
    /* A straddles the 0x2000 boundary and B shares A's second page. */
    {"straddle_boundary", 0x1e00, 0x2200},
};

enum { LAYOUT_COUNT = sizeof(LAYOUTS) / sizeof(LAYOUTS[0]) };

_Static_assert(REGION_WORDS % ST_LOCAL_SIZE_X == 0,
               "regions must be whole workgroups");
_Static_assert(REGION_BYTES == 0x400 && ST_PAGE_BYTES == 0x1000,
               "the layout offsets assume 1 KiB regions and 4 KiB pages");

static uint32_t guard_word(uint32_t salt, uint32_t window_word) {
    return 0x47554152u ^ (window_word * 0x9E3779B1u) ^ salt;
}

/* Must match writer.comp. */
static uint32_t expected_a(uint32_t salt, uint32_t i) {
    return (i * 0x01000193u) ^ (salt * 0x85EBCA6Bu) ^ 0xA0000000u;
}

static uint32_t cpu_b(uint32_t salt, uint32_t i) {
    return (i * 0x27D4EB2Fu) + (salt ^ 0xC0000000u);
}

/* Must match checker.comp. */
static uint32_t combine(uint32_t a, uint32_t b, uint32_t i) {
    return (a * 3u) ^ ((b << 7) | (b >> 25)) ^ (i * 0x165667B1u);
}

typedef struct {
    const Layout* layout;
    uint32_t salt;
    volatile uint32_t* window;
    volatile uint32_t* a;
    volatile uint32_t* b;
    volatile uint32_t* r;
    volatile uint32_t* params;
    GnmBuffer* writer_table;
    GnmBuffer* checker_table;
} Case;

typedef struct {
    const char* reason;
    uint32_t index;
    uint32_t expected;
    uint32_t got;
} Failure;

/* Mismatching words per checked area. */
enum { AREA_R, AREA_A, AREA_B, AREA_GUARD, AREA_COUNT };

typedef struct {
    Failure first;
    unsigned bad[AREA_COUNT];
} CaseResult;

static void note_failure(CaseResult* result, unsigned area, const char* reason,
                         uint32_t index, uint32_t expected, uint32_t got) {
    Failure* f = &result->first;
    if (!f->reason) {
        f->reason = reason;
        f->index = index;
        f->expected = expected;
        f->got = got;
    }
    result->bad[area] += 1;
}

static bool window_word_is_owned(const Case* c, uint32_t j) {
    const uint32_t a0 = c->layout->a_offset / sizeof(uint32_t);
    const uint32_t b0 = c->layout->b_offset / sizeof(uint32_t);
    const uint32_t r0 = RESULT_OFFSET / sizeof(uint32_t);
    const uint32_t p0 = PARAMS_OFFSET / sizeof(uint32_t);
    return (j >= a0 && j < a0 + REGION_WORDS) ||
           (j >= b0 && j < b0 + REGION_WORDS) ||
           (j >= r0 && j < r0 + REGION_WORDS) || j == p0;
}

static void prepare_case(Case* c) {
    for (uint32_t j = 0; j < WINDOW_WORDS; ++j) {
        c->window[j] = guard_word(c->salt, j);
    }
    c->params[0] = c->salt;

    c->writer_table[0] = st_raw_buffer(c->a, REGION_BYTES);
    c->writer_table[1] = st_raw_buffer(c->params, PARAMS_BYTES);
    c->checker_table[0] = st_raw_buffer(c->a, REGION_BYTES);
    c->checker_table[1] = st_raw_buffer(c->b, REGION_BYTES);
    c->checker_table[2] = st_raw_buffer(c->r, REGION_BYTES);
}

/* Checks everything the CPU can see after the checker dispatch. The first
 * failure is reported in priority order: what the GPU saw (R), then what the
 * CPU sees in A, B, the guard words and params. */
static void verify_case(const Case* c, CaseResult* result) {
    const uint32_t a0 = c->layout->a_offset / sizeof(uint32_t);
    const uint32_t b0 = c->layout->b_offset / sizeof(uint32_t);
    const uint32_t r0 = RESULT_OFFSET / sizeof(uint32_t);

    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        const uint32_t a = expected_a(c->salt, i);
        const uint32_t b = cpu_b(c->salt, i);
        const uint32_t old_a = guard_word(c->salt, a0 + i);
        const uint32_t old_b = guard_word(c->salt, b0 + i);
        const uint32_t expected = combine(a, b, i);
        const uint32_t got = c->r[i];
        if (got == expected) {
            continue;
        }

        const char* reason = "result_mismatch";
        if (got == combine(old_a, b, i)) {
            reason = "gpu_stale_a";
        } else if (got == combine(a, old_b, i)) {
            reason = "gpu_stale_b";
        } else if (got == combine(old_a, old_b, i)) {
            reason = "gpu_stale_ab";
        } else if (got == guard_word(c->salt, r0 + i)) {
            reason = "result_unwritten";
        }
        note_failure(result, AREA_R, reason, i, expected, got);
    }

    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        const uint32_t expected = expected_a(c->salt, i);
        const uint32_t got = c->a[i];
        if (got != expected) {
            note_failure(result, AREA_A,
                         got == guard_word(c->salt, a0 + i) ? "cpu_stale_a"
                                                            : "a_mismatch",
                         i, expected, got);
        }
    }

    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        const uint32_t expected = cpu_b(c->salt, i);
        const uint32_t got = c->b[i];
        if (got != expected) {
            note_failure(result, AREA_B,
                         got == guard_word(c->salt, b0 + i) ? "b_reverted"
                                                            : "b_mismatch",
                         i, expected, got);
        }
    }

    for (uint32_t j = 0; j < WINDOW_WORDS; ++j) {
        if (window_word_is_owned(c, j)) {
            continue;
        }
        const uint32_t expected = guard_word(c->salt, j);
        const uint32_t got = c->window[j];
        if (got != expected) {
            /* index is the byte offset in the window for guard words. */
            note_failure(result, AREA_GUARD, "guard",
                         j * (uint32_t)sizeof(uint32_t), expected, got);
        }
    }

    if (c->params[0] != c->salt) {
        note_failure(result, AREA_GUARD, "params", 0, c->salt, c->params[0]);
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

    StComputeShader writer;
    StComputeShader checker;
    if (!st_load_compute_shader(&arena, "/app0/assets/writer.comp.sb",
                                &writer)) {
        return st_fail("reason=shader_load shader=writer");
    }
    if (!st_load_compute_shader(&arena, "/app0/assets/checker.comp.sb",
                                &checker)) {
        return st_fail("reason=shader_load shader=checker");
    }

    /* Descriptor tables get their own page, apart from every window. */
    GnmBuffer* tables = (GnmBuffer*)st_arena_alloc(
        &arena,
        LAYOUT_COUNT * (WRITER_BINDINGS + CHECKER_BINDINGS) * sizeof(GnmBuffer),
        ST_PAGE_BYTES);
    if (!tables) {
        return st_fail("reason=descriptor_memory");
    }

    Case cases[LAYOUT_COUNT];
    for (unsigned k = 0; k < LAYOUT_COUNT; ++k) {
        uint8_t* window =
            (uint8_t*)st_arena_alloc(&arena, WINDOW_BYTES, ST_PAGE_BYTES);
        if (!window) {
            return st_fail("reason=window_memory");
        }
        Case* c = &cases[k];
        c->layout = &LAYOUTS[k];
        c->salt = 0x3C6EF372u * (k + 1);
        c->window = (volatile uint32_t*)window;
        c->a = (volatile uint32_t*)(window + LAYOUTS[k].a_offset);
        c->b = (volatile uint32_t*)(window + LAYOUTS[k].b_offset);
        c->r = (volatile uint32_t*)(window + RESULT_OFFSET);
        c->params = (volatile uint32_t*)(window + PARAMS_OFFSET);
        c->writer_table = tables + k * (WRITER_BINDINGS + CHECKER_BINDINGS);
        c->checker_table = c->writer_table + WRITER_BINDINGS;
    }

    unsigned failed_cases = 0;
    const Case* first_case = NULL;
    Failure first = {0};
    char failed_names[128] = "";
    uint32_t checksum = 0x811c9dc5u;

    for (unsigned k = 0; k < LAYOUT_COUNT; ++k) {
        Case* c = &cases[k];
        prepare_case(c);

        if (!st_dispatch_and_wait(&queue, &writer, c->writer_table, GROUPS_X)) {
            return st_fail("reason=dispatch stage=writer case=%s",
                           c->layout->name);
        }

        /* CPU-owned update: B only. A was just written by the GPU. */
        for (uint32_t i = 0; i < REGION_WORDS; ++i) {
            c->b[i] = cpu_b(c->salt, i);
        }

        if (!st_dispatch_and_wait(&queue, &checker, c->checker_table,
                                  GROUPS_X)) {
            return st_fail("reason=dispatch stage=checker case=%s",
                           c->layout->name);
        }

        CaseResult result = {0};
        verify_case(c, &result);
        const Failure* f = &result.first;
        if (f->reason) {
            printf(
                "case %s a=0x%04x b=0x%04x: FAIL bad_r=%u bad_a=%u bad_b=%u "
                "bad_guard=%u first %s index=%u expected=%08x got=%08x\n",
                c->layout->name, c->layout->a_offset, c->layout->b_offset,
                result.bad[AREA_R], result.bad[AREA_A], result.bad[AREA_B],
                result.bad[AREA_GUARD], f->reason, f->index, f->expected,
                f->got);
            if (!first_case) {
                first_case = c;
                first = *f;
            }
            failed_cases += 1;
            const size_t used = strlen(failed_names);
            snprintf(failed_names + used, sizeof(failed_names) - used, "%s%s",
                     used ? "," : "", c->layout->name);
        } else {
            printf("case %s a=0x%04x b=0x%04x: ok\n", c->layout->name,
                   c->layout->a_offset, c->layout->b_offset);
            checksum = fnv1a_words(checksum, c->r, REGION_WORDS);
        }
    }

    if (st_gnm_errors != 0) {
        return st_fail("reason=gnm_error count=%u", st_gnm_errors);
    }

    if (first_case) {
        return st_fail(
            "reason=%s case=%s index=%u expected=%08x got=%08x "
            "failed_cases=%u failed=%s",
            first.reason, first_case->layout->name, first.index, first.expected,
            first.got, failed_cases, failed_names);
    }

    st_emit_marker("PASS", "cases=%u region_words=%u checksum=%08x",
                   (unsigned)LAYOUT_COUNT, (unsigned)REGION_WORDS, checksum);

    st_unload_compute_shader(&checker);
    st_unload_compute_shader(&writer);
    st_arena_destroy(&arena);
    return 0;
}
