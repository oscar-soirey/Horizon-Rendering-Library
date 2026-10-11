// HRL - OpenGL ES 3.0 backend: built-in shaders (GLSL ES 3.00).
//
// These are ports of src/shaders/opengl/*.glsl. Uniform names, vertex attribute
// locations and the std140 LightBlock layout are identical to the OpenGL 3.3
// backend, so materials behave the same on both.
//
// What the ES versions leave out (see docs/GLES3_BACKEND.md):
//   shadow maps, global illumination, environment reflections, voxel light
//   field, bloom / picking / G-buffer outputs and the debug views.
#ifndef HRL_GLES3_SHADERS_H
#define HRL_GLES3_SHADERS_H

namespace gles3_shaders
{

// ---------------------------------------------------------------------------
// Shared chunks
// ---------------------------------------------------------------------------

#define HRL_GLES3_FRAGMENT_HEADER \
	"#version 300 es\n" \
	"precision highp float;\n" \
	"precision highp int;\n"

// Same memory layout as GL_Light in the OpenGL 3.3 backend (144 bytes, std140).
#define HRL_GLES3_LIGHT_BLOCK R"GLSL(
#define MAX_LIGHTS 32

#define HRL_PointLight       0x0011u
#define HRL_DirectionalLight 0x0012u
#define HRL_SpotLight        0x0013u
#define HRL_SkyLight         0x0014u

struct Light
{
    uint  type;
    float intensity;
    float attenuation;
    float innerCutoff;

    vec3  position;
    float outerCutoff;

    vec3  rotation;
    float padding3;

    vec3  color;
    float shadowStrength;

    mat4  shadowMatrix;
    vec4  shadowParams;
};

layout(std140) uniform LightBlock
{
    Light lights[MAX_LIGHTS];
};

// Number of used entries at the start of lights[].
uniform int LightCount;
)GLSL"

#define HRL_GLES3_FOG R"GLSL(
const int FOG_LINEAR = 0x0090;
const int FOG_EXP    = 0x0091;
const int FOG_EXP2   = 0x0092;

uniform int   FogEnabled;
uniform int   FogMode;
uniform vec4  FogColor;
uniform float FogStart;
uniform float FogEnd;
uniform float FogDensity;

vec4 applyFog(vec4 litColor, float dist)
{
    if (FogEnabled == 0)
        return litColor;

    float f;
    if      (FogMode == FOG_LINEAR) f = (FogEnd - dist) / max(FogEnd - FogStart, 1e-5);
    else if (FogMode == FOG_EXP)    f = exp(-FogDensity * dist);
    else                            f = exp(-FogDensity * FogDensity * dist * dist);
    return mix(FogColor, litColor, clamp(f, 0.0, 1.0));
}
)GLSL"

// ---------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------

static const char* const kSpriteVertex = R"GLSL(#version 300 es
precision highp float;
precision highp int;

// Sprite = shared unit plane. The default sprite path is instanced.
layout(location = 0) in vec3 aPosition;
layout(location = 2) in vec2 aTexCoord;
layout(location = 5) in mat4 aInstanceModel;
layout(location = 9) in vec4 aInstanceUVRegion;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec4 UVRegion;
uniform int uInstanced;

out vec3 fragPos;
out vec2 uv;

void main()
{
    vec4 pos = vec4(aPosition, 1.0);
    vec4 worldPos;
    vec4 region;

    if (uInstanced != 0)
    {
        worldPos = aInstanceModel * pos;
        region = aInstanceUVRegion;
    }
    else
    {
        worldPos = model * pos;
        region = UVRegion;
    }

    fragPos = worldPos.xyz;
    uv = mix(region.xy, region.zw, aTexCoord);
    gl_Position = projection * view * worldPos;
}
)GLSL";

static const char* const kSpriteFragment =
	HRL_GLES3_FRAGMENT_HEADER
	R"GLSL(
layout(location = 0) out vec4 FragColor;

in vec3 fragPos;
in vec2 uv;

uniform sampler2D T_Albedo;
uniform sampler2D T_Normal;
uniform sampler2D T_Specular;
uniform sampler2D T_Roughness;
uniform sampler2D T_Metallic;
uniform sampler2D T_Alpha;

uniform vec3 TintColor;
uniform vec3 CamPos;

