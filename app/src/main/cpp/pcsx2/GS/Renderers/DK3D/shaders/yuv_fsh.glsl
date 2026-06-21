#version 460

// Convert the merge circuit to YUV and write it back into the feedback buffer. Ported from vulkan/convert.glsl ps_yuv.

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out vec4 oColor;

layout (binding = 0) uniform sampler2D uTexture;

layout (binding = 0, std140) uniform cb0
{
	int EMODA;
	int EMODC;
};

void main()
{
	vec4 i = texture(uTexture, vTexCoord);
	vec4 o = vec4(0.0);

	mat3 rgb2yuv;
	rgb2yuv[0] = vec3(0.587, -0.311, -0.419);
	rgb2yuv[1] = vec3(0.114, 0.500, -0.081);
	rgb2yuv[2] = vec3(0.299, -0.169, 0.500);

	vec3 yuv = rgb2yuv * i.gbr;

	float Y = float(0xDB) / 255.0 * yuv.x + float(0x10) / 255.0;
	float Cr = float(0xE0) / 255.0 * yuv.y + float(0x80) / 255.0;
	float Cb = float(0xE0) / 255.0 * yuv.z + float(0x80) / 255.0;

	switch (EMODA)
	{
		case 0: o.a = i.a; break;
		case 1: o.a = Y; break;
		case 2: o.a = Y / 2.0; break;
		case 3: o.a = 0.0; break;
	}

	switch (EMODC)
	{
		case 0: o.rgb = i.rgb; break;
		case 1: o.rgb = vec3(Y); break;
		case 2: o.rgb = vec3(Y, Cb, Cr); break;
		case 3: o.rgb = vec3(i.a); break;
	}

	oColor = o;
}
