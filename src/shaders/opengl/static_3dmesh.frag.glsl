#version 330 core

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 BrightColor;
layout(location = 2) out vec4 ColorPickingBuffer;

#define MAX_LIGHTS 32
#define MAX_SHADOW_SLOTS 4

#define HRL_PointLight       (uint(0x0011))
#define HRL_DirectionalLight (uint(0x0012))
#define HRL_SpotLight        (uint(0x0013))

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
uniform vec3 CamPos;
uniform float BrightThreshold;
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
    float padding4;

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

vec3 evalBRDF(vec3 lightDir,
              vec3 normalWorld, vec3 viewDir,
              vec3 F0, float shininess, float specMap,
              float metallic, vec3 lightColor)
{
    float NdotL = max(dot(normalWorld, lightDir), 0.0);
    vec3 kD = (1.0 - metallic) * NdotL * lightColor;

    vec3 halfDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(normalWorld, halfDir), 0.0);
    float spec = pow(NdotH, shininess) * specMap;
    vec3 kS = spec * F0 * lightColor;

    return kD + kS;
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

float ShadowCompareCube(int slot, vec3 direction, float referenceDepth)
{
    vec3 d = normalize(direction);
    const vec3 offsets[5] = vec3[](
        vec3(0.0),
        vec3(0.015, 0.0, 0.0),
        vec3(-0.015, 0.0, 0.0),
        vec3(0.0, 0.015, 0.0),
        vec3(0.0, -0.015, 0.0)
    );

    float visible = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        vec3 sampleDir = normalize(d + offsets[i]);
        float storedDepth;
        if (slot == 0) storedDepth = texture(ShadowMapCube_0, sampleDir).r;
        else if (slot == 1) storedDepth = texture(ShadowMapCube_1, sampleDir).r;
        else if (slot == 2) storedDepth = texture(ShadowMapCube_2, sampleDir).r;
        else storedDepth = texture(ShadowMapCube_3, sampleDir).r;
        visible += storedDepth >= referenceDepth ? 1.0 : 0.0;
    }
    return visible / 5.0;
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
        if (slot == 0) return ShadowCompare2D_0(coord);
        if (slot == 1) return ShadowCompare2D_1(coord);
        if (slot == 2) return ShadowCompare2D_2(coord);
        return ShadowCompare2D_3(coord);
    }

    float farPlane = max(light.shadowParams.y, 0.001);
    vec3 toLight = worldPos - light.position;
    float referenceDepth = length(toLight) / farPlane;
    referenceDepth = max(referenceDepth - bias, 0.0);
    return ShadowCompareCube(slot, toLight, referenceDepth);
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
    vec3 albedo = texture(T_Albedo, uv).rgb;
    float metallic = texture(T_Metallic, uv).r;
    float roughness = clamp(texture(T_Roughness, uv).r, 0.05, 1.0);
    float specMap = texture(T_Specular, uv).r;
    vec3 normalTex = texture(T_Normal, uv).rgb * 2.0 - 1.0;

    vec3 N = normalize(worldNormal);
    vec3 T = normalize(worldTangent - N * dot(N, worldTangent));
    vec3 B = normalize(cross(N, T));
    if (dot(B, worldBitangent) < 0.0)
        B = -B;
    vec3 normalWorld = normalize(T * normalTex.x + B * normalTex.y + N * normalTex.z);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float shininess = pow(2.0, (1.0 - roughness) * 11.0);

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

        if (light.type == HRL_PointLight)
        {
            vec3 toLight = light.position - fragPos;
            float dist = length(toLight);
            lightDir = (dist > 0.0001) ? toLight / dist : vec3(0.0, 0.0, 1.0);
            float attenuation = 1.0 / (1.0 + light.attenuation * dist * dist);
            shadow = ComputeShadow(light, fragPos, normalWorld, lightDir);
            lighting += evalBRDF(lightDir, normalWorld, viewDir, F0, shininess, specMap, metallic, lightColor) * attenuation * shadow;
        }
        else if (light.type == HRL_DirectionalLight)
        {
            lightDir = normalize(-light.rotation);
            shadow = ComputeShadow(light, fragPos, normalWorld, lightDir);
            lighting += evalBRDF(lightDir, normalWorld, viewDir, F0, shininess, specMap, metallic, lightColor) * shadow;
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
            lighting += evalBRDF(lightDir, normalWorld, viewDir, F0, shininess, specMap, metallic, lightColor) * attenuation * spotFactor * shadow;
        }
    }

    vec3 litResult = lighting * albedo * TintColor;
    if (EnvironmentEnabled != 0 && EnvironmentStrength > 0.0)
    {
        vec3 reflectionDir = reflect(-viewDir, normalWorld);
        vec3 environmentColor = texture(EnvironmentMap, EquirectangularUV(reflectionDir)).rgb;
        float reflectionAmount = clamp(EnvironmentStrength * mix(0.08, 1.0, metallic), 0.0, 1.0);
        litResult = mix(litResult, litResult + environmentColor, reflectionAmount);
    }

    float alpha = texture(T_Albedo, uv).a * texture(T_Alpha, uv).r;

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
