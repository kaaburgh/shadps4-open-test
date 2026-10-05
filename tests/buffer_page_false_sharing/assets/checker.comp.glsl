#version 450

// Reads the GPU-written region A and the CPU-written region B and records
// R = f(A, B, i). Must match combine() in main.c.

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

void main() {
    uint i = gl_GlobalInvocationID.x;
    uint bv = b[i];
    r[i] = (a[i] * 3u) ^ ((bv << 7) | (bv >> 25)) ^ (i * 0x165667B1u);
}
