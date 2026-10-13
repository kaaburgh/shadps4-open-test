/*
 * hle_png: libScePngDec / libScePngEnc robustness on valid, truncated and
 * undersized inputs.
 *
 * Fixtures (src/fixtures.h, from tools/gen_fixtures.py): a 5x4 pixel pattern
 * as 8-bit RGBA, 8-bit RGB and Adam7-interlaced RGBA PNGs.
 *
 * Every case creates its own decoder and encoder, except the *_repeat and
 * recover cases, which test reuse of one handle.
 *
 * Decoder cases:
 *   parse_header      scePngDecParseHeader reports 5x4 RGBA, 8 bits.
 *   decode_rgba       tight rows (pitch 0): pixels match, the bytes after the
 *                     image are untouched.
 *   decode_pitch      pitch 32: pixels match, row padding is untouched.
 *   decode_rgb        RGB gets the caller's alpha value.
 *   decode_adam7      interlaced decodes to the same pixels.
 *   short_output      imageMemSize one byte short, and a pitch below the row
 *                     size: the call fails and writes nothing.
 *   parse_truncated   scePngDecParseHeader on PNGs cut inside the signature
 *                     and inside IHDR fails.
 *   truncated         scePngDecDecode on PNGs cut inside the signature, IHDR
 *                     or IDAT, or missing only IEND, fails and writes nothing
 *                     past imageMemSize.
 *   decode_repeat     two more decodes on one handle give the same pixels.
 *   corrupt           a PNG whose IHDR CRC is wrong: the call fails.
 *   recover           on one handle, the corrupt PNG fails and the valid one
 *                     then decodes.
 *   delete            scePngDecDelete succeeds; a new decoder works.
 *
 * Encoder cases (each output is decoded back with libScePngDec):
 *   encode_rgba       tight RGBA rows (pitch 20) round-trip.
 *   encode_pitch      RGBA rows with pitch 32 round-trip.
 *   encode_rgb        RGB output from RGBA rows with pitch 32 round-trips.
 *   encode_short_src  imageMemSize one byte short of the last row: the call
 *                     fails.
 *   encode_short_out  pngMemSize 40: the call fails and writes nothing past
 *                     40 bytes.
 *   encode_repeat     three encodes on one handle all round-trip.
 *
 * The encoder cases always pass the pitch: what pitch 0 means to the encoder
 * is not established.
 *
 * Failing calls are only required to return a negative error: the exact PS4
 * error codes are not established, so the log prints them for reference.
 *
 * A case that crashes the emulator ends the run without a marker; the log's
 * last "case <name>: start" line names it. Cases that reuse a handle or feed
 * libpng a broken stream run last for that reason.
 */

#define SHADTEST_NAME "hle_png"
#include "shadtest_guest.h"

#include "fixtures.h"

/* libSceSysmodule ids. */
enum {
    SYSMODULE_PNG_DEC = 0x008c,
    SYSMODULE_PNG_ENC = 0x008d,
};

int32_t sceSysmoduleLoadModule(uint16_t id);

/* libScePngDec. */
typedef struct {
    uint32_t this_size;
    uint32_t attribute;
    uint32_t max_image_width;
} PngDecCreateParam;

typedef struct {
    const void* png_mem_addr;
    void* image_mem_addr;
    uint32_t png_mem_size;
    uint32_t image_mem_size;
    uint16_t pixel_format; /* 0: R8G8B8A8 */
    uint16_t alpha_value;
    uint32_t image_pitch;
} PngDecDecodeParam;

typedef struct {
    const void* png_mem_addr;
    uint32_t png_mem_size;
    uint32_t reserved;
} PngDecParseParam;

typedef struct {
    uint32_t image_width;
    uint32_t image_height;
    uint16_t color_space; /* 19: RGBA */
    uint16_t bit_depth;
    uint32_t image_flag;
} PngDecImageInfo;

int32_t scePngDecQueryMemorySize(const PngDecCreateParam* param);
int32_t scePngDecCreate(const PngDecCreateParam* param, void* memory,
                        uint32_t memory_size, void** handle);
