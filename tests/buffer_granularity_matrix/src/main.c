/*
 * buffer_granularity_matrix: Stage 5 of docs/memory-uma-test-roadmap.md.
 *
 * Runs the Stage 3 false-sharing operation over a matrix of offsets, sizes,
 * alignments and sparse vs contiguous ranges, three generations each:
 *
 *   for generation n = 1..3:
 *     1. writer.comp: element k of A = G(k, salt, n)
 *     2. CPU: element j of B = C(j, salt, n); A is not read first
 *     3. checker.comp: R = fa(A elements) followed by fb(B elements)
 *     4. CPU verifies R, A, B and every other word of the case's window
 *
 * An element is one word; A element k is the word at a_offset + 4*k*a_stride,
 * so a stride of 1024 touches one word per 4 KiB page. A and B never share a
 * word, but in most cases they share pages. Each case has a fresh window with
 * guard words everywhere A and B do not write, starting and ending with a
 * page the case never touches. R and the parameters live outside the window.
 *
 * The cases cover sizes from one word to 64 KiB, so some bindings are small
 * and some are not; the oracle is the same for all of them.
 */

#define SHADTEST_NAME "buffer_granularity_matrix"
#include "shadtest_guest.h"

enum {
    GENERATIONS = 3,
    WORD_BYTES = sizeof(uint32_t),
    PAGE_WORDS = ST_PAGE_BYTES / WORD_BYTES,
    WRITER_BINDINGS = 2,
    CHECKER_BINDINGS = 4,
    PARAMS_WORDS = 6,
    ARENA_BYTES = 4 * 1024 * 1024,
};

static const uint32_t RESULT_POISON = 0xcdcdcdcdu;

typedef struct {
    const char* name;
    uint32_t a_offset; /* bytes from the window start */
    uint32_t a_count;  /* elements */
    uint32_t a_stride; /* words between elements */
    uint32_t b_offset;
    uint32_t b_count;
    uint32_t b_stride;
} Layout;

static const Layout LAYOUTS[] = {
    /* Control: A and B on separate pages with a page between them. */
    {"separate_pages", 0x1000, 256, 1, 0x3000, 256, 1},
    /* One word each, next to each other. */
    {"word_pair", 0x1000, 1, 1, 0x1004, 1, 1},
    /* 15 words each, B at a 4-byte (not 16-byte) aligned offset. */
    {"small_unaligned", 0x1000, 15, 1, 0x1044, 15, 1},
    /* A ends exactly at a page boundary and B starts there. */
    {"page_boundary", 0x1f00, 64, 1, 0x2000, 64, 1},
    /* Two words each; A's second word is the first word of the next page. */
    {"straddle_word", 0x1ffc, 2, 1, 0x2004, 2, 1},
    /* One page plus one word each: A's last word and B's first share a page. */
    {"page_plus_word", 0x1000, 1025, 1, 0x2004, 1025, 1},
    /* 1 KiB each at 4- and 16-byte aligned offsets in one page. */
    {"aligned_4", 0x1004, 256, 1, 0x1404, 256, 1},
    {"aligned_16", 0x1010, 256, 1, 0x1810, 256, 1},
    /* 16 KiB each: page-aligned and separate, then sharing a page. */
    {"16k_separate", 0x1000, 4096, 1, 0x6000, 4096, 1},
    {"16k_shared_page", 0x1800, 4096, 1, 0x5800, 4096, 1},
    /* 64 KiB each, sharing the page where A ends. */
    {"64k_shared_page", 0x1c00, 16384, 1, 0x11c00, 16384, 1},
    /* One word of A and one of B in each of 16 pages. */
    {"sparse_interleaved", 0x1000, 16, 1024, 0x1008, 16, 1024},
    /* A one word per page; B a short contiguous run in A's first page. */
    {"sparse_gpu_dense_cpu", 0x1000, 8, 1024, 0x1010, 8, 1},
    /* A a contiguous 4 KiB run; B one word per page, the first in A's page. */
    {"dense_gpu_sparse_cpu", 0x1800, 1024, 1, 0x2c00, 4, 1024},
};

enum { LAYOUT_COUNT = sizeof(LAYOUTS) / sizeof(LAYOUTS[0]) };

