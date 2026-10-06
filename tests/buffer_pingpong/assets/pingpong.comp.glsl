#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) buffer StateBuffer {
    uint state[];
};

layout(std430, set = 0, binding = 1) readonly buffer CpuInputBuffer {
    uint input_data[];
};

void main() {
    uint i = gl_GlobalInvocationID.x;
    state[i] = state[i] + (input_data[i] | 1u) + i * 2u;
}
