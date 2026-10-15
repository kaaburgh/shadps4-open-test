/*
 * present_transfer: presents a frame of known sRGB patches through VideoOut,
 * so the host can check shadPS4's presentation transfer curve on the screen.
 *
 * The frame is 1280x720 A8R8G8B8_SRGB, linear tiling, with the 32 patches of
 * tools/patches.py. The guest flips it for PRESENT_SECONDS and then reports
 * PASS; that only says the frame was presented. The pixel check happens on
 * the host: run.sh captures the X display while the frame is up
 * (tools/capture.py) and compares the patch centres with the CPU reference in
 * tools/patches.py, once with the default output and once with the Rec.709
 * curve (GPU.rec709_output).
 */

#define SHADTEST_NAME "present_transfer"
#include "shadtest_guest.h"

#include "patches.h"

int32_t sceVideoOutOpen(int32_t user_id, int32_t bus, int32_t index,
                        const void* param);
int32_t sceVideoOutClose(int32_t handle);
void sceVideoOutSetBufferAttribute(void* attribute, uint32_t pixel_format,
                                   uint32_t tiling, uint32_t aspect,
                                   uint32_t width, uint32_t height,
                                   uint32_t pitch_in_pixels);
int32_t sceVideoOutRegisterBuffers(int32_t handle, int32_t start,
                                   void* const* addresses, int32_t count,
                                   const void* attribute);
int32_t sceVideoOutSubmitFlip(int32_t handle, int32_t index,
                              uint32_t flip_mode, int64_t flip_arg);
int32_t sceVideoOutIsFlipPending(int32_t handle);

enum {
    VIDEO_OUT_USER_SYSTEM = 0xff,
    VIDEO_OUT_BUS_MAIN = 0,
    TILING_LINEAR = 1,
    ASPECT_16_9 = 0,
    FLIP_VSYNC = 1,
    FRAME_BYTES = FRAME_W * FRAME_H * 4,
    ARENA_BYTES = 8 * 1024 * 1024,
    PRESENT_SECONDS = 15,
    FLIP_US = 50 * 1000,
    PENDING_WAIT_MS = 5000,
};

/* Waits until the presenter has taken every submitted flip. */
static bool wait_flips_done(int32_t handle) {
    for (unsigned ms = 0; ms < PENDING_WAIT_MS; ++ms) {
        if (sceVideoOutIsFlipPending(handle) <= 0) {
            return true;
        }
        sceKernelUsleep(1000);
    }
    return false;
}

#define PIXEL_FORMAT_A8R8G8B8_SRGB 0x80000000u

static uint8_t g_attribute[64];

int main(void) {
    st_begin();

    StArena arena;
    if (!st_arena_init(&arena, ARENA_BYTES)) {
        return st_fail("reason=arena");
    }
    uint32_t* frame = st_arena_alloc(&arena, FRAME_BYTES, 64 * 1024);
    if (!frame) {
        return st_fail("reason=frame_alloc");
    }

    /* A8R8G8B8: one little-endian word 0xAARRGGBB per pixel. */
    for (uint32_t y = 0; y < FRAME_H; ++y) {
        for (uint32_t x = 0; x < FRAME_W; ++x) {
            const unsigned patch = (y / PATCH_H) * PATCH_COLUMNS + x / PATCH_W;
            const uint8_t* rgb = patch_rgb[patch];
            frame[y * FRAME_W + x] = 0xff000000u | ((uint32_t)rgb[0] << 16) |
                                     ((uint32_t)rgb[1] << 8) | rgb[2];
        }
    }
    st_store_fence();

    const int32_t handle =
        sceVideoOutOpen(VIDEO_OUT_USER_SYSTEM, VIDEO_OUT_BUS_MAIN, 0, NULL);
    if (handle < 0) {
        return st_fail("reason=video_out_open rc=%08x", (uint32_t)handle);
    }
    sceVideoOutSetBufferAttribute(g_attribute, PIXEL_FORMAT_A8R8G8B8_SRGB,
                                  TILING_LINEAR, ASPECT_16_9, FRAME_W, FRAME_H,
                                  FRAME_W);
    void* buffers[1] = {frame};
    int32_t rc = sceVideoOutRegisterBuffers(handle, 0, buffers, 1, g_attribute);
    if (rc < 0) {
        return st_fail("reason=register_buffers rc=%08x", (uint32_t)rc);
    }

    printf("presenting %ux%u for %u s\n", FRAME_W, FRAME_H, PRESENT_SECONDS);
    unsigned flips = 0;
    for (unsigned t = 0; t < PRESENT_SECONDS * 1000000u / FLIP_US; ++t) {
        if (!wait_flips_done(handle)) {
            return st_fail("reason=flip_stuck flips=%u", flips);
        }
        rc = sceVideoOutSubmitFlip(handle, 0, FLIP_VSYNC, t);
        if (rc < 0) {
            return st_fail("reason=submit_flip rc=%08x flips=%u", (uint32_t)rc,
                           flips);
        }
        ++flips;
        sceKernelUsleep(FLIP_US);
    }
    sceVideoOutClose(handle);

    st_emit_marker("PASS", "flips=%u", flips);
    return 0;
}
