#version 460

// Test for StretchRect/PresentRect/DoMerge.

layout (location = 0) in vec4 aPos;
layout (location = 1) in vec2 aTexCoord;

layout (location = 0) out vec2 vTexCoord;

void main()
{
	gl_Position = aPos;
	vTexCoord = aTexCoord;
}
