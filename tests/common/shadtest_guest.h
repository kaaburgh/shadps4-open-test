/*
 * Shared guest-side helpers for shadps4-open-test compute/buffer workloads.
 *
 * Header-only (static functions) so every test stays one translation unit
 * and can be built and read on its own. A test defines SHADTEST_NAME before
 * including this header.
 *
 * What lives here is the plumbing that every buffer test needs and that is
 * not itself under test: the SHADTEST marker, OpenGNM message accounting, a
 * direct-memory arena, loading an opengnm-psbc compute shader, raw buffer
 * V#s for the psbc set-0 resource ABI (docs/psbc-resource-abi.md), and one
 * dispatch + end-of-pipe wait per submission.
 */
#ifndef SHADTEST_GUEST_H
#define SHADTEST_GUEST_H

#ifndef SHADTEST_NAME
#error "define SHADTEST_NAME before including shadtest_guest.h"
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <orbis/libkernel.h>

#include <gnm_buffer.h>
#include <gnm_commandbuffer.h>
#include <gnm_drawcommandbuffer.h>
#include <gnm_error.h>
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
    ST_PAGE_BYTES = 4 * 1024,
    ST_DIRECT_MEMORY_ALIGNMENT = 2 * 1024 * 1024,
    ST_COMMAND_BUFFER_BYTES = 64 * 1024,
    ST_EOP_WAIT_MS = 5000,
    ST_LOCAL_SIZE_X = 64,
};

/* ------------------------------------------------------------------------ */
/* Result marker and OpenGNM messages                                        */
/* ------------------------------------------------------------------------ */

static unsigned st_gnm_errors = 0;

__attribute__((format(printf, 2, 3))) static inline void st_emit_marker(
    const char* status, const char* fmt, ...) {
    char detail[224] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
    }

    char line[320];
    if (detail[0]) {
        snprintf(line, sizeof(line), "SHADTEST name=%s status=%s %s",
                 SHADTEST_NAME, status, detail);
    } else {
        snprintf(line, sizeof(line), "SHADTEST name=%s status=%s",
                 SHADTEST_NAME, status);
    }

    puts(line);
    fflush(stdout);
    sceKernelDebugOutText(0, line);
    sceKernelDebugOutText(0, "\n");
}

