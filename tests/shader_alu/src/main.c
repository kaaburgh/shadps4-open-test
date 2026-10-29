/*
 * shader_alu: differential test of shader ALU operations.
 *
 * Each family is a compute shader that applies a list of operations to
 * operands from an input buffer and writes every result to an output buffer.
 * The guest computes the same operations on the CPU and compares the results
 * bit for bit, or within a few ULPs for operations GCN only approximates.
 *
 * The shaders are compiled by opengnm-psbc (Mesa ACO) for GFX7, so they test
 * shadPS4's translation of the GCN instructions ACO selects. Families: int
 * (32-bit integer), f32, cvt (conversions and packing), f64 and i64. Every
 * family has two variants, compiled from the same GLSL:
 *   <family>_v  operands differ per thread, so ACO selects VALU instructions;
 *   <family>_s  operands are uniform per workgroup and loaded with SMEM, so
 *               ACO selects SALU instructions where GCN has them.
 * Where GLSL leaves a result open, the reference follows the GCN instruction
 * ACO emits: V_CVT_I32_F32 saturates, V_CVT_PKRTZ_F16_F32 rounds toward zero,
 * V_FRACT_F32 stays below 1.0. A denormal may come back as a zero.
 *
 * The lds case (assets/lds.comp.glsl) checks shared memory across the four
 * waves of a 256-invocation workgroup, LDS and buffer atomics, and DS_APPEND.
 *
 * The f64_pack cases (assets/f64_pack.comp.glsl) multiply f64 values around the
 * f32 range by 1.0, from a buffer and as shader constants. On a device without
 * shaderFloat64 shadPS4 lowers f64 arithmetic to f32; the result line names
 * the mode the device used (f64_mode=fp64 or lowered), and every value must
 * match that mode bit for bit.
 *
 * Output: one line per failing operation ("op <family>.<n> <name>: FAIL ..."
 * with the first failing operands, "lds <check>: FAIL ..." or "pack <case>[n]:
 * FAIL ..."), then one case line per family variant, one for lds and two for
 * f64_pack.
 */

#define SHADTEST_NAME "shader_alu"
#include "shadtest_guest.h"

#include <float.h>
#include <math.h>

enum {
    GROUP_SIZE = 64,
    VALU_GROUPS = 64,
    THREADS = GROUP_SIZE * VALU_GROUPS,
    SALU_GROUPS = VALU_GROUPS,
    MAX_OPS = 64,
    ARENA_BYTES = 8 * 1024 * 1024,
};

static const uint32_t POISON = 0xdeadbeefu;

/* ------------------------------------------------------------------------ */
/* Operands                                                                  */
/* ------------------------------------------------------------------------ */

static const uint32_t SPECIAL_INT[] = {
    0x00000000u, 0x00000001u, 0x00000002u, 0x00000003u, 0x0000001fu,
    0x00000020u, 0x00000021u, 0x0000ffffu, 0x00010000u, 0x00800000u,
    0x00ffffffu, 0x01000000u, 0x12345678u, 0x55555555u, 0x7fffffffu,
    0x80000000u, 0x80000001u, 0x9abcdef0u, 0xaaaaaaaau, 0xff800000u,
    0xfffffffeu, 0xffffffffu,
};
enum { SPECIAL_INT_COUNT = sizeof(SPECIAL_INT) / sizeof(SPECIAL_INT[0]) };

static uint32_t rng_state = 0x2545f491u;

