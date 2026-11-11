#version 460
// Tone mapping and the colour controls of the built-in post-process material
// (HRL_DEFAULT_POST_PROCESS_SHADER), with the same meaning as on OpenGL.
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform sampler2D uBloom;
layout(set = 0, binding = 2, std140) uniform Grading
{
	vec4 color;         // brightness, contrast, saturation, hue shift (degrees)
	vec4 tint;          // rgb, invert (0 / 1)
	vec4 vignette;      // strength, radius, softness, chromatic aberration (pixels)
	vec4 vignetteColor; // rgb, sharpen strength
	vec4 grain;         // strength, scale, time (s), fade amount
	vec4 fadeColor;     // rgb, 1 when any display control is active
	vec4 screen;        // image size in pixels, 1 / size
} uGrade;
layout(push_constant) uniform Push
{
	float exposure;     // stops
	uint encodeSRGB;    // 1 when the target is not an sRGB format
	float bloom;        // share of the blurred image, 0 : off
	uint toneMap;       // 0 : the image is only clamped (toneMappingEnabled = 0)
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
vec3 Encode(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
vec3 Decode(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
float Hash12(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

vec3 SceneColor(vec2 uv)
{
	vec3 c = texture(uScene, uv).rgb;
	if (pc.bloom > 0.0) c = mix(c, texture(uBloom, uv).rgb, pc.bloom);
	return c;
}

void main()
{
	vec2 uv = vUV;
	vec3 color;
	// Chromatic aberration: red and blue sampled apart from the centre.
	float aberration = uGrade.vignette.w;
	if (aberration > 0.0001)
	{
		vec2 centered = uv - 0.5;
		float len = length(centered);
		vec2 offset = (len > 0.0001 ? centered / len : vec2(0.0)) * aberration * uGrade.screen.zw;
		color = vec3(SceneColor(clamp(uv + offset, 0.0, 1.0)).r, SceneColor(uv).g, SceneColor(clamp(uv - offset, 0.0, 1.0)).b);
	}
	else color = SceneColor(uv);
	// Sharpening against the 4 neighbours.
	float sharpen = uGrade.vignetteColor.w;
	if (sharpen > 0.0001)
	{
		vec2 t = uGrade.screen.zw;
		vec3 blur = (SceneColor(uv + vec2(-t.x, 0.0)) + SceneColor(uv + vec2(t.x, 0.0)) +
			SceneColor(uv + vec2(0.0, -t.y)) + SceneColor(uv + vec2(0.0, t.y))) * 0.25;
		color = max(vec3(0.0), color + (color - blur) * sharpen);
	}
	color *= exp2(pc.exposure) * uGrade.color.x;
	color = pc.toneMap != 0u ? clamp(ACESOutput * RRTAndODTFit(ACESInput * color), 0.0, 1.0) : clamp(color, 0.0, 1.0);

	// Display controls, applied to display values as on OpenGL.
	if (uGrade.fadeColor.w > 0.5)
	{
		vec3 d = Encode(color);
		d = (d - 0.5) * uGrade.color.y + 0.5;
		float luma = dot(d, vec3(0.2126, 0.7152, 0.0722));
		d = mix(vec3(luma), d, uGrade.color.z);
		if (abs(uGrade.color.w) > 0.0001)
		{
			float angle = radians(uGrade.color.w), c = cos(angle), s = sin(angle);
			const vec3 axis = vec3(0.57735027);
			d = d * c + cross(axis, d) * s + axis * dot(axis, d) * (1.0 - c);
		}
		d *= uGrade.tint.rgb;
		d = mix(d, 1.0 - d, uGrade.tint.w);
		if (uGrade.vignette.x > 0.0001)
		{
			float dist = length((uv - 0.5) * 2.0);
			float radius = max(uGrade.vignette.y, 0.0001), softness = max(uGrade.vignette.z, 0.0001);
			float mask = smoothstep(radius, radius + softness, dist);
			d = mix(d, uGrade.vignetteColor.rgb, clamp(mask * clamp(uGrade.vignette.x, 0.0, 1.0), 0.0, 1.0));
		}
		if (uGrade.grain.x > 0.0001)
		{
			vec2 grainUV = uv * max(uGrade.grain.y, 0.0001) * uGrade.screen.xy;
			d += (Hash12(grainUV + vec2(uGrade.grain.z * 17.13, uGrade.grain.z * 9.71)) - 0.5) * uGrade.grain.x;
		}
		d = mix(d, uGrade.fadeColor.rgb, clamp(uGrade.grain.w, 0.0, 1.0));
		color = Decode(clamp(d, 0.0, 1.0));
	}
	if (pc.encodeSRGB != 0u) color = Encode(color);
	outColor = vec4(color, 1.0);
}
