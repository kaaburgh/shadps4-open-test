#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <orbis/libkernel.h>

#include <gnm_commandbuffer.h>
#include <gnm_drawcommandbuffer.h>
#include <gnm_rendertarget.h>
#include <gnm_shader.h>
#include <gnm_shaderbinary.h>
#include <gnm_types.h>
#include <gnmdriver.h>

#ifndef ORBIS_KERNEL_WC_GARLIC
#define ORBIS_KERNEL_WC_GARLIC 3
#endif
#ifndef ORBIS_KERNEL_PROT_CPU_READ
#define ORBIS_KERNEL_PROT_CPU_READ 0x01
#endif
#ifndef ORBIS_KERNEL_PROT_CPU_RW
#define ORBIS_KERNEL_PROT_CPU_RW 0x02
#endif
#ifndef ORBIS_KERNEL_PROT_GPU_READ
#define ORBIS_KERNEL_PROT_GPU_READ 0x10
#endif
#ifndef ORBIS_KERNEL_PROT_GPU_WRITE
#define ORBIS_KERNEL_PROT_GPU_WRITE 0x20
#endif

enum {
    WIDTH = 64,
    HEIGHT = 64,
    DIRECT_MEMORY_SIZE = 2 * 1024 * 1024,
    DIRECT_MEMORY_ALIGNMENT = 2 * 1024 * 1024,
    COMMAND_BUFFER_SIZE = 64 * 1024,
    EOP_WAIT_MS = 5000,
};

static const char* const TEST_NAME = "gpu_solid_rt";
static const uint64_t EOP_VALUE = 0x5348414454455354ULL; /* "SHADTEST" */

typedef struct {
    off_t direct_memory;
    uint8_t* base;
    size_t size;
    size_t used;
} GpuArena;

typedef struct {
    void* stage;
    void* gpu_code;
    uint32_t gpu_code_size;
    GnmShaderType type;
} LoadedShader;

