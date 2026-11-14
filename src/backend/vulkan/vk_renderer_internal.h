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
constexpr uint32_t kMaxPointShadows = 4;        // point lights with shadow maps (6 faces each)
constexpr uint32_t kMaxPointShadowResolution = 1024;
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kIdFormat = VK_FORMAT_R32_UINT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t kEnvSize = 256;
constexpr uint32_t kPrefilterSize = 128;
constexpr uint32_t kPrefilterMips = 6;
constexpr uint32_t kIrradianceSize = 32;
constexpr uint32_t kBrdfLutSize = 256;
constexpr uint32_t kNoObject = 0xFFFFFFFFu;
constexpr VkFormat kBloomFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
constexpr uint32_t kMaxBloomLevels = 6;

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
	glm::vec4 wind;
	glm::vec4 interactors[16];
	glm::mat4 pointViewProj[kMaxPointShadows * 6];
	glm::vec4 giOrigin, giSpacing, giResolution; // probe volume (vk_gi.cpp), giResolution.w : 1 when active
	glm::vec4 voxelLightRegion, voxelLight;       // light field of the emissive voxels (vk_voxel.cpp)
	glm::vec4 mediaParams;                       // participating media (media.glsl, vk_render_pipeline.cpp)
};
struct InstanceData { glm::mat4 model, normalMatrix; glm::uvec4 ids; };
struct MaterialData { glm::vec4 baseColor, emissive, params; glm::uvec4 tex0, tex1, flags; };
struct LightData { glm::vec4 position, direction, color, spot; };
struct SkyPush { glm::mat4 rotation; glm::vec4 top, horizon, bottom; uint32_t texture; float lod; };

// GPU culling (see cull.comp / cull_common.glsl).
constexpr uint32_t kCulledBuckets = 4; // opaque and masked buckets; translucent ones are sorted on the CPU
enum : uint32_t { CullFlagOcclusion = 1, CullFlagPerspective = 2, CullFlagShadow = 4 };
enum : uint32_t { RecordAutoLod = 1, RecordDistanceLod = 2 };
struct DrawRecord
{
	glm::vec4 sphere;   // local bounds centre, radius (0 : never culled)
	glm::uvec4 info;    // instance, bucket, first LOD range, LOD count
	glm::vec4 lod;      // base distance or screen threshold, scale, min distance, max distance
	glm::uvec4 lodMode; // flags, forced LOD (~0u : none), hysteresis (float bits)
};
struct LodRange { uint32_t indexCount, firstIndex; int32_t vertexOffset; uint32_t pad; };
struct CullView
{
	glm::mat4 viewProj;
	glm::vec4 planes[6];
	glm::vec4 camPos;     // w : proj[1][1]
	glm::vec4 hzb;        // xy : mip 0 size, z : mip count
	glm::uvec4 params;    // item count, phase, viewport bit, flags
	glm::uvec4 bucketBase;
	glm::vec4 lodInfo;    // xyz : camera forward
};
constexpr uint32_t kStateVisibleShift = 8; // State bits 8.. : visible last frame, one per viewport
struct FilterPush { float roughness; float envResolution; uint32_t mode; uint32_t sampleCount; };

enum : uint32_t { ViewFlagIBL = 1, ViewFlagRTShadows = 2, ViewFlagFog = 4, ViewFlagFogExp = 8 };
enum : uint32_t { MatRoughnessValue = 1, MatMetallicValue = 2, MatSpecularValue = 4, MatOpacityValue = 8,
	MatRoughnessInvert = 16, MatAlphaInvert = 32 };

// Draw buckets: one pipeline each.
enum Bucket : uint32_t
{
	BucketOpaque, BucketOpaqueTwoSided,
	BucketMasked, BucketMaskedTwoSided,           // alpha test
	BucketTranslucent, BucketTranslucentTwoSided, // alpha blended, drawn back to front after the sky
	BucketSprite,                                 // sprites: translucent, two sided, nearest texels
	BucketCount
};
inline bool IsMasked(uint32_t bucket) { return bucket == BucketMasked || bucket == BucketMaskedTwoSided; }
inline bool IsTranslucent(uint32_t bucket) { return bucket >= BucketTranslucent; }
inline bool IsTwoSided(uint32_t bucket) { return bucket == BucketSprite || (bucket & 1u) != 0; }

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
	Blas blas; // LOD 0, only when ray tracing is available (static meshes)

	// Skeletal meshes: source vertices with bone weights. The skinned result is
	// written by a compute pass into the LOD 0 range of the vertex pool.
	Buffer skinSource;
	uint32_t skinVertexCount = 0;
	mutable uint64_t poseSerial = 0; // HRL_SkeletalMesh::pose_serial_ last skinned
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
	bool noCull = false;     // skinned meshes: their bounds follow the animation
	bool sprite = false;     // HRL_SPRITE: drawn in BucketSprite whatever its material
	HRL_id custom = HRL_INVALID_ID; // surface shader of the material (drawn from the CPU, not GPU culled)
	bool displacement = false; // screen space displacement material: drawn after the scene, over a copy of it
	float distance2 = 0.f;   // per viewport
	uint32_t lod = 0;        // per viewport
};

