#version 450
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
