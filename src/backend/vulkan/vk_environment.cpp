// HRL native Vulkan backend - image based lighting: environment cube,
// prefiltered specular, diffuse irradiance, BRDF lookup table.
#include "vk_renderer_internal.h"

namespace hrlvk
{
// ---------------------------------------------------------------------------
// Image based lighting
// ---------------------------------------------------------------------------
namespace
{
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

} // namespace

glm::mat4 SkyRotation(const glm::vec3& degrees)
{
	glm::mat4 r(1.f);
	r = glm::rotate(r, glm::radians(degrees.x), glm::vec3(1.f, 0.f, 0.f));
	r = glm::rotate(r, glm::radians(degrees.y), glm::vec3(0.f, 1.f, 0.f));
	r = glm::rotate(r, glm::radians(degrees.z), glm::vec3(0.f, 0.f, 1.f));
	return glm::transpose(r); // world direction -> sky direction
}

namespace
{
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

// Texture the environment is built from, HRL_INVALID_ID for the sky gradient.
HRL_id EnvironmentSource(const hrl_scene_t* scene)
{
	const HRL_id source = scene->environment_texture != HRL_INVALID_ID ? scene->environment_texture : scene->sky_texture;
	return R.textures.find(source) != R.textures.end() ? source : HRL_INVALID_ID;
}
} // namespace

bool PrepareEnvironment(SceneGPU& gpu, const hrl_scene_t* scene, bool& dirty)
{
	dirty = false;
	if (!scene->environment_mapping_enabled) return false;
	const HRL_id source = EnvironmentSource(scene);

	Environment& env = gpu.env;
	if (env.valid && env.texture == source && env.rotation == scene->sky_rotation &&
		(source != HRL_INVALID_ID || (env.top == scene->sky_top_color && env.horizon == scene->sky_horizon_color && env.bottom == scene->sky_bottom_color)))
		return true;

	if (!env.cube && !CreateEnvironmentImages(env)) { DestroyEnvironment(env); return false; }
	dirty = true;
	return true;
}

void RecordEnvironment(VkCommandBuffer cmd, SceneGPU& gpu, const hrl_scene_t* scene)
{
	Device& dev = Dev();
	Environment& env = gpu.env;
	const HRL_id source = EnvironmentSource(scene);
	auto texIt = R.textures.find(source);

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

	env.valid = true;
	env.texture = source;
	env.rotation = scene->sky_rotation;
	env.top = scene->sky_top_color;
	env.horizon = scene->sky_horizon_color;
	env.bottom = scene->sky_bottom_color;
}

void RecordBrdfLut(VkCommandBuffer cmd)
{
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
} // namespace hrlvk
