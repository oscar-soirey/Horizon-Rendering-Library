// HRL native Vulkan backend - scene renderer implementation.
#include "vk_renderer.h"

#include "vk_device.h"
#include "hrl_vk_shaders.h"
#include "../../core/render_common.h"
#include "../../core/utils_functions.h"

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
namespace
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

struct SceneGPU
{
	bool onScreen = false;
	bool explicitSize = false;
	uint32_t width = 0, height = 0;
	Image hdr, ids, depth, ldr;
	Environment env;
	Tlas tlas[kFramesInFlight];
	uint64_t renderedFrame = ~0ull;
	glm::mat4 lastViewProj{1.f};
	float lastRect[4] = {0.f, 0.f, 1.f, 1.f}; // pixels, top-left origin
};

struct DrawItem
{
	const HRL_Mesh* mesh = nullptr;
	const MeshGPU* gpu = nullptr;
	glm::mat4 model{1.f};
	uint32_t instance = 0;
	Bucket bucket = BucketOpaque;
	float distance2 = 0.f;
	uint32_t lod = 0;
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
};
State R;

Device& Dev() { return GetDevice(); }

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------
VkShaderModule MakeShader(const HRL_VkShaderBlob& blob)
{
	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = blob.size;
	ci.pCode = blob.code;
	VkShaderModule m = VK_NULL_HANDLE;
	VkResult r = vkCreateShaderModule(Dev().device, &ci, nullptr, &m);
	if (r != VK_SUCCESS) ReportError("vkCreateShaderModule", r);
	return m;
}

enum class VertexInput { None, Mesh, PositionOnly, Debug };

struct PipelineDesc
{
	const HRL_VkShaderBlob* vs = nullptr;
	const HRL_VkShaderBlob* fs = nullptr;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VertexInput input = VertexInput::None;
	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	std::vector<VkFormat> colorFormats;
	VkFormat depthFormat = VK_FORMAT_UNDEFINED;
	bool depthTest = false, depthWrite = false;
	VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
	VkCullModeFlags cull = VK_CULL_MODE_NONE;
	bool depthBias = false;
	bool depthClamp = false;
};