static uint32_t next_random(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* For i below n^2, (i % n, special_pair(i, n)) visits every pair of special
 * values once, and the first workgroups (the SALU variants' operands) see
 * all of them in both positions. */
static unsigned special_pair(uint32_t i, unsigned n) {
    return (i / n + 5 * i) % n;
}

/* Threads below SPECIAL_INT_COUNT^2 pair every special value with every
 * other one in a and b; the rest, and all of c and d, are random. */
static void fill_int_operands(volatile uint32_t* v) {
    for (uint32_t i = 0; i < THREADS; ++i) {
        uint32_t a = next_random();
        uint32_t b = next_random();
        if (i < SPECIAL_INT_COUNT * SPECIAL_INT_COUNT) {
            a = SPECIAL_INT[i % SPECIAL_INT_COUNT];
            b = SPECIAL_INT[special_pair(i, SPECIAL_INT_COUNT)];
        }
        v[i * 4 + 0] = a;
        v[i * 4 + 1] = b;
        v[i * 4 + 2] = next_random();
        v[i * 4 + 3] = next_random();
    }
}

/* ------------------------------------------------------------------------ */
/* 32-bit integer reference (assets/ops_int.glsl)                            */
/* ------------------------------------------------------------------------ */

static const char* const INT_OPS[] = {
    "add", "sub", "mul_lo", "mul_hi_u32", "mul_hi_i32", "mul_u24",
    "mul_hi_u24", "mul_i24", "mul_hi_i24", "udiv", "umod", "sdiv", "smod",
    "min_u32", "max_u32", "min_i32", "max_i32", "abs_i32", "med3_i32",
    "med3_u32", "shl", "lshr", "ashr", "bfe_u32", "bfe_i32", "bfi",
    "bfrev", "bcnt", "ffbl", "ffbh_u32", "ffbh_i32", "not", "and", "or",
    "xor", "andn2", "orn2", "nor", "xnor", "nand", "add_carry", "carry",
    "sub_borrow", "borrow", "cmp_mask", "select", "sext_i8_i16", "mad_u24",
};
enum { INT_OP_COUNT = sizeof(INT_OPS) / sizeof(INT_OPS[0]) };

static uint32_t mul_hi_u(uint32_t a, uint32_t b) {
    return (uint32_t)(((uint64_t)a * b) >> 32);
}

static uint32_t mul_hi_s(int32_t a, int32_t b) {
    return (uint32_t)(((int64_t)a * b) >> 32);
}

static int32_t sext(uint32_t x, unsigned bits) {
    const uint32_t m = 1u << (bits - 1);
    x &= bits == 32 ? 0xffffffffu : (1u << bits) - 1;
    return (int32_t)((x ^ m) - m);
}

static uint32_t clamp_u(uint32_t x, uint32_t lo, uint32_t hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static int32_t clamp_s(int32_t x, int32_t lo, int32_t hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static uint32_t bitfield_extract_u(uint32_t x, unsigned off, unsigned bits) {
    if (bits == 0) {
        return 0;
    }
    return (x >> off) & (bits == 32 ? 0xffffffffu : (1u << bits) - 1);
}

static int32_t find_msb_s(int32_t x) {
    const uint32_t u = x < 0 ? ~(uint32_t)x : (uint32_t)x;
    return u == 0 ? -1 : 31 - __builtin_clz(u);
}

static uint32_t ref_int(unsigned op, const volatile uint32_t* o) {
    const uint32_t a = o[0];
    const uint32_t b = o[1];
    const uint32_t c = o[2];
    const uint32_t d = o[3];
    const int32_t ia = (int32_t)a;
    const int32_t ib = (int32_t)b;
    const int32_t ic = (int32_t)c;
    const int32_t sa24 = sext(a, 24);
    const int32_t sb24 = sext(b, 24);
    const unsigned off = c & 31;
    const unsigned bits = (d & 31) < 32 - off ? (d & 31) : 32 - off;
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return mul_hi_u(a, b);
    case 4: return mul_hi_s(ia, ib);
    case 5: return (a & 0xffffff) * (b & 0xffffff);
    case 6: return mul_hi_u(a & 0xffffff, b & 0xffffff);
    case 7: return (uint32_t)sa24 * (uint32_t)sb24;
    case 8: return mul_hi_s(sa24, sb24);
    case 9: return a / (b | 1);
    case 10: return a % (b | 1);
    case 11: {
        const int32_t div = (ib | 1) == -1 ? 3 : (ib | 1);
        return (uint32_t)(ia / div);
    }
    case 12: return (uint32_t)((ia & 0x7fffffff) % ((ib & 0x7fff) | 1));
    case 13: return a < b ? a : b;
    case 14: return a > b ? a : b;
    case 15: return (uint32_t)(ia < ib ? ia : ib);
    case 16: return (uint32_t)(ia > ib ? ia : ib);
    case 17: return ia < 0 ? 0u - a : a;
    case 18: return (uint32_t)clamp_s(ia, ib < ic ? ib : ic, ib > ic ? ib : ic);
    case 19: return clamp_u(a, b < c ? b : c, b > c ? b : c);
    case 20: return a << (b & 31);
    case 21: return a >> (b & 31);
    case 22: return (uint32_t)(ia >> (b & 31));
    case 23: return bitfield_extract_u(a, off, bits);
    case 24: return bits == 0 ? 0 : (uint32_t)sext(a >> off, bits);
    case 25: {
        const uint32_t mask =
            bits == 0 ? 0 : (bits == 32 ? 0xffffffffu : ((1u << bits) - 1) << off);
        return (a & ~mask) | ((b << off) & mask);
    }
    case 26: {
        uint32_t r = 0;
        for (unsigned k = 0; k < 32; ++k) {
            r |= ((a >> k) & 1) << (31 - k);
        }
        return r;
    }
    case 27: return (uint32_t)__builtin_popcount(a);
    case 28: return a == 0 ? 0xffffffffu : (uint32_t)__builtin_ctz(a);
    case 29: return a == 0 ? 0xffffffffu : (uint32_t)(31 - __builtin_clz(a));
    case 30: return (uint32_t)find_msb_s(ia);
    case 31: return ~a;
    case 32: return a & b;
    case 33: return a | b;
    case 34: return a ^ b;
    case 35: return a & ~b;
    case 36: return a | ~b;
    case 37: return ~(a | b);
    case 38: return ~(a ^ b);
    case 39: return ~(a & b);
    case 40: return (a + b) ^ ((uint32_t)(a + b < a) << 31);
    case 41: return a + b < a;
    case 42: return (a - b) ^ ((uint32_t)(a < b) << 31);
    case 43: return a < b;
    case 44:
        return (uint32_t)(a < b) | ((uint32_t)(a <= b) << 1) | ((uint32_t)(a == b) << 2) |
               ((uint32_t)(a != b) << 3) | ((uint32_t)(a > b) << 4) |
               ((uint32_t)(a >= b) << 5) | ((uint32_t)(ia < ib) << 8) |
               ((uint32_t)(ia <= ib) << 9) | ((uint32_t)(ia > ib) << 12) |
               ((uint32_t)(ia >= ib) << 13);
    case 45: return a == b ? c : d;
    case 46: return (uint32_t)sext(a, 8) ^ (uint32_t)sext(b, 16);
    case 47: return (a & 0xffff) * (b & 0xffff) + c;
    default: return POISON;
    }
}

/* ------------------------------------------------------------------------ */
/* Float helpers                                                             */
/* ------------------------------------------------------------------------ */

static float f32(uint32_t x) {
    float f;
    memcpy(&f, &x, sizeof(f));
    return f;
}

static uint32_t bits32(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    return x;
}

static double f64(uint64_t x) {
    double f;
    memcpy(&f, &x, sizeof(f));
    return f;
}

static uint64_t bits64(double f) {
    uint64_t x;
    memcpy(&x, &f, sizeof(x));
    return x;
}

static bool is_nan32(uint32_t x) {
    return (x & 0x7fffffffu) > 0x7f800000u;
}

static bool is_nan64(uint64_t x) {
    return (x & 0x7fffffffffffffffull) > 0x7ff0000000000000ull;
}

/* Distance in representable values; both must be non-NaN. */
static uint64_t ulp_distance32(uint32_t x, uint32_t y) {
    const int64_t ox = (x & 0x80000000u) ? -(int64_t)(x & 0x7fffffffu) : (int64_t)x;
    const int64_t oy = (y & 0x80000000u) ? -(int64_t)(y & 0x7fffffffu) : (int64_t)y;
    return (uint64_t)(ox > oy ? ox - oy : oy - ox);
}

static uint64_t ulp_distance64(uint64_t x, uint64_t y) {
    const int64_t m = INT64_MAX;
    const int64_t ox = (x >> 63) ? -(int64_t)(x & (uint64_t)m) : (int64_t)x;
    const int64_t oy = (y >> 63) ? -(int64_t)(y & (uint64_t)m) : (int64_t)y;
    return ox > oy ? (uint64_t)ox - (uint64_t)oy : (uint64_t)oy - (uint64_t)ox;
}

/* Zero or denormal. The GPU's denormal mode depends on the shader and on the
 * host driver, so a denormal on one side may be a zero on the other. */
static bool tiny32(uint32_t x) {
    return (x & 0x7f800000u) == 0;
}

static bool tiny64(uint64_t x) {
    return (x & 0x7ff0000000000000ull) == 0;
}

/* An f32 result matches: same bits, both NaN, or a nonzero denormal on one side
 * and a zero of either sign on the other. Zeros of opposite sign differ. */
static bool match32_exact(uint32_t expected, uint32_t got) {
    if (got == expected || (is_nan32(expected) && is_nan32(got))) {
        return true;
    }
    return tiny32(expected) && tiny32(got) &&
           (((expected & 0x7fffffffu) == 0) != ((got & 0x7fffffffu) == 0));
}

static bool match64_exact(uint64_t expected, uint64_t got) {
    if (got == expected || (is_nan64(expected) && is_nan64(got))) {
        return true;
    }
    const uint64_t m = 0x7fffffffffffffffull;
    return tiny64(expected) && tiny64(got) && (((expected & m) == 0) != ((got & m) == 0));
}

static bool match32_ulp(uint32_t expected, uint32_t got, uint64_t ulps) {
    if (match32_exact(expected, got)) {
        return true;
    }
    if (is_nan32(expected) || is_nan32(got)) {
        return false;
    }
    if ((expected & 0x7fffffffu) == 0x7f800000u || (got & 0x7fffffffu) == 0x7f800000u) {
        return false; /* infinities must match exactly */
    }
    if (((expected | got) & 0x7fffffffu) == 0) {
        return false; /* zeros of opposite sign */
    }
    return ulp_distance32(expected, got) <= ulps;
}

/* GCN V_CVT_I32_F32 / V_CVT_U32_F32: truncate, saturate, NaN -> 0. */
static uint32_t cvt_i32_gcn(double x) {
    if (x != x) {
        return 0;
    }
    if (x >= 2147483647.0) {
        return 0x7fffffffu;
    }
    if (x <= -2147483648.0) {
        return 0x80000000u;
    }
    return (uint32_t)(int32_t)x;
}

static uint32_t cvt_u32_gcn(double x) {
    if (x != x || x <= 0.0) {
        return 0;
    }
    if (x >= 4294967295.0) {
        return 0xffffffffu;
    }
    return (uint32_t)x;
}

/* ------------------------------------------------------------------------ */
/* Float operands                                                            */
/* ------------------------------------------------------------------------ */

/* No denormals: GCN flushes f32 denormals by default, the CPU does not. */
static const uint32_t SPECIAL_F32[] = {
    0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x3f000000u, 0xbf000000u,
    0x3fc00000u, 0x40200000u, 0xc0200000u, 0x40400000u, 0x3dcccccdu, 0xbdcccccdu,
    0x501502f9u, 0xd01502f9u, 0x477fe000u, 0x477ff000u, 0x3727c5acu, 0x38800000u,
    0x33d6bf95u, 0xb2abcc77u, 0x7f7fffffu, 0xff7fffffu, 0x00800000u, 0x7f800000u,
    0xff800000u, 0x7fc00000u, 0xffc00000u, 0x4f000000u, 0xcf000000u, 0x4f800000u,
    0x4f7fffffu, 0x3f7fffffu, 0x3effffffu, 0x4b000001u, 0x40490fdbu, 0xc2c80000u,
};
enum { SPECIAL_F32_COUNT = sizeof(SPECIAL_F32) / sizeof(SPECIAL_F32[0]) };

/* A random normal f32 with a moderate exponent, or a special value. */
static uint32_t random_f32(void) {
    const uint32_t r = next_random();
    if ((r & 7) == 0) {
        return SPECIAL_F32[(r >> 3) % SPECIAL_F32_COUNT];
    }
    return (r & 0x807fffffu) | ((96u + ((r >> 23) % 64u)) << 23);
}

static void fill_f32_operands(volatile uint32_t* v) {
    for (uint32_t i = 0; i < THREADS; ++i) {
        uint32_t a = random_f32();
        uint32_t b = random_f32();
        if (i < SPECIAL_F32_COUNT * SPECIAL_F32_COUNT) {
            a = SPECIAL_F32[i % SPECIAL_F32_COUNT];
            b = SPECIAL_F32[special_pair(i, SPECIAL_F32_COUNT)];
        }
        v[i * 4 + 0] = a;
        v[i * 4 + 1] = b;
        v[i * 4 + 2] = random_f32();
        v[i * 4 + 3] = next_random();
    }
}

/* ------------------------------------------------------------------------ */
/* 32-bit float reference (assets/ops_f32.glsl)                              */
/* ------------------------------------------------------------------------ */

static const char* const F32_OPS[] = {
    "add", "sub", "mul", "fma", "mad", "min", "max", "abs", "neg", "floor",
    "ceil", "trunc", "rndne", "fract", "sign", "saturate", "med3", "step",
    "sqrt", "rsq", "rcp", "div", "exp2", "log2", "sin", "cos", "ldexp",
    "frexp_mant", "frexp_exp", "class", "cmp_mask", "cmp_u_o", "select",
    "neg_abs", "min3", "max3", "clamp_mul2", "mul_half", "fma_neg", "mad_imm",
};
enum { F32_OP_COUNT = sizeof(F32_OPS) / sizeof(F32_OPS[0]) };

static uint32_t cmp_mask32(float a, float b) {
    return (uint32_t)(a < b) | ((uint32_t)(a <= b) << 1) | ((uint32_t)(a == b) << 2) |
           ((uint32_t)(a != b) << 3) | ((uint32_t)(a > b) << 4) | ((uint32_t)(a >= b) << 5) |
           ((uint32_t)!(a < b) << 8) | ((uint32_t)!(a <= b) << 9) |
           ((uint32_t)!(a > b) << 10) | ((uint32_t)!(a >= b) << 11);
}

/* GCN V_FRACT_F32: x - floor(x), never 1.0, NaN for infinities. */
static float fract_gcn(float x) {
    if (isinf(x)) {
        return NAN;
    }
    const float r = x - floorf(x);
    return r >= 1.0f ? f32(0x3f7fffffu) : r;
}

static uint32_t ref_f32(unsigned op, const volatile uint32_t* o) {
    const float a = f32(o[0]);
    const float b = f32(o[1]);
    const float c = f32(o[2]);
    const int e = (int)(o[3] & 63) - 32;
    int fe = 0;
    switch (op) {
    case 0: return bits32(a + b);
    case 1: return bits32(a - b);
    case 2: return bits32(a * b);
    case 3: return bits32(a * b + c);
    case 4: return bits32(a * b + c);
    case 5: return bits32(a < b ? a : b);
    case 6: return bits32(a > b ? a : b);
    case 7: return o[0] & 0x7fffffffu;
    case 8: return o[0] ^ 0x80000000u;
    case 9: return bits32(floorf(a));
    case 10: return bits32(ceilf(a));
    case 11: return bits32(truncf(a));
    case 12: return bits32(rintf(a));
    case 13: return bits32(fract_gcn(a));
    case 14: return bits32(a > 0.0f ? 1.0f : (a < 0.0f ? -1.0f : 0.0f));
    case 15: return bits32(a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a));
    case 16: {
        const float lo = b < c ? b : c;
        const float hi = b > c ? b : c;
        return bits32(a < lo ? lo : (a > hi ? hi : a));
    }
    case 17: return bits32(a < b ? 0.0f : 1.0f);
    case 18: return bits32(sqrtf(a));
    case 19: return bits32(1.0f / sqrtf(a));
    case 20: return bits32(1.0f / a);
    case 21: return bits32(a / b);
    case 22: return bits32(exp2f(a));
    case 23: return bits32(log2f(a));
    case 24: return bits32(sinf(a));
    case 25: return bits32(cosf(a));
    case 26: return bits32(ldexpf(a, e));
    case 27: return bits32(frexpf(a, &fe));
    case 28: frexpf(a, &fe); return (uint32_t)fe;
    case 29:
        return (uint32_t)isnan(a) | ((uint32_t)isinf(a) << 1) | ((uint32_t)isnan(b) << 2) |
               ((uint32_t)isinf(b) << 3);
    case 30: return cmp_mask32(a, b);
    case 31: {
        const bool u = isnan(a) || isnan(b);
        return (uint32_t)u | ((uint32_t)!u << 1);
    }
    case 32: return bits32(a == b ? c : a);
    case 33: return o[0] | 0x80000000u;
    case 34: {
        const float m = a < b ? a : b;
        return bits32(m < c ? m : c);
    }
    case 35: {
        const float m = a > b ? a : b;
        return bits32(m > c ? m : c);
    }
    case 36: {
        const float m = a * 2.0f;
        return bits32(m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m));
    }
    case 37: return bits32(a * 0.5f);
    case 38: return bits32(-a * b - c);
    case 39: return bits32(a * 4.0f + 1.0f);
    default: return POISON;
    }
}

static bool match_f32(unsigned op, const volatile uint32_t* o, uint32_t expected,
                      const volatile uint32_t* row) {
    const uint32_t got = row[op];
    const float a = f32(o[0]);
    const float b = f32(o[1]);
    const float c = f32(o[2]);
    const bool any_nan = isnan(a) || isnan(b) || isnan(c);
    switch (op) {
    case 3:
    case 4:
    case 38:
    case 39: {
        /* May be fused or not. */
        const float fused = op == 3 || op == 4 ? fmaf(a, b, c)
                            : op == 38         ? fmaf(-a, b, -c)
                                               : fmaf(a, 4.0f, 1.0f);
        return match32_exact(expected, got) || match32_exact(bits32(fused), got);
    }
    case 5:
    case 6:
    case 15:
    case 16:
    case 34:
    case 35:
    case 36:
        /* GLSL leaves min, max and clamp of NaN undefined, and of +0 and -0 either may
         * come back. */
        return any_nan || match32_exact(expected, got) ||
               ((expected | got) & 0x7fffffffu) == 0;
    case 14:
        if (isnan(a)) {
            return true;
        }
        return a == 0.0f ? (got & 0x7fffffffu) == 0 : got == expected;
    case 21:
        /* A GPU that flushes denormals gets rcp(b) = 0 for |b| > 2^126, as GCN does.
         * Vulkan's precision for division only covers |b| in [2^-126, 2^126]. */
        if (fabsf(b) > 0x1p126f) {
            return true;
        }
        return match32_ulp(expected, got, 3);
    case 18:
    case 19:
    case 20:
        return match32_ulp(expected, got, 3);
    case 22:
        /* Vulkan: 3 + 2|x| ULP. */
        return match32_ulp(expected, got, 3 + 2 * (uint64_t)fminf(fabsf(a), 1e6f));
    case 23:
        /* Vulkan: absolute error 2^-21 on [0.5, 2], 3 ULP elsewhere. */
        if (a >= 0.5f && a <= 2.0f) {
            return !is_nan32(got) && fabsf(f32(got) - f32(expected)) <= 1.0f / 2097152.0f;
        }
        return match32_ulp(expected, got, 3);
    case 24:
    case 25:
        /* V_SIN/V_COS take revolutions and are only valid for small inputs. */
        if (!(fabsf(a) <= 256.0f)) {
            return true;
        }
        return !is_nan32(got) && fabsf(f32(got) - f32(expected)) <= 2e-3f;
    case 27:
        /* frexp of an infinity or NaN is undefined. */
        return !isfinite(a) || match32_exact(expected, got);
    case 28:
        return !isfinite(a) || got == expected;
    case 29:
    case 30:
    case 31:
        return got == expected; /* integer results */
    default:
        return match32_exact(expected, got);
    }
}

/* ------------------------------------------------------------------------ */
/* Conversions (assets/ops_cvt.glsl)                                         */
/* ------------------------------------------------------------------------ */

static const char* const CVT_OPS[] = {
    "cvt_i32_f32", "cvt_u32_f32", "cvt_f32_i32", "cvt_f32_u32", "pkrtz_f16",
    "f16_lo", "f16_hi", "pack_unorm4x8", "pack_snorm4x8", "unorm8_x",
    "unorm8_w", "snorm8_y", "pknorm_u16", "pknorm_i16", "unorm16_y",
    "snorm16_x", "ubyte0", "ubyte2", "ubyte3", "sext4",
};
enum { CVT_OP_COUNT = sizeof(CVT_OPS) / sizeof(CVT_OPS[0]) };

static const uint16_t SPECIAL_F16[] = {
    0x0000, 0x8000, 0x3c00, 0xbc00, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7e00,
    0x0001, 0x03ff, 0x0400, 0x3555, 0x5640, 0xc8b0, 0x7d01,
};
enum { SPECIAL_F16_COUNT = sizeof(SPECIAL_F16) / sizeof(SPECIAL_F16[0]) };

static void fill_cvt_operands(volatile uint32_t* v) {
    fill_f32_operands(v);
    for (uint32_t i = 0; i < THREADS; ++i) {
        uint32_t z = next_random();
        if (i < SPECIAL_F16_COUNT * SPECIAL_F16_COUNT) {
            z = SPECIAL_F16[i % SPECIAL_F16_COUNT] |
                ((uint32_t)SPECIAL_F16[special_pair(i, SPECIAL_F16_COUNT)] << 16);
        }
        v[i * 4 + 2] = z;
        v[i * 4 + 3] = random_f32();
    }
}

/* f32 -> f16 with round toward zero, as V_CVT_PKRTZ_F16_F32. */
static uint32_t f16_rtz(uint32_t x) {
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t mag = x & 0x7fffffffu;
    if (mag > 0x7f800000u) {
        return sign | 0x7e00u | ((mag >> 13) & 0x3ffu);
    }
    if (mag == 0x7f800000u) {
        return sign | 0x7c00u;
    }
    const int exp = (int)(mag >> 23) - 127;
    if (exp > 15) {
        return sign | 0x7bffu;
    }
    if (exp >= -14) {
        return sign | ((uint32_t)(exp + 15) << 10) | ((mag >> 13) & 0x3ffu);
    }
    if (exp < -25) {
        return sign;
    }
    /* f16 denormal: mantissa with the implicit bit, shifted. */
    const uint32_t m = (mag & 0x7fffffu) | 0x800000u;
    return sign | (m >> (13 + (-14 - exp)));
}

static bool is_nan16(uint32_t h) {
    return (h & 0x7fffu) > 0x7c00u;
}

static float f16_to_f32(uint32_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1fu;
    const uint32_t man = h & 0x3ffu;
    if (exp == 0x1f) {
        return f32(sign | 0x7f800000u | (man << 13));
    }
    if (exp == 0) {
        const float m = (float)man * (1.0f / 16777216.0f);
        return sign ? -m : m;
    }
    return f32(sign | ((exp + 112u) << 23) | (man << 13));
}

static float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static uint32_t unorm8(float x) {
    return (uint32_t)rintf(clampf(x, 0.0f, 1.0f) * 255.0f);
}

static uint32_t snorm8(float x) {
    return (uint32_t)(int32_t)rintf(clampf(x, -1.0f, 1.0f) * 127.0f) & 0xffu;
}

static uint32_t ref_cvt(unsigned op, const volatile uint32_t* o) {
    const float a = f32(o[0]);
    const float b = f32(o[1]);
    const float c = f32(o[2]);
    const float d = f32(o[3]);
    const uint32_t z = o[2];
    switch (op) {
    case 0: return cvt_i32_gcn(a);
    case 1: return cvt_u32_gcn(a);
    case 2: return bits32((float)(int32_t)o[1]);
    case 3: return bits32((float)o[1]);
    case 4: return f16_rtz(o[0]) | (f16_rtz(o[1]) << 16);
    case 5: return bits32(f16_to_f32(z & 0xffff));
    case 6: return bits32(f16_to_f32(z >> 16));
    case 7: return unorm8(a) | (unorm8(b) << 8) | (unorm8(c) << 16) | (unorm8(d) << 24);
    case 8: return snorm8(a) | (snorm8(b) << 8) | (snorm8(c) << 16) | (snorm8(d) << 24);
    case 9: return bits32((float)(z & 0xff) * (1.0f / 255.0f));
    case 10: return bits32((float)(z >> 24) * (1.0f / 255.0f));
    case 11: {
        const float s = (float)(int8_t)(z >> 8) * (1.0f / 127.0f);
        return bits32(s < -1.0f ? -1.0f : s);
    }
    case 12:
        return (uint32_t)rintf(clampf(a, 0.0f, 1.0f) * 65535.0f) |
               ((uint32_t)rintf(clampf(b, 0.0f, 1.0f) * 65535.0f) << 16);
    case 13:
        return ((uint32_t)(int32_t)rintf(clampf(a, -1.0f, 1.0f) * 32767.0f) & 0xffffu) |
               ((uint32_t)(int32_t)rintf(clampf(b, -1.0f, 1.0f) * 32767.0f) << 16);
    case 14: return bits32((float)(z >> 16) * (1.0f / 65535.0f));
    case 15: {
        const float s = (float)(int16_t)(z & 0xffff) * (1.0f / 32767.0f);
        return bits32(s < -1.0f ? -1.0f : s);
    }
    case 16: return bits32((float)(z & 0xff));
    case 17: return bits32((float)((z >> 16) & 0xff));
    case 18: return bits32((float)(z >> 24));
    case 19: return bits32((float)((int32_t)(z << 28) >> 28));
    default: return POISON;
    }
}

/* Mask of the result bits that do not depend on a NaN component. */
static uint32_t lanes_without_nan(const float* x, unsigned count, unsigned width) {
    uint32_t mask = 0;
    for (unsigned k = 0; k < count; ++k) {
        if (!isnan(x[k])) {
            mask |= (width == 32 ? 0xffffffffu : (1u << width) - 1) << (k * width);
        }
    }
    return mask;
}

static bool match_cvt(unsigned op, const volatile uint32_t* o, uint32_t expected,
                      const volatile uint32_t* row) {
    const uint32_t got = row[op];
    const float x[4] = {f32(o[0]), f32(o[1]), f32(o[2]), f32(o[3])};
    switch (op) {
    case 0:
    case 1:
        return got == expected; /* integer results */
    case 4: {
        /* Either half may be a NaN with any payload. */
        for (unsigned k = 0; k < 2; ++k) {
            const uint32_t e = (expected >> (16 * k)) & 0xffff;
            const uint32_t g = (got >> (16 * k)) & 0xffff;
            if (e != g && !(is_nan16(e) && is_nan16(g))) {
                return false;
            }
        }
        return true;
    }
    case 7:
    case 8: {
        const uint32_t m = lanes_without_nan(x, 4, 8);
        return (got & m) == (expected & m);
    }
    case 12:
    case 13: {
        /* The hardware's rounding of ties is not documented: allow 1. */
        const uint32_t m = lanes_without_nan(x, 2, 16);
        for (unsigned k = 0; k < 2; ++k) {
            if (!((m >> (16 * k)) & 1)) {
                continue;
            }
            const int32_t e = (int32_t)((expected >> (16 * k)) & 0xffff);
            const int32_t g = (int32_t)((got >> (16 * k)) & 0xffff);
            if (e != g && e - g != 1 && g - e != 1) {
                return false;
            }
        }
        return true;
    }
    default:
        return match32_exact(expected, got);
    }
}

/* ------------------------------------------------------------------------ */
/* 64-bit float reference (assets/ops_f64.glsl)                              */
/* ------------------------------------------------------------------------ */

static const char* const F64_OPS[] = {
    "add.lo", "add.hi", "sub.lo", "sub.hi", "mul.lo", "mul.hi", "fma.lo", "fma.hi",
    "div.lo", "div.hi", "sqrt.lo", "sqrt.hi", "floor.lo", "floor.hi", "ceil.lo",
    "ceil.hi", "trunc.lo", "trunc.hi", "rndne.lo", "rndne.hi", "fract.lo",
    "fract.hi", "abs.lo", "abs.hi", "neg.lo", "neg.hi", "min.lo", "min.hi",
    "max.lo", "max.hi", "f32_f64.lo", "f32_f64.hi", "cvt_f64_i32.lo",
    "cvt_f64_i32.hi", "cvt_f64_u32.lo", "cvt_f64_u32.hi", "ldexp.lo", "ldexp.hi",
    "frexp_mant.lo", "frexp_mant.hi", "rsq.lo", "rsq.hi", "rcp.lo", "rcp.hi",
    "frexp_exp", "cvt_f32_f64", "cvt_i32_f64", "cvt_u32_f64", "cmp_mask",
    "class", "cmp_u_o", "zero",
};
enum { F64_OP_COUNT = sizeof(F64_OPS) / sizeof(F64_OPS[0]) };

static const uint64_t SPECIAL_F64[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3ff0000000000000ull,
    0xbff0000000000000ull, 0x3fe0000000000000ull, 0x3ff8000000000000ull,
    0x4004000000000000ull, 0xc004000000000000ull, 0x4008000000000000ull,
    0x3fb999999999999aull, 0x7e37e43c8800759cull, 0xfe37e43c8800759cull,
    0x01a56e1fc2f8f359ull, 0x7fefffffffffffffull, 0x7ff0000000000000ull,
    0xfff0000000000000ull, 0x7ff8000000000000ull, 0x41e0000000000000ull,
    0xc1e0000000100000ull, 0x41efffffffe00000ull, 0x4340000000000001ull,
    0x4202a05f20000000ull, 0x3fd5555555555555ull, 0xbc7cd2b297d889bcull,
    0x47efffffe0000000ull, 0x400921fb54442d18ull,
};
enum { SPECIAL_F64_COUNT = sizeof(SPECIAL_F64) / sizeof(SPECIAL_F64[0]) };

static uint64_t random_f64(void) {
    const uint64_t r = ((uint64_t)next_random() << 32) | next_random();
    if ((r & 7) == 0) {
        return SPECIAL_F64[(r >> 3) % SPECIAL_F64_COUNT];
    }
    return (r & 0x800fffffffffffffull) | ((uint64_t)(992u + ((r >> 52) % 64u)) << 52);
}

static void fill_f64_operands(volatile uint32_t* v) {
    for (uint32_t i = 0; i < THREADS; ++i) {
        uint64_t a = random_f64();
        uint64_t b = random_f64();
        if (i < SPECIAL_F64_COUNT * SPECIAL_F64_COUNT) {
            a = SPECIAL_F64[i % SPECIAL_F64_COUNT];
            b = SPECIAL_F64[special_pair(i, SPECIAL_F64_COUNT)];
        }
        v[i * 4 + 0] = (uint32_t)a;
        v[i * 4 + 1] = (uint32_t)(a >> 32);
        v[i * 4 + 2] = (uint32_t)b;
        v[i * 4 + 3] = (uint32_t)(b >> 32);
    }
}

/* GCN V_FRACT_F64: x - floor(x), never 1.0, NaN for infinities. */
static double fract64_gcn(double x) {
    if (isinf(x)) {
        return NAN;
    }
    const double r = x - floor(x);
    return r >= 1.0 ? f64(0x3fefffffffffffffull) : r;
}

static double ref_f64_value(unsigned k, double a, double b, uint32_t z) {
    int fe = 0;
    switch (k) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a * b + a;
    case 4: return a / b;
    case 5: return sqrt(fabs(a));
    case 6: return floor(a);
    case 7: return ceil(a);
    case 8: return trunc(a);
    case 9: return rint(a);
    case 10: return fract64_gcn(a);
    case 11: return fabs(a);
    case 12: return -a;
    case 13: return a < b ? a : b;
    case 14: return a > b ? a : b;
    case 15: return (double)(float)a;
    case 16: return (double)(int32_t)(uint32_t)bits64(a);
    case 17: return (double)(uint32_t)bits64(a);
    case 18: return ldexp(a, (int)(z & 63) - 32);
    case 19: return frexp(a, &fe);
    case 20: return 1.0 / sqrt(fabs(a));
    case 21: return 1.0 / a;
    default: return 0.0;
    }
}

