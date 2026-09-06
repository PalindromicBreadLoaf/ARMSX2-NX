#version 460

// ImGui vertex shader

layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aUV;
layout (location = 2) in vec4 aColor;

layout (location = 0) out vec2 vUV;
layout (location = 1) out vec4 vColor;

layout (binding = 0, std140) uniform Constants
{
	vec2 uScale;
	vec2 uTranslate;
};

void main()
{
	vUV = aUV;
	vColor = aColor;
	vec2 p = aPos * uScale + uTranslate;
	// Flip Y-space for ImGui
	gl_Position = vec4(p.x, -p.y, 0.0, 1.0);
}
