// Built-in GLSL 3.30 sources shared by the OpenGL 3.3 and Vulkan backends.
// The Vulkan backend translates these exact sources at runtime, so both
// backends always render with the same shader code.
#ifndef HRL_BUILTIN_GLSL_H
#define HRL_BUILTIN_GLSL_H

inline constexpr const char* kVoxel2DVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aLayer;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec3 fragPos;
out vec3 worldNormal;
out vec4 voxelColor;
flat out int voxelLayer;

void main()
{
    vec4 worldPos = model * vec4(aPosition, 1.0);
    fragPos = worldPos.xyz;
    worldNormal = normalize(mat3(model) * aNormal);
    voxelColor = aColor;
    voxelLayer = int(floor(aLayer + 0.5));
    gl_Position = projection * view * worldPos;
}
)GLSL";

inline constexpr const char* kVoxel2DFragmentShader = R"GLSL(#version 330 core
layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 BrightColor;
layout(location = 2) out vec4 ColorPickingBuffer;
layout(location = 3) out vec4 GIAlbedoBuffer;
layout(location = 4) out vec4 GINormalBuffer;

#define MAX_LIGHTS 256
#define MAX_SHADOW_SLOTS 4

#define HRL_PointLight       uint(0x0011)
#define HRL_DirectionalLight uint(0x0012)
#define HRL_SpotLight        uint(0x0013)
#define HRL_SkyLight         uint(0x0014)

const int HRL_DEBUG_VIEW_NONE      = 0x0060;
const int HRL_DEBUG_VIEW_UNLIT     = 0x0061;
const int HRL_DEBUG_VIEW_NORMAL    = 0x0062;
const int HRL_DEBUG_VIEW_LIGHTS    = 0x0063;

const int FOG_LINEAR = 0x0090;
const int FOG_EXP    = 0x0091;
const int FOG_EXP2   = 0x0092;

in vec3 fragPos;
in vec3 worldNormal;
in vec4 voxelColor;
flat in int voxelLayer;

// Textures of the voxel types (HRL_SetVoxelTypeTexture) : one layer per
// textured type. VoxelTexTiles[layer / 4][layer % 4] : world size of one copy.
uniform sampler2DArray VoxelTextures;
uniform int VoxelTexturesEnabled;
uniform vec4 VoxelTexTiles[64];

// Light of the emissive voxels (HRL_SetVoxelEmissiveLighting) : GPU light
// field built once per viewport, whatever the number of emitters.
uniform sampler2D VoxelLightField;
uniform int VoxelLightEnabled;
uniform vec4 VoxelLightRegion;   // xy : world min, zw : 1 / world size
uniform float VoxelLightFalloff;

uniform vec3 CamPos;
uniform float BrightThreshold;
uniform float VoxelSize;
uniform int VoxelEmissionPass;
uniform int VoxelLightCount;
uniform int VoxelShadowLightCount;
uniform int DebugView;
uniform int FogEnabled;
uniform int FogMode;
uniform vec4 FogColor;
uniform float FogStart;
uniform float FogEnd;
uniform float FogDensity;

uniform sampler2D ShadowMap2D_0;
uniform sampler2D ShadowMap2D_1;
uniform sampler2D ShadowMap2D_2;
uniform sampler2D ShadowMap2D_3;
uniform samplerCube ShadowMapCube_0;
uniform samplerCube ShadowMapCube_1;
uniform samplerCube ShadowMapCube_2;
uniform samplerCube ShadowMapCube_3;

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
    if (FogMode == FOG_LINEAR)
        f = (FogEnd - dist) / max(FogEnd - FogStart, 1e-6);
    else if (FogMode == FOG_EXP)
        f = exp(-FogDensity * dist);
    else
        f = exp(-FogDensity * FogDensity * dist * dist);
    return clamp(f, 0.0, 1.0);
}

