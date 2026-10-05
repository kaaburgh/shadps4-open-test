/*
 * ordered_async_handoff: Stage 7 of docs/memory-uma-test-roadmap.md.
 *
 * CPU/GPU handoffs that rely only on guest-visible order, with GPU work
 * queued ahead of the CPU:
 *
 *   1. Before any input exists, the CPU submits ROUNDS command buffers.
 *      Command buffer r:
 *        WAIT_REG_MEM go >= r     (GPU waits for the CPU's release)
 *        dispatch out[r] = transform(in[r])
 *        EOP: done[r] = token(r)
 *   2. For r = 1..ROUNDS the CPU:
 *        writes in[r] (in[1] is a seed, later inputs are derived from the
 *        verified out[r-1]),
 *        sfence, then releases go = r,
 *        polls done[r], then verifies out[r] word by word.
 *
 * So the CPU writes each input after the command buffer that reads it was
 * submitted, and it reads out[r] while command buffers r+1.. are still
 * queued and blocked on a release only it will give. A host that uploads at
 * submit time hands the GPU unwritten inputs. A host whose readback waits
 * for the whole GPU queue to go idle never returns from the read of out[r]
 * (the run then ends without a marker, as an infrastructure failure).
 *
 * Every slot is fresh memory, so no stale cache line can satisfy a read.
 */

#define SHADTEST_NAME "ordered_async_handoff"
#include "shadtest_guest.h"

enum {
    ROUNDS = 4,
    REGION_BYTES = 16 * 1024,
    REGION_WORDS = REGION_BYTES / sizeof(uint32_t),
    GROUPS_X = REGION_WORDS / ST_LOCAL_SIZE_X,
    BINDINGS = 2,
    DCB_BYTES = 4 * 1024,
    ARENA_BYTES = 2 * 1024 * 1024,
};

_Static_assert(REGION_WORDS % ST_LOCAL_SIZE_X == 0,
               "regions must be whole workgroups");

static const uint32_t INPUT_POISON = 0x9a9a9a9au;
static const uint32_t OUTPUT_POISON = 0xcdcdcdcdu;

/* Must match transform.comp. */
static uint32_t transform(uint32_t v, uint32_t i) {
    return ((v << 5) | (v >> 27)) ^ (i * 0x2545F491u) ^ 0x6A09E667u;
}

static uint32_t seed(uint32_t i) {
    return (i * 0x9E3779B1u) ^ 0x0BADF00Du;
}

/* Input for round r + 1, derived from the verified output of round r. */
static uint32_t next_input(uint32_t out, unsigned round, uint32_t i) {
    return out ^ (round * 0x7F4A7C15u) ^ (i << 3);
}

static uint64_t token(unsigned round) {
    return 0x4F52444552000000ULL | round; /* "ORDER" */
}

static inline void store_fence(void) {
    __asm__ __volatile__("sfence" ::: "memory");
}

typedef struct {
    void* memory;
    uint32_t bytes;
} Dcb;

static bool build_round(Dcb* dcb, void* memory, const StComputeShader* shader,
                        GnmBuffer* table, volatile uint32_t* go, unsigned round,
                        volatile uint64_t* done) {
    GnmCommandBuffer cmd = sceGnmCmdInit(memory, DCB_BYTES, NULL, NULL);
    memset(&cmd.flags, 0, sizeof(cmd.flags));
    sceGnmDrawCmdInitDefaultHardwareState(&cmd);

    sceGnmDrawCmdWaitMem(&cmd, GNM_WAIT_REG_MEM_FUNC_GREATER_EQUAL,
                         (uint64_t)(uintptr_t)go, round, 0xffffffffu);
    sceGnmDrawCmdSetCsShader(&cmd, &shader->shader->registers);
    sceGnmDrawCmdSetPointerUserData(&cmd, GNM_STAGE_CS,
                                    shader->descriptor_table_slot, table);
    sceGnmDrawCmdDispatchDirect(&cmd, GROUPS_X, 1, 1, 0);
    sceGnmDrawCmdEventWriteEop(&cmd, GNM_CACHE_FLUSH_AND_INV_TS_EVENT,
                               (uint64_t)(uintptr_t)done,
                               GNM_DATA_SEL_SEND_DATA64, token(round));

    dcb->memory = cmd.beginptr;
    dcb->bytes = (uint32_t)((uintptr_t)cmd.cmdptr - (uintptr_t)cmd.beginptr);
    return st_gnm_errors == 0;
}

static bool wait_label(volatile uint64_t* label, uint64_t value) {
    for (unsigned ms = 0; ms < ST_EOP_WAIT_MS; ++ms) {
        if (*label == value) {
            st_compiler_barrier();
            return true;
        }
        sceKernelUsleep(1000);
    }
    return false;
}

