/*
 * hle_video_recording: which sceVideoRecordingSetInfo requests the library
 * accepts. shadPS4 does not record video; the call only validates.
 *
 * Cases:
 *   text_types   subtitle (2), description (6), comments (7), keywords (8)
 *                and copyright (0xa01) with a short string succeed.
 *   bad_types    ids between the defined ones (3, 9, 0xa005) and past the
 *                last (0xa00a) fail.
 *   null_info    a defined id with a NULL value fails.
 *   user_meta    user metadata (0xa009) of a whole 176-byte
 *                SceVideoRecordingInfoUserMeta succeeds.
 *
 * Failing calls are only required to return a negative error; the codes are
 * printed for reference.
 */

#define SHADTEST_NAME "hle_video_recording"
#include "shadtest_guest.h"

int32_t sceVideoRecordingSetInfo(int32_t info, const void* value, size_t len);

typedef struct {
    uint64_t size;
    int32_t flags;
    char name[32];
    char data[128];
} UserMeta;

_Static_assert(sizeof(UserMeta) == 176, "SceVideoRecordingInfoUserMeta size");

enum {
    INFO_SUBTITLE = 0x2,
    INFO_DESCRIPTION = 0x6,
    INFO_COMMENTS = 0x7,
    INFO_KEYWORDS = 0x8,
    INFO_COPYRIGHT = 0xa01,
    INFO_USER_META = 0xa009,
    USER_META_CONSTANT = 1,
};

static void start(const StCase* c) { printf("case %s: start\n", c->name); }

static int32_t set_info(int32_t info, const void* value, size_t len) {
    const int32_t rc = sceVideoRecordingSetInfo(info, value, len);
    printf("  info %#x len %zu returned %08x\n", (unsigned)info, len,
           (uint32_t)rc);
    return rc;
}

enum { C_TEXT_TYPES, C_BAD_TYPES, C_NULL_INFO, C_USER_META, C_COUNT };

int main(void) {
    st_begin();

    StCase cases[C_COUNT] = {
        [C_TEXT_TYPES] = {.name = "text_types"},
        [C_BAD_TYPES] = {.name = "bad_types"},
        [C_NULL_INFO] = {.name = "null_info"},
        [C_USER_META] = {.name = "user_meta"},
    };

    static const char text[] = "shadps4-open-test";
    {
        StCase* c = &cases[C_TEXT_TYPES];
        start(c);
        const int32_t ids[] = {INFO_SUBTITLE, INFO_DESCRIPTION, INFO_COMMENTS,
                               INFO_KEYWORDS, INFO_COPYRIGHT};
        for (unsigned k = 0; k < sizeof(ids) / sizeof(ids[0]); ++k) {
            const int32_t rc = set_info(ids[k], text, sizeof(text));
            if (rc < 0) {
                st_case_note(c, "rejected", (uint32_t)ids[k], 0, (uint32_t)rc);
            }
        }
    }
    {
        StCase* c = &cases[C_BAD_TYPES];
        start(c);
        const int32_t ids[] = {0x3, 0x9, 0xa005, 0xa00a};
        for (unsigned k = 0; k < sizeof(ids) / sizeof(ids[0]); ++k) {
            const int32_t rc = set_info(ids[k], text, sizeof(text));
            if (rc >= 0) {
                st_case_note(c, "accepted", (uint32_t)ids[k], 0x80000000u,
                             (uint32_t)rc);
            }
        }
    }
    {
        StCase* c = &cases[C_NULL_INFO];
        start(c);
        const int32_t rc = set_info(INFO_DESCRIPTION, NULL, sizeof(text));
        if (rc >= 0) {
            st_case_note(c, "accepted", 0, 0x80000000u, (uint32_t)rc);
        }
    }
    {
        StCase* c = &cases[C_USER_META];
        start(c);
        UserMeta meta;
        memset(&meta, 0, sizeof(meta));
        meta.size = sizeof(meta);
        meta.flags = USER_META_CONSTANT;
        strcpy(meta.name, "level");
        strcpy(meta.data, "1");
        const int32_t rc = set_info(INFO_USER_META, &meta, sizeof(meta));
        if (rc < 0) {
            st_case_note(c, "rejected", 0, 0, (uint32_t)rc);
        }
    }

    return st_finish_cases(cases, C_COUNT, "");
}
