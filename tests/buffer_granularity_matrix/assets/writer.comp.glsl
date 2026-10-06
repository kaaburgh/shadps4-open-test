#version 450

// GPU-owned update: element k of A (word k * a_stride) = G(k, salt, generation).
// Must match gpu_value() in main.c. Invocations past a_count repeat the last
// element's identical write, so no branch is needed.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) writeonly buffer RegionA {
    uint a[];
};

layout(std430, set = 0, binding = 1) readonly buffer Params {
    uint salt;
    uint generation;
    uint a_count;
    uint a_stride;
    uint b_count;
    uint b_stride;
};

void main() {
    uint k = min(gl_GlobalInvocationID.x, a_count - 1u);
    a[k * a_stride] = (k * 0x01000193u) ^ (salt * 0x85EBCA6Bu) ^
                      (generation * 0x9E3779B1u) ^ 0xA0000000u;
}
