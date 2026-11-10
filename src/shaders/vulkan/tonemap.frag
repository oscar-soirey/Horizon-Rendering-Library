#version 460
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(push_constant) uniform Push
{
	float exposure;     // stops
	uint encodeSRGB;    // 1 when the target is not an sRGB format
} pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// ACES fitted (Stephen Hill): RRT + ODT approximation in linear sRGB.
const mat3 ACESInput = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
const mat3 ACESOutput = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);

vec3 RRTAndODTFit(vec3 v)
{
	vec3 a = v * (v + 0.0245786) - 0.000090537;
	vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
	return a / b;
}

void main()
{
	vec3 color = texture(uScene, vUV).rgb * exp2(pc.exposure);
	color = clamp(ACESOutput * RRTAndODTFit(ACESInput * color), 0.0, 1.0);
	if (pc.encodeSRGB != 0u)
		color = mix(color * 12.92, 1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, color));
	outColor = vec4(color, 1.0);
}
