#include "types.glsl"

#extension GL_EXT_shader_16bit_storage : require

#include "rope_params.glsl"

// 8 rows per workgroup, 32 invocations per row stepping through it 32 pairs at a
// time: rows are often short (64-dim heads), so one row per workgroup would leave
// most invocations idle. Workgroups cover whole rows; splitting rows into column
// slices across workgroups made concurrent workgroups hit the same few memory
// channels on AMD when rows are 512 bytes or wider.
layout(local_size_x = 8, local_size_y = 32, local_size_z = 1) in;

uint rope_i0() {
    return 2 * (gl_LocalInvocationIndex % 32);
}

uint rope_row() {
    return gl_WorkGroupID.x * 8 + gl_LocalInvocationIndex / 32 + 32768 * gl_GlobalInvocationID.z;
}

layout (binding = 0) readonly buffer X {A_TYPE rope_data_a[];};
layout (binding = 1) readonly buffer Y {int rope_data_pos[];};
layout (binding = 2) readonly buffer Z {float rope_data_ff[];};
layout (binding = 3) writeonly buffer D {ROPE_D_TYPE rope_data_d[];};
layout (binding = 4) readonly buffer I {uvec2 rope_data_i[];}; // indices for set_rows


layout (push_constant) uniform parameter {
    rope_params pc;
};

