#version 450

// dst[i] = word i of a 4 KiB uniform buffer. SMALL_WORDS in main.c must
// match the array size times four.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std140, set = 0, binding = 0) uniform Source {
    uvec4 src[256];
};

layout(std430, set = 0, binding = 1) writeonly buffer Destination {
    uint dst[];
};

void main() {
    uint i = gl_GlobalInvocationID.x;
    dst[i] = src[i >> 2][i & 3u];
}