// CPU copy of what the shaders read for one scene: instances, materials and
// the draw list. It is kept between frames and only the entries whose source
// changed are recomputed (see vk_scene_data.cpp).
struct SceneData
{
	struct MaterialKey
	{
		const HRL_Material* material = nullptr;
		uint64_t revision = 0;
	};
	std::unordered_map<HRL_id, uint32_t> itemOfMesh; // mesh id -> item
	std::vector<uint32_t> skinnedItems;
	std::vector<uint32_t> spriteItems;
	std::vector<uint32_t> customItems;
	std::vector<uint32_t> displacementItems;
	std::vector<DrawItem> items;
	std::vector<InstanceData> instances;     // parallel to items
	std::vector<MaterialData> materials;
	std::vector<Bucket> materialBuckets;     // parallel to materials
	std::vector<MaterialKey> materialKeys;   // parallel to materials (material null: default material)
	std::unordered_map<HRL_id, uint32_t> materialIndexOf;

	bool built = false;
	uint64_t meshTableRevision = 0;
	uint64_t sceneMeshRevision = 0;          // hrl_scene_t::mesh_revision
	uint64_t textureRevision = 0;
	size_t materialCount = 0;
	HRL_id lastMaterialId = HRL_INVALID_ID;

	// GPU culling inputs (parallel to items), rebuilt with them.
	std::vector<DrawRecord> records;
	std::vector<LodRange> lodRanges;
	std::vector<uint32_t> translucentItems;  // sorted on the CPU every frame
	std::vector<uint32_t> autoLodItems;      // items whose LOD is reported back to the mesh
	uint32_t bucketBase[kCulledBuckets]{};   // first command of each culled bucket in a view
	uint32_t culledItems = 0;                // items of the culled buckets
	uint64_t serial = 0;                     // incremented by every rebuild
	// Instances changed since the last upload (all of them after a rebuild).
	std::vector<uint32_t> dirtyInstances;
	bool uploadAll = true;

	// Work done by the last update (diagnostics, tests).
	uint32_t rebuilds = 0, transformUpdates = 0, materialUpdates = 0;
};

// Draw bucket of an item: its material's, except for sprites.
inline uint32_t ItemBucket(const SceneData& data, const DrawItem& item)
{
	return item.sprite ? (uint32_t)BucketSprite : (uint32_t)data.materialBuckets[item.material];
}

// Landscapes (see vk_landscape.cpp).
constexpr uint32_t kLandscapeMaxLods = 16;
struct LandscapeUniform // landscape.glsl
{
	glm::mat4 model, normalMatrix;
	glm::vec4 samples, spacing, uvScale, morphCamera;
	glm::uvec4 ids;
	glm::vec4 morph[kLandscapeMaxLods];
};
struct LandscapeGPU
{
	Image heights;                       // R32F, sampled through the bindless array
	uint32_t slot = 0xFFFFFFFFu;
	uint32_t width = 0, height = 0;
	uint64_t revision = 0;               // HRL_Landscape::heights_revision_ uploaded
	uint32_t levels = 0;                 // quadtree levels, 0 : leaves
	std::vector<glm::uvec2> dims;        // nodes per level
	std::vector<std::vector<glm::vec2>> minMax; // height range of each node
	Blas blas;                           // ray tracing mesh
	Buffer blasVertices, blasIndices;
	bool blasDirty = true;
	uint64_t lastFrame = 0;
};
struct LandscapeDraw
{
	HRL_id id = HRL_INVALID_ID;
	const HRL_Landscape* landscape = nullptr;
	VkDescriptorSet set = VK_NULL_HANDLE;
	glm::mat4 model{1.f};
	float ranges[kLandscapeMaxLods]{};
};
struct LandscapeNodes
{
	FrameSlice slice;
	uint32_t count = 0;
};

// Foliage (see vk_foliage.cpp / vk_impostor.cpp).
constexpr uint32_t kImpostorFrames = 8;     // views per side of the hemi-octahedral atlas
constexpr uint32_t kImpostorFrameSize = 128; // pixels per view
struct FoliageUniform // FOLIAGE_UNIFORM_BLOCK of foliage.glsl
{
	glm::vec4 bounds, heights, lodEnd, draw;
	glm::uvec4 ids, materials;
	glm::uvec4 lods[12];
	glm::vec4 impostor;
};
struct FoliageCellData { glm::vec4 lo, hi; glm::uvec4 range; };
struct FoliageCellGPU
{
	VmaVirtualAllocation alloc = VK_NULL_HANDLE;
	uint32_t offset = 0, capacity = 0, count = 0; // in instances
	uint64_t revision = 0;
};
struct FoliageGPU
{
	Buffer instances;                     // every instance, sub-allocated per cell
	VmaVirtualBlock block = VK_NULL_HANDLE;
	uint32_t poolCapacity = 0;
	std::vector<FoliageCellGPU> cells;    // parallel to HRL_Foliage::cells
	uint64_t cellsRevision = 0;
	uint64_t instancesRevision = 0;       // HRL_Foliage::instances_revision uploaded
	Buffer cellTable;                     // non-empty cells (FoliageCellData), device local
	uint32_t cellCount = 0;
	Buffer visible;                       // 4 regions (LOD 0, 1, 2, impostor) of regionCapacity entries
	uint32_t regionCapacity = 0;
	Buffer draw;                          // counts and indirect commands
	// Impostor atlas (albedo + alpha, normal + roughness).
	Image impostorAlbedo, impostorNormal;
	VkImageView impostorAlbedoMip0 = VK_NULL_HANDLE, impostorNormalMip0 = VK_NULL_HANDLE;
	uint32_t albedoSlot = 0xFFFFFFFFu, normalSlot = 0xFFFFFFFFu;
	glm::vec3 impostorCenter{0.f};
	float impostorHalfSize = 1.f;
	uint64_t impostorRevision = 0;        // HRL_Foliage::geometry_revision baked
	bool impostorReady = false;
	uint64_t lastFrame = 0;
	// Ray tracing structures of each part and LOD: a shadow ray starting on a
	// plant hits the same triangles as were rasterized (no self shadowing).
	Blas lodBlas[4][3];
	uint64_t blasRevision = 0;
	// Counts of the first camera view, read back when its frame slot comes back.
	Buffer readback[kFramesInFlight];
	bool readbackPending[kFramesInFlight]{};
	uint32_t stats[4]{};
};
struct FoliageDraw
{
	HRL_id id = HRL_INVALID_ID;
	const HRL_Foliage* foliage = nullptr;
	FrameSlice uniform;
	VkBuffer cells = VK_NULL_HANDLE;
	uint32_t cellCount = 0, partCount = 0;
	VkDescriptorSet set = VK_NULL_HANDLE;   // set 2 of the foliage pipelines
	bool masked[4]{}, twoSided[4]{};
	bool impostors = false;
};

