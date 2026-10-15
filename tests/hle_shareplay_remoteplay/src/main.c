/*
 * hle_shareplay_remoteplay: output-pointer checks in two status queries that
 * shadPS4 answers without a session (Share Play dormant, Remote Play
 * disconnected).
 *
 * Cases:
 *   shareplay_info     sceSharePlayGetCurrentConnectionInfo fills the 56-byte
 *                      SceSharePlayConnectionInfo with status 0 (dormant) and
 *                      writes nothing past it.
 *   shareplay_null     the same call with NULL fails.
 *   remoteplay_status  sceRemoteplayGetConnectionStatus reports 0
 *                      (disconnected) for user 1000, shadPS4's default user.
 *   remoteplay_null    the same call with a NULL status fails.
 *
 * Neither library is initialized first: shadPS4 does not require it, and the
 * signatures of the initialize calls are not public. On hardware the valid
 * queries may need it. Failing calls are only required to return a negative
 * error; the codes are printed for reference.
 */

#define SHADTEST_NAME "hle_shareplay_remoteplay"
#include "shadtest_guest.h"

enum {
    SHAREPLAY_INFO_BYTES = 56,
    CANARY_BYTES = 16,
    CANARY = 0xa5,
    USER_ID = 1000,
};

int32_t sceSharePlayGetCurrentConnectionInfo(void* info);
int32_t sceRemoteplayGetConnectionStatus(int32_t user_id, int32_t* status);

static uint8_t g_info[SHAREPLAY_INFO_BYTES + CANARY_BYTES];

static void start(const StCase* c) { printf("case %s: start\n", c->name); }

static void expect_error(StCase* c, const char* what, int32_t rc) {
    printf("  %s returned %08x\n", what, (uint32_t)rc);
    if (rc >= 0) {
        st_case_note(c, what, 0, 0x80000000u, (uint32_t)rc);
    }
}

enum {
    C_SHAREPLAY_INFO,
    C_REMOTEPLAY_STATUS,
    C_SHAREPLAY_NULL,
    C_REMOTEPLAY_NULL,
    C_COUNT,
};

int main(void) {
    st_begin();

    StCase cases[C_COUNT] = {
        [C_SHAREPLAY_INFO] = {.name = "shareplay_info"},
        [C_REMOTEPLAY_STATUS] = {.name = "remoteplay_status"},
        [C_SHAREPLAY_NULL] = {.name = "shareplay_null"},
        [C_REMOTEPLAY_NULL] = {.name = "remoteplay_null"},
    };

    {
        StCase* c = &cases[C_SHAREPLAY_INFO];
        start(c);
        memset(g_info, CANARY, sizeof(g_info));
        const int32_t rc = sceSharePlayGetCurrentConnectionInfo(g_info);
        printf("  returned %08x\n", (uint32_t)rc);
        st_case_expect(c, "rc", 0, 0, (uint32_t)rc);
        int32_t status;
        memcpy(&status, g_info, sizeof(status));
        st_case_expect(c, "status", 0, 0, (uint32_t)status);
        for (unsigned i = SHAREPLAY_INFO_BYTES; i < sizeof(g_info); ++i) {
            st_case_expect(c, "past_info", i, CANARY, g_info[i]);
        }
    }
    {
        StCase* c = &cases[C_REMOTEPLAY_STATUS];
        start(c);
        int32_t status = 0x5a5a5a5a;
        const int32_t rc = sceRemoteplayGetConnectionStatus(USER_ID, &status);
        printf("  returned %08x status=%d\n", (uint32_t)rc, status);
        st_case_expect(c, "rc", 0, 0, (uint32_t)rc);
        st_case_expect(c, "status", 0, 0, (uint32_t)status);
    }
    {
        StCase* c = &cases[C_SHAREPLAY_NULL];
        start(c);
        expect_error(c, "info", sceSharePlayGetCurrentConnectionInfo(NULL));
    }
    {
        StCase* c = &cases[C_REMOTEPLAY_NULL];
        start(c);
        expect_error(c, "status", sceRemoteplayGetConnectionStatus(USER_ID, NULL));
    }

    return st_finish_cases(cases, C_COUNT, "");
}
