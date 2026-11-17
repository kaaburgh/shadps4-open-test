#version 450

// dst[i] = src[i] for a read-only storage buffer.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) readonly buffer Source {
    uint src[];
};

layout(std430, set = 0, binding = 1) writeonly buffer Destination {
    uint dst[];
};

void main() {
    uint i = gl_GlobalInvocationID.x;
    dst[i] = src[i];
}