int32_t scePngDecDecode(void* handle, const PngDecDecodeParam* param,
                        PngDecImageInfo* info);
int32_t scePngDecParseHeader(const PngDecParseParam* param,
                             PngDecImageInfo* info);
int32_t scePngDecDelete(void* handle);

/* libScePngEnc. */
typedef struct {
    uint32_t this_size;
    uint32_t attribute;
    uint32_t max_image_width;
    uint32_t max_filter_number;
} PngEncCreateParam;

typedef struct {
    const void* image_mem_addr;
    void* png_mem_addr;
    uint32_t image_mem_size;
    uint32_t png_mem_size;
    uint32_t image_width;
    uint32_t image_height;
    uint32_t image_pitch;
    uint16_t pixel_format; /* 0: R8G8B8A8 */
    uint16_t color_space;  /* 3: RGB, 19: RGBA */
    uint16_t bit_depth;
    uint16_t clut_number;
    uint16_t filter_type;
    uint16_t compression_level;
} PngEncEncodeParam;

typedef struct {
    uint32_t data_size;
    uint32_t processed_height;
} PngEncOutputInfo;

int32_t scePngEncQueryMemorySize(const PngEncCreateParam* param);
int32_t scePngEncCreate(const PngEncCreateParam* param, void* memory,
                        uint32_t memory_size, void** handle);
int32_t scePngEncEncode(void* handle, const PngEncEncodeParam* param,
                        PngEncOutputInfo* info);
int32_t scePngEncDelete(void* handle);

enum {
    W = FIXTURE_WIDTH,
    H = FIXTURE_HEIGHT,
    ROW_BYTES = W * 4,
    IMAGE_BYTES = ROW_BYTES * H,
    PADDED_PITCH = 32,
    PADDED_BYTES = PADDED_PITCH * (H - 1) + ROW_BYTES,
    /* Output buffers get this much canary space behind the region a call
     * may write. */
    CANARY_BYTES = 64,
    CANARY = 0xa5,
    COLOR_SPACE_RGBA = 19,
    ENC_RGB = 3,
    ENC_RGBA = 19,
    WORK_BYTES = 4096,
    PNG_OUT_BYTES = 4096,
    ALPHA_FOR_RGB = 0x7f,
};

static uint8_t g_out[PADDED_BYTES + CANARY_BYTES];
static uint8_t g_dec_work[WORK_BYTES];
static uint8_t g_dec_work2[WORK_BYTES];
static uint8_t g_enc_work[WORK_BYTES];
static uint8_t g_png[PNG_OUT_BYTES];
static uint8_t g_src[PADDED_BYTES];

static void start(const StCase* c) { printf("case %s: start\n", c->name); }

static void fill_canary(void* p, size_t n) { memset(p, CANARY, n); }

/* Index of the first byte of [p, p+n) that is not the canary, or -1. */
static long first_non_canary(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != CANARY) {
            return (long)i;
        }
    }
    return -1;
}

static void expect_ok(StCase* c, const char* what, int32_t rc) {
    if (rc < 0) {
        printf("  %s returned %08x\n", what, (uint32_t)rc);
        st_case_note(c, what, 0, 0, (uint32_t)rc);
    }
}

static void expect_error(StCase* c, const char* what, int32_t rc) {
    printf("  %s returned %08x\n", what, (uint32_t)rc);
    if (rc >= 0) {
        st_case_note(c, what, 0, 0x80000000u, (uint32_t)rc);
    }
}

static void expect_canary(StCase* c, const char* what, const uint8_t* p,
                          size_t n) {
    const long bad = first_non_canary(p, n);
    if (bad >= 0) {
        st_case_note(c, what, (uint32_t)bad, CANARY, p[bad]);
    }
}

/* Compares decoded rows at `pitch` with the fixture pixels; `alpha` < 0 keeps
 * the fixture's alpha. Padding between rows must keep the canary. */
