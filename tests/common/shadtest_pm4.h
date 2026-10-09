/*
 * Raw PM4 packets that OpenGNM has no emitter for (milestone M0 of
 * docs/uma-e1c-e3-scenario-plan.md).
 *
 * Plain C over <stdint.h> and OpenGNM's PM4 register headers, with no guest
 * dependencies, so the host self-test (tests/common/pm4_selftest.c, run by
 * scripts/test-pm4.sh) can pin every dword. Each st_pm4_* builder writes one
 * packet sequence into dst and returns its length in dwords; the matching
 * ST_PM4_* constant gives the length up front. shadtest_guest.h emits them
 * into a command buffer.
 *
 * Field positions follow OpenGNM include/pm4/amdgfxregs.h and the packet
 * layouts shadPS4 parses in src/video_core/amdgpu/pm4_cmds.h.
 */
#ifndef SHADTEST_PM4_H
#define SHADTEST_PM4_H

#include <stdbool.h>
#include <stdint.h>

#include <pm4/sid.h>

enum {
    ST_PM4_EVENT_WRITE_DWORDS = 2,
    ST_PM4_ACQUIRE_MEM_DWORDS = 7,
    ST_PM4_PFP_SYNC_ME_DWORDS = 2,
    ST_PM4_COND_EXEC_DWORDS = 4,
    /* WRITE_DATA: header, control, address lo/hi, then the data dwords. */
    ST_PM4_WRITE_DATA_BASE_DWORDS = 4,
    ST_PM4_WRITE_DATA_MAX_DATA = 16,
    /* Two partial flushes and one ACQUIRE_MEM. */
    ST_PM4_BARRIER_MAX_DWORDS =
        2 * ST_PM4_EVENT_WRITE_DWORDS + ST_PM4_ACQUIRE_MEM_DWORDS,
};

/* st_pm4_barrier flags. */
enum {
    /* Also wait for pixel shaders (after a draw), not only compute. */
    ST_PM4_BARRIER_AFTER_DRAW = 1u << 0,
    /* Also flush and invalidate the CB caches (a render target follows or
     * precedes the edge). */
    ST_PM4_BARRIER_CB = 1u << 1,
};

static inline uint32_t st_pm4_event_write(uint32_t* dst, uint32_t event_type,
                                          uint32_t event_index) {
    dst[0] = PKT3(PKT3_EVENT_WRITE, 0, 0);
    dst[1] = EVENT_TYPE(event_type) | EVENT_INDEX(event_index);
    return ST_PM4_EVENT_WRITE_DWORDS;
}

/* Cache actions over the whole address space. The size and poll interval
 * match OpenGNM's sceGnmDrawCmdWaitGraphicsWrite; CP_COHER_SIZE_HI is 8 bits
 * on GFX7. Bit 31 stays clear, so the PFP waits too (Mesa radeonsi does not
 * set it on GFX7 either). */
static inline uint32_t st_pm4_acquire_mem(uint32_t* dst,
                                          uint32_t cp_coher_cntl) {
    dst[0] = PKT3(PKT3_ACQUIRE_MEM, 5, 0);
    dst[1] = cp_coher_cntl;
    dst[2] = 0xffffffffu; /* CP_COHER_SIZE, 256-byte units */
    dst[3] = 0x000000ffu; /* CP_COHER_SIZE_HI */
    dst[4] = 0;           /* CP_COHER_BASE */
    dst[5] = 0;           /* CP_COHER_BASE_HI */
    dst[6] = 0x0000000au; /* POLL_INTERVAL */
    return ST_PM4_ACQUIRE_MEM_DWORDS;
}

/* L2 write-back + invalidate, vector L1 and scalar (K$) invalidate. TC_WB is
 * set with TC as in the PS4 hardware-init ACQUIRE_MEM that OpenGNM replays
 * (src/hwinit_sequences.h); Mesa radeonsi sets it from GFX8 on. */
