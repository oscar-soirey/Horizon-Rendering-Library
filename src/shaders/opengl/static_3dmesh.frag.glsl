#version 330 core

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 BrightColor;
layout(location = 2) out vec4 ColorPickingBuffer;
// GI G-buffer: unlit linear base color and final world-space shading normal.
layout(location = 3) out vec4 GIAlbedoBuffer;
layout(location = 4) out vec4 GINormalBuffer;

#define MAX_LIGHTS 32
#define MAX_SHADOW_SLOTS 4

#define HRL_PointLight       (uint(0x0011))
#define HRL_DirectionalLight (uint(0x0012))
#define HRL_SpotLight        (uint(0x0013))
#define HRL_SkyLight         (uint(0x0014))

const int FOG_LINEAR = 0x0090;
const int FOG_EXP    = 0x0091;
const int FOG_EXP2   = 0x0092;

const int HRL_DEBUG_VIEW_NONE      = 0x0060;
const int HRL_DEBUG_VIEW_UNLIT     = 0x0061;
const int HRL_DEBUG_VIEW_NORMAL    = 0x0062;
const int HRL_DEBUG_VIEW_LIGHTS    = 0x0063;
const int HRL_DEBUG_VIEW_WIREFRAME = 0x0064;
const int HRL_DEBUG_VIEW_LOD       = 0x0065;

in vec3 fragPos;
in vec2 uv;
in vec3 worldNormal;
in vec3 worldTangent;
in vec3 worldBitangent;
flat in uint sprite_id;

uniform sampler2D T_Albedo;
uniform sampler2D T_Normal;
uniform sampler2D T_Specular;
uniform sampler2D T_Roughness;
uniform sampler2D T_Metallic;
uniform sampler2D T_AO;
uniform sampler2D T_Alpha;

uniform sampler2D ShadowMap2D_0;
uniform sampler2D ShadowMap2D_1;
uniform sampler2D ShadowMap2D_2;
uniform sampler2D ShadowMap2D_3;
uniform samplerCube ShadowMapCube_0;
uniform samplerCube ShadowMapCube_1;
uniform samplerCube ShadowMapCube_2;
uniform samplerCube ShadowMapCube_3;

uniform sampler2D EnvironmentMap;
uniform int EnvironmentEnabled;
uniform float EnvironmentStrength;

uniform vec3 TintColor;
uniform float BaseColorAlpha;
uniform float RoughnessValue;
uniform float MetallicValue;
uniform float SpecularValue;
uniform float OpacityValue;
uniform int ss_displacement_enabled;
uniform int RoughnessUseValue;
uniform int MetallicUseValue;
uniform int SpecularUseValue;
uniform int OpacityUseValue;
uniform int RoughnessInvert;
uniform int AlphaInvert;
uniform int TwoSided;
uniform vec3 CamPos;
uniform float BrightThreshold;

// ─────────────────────────────────────────────────────────────────────────────
//  Lumière des voxels émissifs (HRL_SetVoxelEmissiveLighting) : champ de
//  lumière calculé une fois par viewport, une seule lecture de texture quel
//  que soit le nombre d'émetteurs.
// ─────────────────────────────────────────────────────────────────────────────

uniform sampler2D VoxelLightField;
uniform int   VoxelLightEnabled;
uniform vec4  VoxelLightRegion;   // xy : min monde, zw : 1 / taille monde
uniform float VoxelLightFalloff;  // atténuation hors du plan des voxels (Z)

vec3 SampleVoxelLight(vec3 worldPos)
{
    if (VoxelLightEnabled == 0)
        return vec3(0.0);
    vec2 luv = (worldPos.xy - VoxelLightRegion.xy) * VoxelLightRegion.zw;
    if (luv.x < 0.0 || luv.y < 0.0 || luv.x > 1.0 || luv.y > 1.0)
        return vec3(0.0);
    float z = worldPos.z / max(VoxelLightFalloff, 1e-4);
    return max(texture(VoxelLightField, luv).rgb, vec3(0.0)) / (1.0 + z * z);
}
uniform int DebugView;
uniform vec3 DebugLODColor;

uniform int FogEnabled;
uniform int FogMode;
uniform vec4 FogColor;
uniform float FogStart;
uniform float FogEnd;
uniform float FogDensity;

struct Light
{
    uint type;
    float intensity;
    float attenuation;
    float innerCutoff;

    vec3 position;
    float outerCutoff;

    vec3 rotation;
    float padding3;

