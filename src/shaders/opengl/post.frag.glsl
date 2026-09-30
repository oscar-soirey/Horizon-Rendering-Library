//default post process fragment shader

#version 330 core

in vec2 uv;
out vec4 frag_color;

// Common
uniform sampler2D uScene;
uniform sampler2D uBrightScene;
uniform vec2 uScreenSize;
uniform float uTime;

// Color controls
uniform float brightness = 1.0;
uniform float contrast = 1.0;
uniform float saturation = 1.0;
uniform float gamma = 2.2;
uniform float exposure = 0.0;
uniform float hueShift = 0.0;          // degrees
uniform vec3 tintColor = vec3(1.0);
uniform bool invertColor = false;

// Bloom
// 1.0 means HDR bright pixels keep their natural energy in the bloom.
// Set to 5.0, for example, to amplify the glow fivefold.
uniform float bloomStrength = 1.0;

// Screen-space effects
uniform float sharpenStrength = 0.0;   // 0 = disabled, 1 = full strength
uniform float chromaticAberration = 0.0; // pixels
uniform float filmGrainStrength = 0.0; // 0..1
uniform float filmGrainScale = 1.0;

// Optional tone mapping / display transform. Disabled by default so a post-process
// with no explicit color controls preserves the scene color exactly.
uniform int toneMappingEnabled = 0;
uniform int gammaCorrectionEnabled = 0;

// Global HDR display compression. Scene lighting stays HDR until after bloom;
// this is the single place where values above the display range are compressed.
// Values below the knee are untouched, while highlights approach 1 smoothly.
uniform int hdrDisplayCompressionEnabled = 1;
uniform float hdrDisplayKnee = 0.90;
uniform float hdrDisplayCompression = 0.75;

// Vignette
uniform float vignetteStrength = 0.0;  // 0..1
uniform float vignetteRadius = 0.75;   // normalized radius
uniform float vignetteSoftness = 0.25;
uniform vec3 vignetteColor = vec3(0.0);

// Fade / color overlay
uniform float fadeAmount = 0.0;        // 0..1
uniform vec3 fadeColor = vec3(0.0);

// sigma faible -> blur serre
float weights[5] = float[](0.2270270, 0.1945946, 0.1216216, 0.0540540, 0.0162162);

