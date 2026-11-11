#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "overlay.glsl"
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
layout(location = 0) in vec2 vUV;
layout(location = 1) flat in uint vQuad;
layout(location = 0) out vec4 outColor;

vec3 DisplayToLinear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void main()
{
	OverlayQuad q = quads[vQuad];
	// UNORM view: the texels are display values, like the tint.
	vec4 texel = texture(uTextures[nonuniformEXT(q.info.x * 2u)], vUV);
	vec4 color;
	if (q.info.y != 0u)
	{
		// SDF edge at 0.5, smoothed over one pixel whatever the scale.
		float d = texel.r;
		float smoothing = max(fwidth(d), 0.001);
		color = vec4(q.color.rgb, q.color.a * smoothstep(0.5 - smoothing, 0.5 + smoothing, d));
	}
	else color = texel * q.color;
	// An sRGB target encodes what it receives: give it linear values.
	if (pc.encoded == 0u) color.rgb = DisplayToLinear(clamp(color.rgb, 0.0, 1.0));
	outColor = color;
}
