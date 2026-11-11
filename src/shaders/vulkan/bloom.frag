#version 460
// Bloom chain. mode 0 : first downsample of the HDR image (weighted against
// fireflies), mode 1 : downsample, mode 2 : upsample (added to the larger level).
layout(set = 0, binding = 0) uniform sampler2D uSource;
layout(push_constant) uniform Push
{
	vec2 texel;   // 1 / source size
	uint mode;
} pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float Luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main()
{
	vec3 color;
	if (pc.mode == 2u)
	{
		// 3x3 tent filter.
		vec2 d = pc.texel;
		color = texture(uSource, vUV).rgb * 4.0;
		color += (texture(uSource, vUV + vec2(d.x, 0.0)).rgb + texture(uSource, vUV - vec2(d.x, 0.0)).rgb +
		          texture(uSource, vUV + vec2(0.0, d.y)).rgb + texture(uSource, vUV - vec2(0.0, d.y)).rgb) * 2.0;
		color += texture(uSource, vUV + d).rgb + texture(uSource, vUV - d).rgb +
		         texture(uSource, vUV + vec2(d.x, -d.y)).rgb + texture(uSource, vUV + vec2(-d.x, d.y)).rgb;
		color /= 16.0;
	}
	else
	{
		// Four bilinear taps: a 4x4 box of the source.
		vec3 a = texture(uSource, vUV + pc.texel * vec2(-1.0, -1.0)).rgb;
		vec3 b = texture(uSource, vUV + pc.texel * vec2(1.0, -1.0)).rgb;
		vec3 c = texture(uSource, vUV + pc.texel * vec2(-1.0, 1.0)).rgb;
		vec3 d = texture(uSource, vUV + pc.texel * vec2(1.0, 1.0)).rgb;
		if (pc.mode == 0u)
		{
			// Karis average: very bright isolated pixels do not flicker.
			float wa = 1.0 / (1.0 + Luma(a)), wb = 1.0 / (1.0 + Luma(b));
			float wc = 1.0 / (1.0 + Luma(c)), wd = 1.0 / (1.0 + Luma(d));
			color = (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
		}
		else color = (a + b + c + d) * 0.25;
	}
	outColor = vec4(max(color, vec3(0.0)), 1.0);
}
