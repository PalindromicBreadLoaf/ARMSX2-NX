#version 460

// Integer-output ShaderConvert variants for StretchRect. Ported from Metal/convert.metal

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out uint o_int;

layout (binding = 0) uniform sampler2D samp0;

layout (std140, binding = 0) uniform cb
{
	uint variant;
};

// Must match GSDevice.h ShaderConvert ordering.
const uint RGBA8_TO_16_BITS   = 1u;
const uint FLOAT32_TO_16_BITS = 11u;
const uint FLOAT32_TO_32_BITS = 12u;

void main()
{
	if (variant == RGBA8_TO_16_BITS)
	{
		uvec4 cu = uvec4(texture(samp0, vTexCoord) * 255.0 + 0.5);
		o_int = (cu.x >> 3u) | ((cu.y << 2u) & 0x03e0u) | ((cu.z << 7u) & 0x7c00u) | ((cu.w << 8u) & 0x8000u);
	}
	else
	{
		o_int = uint(exp2(32.0) * texture(samp0, vTexCoord).r);
	}
}
