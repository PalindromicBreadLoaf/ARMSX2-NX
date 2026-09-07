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
const uint RGB5A1_TO_16_BITS  = 2u;
const uint DEPTH32_TO_16_BITS = 12u;
const uint DEPTH32_TO_32_BITS = 13u;

void main()
{
	if (variant == RGB5A1_TO_16_BITS)
	{
		uvec4 cu = uvec4(texture(samp0, vTexCoord) * 255.5);
		o_int = (cu.x >> 3u) | ((cu.y << 2u) & 0x03e0u) | ((cu.z << 7u) & 0x7c00u) | ((cu.w << 8u) & 0x8000u);
	}
	else
	{
		o_int = uint(exp2(32.0) * texture(samp0, vTexCoord).r);
		if (variant == DEPTH32_TO_16_BITS)
			o_int &= 0xFFFFu;
	}
}
