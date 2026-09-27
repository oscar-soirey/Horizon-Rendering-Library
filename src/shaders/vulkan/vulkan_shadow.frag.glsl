#version 450
layout(location=0) in vec3 vWorldPos;
uniform int uPointShadow;
uniform vec3 uLightPosition;
uniform float uFarPlane;
void main(){
    if(uPointShadow!=0){
        gl_FragDepth=clamp(length(vWorldPos-uLightPosition)/max(uFarPlane,0.001),0.0,1.0);
    }
}