vec4 applyFog(vec4 color, float dist)
{
    if (FogEnabled == 0)
        return color;
    return mix(FogColor, color, computeFogFactor(dist));
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
    return mix(1.0, shadow, clamp(light.shadowStrength, 0.0, 1.0));
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

vec3 EvaluateVoxelLight(Light light, vec3 albedo, vec3 N)
{
    if (light.intensity <= 0.0)
        return vec3(0.0);

    vec3 lightColor = max(light.color, vec3(0.0)) * light.intensity;
    vec3 L;
    float attenuation = 1.0;

    if (light.type == HRL_SkyLight)
    {
        // Sky light is an ambient diffuse term and intentionally does not use
        // the BRDF's directional cosine twice.
        return albedo * lightColor;
    }

    if (light.type == HRL_PointLight || light.type == HRL_SpotLight)
    {
        vec3 toLight = light.position - fragPos;

        // A 2D voxel surface has zero thickness. For a point light exactly on
        // the voxel plane, lift it slightly along the visible surface normal so
        // coplanar voxel lights still produce useful radial lighting.
        if (abs(toLight.z) < 1e-5)
            toLight.z = (N.z >= 0.0 ? 1.0 : -1.0) * max(VoxelSize * 0.5, 1e-3);

        float dist2 = dot(toLight, toLight);
        if (dist2 <= 1e-10)
            return vec3(0.0);

        float invDist = inversesqrt(dist2);
        float dist = dist2 * invDist;

        // Point lights, including voxel emitters, use only smooth attenuation
        // from the source position. There is deliberately no finite light radius
        // or range fade here: the emitter is a point located on the emitting voxel
        // and its influence falls off continuously with distance.
        attenuation = 1.0 / (1.0 + max(light.attenuation, 0.0) * dist2);

        // The CPU chunk culler uses the same conservative floor. Once a light
        // is inside a chunk, reject individual fragments whose maximum possible
        // RGB contribution is still below that floor. This avoids the expensive
        // normalization/BRDF/shadow path without changing visible contributions.
        const float contributionFloor = 1e-4;
        if (max(lightColor.r, max(lightColor.g, lightColor.b)) * attenuation <= contributionFloor)
            return vec3(0.0);

        L = toLight * invDist;

        if (light.type == HRL_SpotLight)
        {
            float cosTheta = dot(L, normalize(-light.rotation));
            float spotFactor = smoothstep(light.outerCutoff, light.innerCutoff, cosTheta);
            if (spotFactor <= 0.0)
                return vec3(0.0);
            attenuation *= spotFactor;
        }
    }
    else if (light.type == HRL_DirectionalLight)
    {
        L = normalize(-light.rotation);
    }
    else
    {
        return vec3(0.0);
    }

    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0)
        return vec3(0.0);

    float shadow = ComputeShadow(light, fragPos, N, L);
    return albedo * lightColor * NdotL * attenuation * shadow;
}
vec3 EvaluateVoxelLightNoShadow(Light light, vec3 albedo, vec3 N)
{
    if (light.intensity <= 0.0)
        return vec3(0.0);

    vec3 lightColor = max(light.color, vec3(0.0)) * light.intensity;
    vec3 L;
    float attenuation = 1.0;

    if (light.type == HRL_SkyLight)
    {
        // Sky light is an ambient diffuse term and intentionally does not use
        // the BRDF's directional cosine twice.
        return albedo * lightColor;
    }

    if (light.type == HRL_PointLight || light.type == HRL_SpotLight)
    {
        vec3 toLight = light.position - fragPos;

        // A 2D voxel surface has zero thickness. For a point light exactly on
        // the voxel plane, lift it slightly along the visible surface normal so
        // coplanar voxel lights still produce useful radial lighting.
        if (abs(toLight.z) < 1e-5)
            toLight.z = (N.z >= 0.0 ? 1.0 : -1.0) * max(VoxelSize * 0.5, 1e-3);

        float dist2 = dot(toLight, toLight);
        if (dist2 <= 1e-10)
            return vec3(0.0);

        float invDist = inversesqrt(dist2);
        float dist = dist2 * invDist;

        // Point lights, including voxel emitters, use only smooth attenuation
        // from the source position. There is deliberately no finite light radius
        // or range fade here: the emitter is a point located on the emitting voxel
        // and its influence falls off continuously with distance.
        attenuation = 1.0 / (1.0 + max(light.attenuation, 0.0) * dist2);

        // The CPU chunk culler uses the same conservative floor. Once a light
        // is inside a chunk, reject individual fragments whose maximum possible
        // RGB contribution is still below that floor. This avoids the expensive
        // normalization/BRDF/shadow path without changing visible contributions.
        const float contributionFloor = 1e-4;
        if (max(lightColor.r, max(lightColor.g, lightColor.b)) * attenuation <= contributionFloor)
            return vec3(0.0);

        L = toLight * invDist;

        if (light.type == HRL_SpotLight)
        {
            float cosTheta = dot(L, normalize(-light.rotation));
            float spotFactor = smoothstep(light.outerCutoff, light.innerCutoff, cosTheta);
            if (spotFactor <= 0.0)
                return vec3(0.0);
            attenuation *= spotFactor;
        }
    }
    else if (light.type == HRL_DirectionalLight)
    {
        L = normalize(-light.rotation);
    }
    else
    {
        return vec3(0.0);
    }

    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0)
        return vec3(0.0);

    return albedo * lightColor * NdotL * attenuation;
}

vec3 SampleVoxelLight(vec3 worldPos)
{
    if (VoxelLightEnabled == 0)
        return vec3(0.0);
    vec2 uv = (worldPos.xy - VoxelLightRegion.xy) * VoxelLightRegion.zw;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
        return vec3(0.0);
    float z = worldPos.z / max(VoxelLightFalloff, 1e-4);
    return max(texture(VoxelLightField, uv).rgb, vec3(0.0)) / (1.0 + z * z);
}

void main()
{
    vec4 texel = vec4(1.0);
    if (VoxelTexturesEnabled != 0 && voxelLayer >= 0)
    {
        float tile = max(VoxelTexTiles[voxelLayer >> 2][voxelLayer & 3], 1e-6);
        texel = texture(VoxelTextures, vec3(fragPos.xy / tile, float(voxelLayer)));
    }

    float alpha = clamp(voxelColor.a * texel.a, 0.0, 1.0);
    if (alpha <= 0.001)
        discard;

    vec3 baseColor = clamp(voxelColor.rgb * texel.rgb, vec3(0.0), vec3(1.0));

    // Emissive voxels are rendered in a separate sparse geometry pass. This
    // keeps the regular voxel vertices small and avoids storing an emissive
    // color for non-emissive voxels.
    if (VoxelEmissionPass != 0)
    {
        vec3 emission = max(voxelColor.rgb, vec3(0.0)) * texel.rgb;
        // Keep emissive color HDR all the way through the scene buffer.
        // Display compression is performed once, globally, after bloom.
        FragColor = vec4(emission, alpha);

        float brightness = max(emission.r, max(emission.g, emission.b));
        float bloomWidth = max(BrightThreshold * 0.75, 0.0001);
        float bloomWeight = smoothstep(BrightThreshold, BrightThreshold + bloomWidth, brightness);
        BrightColor = (bloomWeight > 0.0001)
            ? vec4(emission * bloomWeight, alpha)
            : vec4(0.0);
        return;
    }

    vec3 N = normalize(gl_FrontFacing ? worldNormal : -worldNormal);

    // Emissive voxels : every emitter around, one texture read.
    vec3 lighting = baseColor * SampleVoxelLight(fragPos);
    if (VoxelShadowLightCount == 0)
    {
        for (int i = 0; i < VoxelLightCount; ++i)
            lighting += EvaluateVoxelLightNoShadow(lights[i], baseColor, N);
    }
    else
    {
        for (int i = 0; i < VoxelLightCount; ++i)
            lighting += EvaluateVoxelLight(lights[i], baseColor, N);
    }

    // Base color is the material/albedo. Its visible contribution comes only
    // through the accumulated global lighting; do not add the unlit base on top,
    // otherwise every voxel stays fully bright even when no light reaches it.
    // The separately configured emissive color is rendered in its own sparse
    // pass and is also injected as a global light source.
    vec3 shadedColor = lighting;

    if (DebugView == HRL_DEBUG_VIEW_UNLIT)
        shadedColor = baseColor;
    else if (DebugView == HRL_DEBUG_VIEW_NORMAL)
        shadedColor = N * 0.5 + 0.5;
    else if (DebugView == HRL_DEBUG_VIEW_LIGHTS)
        shadedColor = lighting;
    else if (FogEnabled != 0)
        shadedColor = applyFog(vec4(shadedColor, alpha), length(CamPos - fragPos)).rgb;

    // Keep the complete HDR lighting result in the scene buffer. The post-process
    // applies one global display compression after bloom, so intermediate HDR
    // values are not locally clipped into flat halos around lights.
    FragColor = vec4(max(shadedColor, vec3(0.0)), alpha);

    float brightness = max(shadedColor.r, max(shadedColor.g, shadedColor.b));
    float bloomWidth = max(BrightThreshold * 0.75, 0.0001);
    float bloomWeight = smoothstep(BrightThreshold, BrightThreshold + bloomWidth, brightness);
    BrightColor = (bloomWeight > 0.0001)
        ? vec4(max(shadedColor, vec3(0.0)) * bloomWeight, alpha)
        : vec4(0.0);

    GIAlbedoBuffer = vec4(baseColor, alpha);
    GINormalBuffer = vec4(N, alpha);
    ColorPickingBuffer = vec4(0.0);
}
)GLSL";

