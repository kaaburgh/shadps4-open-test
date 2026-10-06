#version 450

// Reads every element of A and B and records it, transformed, in R:
// R[k] = fa(A element k) for k < a_count, then R[a_count + j] = fb(B element j).
// Must match result_a() and result_b() in main.c. Invocations past the end
// repeat the last element's identical write.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) readonly buffer RegionA {
    uint a[];
};

layout(std430, set = 0, binding = 1) readonly buffer RegionB {
    uint b[];
};

layout(std430, set = 0, binding = 2) writeonly buffer Result {
    uint r[];
};

layout(std430, set = 0, binding = 3) readonly buffer Params {
    uint salt;
    uint generation;
    uint a_count;
    uint a_stride;
    uint b_count;
    uint b_stride;
};

void main() {
    uint i = min(gl_GlobalInvocationID.x, a_count + b_count - 1u);
    uint j = i - a_count;
    uint from_a = (a[min(i, a_count - 1u) * a_stride] * 3u) ^ (i * 0x165667B1u);
    uint bv = b[min(j, b_count - 1u) * b_stride];
    uint from_b = ((bv << 7) | (bv >> 25)) ^ (j * 0x2545F491u) ^ 0x6A09E667u;
    r[i] = i < a_count ? from_a : from_b;
}