    vec3 color;
    float shadowStrength;

    mat4 shadowMatrix;
    vec4 shadowParams; // x=bias, y=far plane, z=slot, w=type (1=2D, 2=cube)
};

layout(std140) uniform LightBlock
{
    Light lights[MAX_LIGHTS];
};

float computeFogFactor(float dist)
{
    float f;
    if      (FogMode == FOG_LINEAR) f = (FogEnd - dist) / (FogEnd - FogStart);
    else if (FogMode == FOG_EXP)    f = exp(-FogDensity * dist);
    else                            f = exp(-FogDensity * FogDensity * dist * dist);
    return clamp(f, 0.0, 1.0);
}

vec4 applyFog(vec4 litColor, float dist)
{
    if (FogEnabled == 0)
        return litColor;
    return mix(FogColor, litColor, computeFogFactor(dist));
}

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
    // UE4/Disney-style remapping keeps the Smith term stable across the
    // complete roughness range and avoids the old hard specular spikes.
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-6);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX(NdotV, roughness) *
           GeometrySchlickGGX(NdotL, roughness);
}

vec3 evalBRDF(vec3 lightDir,
              vec3 normalWorld, vec3 viewDir,
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
    float G = GeometrySmith(NdotV, NdotL, roughness);

    vec3 specular = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-5);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 diffuse = kD * baseColor / PI;

    return (diffuse + specular) * lightColor * NdotL;
}

float ShadowCompare2D_0(vec3 coord)
{
    vec2 texel = 1.0 / vec2(textureSize(ShadowMap2D_0, 0));
    float result = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            result += texture(ShadowMap2D_0, coord.xy + vec2(x, y) * texel).r >= coord.z ? 1.0 : 0.0;
    return result / 9.0;
}

float ShadowCompare2D_1(vec3 coord)
{
    vec2 texel = 1.0 / vec2(textureSize(ShadowMap2D_1, 0));
    float result = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            result += texture(ShadowMap2D_1, coord.xy + vec2(x, y) * texel).r >= coord.z ? 1.0 : 0.0;
    return result / 9.0;
}

float ShadowCompare2D_2(vec3 coord)
{
    vec2 texel = 1.0 / vec2(textureSize(ShadowMap2D_2, 0));
    float result = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            result += texture(ShadowMap2D_2, coord.xy + vec2(x, y) * texel).r >= coord.z ? 1.0 : 0.0;
    return result / 9.0;
}

float ShadowCompare2D_3(vec3 coord)
{
    vec2 texel = 1.0 / vec2(textureSize(ShadowMap2D_3, 0));
    float result = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            result += texture(ShadowMap2D_3, coord.xy + vec2(x, y) * texel).r >= coord.z ? 1.0 : 0.0;
    return result / 9.0;
}

float CubeShadowStoredDepth(int slot, vec3 direction)
{
    if (slot == 0) return texture(ShadowMapCube_0, direction).r;
    if (slot == 1) return texture(ShadowMapCube_1, direction).r;
    if (slot == 2) return texture(ShadowMapCube_2, direction).r;
    return texture(ShadowMapCube_3, direction).r;
}

int CubeShadowResolution(int slot)
{
    if (slot == 0) return textureSize(ShadowMapCube_0, 0).x;
    if (slot == 1) return textureSize(ShadowMapCube_1, 0).x;
    if (slot == 2) return textureSize(ShadowMapCube_2, 0).x;
    return textureSize(ShadowMapCube_3, 0).x;
}

float ShadowCompareCube(int slot, vec3 direction, float referenceDepth)
{
    vec3 d = normalize(direction);

    // The previous implementation used a fixed 0.015 direction offset. That is
    // enormous for a 1024px cube map and samples directions far enough apart to
    // create visible duplicate/ghosted silhouettes. Scale the kernel to the
    // actual cube-map texel size instead.
    const vec2 kernel[9] = vec2[](
        vec2( 0.0,  0.0),
        vec2( 1.0,  0.0), vec2(-1.0,  0.0),
        vec2( 0.0,  1.0), vec2( 0.0, -1.0),
        vec2( 0.7071,  0.7071), vec2(-0.7071,  0.7071),
        vec2( 0.7071, -0.7071), vec2(-0.7071, -0.7071)
    );

    vec3 referenceUp = abs(d.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(referenceUp, d));
    vec3 bitangent = normalize(cross(d, tangent));
    float angularRadius = 1.5 / float(max(CubeShadowResolution(slot), 1));

    float visible = 0.0;
    for (int i = 0; i < 9; ++i)
    {
        vec2 offset = kernel[i] * angularRadius;
        vec3 sampleDir = normalize(d + tangent * offset.x + bitangent * offset.y);
        float storedDepth = CubeShadowStoredDepth(slot, sampleDir);
        visible += storedDepth >= referenceDepth ? 1.0 : 0.0;
    }
    return visible / 9.0;
}

