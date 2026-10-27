// 64-bit integer operations. Each thread's uvec4 holds two uint64 values, a
// and b. Each result takes two words (low, high). Included by i64_v.comp.glsl
// and i64_s.comp.glsl. The op numbers match I64_OPS in src/main.c.

#extension GL_ARB_gpu_shader_int64 : require

layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) restrict readonly buffer In {
    uvec4 v[];
} inp;
layout(std430, set = 0, binding = 1) restrict writeonly buffer Out {
    uint r[];
} outp;

const uint OPS = 42u;

void put(uint base, uint k, uint64_t x) {
    outp.r[base + 2u * k] = uint(x);
    outp.r[base + 2u * k + 1u] = uint(x >> 32);
}

void main() {
    uint i = gl_GlobalInvocationID.x;
#if UNIFORM_OPERANDS
    uvec4 o = inp.v[gl_WorkGroupID.x];
#else
    uvec4 o = inp.v[i];
#endif
    uint64_t a = packUint2x32(o.xy);
    uint64_t b = packUint2x32(o.zw);
    int64_t ia = int64_t(a);
    int64_t ib = int64_t(b);
    uint s = o.z & 63u;
    uint base = i * OPS;

    put(base, 0u, a + b);
    put(base, 1u, a - b);
    put(base, 2u, a * b);
    put(base, 3u, a << s);
    put(base, 4u, a >> s);
    put(base, 5u, uint64_t(ia >> s));
    put(base, 6u, a & b);
    put(base, 7u, a | b);
    put(base, 8u, a ^ b);
    put(base, 9u, ~a);
    put(base, 10u, min(a, b));
    put(base, 11u, max(a, b));
    put(base, 12u, uint64_t(min(ia, ib)));
    put(base, 13u, uint64_t(max(ia, ib)));
    put(base, 14u, a / (b | 1ul));
    put(base, 15u, a % (b | 1ul));
    put(base, 16u, uint64_t(o.x) * uint64_t(o.y));
    put(base, 17u, uint64_t(int64_t(int(o.x)) * int64_t(int(o.y))));
    put(base, 18u, (a >> (s & 31u)) & 0xffffffffffful);
    put(base, 19u, uint64_t(abs(ia)));
    outp.r[base + 40u] = uint(a < b) | (uint(a == b) << 1) | (uint(a > b) << 2) |
                         (uint(ia < ib) << 3) | (uint(ia > ib) << 4);
    outp.r[base + 41u] = uint(a == 0ul) | (uint(ia < 0l) << 1);
}
