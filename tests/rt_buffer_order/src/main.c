/*
 * rt_buffer_order: a linear render target T at memory M, accessed in one DCB
 * by draws, dispatches and DMA in the orders below. shadPS4 records each draw
 * into an image, and each case checks that the accesses after it see it.
 * Every draw is an R-only draw of red.frag (R = 90, G/B/A kept), over words
 * the CPU wrote to M before the DCB.
 *
 * Cases ([B] is the plan's barrier, with CB flush after a draw):
 *   draw_then_dispatch  draw; [B]; dispatch copies M to a buffer O.
 *                       O holds the draw's result.
 *   dma_over_draw       draw; [B]; DMA copies 256 CPU bytes D to the start
 *                       of M; [B]; dispatch copies M to O. O holds D, then
 *                       the draw's result.
 *   fill_over_draw      draw; [B]; DMA fills M; [B]; draw. T holds the fill
 *                       with R = 90.
 *   copy_into_draw      draw; [B]; DMA copies D to 4 KiB into M; [B]; draw.
 *                       T holds R = 90 over D there and over the CPU's words
 *                       elsewhere.
 *   write_over_draw     draw; [B]; dispatch copies a buffer P to a range
 *                       that starts 16 KiB before M and covers it; [B];
 *                       dispatch copies M to O. O holds P's part over M.
 *   partial_write_over_draw
 *                       as write_over_draw, but the first dispatch only
 *                       writes the 16 KiB before M, though its destination
 *                       still covers M. O holds the draw's result.
 *
 * draw_then_dispatch, dma_over_draw and the two write cases check O, a
 * shader output, so they need readbacks Precise. The other cases read T
 * through shadPS4's linear-image readback.
 */

#define SHADTEST_NAME "rt_buffer_order"
#include "shadtest_guest.h"

#include <gnm_rendertarget.h>

enum {
    WIDTH = 64,
    HEIGHT = 128,
    RED_VALUE = 90, /* red.frag.glsl writes 90 / 255 */
    ARENA_BYTES = 4 * 1024 * 1024,
    SLOT_BYTES = 64 * 1024,
    TARGET_BYTES = WIDTH * HEIGHT * 4,
    WORDS = WIDTH * HEIGHT,
    DMA_WORDS = 64,
    DMA_OFFSET_WORDS = 1024, /* copy_into_draw: 4 KiB into M */
    LEAD_BYTES = 16 * 1024,  /* write cases: P's range starts this far before M */
    BINDINGS = 2,
};

_Static_assert(TARGET_BYTES > 16 * 1024,
               "targets must be larger than shadPS4's stream threshold");
_Static_assert(LEAD_BYTES + TARGET_BYTES <= SLOT_BYTES, "write case slot");

enum {
    CASE_DRAW_THEN_DISPATCH,
    CASE_DMA_OVER_DRAW,
    CASE_FILL_OVER_DRAW,
    CASE_COPY_INTO_DRAW,
    CASE_WRITE_OVER_DRAW,
    CASE_PARTIAL_WRITE_OVER_DRAW,
    CASES,
};

static const uint32_t TARGET_POISON = 0xcdcdcdcdu;
static const uint32_t COPY_POISON = 0xababababu;
static const uint32_t FILL_VALUE = 0x3c4d5e6fu;

static uint32_t source_word(unsigned seed, uint32_t i) {
    return (i * 0x9E3779B1u) ^ (seed * 0x7F4A7C15u) ^ 0x0BADF00Du;
}

static uint32_t dma_word(uint32_t i) {
    return (i * 0x2545F491u) ^ 0x5bd1e995u;
}

static uint32_t p_word(uint32_t i) {
    return (i * 0x85EBCA6Bu) ^ 0xC2B2AE35u;
}

static uint32_t red_over(uint32_t word) {
    return (word & 0xffffff00u) | RED_VALUE;
}

/* ------------------------------------------------------------------------ */
/* Graphics shaders (vertex/pixel), same loading as gpu_solid_rt             */
/* ------------------------------------------------------------------------ */

typedef struct {
    void* stage;
    void* gpu_code;
} GraphicsShader;

