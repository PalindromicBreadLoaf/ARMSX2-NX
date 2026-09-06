#version 460

// Initialises the PrimID tracking image Ported from the ps_stencil_image_init_* variants in vulkan/convert.glsl

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out vec4 oColor;

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
	oColor = vec4(0x7FFFFFFF);

	float a = texelFetch(samp0, ivec2(gl_FragCoord.xy), 0).a;

	bool blocked;
	if (variant == 0u)       // DATM0
		blocked = (127.5 / 255.0) < a;
	else if (variant == 1u)  // DATM1
		blocked = a < (127.5 / 255.0);
	else if (variant == 2u)  // DATM0 + RTA correction
		blocked = (254.5 / 255.0) < a;
	else                     // DATM1 + RTA correction
		blocked = a < (254.5 / 255.0);

	if (blocked)
		oColor = vec4(-1);
}
