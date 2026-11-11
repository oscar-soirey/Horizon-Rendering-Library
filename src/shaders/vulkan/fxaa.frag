#version 460
// FXAA (quality preset close to FXAA 3.11 "12"): edge detection on luma, then
// a search along the edge to find its ends and blend across it.
layout(set = 0, binding = 0) uniform sampler2D uImage; // tone mapped image
layout(push_constant) uniform Push { vec2 rcpFrame; } pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Perceptual luma of a linear colour (the image may be an sRGB format).
float Luma(vec3 c) { return sqrt(dot(c, vec3(0.299, 0.587, 0.114))); }
float LumaAt(vec2 uv) { return Luma(textureLod(uImage, uv, 0.0).rgb); }

const float kEdgeThreshold = 0.125;
const float kEdgeThresholdMin = 0.0312;
const float kSubpixel = 0.75;
const int kSteps = 10;
const float kStepSize[10] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 4.0, 8.0);

void main()
{
	vec2 uv = vUV;
	vec4 center = textureLod(uImage, uv, 0.0);
	float lM = Luma(center.rgb);
	float lN = LumaAt(uv + vec2(0.0, -pc.rcpFrame.y));
	float lS = LumaAt(uv + vec2(0.0, pc.rcpFrame.y));
	float lE = LumaAt(uv + vec2(pc.rcpFrame.x, 0.0));
	float lW = LumaAt(uv + vec2(-pc.rcpFrame.x, 0.0));
	float lMax = max(lM, max(max(lN, lS), max(lE, lW)));
	float lMin = min(lM, min(min(lN, lS), min(lE, lW)));
	float range = lMax - lMin;
	if (range < max(kEdgeThresholdMin, lMax * kEdgeThreshold)) { outColor = center; return; }

	float lNW = LumaAt(uv + vec2(-pc.rcpFrame.x, -pc.rcpFrame.y));
	float lNE = LumaAt(uv + vec2(pc.rcpFrame.x, -pc.rcpFrame.y));
	float lSW = LumaAt(uv + vec2(-pc.rcpFrame.x, pc.rcpFrame.y));
	float lSE = LumaAt(uv + vec2(pc.rcpFrame.x, pc.rcpFrame.y));

	// Sub-pixel aliasing amount.
	float average = (2.0 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE) / 12.0;
	float subpixel = clamp(abs(average - lM) / range, 0.0, 1.0);
	subpixel = smoothstep(0.0, 1.0, subpixel);
	subpixel = subpixel * subpixel * kSubpixel;

	// Edge orientation.
	float horizontal = abs(lN + lS - 2.0 * lM) * 2.0 + abs(lNE + lSE - 2.0 * lE) + abs(lNW + lSW - 2.0 * lW);
	float vertical = abs(lE + lW - 2.0 * lM) * 2.0 + abs(lNE + lNW - 2.0 * lN) + abs(lSE + lSW - 2.0 * lS);
	bool isHorizontal = horizontal >= vertical;

	float l1 = isHorizontal ? lN : lW;
	float l2 = isHorizontal ? lS : lE;
	float gradient1 = abs(l1 - lM), gradient2 = abs(l2 - lM);
	bool steepest1 = gradient1 >= gradient2;
	float gradientScaled = 0.25 * max(gradient1, gradient2);
	float stepLength = isHorizontal ? pc.rcpFrame.y : pc.rcpFrame.x;
	float localAverage;
	if (steepest1) { stepLength = -stepLength; localAverage = 0.5 * (l1 + lM); }
	else localAverage = 0.5 * (l2 + lM);

	// Search the ends of the edge on both sides.
	vec2 edgeUV = uv;
	if (isHorizontal) edgeUV.y += stepLength * 0.5; else edgeUV.x += stepLength * 0.5;
	vec2 offset = isHorizontal ? vec2(pc.rcpFrame.x, 0.0) : vec2(0.0, pc.rcpFrame.y);
	vec2 uv1 = edgeUV - offset, uv2 = edgeUV + offset;
	float end1 = LumaAt(uv1) - localAverage, end2 = LumaAt(uv2) - localAverage;
	bool reached1 = abs(end1) >= gradientScaled, reached2 = abs(end2) >= gradientScaled;
	for (int i = 1; i < kSteps && !(reached1 && reached2); ++i)
	{
		if (!reached1) { uv1 -= offset * kStepSize[i]; end1 = LumaAt(uv1) - localAverage; reached1 = abs(end1) >= gradientScaled; }
		if (!reached2) { uv2 += offset * kStepSize[i]; end2 = LumaAt(uv2) - localAverage; reached2 = abs(end2) >= gradientScaled; }
	}
	float distance1 = isHorizontal ? uv.x - uv1.x : uv.y - uv1.y;
	float distance2 = isHorizontal ? uv2.x - uv.x : uv2.y - uv.y;
	bool nearer1 = distance1 < distance2;
	float distanceFinal = min(distance1, distance2);
	float edgeLength = distance1 + distance2;
	float pixelOffset = -distanceFinal / edgeLength + 0.5;
	// Only blend when the end found is on the expected side of the edge.
	bool centerSmaller = lM < localAverage;
	bool correct = ((nearer1 ? end1 : end2) < 0.0) != centerSmaller;
	float finalOffset = max(correct ? pixelOffset : 0.0, subpixel);

	vec2 finalUV = uv;
	if (isHorizontal) finalUV.y += finalOffset * stepLength; else finalUV.x += finalOffset * stepLength;
	outColor = vec4(textureLod(uImage, finalUV, 0.0).rgb, center.a);
}