static void expect_pixels(StCase* c, const uint8_t* out, uint32_t pitch,
                          int alpha) {
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t i = 0; i < ROW_BYTES; ++i) {
            uint8_t want = pixels_rgba[y * ROW_BYTES + i];
            if (alpha >= 0 && i % 4 == 3) {
                want = (uint8_t)alpha;
            }
            st_case_expect(c, "pixel", y * ROW_BYTES + i, want,
                           out[y * pitch + i]);
        }
        if (y + 1 < H) {
            expect_canary(c, "row_padding", out + y * pitch + ROW_BYTES,
                          pitch - ROW_BYTES);
        }
    }
}

static int32_t create_decoder(uint8_t* work, void** handle) {
    PngDecCreateParam cp = {sizeof(cp), 0, 1024};
    const int32_t need = scePngDecQueryMemorySize(&cp);
    if (need < 0 || need > WORK_BYTES) {
        printf("scePngDecQueryMemorySize returned %08x\n", (uint32_t)need);
        return need < 0 ? need : -1;
    }
    return scePngDecCreate(&cp, work, (uint32_t)need, handle);
}

static int32_t decode(void* handle, const void* png, uint32_t png_size,
                      uint32_t image_size, uint32_t pitch, uint16_t alpha,
                      PngDecImageInfo* info) {
    PngDecDecodeParam dp = {
        .png_mem_addr = png,
        .image_mem_addr = g_out,
        .png_mem_size = png_size,
        .image_mem_size = image_size,
        .pixel_format = 0,
        .alpha_value = alpha,
        .image_pitch = pitch,
    };
    return scePngDecDecode(handle, &dp, info);
}

/* Decodes `png` tightly into g_out and checks it against the fixture. */
static void expect_round_trip(StCase* c, void* dec, const void* png,
                              int32_t png_size, int alpha) {
    if (png_size <= 0) {
        st_case_note(c, "encode", 0, 1, (uint32_t)png_size);
        return;
    }
    fill_canary(g_out, sizeof(g_out));
    const int32_t rc =
        decode(dec, png, (uint32_t)png_size, IMAGE_BYTES, 0, 0, NULL);
    expect_ok(c, "decode_output", rc);
    if (rc >= 0) {
        expect_pixels(c, g_out, ROW_BYTES, alpha);
    }
}

static int32_t encode(void* enc, uint32_t pitch, uint32_t image_size,
                      uint16_t color_space, uint32_t png_size) {
    PngEncEncodeParam ep = {
        .image_mem_addr = g_src,
        .png_mem_addr = g_png,
        .image_mem_size = image_size,
        .png_mem_size = png_size,
        .image_width = W,
        .image_height = H,
        .image_pitch = pitch,
        .pixel_format = 0,
        .color_space = color_space,
        .bit_depth = 8,
        .clut_number = 0,
        .filter_type = 0,
        .compression_level = 6,
    };
    PngEncOutputInfo info = {0, 0};
    return scePngEncEncode(enc, &ep, &info);
}

/* Lays the fixture pixels out in g_src with `pitch`. */
static void fill_source(uint32_t pitch) {
    memset(g_src, 0xee, sizeof(g_src));
    for (uint32_t y = 0; y < H; ++y) {
        memcpy(g_src + y * pitch, pixels_rgba + y * ROW_BYTES, ROW_BYTES);
    }
}

/* A fresh decoder or encoder in the one work buffer each has. */
static void* new_decoder(void) {
    void* dec = NULL;
    const int32_t rc = create_decoder(g_dec_work, &dec);
    if (rc < 0) {
        printf("decoder create returned %08x\n", (uint32_t)rc);
        return NULL;
    }
    return dec;
}

static void* new_encoder(void) {
    PngEncCreateParam cp = {sizeof(cp), 0, 1024, 5};
    const int32_t need = scePngEncQueryMemorySize(&cp);
    if (need < 0 || need > WORK_BYTES) {
        printf("scePngEncQueryMemorySize returned %08x\n", (uint32_t)need);
        return NULL;
    }
    void* enc = NULL;
    const int32_t rc = scePngEncCreate(&cp, g_enc_work, (uint32_t)need, &enc);
    if (rc < 0) {
        printf("encoder create returned %08x\n", (uint32_t)rc);
        return NULL;
    }
    return enc;
}

