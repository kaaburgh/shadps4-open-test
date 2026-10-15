/*
 * patch_lookup: whether shadPS4 applied the XML patch named with -p.
 *
 * tests/patch_lookup/run.sh writes a generated "mask" patch that finds
 * g_marker in the executable's memory and replaces its BEFORE with AFTER!,
 * places it under the user's patches directory, and boots this guest with
 * "-p <name>". /app0/expect.txt says whether the patch must have been
 * applied ("patched") or not ("unpatched").
 */

#define SHADTEST_NAME "patch_lookup"
#include "shadtest_guest.h"

// Writable data, so the patch can change it; the text is the patch's search pattern.
static volatile char g_marker[] = "SHADTEST-PATCH-LOOKUP-BEFORE-6f3a9c";

enum { WORD_AT = 22 };

int main(void) {
    st_begin();

    uint8_t* data = NULL;
    size_t size = 0;
    if (!st_read_file("/app0/expect.txt", &data, &size)) {
        return st_fail("reason=no_expect_file");
    }
    const bool want_patched = size >= 7 && memcmp(data, "patched", 7) == 0;
    free(data);

    char word[7] = {0};
    for (int i = 0; i < 6; ++i) {
        word[i] = g_marker[WORD_AT + i];
    }
    const bool patched = memcmp(word, "AFTER!", 6) == 0;
    printf("marker word=%s patched=%d expected=%d\n", word, patched, want_patched);
    if (patched != want_patched) {
        return st_fail("reason=%s word=%s", want_patched ? "not_patched" : "patched", word);
    }
    st_emit_marker("PASS", "patched=%d", patched);
    return 0;
}
