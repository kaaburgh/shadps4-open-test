/*
 * hle_avplayer_frame: the first video frame AvPlayer hands the guest,
 * compared byte for byte with the picture that was encoded.
 *
 * tests/hle_avplayer_frame/run.sh encodes /app0/video.mp4 losslessly from a
 * synthetic YUV 4:2:0 pattern (tools/make_video.py) and writes its size to
 * /app0/video.txt. The guest plays it with sceAvPlayerInit/AddSource, takes
 * the first frame from sceAvPlayerGetVideoDataEx and checks, for the
 * picture's W x H pixels:
 *   - luma at p_data + y * pitch + x;
 *   - interleaved U/V at p_data + pitch * height + cy * pitch + 2 * cx, with
 *     height as the frame info reports it;
 *   - that rows below the picture, up to the reported height, carry no
 *     U = V = 0 chroma (shown as a green line).
 * It prints the frame info so run.sh can record pitch, size and crop.
 */

#define SHADTEST_NAME "hle_avplayer_frame"
#include "shadtest_guest.h"

/* AvPlayer ABI as shadPS4 declares it (src/core/libraries/avplayer/avplayer.h). */
typedef struct {
    void* object_ptr;
    void* (*allocate)(void* p, uint32_t align, uint32_t size);
    void (*deallocate)(void* p, void* mem);
    void* (*allocate_texture)(void* p, uint32_t align, uint32_t size);
    void (*deallocate_texture)(void* p, void* mem);
} AvMemAllocator;

typedef struct {
    void* object_ptr;
    void* open;
    void* close;
    void* read_offset;
    void* size;
} AvFileReplacement;

typedef struct {
    void* object_ptr;
    void* event_callback;
} AvEventReplacement;

typedef struct {
    AvMemAllocator memory_replacement;
    AvFileReplacement file_replacement;
    AvEventReplacement event_replacement;
    int32_t debug_level;
    uint32_t base_priority;
    int32_t num_output_video_framebuffers;
    bool auto_start;
    uint8_t reserved[3];
    const char* default_language;
} AvInitData;

typedef struct {
    uint32_t width;
    uint32_t height;
    float aspect_ratio;
    uint8_t language_code[4];
    uint32_t framerate;
    uint32_t crop_left_offset;
    uint32_t crop_right_offset;
    uint32_t crop_top_offset;
    uint32_t crop_bottom_offset;
    uint32_t pitch;
    uint8_t luma_bit_depth;
    uint8_t chroma_bit_depth;
    bool video_full_range_flag;
    uint8_t reserved1[37];
} AvVideoEx;

typedef struct {
    void* p_data;
    uint8_t reserved[4];
    uint64_t timestamp;
    union {
        AvVideoEx video;
        uint8_t reserved1[80];
    } details;
} AvFrameInfoEx;

_Static_assert(sizeof(AvVideoEx) == 80, "AvPlayerVideoEx size");
_Static_assert(sizeof(AvFrameInfoEx) == 104, "AvPlayerFrameInfoEx size");

enum { SYSMODULE_AV_PLAYER = 0x00a5 };
int32_t sceSysmoduleLoadModule(uint16_t id);

void* av_init(AvInitData* data) __asm__("sceAvPlayerInit");
int32_t av_add_source(void* handle, const char* path) __asm__("sceAvPlayerAddSource");
bool av_get_video_ex(void* handle, AvFrameInfoEx* info) __asm__("sceAvPlayerGetVideoDataEx");
int32_t av_close(void* handle) __asm__("sceAvPlayerClose");

/* Same pattern as tools/make_video.py, all in video range 16..239. */
static uint8_t pattern_y(uint32_t x, uint32_t y) {
    return (uint8_t)(16 + (x * 7 + y * 13) % 220);
}
static uint8_t pattern_u(uint32_t cx, uint32_t cy) {
    return (uint8_t)(16 + (cx * 5 + cy * 3 + 17) % 224);
}
static uint8_t pattern_v(uint32_t cx, uint32_t cy) {
    return (uint8_t)(16 + (cx * 11 + cy * 19 + 101) % 224);
}

static void* av_alloc(void* p, uint32_t align, uint32_t size) {
    (void)p;
    const size_t alignment = align < 64 ? 64 : align;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
}
static void av_free(void* p, void* mem) {
    (void)p;
    free(mem);
}

