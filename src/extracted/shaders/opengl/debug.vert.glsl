#version 330 core

layout(location = 0) in vec3 apos;
layout(location = 1) in vec3 acolor;

uniform mat4 projection;
uniform mat4 view;

out vec3 vcolor;

void main()
{
    //conserver cet ordre
    gl_Position = projection * view * vec4(apos, 1.0);
    vcolor = acolor;
}