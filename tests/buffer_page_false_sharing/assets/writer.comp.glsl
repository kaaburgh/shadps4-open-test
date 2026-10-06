#version 450

// GPU-owned update: region A = G(i, salt). Must match expected_a() in main.c.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) writeonly buffer RegionA {
    uint a[];
};

layout(std430, set = 0, binding = 1) readonly buffer Params {
    uint salt;
};

void main() {
    uint i = gl_GlobalInvocationID.x;
    a[i] = (i * 0x01000193u) ^ (salt * 0x85EBCA6Bu) ^ 0xA0000000u;
}
