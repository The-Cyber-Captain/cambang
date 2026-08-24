#[compute]
#version 450

// Verification compute shader for CamBANGCaptureResult.get_compute_texture().
// Deliberately trivial: the point is to prove a capture's pixels are reachable
// from a compute dispatch at all, not to do anything useful with them.
//
// Two counters, because they prove different things:
//   pixel_count -- exact and content-independent. Must equal width*height, which
//                  proves the texture was bound, has the expected dimensions,
//                  and every in-bounds invocation could read it.
//   red_sum     -- content-dependent. Compared against a CPU sum of the same
//                  member, which proves the texture actually holds the captured
//                  image rather than an empty or unrelated allocation.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D src_image;

layout(set = 0, binding = 1, std430) restrict buffer Accum {
    uint pixel_count;
    uint red_sum;
} accum;

layout(push_constant, std430) uniform Params {
    uint width;
    uint height;
    uint pad0;
    uint pad1;
} params;

void main() {
    uvec2 gid = gl_GlobalInvocationID.xy;
    if (gid.x >= params.width || gid.y >= params.height) {
        return;
    }
    vec4 texel = texelFetch(src_image, ivec2(gid), 0);
    atomicAdd(accum.pixel_count, 1u);
    atomicAdd(accum.red_sum, uint(round(texel.r * 255.0)));
}
