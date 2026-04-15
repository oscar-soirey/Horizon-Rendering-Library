#version 330 core

in vec2 uv;

uniform vec4 uTintColor;
uniform sampler2D uTexture;

out vec4 FragColor;

void main()
{
    vec4 color = texture(uTexture, uv);
    color = color * uTintColor;
    FragColor = color;
}