static bool load_graphics_shader(StArena* arena, const char* path,
                                 GnmShaderType expected_type,
                                 GraphicsShader* out) {
    memset(out, 0, sizeof(*out));

    uint8_t* file = NULL;
    size_t file_size = 0;
    if (!st_read_file(path, &file, &file_size)) {
        return false;
    }

    size_t shader_offset = 0;
    bool wrapped_container = false;
    if (file_size < sizeof(GnmShaderFileHeader)) {
        free(file);
        return false;
    }
    const GnmShaderFileHeader* header = (const GnmShaderFileHeader*)file;
    if (header->magic != GNM_SHADER_FILE_HEADER_ID) {
        if (file_size < 0x24 + sizeof(GnmShaderFileHeader)) {
            free(file);
            return false;
        }
        header = (const GnmShaderFileHeader*)(file + 0x24);
        if (header->magic != GNM_SHADER_FILE_HEADER_ID) {
            free(file);
            return false;
        }
        shader_offset = 0x24;
        wrapped_container = true;
    }
    if ((GnmShaderType)header->type != expected_type) {
        printf("%s has unexpected shader type %u\n", path, header->type);
        free(file);
        return false;
    }

    const uint8_t* shader_base = file + shader_offset;
    const size_t shader_size = file_size - shader_offset;
    const size_t stage_header_bytes = header->headersizedwords
                                          ? (size_t)header->headersizedwords * 4
                                          : sizeof(GnmShaderCommonData);
    if (sizeof(GnmShaderFileHeader) + stage_header_bytes > shader_size ||
        stage_header_bytes < sizeof(GnmShaderCommonData)) {
        free(file);
        return false;
    }

    const size_t stage_struct_bytes = expected_type == GNM_SHADER_VERTEX
                                          ? sizeof(GnmVsShader)
                                          : sizeof(GnmPsShader);
    if (stage_header_bytes < stage_struct_bytes) {
        free(file);
        return false;
    }

    const GnmShaderCommonData* common =
        (const GnmShaderCommonData*)(shader_base + sizeof(GnmShaderFileHeader));
    uint32_t stage_size = 0;
    const void* code_src = NULL;
    /* The draws set up no vertex exports or pixel interpolants. */
    bool unexpected_io = false;
    if (expected_type == GNM_SHADER_VERTEX) {
        const GnmVsShader* vs = (const GnmVsShader*)common;
        stage_size = sceGnmVsShaderCalcSize(vs);
        code_src = sceGnmVsShaderCodePtr(vs);
        unexpected_io = vs->numexportsemantics != 0;
    } else {
        const GnmPsShader* ps = (const GnmPsShader*)common;
        stage_size = sceGnmPsShaderCalcSize(ps);
        code_src = sceGnmPsShaderCodePtr(ps);
        unexpected_io = ps->numinputsemantics != 0;
    }
    if (unexpected_io) {
        printf("%s has unexpected shader IO\n", path);
        free(file);
        return false;
    }
    if (stage_size > stage_header_bytes) {
        free(file);
        return false;
    }

    uint32_t code_size = sceGnmShaderCommonCodeSize(common);
    if (wrapped_container) {
        uint32_t container_code_size = 0;
        memcpy(&container_code_size, file + 0x10, sizeof(container_code_size));
        if (container_code_size < sizeof(GnmShaderBinaryInfo) ||
            shader_offset + container_code_size > file_size) {
            free(file);
            return false;
        }
        const GnmShaderBinaryInfo* binary_info =
            (const GnmShaderBinaryInfo*)(shader_base + container_code_size -
                                         sizeof(GnmShaderBinaryInfo));
        code_size = binary_info->length;
    }
    if (!st_range_inside(shader_base, shader_size, code_src, code_size)) {
        free(file);
        return false;
    }

    uint32_t copy_size = code_size;
    const uint8_t* after_code = (const uint8_t*)code_src + code_size;
    if (st_range_inside(shader_base, shader_size, after_code,
                        sizeof(GnmShaderBinaryInfo)) &&
        memcmp(after_code, GNM_SHADER_BINARY_INFO_MAGIC, 7) == 0) {
        copy_size += sizeof(GnmShaderBinaryInfo);
    }

    void* stage_copy = malloc(stage_size);
    void* gpu_code =
        st_arena_alloc(arena, copy_size, GNM_ALIGNMENT_SHADER_BYTES);
    if (!stage_copy || !gpu_code) {
        free(stage_copy);
        free(file);
        return false;
    }
    memcpy(stage_copy, common, stage_size);
    memcpy(gpu_code, code_src, copy_size);
    if (expected_type == GNM_SHADER_VERTEX) {
        sceGnmVsRegsSetAddress(&((GnmVsShader*)stage_copy)->registers,
                               gpu_code);
    } else {
        sceGnmPsRegsSetAddress(&((GnmPsShader*)stage_copy)->registers,
                               gpu_code);
    }

    out->stage = stage_copy;
    out->gpu_code = gpu_code;
    free(file);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Render target and draw                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
    GnmRenderTarget rt;
    volatile uint32_t* pixels;
    uint32_t pitch;
    uint32_t words; /* pitch * HEIGHT */
} Target;

