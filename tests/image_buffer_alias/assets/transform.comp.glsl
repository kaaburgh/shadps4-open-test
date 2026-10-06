#version 450

// dst[i] = transform(src[i], i). Must match transform() in main.c.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) readonly buffer Source {
    uint src[];
};

layout(std430, set = 0, binding = 1) writeonly buffer Destination {
    uint dst[];
};

void main() {
    uint i = gl_GlobalInvocationID.x;
    uint v = src[i];
    dst[i] = ((v << 5) | (v >> 27)) ^ (i * 0x2545F491u) ^ 0x6A09E667u;
}