inline constexpr const char* kVFXVertexShader = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in mat4 aInstanceModel;
layout(location = 7) in vec4 aInstanceColor;

uniform mat4 projection;
uniform mat4 view;
uniform mat4 model;
uniform int uInstanced;
uniform vec4 uTintColor;

out vec2 uv;
out vec4 tint;

void main()
{
    uv = aTexCoord;
    tint = (uInstanced != 0) ? aInstanceColor : uTintColor;
    mat4 m = (uInstanced != 0) ? aInstanceModel : model;
    gl_Position = projection * view * m * vec4(aPosition, 1.0);
}
)GLSL";

inline constexpr const char* kVFXFragmentShader = R"GLSL(
#version 330 core
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

inline constexpr const char* kSSDisplacementFragmentShader = R"GLSL(
#version 330 core
layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 BrightColor;
layout(location = 2) out vec4 ColorPickingBuffer;
layout(location = 3) out vec4 GIAlbedoBuffer;
layout(location = 4) out vec4 GINormalBuffer;

in vec2 uv;
flat in uint sprite_id;

uniform sampler2D uScene;
uniform sampler2D uDisplacementMap;
uniform vec2 uScreenSize;
uniform float uStrength;
uniform float uScale;
uniform float uOpacity;
uniform int ss_displacement_enabled;

void main()
{
    vec2 screenSize = max(uScreenSize, vec2(1.0));
    if (ss_displacement_enabled == 0)
    {
        vec4 baseScene = texture(uScene, gl_FragCoord.xy / screenSize);
        FragColor = baseScene;
        BrightColor = vec4(0.0);
        GIAlbedoBuffer = vec4(0.0);
        GINormalBuffer = vec4(0.0);
        ColorPickingBuffer = vec4(0.0);
        return;
    }
    vec2 baseUV = gl_FragCoord.xy / screenSize;
    vec2 mapUV = uv * max(abs(uScale), 0.0001);
    vec2 displacement = texture(uDisplacementMap, mapUV).rg * 2.0 - 1.0;
    vec2 displacedUV = clamp(baseUV + displacement * uStrength / screenSize, vec2(0.0), vec2(1.0));
    vec4 baseScene = texture(uScene, baseUV);
    vec4 displacedScene = texture(uScene, displacedUV);
    float opacity = clamp(uOpacity, 0.0, 1.0);
    FragColor = vec4(mix(baseScene.rgb, displacedScene.rgb, opacity), baseScene.a);
    BrightColor = vec4(0.0);
    GIAlbedoBuffer = vec4(0.0);
    GINormalBuffer = vec4(0.0);

    uint id = sprite_id;
    ColorPickingBuffer = vec4(
        float((id >> 16u) & 255u) / 255.0,
        float((id >> 8u) & 255u) / 255.0,
        float(id & 255u) / 255.0,
        1.0
    );
}
)GLSL";

inline constexpr const char* kAmbientOcclusionFragmentShader = R"GLSL(
#version 330 core
in vec2 uv;
out vec4 frag_color;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
uniform mat4 uInvViewProjection;
uniform vec3 uCameraPos;
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uScreenSize;
uniform float uRadius;
uniform float uBias;
uniform float uStrength;
uniform float uPower;
vec2 GlobalUV(vec2 localUV) { return uViewportOrigin + localUV * uViewportSize; }
vec3 ReconstructWorld(vec2 globalUV, float depth)
{
    vec2 localUV = (globalUV - uViewportOrigin) / max(uViewportSize, vec2(1e-6));
    vec4 clip = vec4(localUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProjection * clip;
    return abs(world.w) < 1e-6 ? vec3(0.0) : world.xyz / world.w;
}
void main()
{
    vec2 globalUV = GlobalUV(uv);
    vec4 base = texture(uScene, globalUV);
    float depth = texture(uDepth, globalUV).r;
    vec3 normal = texture(uNormal, globalUV).xyz;
    if (depth >= 0.99999 || dot(normal, normal) < 0.01 || uStrength <= 0.0 || uRadius <= 0.0) { frag_color = base; return; }
    normal = normalize(normal);
    vec3 worldPos = ReconstructWorld(globalUV, depth);
    const vec2 kernel[16] = vec2[](
        vec2(1.0,0.0), vec2(-1.0,0.0), vec2(0.0,1.0), vec2(0.0,-1.0),
        vec2(0.707,0.707), vec2(-0.707,0.707), vec2(0.707,-0.707), vec2(-0.707,-0.707),
        vec2(0.382,0.924), vec2(-0.382,0.924), vec2(0.382,-0.924), vec2(-0.382,-0.924),
        vec2(0.924,0.382), vec2(-0.924,0.382), vec2(0.924,-0.382), vec2(-0.924,-0.382));
    float distanceToCamera = max(length(worldPos - uCameraPos), 1.0);
    float radiusPixels = clamp(uRadius * 0.5 * min(uScreenSize.x, uScreenSize.y) / distanceToCamera, 1.0, 32.0);
    float occlusion = 0.0;
    for (int i=0; i<16; ++i)
    {
        float scale = mix(0.35, 1.0, float(i)/15.0);
        vec2 suv = globalUV + kernel[i] * (radiusPixels*scale) / uScreenSize;
        if (any(lessThan(suv,uViewportOrigin)) || any(greaterThan(suv,uViewportOrigin+uViewportSize))) continue;
        float sd = texture(uDepth,suv).r;
        if (sd >= 0.99999) continue;
        vec3 sp = ReconstructWorld(suv,sd);
        vec3 d = sp-worldPos;
        float dist = length(d);
        if (dist <= 1e-5 || dist > uRadius) continue;
        float farther = smoothstep(uBias, uBias + max(0.01,uRadius*0.08), length(sp - uCameraPos)-length(worldPos - uCameraPos));
        float facing = max(dot(normal, normalize(-d)),0.0);
        float rangeWeight = 1.0-smoothstep(0.0,uRadius,dist);
        occlusion += farther*facing*rangeWeight;
    }
    float ao = 1.0-uStrength*pow(clamp(occlusion/16.0,0.0,1.0),uPower);
    frag_color=vec4(base.rgb*ao,base.a);
}
)GLSL";

