/*
 * hle_language: the console language a title sees through
 * sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG).
 *
 * The expected value comes from /app0/expect.txt ("<case> <language>"),
 * which tests/hle_language/run.sh writes next to the ELF together with the
 * title's sce_sys/param.sfo and the shadPS4 global and per-title settings of
 * each scenario. The test passes if the call succeeds and returns that
 * language.
 */

#define SHADTEST_NAME "hle_language"
#include "shadtest_guest.h"

#include <orbis/SystemService.h>

enum { PARAM_ID_LANG = 1 };

int main(void) {
    st_begin();

    uint8_t* data = NULL;
    size_t size = 0;
    if (!st_read_file("/app0/expect.txt", &data, &size)) {
        return st_fail("reason=no_expect_file");
    }
    char text[96] = "";
    memcpy(text, data, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
    free(data);

    char name[64] = "";
    int expected = -1;
    if (sscanf(text, "%63s %d", name, &expected) != 2) {
        return st_fail("reason=bad_expect_file");
    }

    int32_t lang = -1;
    const int32_t rc = sceSystemServiceParamGetInt(PARAM_ID_LANG, &lang);
    printf("case=%s rc=%08x expected=%d actual=%d\n", name, (uint32_t)rc,
           expected, lang);
    if (rc != 0) {
        return st_fail("case=%s reason=param_get_int rc=%08x", name,
                       (uint32_t)rc);
    }
    if (lang != expected) {
        return st_fail("case=%s reason=language expected=%d actual=%d", name,
                       expected, lang);
    }
    st_emit_marker("PASS", "case=%s language=%d", name, lang);
    return 0;
}