static bool read_size(uint32_t* w, uint32_t* h) {
    char text[32] = {0};
    const int32_t fd = sceKernelOpen("/app0/video.txt", 0, 0);
    if (fd < 0) {
        return false;
    }
    sceKernelRead(fd, text, sizeof(text) - 1);
    sceKernelClose(fd);
    return sscanf(text, "%u %u", w, h) == 2;
}

int main(void) {
    st_begin();
    uint32_t w = 0, h = 0;
    if (!read_size(&w, &h)) {
        return st_fail("reason=no_video_txt");
    }

    const int32_t load_rc = sceSysmoduleLoadModule(SYSMODULE_AV_PLAYER);
    if (load_rc != 0) {
        return st_fail("reason=load_module rc=%08x", (uint32_t)load_rc);
    }

    AvInitData init;
    memset(&init, 0, sizeof(init));
    init.memory_replacement.allocate = av_alloc;
    init.memory_replacement.deallocate = av_free;
    init.memory_replacement.allocate_texture = av_alloc;
    init.memory_replacement.deallocate_texture = av_free;
    init.num_output_video_framebuffers = 2;
    init.auto_start = true;
    init.default_language = "eng";
    void* player = av_init(&init);
    if (!player) {
        return st_fail("reason=init");
    }
    const int32_t rc = av_add_source(player, "/app0/video.mp4");
    if (rc != 0) {
        return st_fail("reason=add_source rc=%08x", (uint32_t)rc);
    }

    AvFrameInfoEx info;
    memset(&info, 0, sizeof(info));
    bool got = false;
    for (int i = 0; i < 1000 && !got; ++i) {
        got = av_get_video_ex(player, &info);
        if (!got) {
            sceKernelUsleep(10000);
        }
    }
    if (!got || !info.p_data) {
        return st_fail("reason=no_frame");
    }
    const AvVideoEx* v = &info.details.video;
    printf("video %ux%u: info width=%u height=%u pitch=%u crop l=%u r=%u t=%u b=%u\n", w, h,
           v->width, v->height, v->pitch, v->crop_left_offset, v->crop_right_offset,
           v->crop_top_offset, v->crop_bottom_offset);
    if (v->pitch < w || v->height < h) {
        return st_fail("reason=frame_too_small pitch=%u height=%u", v->pitch, v->height);
    }

    const uint8_t* luma = (const uint8_t*)info.p_data;
    const uint8_t* chroma = luma + (size_t)v->pitch * v->height;
    uint32_t luma_bad = 0, chroma_bad = 0, green = 0;
    uint32_t first_x = 0, first_y = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            if (luma[(size_t)y * v->pitch + x] != pattern_y(x, y) && luma_bad++ == 0) {
                first_x = x;
                first_y = y;
            }
        }
    }
    for (uint32_t cy = 0; cy < h / 2; ++cy) {
        for (uint32_t cx = 0; cx < w / 2; ++cx) {
            const uint8_t* uv = chroma + (size_t)cy * v->pitch + 2 * cx;
            if ((uv[0] != pattern_u(cx, cy) || uv[1] != pattern_v(cx, cy)) &&
                chroma_bad++ == 0 && luma_bad == 0) {
                first_x = cx;
                first_y = cy;
            }
        }
    }
    for (uint32_t cy = h / 2; cy < v->height / 2; ++cy) {
        for (uint32_t cx = 0; cx < w / 2; ++cx) {
            const uint8_t* uv = chroma + (size_t)cy * v->pitch + 2 * cx;
            green += uv[0] == 0 && uv[1] == 0;
        }
    }
    av_close(player);

    if (luma_bad || chroma_bad || green) {
        return st_fail("reason=pixels luma_bad=%u chroma_bad=%u green=%u first=%u,%u pitch=%u "
                       "height=%u",
                       luma_bad, chroma_bad, green, first_x, first_y, v->pitch, v->height);
    }
    st_emit_marker("PASS", "size=%ux%u width=%u height=%u pitch=%u crop=%u,%u,%u,%u", w, h,
                   v->width, v->height, v->pitch, v->crop_left_offset, v->crop_right_offset,
                   v->crop_top_offset, v->crop_bottom_offset);
    return 0;
}
