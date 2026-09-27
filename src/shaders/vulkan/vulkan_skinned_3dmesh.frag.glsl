#version 450
layout(location=0) in vec3 vWorldPos;
layout(location=1) in vec3 vNormal;
layout(location=2) in vec3 vWorldTangent;
layout(location=3) in vec3 vWorldBitangent;
layout(location=4) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outBright;
layout(location=2) out vec4 outPicking;

struct Light { vec4 position; vec4 rotation; vec4 color; vec4 params; };
layout(set=0,binding=32,std140) uniform HRLLights {
    Light lights[32];
    uint count;
};

uniform sampler2D T_Albedo;
uniform sampler2D T_Normal;
uniform sampler2D T_Specular;
uniform sampler2D T_Roughness;
uniform sampler2D T_Metallic;
uniform sampler2D T_Alpha;
uniform sampler2D T_AO;
uniform vec4 TintColor;
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
uniform int NormalUseTexture;
uniform vec3 CamPos;
uniform vec4 FogColor;
uniform float FogStart;
uniform float FogEnd;
uniform float FogDensity;
uniform int FogEnabled;
uniform int FogMode;
uniform int DebugView;
uniform vec3 DebugLODColor;
uniform uint ColorPickingID;
uniform float BrightThreshold;

vec3 BuildWorldNormal()
{
    vec3 N = normalize(vNormal);
    vec3 tangent = vWorldTangent - N * dot(N, vWorldTangent);
    if (dot(tangent, tangent) < 1e-8)
        tangent = abs(N.y) < 0.999 ? cross(N, vec3(0.0, 1.0, 0.0)) : cross(N, vec3(1.0, 0.0, 0.0));
    vec3 T = normalize(tangent);
    vec3 B = normalize(cross(N, T));
    if (dot(B, vWorldBitangent) < 0.0)
        B = -B;
    if (TwoSided != 0 && !gl_FrontFacing)
    {
        N = -N;
        T = -T;
        B = -B;
    }
    vec3 normalTex = NormalUseTexture != 0
        ? texture(T_Normal, vUV).rgb * 2.0 - 1.0
        : vec3(0.0, 0.0, 1.0);
    return normalize(T * normalTex.x + B * normalTex.y + N * normalTex.z);
}

void main() {
    vec4 albedo = texture(T_Albedo, vUV) * TintColor;
    float alphaSample = texture(T_Alpha, vUV).r;
    if (AlphaInvert != 0) alphaSample = 1.0 - alphaSample;
    float alpha = albedo.a * BaseColorAlpha * (OpacityUseValue != 0 ? OpacityValue : alphaSample);
    if (alpha < 0.01) discard;

    vec3 N = BuildWorldNormal();
    vec3 color = albedo.rgb * 0.025;
    float roughSample = texture(T_Roughness, vUV).r;
    if (RoughnessInvert != 0) roughSample = 1.0 - roughSample;
    float rough = clamp(RoughnessUseValue != 0 ? RoughnessValue : roughSample, 0.05, 1.0);
    float metal = clamp(MetallicUseValue != 0 ? MetallicValue : texture(T_Metallic, vUV).r, 0.0, 1.0);
    float specSample = SpecularUseValue != 0 ? SpecularValue : texture(T_Specular, vUV).r;
    float ao = clamp(texture(T_AO, vUV).r, 0.0, 1.0);
    vec3 V = normalize(CamPos - vWorldPos);
    vec3 F0 = mix(vec3(0.04), albedo.rgb, metal);
    float shininess = pow(2.0, (1.0 - rough) * 11.0);

    vec3 lighting = vec3(0.0);
    for (uint i = 0u; i < count && i < 32u; ++i) {
        Light Ld = lights[i];
        uint type = uint(Ld.params.x + 0.5);
        vec3 L;
        float attenuation = 1.0;
        float shadow = 1.0;
        if (type == 0x0012u) {
            L = normalize(-Ld.rotation.xyz);
        } else if (type == 0x0014u) {
            lighting += (1.0 - metal) * Ld.color.rgb * Ld.params.y;
            continue;
        } else if (type == 0x0011u || type == 0x0013u) {
            vec3 toLight = Ld.position.xyz - vWorldPos;
            float d = max(length(toLight), 0.0001);
            L = toLight / d;
            attenuation = 1.0 / (1.0 + max(Ld.params.y, 0.0) * d * d);
            if (type == 0x0013u) {
                float theta = dot(L, normalize(-Ld.rotation.xyz));
                float inner = cos(radians(Ld.params.z));
                float outer = cos(radians(Ld.params.w));
                float range = max(inner - outer, 0.0001);
                attenuation *= clamp((theta - outer) / range, 0.0, 1.0);
            }
        } else {
            continue;
        }

        float ndl = max(dot(N, L), 0.0);
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0), shininess) * specSample;
        vec3 diffuse = (1.0 - metal) * ndl * Ld.color.rgb * Ld.params.y;
        vec3 specular = F0 * spec * Ld.color.rgb * Ld.params.y;
        lighting += (diffuse + specular) * attenuation * shadow;
    }

    // Keep the Vulkan built-in mesh shader visible even when a scene has no
    // direct light, matching the previous backend fallback and the engine
    // expectation that an unlit mesh is still renderable.
    vec3 ambient = albedo.rgb * 0.08;
    vec3 litResult = ambient + lighting * albedo.rgb * ao;

    if (DebugView == 0x0061) {
        outColor = vec4(albedo.rgb, alpha);
        outBright = vec4(0.0);
    } else if (DebugView == 0x0062) {
        outColor = vec4(N * 0.5 + 0.5, alpha);
        outBright = vec4(0.0);
    } else if (DebugView == 0x0065) {
        outColor = vec4(DebugLODColor, alpha);
        outBright = vec4(0.0);
    } else {
        if (FogEnabled != 0) {
            float d = length(vWorldPos - CamPos);
            float fog = (FogMode == 0x0090)
                ? clamp((d - FogStart) / max(FogEnd - FogStart, 0.001), 0.0, 1.0)
                : (FogMode == 0x0091)
                    ? (1.0 - exp(-FogDensity * d))
                    : (1.0 - exp(-FogDensity * FogDensity * d * d));
            if (FogMode == 0x0090)
                litResult = mix(litResult, FogColor.rgb, fog);
            else
                litResult = mix(litResult, FogColor.rgb, clamp(fog, 0.0, 1.0));
        }
        vec4 finalColor = vec4(litResult, alpha);
        float lum = dot(finalColor.rgb, vec3(0.2126, 0.7152, 0.0722));
        outColor = finalColor;
        outBright = lum > BrightThreshold ? vec4(finalColor.rgb, 1.0) : vec4(0.0);
    }

    // Match the OpenGL picking encoding: most-significant byte in R.
    outPicking = vec4(float((ColorPickingID >> 16u) & 255u) / 255.0,
                      float((ColorPickingID >> 8u) & 255u) / 255.0,
                      float(ColorPickingID & 255u) / 255.0,
                      1.0);
}
