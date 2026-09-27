#version 450
layout(location=0) in vec3 vWorldPos;
layout(location=1) in vec3 vNormal;
layout(location=2) in vec3 vWorldTangent;
layout(location=3) in vec3 vWorldBitangent;
layout(location=4) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outBright;
layout(location=2) out vec4 outPicking;

struct Light {
    uint type;
    float intensity;
    float attenuation;
    float innerCutoff;
    vec3 position;
    float outerCutoff;
    vec3 rotation;
    float padding;
    vec3 color;
    float shadowStrength;
    mat4 shadowMatrix;
    vec4 shadowParams;
};
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
uniform sampler2D ShadowMap2D_0;
uniform sampler2D ShadowMap2D_1;
uniform sampler2D ShadowMap2D_2;
uniform sampler2D ShadowMap2D_3;
uniform samplerCube ShadowMapCube_0;
uniform samplerCube ShadowMapCube_1;
uniform samplerCube ShadowMapCube_2;
uniform samplerCube ShadowMapCube_3;
uniform sampler2D EnvironmentMap;

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
uniform int EnvironmentEnabled;
uniform float EnvironmentStrength;

vec3 BuildWorldNormal()
{
    vec3 N = normalize(vNormal);
    vec3 T = vWorldTangent - N * dot(N, vWorldTangent);
    if (dot(T,T) < 1e-8)
        T = abs(N.y) < 0.999 ? cross(N, vec3(0,1,0)) : cross(N, vec3(1,0,0));
    T = normalize(T);
    vec3 B = normalize(cross(N,T));
    if (dot(B,vWorldBitangent) < 0.0) B = -B;
    if (TwoSided != 0 && !gl_FrontFacing) { N=-N; T=-T; B=-B; }
    vec3 nt = NormalUseTexture != 0 ? texture(T_Normal,vUV).rgb*2.0-1.0 : vec3(0,0,1);
    return normalize(T*nt.x + B*nt.y + N*nt.z);
}

float SampleShadow2D(int slot, vec2 uv)
{
    if (slot == 0) return texture(ShadowMap2D_0,uv).r;
    if (slot == 1) return texture(ShadowMap2D_1,uv).r;
    if (slot == 2) return texture(ShadowMap2D_2,uv).r;
    return texture(ShadowMap2D_3,uv).r;
}

float SampleShadowCube(int slot, vec3 dir)
{
    if (slot == 0) return texture(ShadowMapCube_0,dir).r;
    if (slot == 1) return texture(ShadowMapCube_1,dir).r;
    if (slot == 2) return texture(ShadowMapCube_2,dir).r;
    return texture(ShadowMapCube_3,dir).r;
}

int ShadowCubeResolution(int slot)
{
    if (slot == 0) return textureSize(ShadowMapCube_0, 0).x;
    if (slot == 1) return textureSize(ShadowMapCube_1, 0).x;
    if (slot == 2) return textureSize(ShadowMapCube_2, 0).x;
    return textureSize(ShadowMapCube_3, 0).x;
}

float ShadowVisibility(const Light light)
{
    if (light.shadowParams.w == 0.0 || light.shadowParams.z < 0.0) return 1.0;
    int slot = int(light.shadowParams.z + 0.5);
    float bias = max(light.shadowParams.x,0.0);
    float strength = clamp(light.shadowStrength,0.0,1.0);

    if (light.shadowParams.w > 1.5)
    {
        vec3 toSurface = vWorldPos - light.position;
        float distanceToSurface = length(toSurface);
        if (distanceToSurface <= 1e-5) return 1.0;
        vec3 dir = normalize(toSurface);
        vec3 referenceUp = abs(dir.y) < 0.99 ? vec3(0.0,1.0,0.0) : vec3(1.0,0.0,0.0);
        vec3 tangent = normalize(cross(referenceUp, dir));
        vec3 bitangent = normalize(cross(dir, tangent));
        const vec2 kernel[9] = vec2[](
            vec2(0,0), vec2(1,0), vec2(-1,0), vec2(0,1), vec2(0,-1),
            vec2(0.7071,0.7071), vec2(-0.7071,0.7071),
            vec2(0.7071,-0.7071), vec2(-0.7071,-0.7071));
        float angularRadius = 1.5 / float(max(ShadowCubeResolution(slot),1));
        float visible = 0.0;
        float referenceDepth = max(distanceToSurface / max(light.shadowParams.y,0.001) - bias / max(light.shadowParams.y,0.001), 0.0);
        for (int i=0;i<9;++i)
        {
            vec2 o = kernel[i] * angularRadius;
            vec3 sampleDir = normalize(dir + tangent*o.x + bitangent*o.y);
            float stored = SampleShadowCube(slot,sampleDir);
            visible += stored >= referenceDepth ? 1.0 : 0.0;
        }
        visible /= 9.0;
        return mix(1.0,visible,strength);
    }

    vec4 lightClip = light.shadowMatrix * vec4(vWorldPos,1.0);
    if (lightClip.w <= 1e-6) return 1.0;
    vec3 p = lightClip.xyz / lightClip.w;
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z < 0.0 || p.z > 1.0) return 1.0;
    float visible = 0.0;
    const float texel = 1.0 / 2048.0;
    for (int y=-1;y<=1;++y)
        for (int x=-1;x<=1;++x)
        {
            float d = SampleShadow2D(slot,p.xy + vec2(x,y)*texel);
            visible += (p.z - bias <= d) ? 1.0 : 0.0;
        }
    visible /= 9.0;
    return mix(1.0,visible,strength);
}

