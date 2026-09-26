#version 330 core

layout(location = 0) in vec3 aPosition;
layout(location = 5) in uvec4 aBoneIndices;
layout(location = 6) in vec4 aBoneWeights;

uniform mat4 lightViewProjection;
uniform mat4 model;
uniform vec3 lightPosition;
uniform float farPlane;

layout(std140) uniform BoneBlock
{
    mat4 Bones[128];
};

out vec3 worldPosition;

mat4 SkinMatrix()
{
    return
        Bones[aBoneIndices.x] * aBoneWeights.x +
        Bones[aBoneIndices.y] * aBoneWeights.y +
        Bones[aBoneIndices.z] * aBoneWeights.z +
        Bones[aBoneIndices.w] * aBoneWeights.w;
}

void main()
{
    worldPosition = (model * SkinMatrix() * vec4(aPosition, 1.0)).xyz;
    gl_Position = lightViewProjection * vec4(worldPosition, 1.0);
}
