#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
uniform sampler2D uScene;
uniform sampler2D uBrightScene;
uniform sampler2D uDepth;
uniform mat4 uInvViewProjection;
uniform vec3 uCameraPos;
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uScreenSize;
uniform int uAOEnabled;
uniform float uAOStrength;
uniform float uAORadius;
uniform float uAOBias;
uniform float uAOPower;
uniform int uVolumetricFogCount;
uniform vec4 uFogPosRadius[64];
uniform vec4 uFogColorDensity[64];
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

vec2 GlobalUV(vec2 localUV){return uViewportOrigin+localUV*uViewportSize;}
vec3 ReconstructWorld(vec2 localUV,float depth){
    // HRL's Vulkan projection maps OpenGL depth [-1,1] to Vulkan [0,1].
    vec4 clip=vec4(localUV*2.0-1.0,depth,1.0);
    vec4 w=uInvViewProjection*clip;
    return abs(w.w)<1e-6?uCameraPos:w.xyz/w.w;
}

float DepthAO(vec2 globalUV,vec3 worldPos,float depth){
    if(uAOEnabled==0||uAOStrength<=0.0||uAORadius<=0.0||depth>=0.99999)return 1.0;
    const vec2 kernel[16]=vec2[](
        vec2(1,0),vec2(-1,0),vec2(0,1),vec2(0,-1),
        vec2(0.707,0.707),vec2(-0.707,0.707),vec2(0.707,-0.707),vec2(-0.707,-0.707),
        vec2(0.383,0.924),vec2(-0.383,0.924),vec2(0.383,-0.924),vec2(-0.383,-0.924),
        vec2(0.924,0.383),vec2(-0.924,0.383),vec2(0.924,-0.383),vec2(-0.924,-0.383));
    float distanceToCamera=max(length(worldPos-uCameraPos),1.0);
    float radiusPixels=clamp(uAORadius*0.5*min(uScreenSize.x,uScreenSize.y)/distanceToCamera,1.0,32.0);
    float occ=0.0;
    for(int i=0;i<16;++i){
        float scale=mix(0.35,1.0,float(i)/15.0);
        vec2 suv=globalUV+kernel[i]*(radiusPixels*scale)/uScreenSize;
        if(any(lessThan(suv,uViewportOrigin))||any(greaterThan(suv,uViewportOrigin+uViewportSize)))continue;
        float sd=texture(uDepth,suv).r;
        if(sd>=0.99999)continue;
        vec3 sp=ReconstructWorld((suv-uViewportOrigin)/max(uViewportSize,vec2(1e-6)),sd);
        vec3 d=sp-worldPos;
        float dist=length(d);
        if(dist<=1e-5||dist>uAORadius)continue;
        float farther=smoothstep(uAOBias,uAOBias+max(0.01,uAORadius*0.08),length(sp-uCameraPos)-distanceToCamera);
        float facing=max(dot(normalize(-d),normalize(worldPos-uCameraPos)),0.0);
        float rangeWeight=1.0-smoothstep(0.0,uAORadius,dist);
        occ+=farther*facing*rangeWeight;
    }
    return 1.0-uAOStrength*pow(clamp(occ/16.0,0.0,1.0),uAOPower);
}

vec4 ApplyFog(vec4 base,vec3 worldEnd){
    bool globalOn=uGlobalVolumetricFogEnabled!=0&&uGlobalFogDensity>0.0;
    bool localOn=uVolumetricFogCount>0;
    if(!globalOn&&!localOn)return base;
    vec3 ray=worldEnd-uCameraPos;float lenRay=length(ray);if(lenRay<=1e-4)return base;
    vec3 dir=ray/lenRay;
    int steps=clamp(max(uGlobalFogSteps,uVolumetricFogSteps),4,64);
    float stepLen=lenRay/float(steps);float trans=1.0;vec3 scatter=vec3(0);
    for(int i=0;i<64;++i){
        if(i>=steps)break;float t=(float(i)+0.5)/float(steps);vec3 pos=uCameraPos+dir*(lenRay*t);
        float density=0.0;vec3 weighted=vec3(0);
        if(globalOn){density+=uGlobalFogDensity;weighted+=uGlobalFogDensity*uGlobalFogColor;}
        for(int j=0;j<64;++j){if(j>=uVolumetricFogCount)break;float radius=uFogPosRadius[j].w;float d=uFogColorDensity[j].w;if(radius<=0||d<=0)continue;float n=1.0-length(pos-uFogPosRadius[j].xyz)/radius;if(n<=0)continue;float ld=n*n*d;density+=ld;weighted+=ld*uFogColorDensity[j].rgb;}
        if(density<=0)continue;float a=1.0-exp(-density*stepLen);scatter+=trans*a*(weighted/density);trans*=1.0-a;if(trans<0.01)break;
    }
    return vec4(base.rgb*trans+scatter,base.a);
}

vec4 ApplyGodRays(vec4 base,vec2 globalUV){
    if(uGodRaysEnabled==0||uGodRaysWeight<=0.0||uGodRaysDensity<=0.0)return base;
    int samples=clamp(uGodRaysSamples,8,96);vec2 lightUV=uViewportOrigin+uGodRaysLightUV*uViewportSize;
    vec2 delta=(globalUV-lightUV)*(uGodRaysDensity/float(samples));vec2 suv=globalUV;vec3 rays=vec3(0);float illum=1;
    for(int i=0;i<96;++i){if(i>=samples)break;suv-=delta;if(any(lessThan(suv,vec2(0)))||any(greaterThan(suv,vec2(1))))break;vec3 sceneC=texture(uScene,suv).rgb;vec3 bright=texture(uBrightScene,suv).rgb;float l=dot(sceneC,vec3(0.2126,0.7152,0.0722));rays+=(bright+max(l-0.55,0.0)*sceneC*0.35)*illum;illum*=uGodRaysDecay;}
    base.rgb+=rays/float(samples)*uGodRaysColor*uGodRaysWeight;return base;
}
void main(){
    vec2 globalUV=GlobalUV(vUV);
    vec4 color=texture(uScene,globalUV);
    float depth=texture(uDepth,globalUV).r;
    vec3 world=ReconstructWorld(vUV,depth);
    float ao=DepthAO(globalUV,world,depth);
    color.rgb*=ao;
    // Volumetric fog is evaluated along the camera ray for both geometry and
    // background pixels, matching the OpenGL backend.
    color=ApplyFog(color,world);
    color=ApplyGodRays(color,globalUV);
    outColor=color;
}
