#version 450
layout(location=0) in vec3 aPosition;
layout(location=2) in vec2 aTexCoord;
layout(location=0) out vec2 vUV;
uniform mat4 projection;
uniform mat4 view;
uniform mat4 model;
uniform vec4 UVRegion;
void main(){ vUV=aTexCoord*UVRegion.zw+UVRegion.xy; gl_Position=projection*view*model*vec4(aPosition,1.0); }
