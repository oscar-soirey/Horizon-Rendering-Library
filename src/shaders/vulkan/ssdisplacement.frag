#version 460
#extension GL_EXT_nonuniform_qualifier : require
// Screen space displacement materials (HRL_MATERIAL_TEXTURE_SS_DISPLACEMENT_MAPPING):
// the mesh shows the scene behind it, shifted by its displacement texture
// (RG in -1..1, pixels * strength), as the OpenGL backend.
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
layout(set = 2, binding = 0) uniform sampler2D uSceneCopy;
layout(push_constant) uniform Push
{
	vec2 rcpSize;    // 1 / image size
	float strength;  // pixels
	float scale;     // of the texture coordinates
	float opacity;
	uint texture;    // bindless displacement texture
} pc;
layout(location = 2) in vec2 vUV;
layout(location = 5) flat in uvec2 vIds;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outObjectId;
void main()
{
	vec2 baseUV = gl_FragCoord.xy * pc.rcpSize;
	vec2 displacement = texture(uTextures[nonuniformEXT(pc.texture * 2u)], vUV * max(abs(pc.scale), 0.0001)).rg * 2.0 - 1.0;
	// The texture is authored for OpenGL rows (bottom first): v points up.
	vec2 displacedUV = clamp(baseUV + vec2(displacement.x, -displacement.y) * pc.strength * pc.rcpSize, vec2(0.0), vec2(1.0));
	vec4 base = texture(uSceneCopy, baseUV);
	vec4 displaced = texture(uSceneCopy, displacedUV);
	outColor = vec4(mix(base.rgb, displaced.rgb, clamp(pc.opacity, 0.0, 1.0)), base.a);
	outObjectId = vIds.y;
}
