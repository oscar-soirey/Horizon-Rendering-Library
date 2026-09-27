#version 450
layout(location=0) in vec3 aPosition;
layout(location=0) out vec3 vWorldPos;
uniform mat4 lightSpaceMatrix;
uniform mat4 model;
void main(){ vec4 w=model*vec4(aPosition,1.0); vWorldPos=w.xyz; gl_Position=lightSpaceMatrix*w; }
