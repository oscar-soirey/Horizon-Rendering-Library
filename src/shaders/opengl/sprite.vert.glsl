#version 330 core

// Sprite = mesh 3D plane. The default sprite path supports instanced rendering.
layout(location = 0) in vec3 aPosition;
layout(location = 2) in vec2 aTexCoord;
layout(location = 5) in mat4 aInstanceModel;
layout(location = 9) in vec4 aInstanceUVRegion;
layout(location = 10) in uint aInstanceSpriteID;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec4 UVRegion;
uniform uint uSpriteID;
uniform int uInstanced;

out vec3 fragPos;
out vec2 uv;
flat out uint sprite_id;

void main()
{
    vec4 pos = vec4(aPosition, 1.0);
    vec4 worldPos;
    vec4 region;
    uint id;

    if (uInstanced != 0)
    {
        worldPos = aInstanceModel * pos;
        region = aInstanceUVRegion;
        id = aInstanceSpriteID;
    }
    else
    {
        worldPos = model * pos;
        region = UVRegion;
        id = uSpriteID;
    }

    fragPos = worldPos.xyz;
    uv = mix(region.xy, region.zw, aTexCoord);
    sprite_id = id;
    gl_Position = projection * view * worldPos;
}
