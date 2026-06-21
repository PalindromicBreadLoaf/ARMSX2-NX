#version 460

// ShaderConvert::CLUT_4/CLUT_8 for UpdateCLUTTexture. Ported from vulkan/convert.glsl.

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out vec4 oColor;

layout (binding = 0) uniform sampler2D samp0;

layout (std140, binding = 0) uniform cb
{
	uvec2 offset;
	uint doffset;
	uint variant; // 0 = CLUT_4, 1 = CLUT_8
	float scale;
	vec3 pad;
};

void main()
{
	uvec2 pos;
	if (variant == 0u)
	{
		// CLUT4 is just two rows of 8x8.
		uint index = uint(gl_FragCoord.x) + doffset;
		pos = uvec2(index % 8u, index / 8u);
	}
	else
	{
		uint index = min(uint(gl_FragCoord.x) + doffset, 255u);

		// CLUT8 is arranged into 8 groups of 16x2, with the top-right and bottom-left quadrants swapped.
		uint subgroup = (index / 8u) % 4u;
		pos.x = (index % 8u) + ((subgroup >= 2u) ? 8u : 0u);
		pos.y = ((index / 32u) * 2u) + (subgroup % 2u);
	}

	ivec2 final = ivec2(floor(vec2(offset + pos) * vec2(scale)));
	oColor = texelFetch(samp0, final, 0);
}