static uint32_t guard_word(uint32_t salt, uint32_t window_word) {
    return 0x47554152u ^ (window_word * 0x9E3779B1u) ^ salt;
}

/* Must match writer.comp. */
static uint32_t gpu_value(uint32_t k, uint32_t salt, uint32_t generation) {
    return (k * 0x01000193u) ^ (salt * 0x85EBCA6Bu) ^
           (generation * 0x9E3779B1u) ^ 0xA0000000u;
}

static uint32_t cpu_value(uint32_t j, uint32_t salt, uint32_t generation) {
    return (j * 0x27D4EB2Fu) + (salt ^ 0xC0000000u) + generation * 0x7F4A7C15u;
}

/* Must match checker.comp. */
static uint32_t result_a(uint32_t a, uint32_t i) {
    return (a * 3u) ^ (i * 0x165667B1u);
}

static uint32_t result_b(uint32_t b, uint32_t j) {
    return ((b << 7) | (b >> 25)) ^ (j * 0x2545F491u) ^ 0x6A09E667u;
}

static uint32_t span_words(uint32_t count, uint32_t stride) {
    return (count - 1) * stride + 1;
}

typedef struct {
    const Layout* layout;
    uint32_t salt;
    uint32_t window_words;
    volatile uint32_t* window;
    volatile uint32_t* a;
    volatile uint32_t* b;
    volatile uint32_t* r;
    volatile uint32_t* params;
    uint8_t* owned; /* host-side: 1 for each window word that is A or B */
    GnmBuffer* writer_table;
    GnmBuffer* checker_table;
} Case;

typedef struct {
    const char* reason;
    unsigned generation;
    uint32_t index;
    uint32_t expected;
    uint32_t got;
} Failure;

enum { AREA_R, AREA_A, AREA_B, AREA_GUARD, AREA_COUNT };

typedef struct {
    Failure first;
    unsigned bad[AREA_COUNT];
} CaseResult;

static void note_failure(CaseResult* result, unsigned area, const char* reason,
                         unsigned generation, uint32_t index,
                         uint32_t expected, uint32_t got) {
    Failure* f = &result->first;
    if (!f->reason) {
        f->reason = reason;
        f->generation = generation;
        f->index = index;
        f->expected = expected;
        f->got = got;
    }
    result->bad[area] += 1;
}

/* The value an A word holds before generation n writes it. */
static uint32_t previous_a(const Case* c, uint32_t k, unsigned generation) {
    const uint32_t word =
        c->layout->a_offset / WORD_BYTES + k * c->layout->a_stride;
    return generation == 1 ? guard_word(c->salt, word)
                           : gpu_value(k, c->salt, generation - 1);
}

static uint32_t previous_b(const Case* c, uint32_t j, unsigned generation) {
    const uint32_t word =
        c->layout->b_offset / WORD_BYTES + j * c->layout->b_stride;
    return generation == 1 ? guard_word(c->salt, word)
                           : cpu_value(j, c->salt, generation - 1);
}

/* Checks what the GPU saw (R), then what the CPU sees in A, B and the rest of
 * the window, for generation n. */