// Mobile fast path. When the scene is lit by sky lights only and the material
// has none of the normal / specular / roughness / metallic maps, the lighting
// is a constant: the backend sums it on the CPU and the shader needs two
// texture fetches instead of six. The result is identical to the full path.
uniform int  uAmbientOnly;
uniform vec3 uAmbientLight;
)GLSL"
	HRL_GLES3_LIGHT_BLOCK
	HRL_GLES3_FOG
	R"GLSL(
vec3 evalBRDF(vec3 lightDir, vec3 normalTex, vec3 viewDir,
              vec3 F0, float shininess, float specMap,
              float metallic, vec3 lightColor)
{
    float NdotL = max(dot(normalTex, lightDir), 0.0);
    vec3  kD    = (1.0 - metallic) * NdotL * lightColor;

    vec3  halfDir = normalize(lightDir + viewDir);
    float NdotH   = max(dot(normalTex, halfDir), 0.0);
    float spec    = pow(NdotH, shininess) * specMap;
    vec3  kS      = spec * F0 * lightColor;

    return kD + kS;
}

void main()
{
    vec4 albedoSample = texture(T_Albedo, uv);
    vec3 albedo = albedoSample.rgb;
    vec3 result = vec3(0.0);

    if (uAmbientOnly != 0)
    {
        result = uAmbientLight;
    }
    else
    {
        float metallic  = texture(T_Metallic,  uv).r;
        float roughness = clamp(texture(T_Roughness, uv).r, 0.05, 1.0);
        float specMap   = texture(T_Specular,  uv).r;
        vec3  normalTex = texture(T_Normal,    uv).rgb * 2.0 - 1.0;

        vec3  F0        = mix(vec3(0.04), albedo, metallic);
        float shininess = pow(2.0, (1.0 - roughness) * 11.0);
        vec3  viewDir   = normalize(CamPos - fragPos);

        for (int i = 0; i < MAX_LIGHTS; i++)
        {
            if (i >= LightCount)
                break;
            if (lights[i].intensity <= 0.0)
                continue;

            uint lightType  = lights[i].type;
            vec3 lightColor = lights[i].color * lights[i].intensity;

            if (lightType == HRL_SkyLight)
            {
                result += (1.0 - metallic) * lightColor;
            }
            else if (lightType == HRL_PointLight)
            {
                vec3  toLight = lights[i].position - fragPos;
                float dist    = length(toLight);
                if (dist < 1e-4)
                    continue;
                float attenuation = 1.0 / (1.0 + lights[i].attenuation * dist * dist);
                result += evalBRDF(toLight / dist, normalTex, viewDir,
                                   F0, shininess, specMap, metallic, lightColor) * attenuation;
            }
            else if (lightType == HRL_DirectionalLight)
            {
                result += evalBRDF(normalize(-lights[i].rotation), normalTex, viewDir,
                                   F0, shininess, specMap, metallic, lightColor);
            }
            else if (lightType == HRL_SpotLight)
            {
                vec3  toLight = lights[i].position - fragPos;
                float dist    = length(toLight);
                if (dist < 1e-4)
                    continue;
                vec3  lightDir   = toLight / dist;
                float cosTheta   = dot(lightDir, normalize(-lights[i].rotation));
                float spotFactor = smoothstep(lights[i].outerCutoff, lights[i].innerCutoff, cosTheta);
                if (spotFactor <= 0.0)
                    continue;
                float attenuation = 1.0 / (1.0 + lights[i].attenuation * dist * dist);
                result += evalBRDF(lightDir, normalTex, viewDir,
                                   F0, shininess, specMap, metallic, lightColor) * attenuation * spotFactor;
            }
        }
    }

    result *= albedo * TintColor;
    float alpha = albedoSample.a * texture(T_Alpha, uv).r;

    FragColor = applyFog(vec4(result, alpha), length(CamPos - fragPos));
}
)GLSL";

// ---------------------------------------------------------------------------
// 3D meshes (static and skinned share the fragment shader)
// ---------------------------------------------------------------------------

static const char* const kMeshVertex = R"GLSL(#version 300 es
precision highp float;
precision highp int;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec3 aTangent;
layout(location = 4) in vec3 aBitangent;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat3 normalMatrix;
uniform vec4 UVRegion;

out vec3 fragPos;
out vec3 worldNormal;
out vec3 worldTangent;
out vec3 worldBitangent;
out vec2 uv;

void main()
{
    vec4 worldPos = model * vec4(aPosition, 1.0);
    fragPos = worldPos.xyz;
    vec3 N = normalize(normalMatrix * aNormal);
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

    gl_Position = projection * view * worldPos;
}
)GLSL";

