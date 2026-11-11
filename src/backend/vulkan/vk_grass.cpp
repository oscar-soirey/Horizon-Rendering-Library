// HRL native Vulkan backend - procedural grass.
//
// Nothing is stored per blade. Each frame and view, grass_cull.comp walks the
// square tiles around the camera, keeps those in the frustum, in range and
// not hidden (HZB), and writes one indirect draw per tile; grass.vert then
// builds each blade from a hash of its tile and index, on the landscape
// heights, with the wind and the interactors of the foliage. Far tiles draw
// fewer, wider, single-triangle blades.
#include "vk_renderer_internal.h"

namespace hrlvk
{
namespace
{
constexpr float kTileSize = 4.f;

void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
	VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
	VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	mb.srcStageMask = srcStage;
	mb.srcAccessMask = srcAccess;
	mb.dstStageMask = dstStage;
	mb.dstAccessMask = dstAccess;
	VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.memoryBarrierCount = 1;
	di.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2(cmd, &di);
}

void DestroyGrassGPU(GrassGPU& g)
{
	Dev().DestroyBuffer(g.tiles);
	Dev().DestroyBuffer(g.draw);
	g = GrassGPU{};
}
} // namespace

std::vector<GrassDraw> PrepareGrass(const hrl_scene_t* scene, const glm::vec3& camera)
{
	Device& dev = Dev();
	std::vector<GrassDraw> draws;
	for (const auto& [id, grass] : scene->grass)
	{
		if (!grass) continue;
		auto land = scene->landscapes.find(grass->landscape_);
		auto landGpu = R.landscapes.find(grass->landscape_);
		if (land == scene->landscapes.end() || !land->second || landGpu == R.landscapes.end() || !landGpu->second.heights) continue;
		const HRL_Landscape* l = land->second;
		const LandscapeGPU& lg = landGpu->second;
		GrassGPU& g = R.grass[id];
		g.lastFrame = dev.FrameNumber();
		const uint32_t side = 2u * (uint32_t)std::ceil(grass->distance / kTileSize) + 1u;
		const uint32_t maxTiles = side * side;
		if (!g.draw || g.maxTiles < maxTiles)
		{
			DestroyGrassGPU(g);
			g.lastFrame = dev.FrameNumber();
			g.maxTiles = maxTiles;
			if (!dev.CreateBuffer(VkDeviceSize(maxTiles) * sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, g.tiles, "grass tiles") ||
				!dev.CreateBuffer(16 + VkDeviceSize(maxTiles) * sizeof(VkDrawIndirectCommand),
					VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, g.draw, "grass draw"))
			{
				DestroyGrassGPU(g);
				R.grass.erase(id);
				continue;
			}
		}
		GrassUniform u{};
		u.model = CalculateLandscapeModelMatrix(l);
		u.invModel = glm::inverse(u.model);
		u.samples = glm::vec4((float)lg.width, (float)lg.height, 1.f / (float)lg.width, 1.f / (float)lg.height);
		u.spacing = glm::vec4(l->size_.x / float(lg.width - 1), l->size_.y / float(lg.height - 1), l->height_scale_, 0.f);
		u.blade = glm::vec4(grass->blade_height, grass->blade_width, grass->height_variation, grass->density);
		u.baseColor = glm::vec4(grass->base_color, std::cos(glm::radians(grass->max_slope)));
		u.tipColor = glm::vec4(grass->tip_color, grass->distance);
		// Tiles on a fixed world grid, centred on the camera.
		const glm::vec2 snapped = glm::floor(glm::vec2(camera.x, camera.z) / kTileSize) * kTileSize;
		u.grid = glm::vec4(snapped - float(side / 2u) * kTileSize, kTileSize, (float)side);
		auto mask = R.textures.find(grass->mask);
		u.ids = glm::uvec4(lg.slot, mask != R.textures.end() ? mask->second.slot : kNoSlot, (uint32_t)grass->landscape_, maxTiles);
		GrassDraw d;
		d.id = id;
		d.grass = grass;
		d.uniform = dev.AllocFrame(sizeof(GrassUniform), 256);
		d.set = AllocFrameSet(R.grassSetLayout);
		if (!d.uniform.ptr || !d.set) continue;
		std::memcpy(d.uniform.ptr, &u, sizeof u);
		const VkDescriptorBufferInfo infos[2] = {{d.uniform.buffer, d.uniform.offset, sizeof(GrassUniform)}, {g.tiles.handle, 0, VK_WHOLE_SIZE}};
		VkWriteDescriptorSet writes[2]{};
		for (uint32_t i = 0; i < 2; ++i)
		{
			writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[i].dstSet = d.set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &infos[i];
		}
		vkUpdateDescriptorSets(dev.device, 2, writes, 0, nullptr);
		d.tiles = maxTiles;
		draws.push_back(d);
	}
	return draws;
}

