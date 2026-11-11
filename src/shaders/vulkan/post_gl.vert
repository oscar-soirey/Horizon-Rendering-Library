#version 460
// Full-screen triangle for the custom post-processes: texture coordinates in
// the OpenGL convention of HRL (v = 0 at the bottom of the image).
layout(location = 0) out vec2 vUV;
void main()
{
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
	vUV = vec2(p.x, 1.0 - p.y);
}
