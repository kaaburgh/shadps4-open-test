#version 450

// dst[i] = src[i], read only after a long dependent loop, so that the read
// happens late in the dispatch. The loop starts from dst[i] (the poison the
// guest wrote), and the read index depends on its result, so neither the loop
// nor the read can be moved ahead. The guest checks on the CPU that the loop
// result never equals MAGIC for its poison value.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) readonly buffer Source {
    uint src[];
};

layout(std430, set = 0, binding = 1) buffer Destination {
    uint dst[];
};

const uint ITERATIONS = 8192u;
const uint MAGIC = 0x13579BDFu;

void main() {
    uint i = gl_GlobalInvocationID.x;
    uint acc = dst[i];
    for (uint k = 0u; k < ITERATIONS; ++k) {
        acc = acc * 1664525u + 1013904223u;
    }
    dst[i] = acc == MAGIC ? 0u : src[i + (acc == MAGIC ? 1u : 0u)];
}