static const char* const kSkinnedMeshVertex = R"GLSL(#version 300 es
precision highp float;
precision highp int;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec3 aTangent;
layout(location = 4) in vec3 aBitangent;
layout(location = 5) in uvec4 aBoneIndices;
layout(location = 6) in vec4 aBoneWeights;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec4 UVRegion;

layout(std140) uniform BoneBlock
{
    mat4 Bones[128];
};

out vec3 fragPos;
out vec3 worldNormal;
out vec3 worldTangent;
out vec3 worldBitangent;
out vec2 uv;

void main()
{
    mat4 skin =
        Bones[aBoneIndices.x] * aBoneWeights.x +
        Bones[aBoneIndices.y] * aBoneWeights.y +
        Bones[aBoneIndices.z] * aBoneWeights.z +
        Bones[aBoneIndices.w] * aBoneWeights.w;

    vec4 localPos = skin * vec4(aPosition, 1.0);
    mat3 skin3 = mat3(skin);

    vec3 localNormal = normalize(skin3 * aNormal);
    vec3 localTangent = normalize(skin3 * aTangent);
    vec3 localBitangent = normalize(skin3 * aBitangent);

    vec4 worldPos = model * localPos;
    mat3 normalMat = transpose(inverse(mat3(model)));

    fragPos = worldPos.xyz;
    worldNormal = normalize(normalMat * localNormal);
    worldTangent = normalize(normalMat * localTangent);
    worldBitangent = normalize(normalMat * localBitangent);
    uv = mix(UVRegion.xy, UVRegion.zw, aTexCoord);

    gl_Position = projection * view * worldPos;
}
)GLSL";

static const char* const kMeshFragment =
	HRL_GLES3_FRAGMENT_HEADER
	R"GLSL(
layout(location = 0) out vec4 FragColor;

in vec3 fragPos;
in vec2 uv;
in vec3 worldNormal;
in vec3 worldTangent;
in vec3 worldBitangent;

uniform sampler2D T_Albedo;
uniform sampler2D T_Normal;
uniform sampler2D T_Specular;
uniform sampler2D T_Roughness;
uniform sampler2D T_Metallic;
uniform sampler2D T_AO;
uniform sampler2D T_Alpha;

uniform vec3 TintColor;
uniform float BaseColorAlpha;
uniform float RoughnessValue;
uniform float MetallicValue;
uniform float SpecularValue;
uniform float OpacityValue;
uniform int RoughnessUseValue;
uniform int MetallicUseValue;
uniform int SpecularUseValue;
uniform int OpacityUseValue;
uniform int RoughnessInvert;
uniform int AlphaInvert;
uniform int TwoSided;
uniform vec3 CamPos;
)GLSL"
	HRL_GLES3_LIGHT_BLOCK
	HRL_GLES3_FOG
	R"GLSL(
const float PI = 3.14159265359;

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    float f = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return F0 + (1.0 - F0) * f;
}

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-6);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-6);
}

vec3 evalBRDF(vec3 lightDir, vec3 normalWorld, vec3 viewDir,
              vec3 baseColor, float roughness, float specMap,
              float metallic, vec3 lightColor)
{
    float NdotL = max(dot(normalWorld, lightDir), 0.0);
    float NdotV = max(dot(normalWorld, viewDir), 0.0);
    if (NdotL <= 0.0 || NdotV <= 0.0)
        return vec3(0.0);

    vec3 halfDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(normalWorld, halfDir), 0.0);
    float HdotV = max(dot(halfDir, viewDir), 0.0);

    vec3 F0 = mix(vec3(0.04), baseColor, metallic);
    vec3 F = FresnelSchlick(HdotV, F0) * clamp(specMap, 0.0, 1.0);
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);

    vec3 specular = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-5);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 diffuse = kD * baseColor / PI;

    return (diffuse + specular) * lightColor * NdotL;
}

