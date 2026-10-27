// 64-bit float operations. Each thread's uvec4 holds two doubles, a and b.
// Each double result takes two words (low, high). Included by
// f64_v.comp.glsl and f64_s.comp.glsl. The op numbers match F64_OPS in
// src/main.c.

layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) restrict readonly buffer In {
    uvec4 v[];
} inp;
layout(std430, set = 0, binding = 1) restrict writeonly buffer Out {
    uint r[];
} outp;

const uint OPS = 52u;

void put(uint base, uint k, double x) {
    uvec2 w = unpackDouble2x32(x);
    outp.r[base + 2u * k] = w.x;
    outp.r[base + 2u * k + 1u] = w.y;
}

void main() {
    uint i = gl_GlobalInvocationID.x;
#if UNIFORM_OPERANDS
    uvec4 o = inp.v[gl_WorkGroupID.x];
#else
    uvec4 o = inp.v[i];
#endif
    double a = packDouble2x32(o.xy);
    double b = packDouble2x32(o.zw);
    uint base = i * OPS;

    put(base, 0u, a + b);
    put(base, 1u, a - b);
    put(base, 2u, a * b);
    put(base, 3u, fma(a, b, a));
    put(base, 4u, a / b);
    put(base, 5u, sqrt(abs(a)));
    put(base, 6u, floor(a));
    put(base, 7u, ceil(a));
    put(base, 8u, trunc(a));
    put(base, 9u, roundEven(a));
    put(base, 10u, fract(a));
    put(base, 11u, abs(a));
    put(base, 12u, -a);
    put(base, 13u, min(a, b));
    put(base, 14u, max(a, b));
    // NIR folds double(float(a)) to a; gl_WorkGroupID.y is 0 but unknown to it.
    float narrowed = uintBitsToFloat(floatBitsToUint(float(a)) | gl_WorkGroupID.y);
    put(base, 15u, double(narrowed));
    put(base, 16u, double(int(o.x)));
    put(base, 17u, double(o.x));
    put(base, 18u, ldexp(a, int(o.z & 63u) - 32));
    int fe;
    put(base, 19u, frexp(a, fe));
    put(base, 20u, inversesqrt(abs(a)));
    put(base, 21u, 1.0lf / a);
    outp.r[base + 44u] = uint(fe);
    outp.r[base + 45u] = floatBitsToUint(float(a));
    outp.r[base + 46u] = uint(int(a));
    outp.r[base + 47u] = uint(a);
    outp.r[base + 48u] = uint(a < b) | (uint(a <= b) << 1) | (uint(a == b) << 2) |
                         (uint(a != b) << 3) | (uint(a > b) << 4) | (uint(a >= b) << 5) |
                         (uint(!(a < b)) << 8) | (uint(!(a >= b)) << 9);
    outp.r[base + 49u] = uint(isnan(a)) | (uint(isinf(a)) << 1) | (uint(isnan(b)) << 2);
    outp.r[base + 50u] = uint(isnan(a) || isnan(b)) | (uint(!isnan(a) && !isnan(b)) << 1);
    outp.r[base + 51u] = 0u;
}