vec4 ApplyGaussianBlur(sampler2D tex)
{
    vec2 texOffset = vec2(1.0 / textureSize(tex, 0));
    vec3 result = vec3(0.0);
    float totalWeight = 0.0;

    // kernel 17x17
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

vec3 SampleChromaticScene()
{
    if (chromaticAberration <= 0.0001)
        return texture(uScene, uv).rgb;

    vec2 centered = uv - vec2(0.5);
    float lenCenter = length(centered);
    vec2 direction = lenCenter > 0.0001 ? centered / lenCenter : vec2(0.0);
    vec2 offset = direction * (chromaticAberration / max(uScreenSize, vec2(1.0)));

    float r = texture(uScene, clamp(uv + offset, 0.0, 1.0)).r;
    float g = texture(uScene, uv).g;
    float b = texture(uScene, clamp(uv - offset, 0.0, 1.0)).b;
    return vec3(r, g, b);
}

vec3 ApplySharpen(vec3 color)
{
    if (sharpenStrength <= 0.0001)
        return color;

    vec2 texel = 1.0 / max(uScreenSize, vec2(1.0));
    vec3 left  = texture(uScene, clamp(uv + vec2(-texel.x, 0.0), 0.0, 1.0)).rgb;
    vec3 right = texture(uScene, clamp(uv + vec2( texel.x, 0.0), 0.0, 1.0)).rgb;
    vec3 up    = texture(uScene, clamp(uv + vec2(0.0,  texel.y), 0.0, 1.0)).rgb;
    vec3 down  = texture(uScene, clamp(uv + vec2(0.0, -texel.y), 0.0, 1.0)).rgb;

    vec3 blur = (left + right + up + down) * 0.25;
    return max(vec3(0.0), color + (color - blur) * sharpenStrength);
}

vec3 ApplyHueShift(vec3 color, float degrees)
{
    if (abs(degrees) <= 0.0001)
        return color;

    float angle = radians(degrees);
    float c = cos(angle);
    float s = sin(angle);

    // Rodrigues rotation around the luminance axis.
    const vec3 axis = normalize(vec3(1.0, 1.0, 1.0));
    return color * c + cross(axis, color) * s + axis * dot(axis, color) * (1.0 - c);
}

float Hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 CompressHDRForDisplay(vec3 color)
{
    color = max(color, vec3(0.0));
    float peak = max(color.r, max(color.g, color.b));
    float knee = clamp(hdrDisplayKnee, 0.0, 0.999);
    float compression = max(hdrDisplayCompression, 0.0001);

    if (peak <= knee)
        return color;

    // Smooth asymptotic shoulder. Scaling the whole RGB vector together
    // preserves the emissive/light color instead of independently clipping
    // channels toward white. The long shoulder keeps spatial lighting
    // gradients visible instead of producing a bright plateau around emitters.
    float excess = peak - knee;
    float limitedPeak = knee + (1.0 - knee) * (excess / (excess + compression));
    return color * (limitedPeak / max(peak, 1e-6));
}

void main()
{
    // Scene sample, optionally with radial chromatic aberration.
    vec3 color = SampleChromaticScene();

    // Exposure is expressed in stops: +1 doubles the linear HDR light, -1 halves it.
    color *= exp2(exposure);

    // Brightness in linear HDR space.
    color *= brightness;

    // Bloom must be combined before tone mapping so bright HDR highlights are
    // compressed together with the main image.
    vec3 bloom = ApplyGaussianBlur(uBrightScene).rgb * bloomStrength;
    color += bloom;

    // Apply an optional tone mapper when explicitly requested. Otherwise use
    // one global soft HDR shoulder so lighting gradients stay smooth instead
    // of locally clipping every pixel above 1.0 into the same white value.
    if (toneMappingEnabled != 0)
    {
        vec3 x = max(color, vec3(0.0));
        const float a = 2.51;
        const float b = 0.03;
        const float c = 2.43;
        const float d = 0.59;
        const float e = 0.14;
        color = clamp((x * (a * x + b)) / max(x * (c * x + d) + e, vec3(1e-5)), 0.0, 1.0);
    }
    else if (hdrDisplayCompressionEnabled != 0)
    {
        color = CompressHDRForDisplay(color);
    }

    // Contrast.
    color = (color - 0.5) * contrast + 0.5;

    // Saturation.
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, saturation);

    // Hue rotation.
    color = ApplyHueShift(color, hueShift);

    // Gamma correction is also opt-in.
    if (gammaCorrectionEnabled != 0)
        color = pow(max(color, vec3(0.0)), vec3(1.0 / max(gamma, 0.0001)));

    // Tint.
    color *= tintColor;

    // Optional sharpening of the scene image.
    color = ApplySharpen(color);

    // Color inversion.
    color = mix(color, 1.0 - color, float(invertColor));

    // Vignette. Radius is normalized to the distance from the center to a screen edge.
    // Strength accepts both the documented 0..1 range and percentage-style values
    // such as 100.0 (which is clamped to full strength).
    if (vignetteStrength > 0.0001)
    {
        vec2 centered = (uv - vec2(0.5)) * 2.0;
        float dist = length(centered);
        float radius = max(vignetteRadius, 0.0001);
        float softness = max(vignetteSoftness, 0.0001);
        float mask = smoothstep(radius, radius + softness, dist);
        float strength = clamp(vignetteStrength, 0.0, 1.0);
        color = mix(color, vignetteColor, clamp(mask * strength, 0.0, 1.0));
    }

    // Animated film grain.
    if (filmGrainStrength > 0.0001)
    {
        vec2 grainUV = uv * max(filmGrainScale, 0.0001) * uScreenSize;
        float noise = Hash12(grainUV + vec2(uTime * 17.13, uTime * 9.71));
        float grain = (noise - 0.5) * filmGrainStrength;
        color += grain;
    }

    // Final color fade.
    color = mix(color, fadeColor, clamp(fadeAmount, 0.0, 1.0));

    frag_color = vec4(max(color, vec3(0.0)), 1.0);
}
