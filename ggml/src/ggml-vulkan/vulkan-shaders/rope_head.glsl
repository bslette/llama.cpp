#include "types.glsl"

#extension GL_EXT_shader_16bit_storage : require

#include "rope_params.glsl"

// Workgroups cover whole rows, each invocation doing up to two pairs: 32 invocations
// per row up to 128-wide rows (8 rows per workgroup), 64 for 256-wide (4 rows), 128
// for 512-wide (2 rows), 256 beyond. Must match the rows per workgroup the host
// dispatches for. Splitting rows into column slices across workgroups made
// concurrent workgroups hit the same few memory channels on AMD when rows are 512
// bytes or wider; one pair per invocation is slower on Volta.
layout(local_size_x = 8, local_size_y = 32, local_size_z = 1) in;

layout (binding = 0) readonly buffer X {A_TYPE rope_data_a[];};
layout (binding = 1) readonly buffer Y {int rope_data_pos[];};
layout (binding = 2) readonly buffer Z {float rope_data_ff[];};
layout (binding = 3) writeonly buffer D {ROPE_D_TYPE rope_data_d[];};
layout (binding = 4) readonly buffer I {uvec2 rope_data_i[];}; // indices for set_rows


layout (push_constant) uniform parameter {
    rope_params pc;
};

// log2 of the invocations per row
uint rope_lanes_log2() {
    const uint pairs = (pc.ne00 + 1) / 2;
    return pairs <= 64 ? 5 : pairs <= 128 ? 6 : pairs <= 256 ? 7 : 8;
}

uint rope_lanes() {
    return 1u << rope_lanes_log2();
}

uint rope_i0() {
    return 2 * (gl_LocalInvocationIndex & (rope_lanes() - 1));
}

uint rope_row() {
    const uint l = rope_lanes_log2();
    return (gl_WorkGroupID.x << (8 - l)) + (gl_LocalInvocationIndex >> l) + 32768 * gl_GlobalInvocationID.z;
}