/* Emits a FAIL marker and returns the test's failing exit status. */
__attribute__((format(printf, 1, 2))) static inline int st_fail(
    const char* fmt, ...) {
    char detail[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    st_emit_marker("FAIL", "%s", detail);
    return 1;
}

static inline void st_on_gnm_message(GnmMessageSeverity sev, const char* msg,
                                     void* user) {
    (void)user;
    if (sev == GNM_MSGSEV_ERR) {
        st_gnm_errors += 1;
    }
    printf("opengnm %s: %s\n", sev == GNM_MSGSEV_ERR ? "error" : "warning",
           msg);
}

static inline void st_begin(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("shadps4-open-test: %s start\n", SHADTEST_NAME);
    sceGnmSetMessageHandler(st_on_gnm_message, NULL);
}

/* Keeps the compiler from moving CPU accesses to GPU-shared memory across a
 * submission or completion wait. */
static inline void st_compiler_barrier(void) {
    __asm__ __volatile__("" ::: "memory");
}

/* The arena is Garlic memory, which the CPU maps write-combining: x86 orders
 * neither WC stores nor WC loads the way it orders write-back memory, so the
 * compiler barrier alone is not enough on hardware.
 *
 * Drains the CPU's WC stores before the GPU is told to read them. */
static inline void st_store_fence(void) {
    __builtin_ia32_sfence();
}

/* Polls a GPU-written label for up to ST_EOP_WAIT_MS. Once it matches, the
 * load fence keeps later reads of GPU-written data from being satisfied
 * before the label read. */
static inline bool st_wait_label(const volatile uint64_t* label,
                                 uint64_t value) {
    for (unsigned i = 0; i < ST_EOP_WAIT_MS; ++i) {
        if (*label == value) {
            __builtin_ia32_lfence();
            st_compiler_barrier();
            return true;
        }
        sceKernelUsleep(1000);
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Direct-memory arena                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    off_t direct_memory;
    uint8_t* base;
    size_t size;
    size_t used;
} StArena;

static inline size_t st_align_up(size_t value, size_t alignment) {
    const size_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

/* Allocates and maps CPU/GPU read-write Garlic direct memory. */
static inline bool st_arena_init(StArena* arena, size_t size) {
    memset(arena, 0, sizeof(*arena));
    arena->direct_memory = -1;
    arena->size = st_align_up(size, ST_DIRECT_MEMORY_ALIGNMENT);

    int res = sceKernelAllocateDirectMemory(
        0, (off_t)sceKernelGetDirectMemorySize(), arena->size,
        ST_DIRECT_MEMORY_ALIGNMENT, ORBIS_KERNEL_WC_GARLIC,
        &arena->direct_memory);
    if (res != 0) {
        printf("sceKernelAllocateDirectMemory failed: 0x%x\n", res);
        arena->direct_memory = -1;
        return false;
    }

    const int prot = ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW |
                     ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE;
    void* mapped = NULL;
    res = sceKernelMapDirectMemory(&mapped, arena->size, prot, 0,
                                   arena->direct_memory,
                                   ST_DIRECT_MEMORY_ALIGNMENT);
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

static inline void st_arena_destroy(StArena* arena) {
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

static inline void* st_arena_alloc(StArena* arena, size_t size,
                                   size_t alignment) {
    if (!alignment || (alignment & (alignment - 1)) != 0) {
        return NULL;
    }

    const size_t offset = st_align_up(arena->used, alignment);
    if (offset > arena->size || size > arena->size - offset) {
        return NULL;
    }

    arena->used = offset + size;
    return arena->base + offset;
}

/* ------------------------------------------------------------------------ */
/* Compute shader loading (opengnm-psbc .sb files)                           */
/* ------------------------------------------------------------------------ */

typedef struct {
    GnmCsShader* shader;
    void* gpu_code;
    uint32_t gpu_code_size;
    uint8_t descriptor_table_slot;
} StComputeShader;

static inline bool st_range_inside(const uint8_t* base, size_t size,
                                   const void* ptr, size_t len) {
    const uintptr_t start = (uintptr_t)base;
    const uintptr_t p = (uintptr_t)ptr;
    if (p < start) {
        return false;
    }
    const uintptr_t rel = p - start;
    return rel <= size && len <= size - rel;
}

static inline bool st_read_file(const char* path, uint8_t** out_data,
                                size_t* out_size) {
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

/* Loads a compute shader, copies its code (and the OrbShdr footer shadPS4
 * needs to identify it) into GPU-visible memory and records the user SGPR
 * of its PTR_INDIRECTRESOURCETABLE slot. */
static inline bool st_load_compute_shader(StArena* arena, const char* path,
                                          StComputeShader* out) {
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

    if ((GnmShaderType)header->type != GNM_SHADER_COMPUTE) {
        printf("%s has unexpected shader type %u\n", path, header->type);
        free(file);
        return false;
    }

    const uint8_t* shader_base = file + shader_offset;
    const size_t shader_size = file_size - shader_offset;
    const size_t stage_header_bytes = header->headersizedwords
                                          ? (size_t)header->headersizedwords * 4
                                          : sizeof(GnmCsShader);
    if (stage_header_bytes < sizeof(GnmCsShader) ||
        sizeof(GnmShaderFileHeader) + stage_header_bytes > shader_size) {
        free(file);
        return false;
    }

    const GnmCsShader* cs =
        (const GnmCsShader*)(shader_base + sizeof(GnmShaderFileHeader));
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
        if (slots[i].usagetype == GNM_SHINPUTUSAGE_PTR_INDIRECTRESOURCETABLE) {
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

    const void* code_src = (const uint8_t*)cs + cs->registers.computepgmlo;
    uint32_t code_size = sceGnmShaderCommonCodeSize(&cs->common);
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

    GnmCsShader* stage_copy = (GnmCsShader*)malloc(stage_header_bytes);
    void* gpu_code =
        st_arena_alloc(arena, copy_size, GNM_ALIGNMENT_SHADER_BYTES);
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

static inline void st_unload_compute_shader(StComputeShader* shader) {
    free(shader->shader);
    memset(shader, 0, sizeof(*shader));
}

/* ------------------------------------------------------------------------ */
/* Buffers                                                                   */
/* ------------------------------------------------------------------------ */

/* Raw SSBO/UBO V#: with STRIDE = 0, GFX7 MUBUF treats NUM_RECORDS as the
 * byte range. sceGnmCreateConstBuffer stores a 16-byte record count, so
 * replace it. */
static inline GnmBuffer st_raw_buffer(const volatile void* base,
                                      uint32_t bytes) {
    GnmBuffer buffer = sceGnmCreateConstBuffer((void*)(uintptr_t)base, bytes);
    buffer.stride = 0;
    buffer.numrecords = bytes;
    sceGnmBufSetMemoryType(&buffer, GNM_MEMORY_CACHE_COHERENT, false, false);
    return buffer;
}

static inline void st_fill_words(volatile uint32_t* ptr, size_t count,
                                 uint32_t value) {
    for (size_t i = 0; i < count; ++i) {
        ptr[i] = value;
    }
}

/* Returns the first index whose word differs from value, or count. */
static inline size_t st_find_word_mismatch(const volatile uint32_t* ptr,
                                           size_t count, uint32_t value) {
    for (size_t i = 0; i < count; ++i) {
        if (ptr[i] != value) {
            return i;
        }
    }
    return count;
}

/* ------------------------------------------------------------------------ */
/* Submission: one compute dispatch followed by an end-of-pipe label write   */
/* ------------------------------------------------------------------------ */

typedef struct {
    void* command_memory;
    volatile uint64_t* label;
    uint64_t next_value;
    uint32_t last_dcb_bytes;
} StQueue;

static inline bool st_queue_init(StArena* arena, StQueue* queue) {
    memset(queue, 0, sizeof(*queue));
    queue->command_memory = st_arena_alloc(arena, ST_COMMAND_BUFFER_BYTES, 256);
    queue->label = (volatile uint64_t*)st_arena_alloc(arena, sizeof(uint64_t),
                                                      sizeof(uint64_t));
    if (!queue->command_memory || !queue->label) {
        return false;
    }
    *queue->label = 0;
    /* Arbitrary non-zero base; each submission writes a fresh value so a
     * stale label from an earlier submission can never satisfy the wait. */
    queue->next_value = 0x5348414454455354ULL; /* "SHADTEST" */
    return true;
}

/* Dispatches groups_x workgroups of shader with descriptor_table bound,
 * submits, and waits for the end-of-pipe label. Returns false on an OpenGNM
 * error, a submit error or a timeout. */
static inline bool st_dispatch_and_wait(StQueue* queue,
                                        const StComputeShader* shader,
                                        GnmBuffer* descriptor_table,
                                        uint32_t groups_x) {
    const uint64_t value = ++queue->next_value;

    GnmCommandBuffer cmd = sceGnmCmdInit(queue->command_memory,
                                         ST_COMMAND_BUFFER_BYTES, NULL, NULL);
    memset(&cmd.flags, 0, sizeof(cmd.flags));
    sceGnmDrawCmdInitDefaultHardwareState(&cmd);

    sceGnmDrawCmdSetCsShader(&cmd, &shader->shader->registers);
    sceGnmDrawCmdSetPointerUserData(
        &cmd, GNM_STAGE_CS, shader->descriptor_table_slot, descriptor_table);
    sceGnmDrawCmdDispatchDirect(&cmd, groups_x, 1, 1, 0);
    /* OpenGNM's EOP sets no TC cache-action bits; visibility of the shader's
     * buffer writes rests on the CACHE_COHERENT V# memory type from
     * st_raw_buffer. Validated on shadPS4 only, not on PS4 hardware. */
    sceGnmDrawCmdEventWriteEop(&cmd, GNM_CACHE_FLUSH_AND_INV_TS_EVENT,
                               (uint64_t)(uintptr_t)queue->label,
                               GNM_DATA_SEL_SEND_DATA64, value);

    if (st_gnm_errors != 0) {
        return false;
    }

    void* dcb_addrs[1] = {cmd.beginptr};
    uint32_t dcb_sizes[1] = {
        (uint32_t)((uintptr_t)cmd.cmdptr - (uintptr_t)cmd.beginptr),
    };

    st_store_fence();
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

    if (st_wait_label(queue->label, value)) {
        queue->last_dcb_bytes = dcb_sizes[0];
        return true;
    }

    printf("compute EOP timeout waiting for 0x%016llx\n",
           (unsigned long long)value);
    return false;
}

#endif /* SHADTEST_GUEST_H */
