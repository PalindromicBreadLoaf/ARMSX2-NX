#version 460

// Samples the RT alpha at the current pixel and discards fragments that fail the destination-alpha test.
// Ported from the ps_datm* variants in vulkan/convert.glsl
//
// This pass binds depth as the only render target. This must no produce any colour target.

layout (location = 0) in vec2 vTexCoord;

layout (binding = 0) uniform sampler2D samp0;

layout (std140, binding = 0) uniform cb
{
	uint variant;
	uint pad0;
	uint pad1;
	uint pad2;
};

void main()
{
	float a = texelFetch(samp0, ivec2(gl_FragCoord.xy), 0).a;

	bool fail;
	if (variant == 0u)       // DATM0
		fail = (127.5 / 255.0) < a;
	else if (variant == 1u)  // DATM1
		fail = a < (127.5 / 255.0);
	else if (variant == 2u)  // DATM0 + RTA correction
		fail = (254.5 / 255.0) < a;
	else                     // DATM1 + RTA correction
		fail = a < (254.5 / 255.0);

	if (fail)
		discard;

	// Surviving fragments reach the ROP and trigger the stencil replace with 1 op
	gl_FragDepth = gl_FragCoord.z;
}
