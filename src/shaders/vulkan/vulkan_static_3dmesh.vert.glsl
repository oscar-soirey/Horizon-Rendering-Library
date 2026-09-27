#version 450
layout(location=0) in vec3 aPosition;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aTexCoord;
layout(location=3) in vec3 aTangent;
layout(location=4) in vec3 aBitangent;
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

void main() {
    vec4 world = model * vec4(aPosition, 1.0);
    vec3 N = normalMatrix * aNormal;
    if (dot(N, N) < 1e-12) N = vec3(0.0, 0.0, 1.0);
    else N = normalize(N);
    vec3 T = normalMatrix * aTangent;
    if (dot(T, T) < 1e-8)
        T = abs(N.y) < 0.999 ? cross(N, vec3(0.0, 1.0, 0.0)) : cross(N, vec3(1.0, 0.0, 0.0));
    T = normalize(T - N * dot(N, T));
    vec3 B = normalMatrix * aBitangent;
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
