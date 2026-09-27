#version 450
layout(location=0) in vec3 aPosition; layout(location=1) in vec3 aColor; layout(location=0) out vec3 vColor;
uniform mat4 projection; uniform mat4 view;
void main(){vColor=aColor;gl_Position=projection*view*vec4(aPosition,1.0);}
