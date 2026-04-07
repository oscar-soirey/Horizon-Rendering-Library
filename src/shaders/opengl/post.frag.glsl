//default post process fragment shader

#version 330 core

in vec2 uv;
out vec4 frag_color;

//common
uniform sampler2D uScene;
uniform vec2 uScreenSize;
uniform float uTime;

//custom
uniform float brightness = 1.0;
uniform float contrast = 1.0;
uniform float saturation = 1.0;
uniform float gamma = 1.0;
uniform vec3 tintColor = vec3(1.0);
uniform bool invertColor = false;

void main()
{
    vec3 color = texture(uScene, uv).rgb;

    //brightness
    color *= brightness;

    //contrast
    color = (color - 0.5) * contrast + 0.5;

    //saturation
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, saturation);

    //gamma
    color = pow(color, vec3(1.0 / gamma));

    //tint color
    color *= tintColor;

    //color inversion
    color = mix(color, 1.0 - color, float(invertColor));

    frag_color = vec4(color, 1.0);
}