static uint32_t ref_f64(unsigned op, const volatile uint32_t* o) {
    const uint64_t ab = (uint64_t)o[0] | ((uint64_t)o[1] << 32);
    const uint64_t bb = (uint64_t)o[2] | ((uint64_t)o[3] << 32);
    const double a = f64(ab);
    const double b = f64(bb);
    if (op < 44) {
        const uint64_t r = bits64(ref_f64_value(op / 2, a, b, o[2]));
        return op & 1 ? (uint32_t)(r >> 32) : (uint32_t)r;
    }
    int fe = 0;
    switch (op) {
    case 44: frexp(a, &fe); return (uint32_t)fe;
    case 45: return bits32((float)a);
    case 46: return cvt_i32_gcn(a);
    case 47: return cvt_u32_gcn(a);
    case 48:
        return (uint32_t)(a < b) | ((uint32_t)(a <= b) << 1) | ((uint32_t)(a == b) << 2) |
               ((uint32_t)(a != b) << 3) | ((uint32_t)(a > b) << 4) |
               ((uint32_t)(a >= b) << 5) | ((uint32_t)!(a < b) << 8) |
               ((uint32_t)!(a >= b) << 9);
    case 49: return (uint32_t)isnan(a) | ((uint32_t)isinf(a) << 1) | ((uint32_t)isnan(b) << 2);
    case 50: {
        const bool u = isnan(a) || isnan(b);
        return (uint32_t)u | ((uint32_t)!u << 1);
    }
    case 51: return 0;
    default: return POISON;
    }
}