void main()
{
    vec4 albedoSample = texture(T_Albedo, uv);

    float alphaSample = texture(T_Alpha, uv).r;
    if (AlphaInvert != 0)
        alphaSample = 1.0 - alphaSample;
    float alpha = albedoSample.a * BaseColorAlpha *
        (OpacityUseValue != 0 ? OpacityValue : alphaSample);
    if (alpha <= 0.001)
        discard;

    // Color textures are authored in sRGB; lighting is evaluated in linear space.
    vec3 albedo = pow(max(albedoSample.rgb, vec3(0.0)), vec3(2.2));
    float metallic = MetallicUseValue != 0 ? MetallicValue : texture(T_Metallic, uv).r;
    float roughnessSample = texture(T_Roughness, uv).r;
    if (RoughnessInvert != 0)
        roughnessSample = 1.0 - roughnessSample;
    float roughness = RoughnessUseValue != 0 ? RoughnessValue : roughnessSample;
    roughness = clamp(roughness, 0.05, 1.0);

    float specMap = SpecularUseValue != 0 ? SpecularValue : texture(T_Specular, uv).r;
    vec3 normalTex = texture(T_Normal, uv).rgb * 2.0 - 1.0;

    vec3 N = normalize(worldNormal);
    vec3 tangent = worldTangent - N * dot(N, worldTangent);
    if (dot(tangent, tangent) < 1e-8)
        tangent = abs(N.y) < 0.999 ? cross(N, vec3(0.0, 1.0, 0.0)) : cross(N, vec3(1.0, 0.0, 0.0));
    vec3 T = normalize(tangent);
    vec3 B = normalize(cross(N, T));
    if (dot(B, worldBitangent) < 0.0)
        B = -B;
    if (TwoSided != 0 && !gl_FrontFacing)
    {
        N = -N;
        T = -T;
        B = -B;
    }
    vec3 normalWorld = normalize(T * normalTex.x + B * normalTex.y + N * normalTex.z);

    vec3 baseColor = albedo * max(TintColor, vec3(0.0));
    vec3 viewDir = normalize(CamPos - fragPos);
    vec3 lighting = vec3(0.0);

    for (int i = 0; i < MAX_LIGHTS; ++i)
    {
        if (i >= LightCount)
            break;
        if (lights[i].intensity <= 0.0)
            continue;

        uint lightType  = lights[i].type;
        vec3 lightColor = lights[i].color * lights[i].intensity;

        if (lightType == HRL_SkyLight)
        {
            lighting += baseColor * (1.0 - metallic) * lightColor;
        }
        else if (lightType == HRL_PointLight)
        {
            vec3 toLight = lights[i].position - fragPos;
            float dist = length(toLight);
            vec3 lightDir = (dist > 0.0001) ? toLight / dist : vec3(0.0, 0.0, 1.0);
            float attenuation = 1.0 / (1.0 + lights[i].attenuation * dist * dist);
            lighting += evalBRDF(lightDir, normalWorld, viewDir, baseColor, roughness, specMap, metallic, lightColor) * attenuation;
        }
        else if (lightType == HRL_DirectionalLight)
        {
            vec3 lightDir = normalize(-lights[i].rotation);
            lighting += evalBRDF(lightDir, normalWorld, viewDir, baseColor, roughness, specMap, metallic, lightColor);
        }
        else if (lightType == HRL_SpotLight)
        {
            vec3 toLight = lights[i].position - fragPos;
            float dist = length(toLight);
            vec3 lightDir = (dist > 0.0001) ? toLight / dist : vec3(0.0, 0.0, 1.0);
            float cosTheta = dot(lightDir, normalize(-lights[i].rotation));
            float spotFactor = smoothstep(lights[i].outerCutoff, lights[i].innerCutoff, cosTheta);
            if (spotFactor <= 0.0)
                continue;
            float attenuation = 1.0 / (1.0 + lights[i].attenuation * dist * dist);
            lighting += evalBRDF(lightDir, normalWorld, viewDir, baseColor, roughness, specMap, metallic, lightColor) * attenuation * spotFactor;
        }
    }

    float ao = clamp(texture(T_AO, uv).r, 0.0, 1.0);
    vec3 litResult = lighting * ao;

    FragColor = applyFog(vec4(litResult, alpha), length(CamPos - fragPos));
}
)GLSL";

// ---------------------------------------------------------------------------
// Widgets / text
// ---------------------------------------------------------------------------

static const char* const kUIVertex = R"GLSL(#version 300 es
precision highp float;

layout(location = 0) in vec2 apos;
layout(location = 1) in vec2 auv;

uniform mat4 projection;

out vec2 uv;

void main()
{
    gl_Position = projection * vec4(apos, 0.0, 1.0);
    uv = auv;
}
)GLSL";

static const char* const kUIFragment =
	HRL_GLES3_FRAGMENT_HEADER
	R"GLSL(
