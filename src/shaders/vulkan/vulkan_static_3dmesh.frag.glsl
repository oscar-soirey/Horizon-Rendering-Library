#version 450
layout(location=0) in vec3 vWorldPos;
layout(location=1) in vec3 vNormal;
layout(location=2) in vec2 vUV;
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

void main() {
    vec4 albedo = texture(T_Albedo, vUV) * TintColor;
    float alphaSample = texture(T_Alpha, vUV).r;
    if (AlphaInvert != 0) alphaSample = 1.0 - alphaSample;
    float alpha = albedo.a * BaseColorAlpha * (OpacityUseValue != 0 ? OpacityValue : alphaSample);
    if (alpha < 0.01) discard;
    vec3 N = normalize(vNormal);
    vec3 color = albedo.rgb * 0.025;
    float roughSample = texture(T_Roughness, vUV).r;
    if (RoughnessInvert != 0) roughSample = 1.0 - roughSample;
    float rough = clamp(RoughnessUseValue != 0 ? RoughnessValue : roughSample, 0.02, 1.0);
    float metal = clamp(MetallicUseValue != 0 ? MetallicValue : texture(T_Metallic, vUV).r, 0.0, 1.0);
    vec3 V = normalize(CamPos - vWorldPos);
    for (uint i=0u; i<count && i<32u; ++i) {
        Light Ld = lights[i];
        uint type = uint(Ld.params.x + 0.5);
        vec3 L;
        float attenuation = 1.0;
        if (type == 0x0012u) {
            L = normalize(-Ld.rotation.xyz);
        } else if (type == 0x0014u) {
            color += albedo.rgb * Ld.color.rgb * Ld.params.y;
            continue;
        } else {
            vec3 toLight = Ld.position.xyz - vWorldPos;
            float d = max(length(toLight), 0.0001);
            L = toLight / d;
            attenuation = 1.0 / (1.0 + max(Ld.params.y,0.0) * d * d);
            if (type == 0x0013u) {
                float theta = dot(normalize(-Ld.rotation.xyz), L);
                float inner = cos(radians(Ld.params.z));
                float outer = cos(radians(Ld.params.w));
                attenuation *= clamp((theta - outer) / max(inner - outer,0.0001), 0.0, 1.0);
            }
        }
        float ndl = max(dot(N,L),0.0);
        vec3 H = normalize(L+V);
        float specSample = texture(T_Specular, vUV).r;
        float spec = pow(max(dot(N,H),0.0), mix(8.0,128.0,1.0-rough)) * (SpecularUseValue != 0 ? SpecularValue : specSample);
        vec3 diffuse = albedo.rgb * ndl * (1.0-metal);
        vec3 specular = mix(vec3(0.04), albedo.rgb, metal) * spec;
        color += (diffuse + specular) * Ld.color.rgb * Ld.params.y * attenuation;
    }
    if (DebugView == 1) color = albedo.rgb;
    if (DebugView == 2) color = N * 0.5 + 0.5;
    if (DebugView == 3) color = DebugLODColor;
    if (FogEnabled != 0) {
        float d = length(vWorldPos-CamPos);
        float fog = (FogMode == 0) ? clamp((d-FogStart)/max(FogEnd-FogStart,0.001),0.0,1.0)
                                   : (1.0-exp(-FogDensity*d));
        color = mix(color, FogColor.rgb, clamp(fog,0.0,1.0));
    }
    float lum = dot(color, vec3(0.2126,0.7152,0.0722));
    outColor = vec4(color, alpha);
    outBright = lum > BrightThreshold ? vec4(color,1.0) : vec4(0.0);
    outPicking = vec4(float((ColorPickingID >> 0u) & 255u)/255.0,
                      float((ColorPickingID >> 8u) & 255u)/255.0,
                      float((ColorPickingID >> 16u) & 255u)/255.0, 1.0);
}
