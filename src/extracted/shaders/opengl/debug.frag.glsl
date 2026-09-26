#version 330 core

in vec3 vcolor;
out vec4 FragColor;

void main()
{
    FragColor = vec4(vcolor, 1.0);
}