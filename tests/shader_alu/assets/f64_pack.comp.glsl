#version 450
// f64 values through an f64 multiply by 1.0, which is loaded at run time. On a
// device without shaderFloat64, shadPS4 lowers f64 arithmetic to f32: the
// input is converted from f64 to f32 and the product back to f64. Elsewhere
// the product is the input.
//
// Thread i < VALUES reads value i from the buffer; thread 0 also computes every
// value as a shader constant, which reaches shadPS4 as scalar register pairs
// that it folds. ACO folds the zero and NaN constants itself. The values and
// their order match PACK_VALUES in src/main.c.
// In: one (2 words), then VALUES values (2 words each). Out: the dynamic
// products, then the constant products, low word first.

layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) restrict readonly buffer In {
    uint v[];
} inp;
layout(std430, set = 0, binding = 1) restrict writeonly buffer Out {
    uint r[];
} outp;

const uint VALUES = 29u;

void put(uint k, double x) {
    uvec2 w = unpackDouble2x32(x);
    outp.r[2u * k] = w.x;
    outp.r[2u * k + 1u] = w.y;
}

#define C(k, lo, hi) put(VALUES + k, packDouble2x32(uvec2(lo, hi)) * one)

void main() {
    uint i = gl_LocalInvocationID.x;
    double one = packDouble2x32(uvec2(inp.v[0], inp.v[1]));
    if (i < VALUES) {
        put(i, packDouble2x32(uvec2(inp.v[2u + 2u * i], inp.v[3u + 2u * i])) * one);
    }
    if (i == 0u) {
        C(0u, 0x00001000u, 0x3ff00000u);
        C(1u, 0x00000000u, 0x3ff80000u);
        C(2u, 0x00000000u, 0xbff80000u);
        C(3u, 0x00000000u, 0x10000000u);
        C(4u, 0x00000000u, 0x90000000u);
        C(5u, 0x00000000u, 0x38000000u);
        C(6u, 0x00000000u, 0xb8000000u);
        C(7u, 0xe0000000u, 0x380fffffu);
        C(8u, 0xe0000000u, 0xb80fffffu);
        C(9u, 0x00000000u, 0x38100000u);
        C(10u, 0x00000000u, 0xb8100000u);
        C(11u, 0x20000001u, 0x38100000u);
        C(12u, 0x20000001u, 0xb8100000u);
        C(13u, 0x00000000u, 0x47e00000u);
        C(14u, 0x00000000u, 0xc7e00000u);
        C(15u, 0xe0000000u, 0x47efffffu);
        C(16u, 0xe0000000u, 0xc7efffffu);
        C(17u, 0xffffffffu, 0x47efffffu);
        C(18u, 0xffffffffu, 0xc7efffffu);
        C(19u, 0x00000000u, 0x47f00000u);
        C(20u, 0x00000000u, 0xc7f00000u);
        C(21u, 0x00000001u, 0x47f00000u);
        C(22u, 0x00000001u, 0xc7f00000u);
        C(23u, 0x00000000u, 0x00000000u);
        C(24u, 0x00000000u, 0x80000000u);
        C(25u, 0x00000000u, 0x7ff00000u);
        C(26u, 0x00000000u, 0xfff00000u);
        C(27u, 0x00000000u, 0x7ff80000u);
        C(28u, 0x00000000u, 0x37d00000u);
    }
}
