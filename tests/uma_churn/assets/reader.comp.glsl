#version 450

// GPU reads: out[i] = mix(pool[ridx[i]] ^ (i * 0x165667B1)). Must match
// read_result() in main.c. Invocations past the end repeat the last entry's
// identical write.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) readonly buffer Pool {
    uint pool[];
};

layout(std430, set = 0, binding = 1) readonly buffer ReadList {
    uint ridx[];
};

layout(std430, set = 0, binding = 2) writeonly buffer Out {
    uint outv[];
};

layout(std430, set = 0, binding = 3) readonly buffer Params {
    uint generation;
    uint read_count;
    uint write_count;
};

void main() {
    uint i = min(gl_GlobalInvocationID.x, read_count - 1u);
    uint x = pool[ridx[i]] ^ (i * 0x165667B1u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    outv[i] = x;
}
