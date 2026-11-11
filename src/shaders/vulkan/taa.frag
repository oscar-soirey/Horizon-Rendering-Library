#version 460
// Temporal anti-aliasing: the jittered image of this frame is blended with the
// history reprojected from the depth (camera motion). The history is clamped
// to the colours around the pixel, which removes the ghosts of moving objects
// and disocclusions.
layout(set = 0, binding = 0) uniform sampler2D uCurrent; // HDR, this frame
layout(set = 0, binding = 1) uniform sampler2D uHistory; // resolved image of the previous frame
layout(set = 0, binding = 2) uniform sampler2D uDepth;   // reversed-Z
layout(push_constant) uniform Push
{
	mat4 reproject;   // this frame's clip space (jittered) -> previous frame's clip space
	vec4 rect;        // viewport in pixels: x, y, width, height (origin top-left)
	vec4 params;      // xy : 1 / image size, z : 1 = no history, w : weight of this frame
} pc;
layout(location = 0) in vec2 vUV; // 0..1 over the viewport, origin top-left
layout(location = 0) out vec4 outColor;

vec3 ToYCoCg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 FromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
// Tone mapped weights: one very bright sample does not dominate (no flicker).
float Weight(vec3 c) { return 1.0 / (1.0 + max(c.r, max(c.g, c.b))); }

void main()
{
	vec2 pixel = pc.rect.xy + vUV * pc.rect.zw;
	vec2 uv = pixel * pc.params.xy;
	vec3 current = textureLod(uCurrent, uv, 0.0).rgb;
	if (pc.params.z > 0.5) { outColor = vec4(current, 1.0); return; }

	// Colour box of the 3x3 neighbourhood (YCoCg, mean +- deviation), and the
	// nearest depth around the pixel (antialiased edges reproject with the object).
	vec3 m1 = vec3(0.0), m2 = vec3(0.0);
	float depth = 0.0;
	vec2 depthUV = uv;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			vec2 o = vec2(x, y) * pc.params.xy;
			vec3 c = ToYCoCg(textureLod(uCurrent, uv + o, 0.0).rgb);
			m1 += c;
			m2 += c * c;
			float d = textureLod(uDepth, uv + o, 0.0).r;
			if (d > depth) { depth = d; depthUV = uv + o; }
		}
	vec3 mean = m1 / 9.0;
	vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
	vec3 boxMin = mean - 1.25 * sigma, boxMax = mean + 1.25 * sigma;

	// Reprojection of the nearest surface into the previous frame.
	vec2 local = (depthUV / pc.params.xy - pc.rect.xy) / pc.rect.zw;
	vec4 clip = vec4(local.x * 2.0 - 1.0, 1.0 - local.y * 2.0, depth, 1.0);
	vec4 previous = pc.reproject * clip;
	vec2 prevNdc = previous.xy / previous.w;
	vec2 prevLocal = vec2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
	vec2 velocity = prevLocal - local;
	vec2 prevUV = (pc.rect.xy + (vUV + velocity) * pc.rect.zw) * pc.params.xy;
	bool inside = all(greaterThanEqual(vUV + velocity, vec2(0.0))) && all(lessThanEqual(vUV + velocity, vec2(1.0)));
	if (!inside || previous.w <= 0.0) { outColor = vec4(current, 1.0); return; }

	vec3 history = ToYCoCg(textureLod(uHistory, prevUV, 0.0).rgb);
	history = FromYCoCg(clamp(history, boxMin, boxMax));
	// Faster convergence when the camera moves (less blur).
	float speed = length(velocity * pc.rect.zw);
	float alpha = clamp(pc.params.w + speed * 0.02, pc.params.w, 0.5);
	float wc = Weight(current) * alpha, wh = Weight(history) * (1.0 - alpha);
	outColor = vec4((current * wc + history * wh) / (wc + wh), 1.0);
}
