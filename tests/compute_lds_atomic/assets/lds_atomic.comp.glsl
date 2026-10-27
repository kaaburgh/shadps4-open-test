#version 450

// Must match main.c: WORKGROUP_SIZE, BUCKETS, mix() and the counters layout.
//
// Each invocation stores its source word in LDS and, after the barrier, reads
// the word of the invocation at the mirrored position of its workgroup. With
// 256 invocations, that word was written by a different wave on GCN.
// The word it read then goes to dst and into two sets of buffer atomics.

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) readonly buffer Source {
    uint src[];
};

layout(std430, set = 0, binding = 1) writeonly buffer Destination {
    uint dst[];
};

// [0, 64): histogram of (v & 63); [64, 64 + workgroups): per-workgroup sums.
layout(std430, set = 0, binding = 2) buffer Counters {
    uint counters[];
};

shared uint lds[256];

void main() {
    uint lid = gl_LocalInvocationIndex;
    uint i = gl_GlobalInvocationID.x;

    lds[lid] = src[i];
    barrier();
    uint v = lds[255u - lid];

    dst[i] = v ^ (i * 0x2545F491u);
    atomicAdd(counters[v & 63u], 1u);
    atomicAdd(counters[64u + gl_WorkGroupID.x], v);
}
