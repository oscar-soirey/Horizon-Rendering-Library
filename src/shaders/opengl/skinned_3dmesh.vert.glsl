#version 330 core

layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in vec3 aBitangent;
layout (location = 5) in uvec4 aBoneIndices;
layout (location = 6) in vec4 aBoneWeights;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec4 UVRegion;
uniform uint uSpriteID;

layout(std140) uniform BoneBlock
{
    mat4 Bones[128];
};

out vec3 fragPos;
out vec3 worldNormal;
out vec3 worldTangent;
out vec3 worldBitangent;
out vec2 uv;
flat out uint sprite_id;

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
    mat4 skin = SkinMatrix();
    vec4 localPos = skin * vec4(aPosition, 1.0);
    mat3 skin3 = mat3(skin);

    vec3 localNormal = normalize(skin3 * aNormal);
    vec3 localTangent = normalize(skin3 * aTangent);
    vec3 localBitangent = normalize(skin3 * aBitangent);

    vec4 worldPos = model * localPos;
    mat3 normalMatrix = transpose(inverse(mat3(model)));

    fragPos = worldPos.xyz;
    worldNormal = normalize(normalMatrix * localNormal);
    worldTangent = normalize(normalMatrix * localTangent);
    worldBitangent = normalize(normalMatrix * localBitangent);
    uv = mix(UVRegion.xy, UVRegion.zw, aTexCoord);
    sprite_id = uSpriteID;

    gl_Position = projection * view * worldPos;
}
