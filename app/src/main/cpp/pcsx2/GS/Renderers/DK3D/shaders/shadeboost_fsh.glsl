#version 460

// Shadeboost post-processor

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 oColor;

layout(binding = 0) uniform sampler2D samp0;

layout(std140, binding = 0) uniform cb0
{
	vec4 params; // x: brightness, y: contrast, z: saturation
};

vec4 ContrastSaturationBrightness(vec4 color)
{
	float brt = params.x;
	float con = params.y;
	float sat = params.z;

	const float AvgLumR = 0.5;
	const float AvgLumG = 0.5;
	const float AvgLumB = 0.5;

	const vec3 LumCoeff = vec3(0.2125, 0.7154, 0.0721);

	vec3 AvgLumin = vec3(AvgLumR, AvgLumG, AvgLumB);
	vec3 brtColor = color.rgb * brt;
	float dot_intensity = dot(brtColor, LumCoeff);
	vec3 intensity = vec3(dot_intensity, dot_intensity, dot_intensity);
	vec3 satColor = mix(intensity, brtColor, sat);
	vec3 conColor = mix(AvgLumin, satColor, con);

	color.rgb = conColor;
	return color;
}

void main()
{
	vec4 c = texture(samp0, vTexCoord);
	oColor = ContrastSaturationBrightness(c);
}
