// HRL native Vulkan backend - descriptor layouts and pipelines.
#include "vk_renderer_internal.h"

namespace hrlvk
{
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
} // namespace hrlvk