inline constexpr const char* kVolumetricCloudFragmentShader = R"GLSL(
#version 330 core
in vec2 uv;
out vec4 frag_color;

uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform mat4 uInvViewProjection;
uniform vec3 uCameraPos;
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uScreenSize;

uniform int uEnabled;
uniform float uCoverage;
uniform float uDensity;
uniform float uHeightMin;
uniform float uHeightMax;
uniform float uScale;
uniform float uDetail;
uniform vec2 uWind;
uniform float uWindSpeed;
uniform vec3 uCloudColor;
uniform vec3 uLightColor;
uniform float uLightAbsorption;
uniform float uLightIntensity;
uniform int uSteps;
uniform float uTime;
uniform float uMaxDistance;
uniform vec3 uSunDirection;

vec2 ToGlobalUV(vec2 localUV)
{
    return uViewportOrigin + localUV * uViewportSize;
}

vec3 Unproject(vec2 globalUV, float depth)
{
    vec2 localUV = (globalUV - uViewportOrigin) / max(uViewportSize, vec2(1e-6));
    vec4 clip = vec4(localUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProjection * clip;
    return abs(world.w) < 1e-6 ? uCameraPos : world.xyz / world.w;
}

float Hash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float Noise3D(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f*f*(3.0-2.0*f);
    float n000 = Hash(i + vec3(0,0,0));
    float n100 = Hash(i + vec3(1,0,0));
    float n010 = Hash(i + vec3(0,1,0));
    float n110 = Hash(i + vec3(1,1,0));
    float n001 = Hash(i + vec3(0,0,1));
    float n101 = Hash(i + vec3(1,0,1));
    float n011 = Hash(i + vec3(0,1,1));
    float n111 = Hash(i + vec3(1,1,1));
    float nx00 = mix(n000,n100,f.x);
    float nx10 = mix(n010,n110,f.x);
    float nx01 = mix(n001,n101,f.x);
    float nx11 = mix(n011,n111,f.x);
    return mix(mix(nx00,nx10,f.y),mix(nx01,nx11,f.y),f.z);
}

float FBM3(vec3 p)
{
    float value = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i=0; i<3; ++i)
    {
        value += Noise3D(p * freq) * amp;
        freq *= 2.03;
        amp *= 0.5;
        p += vec3(19.1, 7.7, 31.3);
    }
    return value;
}

float FBM2(vec3 p)
{
    float value = 0.0;
    float amp = 0.6;
    float freq = 1.0;
    for (int i=0; i<2; ++i)
    {
        value += Noise3D(p * freq) * amp;
        freq *= 2.03;
        amp *= 0.5;
        p += vec3(19.1, 7.7, 31.3);
    }
    return value;
}

float CloudDensity(vec3 p)
{
    float range = max(uHeightMax - uHeightMin, 0.001);
    float h = clamp((p.y - uHeightMin) / range, 0.0, 1.0);
    if (h <= 0.0 || h >= 1.0) return 0.0;

    float vertical = smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(0.78, 1.0, h));
    vec3 wind = vec3(uWind.x, 0.0, uWind.y) * (uWindSpeed * uTime);
    vec3 q = p * max(uScale, 1e-5) + wind;

    float base = FBM3(q);
    float detail = Noise3D(q * 3.1 + vec3(17.0, 5.0, 29.0));
    float shape = mix(base, base * 0.82 + detail * 0.30, clamp(uDetail, 0.0, 1.0));

    // Coverage is deliberately centered around the useful visible range so
    // the documented default (0.58) produces obvious clouds.
    float threshold = mix(0.36, 0.72, clamp(uCoverage, 0.0, 1.0));
    float d = smoothstep(threshold - 0.08, threshold + 0.12, shape);
    return d * vertical * max(uDensity, 0.0);
}

bool IntersectLayer(vec3 ro, vec3 rd, out float t0, out float t1)
{
    float minH = min(uHeightMin, uHeightMax);
    float maxH = max(uHeightMin, uHeightMax);
    if (abs(rd.y) < 1e-5)
        return (ro.y > minH && ro.y < maxH);

    float a = (minH - ro.y) / rd.y;
    float b = (maxH - ro.y) / rd.y;
    t0 = max(min(a,b), 0.0);
    t1 = max(a,b);
    return t1 > t0 + 1e-4;
}

float LightVisibility(vec3 p, vec3 lightDir, float sampleStep)
{
    float opticalDepth = 0.0;
    float stepLen = max(sampleStep * 2.0, (uHeightMax-uHeightMin)*0.025);
    vec3 q = p;
    for (int i=0; i<3; ++i)
    {
        q += lightDir * stepLen;
        opticalDepth += CloudDensity(q) * stepLen;
        if (opticalDepth > 3.0) break;
    }
    return exp(-opticalDepth * max(uLightAbsorption, 0.0));
}

