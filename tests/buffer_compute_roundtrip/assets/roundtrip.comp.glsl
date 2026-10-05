#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) readonly buffer InputBuffer {
    uint src[];
};

layout(std430, set = 0, binding = 1) writeonly buffer OutputBuffer {
    uint dst[];
};

void main() {
    uint i = gl_GlobalInvocationID.x;
    dst[i] = (src[i] ^ 0xA5A5A5A5u) + i * 0x9E3779B1u;
}
