#version 330 core

layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in vec3 aBitangent;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec4 UVRegion;
uniform uint uSpriteID;

out vec3 fragPos;
out vec3 worldNormal;
out vec3 worldTangent;
out vec3 worldBitangent;
out vec2 uv;
flat out uint sprite_id;

void main()
{
    vec4 worldPos = model * vec4(aPosition, 1.0);
    mat3 normalMatrix = transpose(inverse(mat3(model)));

    fragPos = worldPos.xyz;
    worldNormal = normalize(normalMatrix * aNormal);
    worldTangent = normalize(normalMatrix * aTangent);
    worldBitangent = normalize(normalMatrix * aBitangent);
    uv = mix(UVRegion.xy, UVRegion.zw, aTexCoord);
    sprite_id = uSpriteID;

    gl_Position = projection * view * worldPos;
}
