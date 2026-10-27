/*
 * tiled_rt_buffer_read: a draw renders a per-pixel pattern into a render
 * target with one of several tile modes, then a dispatch in the same DCB
 * copies the target's raw bytes to a buffer O:
 *
 *   draw pattern.frag into T (full channel mask)
 *   [B] after the draw, with CB flush
 *   dispatch copies the bytes of T to O
 *   EOP
 *
 * Oracle: for every pixel (x, y), O at the byte offset that OpenGNM's
 * sceGpaComputeSurfaceCoord gives for T's tiling parameters holds
 * pixel_pattern(x, y). Bytes outside the pixels (padding) are not checked.
 * O is a shader output, so the oracle needs readbacks Precise.
 *
 * shadPS4 records the draw into a Vulkan image. For the dispatch to see the
 * draw, the image has to be tiled into the buffer cache's copy of T's
 * memory, so this also checks shadPS4's tiler against OpenGNM's addressing.
 *
 * Cases, one per tile mode (all R8G8B8A8_UNORM, WIDTH x HEIGHT, own pages):
 *   linear       GNM_TM_DISPLAY_LINEAR_ALIGNED (control for the tiler)
 *   display_1d   GNM_TM_DISPLAY_1D_THIN
 *   display_2d   GNM_TM_DISPLAY_2D_THIN
 *   thin_1d      GNM_TM_THIN_1D_THIN
 *   thin_2d      GNM_TM_THIN_2D_THIN
 *
 * Reasons: copy_unwritten (O kept its poison), buffer_saw_stale_memory (O
 * holds T's poison, so the dispatch did not see the draw), linear_layout
 * (O holds the pixel at its linear offset instead), tiling_mismatch.
 */

#define SHADTEST_NAME "tiled_rt_buffer_read"
#include "shadtest_guest.h"

#include <gnm_rendertarget.h>
#include <gpuaddr.h>

enum {
    WIDTH = 128,
    HEIGHT = 128,
    ARENA_BYTES = 4 * 1024 * 1024,
    BINDINGS = 2,
};

_Static_assert(WIDTH * HEIGHT * 4 > 16 * 1024,
               "targets must be larger than shadPS4's stream threshold");

static const uint32_t TARGET_POISON = 0xcdcdcdcdu;
static const uint32_t COPY_POISON = 0xababababu;

/* Must match pattern.frag.glsl; R8G8B8A8 little-endian. */
static uint32_t pixel_pattern(uint32_t x, uint32_t y) {
    const uint32_t r = (x * 3u + y) & 255u;
    const uint32_t g = (y * 5u + 7u) & 255u;
    const uint32_t b = (x ^ y) & 255u;
    return r | (g << 8) | (b << 16) | (255u << 24);
}

typedef struct {
    const char* name;
    GnmTileMode tile_mode;
} TileCase;

static const TileCase TILE_CASES[] = {
    {"linear", GNM_TM_DISPLAY_LINEAR_ALIGNED},
    {"display_1d", GNM_TM_DISPLAY_1D_THIN},
    {"display_2d", GNM_TM_DISPLAY_2D_THIN},
    {"thin_1d", GNM_TM_THIN_1D_THIN},
    {"thin_2d", GNM_TM_THIN_2D_THIN},
};

enum { CASES = sizeof(TILE_CASES) / sizeof(TILE_CASES[0]) };

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
/* Render target, draw and oracle                                           */
/* ------------------------------------------------------------------------ */

typedef struct {
    GnmRenderTarget rt;
    volatile uint32_t* memory;
    uint32_t bytes;
} Target;

static bool create_target(StArena* arena, GnmTileMode tile_mode, Target* t) {
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
        .colortilemodehint = tile_mode,
        .mingpumode = GNM_GPU_BASE,
    };
    if (sceGnmCreateRenderTarget(&t->rt, &info) != GNM_ERROR_OK) {
        return false;
    }
    uint64_t size = 0;
    uint32_t alignment = 0;
    if (sceGnmRtCalcByteSize(&size, &alignment, &t->rt) != GNM_ERROR_OK ||
        size == 0 || size % (ST_LOCAL_SIZE_X * sizeof(uint32_t)) != 0) {
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
    t->memory = (volatile uint32_t*)memory;
    t->bytes = (uint32_t)size;
    st_fill_words(t->memory, t->bytes / sizeof(uint32_t), TARGET_POISON);
    return true;
}

