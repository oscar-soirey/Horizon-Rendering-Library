#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// 2D voxel worlds: colour of the type times its texture (tiled in world
// space), lit like the meshes but diffuse only, as on OpenGL. The emission
// pass adds the emissive colour of the emissive voxels.
#include "common.glsl"
#include "brdf.glsl"
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
#include "material_alpha.glsl"
#include "lighting.glsl"
// Per texture layer: x : bindless texture, y : world size of one copy (float bits).
layout(set = 2, binding = 0, std430) readonly buffer VoxelLayers { uvec2 voxelLayers[]; };
layout(push_constant) uniform Push { uint emission; } pc;
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec4 vColor;
layout(location = 2) flat in int vLayer;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outObjectId; // not written: voxels are not pickable

void main()
{
	vec4 texel = vec4(1.0);
	if (vLayer >= 0)
	{
		uvec2 layer = voxelLayers[vLayer];
		float tile = max(uintBitsToFloat(layer.y), 1e-6);
		texel = texture(uTextures[nonuniformEXT(layer.x * 2u + 1u)], vWorldPos.xy / tile);
	}
	float alpha = clamp(vColor.a * texel.a, 0.0, 1.0);
	if (alpha <= 0.001) discard;
	outObjectId = 0u;
	if (pc.emission != 0u)
	{
		outColor = vec4(max(vColor.rgb, vec3(0.0)) * texel.rgb, alpha);
		return;
	}
	ShadedSurface s;
	s.position = vWorldPos;
	s.shadowPosition = vWorldPos;
	s.normal = vec3(0.0, 0.0, gl_FrontFacing ? 1.0 : -1.0);
	s.baseColor = clamp(vColor.rgb * texel.rgb, vec3(0.0), vec3(1.0));
	s.roughness = 1.0;
	s.metallic = 0.0;
	s.specular = 0.0;
	s.ao = 1.0;
	s.emissive = vec3(0.0);
	s.transmission = 0.0;
	outColor = vec4(ShadeSurface(s), alpha);
}
