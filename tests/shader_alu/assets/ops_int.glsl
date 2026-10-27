// 32-bit integer operations. Included by int_v.comp.glsl (operands per
// thread, so ACO selects VALU instructions) and int_s.comp.glsl (operands per
// workgroup, loaded with SMEM, so ACO selects SALU instructions where GCN has
// them). The op numbers match INT_OPS in src/main.c.

layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) restrict readonly buffer In {
    uvec4 v[];
} inp;
layout(std430, set = 0, binding = 1) restrict writeonly buffer Out {
    uint r[];
} outp;

const uint OPS = 48u;

void main() {
    uint i = gl_GlobalInvocationID.x;
#if UNIFORM_OPERANDS
    uvec4 o = inp.v[gl_WorkGroupID.x];
#else
    uvec4 o = inp.v[i];
#endif
    uint a = o.x;
    uint b = o.y;
    uint c = o.z;
    uint d = o.w;
    int ia = int(a);
    int ib = int(b);
    int ic = int(c);
    uint hi;
    uint lo;
    int shi;
    int slo;
    uint base = i * OPS;

    outp.r[base + 0u] = a + b;
    outp.r[base + 1u] = a - b;
    outp.r[base + 2u] = a * b;
    umulExtended(a, b, hi, lo);
    outp.r[base + 3u] = hi;
    imulExtended(ia, ib, shi, slo);
    outp.r[base + 4u] = uint(shi);
    outp.r[base + 5u] = (a & 0xffffffu) * (b & 0xffffffu);
    umulExtended(a & 0xffffffu, b & 0xffffffu, hi, lo);
    outp.r[base + 6u] = hi;
    int sa24 = (ia << 8) >> 8;
    int sb24 = (ib << 8) >> 8;
    outp.r[base + 7u] = uint(sa24 * sb24);
    imulExtended(sa24, sb24, shi, slo);
    outp.r[base + 8u] = uint(shi);
    outp.r[base + 9u] = a / (b | 1u);
    outp.r[base + 10u] = a % (b | 1u);
    int sdiv = (ib | 1) == -1 ? 3 : (ib | 1);
    outp.r[base + 11u] = uint(ia / sdiv);
    outp.r[base + 12u] = uint((ia & 0x7fffffff) % ((ib & 0x7fff) | 1));
    outp.r[base + 13u] = min(a, b);
    outp.r[base + 14u] = max(a, b);
    outp.r[base + 15u] = uint(min(ia, ib));
    outp.r[base + 16u] = uint(max(ia, ib));
    outp.r[base + 17u] = uint(abs(ia));
    outp.r[base + 18u] = uint(clamp(ia, min(ib, ic), max(ib, ic)));
    outp.r[base + 19u] = clamp(a, min(b, c), max(b, c));
    outp.r[base + 20u] = a << (b & 31u);
    outp.r[base + 21u] = a >> (b & 31u);
    outp.r[base + 22u] = uint(ia >> (b & 31u));
    int off = int(c & 31u);
    int bits = int(min(d & 31u, 32u - (c & 31u)));
    outp.r[base + 23u] = bitfieldExtract(a, off, bits);
    outp.r[base + 24u] = uint(bitfieldExtract(ia, off, bits));
    outp.r[base + 25u] = bitfieldInsert(a, b, off, bits);
    outp.r[base + 26u] = bitfieldReverse(a);
    outp.r[base + 27u] = uint(bitCount(a));
    outp.r[base + 28u] = uint(findLSB(a));
    outp.r[base + 29u] = uint(findMSB(a));
    outp.r[base + 30u] = uint(findMSB(ia));
    outp.r[base + 31u] = ~a;
    outp.r[base + 32u] = a & b;
    outp.r[base + 33u] = a | b;
    outp.r[base + 34u] = a ^ b;
    outp.r[base + 35u] = a & ~b;
    outp.r[base + 36u] = a | ~b;
    outp.r[base + 37u] = ~(a | b);
    outp.r[base + 38u] = ~(a ^ b);
    outp.r[base + 39u] = ~(a & b);
    uint carry;
    outp.r[base + 40u] = uaddCarry(a, b, carry) ^ (carry << 31);
    outp.r[base + 41u] = carry;
    uint borrow;
    outp.r[base + 42u] = usubBorrow(a, b, borrow) ^ (borrow << 31);
    outp.r[base + 43u] = borrow;
    outp.r[base + 44u] = uint(a < b) | (uint(a <= b) << 1) | (uint(a == b) << 2) |
                         (uint(a != b) << 3) | (uint(a > b) << 4) | (uint(a >= b) << 5) |
                         (uint(ia < ib) << 8) | (uint(ia <= ib) << 9) | (uint(ia > ib) << 12) |
                         (uint(ia >= ib) << 13);
    outp.r[base + 45u] = a == b ? c : d;
    outp.r[base + 46u] = uint((ia << 24) >> 24) ^ uint((ib << 16) >> 16);
    outp.r[base + 47u] = (a & 0xffffu) * (b & 0xffffu) + c;
}
