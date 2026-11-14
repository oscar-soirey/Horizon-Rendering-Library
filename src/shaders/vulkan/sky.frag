#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "common.glsl"
#include "brdf.glsl"
#include "media.glsl"

layout(set = 1, binding = 0) uniform sampler2D uTextures[];
layout(push_constant) uniform Push
{
	mat4 rotation;   // world direction -> sky direction
	vec4 top;
	vec4 horizon;
	vec4 bottom;
	uint texture;    // bindless slot, 0xFFFFFFFF : gradient
	float lod;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outObjectId;

void main()
{
	vec4 far = uView.invViewProj * vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
	vec4 near = uView.invViewProj * vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
	vec3 dir = mat3(pc.rotation) * normalize(far.xyz / far.w - near.xyz / near.w);
	vec3 color;
	if (pc.texture != 0xFFFFFFFFu)
		color = textureLod(uTextures[nonuniformEXT(pc.texture * 2u + 1u)], EquirectUV(dir), 0.0).rgb;
	else
		color = dir.y >= 0.0 ? mix(pc.horizon.rgb, pc.top.rgb, clamp(dir.y, 0.0, 1.0))
		                     : mix(pc.horizon.rgb, pc.bottom.rgb, clamp(-dir.y, 0.0, 1.0));
	// Participating media: everything between the camera and the far end of the froxels.
	if (HRL_MediaActive())
	{
		vec4 m = textureLod(uMediaVolume, vec3(vUV.x, 1.0 - vUV.y, 1.0), 0.0);
		color = color * m.a + m.rgb;
	}
	outColor = vec4(color, 1.0);
	outObjectId = 0xFFFFFFFFu;
}
