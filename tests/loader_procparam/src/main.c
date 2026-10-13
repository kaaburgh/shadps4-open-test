/*
 * loader_procparam: the SDK version shadPS4 reads from the executable's
 * PT_SCE_PROCPARAM segment, as the guest sees it.
 *
 * The guest prints sceKernelGetCompiledSdkVersion and passes if the call
 * succeeds. tests/loader_procparam/run.sh boots copies of this executable
 * whose PT_SCE_PROCPARAM program header declares other segment sizes and
 * compares the printed versions.
 */

#define SHADTEST_NAME "loader_procparam"
#include "shadtest_guest.h"

// OpenOrbis declares it without parameters.
int32_t get_compiled_sdk_version(int32_t* version) __asm__("sceKernelGetCompiledSdkVersion");

int main(void) {
    st_begin();
    int32_t version = -1;
    const int32_t rc = get_compiled_sdk_version(&version);
    if (rc != 0) {
        return st_fail("reason=get_sdk_version rc=%08x", (uint32_t)rc);
    }
    st_emit_marker("PASS", "sdk_version=0x%08x", (uint32_t)version);
    return 0;
}
