//default post process fragment shader

#version 330 core

in vec2 uv;
out vec4 frag_color;

//common
uniform sampler2D uScene;
uniform sampler2D uBrightScene;
uniform vec2 uScreenSize;
uniform float uTime;

//custom
uniform float brightness = 1.0;
uniform float contrast = 1.0;
uniform float saturation = 1.0;
uniform float gamma = 1.0;
uniform vec3 tintColor = vec3(1.0);
uniform bool invertColor = false;

//sigma faible → blur serré
//float weights[5] = float[](0.2270270, 0.1945946, 0.1216216, 0.0540540, 0.0162162);

//sigma plus grand → blur plus étalé et volumétrique
float weights[9] = float[](0.1591, 0.1489, 0.1226, 0.0891, 0.0572, 0.0323, 0.0161, 0.0071, 0.0028);

vec4 ApplyGaussianBlur(sampler2D tex)
{
    vec2 texOffset = vec2(1.0 / textureSize(tex, 0));
    vec3 result = vec3(0.0);
    float totalWeight = 0.0;

    //kernel 17x17
    for (int x = -4; x <= 4; x++)
    {
        for (int y = -4; y <= 4; y++)
        {
            float w = weights[abs(x)] * weights[abs(y)];
            result += texture(tex, uv + vec2(texOffset.x * x, texOffset.y * y)).rgb * w;
            totalWeight += w;
        }
    }

    return vec4(result / totalWeight, 1.0);
}

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

    vec4 bright_color_blur = ApplyGaussianBlur(uBrightScene);

    frag_color = vec4(color, 1.0) + bright_color_blur;
}