void main()
{
    vec2 globalUV = ToGlobalUV(uv);
    vec4 base = texture(uScene, globalUV);
    if (uEnabled == 0 || uDensity <= 0.0 || uHeightMax <= uHeightMin)
    {
        frag_color = base;
        return;
    }

    vec3 nearWorld = Unproject(globalUV, 0.0);
    vec3 farWorld  = Unproject(globalUV, 1.0);
    vec3 rayDir = normalize(farWorld - nearWorld);
    vec3 rayOrigin = nearWorld;
    float raySpan = length(farWorld - nearWorld);
    if (raySpan <= 1e-4)
    {
        frag_color = base;
        return;
    }

    float tSurface = raySpan;
    float depth = texture(uDepth, globalUV).r;
    if (depth < 0.99999)
    {
        vec3 surfaceWorld = Unproject(globalUV, depth);
        tSurface = clamp(dot(surfaceWorld - rayOrigin, rayDir), 0.0, raySpan);
    }
    tSurface = min(tSurface, max(uMaxDistance, 1.0));

    float tEnter, tExit;
    if (!IntersectLayer(rayOrigin, rayDir, tEnter, tExit))
    {
        frag_color = base;
        return;
    }
    tExit = min(tExit, tSurface);
    if (tExit <= tEnter + 1e-4)
    {
        frag_color = base;
        return;
    }

    float cloudDistance = tExit - tEnter;
    int requestedSteps = clamp(uSteps, 12, 96);
    float stepFactor = clamp(cloudDistance / 220.0, 0.0, 1.0);
    int steps = clamp(int(mix(12.0, float(requestedSteps), stepFactor)), 12, requestedSteps);
    float stepLen = cloudDistance / float(steps);
    float jitter = Hash(vec3(globalUV * uScreenSize, uTime)) - 0.5;
    float t = tEnter + stepLen * (0.5 + jitter * 0.65);

    vec3 lightDir = normalize(-uSunDirection);
    vec3 accum = vec3(0.0);
    float trans = 1.0;

    for (int i=0; i<96; ++i)
    {
        if (i >= steps || trans < 0.02) break;
        vec3 p = rayOrigin + rayDir * t;
        float density = CloudDensity(p);
        if (density > 0.001)
        {
            float lightT = (density > 0.015 && uLightIntensity > 0.001)
                ? LightVisibility(p, lightDir, stepLen)
                : 1.0;
            float phase = 0.55 + 0.45 * max(dot(-rayDir, lightDir), 0.0);
            float height = clamp((p.y-uHeightMin)/max(uHeightMax-uHeightMin,1e-4),0.0,1.0);
            float silver = 0.75 + 0.25 * pow(max(height, 1e-3), 0.2);
            float alpha = 1.0 - exp(-density * stepLen);
            vec3 lit = uCloudColor * uLightColor;
            lit *= 0.35 + lightT * phase * silver * max(uLightIntensity,0.0);
            accum += trans * alpha * lit;
            trans *= 1.0 - alpha;
        }
        t += stepLen;
    }

    frag_color = vec4(base.rgb * trans + accum, base.a);
}
)GLSL";

inline constexpr const char* kSceneEffectsFragmentShader = R"GLSL(
#version 330 core
in vec2 uv;
out vec4 frag_color;
uniform sampler2D uScene;
uniform sampler2D uBrightScene;
uniform sampler2D uDepth;
uniform mat4 uInvViewProjection;
uniform vec3 uCameraPos;
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uScreenSize;

#define HRL_MAX_VOLUMETRIC_FOGS 64
uniform int uVolumetricFogCount;
uniform vec3 uFogPositions[HRL_MAX_VOLUMETRIC_FOGS];
uniform float uFogRadii[HRL_MAX_VOLUMETRIC_FOGS];
uniform vec3 uFogColors[HRL_MAX_VOLUMETRIC_FOGS];
uniform float uFogDensities[HRL_MAX_VOLUMETRIC_FOGS];
uniform int uVolumetricFogSteps;

uniform int uGlobalVolumetricFogEnabled;
uniform vec3 uGlobalFogColor;
uniform float uGlobalFogDensity;
uniform int uGlobalFogSteps;

uniform int uGodRaysEnabled;
uniform vec2 uGodRaysLightUV;
uniform vec3 uGodRaysColor;
uniform float uGodRaysDensity;
uniform float uGodRaysDecay;
uniform float uGodRaysWeight;
uniform int uGodRaysSamples;

// Volumetric cloud layer. The implementation is fully procedural so it does not
// require a 3D texture and remains compatible with OpenGL 3.3.
uniform int uVolumetricCloudEnabled;
uniform float uCloudCoverage;
uniform float uCloudDensity;
uniform float uCloudHeightMin;
uniform float uCloudHeightMax;
uniform float uCloudScale;
uniform float uCloudDetail;
uniform vec2 uCloudWind;
uniform float uCloudWindSpeed;
uniform vec3 uCloudColor;
uniform vec3 uCloudLightColor;
uniform float uCloudLightAbsorption;
uniform float uCloudLightIntensity;
uniform int uCloudSteps;
uniform float uCloudTime;
uniform float uCloudMaxDistance;
uniform vec3 uCloudSunDirection;

vec2 GlobalUV(vec2 localUV) { return uViewportOrigin + localUV * uViewportSize; }
vec3 ReconstructWorld(vec2 localUV, float depth)
{
    vec4 clip = vec4(localUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProjection * clip;
    if (abs(world.w) < 1e-6) return uCameraPos;
    return world.xyz / world.w;
}

vec4 ApplyVolumetricFog(vec2 globalUV, vec3 worldEnd)
{
    vec4 base = texture(uScene, globalUV);

    bool hasGlobalFog = uGlobalVolumetricFogEnabled != 0 && uGlobalFogDensity > 0.0;
    bool hasLocalFog = uVolumetricFogCount > 0;
    if (!hasGlobalFog && !hasLocalFog)
        return base;

    vec3 ray = worldEnd - uCameraPos;
    float rayLength = length(ray);
    if (rayLength <= 1e-4)
        return base;

    vec3 direction = ray / rayLength;
    int steps = clamp(max(uGlobalFogSteps, uVolumetricFogSteps), 4, 64);
    float stepLength = rayLength / float(steps);
    float transmittance = 1.0;
    vec3 scattering = vec3(0.0);

    for (int i = 0; i < 64; ++i)
    {
        if (i >= steps) break;

        float t = (float(i) + 0.5) / float(steps);
        vec3 samplePos = uCameraPos + direction * (rayLength * t);

        float totalDensity = 0.0;
        vec3 weightedColor = vec3(0.0);

        if (hasGlobalFog)
        {
            totalDensity += uGlobalFogDensity;
            weightedColor += uGlobalFogDensity * uGlobalFogColor;
        }

        for (int fogIndex = 0; fogIndex < HRL_MAX_VOLUMETRIC_FOGS; ++fogIndex)
        {
            if (fogIndex >= uVolumetricFogCount) break;

            float radius = uFogRadii[fogIndex];
            float density = uFogDensities[fogIndex];
            if (radius <= 0.0 || density <= 0.0) continue;

            float normalized = 1.0 - length(samplePos - uFogPositions[fogIndex]) / radius;
            if (normalized <= 0.0) continue;

            float localDensity = normalized * normalized * density;
            totalDensity += localDensity;
            weightedColor += localDensity * uFogColors[fogIndex];
        }

        if (totalDensity <= 0.0)
            continue;

        float alpha = 1.0 - exp(-totalDensity * stepLength);
        vec3 fogColor = weightedColor / totalDensity;
        scattering += transmittance * alpha * fogColor;
        transmittance *= 1.0 - alpha;
        if (transmittance < 0.01) break;
    }

    return vec4(base.rgb * transmittance + scattering, base.a);
}

float CloudHash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float CloudNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = CloudHash(i + vec3(0,0,0));
    float n100 = CloudHash(i + vec3(1,0,0));
    float n010 = CloudHash(i + vec3(0,1,0));
    float n110 = CloudHash(i + vec3(1,1,0));
    float n001 = CloudHash(i + vec3(0,0,1));
    float n101 = CloudHash(i + vec3(1,0,1));
    float n011 = CloudHash(i + vec3(0,1,1));
    float n111 = CloudHash(i + vec3(1,1,1));

    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);
    return mix(nxy0, nxy1, f.z);
}

