#version 330 core

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 BrightColor;
layout(location = 2) out vec4 ColorPickingBuffer;

in vec3 skyDirection;

uniform vec3 SkyTopColor;
uniform vec3 SkyHorizonColor;
uniform vec3 SkyBottomColor;
uniform sampler2D SkyTexture;
uniform int SkyTextureEnabled;

vec2 EquirectangularUV(vec3 direction)
{
    direction = normalize(direction);
    float longitude = atan(direction.z, direction.x);
    float latitude = asin(clamp(direction.y, -1.0, 1.0));

    // HRL textures are vertically flipped by stb_image on upload.
    return vec2(
        longitude / (2.0 * 3.14159265359) + 0.5,
        clamp(0.5 + latitude / 3.14159265359, 0.001, 0.999)
    );
}

void main()
{
    if (SkyTextureEnabled != 0)
    {
        FragColor = texture(SkyTexture, EquirectangularUV(skyDirection));
    }
    else
    {
        float h = clamp(normalize(skyDirection).y, -1.0, 1.0);

        vec3 color;
        if (h >= 0.0)
        {
            float t = smoothstep(0.0, 1.0, h);
            color = mix(SkyHorizonColor, SkyTopColor, t);
        }
        else
        {
            float t = smoothstep(-1.0, 0.0, h);
            color = mix(SkyBottomColor, SkyHorizonColor, t);
        }

        FragColor = vec4(color, 1.0);
    }

    BrightColor = vec4(0.0);
    ColorPickingBuffer = vec4(0.0);
}
