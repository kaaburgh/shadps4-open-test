#version 450

// Must match main.c: WORKGROUP_SIZE, BUCKETS, SUM_SLOTS, mix() and the
// counters layout.
//
// Each invocation stores its source word in LDS and, after the barrier, reads
// the word of the invocation at the mirrored position of its workgroup. With
// 256 invocations, that word was written by a different wave on GCN.
// The word it read then goes to dst and into two sets of buffer atomics.
//
// Neither atomic has the same address on every lane of a wave. With one
// address for the whole wave (counters[64 + gl_WorkGroupID.x]), the compiler
// turns the atomic into a whole-wave reduction (s_or_saveexec_b64 ..., -1)
// and one lane's atomic, which is not what this test is about.

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) readonly buffer Source {
    uint src[];
};

layout(std430, set = 0, binding = 1) writeonly buffer Destination {
    uint dst[];
};

// [0, 64): histogram of (v & 63); then 4 partial sums per workgroup,
// indexed by local index & 3.
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
    atomicAdd(counters[64u + gl_WorkGroupID.x * 4u + (lid & 3u)], v);
}
