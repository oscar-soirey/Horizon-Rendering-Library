#version 460
layout(location = 0) in vec3 vEmission;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(vEmission, 1.0); }
