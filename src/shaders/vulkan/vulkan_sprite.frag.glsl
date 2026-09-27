#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outBright;
layout(location=2) out vec4 outPicking;
uniform sampler2D T_Albedo;
uniform vec4 TintColor;
uniform uint ColorPickingID;
void main(){ vec4 c=texture(T_Albedo,vUV)*TintColor; if(c.a<0.01) discard; outColor=c; outBright=vec4(0.0); outPicking=vec4(float((ColorPickingID>>0u)&255u)/255.0,float((ColorPickingID>>8u)&255u)/255.0,float((ColorPickingID>>16u)&255u)/255.0,1.0); }
