#version 460

// Fullscreen triangle
// Covers the whole screen and emits 0..1 texture coordinates.

layout (location = 0) out vec2 vTexCoord;

void main()
{
	vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	vTexCoord = uv;
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