static void record_pattern_draw(StDcb* dcb, Target* t, GraphicsShader* vs,
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
    sceGnmDrawCmdSetRenderTargetMask(cmd, 0x0f);
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

/* Returns NULL, or the OpenGNM addressing step that failed. */
static const char* check_copy(StCase* c, const Target* t,
                              const volatile uint32_t* out, GpaError* err) {
    const GpaTextureInfo info = sceGnmRtBuildInfo(&t->rt);
    /* sceGpaTpInit keeps surface flags it does not derive, so start from
     * zero, then mark display tile modes as sceGnmRtCalcByteSize does. */
    GpaTilingParams params = {0};
    GpaSurfaceContext ctx;
    if ((*err = sceGpaTpInit(&params, &info, 0, 0)) != GPA_ERR_OK) {
        return "tp_init";
    }
    params.surfaceflags.display = info.tm >= GNM_TM_DISPLAY_LINEAR_ALIGNED &&
                                  info.tm <= GNM_TM_DISPLAY_2D_THIN_PRT;
    if ((*err = sceGpaInitSurfaceContext(&ctx, t->bytes, &params)) !=
        GPA_ERR_OK) {
        return "surface_context";
    }
    const uint32_t pitch = sceGnmRtGetPitch(&t->rt);
    uint32_t moved = 0; /* pixels away from their linear offset */
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        for (uint32_t x = 0; x < WIDTH; ++x) {
            uint64_t offset = 0;
            uint64_t bit_offset = 0;
            *err = sceGpaComputeSurfaceCoord(&offset, &bit_offset, &ctx, x, y,
                                             0, 0);
            if (*err != GPA_ERR_OK) {
                return "surface_coord";
            }
            if (bit_offset != 0 || offset % sizeof(uint32_t) != 0 ||
                offset >= t->bytes) {
                return "pixel_offset";
            }
            const uint32_t linear = y * pitch + x;
            moved += offset / sizeof(uint32_t) != linear;
            const uint32_t expected = pixel_pattern(x, y);
            const uint32_t got = out[offset / sizeof(uint32_t)];
            if (got == expected) {
                continue;
            }
            const char* reason = "tiling_mismatch";
            if (got == COPY_POISON) {
                reason = "copy_unwritten";
            } else if (got == TARGET_POISON) {
                reason = "buffer_saw_stale_memory";
            } else if (linear < t->bytes / sizeof(uint32_t) &&
                       out[linear] == expected) {
                reason = "linear_layout";
            }
            st_case_note(c, reason, y * WIDTH + x, expected, got);
        }
    }
    /* A tiled case whose addressing came out linear would not test tiling. */
    const bool linear_mode = info.tm == GNM_TM_DISPLAY_LINEAR_ALIGNED;
    if (linear_mode != (moved == 0)) {
        return "layout_class";
    }
    return NULL;
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
    GraphicsShader pattern_ps;
    if (!st_load_compute_shader(&arena, "/app0/assets/copy.comp.sb", &copy) ||
        !load_graphics_shader(&arena, "/app0/assets/fullscreen.vert.sb",
                              GNM_SHADER_VERTEX, &vs) ||
        !load_graphics_shader(&arena, "/app0/assets/pattern.frag.sb",
                              GNM_SHADER_PIXEL, &pattern_ps)) {
        return st_fail("reason=shader_load");
    }

    GnmBuffer* table = (GnmBuffer*)st_arena_alloc(
        &arena, BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    if (!table) {
        return st_fail("reason=arena_memory");
    }

    StCase cases[CASES];
    memset(cases, 0, sizeof(cases));
    for (unsigned k = 0; k < CASES; ++k) {
        cases[k].name = TILE_CASES[k].name;

        Target t;
        if (!create_target(&arena, TILE_CASES[k].tile_mode, &t)) {
            return st_fail("reason=render_target case=%s", cases[k].name);
        }
        volatile uint32_t* out = (volatile uint32_t*)st_arena_alloc(
            &arena, t.bytes, ST_PAGE_BYTES);
        if (!out) {
            return st_fail("reason=arena_memory case=%s", cases[k].name);
        }
        st_fill_words(out, t.bytes / sizeof(uint32_t), COPY_POISON);

        StDcb dcb;
        st_dcb_begin(&dcb, &queue);
        record_pattern_draw(&dcb, &t, &vs, &pattern_ps);
        st_dcb_barrier(&dcb, ST_PM4_BARRIER_AFTER_DRAW | ST_PM4_BARRIER_CB);
        table[0] = st_raw_buffer(t.memory, t.bytes);
        table[1] = st_raw_buffer(out, t.bytes);
        st_dcb_dispatch(&dcb, &copy, table,
                        t.bytes / sizeof(uint32_t) / ST_LOCAL_SIZE_X);
        if (!st_dcb_submit_and_wait(&dcb)) {
            return st_fail("reason=submit_or_eop case=%s", cases[k].name);
        }
        GpaError err = GPA_ERR_OK;
        const char* step = check_copy(&cases[k], &t, out, &err);
        if (step) {
            return st_fail("reason=addressing case=%s step=%s err=%s "
                           "tile_mode=%u pitch=%u bytes=%u",
                           cases[k].name, step, sceGpaStrError(err),
                           (unsigned)t.rt.attrib.tilemode_index,
                           sceGnmRtGetPitch(&t.rt), t.bytes);
        }
    }

    return st_finish_cases(cases, CASES, "");
}