// Ray traced shadows: instances of the acceleration structure and the
// geometry of its alpha tested entries (RtGeometry in lighting.glsl).
struct RtGeometry { uint32_t firstIndex; int32_t vertexOffset; uint32_t material, pad; };
struct RayScene
{
	std::vector<VkAccelerationStructureInstanceKHR> instances;
	std::vector<RtGeometry> table;
};
constexpr float kRayFoliageDistance = 100.f;  // foliage casts ray traced shadows up to this distance
constexpr uint32_t kRayFoliageBudget = 65536;  // instances per frame at most

// Grass (see vk_grass.cpp).
struct GrassUniform // GRASS_UNIFORM_BLOCK of grass.glsl
{
	glm::mat4 model, invModel;
	glm::vec4 samples, spacing, blade, baseColor, tipColor, grid;
	glm::uvec4 ids;
};
struct GrassGPU
{
	Buffer tiles, draw;          // draw : count, then one command per tile
	uint32_t maxTiles = 0;
	uint64_t lastFrame = 0;
};
struct GrassDraw
{
	HRL_id id = HRL_INVALID_ID;
	const HRL_Grass* grass = nullptr;
	FrameSlice uniform;
	VkDescriptorSet set = VK_NULL_HANDLE; // set 2 of the grass pipelines
	uint32_t tiles = 0;
};

// Custom shaders (vk_custom.cpp).
struct CustomParam
{
	enum Kind { Float, Int, Uint, Bool, Sampler, Mat3, Mat4 };
	std::string name;
	Kind kind = Float;
	uint32_t components = 1;
	uint32_t arraySize = 0;  // 0 : not an array
	uint32_t first = 0;      // first vec4 of the parameter in the table
	uint32_t slots = 1;      // vec4s used (matrices: 3 or 4 per element)
	glm::vec4 value{0.f};    // initializer of the declaration
};
// Values of the parameters of an OpenGL effect shader, by uniform name
// (floats, column-major for matrices, element after element for arrays).
using EffectParams = std::unordered_map<std::string, std::vector<float>>;
struct CustomShader
{
	bool surface = false;   // surface function (meshes); otherwise post-process
	bool hasVertex = false; // vertex function
	std::vector<CustomParam> params;
	uint32_t paramSlots = 0;              // vec4s of the parameter table
	std::string fragmentUser, vertexUser; // generated hrl_user.glsl of each stage
	std::vector<uint32_t> fragment, vertex; // SPIR-V (forward opaque / post-process, vertex function)
	VkPipeline forward[BucketCount]{};
	bool forwardTried[BucketCount]{};
	VkPipeline shadow = VK_NULL_HANDLE, shadowMasked = VK_NULL_HANDLE;
	bool shadowTried = false, shadowMaskedTried = false;
	VkPipeline post = VK_NULL_HANDLE;
};

struct HzbImage
{
	Image image;
	std::vector<VkImageView> mips; // one storage view per mip
};

// Temporal anti-aliasing: jitter of the projection (Halton 2, 3) and history.
constexpr uint32_t kTaaSamples = 8;
enum class AntiAliasing : uint32_t { None = 0, Fxaa = 1, Taa = 2 };

struct SceneGPU
{
	bool onScreen = false;
	bool explicitSize = false;
	uint32_t width = 0, height = 0;
	// Persistent targets: object ids (picking), depth, final LDR image. The HDR
	// colour target is a frame graph transient.
	Image ids, depth, ldr;
	// Temporal anti-aliasing: resolved image of the last frame and of this one,
	// and the (unjittered) camera of each viewport in the last frame.
	Image history[2];
	uint32_t historyIndex = 0;
	bool historyValid = false;
	std::unordered_map<HRL_id, glm::mat4> previousViewProj;
	Environment env;
	Tlas tlas[kFramesInFlight];
	SceneData data;
	uint64_t renderedFrame = ~0ull;

