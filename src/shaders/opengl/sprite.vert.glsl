#version 330 core

//vertex/point
layout(location = 0) in vec2 apos;
layout(location = 1) in vec2 auv;
//hold location 2,3,4 and 5 (four vec4)
layout(location = 2) in mat4 amodel;

//vue/ecran
uniform mat4 view;
uniform mat4 projection;

uniform uint uSpriteID;  //utile pour le color picking buffer

out vec3 fragPos;
out vec2 uv;
flat out uint sprite_id;

void main()
{
    vec4 pos = vec4(apos, 0.0, 1.0);

    //on passe les coordonees fragment et uv au fragment shader
    fragPos = vec3(amodel * pos);
    uv = auv;
    sprite_id = uSpriteID;

    //garder cet ordre
    gl_Position = projection * view * amodel * pos;
}