float ApplyShadowStrength(Light light, float shadow)
{
    float strength = clamp(light.shadowStrength, 0.0, 1.0);
    return mix(1.0, shadow, strength);
}

float ComputeShadow(Light light, vec3 worldPos, vec3 normalWorld, vec3 lightDir)
{
    if (light.shadowParams.z < -0.5)
        return 1.0;

    float bias = light.shadowParams.x * (1.0 - max(dot(normalWorld, lightDir), 0.0));
    bias = max(bias, light.shadowParams.x * 0.25);
    int slot = int(light.shadowParams.z + 0.5);

    if (light.shadowParams.w == 1.0)
    {
        vec4 shadowPos = light.shadowMatrix * vec4(worldPos, 1.0);
        if (shadowPos.w <= 0.0)
            return 1.0;

        vec3 coord = shadowPos.xyz / shadowPos.w;
        if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0 || coord.z > 1.0)
            return 1.0;

        coord.z -= bias;
        float rawShadow;
        if (slot == 0) rawShadow = ShadowCompare2D_0(coord);
        else if (slot == 1) rawShadow = ShadowCompare2D_1(coord);
        else if (slot == 2) rawShadow = ShadowCompare2D_2(coord);
        else rawShadow = ShadowCompare2D_3(coord);
        return ApplyShadowStrength(light, rawShadow);
    }

    float farPlane = max(light.shadowParams.y, 0.001);
    vec3 toLight = worldPos - light.position;
    float referenceDepth = length(toLight) / farPlane;
    referenceDepth = max(referenceDepth - bias, 0.0);
    return ApplyShadowStrength(light, ShadowCompareCube(slot, toLight, referenceDepth));
}

vec2 EquirectangularUV(vec3 direction)
{
    direction = normalize(direction);
    float longitude = atan(direction.z, direction.x);
    float latitude = asin(clamp(direction.y, -1.0, 1.0));
    return vec2(
        longitude / (2.0 * 3.14159265359) + 0.5,
        clamp(0.5 + latitude / 3.14159265359, 0.001, 0.999)
    );
}

