#version 450
// Shared memory and atomics. A workgroup of 256 invocations spans four
// waves, so the barriers are real. The layout of Out matches LDS_* in
// src/main.c: four words per thread, eight per workgroup, then the global
// counters.
//
// The atomics index their target with z, a per-thread 0 read from the input.
// With a uniform address ACO combines the atomics of a wave with cross-lane
// reductions, which shadPS4 only emulates on 32- and 64-lane host subgroups;
// lavapipe's are 8 lanes wide. s_appended keeps a uniform address, so ACO
// emits DS_APPEND for it.

layout(local_size_x = 256) in;

layout(std430, set = 0, binding = 0) restrict readonly buffer In {
    uint v[];
} inp;
layout(std430, set = 0, binding = 1) buffer Out {
    uint r[];
} outp;

const uint THREADS = 4096u;
const uint GROUP_BASE = THREADS * 4u;
const uint GLOBAL_BASE = GROUP_BASE + 16u * 8u;

shared uint tile[256];
shared uint s_add[1];
shared uint s_min[1];
shared uint s_max[1];
shared uint s_and[1];
shared uint s_or[1];
shared uint s_xor[1];
shared uint s_ticket[1];
shared uint s_cas[1];
shared uint s_appended;

void main() {
    uint l = gl_LocalInvocationID.x;
    uint g = gl_WorkGroupID.x;
    uint i = gl_GlobalInvocationID.x;
    uint x = inp.v[i];
    uint z = inp.v[THREADS + l];

    if (l == 0u) {
        s_add[0] = 0u;
        s_min[0] = 0xffffffffu;
        s_max[0] = 0u;
        s_and[0] = 0xffffffffu;
        s_or[0] = 0u;
        s_xor[0] = 0u;
        s_ticket[0] = 0u;
        s_cas[0] = 0u;
        s_appended = 0u;
    }
    tile[l] = x;
    barrier();

    // Reads from other waves of the workgroup.
    uint mirrored = tile[255u - l];
    uint neighbour = tile[(l + 64u) & 255u];

    atomicAdd(s_add[z], x);
    atomicMin(s_min[z], x);
    atomicMax(s_max[z], x);
    atomicAnd(s_and[z], x | 0x0f0f0f0fu);
    atomicOr(s_or[z], x & 0x00ff00ffu);
    atomicXor(s_xor[z], x);
    uint ticket = atomicAdd(s_ticket[z], 1u);
    atomicAdd(s_appended, 1u);
    uint seen = s_cas[z];
    while (true) {
        uint prev = atomicCompSwap(s_cas[z], seen, seen + 1u);
        if (prev == seen) {
            break;
        }
        seen = prev;
    }
    barrier();

    // Tree reduction of x through the tile.
    tile[l] = x;
    barrier();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (l < s) {
            tile[l] += tile[l + s];
        }
        barrier();
    }

    uint base = i * 4u;
    outp.r[base + 0u] = mirrored;
    outp.r[base + 1u] = neighbour;
    outp.r[base + 2u] = ticket;
    if (l == 0u) {
        uint gb = GROUP_BASE + g * 8u;
        outp.r[gb + 0u] = s_add[0];
        outp.r[gb + 1u] = s_min[0];
        outp.r[gb + 2u] = s_max[0];
        outp.r[gb + 3u] = s_and[0];
        outp.r[gb + 4u] = s_or[0] ^ s_appended;
        outp.r[gb + 5u] = s_xor[0];
        outp.r[gb + 6u] = s_cas[0];
        outp.r[gb + 7u] = tile[0];
    }

    // Global atomics on the output buffer.
    uint gz = GLOBAL_BASE + z;
    atomicAdd(outp.r[gz + 0u], x & 0xffu);
    atomicMax(outp.r[gz + 1u], x);
    atomicMin(outp.r[gz + 2u], x);
    outp.r[base + 3u] = atomicAdd(outp.r[gz + 3u], 1u);
    atomicXor(outp.r[gz + 4u], x);
    uint gseen = outp.r[gz + 5u];
    while (true) {
        uint prev = atomicCompSwap(outp.r[gz + 5u], gseen, gseen + 3u);
        if (prev == gseen) {
            break;
        }
        gseen = prev;
    }
}
