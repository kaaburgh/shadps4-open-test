/*
 * image_buffer_alias: Stage 8 of docs/memory-uma-test-roadmap.md.
 *
 * The same backing bytes are used as a linear render target and as a raw
 * buffer in ordered phases, in both directions. This moves ownership between
 * shadPS4's TextureCache and BufferCache, which the buffer-only tests avoid.
 * Every render target is 64x64 R8G8B8A8_UNORM, linear, in its own pages.
 *
 * rt_to_buffer:
 *   draw a per-pixel pattern; compute reads the target as a raw buffer
 *   (out = transform(target)); the CPU verifies out and the target.
 *
 * Draws over bytes that were just written (red.frag writes R = 90 and
 * nothing else useful; the channel mask decides what reaches memory):
 *   rt_masked_after_cpu_write            CPU writes; R-only draw (control)
 *   rt_full_after_buffer_write           compute writes; full draw
 *   rt_masked_after_buffer_write         compute writes; R-only draw, so
 *                                        G/B/A must keep the compute values
 *   rt_masked_after_buffer_write_cpu_read
 *                                        compute writes; CPU reads; R-only
 *                                        draw (control)
 *
 * Every case runs even after a failure.
 */

#define SHADTEST_NAME "image_buffer_alias"
#include "shadtest_guest.h"

#include <gnm_rendertarget.h>

enum {
    WIDTH = 64,
    HEIGHT = 64,
    RED_VALUE = 90, /* red.frag.glsl writes 90 / 255 */
    ARENA_BYTES = 2 * 1024 * 1024,
    BINDINGS = 2,
};

static const uint32_t POISON = 0xcdcdcdcdu;

/* Must match pattern.frag.glsl; R8G8B8A8 little-endian. */
static uint32_t pixel_pattern(uint32_t x, uint32_t y) {
    const uint32_t r = (x * 3u + y) & 255u;
    const uint32_t g = (y * 5u + 7u) & 255u;
    const uint32_t b = (x ^ y) & 255u;
    return r | (g << 8) | (b << 16) | (255u << 24);
}

/* Must match transform.comp. */
static uint32_t transform(uint32_t v, uint32_t i) {
    return ((v << 5) | (v >> 27)) ^ (i * 0x2545F491u) ^ 0x6A09E667u;
}

