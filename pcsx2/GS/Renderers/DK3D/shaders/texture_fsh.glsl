#version 460

// Samples a single texture across the fullscreen quad.

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out vec4 oColor;

layout (binding = 0) uniform sampler2D uTexture;

void main()
{
	oColor = texture(uTexture, vTexCoord);
}
