#ifndef HRL_GI_SCENE_H
#define HRL_GI_SCENE_H

#include "../hrl.h"
#include "object_types.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

/**
 * CPU side of the global illumination (shared by every backend).
 *
 * Every GI method of HRL works from the same representation of the scene,
 * rebuilt only when the scene geometry changes (gi_geometry_revision):
 *
 *  - a bounding volume hierarchy of the static triangles (3D meshes LOD 0 and
 *    landscapes), packed as float4 arrays the shaders traverse on the GPU
 *    (ray traced GI, path tracing, DDGI probe rays, voxel lighting);
 *  - a voxelization of the same triangles (albedo + opacity, normal) used by
 *    voxel cone tracing and light propagation volumes;
 *  - the layout of the DDGI probe grid.
 *
 * Surfaces are diffuse in the GI : each triangle stores its average diffuse
 * albedo (material texture, TintColor and metallic).
 */
struct HRL_GISceneData
{
	uint64_t geometryRevision = 0;
	bool valid = false;

	glm::vec3 boundsMin{0.f};
	glm::vec3 boundsMax{0.f};

	// BVH : node i = texels 2i (bmin, a) and 2i+1 (bmax, b).
	//   inner node : a = left child, b = right child (>= 0)
	//   leaf       : a = first triangle, b = -triangle count
	std::vector<glm::vec4> nodes;
	// Triangle i = texels 3i, 3i+1, 3i+2 : (p0, albedo.r) (p1, albedo.g) (p2, albedo.b)
	std::vector<glm::vec4> triangles;
	uint32_t triangleCount = 0;
	uint32_t nodeCount = 0;

	// Voxel volume (cubic voxels), stored as a 2D atlas of Z slices
	// (see HRL_GIAtlasLayout). RGBA8 each.
	int voxelResolution = 0;
	glm::vec3 voxelOrigin{0.f};
	float voxelSize = 1.f;
	std::vector<unsigned char> voxelAlbedo; // rgb = diffuse albedo, a = opacity
	std::vector<unsigned char> voxelNormal; // rgb = normal * 0.5 + 0.5, a = 255 when set

	// DDGI probe grid.
	glm::ivec3 probeCounts{0};
	glm::vec3 probeOrigin{0.f};
	glm::vec3 probeSpacing{1.f};
	int ProbeCount() const { return probeCounts.x * probeCounts.y * probeCounts.z; }
};

// Layout of a cubic volume (resolution^3) stored as a 2D texture of slices.
struct HRL_GIAtlasLayout
{
	int resolution = 1;
	int tilesPerRow = 1;
	int width = 1;
	int height = 1;
	static HRL_GIAtlasLayout For(int resolution);
};

constexpr int kHRLGIVoxelResolution = 64;
constexpr int kHRLGIMaxProbeCount = 1024;
constexpr int kHRLGIMaxProbesPerAxis = 16;

// Rebuilds the data if the geometry revision of the scene changed. Returns
// false when the scene has no geometry usable by the GI.
bool HRL_UpdateGISceneData(HRL_id sceneId, HRL_GISceneData& data);

#endif