/* Sets up a 64x128 R8G8B8A8 linear target at memory, which must hold its
 * bytes, and fills it with TARGET_POISON. */
static bool create_target(void* memory, size_t memory_bytes, Target* t) {
    /* sceGnmCreateRenderTarget leaves CMASK/FMASK/fast-clear fields alone
     * (and reads tilemode_index before writing it), so start from zero. */
    memset(t, 0, sizeof(*t));
    const GnmRenderTargetCreateInfo info = {
        .colorfmt = GNM_FMT_R8G8B8A8_UNORM,
        .width = WIDTH,
        .height = HEIGHT,
        .pitch = WIDTH,
        .numslices = 1,
        .numsamples = 1,
        .numfragments = 1,
        .colortilemodehint = GNM_TM_DISPLAY_LINEAR_ALIGNED,
        .mingpumode = GNM_GPU_BASE,
    };
    if (sceGnmCreateRenderTarget(&t->rt, &info) != GNM_ERROR_OK) {
        return false;
    }
    uint64_t size = 0;
    uint32_t alignment = 0;
    if (sceGnmRtCalcByteSize(&size, &alignment, &t->rt) != GNM_ERROR_OK ||
        size == 0 || size > memory_bytes ||
        (uintptr_t)memory % (alignment ? alignment : 1) != 0) {
        return false;
    }
    sceGnmRtSetBaseAddr(&t->rt, memory);
    t->pixels = (volatile uint32_t*)memory;
    t->pitch = sceGnmRtGetPitch(&t->rt);
    t->words = t->pitch * HEIGHT;
    if (t->pitch != WIDTH || (uint64_t)t->words * sizeof(uint32_t) > size) {
        return false;
    }
    st_fill_words(t->pixels, t->words, TARGET_POISON);
    return true;
}

/* Records the R-only draw of red.frag into t. */
static void record_masked_draw(StDcb* dcb, Target* t, GraphicsShader* vs,
                               GraphicsShader* ps) {
    GnmCommandBuffer* cmd = &dcb->cmd;
    const GnmPrimitiveSetup primitive = {
        .cullmode = GNM_CULL_NONE,
        .frontface = GNM_FACE_CCW,
        .frontmode = GNM_FILL_SOLID,
        .backmode = GNM_FILL_SOLID,
        .provokemode = GNM_PROVOKINGVTX_FIRST,
    };
    sceGnmDrawCmdSetPrimitiveSetup(cmd, &primitive);
    sceGnmDrawCmdSetRenderTarget(cmd, 0, &t->rt);
    sceGnmDrawCmdSetRenderTargetMask(cmd, 0x01);
    sceGnmDrawCmdSetScreenScissor(cmd, 0, 0, WIDTH, HEIGHT);
    const GnmSetViewportInfo viewport = {
        .dmin = 0.0f,
        .dmax = 1.0f,
        .scale = {WIDTH * 0.5f, -HEIGHT * 0.5f, 0.5f},
        .offset = {WIDTH * 0.5f, HEIGHT * 0.5f, 0.5f},
    };
    sceGnmDrawCmdSetViewport(cmd, 0, &viewport);
    sceGnmDrawCmdSetVsShader(cmd, &((GnmVsShader*)vs->stage)->registers, 0);
    sceGnmDrawCmdSetPsShader(cmd, &((GnmPsShader*)ps->stage)->registers);
    sceGnmDrawCmdSetPrimitiveType(cmd, GNM_PT_TRILIST);
    sceGnmDrawCmdDrawIndexAuto(cmd, 3);
}

