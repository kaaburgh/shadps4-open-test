/*
 * game_update_overlay: which folder's files the guest sees in /app0.
 *
 * The guest reads /app0/overlay.txt and prints its first line, or "none".
 * tests/game_update_overlay/run.sh boots it from base game folders with
 * different names, each with an update folder next to it, and puts a
 * different overlay.txt in each folder.
 */

#define SHADTEST_NAME "game_update_overlay"
#include "shadtest_guest.h"

int main(void) {
    st_begin();
    char text[64] = "none";
    const int32_t fd = sceKernelOpen("/app0/overlay.txt", 0, 0);
    if (fd >= 0) {
        const int64_t n = (int64_t)sceKernelRead(fd, text, sizeof(text) - 1);
        sceKernelClose(fd);
        text[n > 0 ? n : 0] = '\0';
        text[strcspn(text, "\r\n")] = '\0';
    }
    st_emit_marker("PASS", "overlay=%s", text);
    return 0;
}