static size_t align_up(size_t value, size_t alignment) {
    const size_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

static void emit_marker(const char* status, const char* detail) {
    char line[256];
    if (detail && detail[0]) {
        snprintf(line, sizeof(line), "SHADTEST name=%s status=%s %s", TEST_NAME,
                 status, detail);
    } else {
        snprintf(line, sizeof(line), "SHADTEST name=%s status=%s", TEST_NAME,
                 status);
    }

    puts(line);
    fflush(stdout);
    sceKernelDebugOutText(0, line);
    sceKernelDebugOutText(0, "\n");
}

static int fail(const char* reason) {
    char detail[160];
    snprintf(detail, sizeof(detail), "reason=%s", reason);
    emit_marker("FAIL", detail);
    return 1;
}

static bool arena_init(GpuArena* arena) {
    memset(arena, 0, sizeof(*arena));
    arena->direct_memory = -1;
    arena->size = DIRECT_MEMORY_SIZE;

    int res = sceKernelAllocateDirectMemory(
        0, (off_t)sceKernelGetDirectMemorySize(), arena->size,
        DIRECT_MEMORY_ALIGNMENT, ORBIS_KERNEL_WC_GARLIC, &arena->direct_memory);
    if (res != 0) {
        printf("sceKernelAllocateDirectMemory failed: 0x%x\n", res);
        return false;
    }

    const int prot = ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW |
                     ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE;
    void* mapped = NULL;
    res = sceKernelMapDirectMemory(&mapped, arena->size, prot, 0,
                                   arena->direct_memory,
                                   DIRECT_MEMORY_ALIGNMENT);
    if (res != 0) {
        printf("sceKernelMapDirectMemory failed: 0x%x\n", res);
        sceKernelReleaseDirectMemory((int64_t)arena->direct_memory, arena->size);
        arena->direct_memory = -1;
        return false;
    }

    arena->base = (uint8_t*)mapped;
    return true;
}

static void arena_destroy(GpuArena* arena) {
    if (arena->base) {
        sceKernelMunmap(arena->base, arena->size);
    }
    if (arena->direct_memory >= 0) {
        sceKernelReleaseDirectMemory((int64_t)arena->direct_memory, arena->size);
    }
    memset(arena, 0, sizeof(*arena));
    arena->direct_memory = -1;
}

static void* arena_alloc(GpuArena* arena, size_t size, size_t alignment) {
    if (!alignment || (alignment & (alignment - 1)) != 0) {
        return NULL;
    }

    const size_t offset = align_up(arena->used, alignment);
    if (offset > arena->size || size > arena->size - offset) {
        return NULL;
    }

    void* result = arena->base + offset;
    arena->used = offset + size;
    return result;
}

static bool read_file(const char* path, uint8_t** out_data, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("failed to open %s\n", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long end = ftell(f);
    if (end <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    uint8_t* data = (uint8_t*)malloc((size_t)end);
    if (!data) {
        fclose(f);
        return false;
    }

    const size_t got = fread(data, 1, (size_t)end, f);
    fclose(f);
    if (got != (size_t)end) {
        free(data);
        return false;
    }

    *out_data = data;
    *out_size = got;
    return true;
}

static bool range_inside(const uint8_t* base, size_t size,
                         const void* ptr, size_t len) {
    const uintptr_t start = (uintptr_t)base;
    const uintptr_t p = (uintptr_t)ptr;
    if (p < start) {
        return false;
    }
    const uintptr_t rel = p - start;
    return rel <= size && len <= size - rel;
}

static bool load_shader(GpuArena* arena, const char* path,
                        GnmShaderType expected_type, LoadedShader* out) {
    memset(out, 0, sizeof(*out));

    uint8_t* file = NULL;
    size_t file_size = 0;
    if (!read_file(path, &file, &file_size)) {
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
    const size_t stage_header_bytes =
        header->headersizedwords ? (size_t)header->headersizedwords * 4
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
    } else if (expected_type == GNM_SHADER_PIXEL) {
        const GnmPsShader* ps = (const GnmPsShader*)common;
        stage_size = sceGnmPsShaderCalcSize(ps);
        code_src = sceGnmPsShaderCodePtr(ps);
    } else {
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
        if (file_size < 0x14) {
            free(file);
            return false;
        }
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

    if (!range_inside(shader_base, shader_size, code_src, code_size)) {
        free(file);
        return false;
    }

    void* stage_copy = malloc(stage_size);
    void* gpu_code = arena_alloc(arena, code_size, GNM_ALIGNMENT_SHADER_BYTES);
    if (!stage_copy || !gpu_code) {
        free(stage_copy);
        free(file);
        return false;
    }

    memcpy(stage_copy, common, stage_size);
    memcpy(gpu_code, code_src, code_size);

    if (expected_type == GNM_SHADER_VERTEX) {
        GnmVsShader* vs = (GnmVsShader*)stage_copy;
        sceGnmVsRegsSetAddress(&vs->registers, gpu_code);
    } else {
        GnmPsShader* ps = (GnmPsShader*)stage_copy;
        sceGnmPsRegsSetAddress(&ps->registers, gpu_code);
    }

    out->stage = stage_copy;
    out->gpu_code = gpu_code;
    out->gpu_code_size = code_size;
    out->type = expected_type;
    free(file);
    return true;
}

static void unload_shader(LoadedShader* shader) {
    free(shader->stage);
    memset(shader, 0, sizeof(*shader));
}

static bool create_linear_rt(GpuArena* arena, GnmRenderTarget* rt,
                             void** out_memory, uint64_t* out_size) {
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

    GnmError err = sceGnmCreateRenderTarget(rt, &info);
    if (err != GNM_ERROR_OK) {
        printf("sceGnmCreateRenderTarget failed: %d\n", (int)err);
        return false;
    }

    uint64_t size = 0;
    uint32_t alignment = 0;
    err = sceGnmRtCalcByteSize(&size, &alignment, rt);
    if (err != GNM_ERROR_OK || size == 0 || alignment == 0) {
        printf("sceGnmRtCalcByteSize failed: %d\n", (int)err);
        return false;
    }

    void* memory = arena_alloc(arena, (size_t)size, alignment);
    if (!memory) {
        return false;
    }
    memset(memory, 0, (size_t)size);
    sceGnmRtSetBaseAddr(rt, memory);

    *out_memory = memory;
    *out_size = size;
    return true;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("shadps4-open-test: %s start\n", TEST_NAME);

    GpuArena arena;
    if (!arena_init(&arena)) {
        return fail("direct_memory");
    }

    int result = 1;
    LoadedShader vs = {0};
    LoadedShader ps = {0};

    GnmRenderTarget rt = {0};
    void* rt_memory = NULL;
    uint64_t rt_size = 0;
    if (!create_linear_rt(&arena, &rt, &rt_memory, &rt_size)) {
        result = fail("render_target");
        goto cleanup;
    }

    if (!load_shader(&arena, "/app0/assets/fullscreen.vert.sb",
                     GNM_SHADER_VERTEX, &vs)) {
        result = fail("vertex_shader");
        goto cleanup;
    }
    if (!load_shader(&arena, "/app0/assets/solid.frag.sb",
                     GNM_SHADER_PIXEL, &ps)) {
        result = fail("pixel_shader");
        goto cleanup;
    }

    GnmVsShader* vshader = (GnmVsShader*)vs.stage;
    GnmPsShader* pshader = (GnmPsShader*)ps.stage;
    if (vshader->numexportsemantics != 0 || pshader->numinputsemantics != 0) {
        result = fail("unexpected_shader_io");
        goto cleanup;
    }

    void* cmd_memory = arena_alloc(&arena, COMMAND_BUFFER_SIZE, 256);
    volatile uint64_t* eop_label =
        (volatile uint64_t*)arena_alloc(&arena, sizeof(uint64_t), sizeof(uint64_t));
    if (!cmd_memory || !eop_label) {
        result = fail("command_memory");
        goto cleanup;
    }
    *eop_label = 0;

    GnmCommandBuffer cmd =
        sceGnmCmdInit(cmd_memory, COMMAND_BUFFER_SIZE, NULL, NULL);
    sceGnmDrawCmdInitDefaultHardwareState(&cmd);

    const GnmPrimitiveSetup primitive = {
        .cullmode = GNM_CULL_NONE,
        .frontface = GNM_FACE_CCW,
        .frontmode = GNM_FILL_SOLID,
        .backmode = GNM_FILL_SOLID,
        .frontoffsetmode = false,
        .backoffsetmode = false,
        .vertexwindowoffsetenable = false,
        .provokemode = GNM_PROVOKINGVTX_FIRST,
        .perspectivecorrectiondisable = false,
    };
    sceGnmDrawCmdSetPrimitiveSetup(&cmd, &primitive);
    sceGnmDrawCmdSetRenderTarget(&cmd, 0, &rt);
    sceGnmDrawCmdSetRenderTargetMask(&cmd, 0x0f);
    sceGnmDrawCmdSetScreenScissor(&cmd, 0, 0, WIDTH, HEIGHT);

    const GnmSetViewportInfo viewport = {
        .dmin = 0.0f,
        .dmax = 1.0f,
        .scale = {WIDTH * 0.5f, -HEIGHT * 0.5f, 0.5f},
        .offset = {WIDTH * 0.5f, HEIGHT * 0.5f, 0.5f},
    };
    sceGnmDrawCmdSetViewport(&cmd, 0, &viewport);

    sceGnmDrawCmdSetVsShader(&cmd, &vshader->registers, 0);
    sceGnmDrawCmdSetPsShader(&cmd, &pshader->registers);
    sceGnmDrawCmdSetPrimitiveType(&cmd, GNM_PT_TRILIST);
    sceGnmDrawCmdDrawIndexAuto(&cmd, 3);

    sceGnmDrawCmdEventWriteEop(
        &cmd, GNM_FLUSH_AND_INV_CB_DATA_TS,
        (uint64_t)(uintptr_t)eop_label, GNM_DATA_SEL_SEND_DATA64, EOP_VALUE);

    void* dcb_addrs[1] = {cmd.beginptr};
    uint32_t dcb_sizes[1] = {
        (uint32_t)((uintptr_t)cmd.cmdptr - (uintptr_t)cmd.beginptr),
    };

    int32_t submit_res =
        sceGnmSubmitCommandBuffers(1, dcb_addrs, dcb_sizes, NULL, NULL);
    if (submit_res < 0) {
        char detail[96];
        snprintf(detail, sizeof(detail), "submit_0x%x", (unsigned)submit_res);
        result = fail(detail);
        goto cleanup;
    }

    int32_t done_res = sceGnmSubmitDone();
    if (done_res < 0) {
        char detail[96];
        snprintf(detail, sizeof(detail), "submit_done_0x%x", (unsigned)done_res);
        result = fail(detail);
        goto cleanup;
    }

    bool completed = false;
    for (unsigned i = 0; i < EOP_WAIT_MS; ++i) {
        if (*eop_label == EOP_VALUE) {
            completed = true;
            break;
        }
        sceKernelUsleep(1000);
    }
    if (!completed) {
        result = fail("eop_timeout");
        goto cleanup;
    }

    const uint32_t pitch = sceGnmRtGetPitch(&rt);
    const uint32_t* pixels = (const uint32_t*)rt_memory;
    static const uint32_t coords[4] = {8, 24, 40, 56};
    unsigned samples = 0;
    for (unsigned yi = 0; yi < 4; ++yi) {
        for (unsigned xi = 0; xi < 4; ++xi) {
            const uint32_t x = coords[xi];
            const uint32_t y = coords[yi];
            const uint32_t got = pixels[(size_t)y * pitch + x];
            ++samples;
            if (got != 0xffffffffu) {
                char detail[160];
                snprintf(detail, sizeof(detail),
                         "reason=pixel_mismatch x=%u y=%u got=%08x pitch=%u",
                         x, y, got, pitch);
                emit_marker("FAIL", detail);
                result = 1;
                goto cleanup;
            }
        }
    }

    {
        char detail[160];
        snprintf(detail, sizeof(detail),
                 "samples=%u expected=ffffffff pitch=%u rt_bytes=%llu dcb_bytes=%u",
                 samples, pitch, (unsigned long long)rt_size, dcb_sizes[0]);
        emit_marker("PASS", detail);
    }
    result = 0;

cleanup:
    unload_shader(&ps);
    unload_shader(&vs);
    arena_destroy(&arena);
    return result;
}
