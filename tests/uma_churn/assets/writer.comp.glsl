#version 450

// GPU writes: pool[w] = gpu_value(w, generation) for every word w in the
// write list. Must match gpu_value() in main.c. Invocations past the end
// repeat the last word's identical write, so no branch is needed.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) writeonly buffer Pool {
    uint pool[];
};

layout(std430, set = 0, binding = 1) readonly buffer WriteList {
    uint widx[];
};

layout(std430, set = 0, binding = 2) readonly buffer Params {
    uint generation;
    uint read_count;
    uint write_count;
};

void main() {
    uint k = min(gl_GlobalInvocationID.x, write_count - 1u);
    uint w = widx[k];
    uint x = w * 0x9E3779B1u + generation * 0x85EBCA6Bu;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    pool[w] = x ^ 0x6A09E667u;
}