static uint32_t source_word(uint32_t i) {
    return (i * 0x9E3779B1u) ^ 0x0BADF00Du;
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

    const GnmShaderCommonData* common =
        (const GnmShaderCommonData*)(shader_base + sizeof(GnmShaderFileHeader));
    uint32_t stage_size = 0;
    const void* code_src = NULL;
    if (expected_type == GNM_SHADER_VERTEX) {
        const GnmVsShader* vs = (const GnmVsShader*)common;
        stage_size = sceGnmVsShaderCalcSize(vs);
        code_src = sceGnmVsShaderCodePtr(vs);
    } else {
        const GnmPsShader* ps = (const GnmPsShader*)common;
        stage_size = sceGnmPsShaderCalcSize(ps);
        code_src = sceGnmPsShaderCodePtr(ps);
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
/* Render targets and draws                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    GnmRenderTarget rt;
    volatile uint32_t* pixels;
    uint32_t pitch;
    uint32_t words; /* pitch * HEIGHT */
} Target;

static bool create_target(StArena* arena, Target* t) {
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

static bool draw_and_wait(StQueue* queue, Target* t, GraphicsShader* vs,
                          GraphicsShader* ps, uint32_t channel_mask) {
    const uint64_t value = ++queue->next_value;
    GnmCommandBuffer cmd = sceGnmCmdInit(queue->command_memory,
                                         ST_COMMAND_BUFFER_BYTES, NULL, NULL);
    memset(&cmd.flags, 0, sizeof(cmd.flags));
    sceGnmDrawCmdInitDefaultHardwareState(&cmd);

    const GnmPrimitiveSetup primitive = {
        .cullmode = GNM_CULL_NONE,
        .frontface = GNM_FACE_CCW,
        .frontmode = GNM_FILL_SOLID,
        .backmode = GNM_FILL_SOLID,
        .provokemode = GNM_PROVOKINGVTX_FIRST,
    };
    sceGnmDrawCmdSetPrimitiveSetup(&cmd, &primitive);
    sceGnmDrawCmdSetRenderTarget(&cmd, 0, &t->rt);
    sceGnmDrawCmdSetRenderTargetMask(&cmd, channel_mask);
    sceGnmDrawCmdSetScreenScissor(&cmd, 0, 0, WIDTH, HEIGHT);
    const GnmSetViewportInfo viewport = {
        .dmin = 0.0f,
        .dmax = 1.0f,
        .scale = {WIDTH * 0.5f, -HEIGHT * 0.5f, 0.5f},
        .offset = {WIDTH * 0.5f, HEIGHT * 0.5f, 0.5f},
    };
    sceGnmDrawCmdSetViewport(&cmd, 0, &viewport);
    sceGnmDrawCmdSetVsShader(&cmd, &((GnmVsShader*)vs->stage)->registers, 0);
    sceGnmDrawCmdSetPsShader(&cmd, &((GnmPsShader*)ps->stage)->registers);
    sceGnmDrawCmdSetPrimitiveType(&cmd, GNM_PT_TRILIST);
    sceGnmDrawCmdDrawIndexAuto(&cmd, 3);
    sceGnmDrawCmdEventWriteEop(&cmd, GNM_FLUSH_AND_INV_CB_DATA_TS,
                               (uint64_t)(uintptr_t)queue->label,
                               GNM_DATA_SEL_SEND_DATA64, value);
    if (st_gnm_errors != 0) {
        return false;
    }

    void* addrs[1] = {cmd.beginptr};
    uint32_t sizes[1] = {
        (uint32_t)((uintptr_t)cmd.cmdptr - (uintptr_t)cmd.beginptr)};
    st_compiler_barrier();
    if (sceGnmSubmitCommandBuffers(1, addrs, sizes, NULL, NULL) < 0 ||
        sceGnmSubmitDone() < 0) {
        return false;
    }
    for (unsigned ms = 0; ms < ST_EOP_WAIT_MS; ++ms) {
        if (*queue->label == value) {
            st_compiler_barrier();
            return true;
        }
        sceKernelUsleep(1000);
    }
    printf("draw EOP timeout\n");
    return false;
}

/* ------------------------------------------------------------------------ */
/* Cases                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char* name;
    const char* reason;
    unsigned bad_words;
    uint32_t x;
    uint32_t y;
    uint32_t expected;
    uint32_t got;
} CaseResult;

static void note(CaseResult* r, const char* reason, uint32_t index,
                 uint32_t pitch, uint32_t expected, uint32_t got) {
    if (!r->reason) {
        r->reason = reason;
        r->x = index % pitch;
        r->y = index / pitch;
        r->expected = expected;
        r->got = got;
    }
    r->bad_words += 1;
}

static void report(const CaseResult* r) {
    if (r->reason) {
        printf(
            "case %s: FAIL bad_words=%u first %s x=%u y=%u expected=%08x "
            "got=%08x\n",
            r->name, r->bad_words, r->reason, r->x, r->y, r->expected, r->got);
    } else {
        printf("case %s: ok\n", r->name);
    }
}

/* ------------------------------------------------------------------------ */
/* Draws over bytes written by the CPU or by a compute dispatch              */
/* ------------------------------------------------------------------------ */

typedef enum {
    PREP_CPU_WRITE,
    PREP_BUFFER_WRITE,
    PREP_BUFFER_WRITE_CPU_READ,
} Prep;

typedef struct {
    const char* name;
    Prep prep;
    uint32_t channel_mask;
} DrawCaseSpec;

static const DrawCaseSpec DRAW_CASE_SPECS[] = {
    /* control: the masked draw itself */
    {"rt_masked_after_cpu_write", PREP_CPU_WRITE, 0x01},
    {"rt_full_after_buffer_write", PREP_BUFFER_WRITE, 0x0f},
    {"rt_masked_after_buffer_write", PREP_BUFFER_WRITE, 0x01},
    /* control: the CPU reads the buffer-written bytes before the draw */
    {"rt_masked_after_buffer_write_cpu_read", PREP_BUFFER_WRITE_CPU_READ, 0x01},
};

enum { DRAW_CASES = sizeof(DRAW_CASE_SPECS) / sizeof(DRAW_CASE_SPECS[0]) };

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

/* Prepares the target's bytes as the spec says, draws red.frag with the
 * spec's channel mask and verifies what the CPU then sees. Returns false only
 * for a submission failure. */
static bool run_draw_case(StQueue* queue, const StComputeShader* cs,
                          GnmBuffer* table, Target* t, GraphicsShader* vs,
                          GraphicsShader* red_ps, const DrawCaseSpec* spec,
                          CaseResult* r) {
    const bool buffer_written = spec->prep != PREP_CPU_WRITE;
    if (buffer_written) {
        if (!st_dispatch_and_wait(queue, cs, table,
                                  t->words / ST_LOCAL_SIZE_X)) {
            return false;
        }
    } else {
        for (uint32_t i = 0; i < t->words; ++i) {
            t->pixels[i] = source_word(i);
        }
    }
    if (spec->prep == PREP_BUFFER_WRITE_CPU_READ) {
        for (uint32_t i = 0; i < t->words; ++i) {
            const uint32_t expected = transform(source_word(i), i);
            if (t->pixels[i] != expected) {
                note(r, "buffer_write_not_visible", i, t->pitch, expected,
                     t->pixels[i]);
            }
        }
    }

    if (!draw_and_wait(queue, t, vs, red_ps, spec->channel_mask)) {
        return false;
    }

    const bool masked = spec->channel_mask != 0x0f;
    for (uint32_t i = 0; i < t->words; ++i) {
        if (i % t->pitch >= WIDTH) {
            continue;
        }
        const uint32_t under =
            buffer_written ? transform(source_word(i), i) : source_word(i);
        const uint32_t expected =
            masked ? (under & 0xffffff00u) | RED_VALUE : (uint32_t)RED_VALUE;
        const uint32_t got = t->pixels[i];
        if (got == expected) {
            continue;
        }
        const char* reason = "rt_mismatch";
        if (got == under) {
            reason = "draw_not_visible";
        } else if (masked && (got & 0xffffff00u) == (POISON & 0xffffff00u)) {
            reason = "rt_saw_stale_memory";
        } else if (masked && (got & 0xffffff00u) != (under & 0xffffff00u)) {
            reason = "masked_channels_changed";
        }
        note(r, reason, i, t->pitch, expected, got);
    }
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

    StComputeShader transform_cs;
    GraphicsShader vs;
    GraphicsShader pattern_ps;
    GraphicsShader red_ps;
    if (!st_load_compute_shader(&arena, "/app0/assets/transform.comp.sb",
                                &transform_cs) ||
        !load_graphics_shader(&arena, "/app0/assets/fullscreen.vert.sb",
                              GNM_SHADER_VERTEX, &vs) ||
        !load_graphics_shader(&arena, "/app0/assets/pattern.frag.sb",
                              GNM_SHADER_PIXEL, &pattern_ps) ||
        !load_graphics_shader(&arena, "/app0/assets/red.frag.sb",
                              GNM_SHADER_PIXEL, &red_ps)) {
        return st_fail("reason=shader_load");
    }

    Target a;
    Target targets[DRAW_CASES];
    if (!create_target(&arena, &a)) {
        return st_fail("reason=render_target");
    }
    for (unsigned k = 0; k < DRAW_CASES; ++k) {
        if (!create_target(&arena, &targets[k])) {
            return st_fail("reason=render_target");
        }
    }
    if (a.words != WIDTH * HEIGHT) {
        printf("render target pitch %u, expected %u\n", a.pitch, WIDTH);
    }

    GnmBuffer* tables = (GnmBuffer*)st_arena_alloc(
        &arena, (1 + DRAW_CASES) * BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    volatile uint32_t* out = (volatile uint32_t*)st_arena_alloc(
        &arena, a.words * sizeof(uint32_t), ST_PAGE_BYTES);
    volatile uint32_t* source = (volatile uint32_t*)st_arena_alloc(
        &arena, a.words * sizeof(uint32_t), ST_PAGE_BYTES);
    if (!tables || !out || !source) {
        return st_fail("reason=arena_memory");
    }
    st_fill_words(out, a.words, POISON);
    for (uint32_t i = 0; i < a.words; ++i) {
        source[i] = source_word(i);
    }
    tables[0] = st_raw_buffer(a.pixels, a.words * sizeof(uint32_t));
    tables[1] = st_raw_buffer(out, a.words * sizeof(uint32_t));
    for (unsigned k = 0; k < DRAW_CASES; ++k) {
        GnmBuffer* table = &tables[(1 + k) * BINDINGS];
        table[0] = st_raw_buffer(source, a.words * sizeof(uint32_t));
        table[1] = st_raw_buffer(targets[k].pixels,
                                 targets[k].words * sizeof(uint32_t));
    }

    CaseResult cases[1 + DRAW_CASES] = {{.name = "rt_to_buffer"}};
    for (unsigned k = 0; k < DRAW_CASES; ++k) {
        cases[1 + k].name = DRAW_CASE_SPECS[k].name;
    }

    /* rt_to_buffer */
    if (!draw_and_wait(&queue, &a, &vs, &pattern_ps, 0x0f)) {
        return st_fail("reason=draw case=rt_to_buffer");
    }
    if (!st_dispatch_and_wait(&queue, &transform_cs, &tables[0],
                              a.words / ST_LOCAL_SIZE_X)) {
        return st_fail("reason=dispatch case=rt_to_buffer");
    }
    for (uint32_t i = 0; i < a.words; ++i) {
        const uint32_t x = i % a.pitch;
        const uint32_t y = i / a.pitch;
        if (x >= WIDTH) {
            continue;
        }
        const uint32_t expected = transform(pixel_pattern(x, y), i);
        const uint32_t got = out[i];
        if (got != expected) {
            const char* reason = "buffer_mismatch";
            if (got == POISON) {
                reason = "buffer_unwritten";
            } else if (got == transform(POISON, i)) {
                reason = "buffer_saw_stale_memory";
            }
            note(&cases[0], reason, i, a.pitch, expected, got);
        }
    }
    for (uint32_t i = 0; i < a.words; ++i) {
        const uint32_t x = i % a.pitch;
        if (x >= WIDTH) {
            continue;
        }
        const uint32_t expected = pixel_pattern(x, i / a.pitch);
        const uint32_t got = a.pixels[i];
        if (got != expected) {
            note(&cases[0], got == POISON ? "cpu_rt_stale" : "cpu_rt_mismatch",
                 i, a.pitch, expected, got);
        }
    }

    /* Draws over bytes the CPU or a compute dispatch wrote. */
    for (unsigned k = 0; k < DRAW_CASES; ++k) {
        if (!run_draw_case(&queue, &transform_cs, &tables[(1 + k) * BINDINGS],
                           &targets[k], &vs, &red_ps, &DRAW_CASE_SPECS[k],
                           &cases[1 + k])) {
            return st_fail("reason=submit case=%s", DRAW_CASE_SPECS[k].name);
        }
    }

    unsigned failed = 0;
    const CaseResult* first = NULL;
    char failed_names[160] = "";
    for (unsigned k = 0; k < 1 + DRAW_CASES; ++k) {
        report(&cases[k]);
        if (cases[k].reason) {
            failed += 1;
            if (!first) {
                first = &cases[k];
            }
            const size_t used = strlen(failed_names);
            snprintf(failed_names + used, sizeof(failed_names) - used, "%s%s",
                     used ? "," : "", cases[k].name);
        }
    }
    if (st_gnm_errors != 0) {
        return st_fail("reason=gnm_error count=%u", st_gnm_errors);
    }
    if (first) {
        return st_fail(
            "reason=%s case=%s x=%u y=%u expected=%08x got=%08x "
            "failed_cases=%u failed=%s",
            first->reason, first->name, first->x, first->y, first->expected,
            first->got, failed, failed_names);
    }

    uint32_t checksum = fnv1a_words(0x811c9dc5u, out, a.words);
    for (unsigned k = 0; k < DRAW_CASES; ++k) {
        checksum = fnv1a_words(checksum, targets[k].pixels, targets[k].words);
    }
    st_emit_marker("PASS", "cases=%u pitch=%u checksum=%08x",
                   (unsigned)(1 + DRAW_CASES), a.pitch, checksum);

    free(red_ps.stage);
    free(pattern_ps.stage);
    free(vs.stage);
    st_unload_compute_shader(&transform_cs);
    st_arena_destroy(&arena);
    return 0;
}
