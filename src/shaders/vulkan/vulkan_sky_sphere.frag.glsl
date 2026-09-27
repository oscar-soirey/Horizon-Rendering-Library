#version 450
layout(location=0) in vec2 vUV; layout(location=0) out vec4 outColor; layout(location=1) out vec4 outBright; layout(location=2) out vec4 outPicking;
uniform mat4 projection; uniform mat4 view; uniform mat4 skyRotation; uniform vec3 SkyTopColor; uniform vec3 SkyHorizonColor; uniform vec3 SkyBottomColor; uniform sampler2D SkyTexture; uniform int SkyUseTexture;
void main(){
 vec4 clip=vec4(vUV*2.0-1.0,1.0,1.0); mat4 invPV=inverse(projection*mat4(mat3(view))); vec3 dir=normalize((invPV*clip).xyz); dir=normalize((mat3(skyRotation)*dir));
 vec3 c;
 if(SkyUseTexture!=0){float lon=atan(dir.z,dir.x)/(2.0*3.14159265)+0.5;float lat=asin(clamp(dir.y,-1.0,1.0))/3.14159265+0.5;c=texture(SkyTexture,vec2(lon,lat)).rgb;}
 else {float t=smoothstep(0.0,1.0,dir.y*0.5+0.5);vec3 hc=mix(SkyHorizonColor,SkyTopColor,t);float b=smoothstep(0.0,1.0,-dir.y);c=mix(hc,SkyBottomColor,b);}
 outColor=vec4(c,1);outBright=vec4(0);outPicking=vec4(0);
}