int main(void) {
    st_begin();

    StArena arena;
    if (!st_arena_init(&arena, ARENA_BYTES)) {
        return st_fail("reason=direct_memory");
    }

    StComputeShader shader;
    if (!st_load_compute_shader(&arena, "/app0/assets/transform.comp.sb",
                                &shader)) {
        return st_fail("reason=shader_load");
    }

    /* Control words get their own page: go (CPU-written, GPU-polled) and
     * done[r] (EOP labels). */
    uint8_t* control =
        (uint8_t*)st_arena_alloc(&arena, ST_PAGE_BYTES, ST_PAGE_BYTES);
    GnmBuffer* tables = (GnmBuffer*)st_arena_alloc(
        &arena, ROUNDS * BINDINGS * sizeof(GnmBuffer), ST_PAGE_BYTES);
    uint8_t* dcb_memory =
        (uint8_t*)st_arena_alloc(&arena, ROUNDS * DCB_BYTES, ST_PAGE_BYTES);
    volatile uint32_t* in[ROUNDS];
    volatile uint32_t* out[ROUNDS];
    for (unsigned r = 0; r < ROUNDS; ++r) {
        in[r] = (volatile uint32_t*)st_arena_alloc(&arena, REGION_BYTES,
                                                   ST_PAGE_BYTES);
        out[r] = (volatile uint32_t*)st_arena_alloc(&arena, REGION_BYTES,
                                                    ST_PAGE_BYTES);
    }
    if (!control || !tables || !dcb_memory || !out[ROUNDS - 1]) {
        return st_fail("reason=arena_memory");
    }

    volatile uint32_t* const go = (volatile uint32_t*)control;
    volatile uint64_t* const done = (volatile uint64_t*)(control + 64);
    *go = 0;
    for (unsigned r = 0; r < ROUNDS; ++r) {
        done[r] = 0;
        st_fill_words(in[r], REGION_WORDS, INPUT_POISON);
        st_fill_words(out[r], REGION_WORDS, OUTPUT_POISON);
        tables[r * BINDINGS + 0] = st_raw_buffer(in[r], REGION_BYTES);
        tables[r * BINDINGS + 1] = st_raw_buffer(out[r], REGION_BYTES);
    }

    /* 1. Queue every round before any input exists. */
    for (unsigned r = 0; r < ROUNDS; ++r) {
        Dcb dcb;
        if (!build_round(&dcb, dcb_memory + r * DCB_BYTES, &shader,
                         &tables[r * BINDINGS], go, r + 1, &done[r])) {
            return st_fail("reason=gnm_error stage=build round=%u", r + 1);
        }
        void* addrs[1] = {dcb.memory};
        uint32_t sizes[1] = {dcb.bytes};
        st_compiler_barrier();
        const int32_t res =
            sceGnmSubmitCommandBuffers(1, addrs, sizes, NULL, NULL);
        if (res < 0) {
            return st_fail("reason=submit round=%u res=0x%x", r + 1,
                           (unsigned)res);
        }
    }
    const int32_t done_res = sceGnmSubmitDone();
    if (done_res < 0) {
        return st_fail("reason=submit_done res=0x%x", (unsigned)done_res);
    }

    /* Nothing may complete before its release. */
    sceKernelUsleep(20 * 1000);
    for (unsigned r = 0; r < ROUNDS; ++r) {
        if (done[r] != 0) {
            return st_fail("reason=ran_before_release round=%u", r + 1);
        }
    }

    /* 2. Release the rounds one at a time. `expected_in` is the CPU's own
     * copy of each round's input, so verification never trusts guest memory
     * for its expectations. */
    uint32_t* expected_in = (uint32_t*)malloc(REGION_BYTES);
    if (!expected_in) {
        return st_fail("reason=host_memory");
    }
    for (uint32_t i = 0; i < REGION_WORDS; ++i) {
        expected_in[i] = seed(i);
    }

    uint32_t checksum = 0x811c9dc5u;
    for (unsigned r = 0; r < ROUNDS; ++r) {
        const unsigned round = r + 1;
        for (uint32_t i = 0; i < REGION_WORDS; ++i) {
            in[r][i] = expected_in[i];
        }
        store_fence();
        *go = round;
        store_fence();

        if (!wait_label(&done[r], token(round))) {
            return st_fail("reason=eop_timeout round=%u", round);
        }

        /* Later rounds must still be waiting for their release. */
        for (unsigned later = r + 1; later < ROUNDS; ++later) {
            if (done[later] != 0) {
                return st_fail("reason=ran_before_release round=%u", later + 1);
            }
        }

        /* What the GPU read first, then whether the CPU's input survived. */
        for (uint32_t i = 0; i < REGION_WORDS; ++i) {
            const uint32_t expected = transform(expected_in[i], i);
            const uint32_t got = out[r][i];
            if (got == expected) {
                continue;
            }
            const char* reason = "output_mismatch";
            if (got == OUTPUT_POISON) {
                reason = "output_unwritten";
            } else if (got == transform(INPUT_POISON, i)) {
                reason = "gpu_read_unreleased_input";
            }
            return st_fail("reason=%s round=%u index=%u expected=%08x got=%08x",
                           reason, round, i, expected, got);
        }
        for (uint32_t i = 0; i < REGION_WORDS; ++i) {
            if (in[r][i] != expected_in[i]) {
                return st_fail(
                    "reason=input_changed round=%u index=%u expected=%08x "
                    "got=%08x",
                    round, i, expected_in[i], in[r][i]);
            }
        }
        printf("round %u: ok\n", round);

        for (uint32_t i = 0; i < REGION_WORDS; ++i) {
            const uint32_t produced = transform(expected_in[i], i);
            expected_in[i] = next_input(produced, round, i);
            uint32_t v = produced;
            for (int byte = 0; byte < 4; ++byte) {
                checksum ^= v & 0xffu;
                checksum *= 0x01000193u;
                v >>= 8;
            }
        }
    }

    if (st_gnm_errors != 0) {
        return st_fail("reason=gnm_error count=%u", st_gnm_errors);
    }

    st_emit_marker("PASS", "rounds=%u queued_ahead=%u checksum=%08x",
                   (unsigned)ROUNDS, (unsigned)ROUNDS, checksum);

    free(expected_in);
    st_unload_compute_shader(&shader);
    st_arena_destroy(&arena);
    return 0;
}
