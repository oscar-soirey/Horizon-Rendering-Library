#pragma once

namespace VulkanBuiltinShaders
{
static constexpr const char* StaticVertex = R"glsl(#version 450
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
    vec3 N = normalize(normalMatrix * aNormal);
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
)glsl";

static constexpr const char* StaticFragment = R"glsl(#version 450
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

    vec3 litResult = lighting * albedo.rgb * ao;
    if (DebugView == 1) {
        outColor = vec4(albedo.rgb, alpha);
        outBright = vec4(0.0);
    } else if (DebugView == 2) {
        outColor = vec4(N * 0.5 + 0.5, alpha);
        outBright = vec4(0.0);
    } else if (DebugView == 3) {
        outColor = vec4(DebugLODColor, alpha);
        outBright = vec4(0.0);
    } else {
        if (FogEnabled != 0) {
            float d = length(vWorldPos - CamPos);
            float fog = (FogMode == 0)
                ? clamp((d - FogStart) / max(FogEnd - FogStart, 0.001), 0.0, 1.0)
                : (1.0 - exp(-FogDensity * d));
            if (FogMode == 0)
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
)glsl";

static constexpr const char* SkeletalVertex = R"glsl(#version 450
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
    vec3 localNormal = normalize(skin3 * aNormal);
    vec3 localTangent = skin3 * aTangent;
    vec3 localBitangent = skin3 * aBitangent;

    vec4 world = model * local;
    vec3 N = normalize(normalMatrix * localNormal);
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
)glsl";

static constexpr const char* SpriteVertex = R"glsl(#version 450
layout(location=0) in vec3 aPosition;
layout(location=2) in vec2 aUV;
layout(location=0) out vec2 vUV;
uniform mat4 projection;
uniform mat4 view;
uniform mat4 model;
uniform vec4 UVRegion;
void main(){ vUV=mix(UVRegion.xy,UVRegion.zw,aUV); gl_Position=projection*view*model*vec4(aPosition,1.0); }
)glsl";

static constexpr const char* SpriteFragment = R"glsl(#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outBright;
layout(location=2) out vec4 outPicking;
uniform sampler2D T_Albedo;
uniform vec4 TintColor;
uniform uint ColorPickingID;
void main(){ vec4 c=texture(T_Albedo,vUV)*TintColor; if(c.a<0.01) discard; outColor=c; outBright=vec4(0.0); outPicking=vec4(float((ColorPickingID>>0u)&255u)/255.0,float((ColorPickingID>>8u)&255u)/255.0,float((ColorPickingID>>16u)&255u)/255.0,1.0); }
)glsl";

static constexpr const char* DebugVertex = R"glsl(#version 450
layout(location=0) in vec3 aPosition; layout(location=1) in vec3 aColor; layout(location=0) out vec3 vColor;
uniform mat4 projection; uniform mat4 view;
void main(){vColor=aColor;gl_Position=projection*view*vec4(aPosition,1.0);}
)glsl";

static constexpr const char* DebugFragment = R"glsl(#version 450
layout(location=0) in vec3 vColor; layout(location=0) out vec4 outColor; layout(location=1) out vec4 outBright; layout(location=2) out vec4 outPicking;
void main(){outColor=vec4(vColor,1.0);outBright=vec4(0.0);outPicking=vec4(0.0);}
)glsl";

static constexpr const char* SkyVertex = R"glsl(#version 450
layout(location=0) out vec2 vUV;
void main(){vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));vUV=p[gl_VertexIndex]*0.5+0.5;gl_Position=vec4(p[gl_VertexIndex],0,1);}
)glsl";

