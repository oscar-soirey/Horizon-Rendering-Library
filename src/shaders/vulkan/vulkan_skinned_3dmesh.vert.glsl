#version 450
layout(location=0) in vec3 aPosition;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aTexCoord;
layout(location=3) in vec3 aTangent;
layout(location=4) in vec3 aBitangent;
layout(location=5) in uvec4 aBoneIndices;
layout(location=6) in vec4 aBoneWeights;
layout(location=0) out vec3 vWorldPos;
layout(location=1) out vec3 vNormal;
layout(location=2) out vec3 vWorldTangent;
layout(location=3) out vec3 vWorldBitangent;
layout(location=4) out vec2 vUV;

uniform mat4 projection;
uniform mat4 view;
uniform mat4 model;
uniform mat3 normalMatrix;
uniform vec4 UVRegion;
uniform mat4 boneMatrices[128];

void main() {
    mat4 skin = mat4(0.0);
    for (int i = 0; i < 4; ++i)
        skin += boneMatrices[aBoneIndices[i]] * aBoneWeights[i];
    if (aBoneWeights.x + aBoneWeights.y + aBoneWeights.z + aBoneWeights.w <= 0.0)
        skin = mat4(1.0);

    vec4 local = skin * vec4(aPosition, 1.0);
    mat3 skin3 = mat3(skin);
    vec3 localNormal = skin3 * aNormal;
    if (dot(localNormal, localNormal) < 1e-12) localNormal = vec3(0.0, 0.0, 1.0);
    else localNormal = normalize(localNormal);
    vec3 localTangent = skin3 * aTangent;
    vec3 localBitangent = skin3 * aBitangent;

    vec4 world = model * local;
    vec3 N = normalMatrix * localNormal;
    if (dot(N, N) < 1e-12) N = vec3(0.0, 0.0, 1.0);
    else N = normalize(N);
    vec3 T = normalMatrix * localTangent;
    if (dot(T, T) < 1e-8)
        T = abs(N.y) < 0.999 ? cross(N, vec3(0.0, 1.0, 0.0)) : cross(N, vec3(1.0, 0.0, 0.0));
    T = normalize(T - N * dot(N, T));
    vec3 B = normalMatrix * localBitangent;
    if (dot(B, B) < 1e-8)
        B = cross(N, T);
    else
        B = normalize(B);

    vWorldPos = world.xyz;
    vNormal = N;
    vWorldTangent = T;
    vWorldBitangent = B;
    vUV = mix(UVRegion.xy, UVRegion.zw, aTexCoord);
    gl_Position = projection * view * world;
}