float CloudFBM3(vec3 p)
{
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    for (int i = 0; i < 3; ++i)
    {
        value += CloudNoise(p * frequency) * amplitude;
        frequency *= 2.03;
        amplitude *= 0.5;
        p += vec3(19.1, 7.7, 31.3);
    }
    return value;
}

float CloudFBM2(vec3 p)
{
    float value = 0.0;
    float amplitude = 0.65;
    float frequency = 1.0;
    for (int i = 0; i < 2; ++i)
    {
        value += CloudNoise(p * frequency) * amplitude;
        frequency *= 2.03;
        amplitude *= 0.5;
        p += vec3(19.1, 7.7, 31.3);
    }
    return value / 0.975;
}

float CloudHeightShape(vec3 p)
{
    float range = max(uCloudHeightMax - uCloudHeightMin, 0.001);
    float h = clamp((p.y - uCloudHeightMin) / range, 0.0, 1.0);
    if (h <= 0.0 || h >= 1.0) return 0.0;
    return smoothstep(0.0, 0.12, h) * (1.0 - smoothstep(0.76, 1.0, h));
}

vec3 CloudWindPosition(vec3 p)
{
    vec3 windOffset = vec3(uCloudWind.x, 0.0, uCloudWind.y) * (uCloudWindSpeed * uCloudTime);
    return p * max(uCloudScale, 0.00001) + windOffset;
}

float CloudDensity(vec3 worldPos)
{
    float heightShape = CloudHeightShape(worldPos);
    if (heightShape <= 0.0) return 0.0;

    vec3 q = CloudWindPosition(worldPos);
    float base = CloudFBM3(q);
    // One extra noise lookup for detail instead of a second multi-octave FBM.
    float detail = CloudNoise(q * 3.1 + vec3(5.7, 13.1, 2.9));
    float detailAmount = clamp(uCloudDetail, 0.0, 1.0);
    float shape = mix(base, base * 0.82 + detail * 0.30, detailAmount);

    float threshold = clamp(uCloudCoverage, 0.0, 1.0);
    float d = smoothstep(threshold - 0.055, min(0.999, threshold + 0.16), shape);
    return d * heightShape * max(uCloudDensity, 0.0);
}

float CloudLightDensity(vec3 worldPos)
{
    float heightShape = CloudHeightShape(worldPos);
    if (heightShape <= 0.0) return 0.0;

    vec3 q = CloudWindPosition(worldPos);
    float shape = CloudFBM2(q);
    float threshold = clamp(uCloudCoverage, 0.0, 1.0);
    float d = smoothstep(threshold - 0.08, min(0.999, threshold + 0.20), shape);
    return d * heightShape * max(uCloudDensity, 0.0);
}

float CloudLightTransmittance(vec3 startPos, vec3 towardLight, float sampleStep)
{
    float opticalDepth = 0.0;
    float layerHeight = max(uCloudHeightMax - uCloudHeightMin, 1.0);
    float lightStep = max(sampleStep * 3.0, layerHeight * 0.065);
    vec3 p = startPos;
    for (int i = 0; i < 2; ++i)
    {
        p += towardLight * lightStep;
        opticalDepth += CloudLightDensity(p) * lightStep;
    }
    return exp(-opticalDepth * max(uCloudLightAbsorption, 0.0));
}

bool CloudIntersectLayer(vec3 rayOrigin, vec3 rayDirection, out float tEnter, out float tExit)
{
    if (abs(rayDirection.y) < 1e-5)
    {
        tEnter = 0.0;
        tExit = 0.0;
        return rayOrigin.y > uCloudHeightMin && rayOrigin.y < uCloudHeightMax;
    }

    float a = (uCloudHeightMin - rayOrigin.y) / rayDirection.y;
    float b = (uCloudHeightMax - rayOrigin.y) / rayDirection.y;
    tEnter = max(min(a, b), 0.0);
    tExit = max(a, b);
    return tExit > tEnter + 1e-4;
}