vec2 EquirectangularUV(vec3 dir)
{
    dir = normalize(dir);
    return vec2(atan(dir.z,dir.x)/(2.0*3.14159265)+0.5,
                asin(clamp(dir.y,-1.0,1.0))/3.14159265+0.5);
}

void main()
{
    vec4 albedo = texture(T_Albedo,vUV) * TintColor;
    float alphaSample = texture(T_Alpha,vUV).r;
    if (AlphaInvert != 0) alphaSample = 1.0-alphaSample;
    float alpha = albedo.a * BaseColorAlpha * (OpacityUseValue != 0 ? OpacityValue : alphaSample);
    if (alpha <= 0.001) discard;

    vec3 N = BuildWorldNormal();
    vec3 V = normalize(CamPos-vWorldPos);
    float roughSample = texture(T_Roughness,vUV).r;
    if (RoughnessInvert != 0) roughSample=1.0-roughSample;
    float rough = clamp(RoughnessUseValue != 0 ? RoughnessValue : roughSample,0.04,1.0);
    float metallic = clamp(MetallicUseValue != 0 ? MetallicValue : texture(T_Metallic,vUV).r,0.0,1.0);
    float specularValue = clamp(SpecularUseValue != 0 ? SpecularValue : texture(T_Specular,vUV).r,0.0,1.0);
    float materialAO = clamp(texture(T_AO,vUV).r,0.0,1.0);

    vec3 F0 = mix(vec3(0.04)*specularValue,albedo.rgb,metallic);
    float shininess = mix(4.0,256.0,1.0-rough);
    vec3 lighting = vec3(0.0);

    for (uint i=0u; i<count && i<32u; ++i)
    {
        Light light = lights[i];
        vec3 L = vec3(0);
        float attenuation = 1.0;
        float spot = 1.0;
        bool valid = true;

        if (light.type == 0x0012u)
        {
            L = normalize(-light.rotation);
        }
        else if (light.type == 0x0011u || light.type == 0x0013u)
        {
            vec3 toLight = light.position-vWorldPos;
            float distanceToLight = length(toLight);
            if (distanceToLight <= 1e-5) continue;
            L = toLight/distanceToLight;
            attenuation = 1.0/(1.0 + max(light.attenuation,0.0)*distanceToLight*distanceToLight);
            if (light.type == 0x0013u)
            {
                float theta = dot(L,normalize(-light.rotation));
                float inner = light.innerCutoff;
                float outer = light.outerCutoff;
                spot = clamp((theta-outer)/max(inner-outer,1e-5),0.0,1.0);
                if (spot <= 0.0) continue;
            }
        }
        else if (light.type == 0x0014u)
        {
            lighting += light.color * light.intensity * (1.0-metallic) * 0.35;
            continue;
        }
        else
        {
            valid=false;
        }
        if (!valid) continue;

        float ndl = max(dot(N,L),0.0);
        vec3 H = normalize(L+V);
        float spec = pow(max(dot(N,H),0.0),shininess) * specularValue;
        vec3 kd = (1.0-metallic)*albedo.rgb/3.14159265;
        vec3 direct = kd*ndl + F0*spec;
        float lightPower = max(light.intensity,0.0) * attenuation * spot;
        float shadow = ShadowVisibility(light);
        lighting += direct * light.color * lightPower * shadow;
    }

    vec3 ambient = albedo.rgb * (0.035 + 0.025*materialAO);
    vec3 litResult = ambient + lighting * materialAO;

    if (EnvironmentEnabled != 0 && EnvironmentStrength > 0.0)
    {
        vec3 reflectionDir = reflect(-V,N);
        vec3 environmentColor = texture(EnvironmentMap,EquirectangularUV(reflectionDir)).rgb;
        float facing = pow(1.0-max(dot(N,V),0.0),5.0);
        float reflectionAmount = clamp(EnvironmentStrength*mix(0.08,1.0,metallic),0.0,1.0);
        litResult += environmentColor * reflectionAmount * mix(0.25,1.0,facing);
    }

    if (DebugView == 0x0061)
    {
        outColor = vec4(albedo.rgb,alpha);
        outBright = vec4(0);
    }
    else if (DebugView == 0x0062)
    {
        outColor = vec4(N*0.5+0.5,alpha);
        outBright = vec4(0);
    }
    else if (DebugView == 0x0065)
    {
        outColor = vec4(DebugLODColor,alpha);
        outBright = vec4(0);
    }
    else
    {
        if (FogEnabled != 0)
        {
            float d=length(vWorldPos-CamPos);
            float f=(FogMode==0x0090) ? clamp((d-FogStart)/max(FogEnd-FogStart,1e-3),0.0,1.0) :
                    (FogMode==0x0091) ? 1.0-exp(-FogDensity*d) :
                    1.0-exp(-FogDensity*FogDensity*d*d);
            litResult=mix(litResult,FogColor.rgb,clamp(f,0.0,1.0));
        }
        vec4 finalColor=vec4(max(litResult,vec3(0)),alpha);
        float lum=dot(finalColor.rgb,vec3(0.2126,0.7152,0.0722));
        outColor=finalColor;
        outBright=lum>BrightThreshold?vec4(finalColor.rgb,1):vec4(0);
    }
    outPicking=vec4(float((ColorPickingID>>16u)&255u)/255.0,
                    float((ColorPickingID>>8u)&255u)/255.0,
                    float(ColorPickingID&255u)/255.0,1.0);
}
