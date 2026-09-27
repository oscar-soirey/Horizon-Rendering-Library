#version 450

layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outBright;
layout(location=2) out vec4 outPicking;

uniform sampler2D uSource;

void main()
{
    vec4 color = texture(uSource, vUV);
    outColor = color;
    outBright = vec4(0.0);
    outPicking = vec4(0.0);
}