void AddGrassCull(FrameGraph& graph, const GrassDraw& draw, const CullRequest& request, const glm::vec3& camera, FrameGraph::Handle hzb)
{
	Device& dev = Dev();
	auto it = R.grass.find(draw.id);
	if (it == R.grass.end()) return;
	const GrassGPU& g = it->second;
	CullView cv{};
	cv.viewProj = request.viewProj;
	for (int i = 0; i < 6; ++i) cv.planes[i] = request.frustum.planes[i];
	cv.camPos = glm::vec4(camera, 0.f);
	cv.hzb = glm::vec4(request.hzbSize, (float)std::max(request.hzbMips, 1u), 0.f);
	uint32_t flags = request.flags;
	if (!request.hzbView) flags &= ~CullFlagOcclusion;
	cv.params = glm::uvec4(0u, 0u, 0u, flags);
	const FrameSlice ubo = dev.AllocFrame(sizeof(CullView), 256);
	const VkDescriptorSet set = AllocFrameSet(R.grassCullSetLayout);
	if (!ubo.ptr || !set) return;
	std::memcpy(ubo.ptr, &cv, sizeof cv);
	const VkDescriptorBufferInfo buffers[6] = {
		{draw.uniform.buffer, draw.uniform.offset, sizeof(GrassUniform)},
		{g.tiles.handle, 0, VK_WHOLE_SIZE},
		{g.draw.handle, 16, VK_WHOLE_SIZE},
		{g.draw.handle, 0, 16},
		{},
		{ubo.buffer, ubo.offset, sizeof(CullView)}};
	const VkDescriptorImageInfo image{R.linearClamp, request.hzbView ? request.hzbView : R.white.image.view,
		request.hzbView ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet writes[6]{};
	for (uint32_t i = 0; i < 6; ++i)
	{
		writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		if (i == 4)
		{
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i].pImageInfo = &image;
		}
		else
		{
			writes[i].descriptorType = (i == 0 || i == 5) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &buffers[i];
		}
	}
	vkUpdateDescriptorSets(dev.device, 6, writes, 0, nullptr);
	const VkBuffer drawBuffer = g.draw.handle;
	const uint32_t groups = (draw.tiles + 63u) / 64u;
	auto& pass = graph.AddPass("grass cull", [=](VkCommandBuffer c)
	{
		Barrier(c, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
			VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
			VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		vkCmdFillBuffer(c, drawBuffer, 0, 16, 0);
		Barrier(c, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		const VkDescriptorSet sets[2] = {set, R.bindlessSet};
		vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, R.grassCullLayout, 0, 2, sets, 0, nullptr);
		vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, R.grassCullPipeline);
		vkCmdDispatch(c, groups, 1, 1);
		Barrier(c, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
			VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
	});
	if (request.hzbView) pass.Use(hzb, Access::Storage);
}

void DrawGrass(VkCommandBuffer cmd, const GrassDraw& draw, bool shadow, const glm::mat4& lightViewProj)
{
	auto it = R.grass.find(draw.id);
	if (it == R.grass.end()) return;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow ? R.grassShadowPipeline : R.grassPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.grassLayout, 2, 1, &draw.set, 0, nullptr);
	if (shadow)
		vkCmdPushConstants(cmd, R.grassLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::mat4), &lightViewProj);
	vkCmdDrawIndirectCount(cmd, it->second.draw.handle, 16, it->second.draw.handle, 0, draw.tiles, sizeof(VkDrawIndirectCommand));
}

void ReleaseUnusedGrass()
{
	const uint64_t frame = Dev().FrameNumber();
	for (auto it = R.grass.begin(); it != R.grass.end();)
	{
		if (it->second.lastFrame + 8 < frame)
		{
			DestroyGrassGPU(it->second);
			it = R.grass.erase(it);
		}
		else ++it;
	}
}

void DestroyAllGrass()
{
	for (auto& [id, g] : R.grass) DestroyGrassGPU(g);
	R.grass.clear();
}
} // namespace hrlvk
