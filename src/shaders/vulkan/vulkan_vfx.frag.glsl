#version 450
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
