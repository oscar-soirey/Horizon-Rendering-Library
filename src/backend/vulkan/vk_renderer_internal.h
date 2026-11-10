// HRL native Vulkan backend - types and functions shared by the renderer files.
// Not included outside src/backend/vulkan.
#ifndef HRL_VK_RENDERER_INTERNAL_H
#define HRL_VK_RENDERER_INTERNAL_H

#include "vk_device.h"
#include "vk_frame_graph.h"
#include "hrl_vk_shaders.h"
#include "../../core/backend_api.h"
#include "../../core/render_common.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace hrlvk
{
constexpr uint32_t kCascades = 4;
constexpr uint32_t kMaxSpotShadows = 4;
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kIdFormat = VK_FORMAT_R32_UINT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t kEnvSize = 256;
constexpr uint32_t kPrefilterSize = 128;
constexpr uint32_t kPrefilterMips = 6;
constexpr uint32_t kIrradianceSize = 32;
constexpr uint32_t kBrdfLutSize = 256;
constexpr uint32_t kNoObject = 0xFFFFFFFFu;

// ---- GPU structures (see src/shaders/vulkan/common.glsl) -------------------
struct ViewData
{
	glm::mat4 viewProj, view, invViewProj;
	glm::vec4 camPos, viewportSize;
	glm::mat4 cascadeViewProj[kCascades];
	glm::vec4 cascadeSplits;
	glm::mat4 spotViewProj[kMaxSpotShadows];
	glm::vec4 shadowInfo, fogColor, fogParams, envParams;
	glm::uvec4 counts;
};
struct InstanceData { glm::mat4 model, normalMatrix; glm::uvec4 ids; };
struct MaterialData { glm::vec4 baseColor, emissive, params; glm::uvec4 tex0, tex1, flags; };
struct LightData { glm::vec4 position, direction, color, spot; };
struct SkyPush { glm::mat4 rotation; glm::vec4 top, horizon, bottom; uint32_t texture; float lod; };
struct FilterPush { float roughness; float envResolution; uint32_t mode; uint32_t sampleCount; };

enum : uint32_t { ViewFlagIBL = 1, ViewFlagRTShadows = 2, ViewFlagFog = 4, ViewFlagFogExp = 8 };
enum : uint32_t { MatRoughnessValue = 1, MatMetallicValue = 2, MatSpecularValue = 4, MatOpacityValue = 8,
	MatRoughnessInvert = 16, MatAlphaInvert = 32 };

// Draw buckets: one pipeline each.
enum Bucket : uint32_t { BucketOpaque, BucketOpaqueTwoSided, BucketMasked, BucketMaskedTwoSided, BucketCount };

// ---- Resources -------------------------------------------------------------
struct Texture
{
	Image image;
	VkImageView srgbView = VK_NULL_HANDLE;
	uint32_t slot = 0;
	HRL_EFilterType minFilter = HRL_FILTER_ANISOTROPIC;
	HRL_EFilterType magFilter = HRL_FILTER_LINEAR;
};

struct Blas
{
	VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
	Buffer storage;
	VkDeviceAddress address = 0;
};

struct MeshLOD
{
	VmaVirtualAllocation vertexAlloc = VK_NULL_HANDLE;
	VmaVirtualAllocation indexAlloc = VK_NULL_HANDLE;
	uint32_t vertexOffset = 0, vertexCount = 0;
	uint32_t firstIndex = 0, indexCount = 0;
	bool valid = false;
};

struct MeshGPU
{
	std::vector<MeshLOD> lods;
	Blas blas; // LOD 0, only when ray tracing is available
};

struct Tlas
{
	VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
	Buffer storage, instances, scratch;
	uint32_t capacity = 0;
	uint32_t count = 0; // instances of the pending build
};

struct Environment
{
	Image cube, prefiltered, irradiance;
	VkImageView cubeStorage = VK_NULL_HANDLE;
	std::vector<VkImageView> prefilteredMips;
	VkImageView irradianceStorage = VK_NULL_HANDLE;
	bool valid = false;
	// Signature of the inputs the environment was generated from.
	HRL_id texture = HRL_INVALID_ID;
	glm::vec3 top{0}, horizon{0}, bottom{0}, rotation{0};
};


struct DrawItem
{
	const HRL_Mesh* mesh = nullptr;
	const MeshGPU* gpu = nullptr;
	glm::mat4 model{1.f};
	uint32_t instance = 0;   // index in SceneData::instances
	uint32_t material = 0;   // index in SceneData::materials
	float distance2 = 0.f;   // per viewport
	uint32_t lod = 0;        // per viewport
};

// CPU copy of what the shaders read for one scene: instances, materials and
// the draw list. It is kept between frames and only the entries whose source
// changed are recomputed (see vk_scene_data.cpp).
struct SceneData
{
	struct MeshKey
	{
		HRL_id id = HRL_INVALID_ID;
		const HRL_Mesh* mesh = nullptr;
		HRL_id material = HRL_INVALID_ID;
		float radius = 0.f;
		int32_t item = -1; // index in items, -1 when the mesh cannot be drawn
		glm::vec3 position{0.f}, rotation{0.f}, scale{1.f}, pivot{0.f};
	};
	struct MaterialKey
	{
		const HRL_Material* material = nullptr;
		uint64_t revision = 0;
	};
	std::vector<MeshKey> keys;               // every 3D mesh of the scene, in scene order
	std::vector<DrawItem> items;
	std::vector<InstanceData> instances;     // parallel to items
	std::vector<MaterialData> materials;
	std::vector<Bucket> materialBuckets;     // parallel to materials
	std::vector<MaterialKey> materialKeys;   // parallel to materials

	bool built = false;
	uint64_t meshTableRevision = 0;
	uint64_t textureRevision = 0;
	size_t materialCount = 0;
	HRL_id lastMaterialId = HRL_INVALID_ID;

	// Work done by the last update (diagnostics, tests).
	uint32_t rebuilds = 0, transformUpdates = 0, materialUpdates = 0;
};

struct SceneGPU
{
	bool onScreen = false;
	bool explicitSize = false;
	uint32_t width = 0, height = 0;
	// Persistent targets: object ids (picking), depth, final LDR image. The HDR
	// colour target is a frame graph transient.
	Image ids, depth, ldr;
	Environment env;
	Tlas tlas[kFramesInFlight];
	SceneData data;
	uint64_t renderedFrame = ~0ull;
	// Last rendered frame (diagnostics).
	struct Stats { uint32_t drawn = 0, passes = 0; } stats;
};

struct PendingScreenshot
{
	Buffer buffer;
	std::string path;
	uint32_t width = 0, height = 0;
	uint64_t frame = 0;
};

struct State
{
	bool initialized = false;
	uint32_t width = 0, height = 0;
	bool rtShadows = false;
	float shadowDistance = 150.f;

	// Layouts / pipelines
	VkDescriptorSetLayout viewLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout bindlessLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout imageLayout = VK_NULL_HANDLE;   // sampler + storage image (compute)
	VkDescriptorSetLayout samplerLayout = VK_NULL_HANDLE; // one sampler (tone mapping)
	VkPipelineLayout sceneLayout = VK_NULL_HANDLE;
	VkPipelineLayout computeLayout = VK_NULL_HANDLE;
	VkPipelineLayout tonemapLayout = VK_NULL_HANDLE;
	VkPipelineLayout debugLayout = VK_NULL_HANDLE;
	VkPipeline meshPipelines[BucketCount]{};
	VkPipeline shadowPipeline = VK_NULL_HANDLE;
	VkPipeline skyPipeline = VK_NULL_HANDLE;
	VkPipeline tonemapPipeline = VK_NULL_HANDLE;
	VkPipeline debugLinePipeline = VK_NULL_HANDLE;
	VkPipeline debugTrianglePipeline = VK_NULL_HANDLE;
	VkPipeline envCubePipeline = VK_NULL_HANDLE;
	VkPipeline envFilterPipeline = VK_NULL_HANDLE;
	VkPipeline brdfLutPipeline = VK_NULL_HANDLE;
	VkFormat ldrFormat = VK_FORMAT_R8G8B8A8_SRGB;

	// Bindless textures
	VkDescriptorPool bindlessPool = VK_NULL_HANDLE;
	VkDescriptorSet bindlessSet = VK_NULL_HANDLE;
	std::unordered_map<HRL_id, Texture> textures;
	std::vector<uint32_t> freeSlots;
	uint32_t nextSlot = 0;
	Texture white, flatNormal, black;
	std::unordered_map<uint64_t, VkSampler> samplers;
	VkSampler linearClamp = VK_NULL_HANDLE;
	VkSampler shadowSampler = VK_NULL_HANDLE;

	// Geometry pool: every mesh lives in one vertex and one index buffer so a
	// whole pass is a single multi-draw-indirect call.
	Buffer vertexBuffer, indexBuffer;
	VmaVirtualBlock vertexBlock = VK_NULL_HANDLE, indexBlock = VK_NULL_HANDLE;
	std::unordered_map<HRL_id, MeshGPU> meshes;

	// Shared images
	Image brdfLut;
	bool brdfLutReady = false;
	Image dummyCube;
	Image shadowMaps;
	std::vector<VkImageView> shadowLayers;
	uint32_t shadowResolution = 0;

	std::unordered_map<HRL_id, SceneGPU> scenes;
	std::vector<PendingScreenshot> screenshots;
	HRL_id lastScene = HRL_INVALID_ID;
	glm::mat4 lastView{1.f}, lastProjGL{1.f};

	// Ray tracing entry points (VK_KHR_acceleration_structure)
	PFN_vkCreateAccelerationStructureKHR createAS = nullptr;
	PFN_vkDestroyAccelerationStructureKHR destroyAS = nullptr;
	PFN_vkGetAccelerationStructureBuildSizesKHR buildSizesAS = nullptr;
	PFN_vkCmdBuildAccelerationStructuresKHR buildAS = nullptr;
	PFN_vkGetAccelerationStructureDeviceAddressKHR addressAS = nullptr;
	VkDeviceSize scratchAlignment = 1;

	// Frame graph transient images, shared by every scene.
	TransientPool transients;
	// Incremented when an entry of `meshes` is created or destroyed, and when
	// the bindless slot of a texture may have changed: the scene data caches
	// compare them (see vk_scene_data.cpp).
	uint64_t meshTableRevision = 1;
	uint64_t textureRevision = 1;
};
extern State R;
inline Device& Dev() { return GetDevice(); }
inline VkDeviceAddress AlignUp(VkDeviceAddress v, VkDeviceSize a) { return (v + a - 1) / a * a; }

// ---- vk_pipelines.cpp -------------------------------------------------------
bool CreatePipelines();
// Descriptor set valid for the current frame only.
VkDescriptorSet AllocFrameSet(VkDescriptorSetLayout layout);

// ---- vk_resources.cpp -------------------------------------------------------
VkSampler GetTextureSampler(HRL_EFilterType minFilter, HRL_EFilterType magFilter);
bool CreateTextureInternal(Texture& t, const unsigned char* rgba, int width, int height);
void DestroyTextureInternal(Texture& t, bool releaseSlot);
// Bindless slot of a material texture, `fallback` when it is not set.
uint32_t TextureSlot(const HRL_Material* m, const char* key, uint32_t fallback);
bool EnsurePoolCapacity(Buffer& buffer, VkDeviceSize needed, bool index);
void DestroyBlas(Blas& blas);

// ---- vk_environment.cpp -----------------------------------------------------
glm::mat4 SkyRotation(const glm::vec3& degrees);
void DestroyEnvironment(Environment& env);
// Returns true when the scene is lit by an environment. `dirty` is set when
// RecordEnvironment must run this frame (sky inputs changed).
bool PrepareEnvironment(SceneGPU& gpu, const hrl_scene_t* scene, bool& dirty);
void RecordEnvironment(VkCommandBuffer cmd, SceneGPU& gpu, const hrl_scene_t* scene);
// One-off: BRDF lookup table and the placeholder cube map.
void RecordBrdfLut(VkCommandBuffer cmd);

// ---- vk_scene_data.cpp ------------------------------------------------------
float MaterialFloat(const HRL_Material* m, const char* name, float fallback);
// Brings the cached scene data in line with the frontend scene.
void UpdateSceneData(SceneData& data, const HRL_SceneFrame& frame);

struct LightSetup
{
	std::vector<LightData> lights;
	const HRL_Light* cascadeLight = nullptr;            // directional light using the cascades
	std::vector<const HRL_Light*> spotShadowLights;     // spot lights with a shadow map
	uint32_t shadowResolution = 0;                      // 0 : no shadow map this frame
};
LightSetup PackLights(const hrl_scene_t* scene, bool rayTracedShadows);

struct Camera
{
	glm::mat4 view{1.f};
	glm::mat4 proj{1.f};    // Vulkan clip space, reversed-Z
	glm::mat4 projGL{1.f};  // OpenGL clip space (culling helpers, public API)
	glm::vec3 position{0.f};
	float nearPlane = 0.1f, farPlane = 1000.f;
	bool perspective = true;
	float fovY = 1.f, aspect = 1.f, orthoHalfHeight = 1.f;
};

Camera MakeCamera(const HRL_Camera* c, float width, float height);

struct ShadowView
{
	glm::mat4 viewProj{1.f};   // Vulkan clip space, depth 0..1
	glm::mat4 cullClip{1.f};   // OpenGL clip space, for the CPU frustum test
	uint32_t layer = 0;
};
// Fills the cascade matrices and splits of `view` and appends one shadow view per cascade.
void ComputeCascades(const Camera& cam, const glm::vec3& lightDir, float resolution,
	ViewData& view, std::vector<ShadowView>& out);
} // namespace hrlvk

#endif
