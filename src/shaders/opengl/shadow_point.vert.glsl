#version 330 core

layout(location = 0) in vec3 aPosition;

uniform mat4 lightViewProjection;
uniform mat4 model;
uniform vec3 lightPosition;
uniform float farPlane;

out vec3 worldPosition;

void main()
{
    worldPosition = (model * vec4(aPosition, 1.0)).xyz;
    gl_Position = lightViewProjection * vec4(worldPosition, 1.0);
}