static inline uint32_t st_pm4_full_cache_cntl(bool cb) {
    uint32_t cntl = S_0301F0_TC_ACTION_ENA(1) | S_0301F0_TC_WB_ACTION_ENA(1) |
                    S_0301F0_TCL1_ACTION_ENA(1) |
                    S_0301F0_SH_KCACHE_ACTION_ENA(1);
    if (cb) {
        cntl |= S_0301F0_CB_ACTION_ENA(1) | S_0301F0_CB0_DEST_BASE_ENA(1) |
                S_0301F0_CB1_DEST_BASE_ENA(1) | S_0301F0_CB2_DEST_BASE_ENA(1) |
                S_0301F0_CB3_DEST_BASE_ENA(1) | S_0301F0_CB4_DEST_BASE_ENA(1) |
                S_0301F0_CB5_DEST_BASE_ENA(1) | S_0301F0_CB6_DEST_BASE_ENA(1) |
                S_0301F0_CB7_DEST_BASE_ENA(1);
    }
    return cntl;
}

/* The plan's [B]: earlier shaders finish, then every cache between the
 * agents is written back and invalidated. */
static inline uint32_t st_pm4_barrier(uint32_t* dst, unsigned flags) {
    uint32_t n = 0;
    if (flags & ST_PM4_BARRIER_AFTER_DRAW) {
        n += st_pm4_event_write(dst + n, V_028A90_PS_PARTIAL_FLUSH, 4);
    }
    n += st_pm4_event_write(dst + n, V_028A90_CS_PARTIAL_FLUSH, 4);
    n += st_pm4_acquire_mem(
        dst + n, st_pm4_full_cache_cntl((flags & ST_PM4_BARRIER_CB) != 0));
    return n;
}

/* The plan's [P]: the PFP waits until the ME has caught up. */
static inline uint32_t st_pm4_pfp_sync_me(uint32_t* dst) {
    dst[0] = PKT3(PKT3_PFP_SYNC_ME, 0, 0);
    dst[1] = 0;
    return ST_PM4_PFP_SYNC_ME_DWORDS;
}

static inline uint32_t st_pm4_write_data_dwords(uint32_t count) {
    return ST_PM4_WRITE_DATA_BASE_DWORDS + count;
}

/* WRITE_DATA of count dwords to memory, executed by the ME after the write is
 * confirmed. shadPS4 accepts DST_SEL 2 and 5 only. Returns 0 for an
 * unsupported count. */
static inline uint32_t st_pm4_write_data(uint32_t* dst, uint64_t va,
                                         const uint32_t* data, uint32_t count) {
    if (count == 0 || count > ST_PM4_WRITE_DATA_MAX_DATA || (va & 3) != 0) {
        return 0;
    }
    dst[0] = PKT3(PKT3_WRITE_DATA, 2 + count, 0);
    dst[1] = S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) |
             S_370_ENGINE_SEL(V_370_ME);
    dst[2] = (uint32_t)va;
    dst[3] = (uint32_t)(va >> 32);
    for (uint32_t i = 0; i < count; ++i) {
        dst[ST_PM4_WRITE_DATA_BASE_DWORDS + i] = data[i];
    }
    return st_pm4_write_data_dwords(count);
}

/* COND_EXEC: skip the next exec_count dwords when the predicate at va is
 * zero. This is the three-dword body shadPS4 parses (PM4CmdCondExec:
 * address lo, address hi + COMMAND, EXEC_COUNT). Mesa emits a four-dword
 * body with a cache-policy dword on GFX8+ (radv); which form Liverpool takes
 * is not settled by the sources at hand. shadPS4 reads only the predicate's
 * first byte, so predicates should be 0 or 1. Returns 0 for a bad argument. */
static inline uint32_t st_pm4_cond_exec(uint32_t* dst, uint64_t va,
                                        uint32_t exec_count) {
    if ((va & 3) != 0 || (va >> 48) != 0 || exec_count > 0x3fff) {
        return 0;
    }
    dst[0] = PKT3(PKT3_COND_EXEC, 2, 0);
    dst[1] = (uint32_t)va & ~3u;
    dst[2] = (uint32_t)(va >> 32) & 0xffffu; /* COMMAND (31:28) = 0 */
    dst[3] = exec_count;
    return ST_PM4_COND_EXEC_DWORDS;
}

#endif /* SHADTEST_PM4_H */
