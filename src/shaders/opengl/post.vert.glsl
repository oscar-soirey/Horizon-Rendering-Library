#version 330 core

layout(location=0) in vec2 apos;

out vec2 uv;

void main()
{
    uv = apos * 0.5 + 0.5; //from [-1,1] to [0,1];
    gl_Position = vec4(apos, 0.0, 1.0);
}