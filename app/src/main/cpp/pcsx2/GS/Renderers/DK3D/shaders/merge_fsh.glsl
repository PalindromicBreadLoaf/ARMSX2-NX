#version 460

// Output circuit 1 alpha-blended over the background. Ported from Metal/merge.metal

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out vec4 oColor;

layout (binding = 0) uniform sampler2D uTexture;

layout (binding = 0, std140) uniform cb0
{
	vec4 BGColor;
	uint u_mmod;
};

void main()
{
	vec4 c = texture(uTexture, vTexCoord);
	if (u_mmod == 0u)
		c.a *= 2.0; // Blend by the circuit's own alpha
	else
		c.a = BGColor.a; // Blend by the ALP register
	oColor = c;
}
