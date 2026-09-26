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
    float shadowStrength;

    mat4 shadowMatrix;
    vec4 shadowParams; // x=bias, y=far plane, z=slot, w=type (1=2D, 2=cube)
};

layout(std140) uniform LightBlock
{
    Light lights[MAX_LIGHTS];
};

// World-space DDGI probe volume. Six directional radiance lobes are stored
// as plain vec4 values across three small std140 blocks. This deliberately
// avoids integer bit-packing so the shader remains conservative on older
// OpenGL 3.3 drivers.
#define DDGI_MAX_PROBES 384
#define DDGI_LOBES 6
#define DDGI_LOBES_PER_BLOCK 2
layout(std140) uniform DDGIBlock0
{
    vec4 DDGIProbeData0[DDGI_MAX_PROBES * DDGI_LOBES_PER_BLOCK];
};
layout(std140) uniform DDGIBlock1
{
    vec4 DDGIProbeData1[DDGI_MAX_PROBES * DDGI_LOBES_PER_BLOCK];
};
layout(std140) uniform DDGIBlock2
{
    vec4 DDGIProbeData2[DDGI_MAX_PROBES * DDGI_LOBES_PER_BLOCK];
};
uniform int DDGIEnabled;
uniform vec3 DDGIProbeOrigin;
uniform vec3 DDGIProbeSpacing;
uniform vec3 DDGIProbeResolution;
uniform float DDGIStrength;

vec4 DDGIGetLobeData(int probeIndex, int lobe)
{
    int localLobe = lobe % DDGI_LOBES_PER_BLOCK;
    int localIndex = probeIndex * DDGI_LOBES_PER_BLOCK + localLobe;
    if (lobe < 2)
        return DDGIProbeData0[localIndex];
    if (lobe < 4)
        return DDGIProbeData1[localIndex];
    return DDGIProbeData2[localIndex];
}

vec3 DDGIGetLobe(int probeIndex, int lobe)
{
    return DDGIGetLobeData(probeIndex, lobe).rgb;
}

vec3 DDGIProbeLobe(int x, int y, int z, int lobe)
{
    int nx = int(DDGIProbeResolution.x);
    int ny = int(DDGIProbeResolution.y);
    int index = (z * ny + y) * nx + x;
    if (index < 0 || index >= DDGI_MAX_PROBES)
        return vec3(0.0);
    return DDGIGetLobe(index, lobe);
}

float DDGIProbeValid(int x, int y, int z)
{
    int nx = int(DDGIProbeResolution.x);
    int ny = int(DDGIProbeResolution.y);
    int index = (z * ny + y) * nx + x;
    if (index < 0 || index >= DDGI_MAX_PROBES)
        return 0.0;
    return DDGIGetLobeData(index, 0).a > 0.5 ? 1.0 : 0.0;
}

vec3 DDGIProbeRadianceAt(int x, int y, int z, vec3 normalWorld)
{
    if (DDGIProbeValid(x, y, z) < 0.5)
        return vec3(0.0);

    vec3 n = normalize(normalWorld);
    float wx  = max(dot(n, vec3( 1.0, 0.0, 0.0)), 0.0);
    float wnx = max(dot(n, vec3(-1.0, 0.0, 0.0)), 0.0);
    float wy  = max(dot(n, vec3( 0.0, 1.0, 0.0)), 0.0);
    float wny = max(dot(n, vec3( 0.0,-1.0, 0.0)), 0.0);
    float wz  = max(dot(n, vec3( 0.0, 0.0, 1.0)), 0.0);
    float wnz = max(dot(n, vec3( 0.0, 0.0,-1.0)), 0.0);
    float sumW = wx + wnx + wy + wny + wz + wnz;
    if (sumW <= 1e-5)
        return vec3(0.0);

    vec3 result = vec3(0.0);
    result += DDGIProbeLobe(x, y, z, 0) * wx;
    result += DDGIProbeLobe(x, y, z, 1) * wnx;
    result += DDGIProbeLobe(x, y, z, 2) * wy;
    result += DDGIProbeLobe(x, y, z, 3) * wny;
    result += DDGIProbeLobe(x, y, z, 4) * wz;
    result += DDGIProbeLobe(x, y, z, 5) * wnz;
    return result / sumW;
}

vec3 SampleDDGI(vec3 worldPos, vec3 normalWorld)
{
    if (DDGIEnabled == 0 || DDGIStrength <= 0.0)
        return vec3(0.0);

    vec3 grid = (worldPos - DDGIProbeOrigin) / max(DDGIProbeSpacing, vec3(1e-4));
    vec3 maxGrid = max(DDGIProbeResolution - vec3(1.0), vec3(0.0));

    if (any(lessThan(grid, vec3(0.0))) || any(greaterThan(grid, maxGrid)))
        return vec3(0.0);

    vec3 base = floor(grid);
    vec3 f = grid - base;
    int x0 = int(base.x);
    int y0 = int(base.y);
    int z0 = int(base.z);
    int x1 = min(x0 + 1, int(maxGrid.x));
    int y1 = min(y0 + 1, int(maxGrid.y));
    int z1 = min(z0 + 1, int(maxGrid.z));

    float wx0 = 1.0 - f.x;
    float wx1 = f.x;
    float wy0 = 1.0 - f.y;
    float wy1 = f.y;
    float wz0 = 1.0 - f.z;
    float wz1 = f.z;

    vec3 result = vec3(0.0);
    float totalWeight = 0.0;

    for (int ix = 0; ix < 2; ++ix)
        for (int iy = 0; iy < 2; ++iy)
            for (int iz = 0; iz < 2; ++iz)
            {
                int px = (ix == 0) ? x0 : x1;
                int py = (iy == 0) ? y0 : y1;
                int pz = (iz == 0) ? z0 : z1;
                float wx = (ix == 0) ? wx0 : wx1;
                float wy = (iy == 0) ? wy0 : wy1;
                float wz = (iz == 0) ? wz0 : wz1;
                float spatial = wx * wy * wz;
                if (spatial <= 0.0)
                    continue;

                float valid = DDGIProbeValid(px, py, pz);
                if (valid <= 0.0)
                    continue;

                result += DDGIProbeRadianceAt(px, py, pz, normalWorld) * spatial;
                totalWeight += spatial;
            }

    return totalWeight > 1e-5 ? result / totalWeight : vec3(0.0);
}

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

    vec3 albedo = texture(T_Albedo, uv).rgb;
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

    // Diffuse world-space GI is evaluated directly in the material shader.
    // This keeps GI independent of the camera and avoids any framebuffer copy or
    // depth reconstruction in the GI path. DDGIProbeRadiance already represents
    // outgoing diffuse radiance, so the surface albedo is applied exactly once.
    if (DDGIEnabled != 0)
    {
        vec3 diffuseAlbedo = albedo * TintColor * (1.0 - metallic);
        litResult += SampleDDGI(fragPos, normalWorld) * diffuseAlbedo * DDGIStrength;
    }
    if (EnvironmentEnabled != 0 && EnvironmentStrength > 0.0)
    {
        vec3 reflectionDir = reflect(-viewDir, normalWorld);
        vec3 environmentColor = texture(EnvironmentMap, EquirectangularUV(reflectionDir)).rgb;
        float reflectionAmount = clamp(EnvironmentStrength * mix(0.08, 1.0, metallic), 0.0, 1.0);
        litResult = mix(litResult, litResult + environmentColor, reflectionAmount);
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
        GIAlbedoBuffer = vec4(albedo * TintColor, alpha);
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
