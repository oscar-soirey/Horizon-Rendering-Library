// HRL native Vulkan backend - descriptor layouts and pipelines.
#include "vk_renderer_internal.h"

#include <cstdio>
#include <cstdlib>

namespace hrlvk
{
// ---------------------------------------------------------------------------
// Pipeline cache on disk: compiled pipelines are reused by the next run, which
// removes most of the start-up (and first-use) pipeline compilation time.
// ---------------------------------------------------------------------------
namespace
{
std::string g_pipelineCachePath;

std::string PipelineCachePath()
{
	if (!g_pipelineCachePath.empty()) return g_pipelineCachePath;
	const char* env = std::getenv("HRL_VULKAN_PIPELINE_CACHE");
	return env ? env : "";
}
} // namespace

void SetPipelineCachePath(const char* path) { g_pipelineCachePath = path ? path : ""; }

bool CreatePipelineCache()
{
	Device& dev = Dev();
	std::vector<unsigned char> data;
	const std::string path = PipelineCachePath();
	if (!path.empty())
		if (FILE* f = std::fopen(path.c_str(), "rb"))
		{
			std::fseek(f, 0, SEEK_END);
			const long size = std::ftell(f);
			std::fseek(f, 0, SEEK_SET);
			if (size > 0 && size < (256l << 20))
			{
				data.resize((size_t)size);
				if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
			}
			std::fclose(f);
		}
	// Only data written by this GPU and this driver version is usable.
	if (!data.empty())
	{
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(dev.physical, &props);
		uint32_t header[4] = {};
		bool valid = data.size() >= 32;
		if (valid)
		{
			std::memcpy(header, data.data(), sizeof(header));
			valid = header[0] == 32 && header[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
				header[2] == props.vendorID && header[3] == props.deviceID &&
				std::memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
		}
		if (!valid) data.clear();
	}
	VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	ci.initialDataSize = data.size();
	ci.pInitialData = data.empty() ? nullptr : data.data();
	VkResult r = vkCreatePipelineCache(dev.device, &ci, nullptr, &R.pipelineCache);
	if (r != VK_SUCCESS && !data.empty())
	{
		// Rejected data: start from an empty cache.
		ci.initialDataSize = 0;
		ci.pInitialData = nullptr;
		r = vkCreatePipelineCache(dev.device, &ci, nullptr, &R.pipelineCache);
	}
	if (r != VK_SUCCESS) { ReportError("vkCreatePipelineCache", r); return false; }
	return true;
}

void SavePipelineCache()
{
	const std::string path = PipelineCachePath();
	if (path.empty() || !R.pipelineCache) return;
	size_t size = 0;
	if (vkGetPipelineCacheData(Dev().device, R.pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0) return;
	std::vector<unsigned char> data(size);
	if (vkGetPipelineCacheData(Dev().device, R.pipelineCache, &size, data.data()) != VK_SUCCESS) return;
	// Written next to the target then renamed, so a crash never leaves a truncated cache.
	const std::string temporary = path + ".tmp";
	FILE* f = std::fopen(temporary.c_str(), "wb");
	if (!f) { ReportWarning("cannot write the pipeline cache " + path); return; }
	const bool ok = std::fwrite(data.data(), 1, size, f) == size;
	std::fclose(f);
	if (ok)
	{
		std::remove(path.c_str());
		if (std::rename(temporary.c_str(), path.c_str()) != 0) std::remove(temporary.c_str());
	}
	else std::remove(temporary.c_str());
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------
namespace
{
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

enum class VertexInput { None, Mesh, PositionOnly, PositionUV, Debug, Landscape };

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
	bool blend = false; // alpha blending of the first colour attachment
	bool additive = false; // with blend: destination + source
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

	std::vector<VkVertexInputBindingDescription> bindings = {{0, sizeof(HRL_Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX}};
	VkVertexInputBindingDescription& binding = bindings[0];
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
	else if (d.input == VertexInput::PositionUV)
		attrs = {
			{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(HRL_Vertex3D, position)},
			{2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HRL_Vertex3D, uv)}};
	else if (d.input == VertexInput::Landscape)
	{
		// Patch grid position per vertex, quadtree node per instance.
		binding.stride = sizeof(glm::vec2);
		bindings.push_back({1, sizeof(glm::vec4), VK_VERTEX_INPUT_RATE_INSTANCE});
		attrs = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};
	}
	else if (d.input == VertexInput::Debug)
	{
		binding.stride = sizeof(DebugVertex);
		attrs = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}};
	}
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	if (d.input != VertexInput::None)
	{
		vi.vertexBindingDescriptionCount = (uint32_t)bindings.size();
		vi.pVertexBindingDescriptions = bindings.data();
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
	if (d.blend && !blends.empty())
	{
		blends[0].blendEnable = VK_TRUE;
		blends[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blends[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blends[0].colorBlendOp = VK_BLEND_OP_ADD;
		blends[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blends[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blends[0].alphaBlendOp = VK_BLEND_OP_ADD;
		if (d.additive)
		{
			blends[0].srcColorBlendFactor = blends[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
			blends[0].srcAlphaBlendFactor = blends[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		}
	}
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
	VkResult r = vkCreateGraphicsPipelines(Dev().device, R.pipelineCache, 1, &ci, nullptr, &p);
	if (r != VK_SUCCESS) ReportError("vkCreateGraphicsPipelines", r);
	else R.ownedPipelines.push_back(p);
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
	VkResult r = vkCreateComputePipelines(Dev().device, R.pipelineCache, 1, &ci, nullptr, &p);
	if (r != VK_SUCCESS) ReportError("vkCreateComputePipelines", r);
	else R.ownedPipelines.push_back(p);
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
	else R.ownedSetLayouts.push_back(l);
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
	else R.ownedPipelineLayouts.push_back(l);
	return l;
}

} // namespace

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
	{
		view.push_back({8, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
		// Vertices, indices and table of the alpha tested shadow casters.
		for (uint32_t b = 9; b <= 11; ++b) view.push_back({b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
	}
	R.viewLayout = MakeSetLayout(view);

	const VkDescriptorBindingFlags bindlessFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
	const uint32_t bindlessCount = dev.caps.maxBindlessTextures * 2u;
	// Vertex shaders read landscape heights, compute shaders the foliage / grass inputs.
	R.bindlessLayout = MakeSetLayout({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, bindlessCount,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
		&bindlessFlags, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);
	R.imageLayout = MakeSetLayout({
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}});
	R.samplerLayout = MakeSetLayout({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
	R.tonemapSetLayout = MakeSetLayout({
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
	if (!R.viewLayout || !R.bindlessLayout || !R.imageLayout || !R.samplerLayout || !R.tonemapSetLayout) return false;

	R.sceneLayout = MakePipelineLayout({R.viewLayout, R.bindlessLayout}, VF, sizeof(SkyPush));
	R.computeLayout = MakePipelineLayout({R.imageLayout}, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(SkyPush));
	R.tonemapLayout = MakePipelineLayout({R.tonemapSetLayout}, VK_SHADER_STAGE_FRAGMENT_BIT, 12);
	R.bloomLayout = MakePipelineLayout({R.samplerLayout}, VK_SHADER_STAGE_FRAGMENT_BIT, 12);
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
		d.fs = IsMasked(b) ? maskedFs : opaqueFs;
		d.layout = R.sceneLayout;
		d.input = VertexInput::Mesh;
		d.colorFormats = {kHdrFormat, kIdFormat};
		d.depthFormat = kDepthFormat;
		d.depthTest = true;
		d.depthWrite = !IsTranslucent(b);
		d.blend = IsTranslucent(b);
		d.depthCompare = VK_COMPARE_OP_GREATER; // reversed-Z
		d.cull = IsTwoSided(b) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
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
		d.vs = &hrl_vk_shadow_at_vert;
		d.fs = &hrl_vk_shadow_frag;
		d.input = VertexInput::PositionUV;
		R.shadowMaskedPipeline = MakeGraphicsPipeline(d);
	}
	{
		// Foliage (vk_foliage.cpp, vk_impostor.cpp).
		R.foliageSetLayout = MakeSetLayout({
			{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VF, nullptr},
			{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
			{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}});
		const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
		R.foliageCullSetLayout = MakeSetLayout({
			{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, C, nullptr},
			{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, nullptr},
			{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, nullptr},
			{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, nullptr},
			{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, nullptr},
			{5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, nullptr},
			{6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, C, nullptr}});
		if (!R.foliageSetLayout || !R.foliageCullSetLayout) return false;
		// Same push range as sceneLayout: sets 0 and 1 stay bound across the layouts.
		R.foliageLayout = MakePipelineLayout({R.viewLayout, R.bindlessLayout, R.foliageSetLayout}, VF, sizeof(SkyPush));
		R.foliageCullLayout = MakePipelineLayout({R.foliageCullSetLayout}, C, 0);
		R.foliageCullPipeline = MakeComputePipeline(hrl_vk_foliage_cull_comp, R.foliageCullLayout);
		R.foliageCommandsPipeline = MakeComputePipeline(hrl_vk_foliage_commands_comp, R.foliageCullLayout);
		for (uint32_t i = 0; i < 4; ++i)
		{
			const bool masked = i >= 2, twoSided = (i & 1) != 0;
			PipelineDesc d;
			d.vs = &hrl_vk_foliage_vert;
			d.fs = masked ? (rq ? &hrl_vk_mesh_fol_at_rq_frag : &hrl_vk_mesh_fol_at_frag) : (rq ? &hrl_vk_mesh_fol_rq_frag : &hrl_vk_mesh_fol_frag);
			d.layout = R.foliageLayout;
			d.input = VertexInput::Mesh;
			d.colorFormats = {kHdrFormat, kIdFormat};
			d.depthFormat = kDepthFormat;
			d.depthTest = d.depthWrite = true;
			d.depthCompare = VK_COMPARE_OP_GREATER;
			d.cull = twoSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
			R.foliagePipelines[i] = MakeGraphicsPipeline(d);
			if (!R.foliagePipelines[i]) return false;
		}
		PipelineDesc sd;
		sd.vs = &hrl_vk_foliage_shadow_vert;
		sd.layout = R.foliageLayout;
		sd.input = VertexInput::PositionOnly;
		sd.depthFormat = kDepthFormat;
		sd.depthTest = sd.depthWrite = true;
		sd.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
		sd.depthBias = true;
		R.foliageShadowPipeline = MakeGraphicsPipeline(sd);
		sd.vs = &hrl_vk_foliage_shadow_at_vert;
		sd.fs = &hrl_vk_shadow_frag;
		sd.input = VertexInput::PositionUV;
		R.foliageShadowMaskedPipeline = MakeGraphicsPipeline(sd);
		// Impostors: quads generated from gl_VertexIndex.
		PipelineDesc id;
		id.vs = &hrl_vk_impostor_vert;
		id.fs = rq ? &hrl_vk_impostor_rq_frag : &hrl_vk_impostor_frag;
		id.layout = R.foliageLayout;
		id.colorFormats = {kHdrFormat, kIdFormat};
		id.depthFormat = kDepthFormat;
		id.depthTest = id.depthWrite = true;
		id.depthCompare = VK_COMPARE_OP_GREATER;
		R.impostorPipeline = MakeGraphicsPipeline(id);
		PipelineDesc is = sd;
		is.vs = &hrl_vk_impostor_shadow_vert;
		is.fs = &hrl_vk_impostor_shadow_frag;
		is.input = VertexInput::None;
		R.impostorShadowPipeline = MakeGraphicsPipeline(is);
		for (uint32_t i = 0; i < 2; ++i)
		{
			PipelineDesc bd;
			bd.vs = &hrl_vk_impostor_bake_vert;
			bd.fs = i ? &hrl_vk_impostor_bake_at_frag : &hrl_vk_impostor_bake_frag;
			bd.layout = R.sceneLayout;
			bd.input = VertexInput::Mesh;
			bd.colorFormats = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
			bd.depthFormat = kDepthFormat;
			bd.depthTest = bd.depthWrite = true;
			bd.depthCompare = VK_COMPARE_OP_LESS;
			R.impostorBakePipelines[i] = MakeGraphicsPipeline(bd);
		}
		if (!R.foliageCullPipeline || !R.foliageCommandsPipeline || !R.foliageShadowPipeline || !R.foliageShadowMaskedPipeline ||
			!R.impostorPipeline || !R.impostorShadowPipeline || !R.impostorBakePipelines[0] || !R.impostorBakePipelines[1])
			return false;
	}
	{
		// Grass (vk_grass.cpp): blades built in the vertex shader, no vertex input.
		const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
		R.grassSetLayout = MakeSetLayout({
			{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VF, nullptr},
			{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}});
		R.grassCullSetLayout = MakeSetLayout({
			{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, C, nullptr},
			{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, nullptr},
			{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, nullptr},
			{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, nullptr},
			{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, nullptr},
			{5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, C, nullptr}});
		if (!R.grassSetLayout || !R.grassCullSetLayout) return false;
		R.grassLayout = MakePipelineLayout({R.viewLayout, R.bindlessLayout, R.grassSetLayout}, VF, sizeof(SkyPush));
		R.grassCullLayout = MakePipelineLayout({R.grassCullSetLayout, R.bindlessLayout}, C, 0);
		R.grassCullPipeline = MakeComputePipeline(hrl_vk_grass_cull_comp, R.grassCullLayout);
		PipelineDesc d;
		d.vs = &hrl_vk_grass_vert;
		d.fs = rq ? &hrl_vk_grass_rq_frag : &hrl_vk_grass_frag;
		d.layout = R.grassLayout;
		d.colorFormats = {kHdrFormat, kIdFormat};
		d.depthFormat = kDepthFormat;
		d.depthTest = d.depthWrite = true;
		d.depthCompare = VK_COMPARE_OP_GREATER;
		R.grassPipeline = MakeGraphicsPipeline(d);
		PipelineDesc sd;
		sd.vs = &hrl_vk_grass_shadow_vert;
		sd.layout = R.grassLayout;
		sd.depthFormat = kDepthFormat;
		sd.depthTest = sd.depthWrite = true;
		sd.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
		sd.depthBias = true;
		R.grassShadowPipeline = MakeGraphicsPipeline(sd);
		if (!R.grassCullPipeline || !R.grassPipeline || !R.grassShadowPipeline) return false;
	}
	{
		// Landscapes: patches instanced per quadtree node (vk_landscape.cpp).
		R.landscapeSetLayout = MakeSetLayout({{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VF, nullptr}});
		if (!R.landscapeSetLayout) return false;
		// Same push range as sceneLayout: the two layouts stay compatible for sets 0 and 1.
		R.landscapeLayout = MakePipelineLayout({R.viewLayout, R.bindlessLayout, R.landscapeSetLayout}, VF, sizeof(SkyPush));
		PipelineDesc d;
		d.vs = &hrl_vk_landscape_vert;
		d.fs = rq ? &hrl_vk_mesh_land_rq_frag : &hrl_vk_mesh_land_frag;
		d.layout = R.landscapeLayout;
		d.input = VertexInput::Landscape;
		d.colorFormats = {kHdrFormat, kIdFormat};
		d.depthFormat = kDepthFormat;
		d.depthTest = d.depthWrite = true;
		d.depthCompare = VK_COMPARE_OP_GREATER;
		d.cull = VK_CULL_MODE_BACK_BIT;
		R.landscapePipeline = MakeGraphicsPipeline(d);
		PipelineDesc sd;
		sd.vs = &hrl_vk_landscape_shadow_vert;
		sd.layout = R.landscapeLayout;
		sd.input = VertexInput::Landscape;
		sd.depthFormat = kDepthFormat;
		sd.depthTest = sd.depthWrite = true;
		sd.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
		sd.depthBias = true;
		R.landscapeShadowPipeline = MakeGraphicsPipeline(sd);
		if (!R.landscapePipeline || !R.landscapeShadowPipeline) return false;
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
		d.fs = &hrl_vk_bloom_frag;
		d.layout = R.bloomLayout;
		d.colorFormats = {kBloomFormat};
		R.bloomDownPipeline = MakeGraphicsPipeline(d);
		d.blend = d.additive = true;
		R.bloomUpPipeline = MakeGraphicsPipeline(d);
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
	const VkDescriptorSetLayoutBinding skinBindings[3] = {
		{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
	R.skinLayout = MakeSetLayout({skinBindings[0], skinBindings[1], skinBindings[2]});
	if (!R.skinLayout) return false;
	R.skinPipelineLayout = MakePipelineLayout({R.skinLayout}, VK_SHADER_STAGE_COMPUTE_BIT, 12);
	R.skinPipeline = MakeComputePipeline(hrl_vk_skin_comp, R.skinPipelineLayout);
	R.envCubePipeline = MakeComputePipeline(hrl_vk_env_cube_comp, R.computeLayout);
	R.envFilterPipeline = MakeComputePipeline(hrl_vk_env_filter_comp, R.computeLayout);
	R.brdfLutPipeline = MakeComputePipeline(hrl_vk_brdf_lut_comp, R.computeLayout);
	R.hzbPipeline = MakeComputePipeline(hrl_vk_hzb_comp, R.computeLayout);
	{
		const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
		std::vector<VkDescriptorSetLayoutBinding> cull;
		for (uint32_t i = 0; i < 6; ++i) cull.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, nullptr});
		cull.push_back({6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, nullptr});
		cull.push_back({7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, C, nullptr});
		R.cullSetLayout = MakeSetLayout(cull);
		if (!R.cullSetLayout) return false;
		R.cullLayout = MakePipelineLayout({R.cullSetLayout}, C, 0);
		R.cullPipeline = MakeComputePipeline(hrl_vk_cull_comp, R.cullLayout);
	}
	return R.shadowPipeline && R.shadowMaskedPipeline && R.skyPipeline && R.skinPipeline && R.tonemapPipeline && R.bloomDownPipeline && R.bloomUpPipeline && R.debugLinePipeline &&
		R.debugTrianglePipeline && R.envCubePipeline && R.envFilterPipeline && R.brdfLutPipeline &&
		R.hzbPipeline && R.cullPipeline;
}
} // namespace hrlvk
