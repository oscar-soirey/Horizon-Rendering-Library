#version 460
// Voxel light field: box filtered pyramid (mode 0), then the weighted sum of
// the levels, coarsest first (mode 1), as the OpenGL backend.
layout(set = 0, binding = 0) uniform sampler2D uCurrent;
layout(set = 0, binding = 1) uniform sampler2D uLow;
layout(push_constant) uniform Push
{
	vec2 texel;     // mode 0 : texel of the source, mode 1 : texel of the coarser level
	float weight;   // mode 1
	uint mode;      // 0 : downsample, 1 : accumulate, 2 : accumulate without coarser level
} pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main()
{
	if (pc.mode == 0u)
	{
		vec3 sum = texture(uCurrent, vUV + vec2(-1.0, -1.0) * pc.texel).rgb + texture(uCurrent, vUV + vec2(1.0, -1.0) * pc.texel).rgb +
			texture(uCurrent, vUV + vec2(-1.0, 1.0) * pc.texel).rgb + texture(uCurrent, vUV + vec2(1.0, 1.0) * pc.texel).rgb;
		outColor = vec4(sum * 0.25, 1.0);
		return;
	}
	vec3 light = pc.weight * texture(uCurrent, vUV).rgb;
	if (pc.mode == 1u)
	{
		vec2 t = pc.texel;
		vec3 sum = texture(uLow, vUV + vec2(-1.0, -1.0) * t).rgb + texture(uLow, vUV + vec2(0.0, -1.0) * t).rgb * 2.0 +
			texture(uLow, vUV + vec2(1.0, -1.0) * t).rgb + texture(uLow, vUV + vec2(-1.0, 0.0) * t).rgb * 2.0 +
			texture(uLow, vUV).rgb * 4.0 + texture(uLow, vUV + vec2(1.0, 0.0) * t).rgb * 2.0 +
			texture(uLow, vUV + vec2(-1.0, 1.0) * t).rgb + texture(uLow, vUV + vec2(0.0, 1.0) * t).rgb * 2.0 +
			texture(uLow, vUV + vec2(1.0, 1.0) * t).rgb;
		light += sum * (1.0 / 16.0);
	}
	outColor = vec4(light, 1.0);
}
