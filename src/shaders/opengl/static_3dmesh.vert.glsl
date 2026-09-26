#version 330 core

layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in vec3 aBitangent;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat3 normalMatrix;
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
    fragPos = worldPos.xyz;
    vec3 N = normalMatrix * aNormal;
    N = normalize(N);
    vec3 T = normalMatrix * aTangent;
    if (dot(T, T) < 1e-8)
        T = abs(N.y) < 0.999 ? cross(N, vec3(0.0, 1.0, 0.0)) : cross(N, vec3(1.0, 0.0, 0.0));
    T = normalize(T - N * dot(N, T));
    vec3 B = cross(N, T);
    if (dot(B, B) < 1e-8)
        B = abs(N.x) < 0.999 ? cross(N, vec3(1.0, 0.0, 0.0)) : cross(N, vec3(0.0, 0.0, 1.0));
    B = normalize(B);
    worldNormal = N;
    worldTangent = T;
    worldBitangent = B;
    uv = mix(UVRegion.xy, UVRegion.zw, aTexCoord);
    sprite_id = uSpriteID;

    gl_Position = projection * view * worldPos;
}