static void verify_generation(const Case* c, unsigned n, CaseResult* result) {
    const Layout* l = c->layout;

    for (uint32_t k = 0; k < l->a_count; ++k) {
        const uint32_t expected = result_a(gpu_value(k, c->salt, n), k);
        const uint32_t got = c->r[k];
        if (got == expected) {
            continue;
        }
        const char* reason = "result_mismatch";
        if (got == RESULT_POISON) {
            reason = "result_unwritten";
        } else if (got == result_a(previous_a(c, k, n), k)) {
            reason = "gpu_stale_a";
        }
        note_failure(result, AREA_R, reason, n, k, expected, got);
    }
    for (uint32_t j = 0; j < l->b_count; ++j) {
        const uint32_t i = l->a_count + j;
        const uint32_t expected = result_b(cpu_value(j, c->salt, n), j);
        const uint32_t got = c->r[i];
        if (got == expected) {
            continue;
        }
        const char* reason = "result_mismatch";
        if (got == RESULT_POISON) {
            reason = "result_unwritten";
        } else if (got == result_b(previous_b(c, j, n), j)) {
            reason = "gpu_stale_b";
        }
        note_failure(result, AREA_R, reason, n, i, expected, got);
    }

    for (uint32_t k = 0; k < l->a_count; ++k) {
        const uint32_t expected = gpu_value(k, c->salt, n);
        const uint32_t got = c->a[k * l->a_stride];
        if (got != expected) {
            note_failure(result, AREA_A,
                         got == previous_a(c, k, n) ? "cpu_stale_a"
                                                    : "a_mismatch",
                         n, k, expected, got);
        }
    }
    for (uint32_t j = 0; j < l->b_count; ++j) {
        const uint32_t expected = cpu_value(j, c->salt, n);
        const uint32_t got = c->b[j * l->b_stride];
        if (got != expected) {
            note_failure(result, AREA_B,
                         got == previous_b(c, j, n) ? "b_reverted"
                                                    : "b_mismatch",
                         n, j, expected, got);
        }
    }

    for (uint32_t w = 0; w < c->window_words; ++w) {
        if (c->owned[w]) {
            continue;
        }
        const uint32_t expected = guard_word(c->salt, w);
        const uint32_t got = c->window[w];
        if (got != expected) {
            /* index is the byte offset in the window for guard words. */
            note_failure(result, AREA_GUARD, "guard", n, w * WORD_BYTES,
                         expected, got);
        }
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

static uint32_t groups_for(uint32_t invocations) {
    return (invocations + ST_LOCAL_SIZE_X - 1) / ST_LOCAL_SIZE_X;
}

/* Sets up case k: window, R, params and descriptor tables. */
static bool setup_case(StArena* arena, unsigned k, Case* c) {
    const Layout* l = &LAYOUTS[k];
    const uint32_t a_end =
        l->a_offset + span_words(l->a_count, l->a_stride) * WORD_BYTES;
    const uint32_t b_end =
        l->b_offset + span_words(l->b_count, l->b_stride) * WORD_BYTES;
    const uint32_t end = a_end > b_end ? a_end : b_end;
    /* Up to the end of the last touched page, plus one untouched page. */
    const uint32_t window_bytes =
        (uint32_t)st_align_up(end, ST_PAGE_BYTES) + ST_PAGE_BYTES;
    const uint32_t r_words = l->a_count + l->b_count;

    c->layout = l;
    c->salt = 0x3C6EF372u * (k + 1);
    c->window_words = window_bytes / WORD_BYTES;
    uint8_t* window =
        (uint8_t*)st_arena_alloc(arena, window_bytes, ST_PAGE_BYTES);
    c->r = (volatile uint32_t*)st_arena_alloc(arena, r_words * WORD_BYTES,
                                              ST_PAGE_BYTES);
    c->params = (volatile uint32_t*)st_arena_alloc(
        arena, PARAMS_WORDS * WORD_BYTES, ST_PAGE_BYTES);
    c->writer_table = (GnmBuffer*)st_arena_alloc(
        arena, (WRITER_BINDINGS + CHECKER_BINDINGS) * sizeof(GnmBuffer), 256);
    c->owned = (uint8_t*)calloc(c->window_words, 1);
    if (!window || !c->r || !c->params || !c->writer_table || !c->owned) {
        return false;
    }
    c->window = (volatile uint32_t*)window;
    c->a = (volatile uint32_t*)(window + l->a_offset);
    c->b = (volatile uint32_t*)(window + l->b_offset);
    c->checker_table = c->writer_table + WRITER_BINDINGS;

    for (uint32_t k2 = 0; k2 < l->a_count; ++k2) {
        c->owned[l->a_offset / WORD_BYTES + k2 * l->a_stride] = 1;
    }
    for (uint32_t j = 0; j < l->b_count; ++j) {
        const uint32_t w = l->b_offset / WORD_BYTES + j * l->b_stride;
        if (c->owned[w]) {
            printf("layout %s: A and B overlap at word %u\n", l->name, w);
            return false;
        }
        c->owned[w] = 1;
    }

    for (uint32_t w = 0; w < c->window_words; ++w) {
        c->window[w] = guard_word(c->salt, w);
    }

    const uint32_t a_bytes = span_words(l->a_count, l->a_stride) * WORD_BYTES;
    const uint32_t b_bytes = span_words(l->b_count, l->b_stride) * WORD_BYTES;
    c->writer_table[0] = st_raw_buffer(c->a, a_bytes);
    c->writer_table[1] = st_raw_buffer(c->params, PARAMS_WORDS * WORD_BYTES);
    c->checker_table[0] = st_raw_buffer(c->a, a_bytes);
    c->checker_table[1] = st_raw_buffer(c->b, b_bytes);
    c->checker_table[2] = st_raw_buffer(c->r, r_words * WORD_BYTES);
    c->checker_table[3] = st_raw_buffer(c->params, PARAMS_WORDS * WORD_BYTES);
    return true;
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

    unsigned failed_cases = 0;
    const Layout* first_layout = NULL;
    Failure first = {0};
    char failed_names[192] = "";
    uint32_t checksum = 0x811c9dc5u;

    for (unsigned k = 0; k < LAYOUT_COUNT; ++k) {
        Case c;
        if (!setup_case(&arena, k, &c)) {
            return st_fail("reason=setup case=%s", LAYOUTS[k].name);
        }
        const Layout* l = c.layout;
        const uint32_t r_words = l->a_count + l->b_count;
        CaseResult result = {0};

        /* Every generation runs even after a failure, so the line shows how
         * the case behaves across repeated transitions. */
        for (unsigned n = 1; n <= GENERATIONS; ++n) {
            c.params[0] = c.salt;
            c.params[1] = n;
            c.params[2] = l->a_count;
            c.params[3] = l->a_stride;
            c.params[4] = l->b_count;
            c.params[5] = l->b_stride;
            st_fill_words(c.r, r_words, RESULT_POISON);

            if (!st_dispatch_and_wait(&queue, &writer, c.writer_table,
                                      groups_for(l->a_count))) {
                return st_fail("reason=dispatch stage=writer case=%s gen=%u",
                               l->name, n);
            }

            /* CPU-owned update: B only. A was just written by the GPU. */
            for (uint32_t j = 0; j < l->b_count; ++j) {
                c.b[j * l->b_stride] = cpu_value(j, c.salt, n);
            }

            if (!st_dispatch_and_wait(&queue, &checker, c.checker_table,
                                      groups_for(r_words))) {
                return st_fail("reason=dispatch stage=checker case=%s gen=%u",
                               l->name, n);
            }

            verify_generation(&c, n, &result);
        }

        const Failure* f = &result.first;
        if (f->reason) {
            printf(
                "case %s a=0x%x+%ux%u b=0x%x+%ux%u: FAIL bad_r=%u bad_a=%u "
                "bad_b=%u bad_guard=%u first gen=%u %s index=%u "
                "expected=%08x got=%08x\n",
                l->name, l->a_offset, l->a_count, l->a_stride, l->b_offset,
                l->b_count, l->b_stride, result.bad[AREA_R],
                result.bad[AREA_A], result.bad[AREA_B], result.bad[AREA_GUARD],
                f->generation, f->reason, f->index, f->expected, f->got);
            if (!first_layout) {
                first_layout = l;
                first = *f;
            }
            failed_cases += 1;
            const size_t used = strlen(failed_names);
            snprintf(failed_names + used, sizeof(failed_names) - used, "%s%s",
                     used ? "," : "", l->name);
        } else {
            printf("case %s a=0x%x+%ux%u b=0x%x+%ux%u: ok\n", l->name,
                   l->a_offset, l->a_count, l->a_stride, l->b_offset,
                   l->b_count, l->b_stride);
            checksum = fnv1a_words(checksum, c.r, r_words);
        }
        free(c.owned);
    }

    if (st_gnm_errors != 0) {
        return st_fail("reason=gnm_error count=%u", st_gnm_errors);
    }

    if (first_layout) {
        return st_fail(
            "reason=%s case=%s gen=%u index=%u expected=%08x got=%08x "
            "failed_cases=%u failed=%s",
            first.reason, first_layout->name, first.generation, first.index,
            first.expected, first.got, failed_cases, failed_names);
    }

    st_emit_marker("PASS", "cases=%u generations=%u checksum=%08x",
                   (unsigned)LAYOUT_COUNT, (unsigned)GENERATIONS, checksum);

    st_unload_compute_shader(&checker);
    st_unload_compute_shader(&writer);
    st_arena_destroy(&arena);
    return 0;
}
