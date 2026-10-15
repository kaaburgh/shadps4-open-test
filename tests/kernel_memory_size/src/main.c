/*
 * kernel_memory_size: the direct and flexible memory sizes shadPS4 reports,
 * and whether the guest can actually use all of them.
 *
 * The guest reads sceKernelGetDirectMemorySize, sceKernelIsNeoMode and
 * sceKernelAvailableFlexibleMemorySize, then:
 *   - allocates, maps, writes and reads the last 2 MiB of direct memory;
 *   - maps flexible memory in 16 MiB chunks, touching each one, until less
 *     than a chunk is reported available; every map must succeed, the
 *     reported size must shrink by the size mapped, and one more chunk must be
 *     refused;
 *   - unmaps everything and expects the starting sizes back.
 * It prints the sizes in the PASS marker; tests/kernel_memory_size/run.sh
 * compares them across shadPS4 settings.
 */

#define SHADTEST_NAME "kernel_memory_size"
#include "shadtest_guest.h"

// OpenOrbis declares it without the output pointer.
int32_t available_flexible(size_t* size) __asm__("sceKernelAvailableFlexibleMemorySize");

// Kernel memory parameters as a title declares them, layout as in shadPS4's
// OrbisKernelMemParam. OpenOrbis's crt1 points the process parameters at its
// own block, with no flexible memory size. tools/set_flexible_size.py makes a
// copy of this executable that points them here instead, with the size set.
struct shadtest_mem_param {
    uint64_t size;
    uint64_t* extended_page_table;
    uint64_t* flexible_memory_size;
    uint8_t* extended_memory_1;
    uint64_t* extended_gpu_page_table;
    uint8_t* extended_memory_2;
    uint64_t* extended_cpu_page_table;
};
// In .data, not .bss, so the size has bytes in the file to patch.
__attribute__((used, section(".data"))) uint64_t shadtest_flexible_size = 0;
__attribute__((used)) struct shadtest_mem_param shadtest_mem_param = {
    .size = sizeof(struct shadtest_mem_param),
    .flexible_memory_size = &shadtest_flexible_size,
};

enum {
    CHUNK = 16 * 1024 * 1024,
    TOP_BYTES = 2 * 1024 * 1024,
    MAX_CHUNKS = 512,
};

static void* g_chunks[MAX_CHUNKS];

static int check_direct_top(size_t direct) {
    off_t phys = -1;
    int32_t rc = sceKernelAllocateDirectMemory((off_t)(direct - TOP_BYTES), (off_t)direct,
                                               TOP_BYTES, TOP_BYTES, ORBIS_KERNEL_WC_GARLIC,
                                               &phys);
    if (rc != 0) {
        return st_fail("reason=direct_top_alloc rc=%08x direct=0x%zx", (uint32_t)rc, direct);
    }
    if ((size_t)phys != direct - TOP_BYTES) {
        sceKernelReleaseDirectMemory(phys, TOP_BYTES);
        return st_fail("reason=direct_top_addr phys=0x%llx direct=0x%zx",
                       (unsigned long long)phys, direct);
    }
    void* mapped = NULL;
    rc = sceKernelMapDirectMemory(&mapped, TOP_BYTES,
                                  ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW, 0, phys,
                                  TOP_BYTES);
    if (rc != 0) {
        sceKernelReleaseDirectMemory(phys, TOP_BYTES);
        return st_fail("reason=direct_top_map rc=%08x", (uint32_t)rc);
    }
    volatile uint64_t* words = (volatile uint64_t*)mapped;
    words[0] = 0x5348414454455354ull;
    words[TOP_BYTES / 8 - 1] = direct;
    const bool ok = words[0] == 0x5348414454455354ull && words[TOP_BYTES / 8 - 1] == direct;
    sceKernelMunmap(mapped, TOP_BYTES);
    sceKernelReleaseDirectMemory(phys, TOP_BYTES);
    return ok ? 0 : st_fail("reason=direct_top_readback");
}

static int check_flexible(size_t* out_mapped) {
    size_t start = 0;
    if (available_flexible(&start) != 0) {
        return st_fail("reason=flexible_available_query");
    }
    unsigned count = 0;
    size_t available = start;
    while (available >= CHUNK) {
        if (count == MAX_CHUNKS) {
            return st_fail("reason=too_many_chunks available=0x%zx", available);
        }
        void* addr = NULL;
        const int32_t rc = sceKernelMapFlexibleMemory(
            &addr, CHUNK, ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW, 0);
        if (rc != 0) {
            return st_fail("reason=flexible_map rc=%08x chunk=%u mapped=0x%zx available=0x%zx",
                           (uint32_t)rc, count, (size_t)count * CHUNK, available);
        }
        volatile uint32_t* first = (volatile uint32_t*)addr;
        volatile uint32_t* last = (volatile uint32_t*)((uint8_t*)addr + CHUNK) - 1;
        *first = count;
        *last = ~count;
        if (*first != count || *last != ~count) {
            return st_fail("reason=flexible_readback chunk=%u", count);
        }
        g_chunks[count++] = addr;
        size_t now = 0;
        available_flexible(&now);
        if (now != start - (size_t)count * CHUNK) {
            return st_fail("reason=flexible_accounting chunk=%u start=0x%zx now=0x%zx", count,
                           start, now);
        }
        available = now;
    }
    printf("mapped %u flexible chunks, 0x%zx left\n", count, available);

    void* extra = NULL;
    const int32_t extra_rc = sceKernelMapFlexibleMemory(
        &extra, CHUNK, ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW, 0);
    if (extra_rc == 0) {
        return st_fail("reason=flexible_over_limit_accepted available=0x%zx", available);
    }

    for (unsigned i = 0; i < count; ++i) {
        if (*(volatile uint32_t*)g_chunks[i] != i) {
            return st_fail("reason=flexible_chunk_changed chunk=%u", i);
        }
        sceKernelMunmap(g_chunks[i], CHUNK);
    }
    size_t end = 0;
    available_flexible(&end);
    if (end != start) {
        return st_fail("reason=flexible_not_returned start=0x%zx end=0x%zx", start, end);
    }
    *out_mapped = (size_t)count * CHUNK;
    return 0;
}

int main(void) {
    st_begin();
    const size_t direct = sceKernelGetDirectMemorySize();
    const int neo = sceKernelIsNeoMode();
    size_t available = 0;
    if (available_flexible(&available) != 0) {
        return st_fail("reason=flexible_size_query");
    }
    printf("direct=0x%zx neo=%d flexible available=0x%zx\n", direct, neo, available);
    if (direct < TOP_BYTES || direct % TOP_BYTES != 0) {
        return st_fail("reason=direct_size direct=0x%zx", direct);
    }

    int rc = check_direct_top(direct);
    if (rc != 0) {
        return rc;
    }
    size_t mapped = 0;
    rc = check_flexible(&mapped);
    if (rc != 0) {
        return rc;
    }
    if (sceKernelGetDirectMemorySize() != direct) {
        return st_fail("reason=direct_size_changed");
    }

    st_emit_marker("PASS", "direct=0x%zx neo=%d flexible=0x%zx flexible_mapped=0x%zx", direct,
                   neo, available, mapped);
    return 0;
}