/* What T (or a copy of it) must hold at word i, before any check. */
static uint32_t expected_word(unsigned k, uint32_t i) {
    const uint32_t under = source_word(k, i);
    switch (k) {
    case CASE_DMA_OVER_DRAW:
        return i < DMA_WORDS ? dma_word(i) : red_over(under);
    case CASE_FILL_OVER_DRAW:
        return red_over(FILL_VALUE);
    case CASE_COPY_INTO_DRAW:
        if (i >= DMA_OFFSET_WORDS && i < DMA_OFFSET_WORDS + DMA_WORDS) {
            return red_over(dma_word(i - DMA_OFFSET_WORDS));
        }
        return red_over(under);
    case CASE_WRITE_OVER_DRAW:
        return p_word(LEAD_BYTES / sizeof(uint32_t) + i);
    default:
        return red_over(under);
    }
}

static void check_words(StCase* c, unsigned k, const volatile uint32_t* words,
                        uint32_t poison) {
    for (uint32_t i = 0; i < WORDS; ++i) {
        const uint32_t expected = expected_word(k, i);
        const uint32_t got = words[i];
        if (got == expected) {
            continue;
        }
        const char* reason = "mismatch";
        if (got == poison) {
            reason = "unwritten";
        } else if (got == source_word(k, i)) {
            reason = "saw_cpu_words";
        } else if (got == red_over(source_word(k, i))) {
            reason = "saw_first_draw";
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
    GraphicsShader vs;
    GraphicsShader red_ps;
    if (!st_load_compute_shader(&arena, "/app0/assets/copy.comp.sb", &copy) ||
        !load_graphics_shader(&arena, "/app0/assets/fullscreen.vert.sb",
                              GNM_SHADER_VERTEX, &vs) ||
        !load_graphics_shader(&arena, "/app0/assets/red.frag.sb",
                              GNM_SHADER_PIXEL, &red_ps)) {
        return st_fail("reason=shader_load");
    }

    GnmBuffer* table = (GnmBuffer*)st_arena_alloc(
        &arena, BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    volatile uint32_t* dma_source = (volatile uint32_t*)st_arena_alloc(
        &arena, DMA_WORDS * sizeof(uint32_t), ST_PAGE_BYTES);
    volatile uint32_t* p = (volatile uint32_t*)st_arena_alloc(
        &arena, SLOT_BYTES, ST_PAGE_BYTES);
    if (!table || !dma_source || !p) {
        return st_fail("reason=arena_memory");
    }
    for (uint32_t i = 0; i < DMA_WORDS; ++i) {
        dma_source[i] = dma_word(i);
    }
    const uint32_t p_words = (LEAD_BYTES + TARGET_BYTES) / sizeof(uint32_t);
    for (uint32_t i = 0; i < p_words; ++i) {
        p[i] = p_word(i);
    }

    StCase cases[CASES] = {
        [CASE_DRAW_THEN_DISPATCH] = {.name = "draw_then_dispatch"},
        [CASE_DMA_OVER_DRAW] = {.name = "dma_over_draw"},
        [CASE_FILL_OVER_DRAW] = {.name = "fill_over_draw"},
        [CASE_COPY_INTO_DRAW] = {.name = "copy_into_draw"},
        [CASE_WRITE_OVER_DRAW] = {.name = "write_over_draw"},
        [CASE_PARTIAL_WRITE_OVER_DRAW] = {.name = "partial_write_over_draw"},
    };

    for (unsigned k = 0; k < CASES; ++k) {
        /* Each case gets its own slot; the write cases put M LEAD_BYTES in. */
        uint8_t* slot = (uint8_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
        volatile uint32_t* out = (volatile uint32_t*)st_arena_alloc(
            &arena, TARGET_BYTES, ST_PAGE_BYTES);
        if (!slot || !out) {
            return st_fail("reason=arena_memory case=%s", cases[k].name);
        }
        const bool writes_over = k == CASE_WRITE_OVER_DRAW ||
                                 k == CASE_PARTIAL_WRITE_OVER_DRAW;
        const size_t lead = writes_over ? LEAD_BYTES : 0;
        Target t;
        if (!create_target(slot + lead, SLOT_BYTES - lead, &t)) {
            return st_fail("reason=render_target case=%s", cases[k].name);
        }
        for (uint32_t i = 0; i < t.words; ++i) {
            t.pixels[i] = source_word(k, i);
        }
        st_fill_words(out, WORDS, COPY_POISON);
        const uint32_t target_bytes = t.words * sizeof(uint32_t);

        StDcb dcb;
        st_dcb_begin(&dcb, &queue);
        record_masked_draw(&dcb, &t, &vs, &red_ps);
        st_dcb_barrier(&dcb, ST_PM4_BARRIER_AFTER_DRAW | ST_PM4_BARRIER_CB);
        bool copies_out = true;
        switch (k) {
        case CASE_DMA_OVER_DRAW:
            st_dcb_copy(&dcb, t.pixels, dma_source, DMA_WORDS * sizeof(uint32_t));
            st_dcb_barrier(&dcb, 0);
            break;
        case CASE_FILL_OVER_DRAW:
            st_dcb_fill(&dcb, t.pixels, target_bytes, FILL_VALUE);
            st_dcb_barrier(&dcb, ST_PM4_BARRIER_CB);
            record_masked_draw(&dcb, &t, &vs, &red_ps);
            copies_out = false;
            break;
        case CASE_COPY_INTO_DRAW:
            st_dcb_copy(&dcb, t.pixels + DMA_OFFSET_WORDS, dma_source,
                        DMA_WORDS * sizeof(uint32_t));
            st_dcb_barrier(&dcb, ST_PM4_BARRIER_CB);
            record_masked_draw(&dcb, &t, &vs, &red_ps);
            copies_out = false;
            break;
        case CASE_WRITE_OVER_DRAW:
        case CASE_PARTIAL_WRITE_OVER_DRAW: {
            const uint32_t bytes = LEAD_BYTES + target_bytes;
            const uint32_t written =
                k == CASE_WRITE_OVER_DRAW ? bytes : LEAD_BYTES;
            table[0] = st_raw_buffer(p, bytes);
            table[1] = st_raw_buffer(slot, bytes);
            st_dcb_dispatch(&dcb, &copy, table,
                            written / sizeof(uint32_t) / ST_LOCAL_SIZE_X);
            st_dcb_barrier(&dcb, 0);
            break;
        }
        default:
            break;
        }
        GnmBuffer* read_table = &table[0];
        if (copies_out) {
            /* The write cases' dispatch above used table[]; give the
             * read its own descriptors so the CP reads them after [B]. */
            if (writes_over) {
                read_table = (GnmBuffer*)st_arena_alloc(
                    &arena, BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
                if (!read_table) {
                    return st_fail("reason=arena_memory case=%s", cases[k].name);
                }
            }
            read_table[0] = st_raw_buffer(t.pixels, target_bytes);
            read_table[1] = st_raw_buffer(out, target_bytes);
            st_dcb_dispatch(&dcb, &copy, read_table,
                            t.words / ST_LOCAL_SIZE_X);
        }
        if (!st_dcb_submit_and_wait(&dcb)) {
            return st_fail("reason=submit_or_eop case=%s", cases[k].name);
        }
        if (copies_out) {
            check_words(&cases[k], k, out, COPY_POISON);
        } else {
            check_words(&cases[k], k, t.pixels, TARGET_POISON);
        }
    }

    return st_finish_cases(cases, CASES, "");
}
