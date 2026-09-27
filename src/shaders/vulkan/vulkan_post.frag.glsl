#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;

uniform sampler2D uScene;
uniform sampler2D uBrightScene;
uniform vec2 uScreenSize;
uniform float uTime;

uniform float brightness;
uniform float contrast;
uniform float saturation;
uniform float gamma;
uniform float exposure;
uniform float hueShift;
uniform vec3 tintColor;
uniform bool invertColor;

uniform float bloomStrength;
uniform float sharpenStrength;
uniform float chromaticAberration;
uniform float filmGrainStrength;
uniform float filmGrainScale;

uniform float vignetteStrength;
uniform float vignetteRadius;
uniform float vignetteSoftness;
uniform vec3 vignetteColor;

uniform float fadeAmount;
uniform vec3 fadeColor;

const float PI = 3.14159265359;
const float weights[5] = float[5](0.2270270, 0.1945946, 0.1216216, 0.0540542, 0.0162162);

vec4 ApplyGaussianBlur(sampler2D tex)
{
    vec2 texOffset = vec2(1.0) / vec2(textureSize(tex, 0));
    vec3 result = vec3(0.0);
    float totalWeight = 0.0;
    for (int x = -4; x <= 4; ++x)
        for (int y = -4; y <= 4; ++y)
        {
            float w = weights[abs(x)] * weights[abs(y)];
            result += texture(tex, clamp(vUV + vec2(texOffset.x * x, texOffset.y * y), 0.0, 1.0)).rgb * w;
            totalWeight += w;
        }
    return vec4(result / max(totalWeight, 1e-6), 1.0);
}

vec3 SampleChromaticScene()
{
    if (chromaticAberration <= 0.0001) return texture(uScene, vUV).rgb;
    vec2 centered = vUV - vec2(0.5);
    float lenCenter = length(centered);
    vec2 direction = lenCenter > 0.0001 ? centered / lenCenter : vec2(0.0);
    vec2 offset = direction * (chromaticAberration / max(uScreenSize, vec2(1.0)));
    float r = texture(uScene, clamp(vUV + offset, 0.0, 1.0)).r;
    float g = texture(uScene, vUV).g;
    float b = texture(uScene, clamp(vUV - offset, 0.0, 1.0)).b;
    return vec3(r, g, b);
}

vec3 ApplySharpen(vec3 color)
{
    if (sharpenStrength <= 0.0001) return color;
    vec2 texel = 1.0 / max(uScreenSize, vec2(1.0));
    vec3 left  = texture(uScene, clamp(vUV + vec2(-texel.x, 0.0), 0.0, 1.0)).rgb;
    vec3 right = texture(uScene, clamp(vUV + vec2( texel.x, 0.0), 0.0, 1.0)).rgb;
    vec3 up    = texture(uScene, clamp(vUV + vec2(0.0,  texel.y), 0.0, 1.0)).rgb;
    vec3 down  = texture(uScene, clamp(vUV + vec2(0.0, -texel.y), 0.0, 1.0)).rgb;
    vec3 blur = (left + right + up + down) * 0.25;
    return max(vec3(0.0), color + (color - blur) * sharpenStrength);
}

vec3 ApplyHueShift(vec3 color, float degrees)
{
    if (abs(degrees) <= 0.0001) return color;
    float angle = radians(degrees);
    float c = cos(angle);
    float s = sin(angle);
    const vec3 axis = normalize(vec3(1.0));
    return color * c + cross(axis, color) * s + axis * dot(axis, color) * (1.0 - c);
}

float Hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main()
{
    vec3 color = SampleChromaticScene();
    color *= exp2(exposure);
    color *= brightness;
    color = (color - 0.5) * contrast + 0.5;

    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, saturation);
    color = ApplyHueShift(color, hueShift);
    color = pow(max(color, vec3(0.0)), vec3(1.0 / max(gamma, 0.0001)));
    color *= tintColor;
    color = ApplySharpen(color);
    if (invertColor) color = 1.0 - color;

    color += ApplyGaussianBlur(uBrightScene).rgb * bloomStrength;

    if (vignetteStrength > 0.0001)
    {
        vec2 centered = vUV - vec2(0.5);
        float dist = length(centered);
        float radius = max(vignetteRadius, 0.0001);
        float softness = max(vignetteSoftness, 0.0001);
        float mask = smoothstep(radius, radius + softness, dist);
        color = mix(color, vignetteColor, mask * vignetteStrength);
    }

    if (filmGrainStrength > 0.0001)
    {
        vec2 grainUV = vUV * max(filmGrainScale, 0.0001) * uScreenSize;
        float noise = Hash12(grainUV + vec2(uTime * 17.13, uTime * 9.71));
        color += (noise - 0.5) * filmGrainStrength;
    }

    color = mix(color, fadeColor, clamp(fadeAmount, 0.0, 1.0));
    outColor = vec4(max(color, vec3(0.0)), 1.0);
}