void main()
{
    // Default to "no GI surface". Opaque mesh paths overwrite these below.
    GIAlbedoBuffer = vec4(0.0);
    GINormalBuffer = vec4(0.0);

    // Color textures are authored in sRGB space. The lighting pipeline works
    // in linear radiometric space, so decode the base color before evaluating
    // the BRDF. Non-color maps (normal/roughness/metal/AO) stay untouched.
    vec3 albedoSRGB = texture(T_Albedo, uv).rgb;
    vec3 albedo = pow(max(albedoSRGB, vec3(0.0)), vec3(2.2));
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
        Light light = lights[i];
        if (light.intensity <= 0.0)
            continue;

        vec3 lightColor = light.color * light.intensity;
        float shadow = 1.0;
        vec3 lightDir;

        if (light.type == HRL_SkyLight)
        {
            // Sky lights are low-frequency ambient irradiance. Keep them
            // diffuse-only here; specular environment reflection is handled
            // separately below so metallic surfaces do not receive diffuse
            // energy twice.
            lighting += baseColor * (1.0 - metallic) * lightColor;
            continue;
        }

        if (light.type == HRL_PointLight)
        {
            vec3 toLight = light.position - fragPos;
            float dist = length(toLight);
            lightDir = (dist > 0.0001) ? toLight / dist : vec3(0.0, 0.0, 1.0);
            float attenuation = 1.0 / (1.0 + light.attenuation * dist * dist);
            shadow = ComputeShadow(light, fragPos, normalWorld, lightDir);
            lighting += evalBRDF(lightDir, normalWorld, viewDir, baseColor, roughness, specMap, metallic, lightColor) * attenuation * shadow;
        }
        else if (light.type == HRL_DirectionalLight)
        {
            lightDir = normalize(-light.rotation);
            shadow = ComputeShadow(light, fragPos, normalWorld, lightDir);
            lighting += evalBRDF(lightDir, normalWorld, viewDir, baseColor, roughness, specMap, metallic, lightColor) * shadow;
        }
        else if (light.type == HRL_SpotLight)
        {
            vec3 toLight = light.position - fragPos;
            float dist = length(toLight);
            lightDir = (dist > 0.0001) ? toLight / dist : vec3(0.0, 0.0, 1.0);
            float cosTheta = dot(lightDir, normalize(-light.rotation));
            float spotFactor = smoothstep(light.outerCutoff, light.innerCutoff, cosTheta);
            if (spotFactor <= 0.0)
                continue;
            float attenuation = 1.0 / (1.0 + light.attenuation * dist * dist);
            shadow = ComputeShadow(light, fragPos, normalWorld, lightDir);
            lighting += evalBRDF(lightDir, normalWorld, viewDir, baseColor, roughness, specMap, metallic, lightColor) * attenuation * spotFactor * shadow;
        }
    }

    // Emissive voxels : diffuse light of every emitter around (light field).
    lighting += SampleVoxelLight(fragPos) * baseColor * (1.0 - metallic);

    float ao = clamp(texture(T_AO, uv).r, 0.0, 1.0);
    vec3 litResult = lighting * ao;

    // Global illumination is added by a screen pass from the GI G-buffer
    // (diffuse albedo + normal written below), see backend/opengl33/gi.cpp.
    if (EnvironmentEnabled != 0 && EnvironmentStrength > 0.0)
    {
        vec3 reflectionDir = reflect(-viewDir, normalWorld);
        vec3 environmentColorSRGB = texture(EnvironmentMap, EquirectangularUV(reflectionDir)).rgb;
        vec3 environmentColor = pow(max(environmentColorSRGB, vec3(0.0)), vec3(2.2));
        vec3 F0Env = mix(vec3(0.04), baseColor, metallic);
        float NdotV = max(dot(normalWorld, viewDir), 0.0);
        vec3 envFresnel = FresnelSchlick(NdotV, F0Env);
        float roughReflection = 1.0 - roughness * roughness;
        vec3 environmentSpecular = environmentColor * envFresnel * roughReflection * EnvironmentStrength;
        litResult += environmentSpecular;
    }

    float alphaSample = texture(T_Alpha, uv).r;
    if (AlphaInvert != 0)
        alphaSample = 1.0 - alphaSample;
    float alpha = texture(T_Albedo, uv).a * BaseColorAlpha *
        (OpacityUseValue != 0 ? OpacityValue : alphaSample);

    if (DebugView == HRL_DEBUG_VIEW_WIREFRAME)
    {
        FragColor = vec4(1.0);
        BrightColor = vec4(0.0);
    }
    else if (DebugView == HRL_DEBUG_VIEW_LOD)
    {
        FragColor = vec4(DebugLODColor, 1.0);
        BrightColor = vec4(0.0);
    }
    else
    {
        if (alpha <= 0.001)
            discard;

        // These buffers intentionally contain material/geometry information,
        // not direct lighting. GI can therefore reconstruct diffuse transport
        // without guessing albedo or normals from Scene Color/depth.
        GIAlbedoBuffer = vec4(baseColor * (1.0 - metallic), alpha);
        GINormalBuffer = vec4(normalize(normalWorld), alpha);

        if (DebugView == HRL_DEBUG_VIEW_UNLIT)
        {
            FragColor = vec4(albedo * TintColor, alpha);
            BrightColor = vec4(0.0);
        }
        else if (DebugView == HRL_DEBUG_VIEW_NORMAL)
        {
            FragColor = vec4(normalWorld * 0.5 + 0.5, alpha);
            BrightColor = vec4(0.0);
        }
        else if (DebugView == HRL_DEBUG_VIEW_LIGHTS)
        {
            FragColor = vec4(clamp(lighting, 0.0, 1.0), alpha);
            BrightColor = vec4(0.0);
        }
        else
        {
            float dist = length(CamPos - fragPos);
            vec4 color = applyFog(vec4(litResult, alpha), dist);
            FragColor = color;
            float brightness = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
            BrightColor = (brightness > BrightThreshold) ? color : vec4(0.0, 0.0, 0.0, 1.0);
        }
    }

    ColorPickingBuffer = vec4(
        float((sprite_id >> 16u) & 255u) / 255.0,
        float((sprite_id >> 8u) & 255u) / 255.0,
        float(sprite_id & 255u) / 255.0,
        1.0
    );
}
