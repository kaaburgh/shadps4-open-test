#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <orbis/libkernel.h>

#include <gnm_buffer.h>
#include <gnm_commandbuffer.h>
#include <gnm_error.h>
#include <gnm_drawcommandbuffer.h>
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
    WORD_COUNT = 16 * 1024,
    BUFFER_BYTES = WORD_COUNT * sizeof(uint32_t),
    GUARD_BYTES = 4 * 1024,
    DATA_BYTES = GUARD_BYTES + BUFFER_BYTES + GUARD_BYTES +
                 BUFFER_BYTES + GUARD_BYTES,
    GENERATIONS = 8,
    DIRECT_MEMORY_SIZE = 2 * 1024 * 1024,
    DIRECT_MEMORY_ALIGNMENT = 2 * 1024 * 1024,
    COMMAND_BUFFER_SIZE = 64 * 1024,
    EOP_WAIT_MS = 5000,
    LOCAL_SIZE_X = 64,
    DISPATCH_X = WORD_COUNT / LOCAL_SIZE_X,
};

static const char* const TEST_NAME = "buffer_pingpong";
static const uint32_t BASE_SEED = 0x2468ace1u;
static const uint32_t OUTPUT_POISON = 0xcdcdcdcdu;
static const uint32_t GUARD_VALUE = 0x5a17c3e9u;
static const uint32_t CONST_A = 0xa5a5a5a5u;
static const uint32_t CONST_B = 0x9e3779b1u;
static const uint32_t GENERATION_MIX = 0x7f4a7c15u;
static const uint64_t EOP_BASE = 0x50494e47504f4e47ULL; /* "PINGPONG" */

typedef struct {
    off_t direct_memory;
    uint8_t* base;
    size_t size;
    size_t used;
} GpuArena;

typedef struct {
    GnmCsShader* shader;
    void* gpu_code;
    uint32_t gpu_code_size;
    uint8_t descriptor_table_slot;
} LoadedComputeShader;

