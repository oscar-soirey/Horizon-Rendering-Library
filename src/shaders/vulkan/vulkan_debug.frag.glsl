#version 450
layout(location=0) in vec3 vColor; layout(location=0) out vec4 outColor; layout(location=1) out vec4 outBright; layout(location=2) out vec4 outPicking;
void main(){outColor=vec4(vColor,1.0);outBright=vec4(0.0);outPicking=vec4(0.0);}