	// Hierarchical depth of each viewport (in viewport order).
	std::vector<HzbImage> hzb;
	// GPU culling buffers (device local, persistent).
	Buffer instances, records, lodRanges, state, commands, counts;
	uint32_t commandViews = 0;    // views the command buffer has room for
	uint64_t uploadedSerial = 0;  // SceneData::serial of the uploaded records
	// Read back once the frame that wrote them is complete: draw counts
	// (statistics) and the LOD state (HRL_GetMeshLODLevel).
	struct Readback
	{
		Buffer counts, state;
		uint64_t serial = 0;       // SceneData::serial when written, 0 : nothing
		uint32_t mainViews = 0;    // count slots of camera views (phase 1 and 2)
		uint32_t translucent = 0;  // translucent draws (CPU side)
	} readback[kFramesInFlight];
	// Last rendered frame (diagnostics).
	struct Stats { uint32_t drawn = 0, passes = 0, skinned = 0; } stats;
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
	bool occlusionCulling = true;
	float shadowDistance = 150.f;
	AntiAliasing antiAliasing = AntiAliasing::Fxaa;

	// Layouts / pipelines
	VkDescriptorSetLayout viewLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout bindlessLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout imageLayout = VK_NULL_HANDLE;   // sampler + storage image (compute)
	VkDescriptorSetLayout samplerLayout = VK_NULL_HANDLE; // one sampler (bloom)
	VkDescriptorSetLayout tonemapSetLayout = VK_NULL_HANDLE; // scene + bloom
	VkPipelineLayout sceneLayout = VK_NULL_HANDLE;
	VkPipelineLayout computeLayout = VK_NULL_HANDLE;
	VkPipelineLayout tonemapLayout = VK_NULL_HANDLE;
	VkPipelineLayout bloomLayout = VK_NULL_HANDLE;
	VkPipelineLayout debugLayout = VK_NULL_HANDLE;
	VkPipeline meshPipelines[BucketCount]{};
	VkPipeline shadowPipeline = VK_NULL_HANDLE;
	VkPipeline shadowMaskedPipeline = VK_NULL_HANDLE; // alpha tested casters
	VkPipeline skyPipeline = VK_NULL_HANDLE;
	VkPipeline tonemapPipeline = VK_NULL_HANDLE;
	VkPipeline skinPipeline = VK_NULL_HANDLE;
	VkDescriptorSetLayout cullSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout cullLayout = VK_NULL_HANDLE;
	VkPipeline cullPipeline = VK_NULL_HANDLE;
	VkPipeline hzbPipeline = VK_NULL_HANDLE;
	VkDescriptorSetLayout foliageSetLayout = VK_NULL_HANDLE;     // set 2 of the foliage pipelines
	VkDescriptorSetLayout foliageCullSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout foliageLayout = VK_NULL_HANDLE;
	VkPipelineLayout foliageCullLayout = VK_NULL_HANDLE;
	VkPipeline foliagePipelines[4]{};      // (masked ? 2 : 0) + (two sided ? 1 : 0)
	VkPipeline foliageShadowPipeline = VK_NULL_HANDLE;
	VkPipeline foliageShadowMaskedPipeline = VK_NULL_HANDLE;
	VkPipeline foliageCullPipeline = VK_NULL_HANDLE;
	VkPipeline foliageCommandsPipeline = VK_NULL_HANDLE;
	VkPipeline impostorPipeline = VK_NULL_HANDLE;
	VkPipeline impostorShadowPipeline = VK_NULL_HANDLE;
	VkPipeline impostorBakePipelines[2]{};  // opaque, alpha tested
	VkDescriptorSetLayout grassSetLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout grassCullSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout grassLayout = VK_NULL_HANDLE;
	VkPipelineLayout grassCullLayout = VK_NULL_HANDLE;
	VkPipeline grassPipeline = VK_NULL_HANDLE;
	VkPipeline grassShadowPipeline = VK_NULL_HANDLE;
	VkPipeline grassCullPipeline = VK_NULL_HANDLE;
	VkDescriptorSetLayout landscapeSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout landscapeLayout = VK_NULL_HANDLE;
	VkPipeline landscapePipeline = VK_NULL_HANDLE;
	VkPipeline landscapeShadowPipeline = VK_NULL_HANDLE;