vec4 ApplyVolumetricClouds(vec2 globalUV, vec2 localUV, float sceneDepth, vec4 base)
{
    if (uVolumetricCloudEnabled == 0 || uCloudDensity <= 0.0 || uCloudHeightMax <= uCloudHeightMin)
        return base;

    vec3 nearWorld = ReconstructWorld(localUV, 0.0);
    vec3 farWorld = ReconstructWorld(localUV, 1.0);
    vec3 rayVector = farWorld - nearWorld;
    float raySpan = length(rayVector);
    if (raySpan <= 1e-4)
        return base;

    vec3 rayDirection = rayVector / raySpan;
    vec3 rayOrigin = nearWorld;
    float sceneDistance = raySpan;

    if (sceneDepth < 0.99999)
    {
        vec3 worldSurface = ReconstructWorld(localUV, sceneDepth);
        sceneDistance = clamp(dot(worldSurface - rayOrigin, rayDirection), 0.0, raySpan);
    }
    sceneDistance = min(sceneDistance, max(uCloudMaxDistance, 1.0));

    float tEnter, tExit;
    if (!CloudIntersectLayer(rayOrigin, rayDirection, tEnter, tExit))
        return base;

    tExit = min(tExit, sceneDistance);
    if (tExit <= tEnter + 1e-4)
        return base;

    float cloudDistance = tExit - tEnter;
    int requestedSteps = clamp(uCloudSteps, 8, 96);
    float stepFactor = clamp(cloudDistance / 220.0, 0.0, 1.0);
    int steps = clamp(int(mix(8.0, float(requestedSteps), stepFactor)), 8, requestedSteps);
    float stepLength = cloudDistance / float(steps);

    float jitter = CloudHash(vec3(globalUV * uScreenSize, uCloudTime)) - 0.5;
    float t = tEnter + stepLength * (0.5 + jitter * 0.65);
    vec3 lightDir = normalize(-uCloudSunDirection);

    vec3 cloudScattering = vec3(0.0);
    float transmittance = 1.0;

    for (int i = 0; i < 96; ++i)
    {
        if (i >= steps || transmittance < 0.025)
            break;

        vec3 samplePos = rayOrigin + rayDirection * t;
        float density = CloudDensity(samplePos);
        if (density > 0.001)
        {
            float lightT = 1.0;
            if (density > 0.015 && uCloudLightIntensity > 0.001)
                lightT = CloudLightTransmittance(samplePos, lightDir, stepLength);

            float phase = 0.55 + 0.45 * max(dot(-rayDirection, lightDir), 0.0);
            float height01 = clamp((samplePos.y - uCloudHeightMin) / max(uCloudHeightMax - uCloudHeightMin, 1e-4), 0.0, 1.0);
            float silver = 0.65 + 0.35 * pow(max(height01, 1e-3), 0.2);
            float alpha = 1.0 - exp(-density * stepLength);
            vec3 lit = uCloudColor * (0.55 + lightT * phase * silver * max(uCloudLightIntensity, 0.0));
            lit *= uCloudLightColor;

            cloudScattering += transmittance * alpha * lit;
            transmittance *= 1.0 - alpha;
        }
        t += stepLength;
    }

    return vec4(base.rgb * transmittance + cloudScattering, base.a);
}

vec4 ApplyGodRays(vec2 globalUV, vec4 base)
{
    if (uGodRaysEnabled == 0 || uGodRaysWeight <= 0.0 || uGodRaysDensity <= 0.0)
        return base;

    int samples = clamp(uGodRaysSamples, 8, 96);
    vec2 lightUV = uViewportOrigin + uGodRaysLightUV * uViewportSize;
    vec2 delta = (globalUV - lightUV) * (uGodRaysDensity / float(samples));
    vec2 sampleUV = globalUV;
    vec3 rays = vec3(0.0);
    float illumination = 1.0;

    for (int i = 0; i < 96; ++i)
    {
        if (i >= samples) break;
        sampleUV -= delta;

        if (any(lessThan(sampleUV, vec2(0.0))) ||
            any(greaterThan(sampleUV, vec2(1.0))))
            break;

        vec3 sceneColor = texture(uScene, sampleUV).rgb;
        vec3 brightColor = texture(uBrightScene, sampleUV).rgb;
        float luminance = dot(sceneColor, vec3(0.2126, 0.7152, 0.0722));

        rays += (brightColor +
                 max(luminance - 0.55, 0.0) * sceneColor * 0.35) * illumination;
        illumination *= uGodRaysDecay;
    }

    base.rgb += rays / float(samples) * uGodRaysColor * uGodRaysWeight;
    return base;
}

void main()
{
    vec2 globalUV = GlobalUV(uv);
    vec4 color = texture(uScene, globalUV);
    float depth = texture(uDepth, globalUV).r;
    color = ApplyVolumetricFog(globalUV, ReconstructWorld(uv, depth));
    color = ApplyGodRays(globalUV, color);
    frag_color = color;
}
)GLSL";

inline constexpr const char* kSSRFragmentShader = R"GLSL(
#version 330 core
in vec2 uv;
out vec4 frag_color;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
uniform mat4 uViewProjection;
uniform mat4 uInvViewProjection;
uniform vec3 uCameraPos;
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uScreenSize;
uniform float uStrength;
uniform float uMaxDistance;
uniform float uThickness;
uniform float uFadeStart;
uniform float uFadeEnd;
uniform int uSteps;

vec2 GlobalUV(vec2 localUV) { return uViewportOrigin + localUV * uViewportSize; }
vec3 ReconstructWorld(vec2 globalUV, float depth)
{
    vec4 clip = vec4(globalUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProjection * clip;
    if (abs(world.w) < 1e-6) return uCameraPos;
    return world.xyz / world.w;
}
vec2 ProjectUV(vec3 world)
{
    vec4 clip = uViewProjection * vec4(world,1.0);
    if (clip.w <= 1e-5) return vec2(-1.0);
    return clip.xy / clip.w * 0.5 + 0.5;
}
void main()
{
    vec2 globalUV = GlobalUV(uv);
    vec4 base = texture(uScene, globalUV);
    float depth = texture(uDepth, globalUV).r;
    if (depth >= 0.99999 || uStrength <= 0.0 || uMaxDistance <= 0.0)
    {
        frag_color = base;
        return;
    }

    vec4 normalSample = texture(uNormal, globalUV);
    if (normalSample.a <= 0.001)
    {
        frag_color = base;
        return;
    }
    vec3 N = normalize(normalSample.rgb);
    vec3 worldPos = ReconstructWorld(globalUV, depth);
    vec3 V = normalize(uCameraPos - worldPos);
    vec3 R = normalize(reflect(-V, N));
    if (dot(R, R) < 1e-6)
    {
        frag_color = base;
        return;
    }

    float cosTheta = max(dot(N, V), 0.0);
    float fresnel = pow(1.0 - cosTheta, 5.0);
    fresnel = mix(0.04, 1.0, fresnel);
    float stepLength = uMaxDistance / float(max(uSteps, 8));
    vec3 rayPos = worldPos + N * max(uThickness * 0.5, 0.001);
    vec3 hitColor = vec3(0.0);
    float confidence = 0.0;

    for (int i = 0; i < 96; ++i)
    {
        if (i >= uSteps) break;
        rayPos += R * stepLength;
        vec2 hitUV = ProjectUV(rayPos);
        if (any(lessThan(hitUV, vec2(0.001))) || any(greaterThan(hitUV, vec2(0.999))))
            break;
        float hitDepth = texture(uDepth, hitUV).r;
        if (hitDepth >= 0.99999)
            continue;
        vec3 hitWorld = ReconstructWorld(hitUV, hitDepth);
        float sceneDistance = length(hitWorld - uCameraPos);
        float rayDistance = length(rayPos - uCameraPos);
        float thickness = max(uThickness, stepLength * 1.5);
        if (sceneDistance <= rayDistance + thickness && sceneDistance >= length(worldPos-uCameraPos) + stepLength * 0.25)
        {
            hitColor = texture(uScene, hitUV).rgb;
            float distanceFade = 1.0 - smoothstep(uMaxDistance * uFadeStart, uMaxDistance * uFadeEnd, rayDistance);
            float edgeFade = 1.0 - smoothstep(0.0, 0.15, max(abs(hitUV.x-0.5), abs(hitUV.y-0.5)) * 2.0);
            confidence = clamp(uStrength * fresnel * distanceFade * edgeFade, 0.0, 1.0);
            break;
        }
    }

    frag_color = vec4(mix(base.rgb, hitColor, confidence), base.a);
}
)GLSL";

