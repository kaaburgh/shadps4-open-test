// Conversions and packing. Included by cvt_v.comp.glsl and cvt_s.comp.glsl.
// The op numbers match CVT_OPS in src/main.c.

layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) restrict readonly buffer In {
    uvec4 v[];
} inp;
layout(std430, set = 0, binding = 1) restrict writeonly buffer Out {
    uint r[];
} outp;

const uint OPS = 20u;

void main() {
    uint i = gl_GlobalInvocationID.x;
#if UNIFORM_OPERANDS
    uvec4 o = inp.v[gl_WorkGroupID.x];
#else
    uvec4 o = inp.v[i];
#endif
    float a = uintBitsToFloat(o.x);
    float b = uintBitsToFloat(o.y);
    float c = uintBitsToFloat(o.z);
    float d = uintBitsToFloat(o.w);
    uint base = i * OPS;

    outp.r[base + 0u] = uint(int(a));
    outp.r[base + 1u] = uint(a);
    outp.r[base + 2u] = floatBitsToUint(float(int(o.y)));
    outp.r[base + 3u] = floatBitsToUint(float(o.y));
    outp.r[base + 4u] = packHalf2x16(vec2(a, b));
    vec2 h = unpackHalf2x16(o.z);
    outp.r[base + 5u] = floatBitsToUint(h.x);
    outp.r[base + 6u] = floatBitsToUint(h.y);
    outp.r[base + 7u] = packUnorm4x8(vec4(a, b, c, d));
    outp.r[base + 8u] = packSnorm4x8(vec4(a, b, c, d));
    vec4 u8 = unpackUnorm4x8(o.z);
    outp.r[base + 9u] = floatBitsToUint(u8.x);
    outp.r[base + 10u] = floatBitsToUint(u8.w);
    vec4 s8 = unpackSnorm4x8(o.z);
    outp.r[base + 11u] = floatBitsToUint(s8.y);
    outp.r[base + 12u] = packUnorm2x16(vec2(a, b));
    outp.r[base + 13u] = packSnorm2x16(vec2(a, b));
    vec2 u16 = unpackUnorm2x16(o.z);
    outp.r[base + 14u] = floatBitsToUint(u16.y);
    vec2 s16 = unpackSnorm2x16(o.z);
    outp.r[base + 15u] = floatBitsToUint(s16.x);
    outp.r[base + 16u] = floatBitsToUint(float(o.z & 0xffu));
    outp.r[base + 17u] = floatBitsToUint(float((o.z >> 16) & 0xffu));
    outp.r[base + 18u] = floatBitsToUint(float(o.z >> 24));
    outp.r[base + 19u] = floatBitsToUint(float(int(o.z << 28) >> 28));
}