in vec2 uv;

uniform vec4 uTintColor;
uniform sampler2D uTexture;
uniform int uSDFText;

out vec4 FragColor;

void main()
{
    vec4 texel = texture(uTexture, uv);

    if (uSDFText != 0)
    {
        // SDF edge is encoded at 0.5. fwidth keeps the transition stable
        // when the widget is enlarged, reduced, or resized.
        float dist = texel.r;
        float smoothing = max(fwidth(dist), 0.001);
        float alpha = smoothstep(0.5 - smoothing, 0.5 + smoothing, dist);
        FragColor = vec4(uTintColor.rgb, uTintColor.a * alpha);
    }
    else
    {
        FragColor = texel * uTintColor;
    }
}
)GLSL";

// ---------------------------------------------------------------------------
// VFX billboards
// ---------------------------------------------------------------------------

static const char* const kVFXVertex = R"GLSL(#version 300 es
precision highp float;
precision highp int;

layout(location = 0) in vec3 aPosition;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in mat4 aInstanceModel;
layout(location = 7) in vec4 aInstanceColor;

uniform mat4 projection;
uniform mat4 view;

out vec2 uv;
out vec4 tint;

void main()
{
    uv = aTexCoord;
    tint = aInstanceColor;
    gl_Position = projection * view * aInstanceModel * vec4(aPosition, 1.0);
}
)GLSL";

static const char* const kVFXFragment =
	HRL_GLES3_FRAGMENT_HEADER
	R"GLSL(
in vec2 uv;
in vec4 tint;

uniform sampler2D uTexture;
uniform int uUseTexture;

out vec4 FragColor;

void main()
{
    vec4 texel = (uUseTexture != 0) ? texture(uTexture, uv) : vec4(1.0);
    vec4 result = texel * tint;
    if (result.a <= 0.001) discard;
    FragColor = result;
}
)GLSL";

// ---------------------------------------------------------------------------
// Debug lines / triangles
// ---------------------------------------------------------------------------

static const char* const kDebugVertex = R"GLSL(#version 300 es
precision highp float;

layout(location = 0) in vec3 apos;
layout(location = 1) in vec3 acolor;

uniform mat4 projection;
uniform mat4 view;

out vec3 vcolor;

void main()
{
    gl_Position = projection * view * vec4(apos, 1.0);
    vcolor = acolor;
}
)GLSL";

static const char* const kDebugFragment =
	HRL_GLES3_FRAGMENT_HEADER
	R"GLSL(
in vec3 vcolor;
out vec4 FragColor;

void main()
{
    FragColor = vec4(vcolor, 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Sky sphere
// ---------------------------------------------------------------------------

static const char* const kSkyVertex = R"GLSL(#version 300 es
precision highp float;

layout(location = 0) in vec3 aPosition;

uniform mat4 projection;
uniform mat4 view;
uniform mat4 rotation;

out vec3 skyDirection;

void main()
{
    skyDirection = normalize(mat3(rotation) * aPosition);
    gl_Position = projection * view * rotation * vec4(aPosition, 1.0);
}
)GLSL";

static const char* const kSkyFragment =
	HRL_GLES3_FRAGMENT_HEADER
	R"GLSL(
layout(location = 0) out vec4 FragColor;

in vec3 skyDirection;

uniform vec3 SkyTopColor;
uniform vec3 SkyHorizonColor;
uniform vec3 SkyBottomColor;
uniform sampler2D SkyTexture;
uniform int SkyTextureEnabled;

void main()
{
    vec3 direction = normalize(skyDirection);
    if (SkyTextureEnabled != 0)
    {
        float longitude = atan(direction.z, direction.x);
        float latitude = asin(clamp(direction.y, -1.0, 1.0));
        vec2 skyUV = vec2(
            longitude / (2.0 * 3.14159265359) + 0.5,
            clamp(0.5 + latitude / 3.14159265359, 0.001, 0.999));
        FragColor = texture(SkyTexture, skyUV);
    }
    else
    {
        float h = clamp(direction.y, -1.0, 1.0);
        vec3 color = (h >= 0.0)
            ? mix(SkyHorizonColor, SkyTopColor, smoothstep(0.0, 1.0, h))
            : mix(SkyBottomColor, SkyHorizonColor, smoothstep(-1.0, 0.0, h));
        FragColor = vec4(color, 1.0);
    }
}
)GLSL";

} // namespace gles3_shaders

#endif
