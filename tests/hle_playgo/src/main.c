/*
 * hle_playgo: libScePlayGo queries outside an initialized session.
 *
 * The test application has no PlayGo chunk data (sce_sys/playgo-chunk.dat),
 * like most homebrew. Handle 1 is the value shadPS4's scePlayGoOpen hands
 * out; a guest can pass it whether or not Open succeeded.
 *
 * Cases:
 *   lifecycle       scePlayGoInitialize with a 2 MiB buffer succeeds;
 *                   scePlayGoGetChunkId and scePlayGoGetLocus on handle 1 do
 *                   not crash; scePlayGoTerminate succeeds. Open's result is
 *                   printed only.
 *   terminated      after Terminate, GetChunkId, GetLocus, GetProgress,
 *                   GetLanguageMask and GetInstallSpeed on handle 1 fail.
 *   init_null       scePlayGoInitialize(NULL) fails.
 *
 * Failing calls are only required to return a negative error; the codes are
 * printed for reference. A case that crashes the emulator ends the run
 * without a marker; the log's last "case <name>: start" line names it.
 */

#define SHADTEST_NAME "hle_playgo"
#include "shadtest_guest.h"

typedef uint32_t PlayGoHandle;
typedef uint16_t PlayGoChunkId;

typedef struct {
    const void* buf_addr;
    uint32_t buf_size;
    uint32_t reserved;
} PlayGoInitParams;

typedef struct {
    uint64_t progress_size;
    uint64_t total_size;
} PlayGoProgress;

int32_t scePlayGoInitialize(PlayGoInitParams* param);
int32_t scePlayGoTerminate(void);
int32_t scePlayGoOpen(PlayGoHandle* handle, const void* param);
int32_t scePlayGoClose(PlayGoHandle handle);
int32_t scePlayGoGetChunkId(PlayGoHandle handle, PlayGoChunkId* ids,
                            uint32_t count, uint32_t* entries);
int32_t scePlayGoGetLocus(PlayGoHandle handle, const PlayGoChunkId* ids,
                          uint32_t count, int8_t* loci);
int32_t scePlayGoGetProgress(PlayGoHandle handle, const PlayGoChunkId* ids,
                             uint32_t count, PlayGoProgress* progress);
int32_t scePlayGoGetLanguageMask(PlayGoHandle handle, uint64_t* mask);
int32_t scePlayGoGetInstallSpeed(PlayGoHandle handle, int32_t* speed);

enum {
    HANDLE = 1,
    INIT_BUF_BYTES = 2 * 1024 * 1024,
};

static uint8_t g_init_buf[INIT_BUF_BYTES];

static void start(const StCase* c) { printf("case %s: start\n", c->name); }

static void expect_ok(StCase* c, const char* what, int32_t rc) {
    printf("  %s returned %08x\n", what, (uint32_t)rc);
    if (rc < 0) {
        st_case_note(c, what, 0, 0, (uint32_t)rc);
    }
}

static void expect_error(StCase* c, const char* what, int32_t rc) {
    printf("  %s returned %08x\n", what, (uint32_t)rc);
    if (rc >= 0) {
        st_case_note(c, what, 0, 0x80000000u, (uint32_t)rc);
    }
}

enum { C_LIFECYCLE, C_TERMINATED, C_INIT_NULL, C_COUNT };

int main(void) {
    st_begin();

    StCase cases[C_COUNT] = {
        [C_LIFECYCLE] = {.name = "lifecycle"},
        [C_TERMINATED] = {.name = "terminated"},
        [C_INIT_NULL] = {.name = "init_null"},
    };

    PlayGoChunkId ids[4] = {0, 0, 0, 0};
    uint32_t entries = 0;
    int8_t loci[4];
    PlayGoProgress progress;
    uint64_t mask = 0;
    int32_t speed = 0;

    {
        StCase* c = &cases[C_LIFECYCLE];
        start(c);
        PlayGoInitParams init = {g_init_buf, INIT_BUF_BYTES, 0};
        expect_ok(c, "initialize", scePlayGoInitialize(&init));
        PlayGoHandle handle = 0;
        printf("  open returned %08x\n",
               (uint32_t)scePlayGoOpen(&handle, NULL));
        printf("  get_chunk_id returned %08x entries=%u\n",
               (uint32_t)scePlayGoGetChunkId(HANDLE, NULL, 0, &entries),
               entries);
        printf("  get_locus returned %08x\n",
               (uint32_t)scePlayGoGetLocus(HANDLE, ids, 1, loci));
        expect_ok(c, "terminate", scePlayGoTerminate());
    }
    {
        StCase* c = &cases[C_TERMINATED];
        start(c);
        expect_error(c, "get_chunk_id_count",
                     scePlayGoGetChunkId(HANDLE, NULL, 0, &entries));
        expect_error(c, "get_chunk_id_list",
                     scePlayGoGetChunkId(HANDLE, ids, 4, &entries));
        expect_error(c, "get_locus", scePlayGoGetLocus(HANDLE, ids, 1, loci));
        expect_error(c, "get_progress",
                     scePlayGoGetProgress(HANDLE, ids, 1, &progress));
        expect_error(c, "get_language_mask",
                     scePlayGoGetLanguageMask(HANDLE, &mask));
        expect_error(c, "get_install_speed",
                     scePlayGoGetInstallSpeed(HANDLE, &speed));
    }
    {
        StCase* c = &cases[C_INIT_NULL];
        start(c);
        expect_error(c, "initialize", scePlayGoInitialize(NULL));
    }

    return st_finish_cases(cases, C_COUNT, "");
}
