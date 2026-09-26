#version 330 core

layout(location = 0) in vec3 aPosition;

uniform mat4 lightViewProjection;
uniform mat4 model;
uniform vec3 lightPosition;
uniform float farPlane;

out float shadowDepth;

void main()
{
    vec4 worldPosition = model * vec4(aPosition, 1.0);
    shadowDepth = length(worldPosition.xyz - lightPosition) / farPlane;
    gl_Position = lightViewProjection * worldPosition;
}
