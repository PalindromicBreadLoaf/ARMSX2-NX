#version 460 core

// Contrast Adaptive Sharpening (CAS) compute shader for deko3d. Ported from Vulkan cas.glsl

layout(std140, binding = 0) uniform cb0
{
    uvec4 const0;
    uvec4 const1;
    ivec2 srcOffset;
    uint sharpenOnly;
};

layout(binding = 0) uniform sampler2D imgSrc;
layout(binding = 0, rgba8) uniform writeonly image2D imgDst;

#define A_GPU 1
#define A_GLSL 1

#include "ffx_a.h"

AF3 CasLoad(ASU2 p)
{
    return texelFetch(imgSrc, srcOffset + ivec2(p), 0).rgb;
}

void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "ffx_cas.h"

layout(local_size_x=64) in;
void main()
{
    // Remap local xy for a more PS2-like swizzle
    AU2 gxy = ARmp8x8(gl_LocalInvocationID.x) + AU2(gl_WorkGroupID.x << 4u, gl_WorkGroupID.y << 4u);

    // Filter four pixels per invocation
    AF4 c = vec4(0.0f);
    CasFilter(c.r, c.g, c.b, gxy, const0, const1, sharpenOnly != 0u);
    imageStore(imgDst, ASU2(gxy), c);
    gxy.x += 8u;

    CasFilter(c.r, c.g, c.b, gxy, const0, const1, sharpenOnly != 0u);
    imageStore(imgDst, ASU2(gxy), c);
    gxy.y += 8u;

    CasFilter(c.r, c.g, c.b, gxy, const0, const1, sharpenOnly != 0u);
    imageStore(imgDst, ASU2(gxy), c);
    gxy.x -= 8u;

    CasFilter(c.r, c.g, c.b, gxy, const0, const1, sharpenOnly != 0u);
    imageStore(imgDst, ASU2(gxy), c);
}
