// CPU-side helpers of the 2D voxel world renderer, shared by every backend
// (OpenGL 3.3 and Vulkan). Pure CPU code: no graphics API is used here.
#ifndef HRL_VOXEL_RENDER_H
#define HRL_VOXEL_RENDER_H

#include "../hrl.h"
#include "object_types.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_set>
#include <vector>

// Compact voxel vertex: 2D position (z = 0), RGBA color, texture-array layer
// (-1 = color only). The constant normal (0,0,1) is supplied by the backend.
struct HRL_VoxelMeshVertex
{
	float position[2];
	float color[4];
	float layer;
};

constexpr int VOXEL_CHUNK_STREAM_MARGIN = 1;

float GetVoxelTypeTextureLayer(const HRL_VoxelWorld* world, uint32_t type);
uint64_t MakeVoxelChunkKey(int chunkX, int chunkY);
int VoxelChunkXFromKey(uint64_t key);
int VoxelChunkYFromKey(uint64_t key);
glm::mat4 CalculateVoxelViewportProjection(const HRL_Viewport* viewport);
glm::mat4 CalculateVoxelViewportView(const HRL_Viewport* viewport);
bool ComputeVoxelViewWorldRect(const HRL_VoxelWorld* world, const HRL_Viewport* viewport,
	float& minWorldX, float& minWorldY, float& maxWorldX, float& maxWorldY);
float VoxelLightReachVoxels(const HRL_VoxelWorld* world);
int VoxelStreamMarginChunks(const HRL_VoxelWorld* world);
bool CollectVoxelVisibleChunksForViewport(const HRL_VoxelWorld* world,
	const HRL_Viewport* viewport, std::unordered_set<uint64_t>& desired);
glm::vec4 GetVoxelTypeColor(const HRL_VoxelWorld* world, uint32_t type);
glm::vec3 GetVoxelTypeEmissiveColor(const HRL_VoxelWorld* world, uint32_t type);
bool BuildVoxelChunkGeometry(const HRL_VoxelWorld* world, int chunkX, int chunkY,
	std::vector<HRL_VoxelMeshVertex>& vertices, std::vector<HRL_uint>& indices,
	std::vector<HRL_VoxelMeshVertex>& emissiveVertices, std::vector<HRL_uint>& emissiveIndices);

#endif
