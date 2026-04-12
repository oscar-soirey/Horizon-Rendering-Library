#version 330 core

layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;     //(lighting)
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec3 aTangent;    //(normal map)
layout (location = 4) in vec3 aBitangent;  //(normal map)
//hold location 5, 6, 7 and 8
layout (location = 5) in mat4 aModel;

