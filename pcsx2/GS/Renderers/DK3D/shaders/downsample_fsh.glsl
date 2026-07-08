#version 460

// ShaderConvert::DOWNSAMPLE_COPY for FilteredDownsampleTexture. Ported from vulkan/convert.glsl.

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out vec4 oColor;

layout (binding = 0) uniform sampler2D samp0;

layout (std140, binding = 0) uniform cb
{
	ivec2 ClampMin;
	int DownsampleFactor;
	int pad0;
	float Weight;
	vec3 pad1;
};

void main()
{
	ivec2 coord = max(ivec2(gl_FragCoord.xy) * DownsampleFactor, ClampMin);
	vec4 result = vec4(0.0);
	for (int yoff = 0; yoff < DownsampleFactor; yoff++)
	{
		for (int xoff = 0; xoff < DownsampleFactor; xoff++)
			result += texelFetch(samp0, coord + ivec2(xoff, yoff), 0);
	}
	oColor = result / Weight;
}
