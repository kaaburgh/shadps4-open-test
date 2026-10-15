/*
 * hle_audio_queue: a numbered PCM stream through sceAudioOutOutput.
 *
 * The guest opens the main audio port (256 frames, 48 kHz, S16 stereo) and
 * outputs SECONDS of audio in which every frame carries its own number,
 * starting at 1: the left sample holds bits 0-14, the right sample bits
 * 15-29. It then outputs TAIL_MS of silence, so that what is still queued in
 * the host plays out before the port closes.
 *
 * The guest only checks that every call succeeds. tests/hle_audio_queue/run.sh
 * runs it with SDL's disk audio driver, which writes what the device plays to
 * a file, and tools/analyze.py reads the frame numbers back to find what was
 * dropped, repeated or reordered.
 */

#define SHADTEST_NAME "hle_audio_queue"
#include "shadtest_guest.h"

#include <orbis/AudioOut.h>

enum {
    FRAMES = 256,
    RATE = 48000,
    SECONDS = 6,
    TAIL_MS = 500,
    PARAM_S16_STEREO = 1,
    PORT_MAIN = 0,
    SYSTEM_USER = 255,
};

static int16_t g_buf[FRAMES * 2] __attribute__((aligned(32)));

int main(void) {
    st_begin();

    const int32_t init = sceAudioOutInit();
    if (init < 0) {
        return st_fail("reason=init rc=%08x", (uint32_t)init);
    }
    const int32_t port = sceAudioOutOpen(SYSTEM_USER, PORT_MAIN, 0, FRAMES,
                                         RATE, PARAM_S16_STEREO);
    if (port < 0) {
        return st_fail("reason=open rc=%08x", (uint32_t)port);
    }

    const uint32_t buffers = SECONDS * RATE / FRAMES;
    uint32_t frame = 1;
    const uint64_t t0 = sceKernelGetProcessTime();
    for (uint32_t b = 0; b < buffers; ++b) {
        for (uint32_t i = 0; i < FRAMES; ++i, ++frame) {
            g_buf[2 * i] = (int16_t)(frame & 0x7FFF);
            g_buf[2 * i + 1] = (int16_t)((frame >> 15) & 0x7FFF);
        }
        const int32_t rc = sceAudioOutOutput(port, g_buf);
        if (rc < 0) {
            return st_fail("reason=output buffer=%u rc=%08x", b, (uint32_t)rc);
        }
    }
    const uint64_t elapsed = sceKernelGetProcessTime() - t0;

    memset(g_buf, 0, sizeof(g_buf));
    for (uint32_t b = 0; b < TAIL_MS * RATE / 1000 / FRAMES; ++b) {
        sceAudioOutOutput(port, g_buf);
    }
    sceAudioOutClose(port);

    st_emit_marker("PASS", "frames=%u elapsed_us=%llu", frame - 1,
                   (unsigned long long)elapsed);
    return 0;
}