static size_t align_up(size_t value, size_t alignment) {
    const size_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

static inline void compiler_memory_barrier(void) {
    __asm__ __volatile__("" ::: "memory");
}

static void emit_marker(const char* status, const char* detail) {
    char line[256];
    if (detail && detail[0]) {
        snprintf(line, sizeof(line), "SHADTEST name=%s status=%s %s",
                 TEST_NAME, status, detail);
    } else {
        snprintf(line, sizeof(line), "SHADTEST name=%s status=%s",
                 TEST_NAME, status);
    }

    puts(line);
    fflush(stdout);
    sceKernelDebugOutText(0, line);
    sceKernelDebugOutText(0, "\n");
}

static unsigned g_gnm_errors = 0;

static void on_gnm_message(GnmMessageSeverity sev, const char* msg, void* user) {
    (void)user;
    if (sev == GNM_MSGSEV_ERR) {
        g_gnm_errors += 1;
    }
    printf("opengnm %s: %s\n",
           sev == GNM_MSGSEV_ERR ? "error" : "warning", msg);
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
        DIRECT_MEMORY_ALIGNMENT, ORBIS_KERNEL_WC_GARLIC,
        &arena->direct_memory);
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
        sceKernelReleaseDirectMemory((int64_t)arena->direct_memory,
                                     arena->size);
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
        sceKernelReleaseDirectMemory((int64_t)arena->direct_memory,
                                     arena->size);
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

    const long end = ftell(f);
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

static bool load_compute_shader(GpuArena* arena, const char* path,
                                LoadedComputeShader* out) {
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

    if ((GnmShaderType)header->type != GNM_SHADER_COMPUTE) {
        printf("%s has unexpected shader type %u\n", path, header->type);
        free(file);
        return false;
    }

    const uint8_t* shader_base = file + shader_offset;
    const size_t shader_size = file_size - shader_offset;
    const size_t stage_header_bytes =
        header->headersizedwords ? (size_t)header->headersizedwords * 4
                                 : sizeof(GnmCsShader);
    if (stage_header_bytes < sizeof(GnmCsShader) ||
        sizeof(GnmShaderFileHeader) + stage_header_bytes > shader_size) {
        free(file);
        return false;
    }

    const GnmCsShader* cs = (const GnmCsShader*)(
        shader_base + sizeof(GnmShaderFileHeader));
    const size_t slot_bytes =
        (size_t)cs->common.numinputusageslots * sizeof(GnmInputUsageSlot);
    if (sizeof(GnmCsShader) + slot_bytes > stage_header_bytes) {
        free(file);
        return false;
    }

    const GnmInputUsageSlot* slots =
        (const GnmInputUsageSlot*)((const uint8_t*)cs + sizeof(GnmCsShader));
    unsigned descriptor_slots = 0;
    uint8_t descriptor_table_slot = 0;
    for (uint32_t i = 0; i < cs->common.numinputusageslots; ++i) {
        if (slots[i].usagetype ==
            GNM_SHINPUTUSAGE_PTR_INDIRECTRESOURCETABLE) {
            descriptor_slots += 1;
            descriptor_table_slot = slots[i].startregister;
        } else {
            printf("unsupported compute input usage type 0x%x\n",
                   slots[i].usagetype);
            free(file);
            return false;
        }
    }
    if (descriptor_slots != 1) {
        printf("expected one indirect resource table slot, got %u\n",
               descriptor_slots);
        free(file);
        return false;
    }

    const void* code_src =
        (const uint8_t*)cs + cs->registers.computepgmlo;
    uint32_t code_size = sceGnmShaderCommonCodeSize(&cs->common);
    if (wrapped_container) {
        uint32_t container_code_size = 0;
        if (file_size < 0x14) {
            free(file);
            return false;
        }
        memcpy(&container_code_size, file + 0x10,
               sizeof(container_code_size));
        if (container_code_size < sizeof(GnmShaderBinaryInfo) ||
            shader_offset + container_code_size > file_size) {
            free(file);
            return false;
        }

        const GnmShaderBinaryInfo* binary_info =
            (const GnmShaderBinaryInfo*)(
                shader_base + container_code_size -
                sizeof(GnmShaderBinaryInfo));
        code_size = binary_info->length;
    }

    if (!range_inside(shader_base, shader_size, code_src, code_size)) {
        free(file);
        return false;
    }

    uint32_t copy_size = code_size;
    const uint8_t* after_code = (const uint8_t*)code_src + code_size;
    if (range_inside(shader_base, shader_size, after_code,
                     sizeof(GnmShaderBinaryInfo)) &&
        memcmp(after_code, GNM_SHADER_BINARY_INFO_MAGIC, 7) == 0) {
        copy_size += sizeof(GnmShaderBinaryInfo);
    }

    GnmCsShader* stage_copy = (GnmCsShader*)malloc(stage_header_bytes);
    void* gpu_code =
        arena_alloc(arena, copy_size, GNM_ALIGNMENT_SHADER_BYTES);
    if (!stage_copy || !gpu_code) {
        free(stage_copy);
        free(file);
        return false;
    }

    memcpy(stage_copy, cs, stage_header_bytes);
    memcpy(gpu_code, code_src, copy_size);
    sceGnmCsRegsSetAddress(&stage_copy->registers, gpu_code);

    out->shader = stage_copy;
    out->gpu_code = gpu_code;
    out->gpu_code_size = code_size;
    out->descriptor_table_slot = descriptor_table_slot;

    free(file);
    return true;
}

static void unload_compute_shader(LoadedComputeShader* shader) {
    free(shader->shader);
    memset(shader, 0, sizeof(*shader));
}

static uint32_t input_value(uint32_t generation, uint32_t i) {
    return BASE_SEED ^ (generation * GENERATION_MIX) ^
           (i * 0x45d9f3bu) ^ (i << 16) ^ (i >> 3);
}

static uint32_t expected_value(uint32_t generation, uint32_t i) {
    return (input_value(generation, i) ^ CONST_A) + i * CONST_B;
}

static void fill_words(volatile uint32_t* ptr, size_t bytes, uint32_t value) {
    const size_t count = bytes / sizeof(uint32_t);
    for (size_t i = 0; i < count; ++i) {
        ptr[i] = value;
    }
}

static GnmBuffer make_raw_buffer(void* base, uint32_t bytes) {
    GnmBuffer buffer = sceGnmCreateConstBuffer(base, bytes);
    buffer.stride = 0;
    buffer.numrecords = bytes;
    sceGnmBufSetMemoryType(&buffer, GNM_MEMORY_CACHE_COHERENT,
                           false, false);
    return buffer;
}

static bool verify_guard(const volatile uint32_t* guard, const char* name,
                         uint32_t generation) {
    const size_t count = GUARD_BYTES / sizeof(uint32_t);
    for (size_t i = 0; i < count; ++i) {
        if (guard[i] != GUARD_VALUE) {
            char detail[208];
            snprintf(detail, sizeof(detail),
                     "reason=guard_mismatch generation=%u guard=%s index=%zu got=%08x expected=%08x",
                     generation, name, i, guard[i], GUARD_VALUE);
            emit_marker("FAIL", detail);
            return false;
        }
    }
    return true;
}

static bool submit_generation(void* cmd_memory,
                              const LoadedComputeShader* shader,
                              GnmBuffer* descriptor_table,
                              volatile uint64_t* eop_label,
                              uint64_t eop_value,
                              uint32_t* out_dcb_bytes) {
    GnmCommandBuffer cmd =
        sceGnmCmdInit(cmd_memory, COMMAND_BUFFER_SIZE, NULL, NULL);
    memset(&cmd.flags, 0, sizeof(cmd.flags));
    sceGnmDrawCmdInitDefaultHardwareState(&cmd);

    sceGnmDrawCmdSetCsShader(&cmd, &shader->shader->registers);
    sceGnmDrawCmdSetPointerUserData(
        &cmd, GNM_STAGE_CS, shader->descriptor_table_slot,
        descriptor_table);
    sceGnmDrawCmdDispatchDirect(&cmd, DISPATCH_X, 1, 1, 0);
    sceGnmDrawCmdEventWriteEop(
        &cmd, GNM_CACHE_FLUSH_AND_INV_TS_EVENT,
        (uint64_t)(uintptr_t)eop_label,
        GNM_DATA_SEL_SEND_DATA64, eop_value);

    if (g_gnm_errors != 0) {
        return false;
    }

    void* dcb_addrs[1] = {cmd.beginptr};
    uint32_t dcb_sizes[1] = {
        (uint32_t)((uintptr_t)cmd.cmdptr - (uintptr_t)cmd.beginptr),
    };

    const int32_t submit_res =
        sceGnmSubmitCommandBuffers(1, dcb_addrs, dcb_sizes, NULL, NULL);
    if (submit_res < 0) {
        printf("sceGnmSubmitCommandBuffers failed: 0x%x\n",
               (unsigned)submit_res);
        return false;
    }

    const int32_t done_res = sceGnmSubmitDone();
    if (done_res < 0) {
        printf("sceGnmSubmitDone failed: 0x%x\n", (unsigned)done_res);
        return false;
    }

    for (unsigned i = 0; i < EOP_WAIT_MS; ++i) {
        if (*eop_label == eop_value) {
            if (out_dcb_bytes) {
                *out_dcb_bytes = dcb_sizes[0];
            }
            return true;
        }
        sceKernelUsleep(1000);
    }
    return false;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("shadps4-open-test: %s start\n", TEST_NAME);
    sceGnmSetMessageHandler(on_gnm_message, NULL);

    GpuArena arena;
    if (!arena_init(&arena)) {
        return fail("direct_memory");
    }

    int result = 1;
    LoadedComputeShader shader = {0};

    uint8_t* data =
        (uint8_t*)arena_alloc(&arena, DATA_BYTES, GUARD_BYTES);
    if (!data) {
        result = fail("data_memory");
        goto cleanup;
    }

    volatile uint32_t* guard0 = (volatile uint32_t*)data;
    volatile uint32_t* buffer_a =
        (volatile uint32_t*)(data + GUARD_BYTES);
    volatile uint32_t* guard1 =
        (volatile uint32_t*)(data + GUARD_BYTES + BUFFER_BYTES);
    volatile uint32_t* buffer_b =
        (volatile uint32_t*)(data + GUARD_BYTES + BUFFER_BYTES + GUARD_BYTES);
    volatile uint32_t* guard2 =
        (volatile uint32_t*)(data + GUARD_BYTES + BUFFER_BYTES + GUARD_BYTES +
                            BUFFER_BYTES);

    fill_words(guard0, GUARD_BYTES, GUARD_VALUE);
    fill_words(guard1, GUARD_BYTES, GUARD_VALUE);
    fill_words(guard2, GUARD_BYTES, GUARD_VALUE);
    fill_words(buffer_a, BUFFER_BYTES, OUTPUT_POISON);
    fill_words(buffer_b, BUFFER_BYTES, OUTPUT_POISON);

    if (!load_compute_shader(&arena, "/app0/assets/pingpong.comp.sb",
                             &shader)) {
        result = fail("compute_shader");
        goto cleanup;
    }

    GnmBuffer* tables =
        (GnmBuffer*)arena_alloc(&arena, 4 * sizeof(GnmBuffer), 16);
    if (!tables) {
        result = fail("descriptor_tables");
        goto cleanup;
    }

    /* Prebuild both directions. The test loop switches only the table pointer;
     * it does not rewrite descriptors between generations. */
    tables[0] =
        make_raw_buffer((void*)(uintptr_t)buffer_a, BUFFER_BYTES);
    tables[1] =
        make_raw_buffer((void*)(uintptr_t)buffer_b, BUFFER_BYTES);
    tables[2] =
        make_raw_buffer((void*)(uintptr_t)buffer_b, BUFFER_BYTES);
    tables[3] =
        make_raw_buffer((void*)(uintptr_t)buffer_a, BUFFER_BYTES);

    void* cmd_memory = arena_alloc(&arena, COMMAND_BUFFER_SIZE, 256);
    volatile uint64_t* eop_labels =
        (volatile uint64_t*)arena_alloc(
            &arena, GENERATIONS * sizeof(uint64_t), sizeof(uint64_t));
    if (!cmd_memory || !eop_labels) {
        result = fail("command_memory");
        goto cleanup;
    }
    for (uint32_t generation = 0; generation < GENERATIONS; ++generation) {
        eop_labels[generation] = 0;
    }

    uint32_t checksum = 2166136261u;
    uint32_t last_dcb_bytes = 0;

    for (uint32_t generation = 0; generation < GENERATIONS; ++generation) {
        volatile uint32_t* src =
            (generation & 1u) ? buffer_b : buffer_a;
        volatile uint32_t* dst =
            (generation & 1u) ? buffer_a : buffer_b;
        GnmBuffer* table = (generation & 1u) ? &tables[2] : &tables[0];

        /* For generation > 0 this overwrites the buffer that the GPU wrote and
         * the CPU verified in the previous generation. The next dispatch then
         * consumes those fresh CPU bytes from the same backing allocation. */
        for (uint32_t i = 0; i < WORD_COUNT; ++i) {
            src[i] = input_value(generation, i);
        }

        compiler_memory_barrier();

        const uint64_t eop_value = EOP_BASE + generation + 1;
        if (!submit_generation(
                cmd_memory, &shader, table, &eop_labels[generation],
                eop_value, &last_dcb_bytes)) {
            char detail[192];
            snprintf(detail, sizeof(detail),
                     "reason=submit_or_eop generation=%u gnm_errors=%u",
                     generation, g_gnm_errors);
            emit_marker("FAIL", detail);
            result = 1;
            goto cleanup;
        }

        compiler_memory_barrier();

        if (!verify_guard(guard0, "before_a", generation) ||
            !verify_guard(guard1, "between_buffers", generation) ||
            !verify_guard(guard2, "after_b", generation)) {
            result = 1;
            goto cleanup;
        }

        for (uint32_t i = 0; i < WORD_COUNT; ++i) {
            const uint32_t expected_src = input_value(generation, i);
            if (src[i] != expected_src) {
                char detail[208];
                snprintf(detail, sizeof(detail),
                         "reason=source_modified generation=%u index=%u got=%08x expected=%08x",
                         generation, i, src[i], expected_src);
                emit_marker("FAIL", detail);
                result = 1;
                goto cleanup;
            }
        }

        for (uint32_t i = 0; i < WORD_COUNT; ++i) {
            const uint32_t expected = expected_value(generation, i);
            const uint32_t got = dst[i];
            if (got != expected) {
                char detail[224];
                snprintf(detail, sizeof(detail),
                         "reason=destination_mismatch generation=%u index=%u got=%08x expected=%08x",
                         generation, i, got, expected);
                emit_marker("FAIL", detail);
                result = 1;
                goto cleanup;
            }
            checksum ^= got + generation;
            checksum *= 16777619u;
        }
    }

    {
        char detail[208];
        snprintf(detail, sizeof(detail),
                 "generations=%u words=%u bytes=%u handoffs=%u checksum=%08x dcb_bytes=%u",
                 GENERATIONS, WORD_COUNT, BUFFER_BYTES,
                 GENERATIONS * 2, checksum, last_dcb_bytes);
        emit_marker("PASS", detail);
    }
    result = 0;

cleanup:
    unload_compute_shader(&shader);
    arena_destroy(&arena);
    return result;
}