inline constexpr const char* kDecalFragmentShader = R"GLSL(
#version 330 core
in vec2 uv;
out vec4 frag_color;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
uniform sampler2D uDecal;
uniform mat4 uInvViewProjection;
uniform mat4 uDecalInvModel;
uniform vec3 uDecalNormal;
uniform vec4 uColor;
uniform float uOpacity;
uniform float uNormalFadeMin;
uniform float uNormalFadeMax;
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uScreenSize;
vec2 GlobalUV(vec2 localUV) { return uViewportOrigin + localUV * uViewportSize; }
vec3 ReconstructWorld(vec2 globalUV, float depth)
{
    vec4 clip = vec4(globalUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProjection * clip;
    if (abs(world.w) < 1e-6) return vec3(0.0);
    return world.xyz / world.w;
}
void main()
{
    vec2 globalUV = GlobalUV(uv);
    vec4 base = texture(uScene, globalUV);
    float depth = texture(uDepth, globalUV).r;
    vec4 n = texture(uNormal, globalUV);
    if (depth >= 0.99999 || n.a <= 0.001 || uOpacity <= 0.0)
    { frag_color = base; return; }
    vec3 worldPos = ReconstructWorld(globalUV, depth);
    vec3 local = (uDecalInvModel * vec4(worldPos,1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5))))
    { frag_color = base; return; }
    vec3 surfaceNormal = normalize(n.rgb);
    float facing = abs(dot(surfaceNormal, normalize(uDecalNormal)));
    float facingFade = smoothstep(uNormalFadeMin, max(uNormalFadeMin + 0.001, uNormalFadeMax), facing);
    if (facingFade <= 0.001)
    { frag_color = base; return; }
    vec2 decalUV = local.xz + 0.5;
    vec4 decal = texture(uDecal, decalUV) * uColor;
    float alpha = clamp(decal.a * uOpacity * facingFade, 0.0, 1.0);
    frag_color = vec4(mix(base.rgb, decal.rgb, alpha), base.a);
}
)GLSL";

inline constexpr const char* kVoxelLightFullscreenVS = R"GLSL(#version 330 core
out vec2 uv;
void main()
{
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

inline constexpr const char* kVoxelLightEmitVS = R"GLSL(#version 330 core
layout(location = 0) in vec2 aPosition;
layout(location = 2) in vec4 aColor;
uniform vec4 Region; // xy : world min, zw : 1 / world size
out vec3 emission;
void main()
{
    vec2 uv = (aPosition - Region.xy) * Region.zw;
    emission = max(aColor.rgb, vec3(0.0)) * clamp(aColor.a, 0.0, 1.0);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

inline constexpr const char* kVoxelLightEmitFS = R"GLSL(#version 330 core
in vec3 emission;
out vec4 FragColor;
void main()
{
    FragColor = vec4(emission, 1.0);
}
)GLSL";

inline constexpr const char* kVoxelLightDownFS = R"GLSL(#version 330 core
in vec2 uv;
uniform sampler2D Src;
uniform vec2 SrcTexel;
out vec4 FragColor;
void main()
{
    vec3 sum = texture(Src, uv + vec2(-1.0, -1.0) * SrcTexel).rgb;
    sum += texture(Src, uv + vec2( 1.0, -1.0) * SrcTexel).rgb;
    sum += texture(Src, uv + vec2(-1.0,  1.0) * SrcTexel).rgb;
    sum += texture(Src, uv + vec2( 1.0,  1.0) * SrcTexel).rgb;
    FragColor = vec4(sum * 0.25, 1.0);
}
)GLSL";

inline constexpr const char* kVoxelLightUpFS = R"GLSL(#version 330 core
in vec2 uv;
uniform sampler2D Cur;
uniform sampler2D Low;
uniform int HasLow;
uniform float Weight;
uniform vec2 LowTexel;
out vec4 FragColor;
void main()
{
    vec3 light = Weight * texture(Cur, uv).rgb;
    if (HasLow != 0)
    {
        // 3x3 tent of the coarser accumulated level.
        vec3 sum = vec3(0.0);
        sum += texture(Low, uv + vec2(-1.0, -1.0) * LowTexel).rgb;
        sum += texture(Low, uv + vec2( 0.0, -1.0) * LowTexel).rgb * 2.0;
        sum += texture(Low, uv + vec2( 1.0, -1.0) * LowTexel).rgb;
        sum += texture(Low, uv + vec2(-1.0,  0.0) * LowTexel).rgb * 2.0;
        sum += texture(Low, uv).rgb * 4.0;
        sum += texture(Low, uv + vec2( 1.0,  0.0) * LowTexel).rgb * 2.0;
        sum += texture(Low, uv + vec2(-1.0,  1.0) * LowTexel).rgb;
        sum += texture(Low, uv + vec2( 0.0,  1.0) * LowTexel).rgb * 2.0;
        sum += texture(Low, uv + vec2( 1.0,  1.0) * LowTexel).rgb;
        light += sum * (1.0 / 16.0);
    }
    FragColor = vec4(light, 1.0);
}
)GLSL";

inline constexpr const char* kUISDFFragmentShader = R"GLSL(#version 330 core

in vec2 uv;

uniform vec4 uTintColor;
uniform sampler2D uTexture;
uniform bool uSDFText;

out vec4 FragColor;

void main()
{
    vec4 texel = texture(uTexture, uv);
    if (uSDFText)
    {
        float distance = texel.r;
        float smoothing = max(fwidth(distance), 0.001);
        float alpha = smoothstep(0.5 - smoothing, 0.5 + smoothing, distance);
        FragColor = vec4(uTintColor.rgb, uTintColor.a * alpha);
    }
    else
    {
        FragColor = texel * uTintColor;
    }
}
)GLSL";

#endif