static constexpr const char* SkyFragment = R"glsl(#version 450
layout(location=0) in vec2 vUV; layout(location=0) out vec4 outColor; layout(location=1) out vec4 outBright; layout(location=2) out vec4 outPicking;
uniform mat4 projection; uniform mat4 view; uniform mat4 skyRotation; uniform vec3 SkyTopColor; uniform vec3 SkyHorizonColor; uniform vec3 SkyBottomColor; uniform sampler2D SkyTexture; uniform int SkyUseTexture;
void main(){
 vec4 clip=vec4(vUV*2.0-1.0,1.0,1.0); mat4 invPV=inverse(projection*mat4(mat3(view))); vec3 dir=normalize((invPV*clip).xyz); dir=normalize((mat3(skyRotation)*dir));
 vec3 c;
 if(SkyUseTexture!=0){float lon=atan(dir.z,dir.x)/(2.0*3.14159265)+0.5;float lat=asin(clamp(dir.y,-1.0,1.0))/3.14159265+0.5;c=texture(SkyTexture,vec2(lon,lat)).rgb;}
 else {float t=smoothstep(0.0,1.0,dir.y*0.5+0.5);vec3 hc=mix(SkyHorizonColor,SkyTopColor,t);float b=smoothstep(0.0,1.0,-dir.y);c=mix(hc,SkyBottomColor,b);}
 outColor=vec4(c,1);outBright=vec4(0);outPicking=vec4(0);
}
)glsl";

static constexpr const char* UIFragment = R"glsl(#version 450
layout(location=0) in vec2 vUV; layout(location=1) in vec4 vColor;
layout(location=0) out vec4 outColor; layout(location=1) out vec4 outBright; layout(location=2) out vec4 outPicking;
uniform sampler2D uTexture; uniform int uHasTexture; uniform int uSDF;
void main(){vec4 c=vColor;if(uHasTexture!=0){vec4 t=texture(uTexture,vUV); if(uSDF!=0){float d=t.r;float smoothing=max(fwidth(d),0.001);c.a*=smoothstep(0.5-smoothing,0.5+smoothing,d);} else c*=t;} if(c.a<0.001)discard;outColor=c;outBright=vec4(0);outPicking=vec4(0);}
)glsl";

static constexpr const char* UIVertex = R"glsl(#version 450
layout(location=0) in vec2 aPos; layout(location=1) in vec2 aUV; layout(location=2) in vec4 aColor; layout(location=0) out vec2 vUV; layout(location=1) out vec4 vColor; uniform mat4 uiProj;
void main(){vUV=aUV;vColor=aColor;gl_Position=uiProj*vec4(aPos,0,1);}
)glsl";

static constexpr const char* VFXVertex = R"glsl(#version 450
layout(location=0) in vec3 aPosition;
layout(location=2) in vec2 aTexCoord;
layout(location=3) in vec4 iModel0;
layout(location=4) in vec4 iModel1;
layout(location=5) in vec4 iModel2;
layout(location=6) in vec4 iModel3;
layout(location=7) in vec4 iColor;
layout(location=0) out vec2 vUV;
layout(location=1) out vec4 vColor;
uniform mat4 projection;
uniform mat4 view;
void main(){
 mat4 model=mat4(iModel0,iModel1,iModel2,iModel3);
 vUV=aTexCoord;vColor=iColor;
 gl_Position=projection*view*model*vec4(aPosition,1.0);
}
)glsl";

static constexpr const char* VFXFragment = R"glsl(#version 450
layout(location=0) in vec2 vUV;
layout(location=1) in vec4 vColor;
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outBright;
layout(location=2) out vec4 outPicking;
uniform sampler2D uTexture;
uniform int uUseTexture;
void main(){
 vec4 texel=(uUseTexture!=0)?texture(uTexture,vUV):vec4(1.0);
 vec4 c=texel*vColor;
 if(c.a<=0.001) discard;
 outColor=c;outBright=vec4(0.0);outPicking=vec4(0.0);
}
)glsl";

static constexpr const char* PostVertex = R"glsl(#version 450
layout(location=0) out vec2 vUV;void main(){vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));vUV=p[gl_VertexIndex]*0.5+0.5;gl_Position=vec4(p[gl_VertexIndex],0,1);}
)glsl";
}