/* Encodes the fixture with `pitch` and decodes the output with a fresh
 * decoder. */
static void encode_round_trip(StCase* c, void* enc, uint32_t pitch,
                              uint16_t color_space) {
    fill_source(pitch);
    const int32_t rc = encode(enc, pitch, pitch * (H - 1) + ROW_BYTES,
                              color_space, sizeof(g_png));
    printf("  encode returned %d\n", rc);
    /* An RGB PNG decodes with alpha_value 0. */
    expect_round_trip(c, new_decoder(), g_png, rc,
                      color_space == ENC_RGB ? 0 : -1);
}

/* The IHDR CRC sits at bytes 29..32 of every fixture. */
static uint8_t g_corrupt[sizeof(png_rgba)];

enum {
    C_PARSE,
    C_RGBA,
    C_PITCH,
    C_RGB,
    C_ADAM7,
    C_SHORT_OUTPUT,
    C_PARSE_TRUNCATED,
    C_TRUNCATED,
    C_ENC_RGBA,
    C_ENC_PITCH,
    C_ENC_RGB,
    C_ENC_SHORT_SRC,
    C_ENC_SHORT_OUT,
    C_REPEAT,
    C_ENC_REPEAT,
    C_CORRUPT,
    C_RECOVER,
    C_DELETE,
    C_COUNT,
};

