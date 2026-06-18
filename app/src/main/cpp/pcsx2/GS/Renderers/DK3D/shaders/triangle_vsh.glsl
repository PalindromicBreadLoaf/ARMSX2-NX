#version 460

// It's a test triangle

const vec4 kPositions[3] = vec4[](
	vec4( 0.0,  0.8, 0.0, 1.0),
	vec4(-0.8, -0.8, 0.0, 1.0),
	vec4( 0.8, -0.8, 0.0, 1.0)
);

const vec4 kColors[3] = vec4[](
	vec4(1.0, 0.2, 0.2, 1.0),
	vec4(0.2, 1.0, 0.2, 1.0),
	vec4(0.2, 0.2, 1.0, 1.0)
);

layout (location = 0) out vec4 vColor;

void main()
{
	gl_Position = kPositions[gl_VertexID];
	vColor = kColors[gl_VertexID];
}