static bool match_f64(unsigned op, const volatile uint32_t* o, uint32_t expected,
                      const volatile uint32_t* row) {
    const double a = f64((uint64_t)o[0] | ((uint64_t)o[1] << 32));
    const double b = f64((uint64_t)o[2] | ((uint64_t)o[3] << 32));
    if (op >= 44) {
        if (op == 44) {
            return !isfinite(a) || row[op] == expected;
        }
        return op == 45 ? match32_exact(expected, row[op]) : row[op] == expected;
    }
    const unsigned k = op / 2;
    const uint64_t e = bits64(ref_f64_value(k, a, b, o[2]));
    const uint64_t g = (uint64_t)row[2 * k] | ((uint64_t)row[2 * k + 1] << 32);
    if (match64_exact(e, g)) {
        return true;
    }
    /* ACO computes these from an f32 reciprocal and Newton steps, without the
     * special cases: only check normal operands and results. */
    const double lo = 4 * DBL_MIN;
    const bool normal_a = isfinite(a) && fabs(a) >= lo;
    const bool normal_b = isfinite(b) && fabs(b) >= lo && fabs(1.0 / b) >= lo;
    switch (k) {
    case 3: /* fused or not */
        return match64_exact(bits64(fma(a, b, a)), g);
    case 13:
    case 14:
        /* NaN undefined; min/max of +0 and -0 may return either. */
        return isnan(a) || isnan(b) || (a == b && (g == bits64(a) || g == bits64(b)));
    case 19:
        return !isfinite(a);
    case 4:
        if (!normal_b || !isfinite(a) || !isfinite(a / b) || fabs(a / b) < lo) {
            return true;
        }
        return !is_nan64(g) && ulp_distance64(e, g) <= 2;
    case 5:
    case 20:
    case 21:
        if (!normal_a || fabs(1.0 / a) < lo) {
            return true;
        }
        return !is_nan64(g) && ulp_distance64(e, g) <= 2;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------ */
/* 64-bit integer reference (assets/ops_i64.glsl)                            */
/* ------------------------------------------------------------------------ */

static const char* const I64_OPS[] = {
    "add.lo", "add.hi", "sub.lo", "sub.hi", "mul.lo", "mul.hi", "shl.lo", "shl.hi",
    "lshr.lo", "lshr.hi", "ashr.lo", "ashr.hi", "and.lo", "and.hi", "or.lo",
    "or.hi", "xor.lo", "xor.hi", "not.lo", "not.hi", "min_u64.lo", "min_u64.hi",
    "max_u64.lo", "max_u64.hi", "min_i64.lo", "min_i64.hi", "max_i64.lo",
    "max_i64.hi", "udiv.lo", "udiv.hi", "umod.lo", "umod.hi", "mul_u32x32.lo",
    "mul_u32x32.hi", "mul_i32x32.lo", "mul_i32x32.hi", "bfe.lo", "bfe.hi",
    "abs.lo", "abs.hi", "cmp_mask", "zero_neg",
};
enum { I64_OP_COUNT = sizeof(I64_OPS) / sizeof(I64_OPS[0]) };

static const uint64_t SPECIAL_I64[] = {
    0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000002ull,
    0x000000000000003full, 0x0000000000000040ull, 0x00000000ffffffffull,
    0x0000000100000000ull, 0x00000001ffffffffull, 0x123456789abcdef0ull,
    0x7fffffffffffffffull, 0x8000000000000000ull, 0x8000000000000001ull,
    0xfedcba9876543210ull, 0xffffffff00000000ull, 0xfffffffffffffffeull,
    0xffffffffffffffffull,
};
enum { SPECIAL_I64_COUNT = sizeof(SPECIAL_I64) / sizeof(SPECIAL_I64[0]) };

static void fill_i64_operands(volatile uint32_t* v) {
    fill_int_operands(v);
    for (uint32_t i = 0; i < SPECIAL_I64_COUNT * SPECIAL_I64_COUNT; ++i) {
        const uint64_t a = SPECIAL_I64[i % SPECIAL_I64_COUNT];
        const uint64_t b = SPECIAL_I64[special_pair(i, SPECIAL_I64_COUNT)];
        v[i * 4 + 0] = (uint32_t)a;
        v[i * 4 + 1] = (uint32_t)(a >> 32);
        v[i * 4 + 2] = (uint32_t)b ^ (i & 63);
        v[i * 4 + 3] = (uint32_t)(b >> 32);
    }
}

static uint64_t ref_i64_value(unsigned k, uint64_t a, uint64_t b, uint32_t x, uint32_t y,
                              unsigned s) {
    const int64_t ia = (int64_t)a;
    const int64_t ib = (int64_t)b;
    switch (k) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a << s;
    case 4: return a >> s;
    case 5: return (uint64_t)(ia >> s);
    case 6: return a & b;
    case 7: return a | b;
    case 8: return a ^ b;
    case 9: return ~a;
    case 10: return a < b ? a : b;
    case 11: return a > b ? a : b;
    case 12: return (uint64_t)(ia < ib ? ia : ib);
    case 13: return (uint64_t)(ia > ib ? ia : ib);
    case 14: return a / (b | 1);
    case 15: return a % (b | 1);
    case 16: return (uint64_t)x * y;
    case 17: return (uint64_t)((int64_t)(int32_t)x * (int64_t)(int32_t)y);
    case 18: return (a >> (s & 31)) & 0xfffffffffffull;
    case 19: return ia < 0 ? 0 - a : a;
    default: return 0;
    }
}

static uint32_t ref_i64(unsigned op, const volatile uint32_t* o) {
    const uint64_t a = (uint64_t)o[0] | ((uint64_t)o[1] << 32);
    const uint64_t b = (uint64_t)o[2] | ((uint64_t)o[3] << 32);
    const int64_t ia = (int64_t)a;
    const int64_t ib = (int64_t)b;
    if (op < 40) {
        const uint64_t r = ref_i64_value(op / 2, a, b, o[0], o[1], o[2] & 63);
        return op & 1 ? (uint32_t)(r >> 32) : (uint32_t)r;
    }
    if (op == 40) {
        return (uint32_t)(a < b) | ((uint32_t)(a == b) << 1) | ((uint32_t)(a > b) << 2) |
               ((uint32_t)(ia < ib) << 3) | ((uint32_t)(ia > ib) << 4);
    }
    return (uint32_t)(a == 0) | ((uint32_t)(ia < 0) << 1);
}

/* ------------------------------------------------------------------------ */
/* Shared memory and atomics (assets/lds.comp.glsl)                          */
/* ------------------------------------------------------------------------ */

enum {
    LDS_GROUP_SIZE = 256,
    LDS_GROUPS = THREADS / LDS_GROUP_SIZE,
    LDS_GROUP_BASE = THREADS * 4,
    LDS_GLOBAL_BASE = LDS_GROUP_BASE + LDS_GROUPS * 8,
    LDS_WORDS = LDS_GLOBAL_BASE + 8,
};

_Static_assert(LDS_GROUPS == 16, "assets/lds.comp.glsl assumes 16 workgroups");

static void lds_expect(StCase* c, const char* what, uint32_t index, uint32_t expected,
                       uint32_t got) {
    if (got != expected) {
        printf("lds %s: FAIL index=%u expected=%08x got=%08x\n", what, index, expected, got);
        st_case_note(c, what, index, expected, got);
    }
}

/* Checks that values[0..count) hold each of 0..count-1 exactly once. */
static void lds_expect_tickets(StCase* c, const char* what, const volatile uint32_t* r,
                               unsigned stride, unsigned count, uint8_t* seen) {
    memset(seen, 0, count);
    for (unsigned k = 0; k < count; ++k) {
        const uint32_t t = r[k * stride];
        if (t >= count || seen[t]) {
            printf("lds %s: FAIL index=%u got=%08x\n", what, k, t);
            st_case_note(c, what, k, k, t);
            return;
        }
        seen[t] = 1;
    }
}

static void check_lds(const volatile uint32_t* x, const volatile uint32_t* r, StCase* c) {
    static uint8_t seen[THREADS];
    uint32_t g_add = 0, g_max = 0, g_min = 0xffffffffu, g_xor = 0;
    for (unsigned g = 0; g < LDS_GROUPS; ++g) {
        const volatile uint32_t* gx = &x[g * LDS_GROUP_SIZE];
        uint32_t add = 0, mn = 0xffffffffu, mx = 0, and_ = 0xffffffffu, or_ = 0, xor_ = 0;
        for (unsigned l = 0; l < LDS_GROUP_SIZE; ++l) {
            const uint32_t t = g * LDS_GROUP_SIZE + l;
            const uint32_t v = gx[l];
            add += v;
            mn = v < mn ? v : mn;
            mx = v > mx ? v : mx;
            and_ &= v | 0x0f0f0f0fu;
            or_ |= v & 0x00ff00ffu;
            xor_ ^= v;
            g_add += v & 0xff;
            lds_expect(c, "mirrored", t, gx[255 - l], r[t * 4 + 0]);
            lds_expect(c, "neighbour", t, gx[(l + 64) & 255], r[t * 4 + 1]);
        }
        g_max = mx > g_max ? mx : g_max;
        g_min = mn < g_min ? mn : g_min;
        g_xor ^= xor_;
        lds_expect_tickets(c, "group_tickets", &r[g * LDS_GROUP_SIZE * 4 + 2], 4,
                           LDS_GROUP_SIZE, seen);
        const volatile uint32_t* gr = &r[LDS_GROUP_BASE + g * 8];
        lds_expect(c, "shared_add", g, add, gr[0]);
        lds_expect(c, "shared_min", g, mn, gr[1]);
        lds_expect(c, "shared_max", g, mx, gr[2]);
        lds_expect(c, "shared_and", g, and_, gr[3]);
        /* s_or ^ s_appended: DS_APPEND adds 1 per invocation, 256 in all. */
        lds_expect(c, "shared_or_append", g, or_ ^ LDS_GROUP_SIZE, gr[4]);
        lds_expect(c, "shared_xor", g, xor_, gr[5]);
        lds_expect(c, "shared_cmpswap", g, LDS_GROUP_SIZE, gr[6]);
        lds_expect(c, "tree_sum", g, add, gr[7]);
    }
    lds_expect_tickets(c, "global_tickets", &r[3], 4, THREADS, seen);
    const volatile uint32_t* gl = &r[LDS_GLOBAL_BASE];
    lds_expect(c, "global_add", 0, g_add, gl[0]);
    lds_expect(c, "global_max", 1, g_max, gl[1]);
    lds_expect(c, "global_min", 2, g_min, gl[2]);
    lds_expect(c, "global_count", 3, THREADS, gl[3]);
    lds_expect(c, "global_xor", 4, g_xor, gl[4]);
    lds_expect(c, "global_cmpswap", 5, 3 * THREADS, gl[5]);
}

/* ------------------------------------------------------------------------ */
/* f64 packing (assets/f64_pack.comp.glsl)                                   */
/* ------------------------------------------------------------------------ */

/* Values around the f32 range: biased f64 exponents 896 and 897 (below and at
 * FLT_MIN's) and 1150 and 1151 (at and above FLT_MAX's), with zero and nonzero
 * mantissas, and special values. Entry 0 tells the modes apart. The order
 * matches the shader. */
static const uint64_t PACK_VALUES[] = {
    0x3ff0000000001000ull,                        /* 1 + 2^-40 */
    0x3ff8000000000000ull, 0xbff8000000000000ull, /* +-1.5 */
    0x1000000000000000ull, 0x9000000000000000ull, /* +-2^-767 */
    0x3800000000000000ull, 0xb800000000000000ull, /* +-2^-127 */
    0x380fffffe0000000ull, 0xb80fffffe0000000ull, /* just below FLT_MIN */
    0x3810000000000000ull, 0xb810000000000000ull, /* +-FLT_MIN */
    0x3810000020000001ull, 0xb810000020000001ull,
    0x47e0000000000000ull, 0xc7e0000000000000ull, /* +-2^127 */
    0x47efffffe0000000ull, 0xc7efffffe0000000ull, /* +-FLT_MAX */
    0x47efffffffffffffull, 0xc7efffffffffffffull,
    0x47f0000000000000ull, 0xc7f0000000000000ull, /* +-2^128 */
    0x47f0000000000001ull, 0xc7f0000000000001ull,
    0x0000000000000000ull, 0x8000000000000000ull, /* +-0 */
    0x7ff0000000000000ull, 0xfff0000000000000ull, /* +-inf */
    0x7ff8000000000000ull,                        /* quiet NaN */
    0x37d0000000000000ull,                        /* 2^-130 */
};
enum { PACK_COUNT = sizeof(PACK_VALUES) / sizeof(PACK_VALUES[0]) };

/* shadPS4's conversions when the device has no shaderFloat64
 * (lower_fp64_to_fp32.cpp). f64 to f32 truncates the mantissa, flushes
 * exponents below the f32 range to a signed zero and saturates exponents above
 * it to infinity. f32 to f64 is exact for the results here. */
static uint32_t pack_f64_to_f32(uint64_t x) {
    const uint32_t sign = (uint32_t)(x >> 32) & 0x80000000u;
    const int32_t exp = (int32_t)((x >> 52) & 0x7ff);
    const uint32_t mantissa = (uint32_t)(x >> 29) & 0x7fffffu;
    if (exp == 0x7ff) {
        return sign | 0x7f800000u | mantissa;
    }
    if (exp == 0) {
        return sign | mantissa;
    }
    const int32_t exp32 = exp - (1023 - 127);
    if (exp32 <= 0) {
        return sign;
    }
    if (exp32 >= 0xff) {
        return sign | 0x7f800000u;
    }
    return sign | ((uint32_t)exp32 << 23) | mantissa;
}

static uint64_t unpack_f32_to_f64(uint32_t f) {
    const uint64_t sign = (uint64_t)(f & 0x80000000u) << 32;
    const uint32_t exp = (f >> 23) & 0xff;
    const uint64_t mantissa = (uint64_t)(f & 0x7fffffu) << 29;
    if (exp == 0xff) {
        return sign | 0x7ff0000000000000ull | mantissa;
    }
    if (exp == 0) {
        return sign | mantissa;
    }
    return sign | ((uint64_t)(exp + 1023 - 127) << 52) | mantissa;
}

/* The product of x and 1.0: x itself when the device multiplies in f64, or x
 * through shadPS4's conversions when it lowers to f32. */
static uint64_t pack_expected(uint64_t x, bool lowered) {
    return lowered ? unpack_f32_to_f64(pack_f64_to_f32(x)) : x;
}

static uint64_t pack_result(const volatile uint32_t* r, unsigned slot) {
    return (uint64_t)r[2 * slot] | ((uint64_t)r[2 * slot + 1] << 32);
}

/* Checks both variants against the mode the probe (entry 0) shows; a probe that
 * matches neither mode fails as a lowered result. Returns the mode. */
static const char* check_f64_pack(const volatile uint32_t* r, StCase* cases) {
    const bool lowered = pack_result(r, 0) != PACK_VALUES[0];
    for (unsigned variant = 0; variant < 2; ++variant) {
        StCase* c = &cases[variant];
        for (unsigned k = 0; k < PACK_COUNT; ++k) {
            const unsigned slot = variant * PACK_COUNT + k;
            const uint64_t expected = pack_expected(PACK_VALUES[k], lowered);
            const uint64_t got = pack_result(r, slot);
            if (got == expected || (is_nan64(expected) && is_nan64(got))) {
                continue;
            }
            /* ACO folds 0 * one and may drop the sign of a zero constant. */
            if (variant == 1 && (expected << 1) == 0 && (got << 1) == 0) {
                continue;
            }
            printf("pack %s[%u]: FAIL value=%016llx expected=%016llx got=%016llx\n", c->name,
                   k, (unsigned long long)PACK_VALUES[k], (unsigned long long)expected,
                   (unsigned long long)got);
            const bool high = (uint32_t)(got >> 32) != (uint32_t)(expected >> 32);
            st_case_note(c, (uint32_t)got == POISON ? "unwritten" : "mismatch",
                         2 * slot + high, (uint32_t)(expected >> (high ? 32 : 0)),
                         (uint32_t)(got >> (high ? 32 : 0)));
        }
    }
    return lowered ? "lowered" : "fp64";
}

/* ------------------------------------------------------------------------ */
/* Families                                                                  */
/* ------------------------------------------------------------------------ */

typedef uint32_t (*RefFn)(unsigned op, const volatile uint32_t* operands);
/* row is the thread's whole output, for results that span two words. */
typedef bool (*MatchFn)(unsigned op, const volatile uint32_t* operands,
                        uint32_t expected, const volatile uint32_t* row);

typedef struct {
    const char* name;
    const char* const* ops;
    unsigned op_count;
    void (*fill)(volatile uint32_t* operands);
    RefFn ref;
    MatchFn match; /* NULL: bit-exact */
} Family;

static const Family FAMILIES[] = {
    {"int", INT_OPS, INT_OP_COUNT, fill_int_operands, ref_int, NULL},
    {"f32", F32_OPS, F32_OP_COUNT, fill_f32_operands, ref_f32, match_f32},
    {"cvt", CVT_OPS, CVT_OP_COUNT, fill_cvt_operands, ref_cvt, match_cvt},
    {"f64", F64_OPS, F64_OP_COUNT, fill_f64_operands, ref_f64, match_f64},
    {"i64", I64_OPS, I64_OP_COUNT, fill_i64_operands, ref_i64, NULL},
};
enum { FAMILY_COUNT = sizeof(FAMILIES) / sizeof(FAMILIES[0]) };

typedef struct {
    unsigned bad;
    uint32_t thread;
    uint32_t expected;
    uint32_t got;
} OpResult;

/* Compares one variant's output; returns the number of failing operations. */
static unsigned check_variant(const Family* f, bool uniform,
                              const volatile uint32_t* operands,
                              const volatile uint32_t* out, StCase* c) {
    OpResult results[MAX_OPS];
    memset(results, 0, sizeof(results));
    for (uint32_t t = 0; t < THREADS; ++t) {
        const uint32_t source = uniform ? t / GROUP_SIZE : t;
        const volatile uint32_t* o = &operands[source * 4];
        for (unsigned op = 0; op < f->op_count; ++op) {
            const uint32_t expected = f->ref(op, o);
            const volatile uint32_t* row = &out[t * f->op_count];
            const uint32_t got = row[op];
            const bool ok = f->match ? f->match(op, o, expected, row) : got == expected;
            if (ok) {
                continue;
            }
            OpResult* r = &results[op];
            if (r->bad++ == 0) {
                r->thread = t;
                r->expected = expected;
                r->got = got;
            }
            st_case_note(c, got == POISON ? "unwritten" : "mismatch",
                         t * f->op_count + op, expected, got);
        }
    }
    unsigned failed = 0;
    for (unsigned op = 0; op < f->op_count; ++op) {
        const OpResult* r = &results[op];
        if (!r->bad) {
            continue;
        }
        failed += 1;
        const uint32_t source = uniform ? r->thread / GROUP_SIZE : r->thread;
        const volatile uint32_t* o = &operands[source * 4];
        printf("op %s_%c.%u %s: FAIL bad=%u thread=%u a=%08x b=%08x c=%08x "
               "d=%08x expected=%08x got=%08x\n",
               f->name, uniform ? 's' : 'v', op, f->ops[op], r->bad, r->thread, o[0],
               o[1], o[2], o[3], r->expected, r->got);
    }
    return failed;
}

/* The PS4 runs guest code with FTZ and DAZ set. Clear both so that the
 * reference is plain IEEE arithmetic, with denormals. */
static void use_ieee_denormals(void) {
    uint32_t mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr &= ~(0x8000u | 0x0040u);
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

int main(void) {
    st_begin();
    use_ieee_denormals();

    StArena arena;
    if (!st_arena_init(&arena, ARENA_BYTES)) {
        return st_fail("reason=direct_memory");
    }
    StQueue queue;
    if (!st_queue_init(&arena, &queue)) {
        return st_fail("reason=command_memory");
    }

    volatile uint32_t* operands = (volatile uint32_t*)st_arena_alloc(
        &arena, THREADS * 4 * sizeof(uint32_t), ST_PAGE_BYTES);
    _Static_assert(LDS_WORDS <= THREADS * MAX_OPS, "lds output fits in out");
    volatile uint32_t* out = (volatile uint32_t*)st_arena_alloc(
        &arena, THREADS * MAX_OPS * sizeof(uint32_t), ST_PAGE_BYTES);
    GnmBuffer* table =
        (GnmBuffer*)st_arena_alloc(&arena, 2 * sizeof(GnmBuffer), ST_PAGE_BYTES);
    if (!operands || !out || !table) {
        return st_fail("reason=arena_memory");
    }

    StCase cases[2 * FAMILY_COUNT + 3];
    char names[2 * FAMILY_COUNT][32];
    unsigned failed_ops = 0;
    for (unsigned k = 0; k < FAMILY_COUNT; ++k) {
        const Family* f = &FAMILIES[k];
        f->fill(operands);
        for (unsigned variant = 0; variant < 2; ++variant) {
            const bool uniform = variant == 1;
            StCase* c = &cases[2 * k + variant];
            memset(c, 0, sizeof(*c));
            snprintf(names[2 * k + variant], sizeof(names[0]), "%s_%c", f->name,
                     uniform ? 's' : 'v');
            c->name = names[2 * k + variant];

            char path[96];
            snprintf(path, sizeof(path), "/app0/assets/%s.comp.sb", c->name);
            StComputeShader shader;
            if (!st_load_compute_shader(&arena, path, &shader)) {
                return st_fail("reason=shader_load case=%s", c->name);
            }
            st_fill_words(out, THREADS * f->op_count, POISON);
            table[0] = st_raw_buffer(operands, THREADS * 4 * sizeof(uint32_t));
            table[1] = st_raw_buffer(out, THREADS * f->op_count * sizeof(uint32_t));
            if (!st_dispatch_and_wait(&queue, &shader, table,
                                      uniform ? SALU_GROUPS : VALU_GROUPS)) {
                return st_fail("reason=submit case=%s", c->name);
            }
            failed_ops += check_variant(f, uniform, operands, out, c);
            st_unload_compute_shader(&shader);
        }
    }

    /* lds: per-thread values are random words. */
    StCase* lds_case = &cases[2 * FAMILY_COUNT];
    memset(lds_case, 0, sizeof(*lds_case));
    lds_case->name = "lds";
    for (uint32_t i = 0; i < THREADS; ++i) {
        operands[i] = next_random();
    }
    for (uint32_t i = 0; i < LDS_GROUP_SIZE; ++i) {
        operands[THREADS + i] = 0; /* z in assets/lds.comp.glsl */
    }
    st_fill_words(out, LDS_WORDS, POISON);
    const uint32_t global_init[8] = {0, 0, 0xffffffffu, 0, 0, 0, 0, 0};
    for (unsigned k = 0; k < 8; ++k) {
        out[LDS_GLOBAL_BASE + k] = global_init[k];
    }
    StComputeShader lds;
    if (!st_load_compute_shader(&arena, "/app0/assets/lds.comp.sb", &lds)) {
        return st_fail("reason=shader_load case=lds");
    }
    table[0] = st_raw_buffer(operands, (THREADS + LDS_GROUP_SIZE) * sizeof(uint32_t));
    table[1] = st_raw_buffer(out, LDS_WORDS * sizeof(uint32_t));
    if (!st_dispatch_and_wait(&queue, &lds, table, LDS_GROUPS)) {
        return st_fail("reason=submit case=lds");
    }
    check_lds(operands, out, lds_case);
    st_unload_compute_shader(&lds);

    /* f64_pack: one, then the values. */
    StCase* pack_cases = &cases[2 * FAMILY_COUNT + 1];
    memset(pack_cases, 0, 2 * sizeof(*pack_cases));
    pack_cases[0].name = "f64_pack";
    pack_cases[1].name = "f64_pack_const";
    operands[0] = 0x00000000u;
    operands[1] = 0x3ff00000u;
    for (unsigned k = 0; k < PACK_COUNT; ++k) {
        operands[2 + 2 * k] = (uint32_t)PACK_VALUES[k];
        operands[3 + 2 * k] = (uint32_t)(PACK_VALUES[k] >> 32);
    }
    st_fill_words(out, 4 * PACK_COUNT, POISON);
    StComputeShader pack;
    if (!st_load_compute_shader(&arena, "/app0/assets/f64_pack.comp.sb", &pack)) {
        return st_fail("reason=shader_load case=f64_pack");
    }
    table[0] = st_raw_buffer(operands, (2 + 2 * PACK_COUNT) * sizeof(uint32_t));
    table[1] = st_raw_buffer(out, 4 * PACK_COUNT * sizeof(uint32_t));
    if (!st_dispatch_and_wait(&queue, &pack, table, 1)) {
        return st_fail("reason=submit case=f64_pack");
    }
    const char* mode = check_f64_pack(out, pack_cases);
    printf("f64_pack: mode=%s\n", mode);
    st_unload_compute_shader(&pack);

    char detail[64];
    snprintf(detail, sizeof(detail), "failed_ops=%u f64_mode=%s", failed_ops, mode);
    return st_finish_cases(cases, 2 * FAMILY_COUNT + 3, detail);
}