VkPipeline MakeGraphicsPipeline(const PipelineDesc& d)
{
	VkShaderModule vs = MakeShader(*d.vs);
	VkShaderModule fs = d.fs ? MakeShader(*d.fs) : VK_NULL_HANDLE;
	VkPipelineShaderStageCreateInfo stages[2]{};
	uint32_t stageCount = 1;
	stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vs;
	stages[0].pName = "main";
	if (fs)
	{
		stages[1] = stages[0];
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fs;
		stageCount = 2;
	}

	VkVertexInputBindingDescription binding{0, sizeof(HRL_Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX};
	std::vector<VkVertexInputAttributeDescription> attrs;
	if (d.input == VertexInput::Mesh)
		attrs = {
			{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(HRL_Vertex3D, position)},
			{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(HRL_Vertex3D, normal)},
			{2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HRL_Vertex3D, uv)},
			{3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(HRL_Vertex3D, tangent)},
			{4, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(HRL_Vertex3D, bitangent)}};
	else if (d.input == VertexInput::PositionOnly)
		attrs = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(HRL_Vertex3D, position)}};
	else if (d.input == VertexInput::Debug)
	{
		binding.stride = sizeof(DebugVertex);
		attrs = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}};
	}
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	if (d.input != VertexInput::None)
	{
		vi.vertexBindingDescriptionCount = 1;
		vi.pVertexBindingDescriptions = &binding;
		vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size();
		vi.pVertexAttributeDescriptions = attrs.data();
	}

	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = d.topology;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = d.cull;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.f;
	rs.depthBiasEnable = d.depthBias ? VK_TRUE : VK_FALSE;
	rs.depthClampEnable = d.depthClamp ? VK_TRUE : VK_FALSE;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = d.depthTest ? VK_TRUE : VK_FALSE;
	ds.depthWriteEnable = d.depthWrite ? VK_TRUE : VK_FALSE;
	ds.depthCompareOp = d.depthCompare;
	std::vector<VkPipelineColorBlendAttachmentState> blends(d.colorFormats.size());
	for (auto& b : blends) b.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = (uint32_t)blends.size();
	cb.pAttachments = blends.data();
	VkDynamicState dynamics[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
	VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dyn.dynamicStateCount = d.depthBias ? 3u : 2u;
	dyn.pDynamicStates = dynamics;

	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = (uint32_t)d.colorFormats.size();
	ri.pColorAttachmentFormats = d.colorFormats.data();
	ri.depthAttachmentFormat = d.depthFormat;

	VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
	ci.stageCount = stageCount;
	ci.pStages = stages;
	ci.pVertexInputState = &vi;
	ci.pInputAssemblyState = &ia;
	ci.pViewportState = &vp;
	ci.pRasterizationState = &rs;
	ci.pMultisampleState = &ms;
	ci.pDepthStencilState = &ds;
	ci.pColorBlendState = &cb;
	ci.pDynamicState = &dyn;
	ci.layout = d.layout;
	VkPipeline p = VK_NULL_HANDLE;
	VkResult r = vkCreateGraphicsPipelines(Dev().device, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
	if (r != VK_SUCCESS) ReportError("vkCreateGraphicsPipelines", r);
	vkDestroyShaderModule(Dev().device, vs, nullptr);
	if (fs) vkDestroyShaderModule(Dev().device, fs, nullptr);
	return p;
}

VkPipeline MakeComputePipeline(const HRL_VkShaderBlob& blob, VkPipelineLayout layout)
{
	VkShaderModule m = MakeShader(blob);
	VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	ci.stage.module = m;
	ci.stage.pName = "main";
	ci.layout = layout;
	VkPipeline p = VK_NULL_HANDLE;
	VkResult r = vkCreateComputePipelines(Dev().device, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
	if (r != VK_SUCCESS) ReportError("vkCreateComputePipelines", r);
	vkDestroyShaderModule(Dev().device, m, nullptr);
	return p;
}

VkDescriptorSetLayout MakeSetLayout(const std::vector<VkDescriptorSetLayoutBinding>& bindings,
	const VkDescriptorBindingFlags* flags = nullptr, VkDescriptorSetLayoutCreateFlags layoutFlags = 0)
{
	VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
	fi.bindingCount = (uint32_t)bindings.size();
	fi.pBindingFlags = flags;
	VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, flags ? &fi : nullptr};
	ci.flags = layoutFlags;
	ci.bindingCount = (uint32_t)bindings.size();
	ci.pBindings = bindings.data();
	VkDescriptorSetLayout l = VK_NULL_HANDLE;
	VkResult r = vkCreateDescriptorSetLayout(Dev().device, &ci, nullptr, &l);
	if (r != VK_SUCCESS) ReportError("vkCreateDescriptorSetLayout", r);
	return l;
}

VkPipelineLayout MakePipelineLayout(const std::vector<VkDescriptorSetLayout>& sets, VkShaderStageFlags pushStages, uint32_t pushSize)
{
	VkPushConstantRange range{pushStages, 0, pushSize};
	VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	ci.setLayoutCount = (uint32_t)sets.size();
	ci.pSetLayouts = sets.data();
	ci.pushConstantRangeCount = pushSize ? 1u : 0u;
	ci.pPushConstantRanges = &range;
	VkPipelineLayout l = VK_NULL_HANDLE;
	VkResult r = vkCreatePipelineLayout(Dev().device, &ci, nullptr, &l);
	if (r != VK_SUCCESS) ReportError("vkCreatePipelineLayout", r);
	return l;
}

VkDescriptorSet AllocFrameSet(VkDescriptorSetLayout layout)
{
	VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	ai.descriptorPool = Dev().FrameDescriptorPool();
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &layout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkResult r = vkAllocateDescriptorSets(Dev().device, &ai, &set);
	if (r != VK_SUCCESS) ReportError("per-frame descriptor allocation", r);
	return set;
}

bool CreatePipelines()
{
	Device& dev = Dev();
	const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	std::vector<VkDescriptorSetLayoutBinding> view = {
		{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VF, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VF, nullptr},
		{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
	if (dev.caps.rayQuery)
		view.push_back({8, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
	R.viewLayout = MakeSetLayout(view);

	const VkDescriptorBindingFlags bindlessFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
	const uint32_t bindlessCount = dev.caps.maxBindlessTextures * 2u;
	R.bindlessLayout = MakeSetLayout({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, bindlessCount, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
		&bindlessFlags, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);
	R.imageLayout = MakeSetLayout({
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}});
	R.samplerLayout = MakeSetLayout({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
	if (!R.viewLayout || !R.bindlessLayout || !R.imageLayout || !R.samplerLayout) return false;

	R.sceneLayout = MakePipelineLayout({R.viewLayout, R.bindlessLayout}, VF, sizeof(SkyPush));
	R.computeLayout = MakePipelineLayout({R.imageLayout}, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(SkyPush));
	R.tonemapLayout = MakePipelineLayout({R.samplerLayout}, VK_SHADER_STAGE_FRAGMENT_BIT, 8);
	R.debugLayout = MakePipelineLayout({}, VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4));

	VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, bindlessCount};
	VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
	pi.maxSets = 1;
	pi.poolSizeCount = 1;
	pi.pPoolSizes = &poolSize;
	if (vkCreateDescriptorPool(dev.device, &pi, nullptr, &R.bindlessPool) != VK_SUCCESS) return false;
	VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	ai.descriptorPool = R.bindlessPool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &R.bindlessLayout;
	if (vkAllocateDescriptorSets(dev.device, &ai, &R.bindlessSet) != VK_SUCCESS) return false;

	const bool rq = dev.caps.rayQuery;
	const HRL_VkShaderBlob* opaqueFs = rq ? &hrl_vk_mesh_rq_frag : &hrl_vk_mesh_frag;
	const HRL_VkShaderBlob* maskedFs = rq ? &hrl_vk_mesh_at_rq_frag : &hrl_vk_mesh_at_frag;
	for (uint32_t b = 0; b < BucketCount; ++b)
	{
		PipelineDesc d;
		d.vs = &hrl_vk_mesh_vert;
		d.fs = (b == BucketMasked || b == BucketMaskedTwoSided) ? maskedFs : opaqueFs;
		d.layout = R.sceneLayout;
		d.input = VertexInput::Mesh;
		d.colorFormats = {kHdrFormat, kIdFormat};
		d.depthFormat = kDepthFormat;
		d.depthTest = d.depthWrite = true;
		d.depthCompare = VK_COMPARE_OP_GREATER; // reversed-Z
		d.cull = (b == BucketOpaqueTwoSided || b == BucketMaskedTwoSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
		R.meshPipelines[b] = MakeGraphicsPipeline(d);
		if (!R.meshPipelines[b]) return false;
	}
	{
		PipelineDesc d;
		d.vs = &hrl_vk_shadow_vert;
		d.layout = R.sceneLayout;
		d.input = VertexInput::PositionOnly;
		d.depthFormat = kDepthFormat;
		d.depthTest = d.depthWrite = true;
		d.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
		d.depthBias = true;
		R.shadowPipeline = MakeGraphicsPipeline(d);
	}
	{
		PipelineDesc d;
		d.vs = &hrl_vk_fullscreen_vert;
		d.fs = &hrl_vk_sky_frag;
		d.layout = R.sceneLayout;
		d.colorFormats = {kHdrFormat, kIdFormat};
		d.depthFormat = kDepthFormat;
		d.depthTest = true;
		d.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL; // only where depth is still 0 (far)
		R.skyPipeline = MakeGraphicsPipeline(d);
	}
	{
		PipelineDesc d;
		d.vs = &hrl_vk_fullscreen_vert;
		d.fs = &hrl_vk_tonemap_frag;
		d.layout = R.tonemapLayout;
		d.colorFormats = {R.ldrFormat};
		R.tonemapPipeline = MakeGraphicsPipeline(d);
	}
	{
		PipelineDesc d;
		d.vs = &hrl_vk_debug_vert;
		d.fs = &hrl_vk_debug_frag;
		d.layout = R.debugLayout;
		d.input = VertexInput::Debug;
		d.colorFormats = {R.ldrFormat};
		d.depthFormat = kDepthFormat;
		d.depthTest = true;
		d.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
		d.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		R.debugLinePipeline = MakeGraphicsPipeline(d);
		d.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		R.debugTrianglePipeline = MakeGraphicsPipeline(d);
	}
	R.envCubePipeline = MakeComputePipeline(hrl_vk_env_cube_comp, R.computeLayout);
	R.envFilterPipeline = MakeComputePipeline(hrl_vk_env_filter_comp, R.computeLayout);
	R.brdfLutPipeline = MakeComputePipeline(hrl_vk_brdf_lut_comp, R.computeLayout);
	return R.shadowPipeline && R.skyPipeline && R.tonemapPipeline && R.debugLinePipeline &&
		R.debugTrianglePipeline && R.envCubePipeline && R.envFilterPipeline && R.brdfLutPipeline;
}

// ---------------------------------------------------------------------------
// Samplers / textures
// ---------------------------------------------------------------------------
VkSampler GetTextureSampler(HRL_EFilterType minFilter, HRL_EFilterType magFilter)
{
	const uint64_t key = (uint64_t(minFilter) << 32) | uint64_t(magFilter);
	auto it = R.samplers.find(key);
	if (it != R.samplers.end()) return it->second;
	VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	ci.magFilter = magFilter == HRL_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	ci.minFilter = minFilter == HRL_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	ci.mipmapMode = (minFilter == HRL_FILTER_TRILINEAR || minFilter == HRL_FILTER_ANISOTROPIC)
		? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	// NEAREST and LINEAR keep the historical "no mip" behaviour.
	ci.maxLod = (minFilter == HRL_FILTER_NEAREST || minFilter == HRL_FILTER_LINEAR) ? 0.f : VK_LOD_CLAMP_NONE;
	if (minFilter == HRL_FILTER_ANISOTROPIC && Dev().caps.anisotropy)
	{
		ci.anisotropyEnable = VK_TRUE;
		ci.maxAnisotropy = std::min(8.f, Dev().caps.maxAnisotropy);
	}
	VkSampler s = VK_NULL_HANDLE;
	vkCreateSampler(Dev().device, &ci, nullptr, &s);
	R.samplers.emplace(key, s);
	return s;
}

void WriteBindless(const Texture& t)
{
	VkSampler sampler = GetTextureSampler(t.minFilter, t.magFilter);
	VkDescriptorImageInfo infos[2] = {
		{sampler, t.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		{sampler, t.srgbView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = R.bindlessSet;
	w.dstBinding = 0;
	w.dstArrayElement = t.slot * 2u;
	w.descriptorCount = 2;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = infos;
	vkUpdateDescriptorSets(Dev().device, 1, &w, 0, nullptr);
}

bool CreateTextureInternal(Texture& t, const unsigned char* rgba, int width, int height)
{
	Device& dev = Dev();
	if (R.freeSlots.empty() && R.nextSlot >= dev.caps.maxBindlessTextures)
	{
		ReportError("texture limit reached (" + std::to_string(dev.caps.maxBindlessTextures) + ")");
		return false;
	}
	ImageDesc d;
	d.format = VK_FORMAT_R8G8B8A8_UNORM;
	d.width = (uint32_t)width;
	d.height = (uint32_t)height;
	d.mips = (uint32_t)std::floor(std::log2((float)std::max(width, height))) + 1u;
	d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	d.mutableFormat = true;
	if (!dev.CreateImage(d, t.image, "texture")) return false;
	t.srgbView = dev.CreateView(t.image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_VIEW_TYPE_2D, 0, t.image.mips, 0, 1);
	if (!t.srgbView || !dev.UploadToImage(t.image, 0, rgba, VkDeviceSize(width) * VkDeviceSize(height) * 4u))
	{
		if (t.srgbView) vkDestroyImageView(dev.device, t.srgbView, nullptr);
		dev.DestroyImage(t.image, false);
		return false;
	}
	dev.GenerateMips(dev.UploadCmd(), t.image);
	if (!R.freeSlots.empty()) { t.slot = R.freeSlots.back(); R.freeSlots.pop_back(); }
	else t.slot = R.nextSlot++;
	WriteBindless(t);
	return true;
}

void DestroyTextureInternal(Texture& t, bool releaseSlot)
{
	Device& dev = Dev();
	VkDevice d = dev.device;
	VkImageView v = t.srgbView;
	const uint32_t slot = t.slot;
	dev.Defer([d, v, slot, releaseSlot]()
	{
		if (v) vkDestroyImageView(d, v, nullptr);
		// The slot is reused only once no in-flight frame can sample it.
		if (releaseSlot) R.freeSlots.push_back(slot);
	});
	dev.DestroyImage(t.image);
	t = Texture{};
}

uint32_t TextureSlot(const HRL_Material* m, const char* key, uint32_t fallback)
{
	auto it = m->textureParams_.find(key);
	if (it == m->textureParams_.end()) return fallback;
	auto t = R.textures.find(it->second);
	return t == R.textures.end() ? fallback : t->second.slot;
}

// ---------------------------------------------------------------------------
// Geometry pool
// ---------------------------------------------------------------------------
VkBufferUsageFlags GeometryUsage(bool index)
{
	VkBufferUsageFlags u = (index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	if (Dev().caps.rayQuery)
		u |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	return u;
}

// Makes sure the pool buffer holds `needed` bytes; existing data is preserved.
bool EnsurePoolCapacity(Buffer& buffer, VkDeviceSize needed, bool index)
{
	if (buffer && buffer.size >= needed) return true;
	Device& dev = Dev();
	Buffer grown;
	const VkDeviceSize size = std::max<VkDeviceSize>(needed, buffer ? buffer.size * 2 : (index ? (8ull << 20) : (32ull << 20)));
	if (!dev.CreateBuffer(size, GeometryUsage(index), false, grown, index ? "index pool" : "vertex pool")) return false;
	if (buffer)
	{
		VkBufferCopy copy{0, 0, buffer.size};
		vkCmdCopyBuffer(dev.UploadCmd(), buffer.handle, grown.handle, 1, &copy);
		dev.DestroyBuffer(buffer);
	}
	buffer = grown;
	return true;
}

void FreeLOD(MeshLOD& lod)
{
	if (lod.vertexAlloc) vmaVirtualFree(R.vertexBlock, lod.vertexAlloc);
	if (lod.indexAlloc) vmaVirtualFree(R.indexBlock, lod.indexAlloc);
	lod = MeshLOD{};
}

void DestroyBlas(Blas& blas)
{
	if (!blas.handle) return;
	VkDevice d = Dev().device;
	VkAccelerationStructureKHR h = blas.handle;
	Dev().Defer([d, h]() { R.destroyAS(d, h, nullptr); });
	Dev().DestroyBuffer(blas.storage);
	blas = Blas{};
}

VkDeviceAddress AlignUp(VkDeviceAddress v, VkDeviceSize a) { return (v + a - 1) / a * a; }

void BuildBlas(MeshGPU& mesh)
{
	Device& dev = Dev();
	const MeshLOD& lod = mesh.lods[0];
	VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
	geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	auto& tri = geo.geometry.triangles;
	tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	tri.vertexData.deviceAddress = R.vertexBuffer.address + VkDeviceSize(lod.vertexOffset) * sizeof(HRL_Vertex3D);
	tri.vertexStride = sizeof(HRL_Vertex3D);
	tri.maxVertex = lod.vertexCount - 1;
	tri.indexType = VK_INDEX_TYPE_UINT32;
	tri.indexData.deviceAddress = R.indexBuffer.address + VkDeviceSize(lod.firstIndex) * sizeof(uint32_t);

	VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geo;
	const uint32_t primitives = lod.indexCount / 3;
	VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
	R.buildSizesAS(dev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);

	Buffer scratch;
	if (!dev.CreateBuffer(sizes.accelerationStructureSize,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false, mesh.blas.storage, "blas") ||
		!dev.CreateBuffer(sizes.buildScratchSize + R.scratchAlignment,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false, scratch, "blas scratch"))
	{
		dev.DestroyBuffer(mesh.blas.storage, false);
		return;
	}
	VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
	ci.buffer = mesh.blas.storage.handle;
	ci.size = sizes.accelerationStructureSize;
	ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if (R.createAS(dev.device, &ci, nullptr, &mesh.blas.handle) != VK_SUCCESS)
	{
		dev.DestroyBuffer(mesh.blas.storage, false);
		dev.DestroyBuffer(scratch, false);
		return;
	}
	info.dstAccelerationStructure = mesh.blas.handle;
	info.scratchData.deviceAddress = AlignUp(scratch.address, R.scratchAlignment);

	VkCommandBuffer cmd = dev.UploadCmd();
	// The geometry was copied into the pool by this same command buffer.
	VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
	mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
	mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
	VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.memoryBarrierCount = 1;
	di.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2(cmd, &di);
	VkAccelerationStructureBuildRangeInfoKHR range{primitives, 0, 0, 0};
	const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
	R.buildAS(cmd, 1, &info, &ranges);
	dev.DestroyBuffer(scratch);

	VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
	ai.accelerationStructure = mesh.blas.handle;
	mesh.blas.address = R.addressAS(dev.device, &ai);
}

// ---------------------------------------------------------------------------
// Scene render targets
// ---------------------------------------------------------------------------
void DestroyEnvironment(Environment& env)
{
	Device& dev = Dev();
	VkDevice d = dev.device;
	std::vector<VkImageView> views = env.prefilteredMips;
	views.push_back(env.cubeStorage);
	views.push_back(env.irradianceStorage);
	dev.Defer([d, views]() { for (VkImageView v : views) if (v) vkDestroyImageView(d, v, nullptr); });
	dev.DestroyImage(env.cube);
	dev.DestroyImage(env.prefiltered);
	dev.DestroyImage(env.irradiance);
	env = Environment{};
}

void DestroyTlas(Tlas& t)
{
	if (t.handle)
	{
		VkDevice d = Dev().device;
		VkAccelerationStructureKHR h = t.handle;
		Dev().Defer([d, h]() { R.destroyAS(d, h, nullptr); });
	}
	Dev().DestroyBuffer(t.storage);
	Dev().DestroyBuffer(t.instances);
	Dev().DestroyBuffer(t.scratch);
	t = Tlas{};
}

void DestroySceneTargets(SceneGPU& s)
{
	Device& dev = Dev();
	dev.DestroyImage(s.hdr);
	dev.DestroyImage(s.ids);
	dev.DestroyImage(s.depth);
	dev.DestroyImage(s.ldr);
}

bool CreateSceneTargets(SceneGPU& s, uint32_t width, uint32_t height)
{
	Device& dev = Dev();
	DestroySceneTargets(s);
	s.width = std::max(width, 1u);
	s.height = std::max(height, 1u);
	ImageDesc d;
	d.width = s.width;
	d.height = s.height;
	d.format = kHdrFormat;
	d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	bool ok = dev.CreateImage(d, s.hdr, "scene hdr");
	d.format = kIdFormat;
	d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ok = ok && dev.CreateImage(d, s.ids, "scene ids");
	d.format = kDepthFormat;
	d.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ok = ok && dev.CreateImage(d, s.depth, "scene depth");
	d.format = R.ldrFormat;
	d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ok = ok && dev.CreateImage(d, s.ldr, "scene ldr");
	if (!ok) DestroySceneTargets(s);
	return ok;
}

bool EnsureShadowMaps(uint32_t resolution, uint32_t layers)
{
	Device& dev = Dev();
	if (R.shadowMaps && R.shadowResolution == resolution && R.shadowMaps.layers == layers) return true;
	VkDevice d = dev.device;
	std::vector<VkImageView> old = R.shadowLayers;
	dev.Defer([d, old]() { for (VkImageView v : old) vkDestroyImageView(d, v, nullptr); });
	R.shadowLayers.clear();
	dev.DestroyImage(R.shadowMaps);
	ImageDesc desc;
	desc.format = kDepthFormat;
	desc.width = desc.height = resolution;
	desc.layers = layers;
	desc.arrayView = true;
	desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (!dev.CreateImage(desc, R.shadowMaps, "shadow maps")) return false;
	for (uint32_t i = 0; i < layers; ++i)
		R.shadowLayers.push_back(dev.CreateView(R.shadowMaps, kDepthFormat, VK_IMAGE_VIEW_TYPE_2D, 0, 1, i, 1));
	R.shadowResolution = resolution;
	return true;
}

// ---------------------------------------------------------------------------
// Image based lighting
// ---------------------------------------------------------------------------
void DispatchImage(VkCommandBuffer cmd, VkPipeline pipeline, VkImageView sampled, VkSampler sampler,
	VkImageView storage, uint32_t size, uint32_t layers, const void* push, uint32_t pushSize)
{
	VkDescriptorSet set = AllocFrameSet(R.imageLayout);
	if (!set) return;
	VkDescriptorImageInfo in{sampler, sampled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkDescriptorImageInfo out{VK_NULL_HANDLE, storage, VK_IMAGE_LAYOUT_GENERAL};
	VkWriteDescriptorSet w[2]{};
	w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
	w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &in;
	w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w[1].dstSet = set; w[1].dstBinding = 1; w[1].descriptorCount = 1;
	w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &out;
	vkUpdateDescriptorSets(Dev().device, 2, w, 0, nullptr);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.computeLayout, 0, 1, &set, 0, nullptr);
	if (pushSize) vkCmdPushConstants(cmd, R.computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize, push);
	const uint32_t groups = (size + 7) / 8;
	vkCmdDispatch(cmd, groups, groups, layers);
}

glm::mat4 SkyRotation(const glm::vec3& degrees)
{
	glm::mat4 r(1.f);
	r = glm::rotate(r, glm::radians(degrees.x), glm::vec3(1.f, 0.f, 0.f));
	r = glm::rotate(r, glm::radians(degrees.y), glm::vec3(0.f, 1.f, 0.f));
	r = glm::rotate(r, glm::radians(degrees.z), glm::vec3(0.f, 0.f, 1.f));
	return glm::transpose(r); // world direction -> sky direction
}

bool CreateEnvironmentImages(Environment& env)
{
	Device& dev = Dev();
	ImageDesc d;
	d.format = kHdrFormat;
	d.cube = true;
	d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	d.width = d.height = kEnvSize;
	d.mips = (uint32_t)std::log2((float)kEnvSize) + 1u;
	if (!dev.CreateImage(d, env.cube, "environment cube")) return false;
	d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	d.width = d.height = kPrefilterSize;
	d.mips = kPrefilterMips;
	if (!dev.CreateImage(d, env.prefiltered, "environment specular")) return false;
	d.width = d.height = kIrradianceSize;
	d.mips = 1;
	if (!dev.CreateImage(d, env.irradiance, "environment irradiance")) return false;
	env.cubeStorage = dev.CreateView(env.cube, kHdrFormat, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 1, 0, 6);
	for (uint32_t m = 0; m < kPrefilterMips; ++m)
		env.prefilteredMips.push_back(dev.CreateView(env.prefiltered, kHdrFormat, VK_IMAGE_VIEW_TYPE_2D_ARRAY, m, 1, 0, 6));
	env.irradianceStorage = dev.CreateView(env.irradiance, kHdrFormat, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 1, 0, 6);
	return true;
}

// Regenerates the environment of a scene when its sky inputs changed.
// Returns true when the scene has a usable environment.
bool UpdateEnvironment(VkCommandBuffer cmd, SceneGPU& gpu, const hrl_scene_t* scene)
{
	if (!scene->environment_mapping_enabled) return false;
	HRL_id source = scene->environment_texture != HRL_INVALID_ID ? scene->environment_texture : scene->sky_texture;
	auto texIt = R.textures.find(source);
	if (texIt == R.textures.end()) source = HRL_INVALID_ID;

	Environment& env = gpu.env;
	if (env.valid && env.texture == source && env.rotation == scene->sky_rotation &&
		(source != HRL_INVALID_ID || (env.top == scene->sky_top_color && env.horizon == scene->sky_horizon_color && env.bottom == scene->sky_bottom_color)))
		return true;

	Device& dev = Dev();
	if (!env.cube && !CreateEnvironmentImages(env)) { DestroyEnvironment(env); return false; }
	dev.BeginLabel(cmd, "HRL environment");

	SkyPush push{};
	push.rotation = SkyRotation(scene->sky_rotation);
	push.top = glm::vec4(scene->sky_top_color, 1.f);
	push.horizon = glm::vec4(scene->sky_horizon_color, 1.f);
	push.bottom = glm::vec4(scene->sky_bottom_color, 1.f);
	push.texture = source != HRL_INVALID_ID ? 1u : 0u;
	VkImageView equirect = R.white.srgbView;
	VkSampler equirectSampler = GetTextureSampler(HRL_FILTER_TRILINEAR, HRL_FILTER_LINEAR);
	if (source != HRL_INVALID_ID)
	{
		const Texture& t = texIt->second;
		equirect = t.srgbView;
		// Pre-filter: one cube texel covers about width / (4 * size) source texels.
		push.lod = std::max(0.f, std::log2((float)t.image.extent.width / (4.f * (float)kEnvSize)));
	}
	dev.Transition(cmd, env.cube, VK_IMAGE_LAYOUT_GENERAL);
	DispatchImage(cmd, R.envCubePipeline, equirect, equirectSampler, env.cubeStorage, kEnvSize, 6, &push, sizeof(push));
	dev.Transition(cmd, env.cube, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	dev.GenerateMips(cmd, env.cube);

	dev.Transition(cmd, env.prefiltered, VK_IMAGE_LAYOUT_GENERAL);
	for (uint32_t m = 0; m < kPrefilterMips; ++m)
	{
		FilterPush fp{(float)m / (float)(kPrefilterMips - 1), (float)kEnvSize, 0u, 96u};
		DispatchImage(cmd, R.envFilterPipeline, env.cube.view, R.linearClamp, env.prefilteredMips[m], kPrefilterSize >> m, 6, &fp, sizeof(fp));
	}
	dev.Transition(cmd, env.prefiltered, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	dev.Transition(cmd, env.irradiance, VK_IMAGE_LAYOUT_GENERAL);
	FilterPush ip{0.f, (float)kEnvSize, 1u, 256u};
	DispatchImage(cmd, R.envFilterPipeline, env.cube.view, R.linearClamp, env.irradianceStorage, kIrradianceSize, 6, &ip, sizeof(ip));
	dev.Transition(cmd, env.irradiance, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	dev.EndLabel(cmd);

	env.valid = true;
	env.texture = source;
	env.rotation = scene->sky_rotation;
	env.top = scene->sky_top_color;
	env.horizon = scene->sky_horizon_color;
	env.bottom = scene->sky_bottom_color;
	return true;
}

void EnsureBrdfLut(VkCommandBuffer cmd)
{
	if (R.brdfLutReady) return;
	Device& dev = Dev();
	dev.Transition(cmd, R.brdfLut, VK_IMAGE_LAYOUT_GENERAL);
	DispatchImage(cmd, R.brdfLutPipeline, R.white.image.view, R.linearClamp, R.brdfLut.view, kBrdfLutSize, 1, nullptr, 0);
	dev.Transition(cmd, R.brdfLut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	dev.Transition(cmd, R.dummyCube, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkClearColorValue black{};
	VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
	vkCmdClearColorImage(cmd, R.dummyCube.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	dev.Transition(cmd, R.dummyCube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	R.brdfLutReady = true;
}

// ---------------------------------------------------------------------------
// Ray tracing: top level acceleration structure of a scene
// ---------------------------------------------------------------------------
bool BuildTlas(VkCommandBuffer cmd, Tlas& tlas, const std::vector<DrawItem>& items)
{
	Device& dev = Dev();
	std::vector<VkAccelerationStructureInstanceKHR> instances;
	instances.reserve(items.size());
	for (const DrawItem& item : items)
	{
		if (!item.gpu->blas.handle) continue;
		VkAccelerationStructureInstanceKHR inst{};
		const glm::mat4 t = glm::transpose(item.model);
		std::memcpy(&inst.transform, &t, sizeof(inst.transform));
		inst.instanceCustomIndex = item.instance & 0xFFFFFFu;
		inst.mask = 0xFF;
		inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		inst.accelerationStructureReference = item.gpu->blas.address;
		instances.push_back(inst);
	}
	const uint32_t count = (uint32_t)instances.size();

	VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
	geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geo;

	if (!tlas.handle || tlas.capacity < count)
	{
		DestroyTlas(tlas);
		tlas.capacity = std::max(64u, count * 2u);
		VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
		R.buildSizesAS(dev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &tlas.capacity, &sizes);
		bool ok = dev.CreateBuffer(sizes.accelerationStructureSize,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false, tlas.storage, "tlas");
		ok = ok && dev.CreateBuffer(sizes.buildScratchSize + R.scratchAlignment,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false, tlas.scratch, "tlas scratch");
		ok = ok && dev.CreateBuffer(VkDeviceSize(tlas.capacity) * sizeof(VkAccelerationStructureInstanceKHR),
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, tlas.instances, "tlas instances");
		if (ok)
		{
			VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
			ci.buffer = tlas.storage.handle;
			ci.size = sizes.accelerationStructureSize;
			ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
			ok = R.createAS(dev.device, &ci, nullptr, &tlas.handle) == VK_SUCCESS;
		}
		if (!ok) { DestroyTlas(tlas); return false; }
	}
	if (count) std::memcpy(tlas.instances.mapped, instances.data(), count * sizeof(VkAccelerationStructureInstanceKHR));
	geo.geometry.instances.data.deviceAddress = tlas.instances.address;
	info.dstAccelerationStructure = tlas.handle;
	info.scratchData.deviceAddress = AlignUp(tlas.scratch.address, R.scratchAlignment);
	VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
	const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
	R.buildAS(cmd, 1, &info, &ranges);

	VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
	mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.memoryBarrierCount = 1;
	di.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2(cmd, &di);
	return true;
}

// ---------------------------------------------------------------------------
// Frame data
// ---------------------------------------------------------------------------
float FloatParam(const HRL_Material* m, const char* name, float fallback)
{
	auto it = m->floatParams_.find(name);
	return it == m->floatParams_.end() ? fallback : it->second;
}
bool IntParam(const HRL_Material* m, const char* name)
{
	auto it = m->intParams_.find(name);
	return it != m->intParams_.end() && it->second != 0;
}

MaterialData PackMaterial(const HRL_Material* m, Bucket& bucket)
{
	MaterialData d{};
	glm::vec3 tint(1.f);
	if (auto it = m->vec3Params_.find("TintColor"); it != m->vec3Params_.end()) tint = it->second;
	d.baseColor = glm::vec4(glm::max(tint, glm::vec3(0.f)), FloatParam(m, "BaseColorAlpha", 1.f));
	// Emissive = colour * intensity * emissive texture. A texture alone emits at full strength.
	if (auto it = m->vec4Params_.find("EmissiveColor"); it != m->vec4Params_.end())
		d.emissive = glm::vec4(glm::vec3(it->second) * it->second.a, 0.f);
	else if (m->textureParams_.count(HRL_T_EMISSIVE))
		d.emissive = glm::vec4(1.f, 1.f, 1.f, 0.f);
	d.params = glm::vec4(FloatParam(m, "RoughnessValue", 1.f), FloatParam(m, "MetallicValue", 0.f),
		FloatParam(m, "SpecularValue", 1.f), FloatParam(m, "OpacityValue", 1.f));
	const uint32_t white = R.white.slot, black = R.black.slot, normal = R.flatNormal.slot;
	// Fallbacks reproduce the built-in OpenGL defaults: rough dielectric, no AO.
	d.tex0 = glm::uvec4(TextureSlot(m, HRL_T_ALBEDO, white), TextureSlot(m, HRL_T_NORMAL, normal),
		TextureSlot(m, HRL_T_ROUGHNESS, white), TextureSlot(m, HRL_T_METALLIC, black));
	d.tex1 = glm::uvec4(TextureSlot(m, HRL_T_AMBIENT_OCCLUSION, white), TextureSlot(m, HRL_T_ALPHA, white),
		TextureSlot(m, HRL_T_SPECULAR, white), TextureSlot(m, HRL_T_EMISSIVE, white));
	uint32_t flags = 0;
	if (IntParam(m, "RoughnessUseValue")) flags |= MatRoughnessValue;
	if (IntParam(m, "MetallicUseValue")) flags |= MatMetallicValue;
	if (IntParam(m, "SpecularUseValue")) flags |= MatSpecularValue;
	if (IntParam(m, "OpacityUseValue")) flags |= MatOpacityValue;
	if (IntParam(m, "RoughnessInvert")) flags |= MatRoughnessInvert;
	if (IntParam(m, "AlphaInvert")) flags |= MatAlphaInvert;
	d.flags = glm::uvec4(flags, 0, 0, 0);
	const bool masked = (flags & MatOpacityValue) || m->textureParams_.count(HRL_T_ALPHA) != 0;
	bucket = m->two_sided_ ? (masked ? BucketMaskedTwoSided : BucketOpaqueTwoSided) : (masked ? BucketMasked : BucketOpaque);
	return d;
}

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

Camera MakeCamera(const HRL_Camera* c, float width, float height)
{
	Camera cam;
	cam.position = c->position_;
	cam.view = glm::lookAt(c->position_, c->position_ + GetForwardVector(c->rotation_), GetUpVector(c->rotation_));
	cam.nearPlane = std::max(c->near_plane_, 1e-4f);
	cam.farPlane = std::max(c->far_plane_, cam.nearPlane + 1e-3f);
	cam.aspect = width / std::max(height, 1.f);
	cam.perspective = c->type_ == HRL_PERSPECTIVE;
	if (cam.perspective)
	{
		cam.fovY = glm::radians(glm::clamp(c->value_, 1.f, 179.f));
		cam.proj = glm::perspectiveRH_ZO(cam.fovY, cam.aspect, cam.farPlane, cam.nearPlane);
		cam.projGL = glm::perspective(cam.fovY, cam.aspect, cam.nearPlane, cam.farPlane);
	}
	else
	{
		cam.orthoHalfHeight = c->value_ * 0.5f;
		const float hw = cam.orthoHalfHeight * cam.aspect;
		cam.proj = glm::orthoRH_ZO(-hw, hw, -cam.orthoHalfHeight, cam.orthoHalfHeight, cam.farPlane, cam.nearPlane);
		cam.projGL = glm::ortho(-hw, hw, -cam.orthoHalfHeight, cam.orthoHalfHeight, cam.nearPlane, cam.farPlane);
	}
	return cam;
}

struct ShadowView
{
	glm::mat4 viewProj{1.f};   // Vulkan clip space, depth 0..1
	glm::mat4 cullClip{1.f};   // OpenGL clip space, for the CPU frustum test
	uint32_t layer = 0;
};

// Stable cascades: each cascade is the bounding sphere of its frustum slice,
// snapped to shadow map texels so the shadows do not shimmer when the camera moves.
void ComputeCascades(const Camera& cam, const glm::vec3& lightDir, float resolution,
	ViewData& view, std::vector<ShadowView>& out)
{
	const float n = cam.nearPlane;
	const float f = std::min(cam.farPlane, std::max(R.shadowDistance, n + 1.f));
	const glm::mat4 invView = glm::inverse(cam.view);
	const glm::vec3 up = std::abs(lightDir.y) > 0.98f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
	const float extrude = 250.f; // casters between the light and the slice
	float previous = n;
	for (uint32_t i = 0; i < kCascades; ++i)
	{
		const float p = float(i + 1) / float(kCascades);
		const float split = glm::mix(n + (f - n) * p, n * std::pow(f / n, p), 0.8f);
		glm::vec3 corners[8];
		int k = 0;
		for (float d : {previous, split})
		{
			const float hh = cam.perspective ? std::tan(cam.fovY * 0.5f) * d : cam.orthoHalfHeight;
			const float hw = hh * cam.aspect;
			for (float sx : {-1.f, 1.f})
				for (float sy : {-1.f, 1.f})
					corners[k++] = glm::vec3(invView * glm::vec4(sx * hw, sy * hh, -d, 1.f));
		}
		glm::vec3 center(0.f);
		for (const auto& c : corners) center += c;
		center /= 8.f;
		float radius = 0.f;
		for (const auto& c : corners) radius = std::max(radius, glm::length(c - center));
		radius = std::ceil(radius * 16.f) / 16.f;

		const glm::mat4 lightView = glm::lookAt(center - lightDir * (radius + extrude), center, up);
		glm::mat4 proj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.f, 2.f * radius + extrude);
		// Texel snapping.
		glm::vec4 origin = proj * lightView * glm::vec4(0.f, 0.f, 0.f, 1.f);
		origin *= resolution * 0.5f;
		const glm::vec2 offset = (glm::round(glm::vec2(origin)) - glm::vec2(origin)) * (2.f / resolution);
		proj[3][0] += offset.x;
		proj[3][1] += offset.y;

		view.cascadeViewProj[i] = proj * lightView;
		view.cascadeSplits[i] = split;
		ShadowView sv;
		sv.viewProj = view.cascadeViewProj[i];
		sv.cullClip = glm::ortho(-radius, radius, -radius, radius, 0.f, 2.f * radius + extrude) * lightView;
		sv.layer = i;
		out.push_back(sv);
		previous = split;
	}
}

void SetViewport(VkCommandBuffer cmd, float x, float y, float w, float h, bool flipY)
{
	VkViewport vp{x, flipY ? y + h : y, w, flipY ? -h : h, 0.f, 1.f};
	vkCmdSetViewport(cmd, 0, 1, &vp);
	VkRect2D sc{{(int32_t)x, (int32_t)y}, {(uint32_t)w, (uint32_t)h}};
	vkCmdSetScissor(cmd, 0, 1, &sc);
}

// Writes the indirect commands of the items accepted by `accept`, grouped by
// bucket, and returns the slice + per-bucket ranges.
struct IndirectList
{
	FrameSlice slice;
	uint32_t first[BucketCount]{};
	uint32_t count[BucketCount]{};
	uint32_t total = 0;
};

template <typename Accept>
IndirectList BuildIndirect(const std::vector<DrawItem>& items, Accept accept)
{
	IndirectList list;
	std::vector<const DrawItem*> selected[BucketCount];
	for (const DrawItem& item : items)
		if (accept(item)) selected[item.bucket].push_back(&item);
	for (uint32_t b = 0; b < BucketCount; ++b)
	{
		// Front to back: maximises early depth rejection.
		std::sort(selected[b].begin(), selected[b].end(),
			[](const DrawItem* a, const DrawItem* c) { return a->distance2 < c->distance2; });
		list.first[b] = list.total;
		list.count[b] = (uint32_t)selected[b].size();
		list.total += list.count[b];
	}
	if (!list.total) return list;
	list.slice = Dev().AllocFrame(VkDeviceSize(list.total) * sizeof(VkDrawIndexedIndirectCommand), 16);
	if (!list.slice.ptr) { list.total = 0; return list; }
	auto* cmds = static_cast<VkDrawIndexedIndirectCommand*>(list.slice.ptr);
	uint32_t i = 0;
	for (uint32_t b = 0; b < BucketCount; ++b)
		for (const DrawItem* item : selected[b])
		{
			const MeshLOD& lod = item->gpu->lods[item->lod];
			cmds[i++] = {lod.indexCount, 1, lod.firstIndex, (int32_t)lod.vertexOffset, item->instance};
		}
	return list;
}

void DrawIndirect(VkCommandBuffer cmd, const IndirectList& list, uint32_t bucket)
{
	if (!list.count[bucket]) return;
	vkCmdDrawIndexedIndirect(cmd, list.slice.buffer,
		list.slice.offset + VkDeviceSize(list.first[bucket]) * sizeof(VkDrawIndexedIndirectCommand),
		list.count[bucket], sizeof(VkDrawIndexedIndirectCommand));
}

void BindGeometry(VkCommandBuffer cmd)
{
	VkDeviceSize zero = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &R.vertexBuffer.handle, &zero);
	vkCmdBindIndexBuffer(cmd, R.indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);
}

float SceneExposure(const hrl_scene_t* scene)
{
	// The exposure of the default post-process material applies to the tone mapper.
	for (const auto& [vid, viewport] : scene->viewports)
		if (viewport)
			for (const auto& [pid, pp] : viewport->post_processes)
			{
				if (!pp) continue;
				auto m = GetPrivateContext()->materials.find(pp->material_);
				if (m != GetPrivateContext()->materials.end() && m->second)
					return FloatParam(m->second, "exposure", 0.f);
			}
	return 0.f;
}

void WritePendingScreenshots(bool all)
{
	Device& dev = Dev();
	for (size_t i = 0; i < R.screenshots.size();)
	{
		PendingScreenshot& s = R.screenshots[i];
		if (!all && s.frame + kFramesInFlight > dev.FrameNumber()) { ++i; continue; }
		std::vector<unsigned char> pixels(size_t(s.width) * s.height * 4u);
		std::memcpy(pixels.data(), s.buffer.mapped, pixels.size());
		for (size_t p = 3; p < pixels.size(); p += 4) pixels[p] = 255;
		if (!WritePNG_RGBA8(s.path.c_str(), (int)s.width, (int)s.height, pixels))
			SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_TakeScreenshot: cannot write " + s.path);
		dev.DestroyBuffer(s.buffer, false);
		R.screenshots.erase(R.screenshots.begin() + (long)i);
	}
}
} // namespace

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------
bool RendererInit(uint32_t width, uint32_t height)
{
	Device& dev = Dev();
	R = State{};
	R.width = width;
	R.height = height;
	// The LDR scene image has the same transfer function as the swapchain so
	// the final copy is a plain blit.
	const bool unormSwapchain = dev.HasSwapchain() &&
		dev.swapchainFormat != VK_FORMAT_B8G8R8A8_SRGB && dev.swapchainFormat != VK_FORMAT_R8G8B8A8_SRGB;
	R.ldrFormat = unormSwapchain ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;

	if (dev.caps.rayQuery)
	{
		R.createAS = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(dev.device, "vkCreateAccelerationStructureKHR");
		R.destroyAS = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(dev.device, "vkDestroyAccelerationStructureKHR");
		R.buildSizesAS = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(dev.device, "vkGetAccelerationStructureBuildSizesKHR");
		R.buildAS = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(dev.device, "vkCmdBuildAccelerationStructuresKHR");
		R.addressAS = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(dev.device, "vkGetAccelerationStructureDeviceAddressKHR");
		VkPhysicalDeviceAccelerationStructurePropertiesKHR ap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
		VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ap};
		vkGetPhysicalDeviceProperties2(dev.physical, &p2);
		R.scratchAlignment = std::max<VkDeviceSize>(ap.minAccelerationStructureScratchOffsetAlignment, 1);
		if (!R.createAS || !R.destroyAS || !R.buildSizesAS || !R.buildAS || !R.addressAS) return false;
	}

	if (!CreatePipelines()) return false;

	VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
	sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sc.maxLod = VK_LOD_CLAMP_NONE;
	if (vkCreateSampler(dev.device, &sc, nullptr, &R.linearClamp) != VK_SUCCESS) return false;
	sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sc.maxLod = 0.f;
	sc.compareEnable = VK_TRUE;
	sc.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	if (vkCreateSampler(dev.device, &sc, nullptr, &R.shadowSampler) != VK_SUCCESS) return false;

	const unsigned char white[4] = {255, 255, 255, 255}, normal[4] = {128, 128, 255, 255}, black[4] = {0, 0, 0, 255};
	if (!CreateTextureInternal(R.white, white, 1, 1) || !CreateTextureInternal(R.flatNormal, normal, 1, 1) ||
		!CreateTextureInternal(R.black, black, 1, 1))
		return false;

	VmaVirtualBlockCreateInfo vb{};
	vb.size = 1ull << 40; // units: vertices / indices, the buffers grow on demand
	if (vmaCreateVirtualBlock(&vb, &R.vertexBlock) != VK_SUCCESS || vmaCreateVirtualBlock(&vb, &R.indexBlock) != VK_SUCCESS)
		return false;
	if (!EnsurePoolCapacity(R.vertexBuffer, 1, false) || !EnsurePoolCapacity(R.indexBuffer, 1, true)) return false;

	ImageDesc lut;
	lut.format = VK_FORMAT_R16G16_SFLOAT;
	lut.width = lut.height = kBrdfLutSize;
	lut.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	ImageDesc cube;
	cube.format = kHdrFormat;
	cube.cube = true;
	cube.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (!dev.CreateImage(lut, R.brdfLut, "brdf lut") || !dev.CreateImage(cube, R.dummyCube, "dummy cube")) return false;
	if (!EnsureShadowMaps(16, kCascades)) return false;

	R.initialized = true;
	return true;
}

void RendererShutdown()
{
	Device& dev = Dev();
	if (!dev.IsInitialized()) return;
	if (dev.InFrame()) dev.EndFrame();
	dev.WaitIdle();
	WritePendingScreenshots(true);

	for (auto& [id, s] : R.scenes)
	{
		DestroySceneTargets(s);
		DestroyEnvironment(s.env);
		for (Tlas& t : s.tlas) DestroyTlas(t);
	}
	R.scenes.clear();
	for (auto& [id, m] : R.meshes) DestroyBlas(m.blas);
	R.meshes.clear();
	for (auto& [id, t] : R.textures) DestroyTextureInternal(t, false);
	R.textures.clear();
	DestroyTextureInternal(R.white, false);
	DestroyTextureInternal(R.flatNormal, false);
	DestroyTextureInternal(R.black, false);
	dev.DestroyImage(R.brdfLut);
	dev.DestroyImage(R.dummyCube);
	for (VkImageView v : R.shadowLayers) vkDestroyImageView(dev.device, v, nullptr);
	R.shadowLayers.clear();
	dev.DestroyImage(R.shadowMaps);
	dev.DestroyBuffer(R.vertexBuffer);
	dev.DestroyBuffer(R.indexBuffer);
	// The deferred destructors use the renderer state (texture slots, geometry
	// blocks, ray tracing entry points): run them before that state goes away.
	dev.DrainDeletions();
	if (R.vertexBlock) { vmaClearVirtualBlock(R.vertexBlock); vmaDestroyVirtualBlock(R.vertexBlock); }
	if (R.indexBlock) { vmaClearVirtualBlock(R.indexBlock); vmaDestroyVirtualBlock(R.indexBlock); }

	VkDevice d = dev.device;
	for (auto& [key, s] : R.samplers) vkDestroySampler(d, s, nullptr);
	if (R.linearClamp) vkDestroySampler(d, R.linearClamp, nullptr);
	if (R.shadowSampler) vkDestroySampler(d, R.shadowSampler, nullptr);
	for (VkPipeline p : R.meshPipelines) if (p) vkDestroyPipeline(d, p, nullptr);
	for (VkPipeline p : {R.shadowPipeline, R.skyPipeline, R.tonemapPipeline, R.debugLinePipeline,
			R.debugTrianglePipeline, R.envCubePipeline, R.envFilterPipeline, R.brdfLutPipeline})
		if (p) vkDestroyPipeline(d, p, nullptr);
	for (VkPipelineLayout l : {R.sceneLayout, R.computeLayout, R.tonemapLayout, R.debugLayout})
		if (l) vkDestroyPipelineLayout(d, l, nullptr);
	if (R.bindlessPool) vkDestroyDescriptorPool(d, R.bindlessPool, nullptr);
	for (VkDescriptorSetLayout l : {R.viewLayout, R.bindlessLayout, R.imageLayout, R.samplerLayout})
		if (l) vkDestroyDescriptorSetLayout(d, l, nullptr);
	R = State{};
}

void RendererResize(uint32_t width, uint32_t height)
{
	R.width = width;
	R.height = height;
	Dev().Resize(width, height);
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
int CreateMesh(HRL_id id, HRL_uint level, const HRL_Vertex3D* vertices, size_t vertexCount, const HRL_uint* indices, size_t indexCount)
{
	if (!R.initialized || !vertices || vertexCount == 0) return HRL_FALSE;
	Device& dev = Dev();
	MeshGPU& mesh = R.meshes[id];
	if (mesh.lods.size() <= level) mesh.lods.resize(level + 1);
	MeshLOD& lod = mesh.lods[level];
	FreeLOD(lod);

	std::vector<HRL_uint> generated;
	if (!indices || indexCount == 0)
	{
		generated.resize(vertexCount);
		for (size_t i = 0; i < vertexCount; ++i) generated[i] = (HRL_uint)i;
		indices = generated.data();
		indexCount = vertexCount;
	}

	VmaVirtualAllocationCreateInfo ai{};
	ai.size = vertexCount;
	VkDeviceSize vOffset = 0, iOffset = 0;
	if (vmaVirtualAllocate(R.vertexBlock, &ai, &lod.vertexAlloc, &vOffset) != VK_SUCCESS) return HRL_FALSE;
	ai.size = indexCount;
	if (vmaVirtualAllocate(R.indexBlock, &ai, &lod.indexAlloc, &iOffset) != VK_SUCCESS) { FreeLOD(lod); return HRL_FALSE; }

	const VkDeviceSize vBytes = vertexCount * sizeof(HRL_Vertex3D), iBytes = indexCount * sizeof(uint32_t);
	if (!EnsurePoolCapacity(R.vertexBuffer, vOffset * sizeof(HRL_Vertex3D) + vBytes, false) ||
		!EnsurePoolCapacity(R.indexBuffer, iOffset * sizeof(uint32_t) + iBytes, true) ||
		!dev.UploadToBuffer(R.vertexBuffer, vOffset * sizeof(HRL_Vertex3D), vertices, vBytes) ||
		!dev.UploadToBuffer(R.indexBuffer, iOffset * sizeof(uint32_t), indices, iBytes))
	{
		FreeLOD(lod);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "Vulkan: mesh upload failed");
		return HRL_FALSE;
	}
	lod.vertexOffset = (uint32_t)vOffset;
	lod.vertexCount = (uint32_t)vertexCount;
	lod.firstIndex = (uint32_t)iOffset;
	lod.indexCount = (uint32_t)indexCount;
	lod.valid = true;

	if (level == 0 && dev.caps.rayQuery)
	{
		DestroyBlas(mesh.blas);
		BuildBlas(mesh);
	}
	return HRL_TRUE;
}

void DeleteMeshLODs(HRL_id id)
{
	auto it = R.meshes.find(id);
	if (it == R.meshes.end()) return;
	for (size_t i = 1; i < it->second.lods.size(); ++i) FreeLOD(it->second.lods[i]);
	it->second.lods.resize(std::min<size_t>(it->second.lods.size(), 1));
}

void DeleteMesh(HRL_id id)
{
	auto it = R.meshes.find(id);
	if (it == R.meshes.end()) return;
	// The pool ranges may be reused at once: a frame that still draws the old
	// mesh was recorded before the new data is uploaded (uploads go first in
	// the next submission), so defer the release by the in-flight frames.
	std::vector<MeshLOD> lods = it->second.lods;
	Dev().Defer([lods]() mutable { for (MeshLOD& l : lods) FreeLOD(l); });
	DestroyBlas(it->second.blas);
	R.meshes.erase(it);
}

HRL_id CreateTexture(HRL_id id, const unsigned char* rgba, int width, int height)
{
	if (!R.initialized || !rgba || width <= 0 || height <= 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "Vulkan: invalid texture data");
		return HRL_INVALID_ID;
	}
	if (id == HRL_INVALID_ID) id = GenerateHRL_ID();
	auto existing = R.textures.find(id);
	if (existing != R.textures.end()) { DestroyTextureInternal(existing->second, true); R.textures.erase(existing); }
	Texture t;
	if (!CreateTextureInternal(t, rgba, width, height)) return HRL_INVALID_ID;
	R.textures.emplace(id, t);
	return id;
}

void DeleteTexture(HRL_id id)
{
	auto it = R.textures.find(id);
	if (it == R.textures.end()) return;
	DestroyTextureInternal(it->second, true);
	R.textures.erase(it);
}

bool IsValidTexture(HRL_id id) { return R.textures.count(id) != 0; }

void GetTextureSize(HRL_id id, int* width, int* height)
{
	auto it = R.textures.find(id);
	if (width) *width = it == R.textures.end() ? 0 : (int)it->second.image.extent.width;
	if (height) *height = it == R.textures.end() ? 0 : (int)it->second.image.extent.height;
}

void SetTextureFilter(HRL_id id, HRL_EFilterType filter, bool minFilter)
{
	auto it = R.textures.find(id);
	if (it == R.textures.end()) return;
	(minFilter ? it->second.minFilter : it->second.magFilter) = filter;
	WriteBindless(it->second);
}

void CreateScene(HRL_id id, bool onScreen)
{
	SceneGPU& s = R.scenes[id];
	s.onScreen = onScreen;
	CreateSceneTargets(s, R.width, R.height);
}

void DeleteScene(HRL_id id)
{
	auto it = R.scenes.find(id);
	if (it == R.scenes.end()) return;
	DestroySceneTargets(it->second);
	DestroyEnvironment(it->second.env);
	for (Tlas& t : it->second.tlas) DestroyTlas(t);
	R.scenes.erase(it);
}

void ResizeScene(HRL_id id, int width, int height)
{
	auto it = R.scenes.find(id);
	if (it == R.scenes.end() || it->second.onScreen || width <= 0 || height <= 0) return;
	it->second.explicitSize = true;
	CreateSceneTargets(it->second, (uint32_t)width, (uint32_t)height);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void RendererBeginFrame()
{
	Device& dev = Dev();
	if (!R.initialized || dev.InFrame()) return;
	dev.BeginFrame();
	WritePendingScreenshots(false);
}

void RendererRenderScene(hrl_scene_t* scene, HRL_id sceneId)
{
	Device& dev = Dev();
	if (!R.initialized || !scene) return;
	auto sceneIt = R.scenes.find(sceneId);
	if (sceneIt == R.scenes.end()) return;
	SceneGPU& gpu = sceneIt->second;
	if (!dev.InFrame()) RendererBeginFrame();
	if (!dev.InFrame()) return;

	// Scene images follow the window unless the scene was resized explicitly.
	if (!gpu.explicitSize && (gpu.width != std::max(R.width, 1u) || gpu.height != std::max(R.height, 1u)))
		CreateSceneTargets(gpu, R.width, R.height);
	if (!gpu.hdr) return;

	VkCommandBuffer cmd = dev.Cmd();
	dev.BeginLabel(cmd, "HRL scene");
	EnsureBrdfLut(cmd);
	const HRL_Context* ctx = GetPrivateContext();

	// ---- Materials ---------------------------------------------------------
	std::vector<MaterialData> materials;
	std::vector<Bucket> materialBuckets;
	std::unordered_map<HRL_id, uint32_t> materialIndex;
	auto resolveMaterial = [&](HRL_id id) -> int
	{
		auto found = materialIndex.find(id);
		if (found != materialIndex.end()) return (int)found->second;
		auto it = ctx->materials.find(id);
		if (it == ctx->materials.end() || !it->second) return -1;
		Bucket bucket = BucketOpaque;
		materials.push_back(PackMaterial(it->second, bucket));
		materialBuckets.push_back(bucket);
		materialIndex.emplace(id, (uint32_t)materials.size() - 1);
		return (int)materials.size() - 1;
	};

	// ---- Instances: every static 3D mesh of the scene ----------------------
	std::vector<DrawItem> items;
	std::vector<InstanceData> instances;
	items.reserve(scene->meshes.size());
	instances.reserve(scene->meshes.size());
	for (const auto& [id, mesh] : scene->meshes)
	{
		if (!mesh || mesh->type_ != HRL_3D_MESH || !IsFiniteBounds(mesh)) continue;
		auto gpuMesh = R.meshes.find(id);
		if (gpuMesh == R.meshes.end() || gpuMesh->second.lods.empty() || !gpuMesh->second.lods[0].valid) continue;
		const int material = resolveMaterial(mesh->material_);
		if (material < 0) continue;
		DrawItem item;
		item.mesh = mesh;
		item.gpu = &gpuMesh->second;
		item.model = CalculateModelMatrix(mesh);
		item.instance = (uint32_t)instances.size();
		item.bucket = materialBuckets[(size_t)material];
		InstanceData inst;
		inst.model = item.model;
		const glm::mat3 m3(item.model);
		const float det = glm::determinant(m3);
		inst.normalMatrix = (std::isfinite(det) && std::abs(det) > 1e-12f)
			? glm::mat4(glm::transpose(glm::inverse(m3))) : glm::mat4(1.f);
		inst.ids = glm::uvec4((uint32_t)material, id, 0, 0);
		instances.push_back(inst);
		items.push_back(item);
	}

	// ---- Lights ------------------------------------------------------------
	std::vector<LightData> lights;
	const HRL_Light* cascadeLight = nullptr;
	std::vector<const HRL_Light*> spotShadowLights;
	const bool rayTracedShadows = R.rtShadows && dev.caps.rayQuery;
	uint32_t shadowResolution = 0;
	for (const auto& [id, light] : scene->lights)
	{
		if (!light || light->intensity_ <= 0.f) continue;
		LightData d{};
		int type = 0;
		switch (light->type_)
		{
		case HRL_DIRECTIONAL_LIGHT: type = 1; break;
		case HRL_SPOT_LIGHT: type = 2; break;
		case HRL_SKY_LIGHT: type = 3; break;
		default: type = 0; break;
		}
		const glm::vec3 dir = GetLightDirection(light);
		d.position = glm::vec4(light->position_, (float)type);
		d.direction = glm::vec4(dir, light->attenuation_);
		float slot = -1.f;
		if (light->cast_shadows_ && type != 3)
		{
			if (rayTracedShadows) slot = 9.f;
			else if (type == 1 && !cascadeLight) { cascadeLight = light; slot = 0.f; }
			else if (type == 2 && spotShadowLights.size() < kMaxSpotShadows)
			{
				spotShadowLights.push_back(light);
				slot = (float)spotShadowLights.size();
			}
			if (slot >= 0.f && slot < 8.f)
				shadowResolution = std::max(shadowResolution, (uint32_t)glm::clamp(light->shadow_resolution_, 256, 8192));
		}
		d.color = glm::vec4(light->color_ * light->intensity_, slot);
		d.spot = glm::vec4(std::cos(glm::radians(light->innerCutoff)), std::cos(glm::radians(light->outerCutoff)),
			light->shadow_strength_, light->shadow_bias_ * 0.25f);
		lights.push_back(d);
	}
	const bool rasterShadows = shadowResolution > 0;
	if (rasterShadows)
		EnsureShadowMaps(shadowResolution, kCascades + (uint32_t)spotShadowLights.size());

	// ---- Environment / acceleration structure ------------------------------
	const bool hasEnvironment = UpdateEnvironment(cmd, gpu, scene);
	Tlas& tlas = gpu.tlas[dev.FrameNumber() % kFramesInFlight];
	// The shaders always declare the acceleration structure: keep a valid one
	// bound, but only rebuild it when shadow rays are actually traced.
	if (dev.caps.rayQuery && (rayTracedShadows || !tlas.handle) && !BuildTlas(cmd, tlas, items)) { dev.EndLabel(cmd); return; }

	// ---- Frame buffers shared by every viewport ----------------------------
	const FrameSlice instanceSlice = dev.AllocFrame(std::max<size_t>(instances.size(), 1) * sizeof(InstanceData), 256);
	const FrameSlice materialSlice = dev.AllocFrame(std::max<size_t>(materials.size(), 1) * sizeof(MaterialData), 256);
	const FrameSlice lightSlice = dev.AllocFrame(std::max<size_t>(lights.size(), 1) * sizeof(LightData), 256);
	if (!instanceSlice.ptr || !materialSlice.ptr || !lightSlice.ptr) { dev.EndLabel(cmd); return; }
	if (!instances.empty()) std::memcpy(instanceSlice.ptr, instances.data(), instances.size() * sizeof(InstanceData));
	if (!materials.empty()) std::memcpy(materialSlice.ptr, materials.data(), materials.size() * sizeof(MaterialData));
	if (!lights.empty()) std::memcpy(lightSlice.ptr, lights.data(), lights.size() * sizeof(LightData));

	// Viewports in creation order (ascending id), like the OpenGL backend.
	std::vector<std::pair<HRL_id, HRL_Viewport*>> viewports(scene->viewports.begin(), scene->viewports.end());
	std::sort(viewports.begin(), viewports.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

	bool cleared = false;
	for (const auto& [viewportId, viewport] : viewports)
	{
		if (!viewport || !viewport->camera_) continue;
		const float vw = std::round(viewport->width_ * (float)gpu.width);
		const float vh = std::round(viewport->height_ * (float)gpu.height);
		const float vx = std::round(viewport->x_ * (float)gpu.width);
		// HRL viewport rectangles have their origin at the bottom left.
		const float vy = (float)gpu.height - std::round(viewport->y_ * (float)gpu.height) - vh;
		if (vw < 1.f || vh < 1.f || vx < 0.f || vy < 0.f || vx + vw > (float)gpu.width || vy + vh > (float)gpu.height) continue;

		const Camera cam = MakeCamera(viewport->camera_, vw, vh);
		ViewData view{};
		view.view = cam.view;
		view.viewProj = cam.proj * cam.view;
		view.invViewProj = glm::inverse(view.viewProj);
		view.camPos = glm::vec4(cam.position, HRL_InternalRenderTimeSeconds());
		view.viewportSize = glm::vec4(vw, vh, 1.f / vw, 1.f / vh);
		view.shadowInfo = glm::vec4(1.f / (float)std::max(R.shadowResolution, 1u), 0.1f, 0.f, 0.f);
		view.fogColor = glm::vec4(scene->fog.r, scene->fog.g, scene->fog.b, 1.f);
		view.fogParams = glm::vec4(scene->fog.range_start, scene->fog.range_end, scene->fog.density, 0.f);
		view.envParams = glm::vec4((float)(kPrefilterMips - 1), 1.f, 0.f, 0.f);
		uint32_t flags = 0;
		if (hasEnvironment) flags |= ViewFlagIBL;
		if (rayTracedShadows) flags |= ViewFlagRTShadows;
		if (scene->fog.enabled) flags |= ViewFlagFog | (scene->fog.mode != HRL_FOG_LINEAR ? ViewFlagFogExp : 0u);
		uint32_t debugView = 0;
		if (scene->debug_view == HRL_DEBUG_VIEW_UNLIT) debugView = 1;
		else if (scene->debug_view == HRL_DEBUG_VIEW_NORMAL) debugView = 2;
		view.counts = glm::uvec4((uint32_t)lights.size(), flags, debugView, 0);

		std::vector<ShadowView> shadowViews;
		if (cascadeLight)
			ComputeCascades(cam, GetLightDirection(cascadeLight), (float)R.shadowResolution, view, shadowViews);
		else
			for (uint32_t i = 0; i < kCascades; ++i) view.cascadeSplits[i] = 0.f;
		for (size_t i = 0; i < spotShadowLights.size(); ++i)
		{
			const HRL_Light* l = spotShadowLights[i];
			const glm::vec3 dir = GetLightDirection(l);
			const glm::vec3 up = std::abs(dir.y) > 0.98f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
			const glm::mat4 lv = glm::lookAt(l->position_, l->position_ + dir, up);
			const float fov = glm::radians(glm::clamp(l->outerCutoff, 1.f, 89.f) * 2.f);
			ShadowView sv;
			sv.viewProj = glm::perspectiveRH_ZO(fov, 1.f, 0.1f, 100.f) * lv;
			sv.cullClip = glm::perspective(fov, 1.f, 0.1f, 100.f) * lv;
			sv.layer = kCascades + (uint32_t)i;
			view.spotViewProj[i] = sv.viewProj;
			shadowViews.push_back(sv);
		}

		// LOD + distance for this camera.
		const glm::mat4 cullClip = cam.projGL * cam.view;
		const FrustumPlaneSet frustum = BuildFrustum(cullClip);
		for (DrawItem& item : items)
		{
			const glm::vec3 center = glm::vec3(item.model * glm::vec4(item.mesh->bounds_center_, 1.f));
			const glm::vec3 delta = center - cam.position;
			item.distance2 = glm::dot(delta, delta);
			int lod = SelectMeshLOD(item.mesh, item.model, cam.view, cam.projGL, vh, viewport);
			lod = glm::clamp(lod, 0, (int)item.gpu->lods.size() - 1);
			while (lod > 0 && !item.gpu->lods[(size_t)lod].valid) --lod;
			item.lod = (uint32_t)lod;
		}

		// ---- View descriptor set -------------------------------------------
		const FrameSlice viewSlice = dev.AllocFrame(sizeof(ViewData), 256);
		VkDescriptorSet viewSet = AllocFrameSet(R.viewLayout);
		if (!viewSlice.ptr || !viewSet) continue;
		std::memcpy(viewSlice.ptr, &view, sizeof(view));
		VkDescriptorBufferInfo buffers[4] = {
			{viewSlice.buffer, viewSlice.offset, sizeof(ViewData)},
			{instanceSlice.buffer, instanceSlice.offset, instanceSlice.size},
			{materialSlice.buffer, materialSlice.offset, materialSlice.size},
			{lightSlice.buffer, lightSlice.offset, lightSlice.size}};
		const Environment& env = gpu.env;
		VkDescriptorImageInfo images[4] = {
			{R.shadowSampler, R.shadowMaps.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{R.linearClamp, hasEnvironment ? env.irradiance.view : R.dummyCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{R.linearClamp, hasEnvironment ? env.prefiltered.view : R.dummyCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{R.linearClamp, R.brdfLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
		VkWriteDescriptorSet writes[9]{};
		for (uint32_t i = 0; i < 8; ++i)
		{
			writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[i].dstSet = viewSet;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			if (i < 4)
			{
				writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				writes[i].pBufferInfo = &buffers[i];
			}
			else
			{
				writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				writes[i].pImageInfo = &images[i - 4];
			}
		}
		VkWriteDescriptorSetAccelerationStructureKHR asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
		uint32_t writeCount = 8;
		if (dev.caps.rayQuery)
		{
			asWrite.accelerationStructureCount = 1;
			asWrite.pAccelerationStructures = &tlas.handle;
			writes[8] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite};
			writes[8].dstSet = viewSet;
			writes[8].dstBinding = 8;
			writes[8].descriptorCount = 1;
			writes[8].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
			writeCount = 9;
		}
		vkUpdateDescriptorSets(dev.device, writeCount, writes, 0, nullptr);
		const VkDescriptorSet sets[2] = {viewSet, R.bindlessSet};
		const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

		// ---- Shadow passes -------------------------------------------------
		if (!shadowViews.empty())
		{
			dev.BeginLabel(cmd, "HRL shadows");
			dev.Transition(cmd, R.shadowMaps, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
			for (const ShadowView& sv : shadowViews)
			{
				const FrustumPlaneSet lightFrustum = BuildFrustum(sv.cullClip);
				const IndirectList list = BuildIndirect(items, [&](const DrawItem& item)
					{ return IsMeshVisible(item.mesh, item.model, lightFrustum); });
				VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
				depth.imageView = R.shadowLayers[sv.layer];
				depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				depth.clearValue.depthStencil = {1.f, 0};
				VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
				ri.renderArea = {{0, 0}, {R.shadowResolution, R.shadowResolution}};
				ri.layerCount = 1;
				ri.pDepthAttachment = &depth;
				vkCmdBeginRendering(cmd, &ri);
				if (list.total)
				{
					SetViewport(cmd, 0, 0, (float)R.shadowResolution, (float)R.shadowResolution, false);
					vkCmdSetDepthBias(cmd, 1.5f, 0.f, 2.f);
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.shadowPipeline);
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.sceneLayout, 0, 2, sets, 0, nullptr);
					vkCmdPushConstants(cmd, R.sceneLayout, VF, 0, sizeof(glm::mat4), &sv.viewProj);
					BindGeometry(cmd);
					for (uint32_t b = 0; b < BucketCount; ++b) DrawIndirect(cmd, list, b);
				}
				vkCmdEndRendering(cmd);
			}
			dev.EndLabel(cmd);
		}
		dev.Transition(cmd, R.shadowMaps, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// ---- Forward pass --------------------------------------------------
		const IndirectList visible = BuildIndirect(items, [&](const DrawItem& item)
			{ return IsMeshVisible(item.mesh, item.model, frustum); });
		dev.Transition(cmd, gpu.hdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		dev.Transition(cmd, gpu.ids, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		dev.Transition(cmd, gpu.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		VkRenderingAttachmentInfo colors[2]{};
		colors[0] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		colors[0].imageView = gpu.hdr.view;
		colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colors[0].loadOp = cleared ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
		colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colors[1] = colors[0];
		colors[1].imageView = gpu.ids.view;
		colors[1].clearValue.color.uint32[0] = kNoObject;
		VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		depth.imageView = gpu.depth.view;
		depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth.loadOp = cleared ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
		depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depth.clearValue.depthStencil = {0.f, 0}; // reversed-Z: 0 is the far plane
		VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea = {{0, 0}, {gpu.width, gpu.height}};
		ri.layerCount = 1;
		ri.colorAttachmentCount = 2;
		ri.pColorAttachments = colors;
		ri.pDepthAttachment = &depth;
		dev.BeginLabel(cmd, "HRL forward");
		vkCmdBeginRendering(cmd, &ri);
		SetViewport(cmd, vx, vy, vw, vh, true);
		if (cleared)
		{
			// A later viewport may overlap an earlier one: reset its depth and ids.
			VkClearAttachment clears[2]{};
			clears[0].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			clears[0].clearValue.depthStencil = {0.f, 0};
			clears[1].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			clears[1].colorAttachment = 1;
			clears[1].clearValue.color.uint32[0] = kNoObject;
			VkClearRect rect{{{(int32_t)vx, (int32_t)vy}, {(uint32_t)vw, (uint32_t)vh}}, 0, 1};
			vkCmdClearAttachments(cmd, 2, clears, 1, &rect);
		}
		cleared = true;
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.sceneLayout, 0, 2, sets, 0, nullptr);
		if (visible.total)
		{
			BindGeometry(cmd);
			for (uint32_t b = 0; b < BucketCount; ++b)
			{
				if (!visible.count[b]) continue;
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.meshPipelines[b]);
				DrawIndirect(cmd, visible, b);
			}
		}
		if (scene->sky_sphere_enabled)
		{
			SkyPush push{};
			push.rotation = SkyRotation(scene->sky_rotation);
			push.top = glm::vec4(scene->sky_top_color, 1.f);
			push.horizon = glm::vec4(scene->sky_horizon_color, 1.f);
			push.bottom = glm::vec4(scene->sky_bottom_color, 1.f);
			auto sky = R.textures.find(scene->sky_texture);
			push.texture = sky != R.textures.end() ? sky->second.slot : 0xFFFFFFFFu;
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.skyPipeline);
			vkCmdPushConstants(cmd, R.sceneLayout, VF, 0, sizeof(push), &push);
			vkCmdDraw(cmd, 3, 1, 0, 0);
		}
		vkCmdEndRendering(cmd);
		dev.EndLabel(cmd);

		gpu.lastViewProj = view.viewProj;
		gpu.lastRect[0] = vx; gpu.lastRect[1] = vy; gpu.lastRect[2] = vw; gpu.lastRect[3] = vh;
		R.lastView = cam.view;
		R.lastProjGL = cam.projGL;
	}

	// ---- Tone mapping to the LDR scene image -------------------------------
	if (!cleared)
	{
		// No camera: the scene is black.
		dev.Transition(cmd, gpu.hdr, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkClearColorValue black{};
		VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdClearColorImage(cmd, gpu.hdr.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	}
	dev.Transition(cmd, gpu.hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	dev.Transition(cmd, gpu.ldr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkDescriptorSet toneSet = AllocFrameSet(R.samplerLayout);
	if (toneSet)
	{
		VkDescriptorImageInfo hdr{R.linearClamp, gpu.hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = toneSet;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &hdr;
		vkUpdateDescriptorSets(dev.device, 1, &w, 0, nullptr);
		VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		color.imageView = gpu.ldr.view;
		color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea = {{0, 0}, {gpu.width, gpu.height}};
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &color;
		dev.BeginLabel(cmd, "HRL tone mapping");
		vkCmdBeginRendering(cmd, &ri);
		SetViewport(cmd, 0, 0, (float)gpu.width, (float)gpu.height, false);
		struct { float exposure; uint32_t encode; } push{SceneExposure(scene), R.ldrFormat == VK_FORMAT_R8G8B8A8_UNORM ? 1u : 0u};
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.tonemapPipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.tonemapLayout, 0, 1, &toneSet, 0, nullptr);
		vkCmdPushConstants(cmd, R.tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRendering(cmd);
		dev.EndLabel(cmd);
	}
	gpu.renderedFrame = dev.FrameNumber();
	R.lastScene = sceneId;
	dev.EndLabel(cmd);
}

void RendererDrawDebug(const DebugRenderer& debug, float)
{
	Device& dev = Dev();
	auto it = R.scenes.find(R.lastScene);
	if (!R.initialized || !dev.InFrame() || it == R.scenes.end()) return;
	SceneGPU& gpu = it->second;
	if (gpu.renderedFrame != dev.FrameNumber() || (debug.lines.empty() && debug.triangles.empty())) return;

	VkCommandBuffer cmd = dev.Cmd();
	dev.Transition(cmd, gpu.ldr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	dev.Transition(cmd, gpu.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	color.imageView = gpu.ldr.view;
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	depth.imageView = gpu.depth.view;
	depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, {gpu.width, gpu.height}};
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &color;
	ri.pDepthAttachment = &depth;
	vkCmdBeginRendering(cmd, &ri);
	SetViewport(cmd, gpu.lastRect[0], gpu.lastRect[1], gpu.lastRect[2], gpu.lastRect[3], true);
	auto draw = [&](const std::vector<DebugVertex>& vertices, VkPipeline pipeline)
	{
		if (vertices.empty()) return;
		const FrameSlice slice = dev.AllocFrame(vertices.size() * sizeof(DebugVertex), 16);
		if (!slice.ptr) return;
		std::memcpy(slice.ptr, vertices.data(), vertices.size() * sizeof(DebugVertex));
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdPushConstants(cmd, R.debugLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &gpu.lastViewProj);
		vkCmdBindVertexBuffers(cmd, 0, 1, &slice.buffer, &slice.offset);
		vkCmdDraw(cmd, (uint32_t)vertices.size(), 1, 0, 0);
	};
	draw(debug.triangles, R.debugTrianglePipeline);
	draw(debug.lines, R.debugLinePipeline);
	vkCmdEndRendering(cmd);
}

void TakeScreenshot(HRL_id scene, const char* path)
{
	Device& dev = Dev();
	auto it = R.scenes.find(scene);
	if (!R.initialized || !path || it == R.scenes.end() || !dev.InFrame()) return;
	SceneGPU& gpu = it->second;
	if (gpu.renderedFrame != dev.FrameNumber()) return;
	PendingScreenshot shot;
	shot.path = path;
	shot.width = gpu.width;
	shot.height = gpu.height;
	shot.frame = dev.FrameNumber();
	if (!dev.CreateBuffer(VkDeviceSize(gpu.width) * gpu.height * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, shot.buffer)) return;
	// Host readable buffers are created for sequential writes; reading back is
	// a one-off here.
	VkCommandBuffer cmd = dev.Cmd();
	dev.Transition(cmd, gpu.ldr, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {gpu.width, gpu.height, 1};
	vkCmdCopyImageToBuffer(cmd, gpu.ldr.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buffer.handle, 1, &region);
	R.screenshots.push_back(shot);
}

void RendererEndFrame()
{
	Device& dev = Dev();
	if (!R.initialized || !dev.InFrame()) return;
	VkCommandBuffer cmd = dev.Cmd();
	if (Image* back = dev.Backbuffer())
	{
		for (auto& [id, gpu] : R.scenes)
		{
			if (!gpu.onScreen || gpu.renderedFrame != dev.FrameNumber()) continue;
			dev.Transition(cmd, gpu.ldr, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			dev.Transition(cmd, *back, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkImageBlit blit{};
			blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			blit.srcOffsets[1] = {(int32_t)gpu.width, (int32_t)gpu.height, 1};
			blit.dstSubresource = blit.srcSubresource;
			blit.dstOffsets[1] = {(int32_t)back->extent.width, (int32_t)back->extent.height, 1};
			const bool sameSize = gpu.width == back->extent.width && gpu.height == back->extent.height;
			vkCmdBlitImage(cmd, gpu.ldr.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, back->handle,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, sameSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
		}
	}
	dev.EndFrame();
}

HRL_id GetHoveredObject(HRL_id scene, int mouseX, int mouseY, HRL_EMeshType* type)
{
	Device& dev = Dev();
	auto it = R.scenes.find(scene);
	if (!R.initialized || it == R.scenes.end() || !it->second.ids) return HRL_INVALID_ID;
	SceneGPU& gpu = it->second;
	if (gpu.renderedFrame == ~0ull || mouseX < 0 || mouseY < 0 || mouseX >= (int)gpu.width || mouseY >= (int)gpu.height)
		return HRL_INVALID_ID;
	// Inside a frame the id image belongs to the frame being recorded.
	if (dev.InFrame() && gpu.renderedFrame == dev.FrameNumber()) return HRL_INVALID_ID;

	Buffer readback;
	if (!dev.CreateBuffer(4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback)) return HRL_INVALID_ID;
	VkCommandBuffer cmd = dev.UploadCmd();
	dev.Transition(cmd, gpu.ids, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	// HRL mouse coordinates use a bottom-left origin for picking.
	region.imageOffset = {mouseX, (int32_t)gpu.height - 1 - mouseY, 0};
	region.imageExtent = {1, 1, 1};
	vkCmdCopyImageToBuffer(cmd, gpu.ids.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &region);
	dev.FlushUploads(true);
	uint32_t id = kNoObject;
	std::memcpy(&id, readback.mapped, sizeof(id));
	dev.DestroyBuffer(readback, false);
	if (id == kNoObject) return HRL_INVALID_ID;
	if (type) *type = HRL_3D_MESH;
	return id;
}

void GetViewMatrix(float* out) { if (out) std::memcpy(out, &R.lastView, sizeof(glm::mat4)); }
void GetProjectionMatrix(float* out) { if (out) std::memcpy(out, &R.lastProjGL, sizeof(glm::mat4)); }

bool RayTracingSupported() { return Dev().IsInitialized() && Dev().caps.rayQuery; }
void SetRayTracedShadows(bool enabled) { R.rtShadows = enabled; }
bool RayTracedShadowsEnabled() { return R.rtShadows && RayTracingSupported(); }
void SetShadowDistance(float distance) { if (distance > 0.f) R.shadowDistance = distance; }
} // namespace hrlvk