	// Foliage and landscapes
	std::unordered_map<HRL_id, FoliageGPU> foliage;
	std::unordered_map<HRL_id, GrassGPU> grass;
	std::unordered_map<HRL_id, LandscapeGPU> landscapes;
	Buffer landscapePatchVertices, landscapePatchIndices;
	uint32_t landscapePatchIndexCount = 0;
	VkDescriptorSetLayout skinLayout = VK_NULL_HANDLE; // source, bones, vertex pool
	VkPipelineLayout skinPipelineLayout = VK_NULL_HANDLE;
	VkPipeline bloomDownPipeline = VK_NULL_HANDLE;
	VkPipeline bloomUpPipeline = VK_NULL_HANDLE; // additive
	VkPipeline debugLinePipeline = VK_NULL_HANDLE;
	// 2D overlays (vk_overlay.cpp): gizmo lines and textured quads.
	VkDescriptorSetLayout overlaySetLayout = VK_NULL_HANDLE;
	VkPipelineLayout overlayLayout = VK_NULL_HANDLE;
	VkPipeline overlayPipeline = VK_NULL_HANDLE;
	VkPipeline gizmoLinePipeline = VK_NULL_HANDLE;
	// Anti-aliasing: FXAA (sampler layout) and TAA.
	VkPipeline fxaaPipeline = VK_NULL_HANDLE;
	VkDescriptorSetLayout taaSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout taaLayout = VK_NULL_HANDLE;
	VkPipeline taaPipeline = VK_NULL_HANDLE;
	// Custom shaders and custom post-processes.
	std::unordered_map<HRL_id, CustomShader> customShaders;
	VkDescriptorSetLayout postSetLayout = VK_NULL_HANDLE; // scene, bright scene, depth, parameters, normals
	VkPipelineLayout postLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout glCompatSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout glCompatLayout = VK_NULL_HANDLE;
	VkPipeline glCompatPipeline = VK_NULL_HANDLE;
	// Particles (vk_vfx.cpp): billboards and mesh particles, per blend mode (alpha, additive, multiply).
	VkDescriptorSetLayout vfxSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout vfxLayout = VK_NULL_HANDLE;
	VkPipeline vfxPipelines[3]{};
	VkPipeline vfxMeshPipelines[3]{};
	// 2D voxel worlds (vk_voxel.cpp).
	VkDescriptorSetLayout voxelSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout voxelLayout = VK_NULL_HANDLE;
	VkPipeline voxelPipeline = VK_NULL_HANDLE, voxelEmissivePipeline = VK_NULL_HANDLE;
	VkPipelineLayout voxelEmitLayout = VK_NULL_HANDLE, voxelLightLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout voxelLightSetLayout = VK_NULL_HANDLE;
	VkPipeline voxelEmitPipeline = VK_NULL_HANDLE, voxelLightPipeline = VK_NULL_HANDLE;
	// Screen space displacement materials.
	VkPipelineLayout displacementLayout = VK_NULL_HANDLE;
	VkPipeline displacementPipeline = VK_NULL_HANDLE;
	VkPipeline debugTrianglePipeline = VK_NULL_HANDLE;
	VkPipeline envCubePipeline = VK_NULL_HANDLE;
	VkPipeline envFilterPipeline = VK_NULL_HANDLE;
	VkPipeline brdfLutPipeline = VK_NULL_HANDLE;
	VkPipelineCache pipelineCache = VK_NULL_HANDLE;
	// Destroyed at shutdown (registered by vk_pipelines.cpp).
	std::vector<VkPipeline> ownedPipelines;
	std::vector<VkPipelineLayout> ownedPipelineLayouts;
	std::vector<VkDescriptorSetLayout> ownedSetLayouts;
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
	VkSampler pointClamp = VK_NULL_HANDLE; // depth and exact texel reads
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
	Image pointShadowMaps;     // kMaxPointShadows * 6 layers
	std::vector<VkImageView> pointShadowLayers;
	uint32_t pointShadowResolution = 0;

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
// Loads the pipeline cache file when a path was given (see SetPipelineCachePath).
bool CreatePipelineCache();
void SavePipelineCache();
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
// Records the build in the upload command buffer. The inputs must stay alive until it ran.
bool BuildBlas(Blas& blas, VkDeviceAddress vertices, uint32_t stride, uint32_t vertexCount,
	VkDeviceAddress indices, uint32_t indexCount, bool opaque);
// Bindless slots for images that are not HRL textures (landscape heights...).
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
uint32_t AllocBindlessSlot();
void WriteBindlessView(uint32_t slot, VkImageView view, VkSampler sampler);
void FreeBindlessSlot(uint32_t slot);

// ---- vk_culling.cpp ---------------------------------------------------------
// Uploads what changed in the scene data to the persistent GPU buffers and
// sizes them for `views` culling views this frame. Adds a pass to `graph`.
bool PrepareCulling(SceneGPU& gpu, uint32_t views, FrameGraph& graph);
// Reads the results of the frame that last used this frame slot.
void ReadCullingResults(SceneGPU& gpu);
// Copies the counts / LOD state for ReadCullingResults (last pass of the scene).
void AddCullingReadback(SceneGPU& gpu, FrameGraph& graph, uint32_t mainViews, uint32_t translucentDraws);

struct CulledList
{
	uint32_t view = 0;                 // slot in the command / count buffers
	uint32_t capacity[kCulledBuckets]{};
	bool empty = true;
};
struct CullRequest
{
	glm::mat4 viewProj{1.f};
	FrustumPlaneSet frustum{};
	glm::vec3 camPos{0.f}, camForward{0.f, 0.f, -1.f};
	float projScale = 1.f;
	uint32_t phase = 0;      // 0 : single pass, 1 / 2 : two-phase occlusion
	uint32_t viewportBit = 0;
	uint32_t flags = 0;
	VkImageView hzbView = VK_NULL_HANDLE;
	glm::vec2 hzbSize{1.f};
	uint32_t hzbMips = 1;
};
// Adds a compute pass that culls the scene for one view.
CulledList AddCullPass(FrameGraph& graph, SceneGPU& gpu, uint32_t view, const CullRequest& request, FrameGraph::Handle hzb);
void DrawCulled(VkCommandBuffer cmd, const SceneGPU& gpu, const CulledList& list, uint32_t bucket);

struct Hzb
{
	FrameGraph::Handle image = 0;
	VkImageView view = VK_NULL_HANDLE; // all mips, sampled
	glm::vec2 size{1.f};
	uint32_t mips = 0;                 // 0 : no HZB
};
// Hierarchical depth of the viewport rectangle of `depth`, written to `target`.
Hzb AddHzbPasses(FrameGraph& graph, HzbImage& target, FrameGraph::Handle depth, VkImageView depthView,
	uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void DestroyHzb(HzbImage& hzb);

// ---- vk_landscape.cpp --------------------------------------------------------
// Uploads the changed heights of the landscapes of a scene (adds a pass).
void PrepareLandscapes(const hrl_scene_t* scene, FrameGraph& graph);
void CollectLandscapeRayInstances(const hrl_scene_t* scene, std::vector<VkAccelerationStructureInstanceKHR>& out);
// One per landscape: uniform data for a camera (the LOD of every view of a viewport follows its camera).
std::vector<LandscapeDraw> PrepareLandscapeDraws(const hrl_scene_t* scene, SceneData& data,
	const HRL_SceneFrame& frame, const glm::vec3& camera);
LandscapeNodes SelectLandscapeNodes(const LandscapeDraw& draw, const glm::vec3& camera, const FrustumPlaneSet& frustum);
// Needs the landscape pipeline and the view / bindless sets bound.
void DrawLandscape(VkCommandBuffer cmd, const LandscapeDraw& draw, const LandscapeNodes& nodes);
void ReleaseUnusedLandscapes();
void DestroyAllLandscapes();

// ---- vk_foliage.cpp / vk_impostor.cpp -------------------------------------------
// Adds the part materials to the material table (before it is uploaded).
void ResolveFoliageMaterials(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame);
// Uploads the changed instances (adds a pass) and returns what the views draw.
std::vector<FoliageDraw> PrepareFoliage(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame, FrameGraph& graph);
// Culls a foliage kind for one view; the next DrawFoliage of it draws the result.
void AddFoliageCull(FrameGraph& graph, const FoliageDraw& draw, const CullRequest& request, const glm::vec3& lodCamera,
	FrameGraph::Handle hzb, bool readback = false);
// Needs the view / bindless sets bound with a layout compatible with foliageLayout.
void DrawFoliage(VkCommandBuffer cmd, const FoliageDraw& draw, bool shadow, const glm::mat4& lightViewProj, const glm::vec3& lightDir);
void ReleaseUnusedFoliage();
void DestroyAllFoliage();
// Foliage instances near `camera` for the ray tracing structure.
void CollectFoliageRayInstances(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame,
	const glm::vec3& camera, RayScene& out);
void AddRayInstance(RayScene& out, const glm::mat4& model, const MeshGPU& gpu, uint32_t material, bool masked);
// Impostors: baked when the geometry of a foliage kind changes (adds passes).
void PrepareImpostors(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame, FrameGraph& graph, VkDescriptorSet viewSet);
void DrawImpostors(VkCommandBuffer cmd, const FoliageDraw& draw, bool shadow, const glm::mat4& lightViewProj, const glm::vec3& lightDir);
void DestroyImpostor(FoliageGPU& g);

// ---- vk_grass.cpp ------------------------------------------------------------
// Needs the landscapes prepared (heights uploaded) first.
std::vector<GrassDraw> PrepareGrass(const hrl_scene_t* scene, const glm::vec3& camera);
void AddGrassCull(FrameGraph& graph, const GrassDraw& draw, const CullRequest& request, const glm::vec3& camera, FrameGraph::Handle hzb);
void DrawGrass(VkCommandBuffer cmd, const GrassDraw& draw, bool shadow, const glm::mat4& lightViewProj);
void ReleaseUnusedGrass();
void DestroyAllGrass();

// ---- vk_environment.cpp -----------------------------------------------------
glm::mat4 SkyRotation(const glm::vec3& degrees);
void DestroyEnvironment(Environment& env);
// Returns true when the scene is lit by an environment. `dirty` is set when
// RecordEnvironment must run this frame (sky inputs changed).
bool PrepareEnvironment(SceneGPU& gpu, const hrl_scene_t* scene, bool& dirty);
void RecordEnvironment(VkCommandBuffer cmd, SceneGPU& gpu, const hrl_scene_t* scene);
// One-off: BRDF lookup table and the placeholder cube map.
void RecordBrdfLut(VkCommandBuffer cmd);

// ---- vk_custom.cpp -----------------------------------------------------------
bool RuntimeShadersAvailable();
bool CreateCustomShader(HRL_id id, const std::string& vert, const std::string& frag);
void DeleteCustomShader(HRL_id id);
const CustomShader* FindCustomShader(HRL_id id);
// Pipelines of a surface shader for a draw bucket / shadows, of a post-process shader.
VkPipeline CustomForwardPipeline(HRL_id id, uint32_t bucket);
VkPipeline CustomShadowPipeline(HRL_id id, bool masked);
VkPipeline CustomPostPipeline(HRL_id id);
// paramSlots vec4s, from the material (or the defaults of the shader).
void WriteCustomParams(const CustomShader& shader, const HRL_Material* m, glm::vec4* out);
void WriteEffectParams(const CustomShader& shader, const EffectParams& values, glm::vec4* out);
// Automatic values of the custom post-processes (camera, sun, ...): written for the
// parameters the shader declares and the material does not set itself.
void WriteAutoParams(const CustomShader& shader, const EffectParams& values, const HRL_Material* m, glm::vec4* out);
// Built-in OpenGL effect shaders (backend/common/hrl_builtin_glsl.h) compiled
// for Vulkan with the post-process rules; null when it does not compile.
const CustomShader* EffectShader(const char* name, const char* source);
// vk_pipelines.cpp: variants built from run time SPIR-V. vs null: the built-in mesh vertex shader.
VkPipeline MakeMeshPipeline(const HRL_VkShaderBlob* vs, const HRL_VkShaderBlob* fs, uint32_t bucket);
VkPipeline MakeCustomShadowPipeline(const HRL_VkShaderBlob* vs, const HRL_VkShaderBlob* fs);
VkPipeline MakePostPipeline(const HRL_VkShaderBlob* fs);
// Render passes (vk_render_pipeline.cpp): fullscreen.vert + `fs`, one HRL_EPassBlend per attachment.
VkPipeline MakeFullscreenPassPipeline(const HRL_VkShaderBlob* fs, VkPipelineLayout layout,
	const std::vector<VkFormat>& formats, const std::vector<uint32_t>& blends);
VkPipeline MakeComputePassPipeline(const HRL_VkShaderBlob* cs, VkPipelineLayout layout);
// Run time compiler and parameter tables of vk_custom.cpp, for the render pass shaders.
bool CompilePassGlsl(bool compute, const std::string& source, std::vector<uint32_t>& spirv, std::string& log);
void ExtractPassParams(const std::string& src, std::string& code, std::vector<CustomParam>& params);
uint32_t LayoutPassParams(std::vector<CustomParam>& params);
std::string PassParamGlobals(const std::vector<CustomParam>& params);
std::string StripGlslComments(const std::string& src);

// ---- vk_vfx.cpp --------------------------------------------------------------
struct Camera;
struct VFXFrame
{
	struct Batch { uint32_t first = 0, count = 0, blend = 0, texture = 0xFFFFFFFFu; };
	struct MeshBatch { uint32_t first = 0, count = 0, blend = 0, indexCount = 0, firstIndex = 0; int32_t vertexOffset = 0; };
	std::vector<Batch> billboards;
	VkDescriptorSet particleSet = VK_NULL_HANDLE;
	std::vector<MeshBatch> meshes;
	FrameSlice meshInstances;                 // InstanceData of the mesh particles
	VkDescriptorSet meshViewSet = VK_NULL_HANDLE; // view set whose instance table is meshInstances
};
// Adds the materials of the mesh particles to the table (before it is uploaded).
void ResolveVFXMaterials(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame);
VFXFrame PrepareVFX(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame, const Camera& cam);
// In the forward pass, after the translucent surfaces.
void DrawVFX(VkCommandBuffer cmd, const VFXFrame& f, VkDescriptorSet viewSet);

// ---- vk_voxel.cpp ------------------------------------------------------------
struct VoxelDraws
{
	struct Chunk { VkBuffer vertices, indices; uint32_t indexCount; };
	std::vector<Chunk> regular, emissive;
	VkDescriptorSet set = VK_NULL_HANDLE; // textures of the voxel types
};
// Builds / uploads the chunks the viewports of the scene see.
void PrepareVoxels(HRL_id sceneId, const hrl_scene_t* scene);
VoxelDraws PrepareVoxelDraws(HRL_id sceneId, const hrl_scene_t* scene, const FrustumPlaneSet& frustum);
void DrawVoxels(VkCommandBuffer cmd, const VoxelDraws& draws, VkDescriptorSet viewSet);
void ReleaseUnusedVoxels();
void DestroyAllVoxels();
// Light of the emissive voxels around a viewport (HRL_SetVoxelEmissiveLighting).
struct VoxelLightField
{
	FrameGraph::Handle image = 0;
	VkImageView view = VK_NULL_HANDLE;
	glm::vec4 region{0.f};
	float falloff = 1.f;
	bool active = false;
};
VoxelLightField AddVoxelLightField(FrameGraph& graph, HRL_id sceneId, const hrl_scene_t* scene, const HRL_Viewport* viewport);

// ---- vk_gi.cpp ---------------------------------------------------------------
struct GIFrame
{
	FrameSlice probes;                 // 6 vec4 lobes per probe
	glm::vec4 origin{0.f};             // w : strength
	glm::vec4 spacing{1.f}, resolution{0.f};
};
// Updates the probe volume of a scene (a few probes per frame) and uploads it.
GIFrame PrepareGI(HRL_id sceneId, const hrl_scene_t* scene);
void ReleaseGIScene(HRL_id sceneId);
void ShutdownGI();

// ---- vk_effects.cpp ----------------------------------------------------------
struct EffectView
{
	const HRL_Viewport* viewport = nullptr;
	float x = 0.f, y = 0.f, w = 1.f, h = 1.f; // rectangle in the scene image, origin top-left
	glm::mat4 viewProj{1.f};                  // Vulkan clip space, as rendered (jitter included)
	glm::mat4 viewGL{1.f}, projGL{1.f};       // OpenGL conventions (effect shaders)
	glm::vec3 camera{0.f};
};
// Screen space effects of the scene (decals, SSR, ambient occlusion, volumetric
// fog, god rays, clouds), then the custom post-processes of each viewport, on
// the HDR image. Returns the image that holds the result.
FrameGraph::Handle AddScreenEffects(FrameGraph& graph, SceneGPU& gpu, const HRL_SceneFrame& frame,
	const std::vector<EffectView>& views, FrameGraph::Handle color, FrameGraph::Handle depth);

// ---- vk_render_pipeline.cpp ---------------------------------------------------
// Render pass shaders (HRL_CreateComputeShader / HRL_CreateFullscreenShader).
bool CreatePassShader(HRL_id id, const std::string& source, bool compute);
bool IsPassShader(HRL_id id);
void DeletePassShader(HRL_id id);
// Participating media of a viewport (HRL_SetViewportMediaVolume): the 3D images
// its view set binds (placeholders when it has none) and ViewData::mediaParams.
struct MediaBinding
{
	Image* integrated = nullptr;
	Image* visibility = nullptr;
	glm::vec4 params{1.f, 1.f, 0.f, 0.f};
	bool active = false;
};
MediaBinding PrepareViewportMedia(FrameGraph& graph, const hrl_scene_t* scene, HRL_id viewportId, const HRL_Viewport* viewport,
	uint32_t vw, uint32_t vh);
// One viewport of the frame, as the render passes see it.
struct PassView
{
	HRL_id sceneId = HRL_INVALID_ID, viewportId = HRL_INVALID_ID;
	const HRL_Viewport* viewport = nullptr;
	float x = 0.f, y = 0.f, w = 1.f, h = 1.f;    // rectangle in the scene image, origin top-left
	uint32_t sceneWidth = 1, sceneHeight = 1;
	VkDescriptorSet viewSet = VK_NULL_HANDLE;
	glm::mat4 viewProjNoJitter{1.f};
	glm::vec3 camera{0.f}, forward{0.f, 0.f, -1.f};
	glm::vec2 jitter{0.f};                      // NDC offset of the anti-aliasing
	Image* mediaIntegrated = nullptr;           // images bound by the view set (16, 17)
	Image* mediaVisibility = nullptr;
};
bool HasRenderPasses(const HRL_Viewport* viewport, HRL_ERenderStage stage);
// Records the camera of this frame (reprojection data of the next one).
void BeginRenderPassView(const PassView& view);
// Adds the enabled passes of `stage` of the viewport. Returns the image that
// holds the HDR scene afterwards (a writable copy at HRL_STAGE_BEFORE_TONEMAP).
FrameGraph::Handle AddRenderPasses(FrameGraph& graph, const HRL_SceneFrame& frame, const PassView& view, HRL_ERenderStage stage,
	FrameGraph::Handle color, FrameGraph::Handle depth, FrameGraph::Handle shadows, FrameGraph::Handle pointShadows);
// End of the frame of a viewport: histories flip, the camera becomes the previous one.
void EndRenderPassFrame(HRL_id sceneId, HRL_id viewportId);
void ReleaseUnusedRenderTargets();
void ShutdownRenderPipeline();

// ---- vk_overlay.cpp ----------------------------------------------------------
struct OverlayView
{
	HRL_id viewportId = HRL_INVALID_ID;
	const HRL_Viewport* viewport = nullptr;
	float x = 0.f, y = 0.f, w = 1.f, h = 1.f; // rectangle in the scene image, origin top-left
	glm::mat4 viewProj{1.f};                  // Vulkan clip space
	glm::mat4 view{1.f};
};
// Gizmos and 2D overlays of the viewports, drawn over the tone mapped image.
void AddOverlayPasses(FrameGraph& graph, const HRL_SceneFrame& frame, const std::vector<OverlayView>& views,
	FrameGraph::Handle target);

// ---- vk_scene_data.cpp ------------------------------------------------------
float MaterialFloat(const HRL_Material* m, const char* name, float fallback);
// Brings the cached scene data in line with the frontend scene.
void UpdateSceneData(SceneData& data, const HRL_SceneFrame& frame);
// Index of a material in data.materials, added when it is not there yet
// (landscapes, foliage). An unknown material gives the default material.
uint32_t MaterialIndex(SceneData& data, const HRL_SceneFrame& frame, HRL_id material);

struct LightSetup
{
	std::vector<LightData> lights;
	const HRL_Light* cascadeLight = nullptr;            // directional light using the cascades
	std::vector<const HRL_Light*> spotShadowLights;     // spot lights with a shadow map
	std::vector<const HRL_Light*> pointShadowLights;    // point lights with 6 shadow maps
	uint32_t shadowResolution = 0;                      // 0 : no shadow map this frame
	uint32_t pointShadowResolution = 0;                 // 0 : no point light shadow map
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
	glm::vec3 lightDir{0.f, -1.f, 0.f}; // direction the light travels
	glm::mat4 viewProj{1.f};   // Vulkan clip space, depth 0..1
	glm::mat4 cullClip{1.f};   // OpenGL clip space, for the CPU frustum test
	uint32_t layer = 0;
	bool point = false;        // layer of the point light shadow maps
};
// Fills the cascade matrices and splits of `view` and appends one shadow view per cascade.
void ComputeCascades(const Camera& cam, const glm::vec3& lightDir, float resolution,
	ViewData& view, std::vector<ShadowView>& out);
} // namespace hrlvk

#endif
