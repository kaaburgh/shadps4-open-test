/*
 * image_source_same_submit: scenario B5 of docs/uma-e1c-e3-scenario-plan.md.
 *
 * A compute dispatch writes memory M, then a draw with only the red channel
 * enabled renders into a linear render target over M, all in one DCB:
 *
 *   dispatch copies source words into M
 *   [B] with CB invalidate (the CB does not read through L2 on GFX7, and
 *       must see the compute data before the partial-mask draw)
 *   R-only draw into the 64x128 R8G8B8A8 target at M
 *   EOP
 *
 * The draw keeps green, blue and alpha, so they must hold what the dispatch
 * wrote. shadPS4 initializes the target image from M while recording the
 * draw. With shared backing M is guest memory, and the dispatch has not run
 * yet at that point; the image-source guard in ObtainBufferForImage keeps
 * that copy on the GPU timeline. A mutant without the guard passes the rest
 * of the suite (it needs the dispatch and the draw in one submission).
 *
 * Cases:
 *   same_submit   the sequence above.
 *   cpu_control   control: the CPU writes M, then the R-only draw
 *                 (image_buffer_alias case rt_masked_after_cpu_write).
 *
 * Reuses image_buffer_alias's shaders and target setup. With the mirror,
 * image_buffer_alias case rt_masked_after_buffer_write (the same sequence in
 * two submissions) fails in readbacks Precise (kaaburgh/shadPS4#4).
 */

#define SHADTEST_NAME "image_source_same_submit"
#include "shadtest_guest.h"

#include <gnm_rendertarget.h>

enum {
    WIDTH = 64,
    HEIGHT = 128,
    RED_VALUE = 90, /* red.frag.glsl writes 90 / 255 */
    ARENA_BYTES = 2 * 1024 * 1024,
    SLOT_BYTES = 64 * 1024,
    BINDINGS = 2,
};

_Static_assert(WIDTH * HEIGHT * 4 > 16 * 1024,
               "targets must be larger than shadPS4's stream threshold");

enum {
    CASE_SAME_SUBMIT,
    CASE_CPU_CONTROL,
    CASES,
};

static const uint32_t POISON = 0xcdcdcdcdu;

static uint32_t source_word(unsigned seed, uint32_t i) {
    return (i * 0x9E3779B1u) ^ (seed * 0x7F4A7C15u) ^ 0x0BADF00Du;
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

static bool create_target(StArena* arena, Target* t) {
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
        size == 0) {
        return false;
    }
    /* Own pages, so nothing else shares tracking state with the target. */
    const size_t align =
        alignment > ST_PAGE_BYTES ? alignment : (size_t)ST_PAGE_BYTES;
    void* memory = st_arena_alloc(arena, (size_t)size, align);
    if (!memory) {
        return false;
    }
    sceGnmRtSetBaseAddr(&t->rt, memory);
    t->pixels = (volatile uint32_t*)memory;
    t->pitch = sceGnmRtGetPitch(&t->rt);
    t->words = t->pitch * HEIGHT;
    if ((uint64_t)t->words * sizeof(uint32_t) > size) {
        return false;
    }
    st_fill_words(t->pixels, t->words, POISON);
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

/* Every pixel must have red = RED_VALUE over the word under it. */
static void check_target(StCase* c, const Target* t, unsigned seed) {
    for (uint32_t i = 0; i < t->words; ++i) {
        if (i % t->pitch >= WIDTH) {
            continue;
        }
        const uint32_t under = source_word(seed, i);
        const uint32_t expected = (under & 0xffffff00u) | RED_VALUE;
        const uint32_t got = t->pixels[i];
        if (got == expected) {
            continue;
        }
        const char* reason = "rt_mismatch";
        if (got == POISON) {
            reason = "nothing_visible";
        } else if (got == under) {
            reason = "draw_not_visible";
        } else if ((got & 0xffffff00u) == (POISON & 0xffffff00u)) {
            reason = "rt_saw_stale_memory";
        } else if ((got & 0xffffff00u) != (under & 0xffffff00u)) {
            reason = "masked_channels_changed";
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
    volatile uint32_t* source =
        (volatile uint32_t*)st_arena_alloc(&arena, SLOT_BYTES, SLOT_BYTES);
    if (!table || !source) {
        return st_fail("reason=arena_memory");
    }

    StCase cases[CASES] = {
        [CASE_SAME_SUBMIT] = {.name = "same_submit"},
        [CASE_CPU_CONTROL] = {.name = "cpu_control"},
    };

    for (unsigned k = 0; k < CASES; ++k) {
        Target t;
        if (!create_target(&arena, &t)) {
            return st_fail("reason=render_target");
        }
        if (t.pitch != WIDTH) {
            return st_fail("reason=pitch pitch=%u", t.pitch);
        }

        StDcb dcb;
        st_dcb_begin(&dcb, &queue);
        if (k == CASE_SAME_SUBMIT) {
            for (uint32_t i = 0; i < t.words; ++i) {
                source[i] = source_word(k, i);
            }
            table[0] = st_raw_buffer(source, t.words * sizeof(uint32_t));
            table[1] = st_raw_buffer(t.pixels, t.words * sizeof(uint32_t));
            st_dcb_dispatch(&dcb, &copy, table, t.words / ST_LOCAL_SIZE_X);
            st_dcb_barrier(&dcb, ST_PM4_BARRIER_CB);
        } else {
            for (uint32_t i = 0; i < t.words; ++i) {
                t.pixels[i] = source_word(k, i);
            }
        }
        record_masked_draw(&dcb, &t, &vs, &red_ps);
        if (!st_dcb_submit_and_wait(&dcb)) {
            return st_fail("reason=submit_or_eop case=%s", cases[k].name);
        }
        check_target(&cases[k], &t, k);
    }

    return st_finish_cases(cases, CASES, "");
}