int main(void) {
    st_begin();

    StCase cases[C_COUNT] = {
        [C_PARSE] = {.name = "parse_header"},
        [C_RGBA] = {.name = "decode_rgba"},
        [C_PITCH] = {.name = "decode_pitch"},
        [C_RGB] = {.name = "decode_rgb"},
        [C_ADAM7] = {.name = "decode_adam7"},
        [C_SHORT_OUTPUT] = {.name = "short_output"},
        [C_PARSE_TRUNCATED] = {.name = "parse_truncated"},
        [C_TRUNCATED] = {.name = "truncated"},
        [C_ENC_RGBA] = {.name = "encode_rgba"},
        [C_ENC_PITCH] = {.name = "encode_pitch"},
        [C_ENC_RGB] = {.name = "encode_rgb"},
        [C_ENC_SHORT_SRC] = {.name = "encode_short_src"},
        [C_ENC_SHORT_OUT] = {.name = "encode_short_out"},
        [C_REPEAT] = {.name = "decode_repeat"},
        [C_ENC_REPEAT] = {.name = "encode_repeat"},
        [C_CORRUPT] = {.name = "corrupt"},
        [C_RECOVER] = {.name = "recover"},
        [C_DELETE] = {.name = "delete"},
    };

    int32_t rc = sceSysmoduleLoadModule(SYSMODULE_PNG_DEC);
    if (rc < 0) {
        return st_fail("reason=load_png_dec rc=%08x", (uint32_t)rc);
    }
    rc = sceSysmoduleLoadModule(SYSMODULE_PNG_ENC);
    if (rc < 0) {
        return st_fail("reason=load_png_enc rc=%08x", (uint32_t)rc);
    }
    if (!new_decoder() || !new_encoder()) {
        return st_fail("reason=create");
    }

    memcpy(g_corrupt, png_rgba, sizeof(png_rgba));
    g_corrupt[29] ^= 0xff;

    {
        StCase* c = &cases[C_PARSE];
        start(c);
        PngDecParseParam pp = {png_rgba, sizeof(png_rgba), 0};
        PngDecImageInfo info;
        memset(&info, 0, sizeof(info));
        expect_ok(c, "parse", scePngDecParseHeader(&pp, &info));
        st_case_expect(c, "width", 0, W, info.image_width);
        st_case_expect(c, "height", 0, H, info.image_height);
        st_case_expect(c, "color_space", 0, COLOR_SPACE_RGBA, info.color_space);
        st_case_expect(c, "bit_depth", 0, 8, info.bit_depth);
        st_case_expect(c, "image_flag", 0, 0, info.image_flag);
    }
    {
        StCase* c = &cases[C_RGBA];
        start(c);
        fill_canary(g_out, sizeof(g_out));
        PngDecImageInfo info;
        memset(&info, 0, sizeof(info));
        rc = decode(new_decoder(), png_rgba, sizeof(png_rgba), IMAGE_BYTES, 0,
                    0, &info);
        expect_ok(c, "decode", rc);
        if (rc >= 0) {
            st_case_expect(c, "return", 0, (W << 16) | H, (uint32_t)rc);
            st_case_expect(c, "width", 0, W, info.image_width);
            expect_pixels(c, g_out, ROW_BYTES, -1);
            expect_canary(c, "after_image", g_out + IMAGE_BYTES,
                          sizeof(g_out) - IMAGE_BYTES);
        }
    }
    {
        StCase* c = &cases[C_PITCH];
        start(c);
        fill_canary(g_out, sizeof(g_out));
        rc = decode(new_decoder(), png_rgba, sizeof(png_rgba), PADDED_BYTES,
                    PADDED_PITCH, 0, NULL);
        expect_ok(c, "decode", rc);
        if (rc >= 0) {
            expect_pixels(c, g_out, PADDED_PITCH, -1);
            expect_canary(c, "after_image", g_out + PADDED_BYTES,
                          sizeof(g_out) - PADDED_BYTES);
        }
    }
    {
        StCase* c = &cases[C_RGB];
        start(c);
        fill_canary(g_out, sizeof(g_out));
        rc = decode(new_decoder(), png_rgb, sizeof(png_rgb), IMAGE_BYTES, 0,
                    ALPHA_FOR_RGB, NULL);
        expect_ok(c, "decode", rc);
        if (rc >= 0) {
            expect_pixels(c, g_out, ROW_BYTES, ALPHA_FOR_RGB);
        }
    }
    {
        StCase* c = &cases[C_ADAM7];
        start(c);
        fill_canary(g_out, sizeof(g_out));
        PngDecImageInfo info;
        memset(&info, 0, sizeof(info));
        rc = decode(new_decoder(), png_rgba_adam7, sizeof(png_rgba_adam7),
                    IMAGE_BYTES, 0, 0, &info);
        expect_ok(c, "decode", rc);
        if (rc >= 0) {
            st_case_expect(c, "image_flag", 0, 1, info.image_flag);
            expect_pixels(c, g_out, ROW_BYTES, -1);
        }
    }
    {
        StCase* c = &cases[C_SHORT_OUTPUT];
        start(c);
        fill_canary(g_out, sizeof(g_out));
        rc = decode(new_decoder(), png_rgba, sizeof(png_rgba), IMAGE_BYTES - 1,
                    0, 0, NULL);
        expect_error(c, "decode_size_short", rc);
        expect_canary(c, "written_size_short", g_out, sizeof(g_out));
        fill_canary(g_out, sizeof(g_out));
        rc = decode(new_decoder(), png_rgba, sizeof(png_rgba), sizeof(g_out),
                    ROW_BYTES - 4, 0, NULL);
        expect_error(c, "decode_pitch_short", rc);
        expect_canary(c, "written_pitch_short", g_out, sizeof(g_out));
    }

    /* Cuts: inside the signature, inside IHDR, inside IDAT, and the whole
     * 12-byte IEND chunk after complete image data. */
    const uint32_t cuts[] = {7, 20, sizeof(png_rgba) - 30,
                             sizeof(png_rgba) - 12};
    {
        StCase* c = &cases[C_PARSE_TRUNCATED];
        start(c);
        for (unsigned k = 0; k < 2; ++k) {
            PngDecParseParam pp = {png_rgba, cuts[k], 0};
            PngDecImageInfo info;
            memset(&info, 0, sizeof(info));
            expect_error(c, "parse", scePngDecParseHeader(&pp, &info));
        }
    }
    {
        StCase* c = &cases[C_TRUNCATED];
        start(c);
        for (unsigned k = 0; k < sizeof(cuts) / sizeof(cuts[0]); ++k) {
            fill_canary(g_out, sizeof(g_out));
            rc = decode(new_decoder(), png_rgba, cuts[k], IMAGE_BYTES, 0, 0,
                        NULL);
            expect_error(c, "decode", rc);
            expect_canary(c, "written_past_size", g_out + IMAGE_BYTES,
                          sizeof(g_out) - IMAGE_BYTES);
        }
    }
    {
        StCase* c = &cases[C_ENC_RGBA];
        start(c);
        encode_round_trip(c, new_encoder(), ROW_BYTES, ENC_RGBA);
    }
    {
        StCase* c = &cases[C_ENC_PITCH];
        start(c);
        encode_round_trip(c, new_encoder(), PADDED_PITCH, ENC_RGBA);
    }
    {
        StCase* c = &cases[C_ENC_RGB];
        start(c);
        encode_round_trip(c, new_encoder(), PADDED_PITCH, ENC_RGB);
    }
    {
        StCase* c = &cases[C_ENC_SHORT_SRC];
        start(c);
        fill_source(ROW_BYTES);
        rc = encode(new_encoder(), ROW_BYTES, IMAGE_BYTES - 1, ENC_RGBA,
                    sizeof(g_png));
        expect_error(c, "encode", rc);
    }
    {
        StCase* c = &cases[C_ENC_SHORT_OUT];
        start(c);
        fill_source(ROW_BYTES);
        fill_canary(g_png, sizeof(g_png));
        rc = encode(new_encoder(), ROW_BYTES, IMAGE_BYTES, ENC_RGBA, 40);
        expect_error(c, "encode", rc);
        expect_canary(c, "written_past_size", g_png + 40, sizeof(g_png) - 40);
    }

    /* From here on, cases reuse a handle or give libpng a broken stream. */
    {
        StCase* c = &cases[C_REPEAT];
        start(c);
        void* dec = new_decoder();
        for (int pass = 0; pass < 2; ++pass) {
            fill_canary(g_out, sizeof(g_out));
            rc = decode(dec, png_rgba, sizeof(png_rgba), IMAGE_BYTES, 0, 0,
                        NULL);
            expect_ok(c, "decode", rc);
            if (rc >= 0) {
                expect_pixels(c, g_out, ROW_BYTES, -1);
            }
        }
    }
    {
        StCase* c = &cases[C_ENC_REPEAT];
        start(c);
        void* enc = new_encoder();
        for (int pass = 0; pass < 3; ++pass) {
            encode_round_trip(c, enc, ROW_BYTES, ENC_RGBA);
        }
        expect_ok(c, "delete", scePngEncDelete(enc));
    }
    {
        StCase* c = &cases[C_CORRUPT];
        start(c);
        fill_canary(g_out, sizeof(g_out));
        rc = decode(new_decoder(), g_corrupt, sizeof(g_corrupt), IMAGE_BYTES, 0,
                    0, NULL);
        expect_error(c, "decode", rc);
        expect_canary(c, "written", g_out, sizeof(g_out));
    }
    {
        StCase* c = &cases[C_RECOVER];
        start(c);
        void* dec = new_decoder();
        rc = decode(dec, g_corrupt, sizeof(g_corrupt), IMAGE_BYTES, 0, 0, NULL);
        expect_error(c, "decode_corrupt", rc);
        expect_round_trip(c, dec, png_rgba, sizeof(png_rgba), -1);
    }
    {
        StCase* c = &cases[C_DELETE];
        start(c);
        void* dec = new_decoder();
        expect_round_trip(c, dec, png_rgba, sizeof(png_rgba), -1);
        expect_ok(c, "delete", scePngDecDelete(dec));
        void* dec2 = NULL;
        rc = create_decoder(g_dec_work2, &dec2);
        expect_ok(c, "create", rc);
        if (rc >= 0) {
            expect_round_trip(c, dec2, png_rgba, sizeof(png_rgba), -1);
            expect_ok(c, "delete2", scePngDecDelete(dec2));
        }
    }

    return st_finish_cases(cases, C_COUNT, "");
}
