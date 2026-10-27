// 32-bit float operations on operand bits. Included by f32_v.comp.glsl and
// f32_s.comp.glsl. The op numbers match F32_OPS in src/main.c.

layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) restrict readonly buffer In {
    uvec4 v[];
} inp;
layout(std430, set = 0, binding = 1) restrict writeonly buffer Out {
    uint r[];
} outp;

const uint OPS = 40u;

uint cmp_mask(float a, float b) {
    return uint(a < b) | (uint(a <= b) << 1) | (uint(a == b) << 2) | (uint(a != b) << 3) |
           (uint(a > b) << 4) | (uint(a >= b) << 5) | (uint(!(a < b)) << 8) |
           (uint(!(a <= b)) << 9) | (uint(!(a > b)) << 10) | (uint(!(a >= b)) << 11);
}

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
    int e = int(o.w & 63u) - 32;
    uint base = i * OPS;

    outp.r[base + 0u] = floatBitsToUint(a + b);
    outp.r[base + 1u] = floatBitsToUint(a - b);
    outp.r[base + 2u] = floatBitsToUint(a * b);
    outp.r[base + 3u] = floatBitsToUint(fma(a, b, c));
    outp.r[base + 4u] = floatBitsToUint(a * b + c);
    outp.r[base + 5u] = floatBitsToUint(min(a, b));
    outp.r[base + 6u] = floatBitsToUint(max(a, b));
    outp.r[base + 7u] = floatBitsToUint(abs(a));
    outp.r[base + 8u] = floatBitsToUint(-a);
    outp.r[base + 9u] = floatBitsToUint(floor(a));
    outp.r[base + 10u] = floatBitsToUint(ceil(a));
    outp.r[base + 11u] = floatBitsToUint(trunc(a));
    outp.r[base + 12u] = floatBitsToUint(roundEven(a));
    outp.r[base + 13u] = floatBitsToUint(fract(a));
    outp.r[base + 14u] = floatBitsToUint(sign(a));
    outp.r[base + 15u] = floatBitsToUint(clamp(a, 0.0, 1.0));
    outp.r[base + 16u] = floatBitsToUint(clamp(a, min(b, c), max(b, c)));
    outp.r[base + 17u] = floatBitsToUint(step(b, a));
    outp.r[base + 18u] = floatBitsToUint(sqrt(a));
    outp.r[base + 19u] = floatBitsToUint(inversesqrt(a));
    outp.r[base + 20u] = floatBitsToUint(1.0 / a);
    outp.r[base + 21u] = floatBitsToUint(a / b);
    outp.r[base + 22u] = floatBitsToUint(exp2(a));
    outp.r[base + 23u] = floatBitsToUint(log2(a));
    outp.r[base + 24u] = floatBitsToUint(sin(a));
    outp.r[base + 25u] = floatBitsToUint(cos(a));
    outp.r[base + 26u] = floatBitsToUint(ldexp(a, e));
    int fe;
    outp.r[base + 27u] = floatBitsToUint(frexp(a, fe));
    outp.r[base + 28u] = uint(fe);
    outp.r[base + 29u] = uint(isnan(a)) | (uint(isinf(a)) << 1) | (uint(isnan(b)) << 2) |
                         (uint(isinf(b)) << 3);
    outp.r[base + 30u] = cmp_mask(a, b);
    outp.r[base + 31u] = uint(isnan(a) || isnan(b)) | (uint(!isnan(a) && !isnan(b)) << 1);
    outp.r[base + 32u] = floatBitsToUint(a == b ? c : a);
    outp.r[base + 33u] = floatBitsToUint(-abs(a));
    outp.r[base + 34u] = floatBitsToUint(min(min(a, b), c));
    outp.r[base + 35u] = floatBitsToUint(max(max(a, b), c));
    outp.r[base + 36u] = floatBitsToUint(clamp(a * 2.0, 0.0, 1.0));
    outp.r[base + 37u] = floatBitsToUint(a * 0.5);
    outp.r[base + 38u] = floatBitsToUint(fma(-a, b, -c));
    outp.r[base + 39u] = floatBitsToUint(a * 4.0 + 1.0);
}
