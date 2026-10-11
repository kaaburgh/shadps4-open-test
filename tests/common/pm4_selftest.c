/*
 * Host self-test for shadtest_pm4.h: pins every dword the builders emit and
 * decodes them with the field positions shadPS4 uses
 * (src/video_core/amdgpu/pm4_cmds.h). Run by scripts/test-pm4.sh.
 */
#include <stdio.h>

#include "shadtest_pm4.h"

static unsigned g_failures = 0;

static void expect_words(const char* what, const uint32_t* got, uint32_t got_n,
                         const uint32_t* want, uint32_t want_n) {
    if (got_n != want_n) {
        printf("FAIL %s: %u dwords, expected %u\n", what, got_n, want_n);
        g_failures += 1;
        return;
    }
    for (uint32_t i = 0; i < want_n; ++i) {
        if (got[i] != want[i]) {
            printf("FAIL %s: dword %u = 0x%08x, expected 0x%08x\n", what, i,
                   got[i], want[i]);
            g_failures += 1;
        }
    }
}

static void expect(const char* what, uint32_t got, uint32_t want) {
    if (got != want) {
        printf("FAIL %s: 0x%x, expected 0x%x\n", what, got, want);
        g_failures += 1;
    }
}

/* PM4Type3Header: predicate 0, shader_type 1, opcode 15:8, count 29:16,
 * type 31:30. The packet is count + 2 dwords long. */
static void expect_header(const char* what, uint32_t header, uint32_t opcode,
                          uint32_t total_dwords) {
    expect(what, header >> 30, 3);
    expect(what, (header >> 8) & 0xff, opcode);
    expect(what, ((header >> 16) & 0x3fff) + 2, total_dwords);
    expect(what, header & 3, 0);
}

int main(void) {
    uint32_t buf[64];
    uint32_t n;

    /* EVENT_WRITE CS_PARTIAL_FLUSH: event_type 5:0 = 7, event_index 11:8 = 4. */
    n = st_pm4_event_write(buf, V_028A90_CS_PARTIAL_FLUSH, 4);
    {
        const uint32_t want[] = {0xc0004600u, 0x00000407u};
        expect_words("event_write cs_partial_flush", buf, n, want, 2);
    }

    /* ACQUIRE_MEM with TC | TC_WB | TCL1 | SH_KCACHE. */
    n = st_pm4_acquire_mem(buf, st_pm4_full_cache_cntl(false));
    {
        const uint32_t want[] = {0xc0055800u, 0x08c40000u, 0xffffffffu,
                                 0x000000ffu, 0u,          0u,
                                 0x0000000au};
        expect_words("acquire_mem full", buf, n, want, 7);
    }

    /* The CB variant adds CB_ACTION (25) and CB0..7_DEST_BASE (13:6). */
    expect("full_cache_cntl cb", st_pm4_full_cache_cntl(true), 0x0ac43fc0u);

    /* Barrier after a dispatch: CS partial flush + ACQUIRE_MEM. */
    n = st_pm4_barrier(buf, 0);
    {
        const uint32_t want[] = {0xc0004600u, 0x00000407u, 0xc0055800u,
                                 0x08c40000u, 0xffffffffu, 0x000000ffu,
                                 0u,          0u,          0x0000000au};
        expect_words("barrier", buf, n, want, 9);
    }

    /* Barrier after a draw into a render target: PS + CS partial flush. */
    n = st_pm4_barrier(buf, ST_PM4_BARRIER_AFTER_DRAW | ST_PM4_BARRIER_CB);
    {
        const uint32_t want[] = {0xc0004600u, 0x00000410u, 0xc0004600u,
                                 0x00000407u, 0xc0055800u, 0x0ac43fc0u,
                                 0xffffffffu, 0x000000ffu, 0u,
                                 0u,          0x0000000au};
        expect_words("barrier after draw", buf, n, want, 11);
    }
    expect("barrier max", n, ST_PM4_BARRIER_MAX_DWORDS);

    n = st_pm4_pfp_sync_me(buf);
    {
        const uint32_t want[] = {0xc0004200u, 0u};
        expect_words("pfp_sync_me", buf, n, want, 2);
    }

    /* WRITE_DATA of two dwords to 0x123456780. */
    const uint32_t data[2] = {0x11111111u, 0x22222222u};
    n = st_pm4_write_data(buf, 0x0000000123456780ull, data, 2);
    {
        const uint32_t want[] = {0xc0043700u, 0x00100500u, 0x23456780u,
                                 0x00000001u, 0x11111111u, 0x22222222u};
        expect_words("write_data", buf, n, want, 6);
    }
    /* shadPS4 PM4CmdWriteData: dst_sel 11:8 (asserts 2 or 5), wr_one_addr
     * 16, wr_confirm 20, engine_sel 30; data size (count - 2) * 4. */
    expect_header("write_data header", buf[0], PKT3_WRITE_DATA, 6);
    expect("write_data dst_sel", (buf[1] >> 8) & 0xf, 5);
    expect("write_data wr_one_addr", (buf[1] >> 16) & 1, 0);
    expect("write_data wr_confirm", (buf[1] >> 20) & 1, 1);
    expect("write_data engine_sel", (buf[1] >> 30) & 3, 0);
    expect("write_data size", (((buf[0] >> 16) & 0x3fff) - 2) * 4, 8);
    expect("write_data bad count", st_pm4_write_data(buf, 0x1000, data, 0), 0);
    expect("write_data unaligned", st_pm4_write_data(buf, 0x1002, data, 1), 0);

    /* COND_EXEC guarding one five-dword packet, predicate at 0x123456788. */
    n = st_pm4_cond_exec(buf, 0x0000000123456788ull,
                         st_pm4_write_data_dwords(1));
    {
        const uint32_t want[] = {0xc0022200u, 0x23456788u, 0x00000001u,
                                 0x00000005u};
        expect_words("cond_exec", buf, n, want, 4);
    }
    /* shadPS4 PM4CmdCondExec: bool_addr_lo 31:2, bool_addr_hi 15:0,
     * command 31:28 (warns unless 0), exec_count 13:0; it skips
     * NumWords + 1 + exec_count dwords from the header. */
    expect_header("cond_exec header", buf[0], PKT3_COND_EXEC, 4);
    expect("cond_exec address lo", (buf[1] >> 2) << 2, 0x23456788u);
    expect("cond_exec address hi", buf[2] & 0xffff, 0x1u);
    expect("cond_exec command", buf[2] >> 28, 0);
    expect("cond_exec exec_count", buf[3] & 0x3fff, 5);
    expect("cond_exec unaligned", st_pm4_cond_exec(buf, 0x1002, 1), 0);

    if (g_failures != 0) {
        printf("pm4_selftest: %u failure(s)\n", g_failures);
        return 1;
    }
    printf("pm4_selftest: ok\n");
    return 0;
}
