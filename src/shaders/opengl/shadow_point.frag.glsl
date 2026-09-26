#version 330 core

in float shadowDepth;

void main()
{
    gl_FragDepth = shadowDepth;
}
