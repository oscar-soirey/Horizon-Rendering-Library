// HRL native Vulkan backend - foliage.
//
// Per foliage kind the GPU keeps every instance (32 bytes) in one buffer,
// sub-allocated per storage cell of the frontend: painting or erasing sends
// only the cells that changed. Per view, foliage_cull.comp tests the cells
// then their instances (frustum, distance, occlusion against the HZB of the
// first occlusion phase) and appends each visible instance to the list of its
// level of detail; foliage_commands.comp then writes one indirect draw per
// part and level. The CPU cost does not depend on the instance count.
//
// The visible lists and draw commands of a foliage kind are reused by every
// view of the frame one after the other (shadow maps, then the camera): each
// culling pass waits for the draws of the previous view.
#include "vk_renderer.h"
#include "vk_renderer_internal.h"

#include <glm/gtc/quaternion.hpp>

namespace hrlvk
{
namespace
{
constexpr VkDeviceSize kDrawCountsOffset = 0;            // uint counts[4]
constexpr VkDeviceSize kDrawOneOffset = 16;              // uint 1 (draw count of the impostor command)
constexpr VkDeviceSize kDrawThreeOffset = 20;            // uint 3 (draw count of the LOD commands of a part)
constexpr VkDeviceSize kDrawCommandsOffset = 32;         // 12 indexed commands (part * 3 + lod)
constexpr VkDeviceSize kDrawImpostorOffset = 32 + 12 * 20; // impostor command
constexpr VkDeviceSize kDrawBufferSize = kDrawImpostorOffset + 16;
constexpr uint32_t kMinCellCapacity = 32;

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

uint32_t NextPowerOfTwo(uint32_t v)
{
	uint32_t p = 1;
	while (p < v) p *= 2;
	return p;
}

void FreeCells(FoliageGPU& g)
{
	for (FoliageCellGPU& c : g.cells)
		if (c.alloc && g.block) vmaVirtualFree(g.block, c.alloc);
	g.cells.clear();
}

bool CreatePool(FoliageGPU& g, uint32_t capacity)
{
	Device& dev = Dev();
	if (g.block) vmaDestroyVirtualBlock(g.block);
	g.block = VK_NULL_HANDLE;
	dev.DestroyBuffer(g.instances);
	VmaVirtualBlockCreateInfo bi{};
	bi.size = VkDeviceSize(capacity);
	if (vmaCreateVirtualBlock(&bi, &g.block) != VK_SUCCESS) return false;
	g.poolCapacity = capacity;
	return dev.CreateBuffer(VkDeviceSize(capacity) * sizeof(HRL_FoliageInstanceData),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, g.instances, "foliage instances");
}

bool AllocateCell(FoliageGPU& g, FoliageCellGPU& c, uint32_t count)
{
	if (c.alloc) vmaVirtualFree(g.block, c.alloc);
	c.alloc = VK_NULL_HANDLE;
	c.capacity = std::max(NextPowerOfTwo(count), kMinCellCapacity);
	VmaVirtualAllocationCreateInfo ai{};
	ai.size = c.capacity;
	VkDeviceSize offset = 0;
	if (vmaVirtualAllocate(g.block, &ai, &c.alloc, &offset) != VK_SUCCESS)
	{
		c.alloc = VK_NULL_HANDLE;
		c.capacity = 0;
		return false;
	}
	c.offset = (uint32_t)offset;
	return true;
}

void DestroyFoliageGPU(FoliageGPU& g)
{
	Device& dev = Dev();
	FreeCells(g);
	if (g.block)
	{
		// The buffer may still be read by frames in flight: keep the block until then too.
		VmaVirtualBlock block = g.block;
		dev.Defer([block]() { vmaDestroyVirtualBlock(block); });
	}
	dev.DestroyBuffer(g.instances);
	dev.DestroyBuffer(g.visible);
	dev.DestroyBuffer(g.draw);
	dev.DestroyBuffer(g.cellTable);
	for (Buffer& b : g.readback) dev.DestroyBuffer(b);
	for (auto& part : g.lodBlas)
		for (Blas& b : part) DestroyBlas(b);
	DestroyImpostor(g);
	g = FoliageGPU{};
}

// End of LOD 0: from the setting, or from the size of the plant.
float LodDistance(const HRL_Foliage* f)
{
	if (f->lod_distance > 0.f) return f->lod_distance;
	return std::max(f->bounds_radius * 12.f, 12.f);
}
} // namespace

// ---------------------------------------------------------------------------
void ResolveFoliageMaterials(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame)
{
	for (const auto& [id, f] : scene->foliage)
		if (f)
			for (const auto& part : f->parts) MaterialIndex(data, frame, part->material);
}

std::vector<FoliageDraw> PrepareFoliage(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame, FrameGraph& graph)
{
	Device& dev = Dev();
	std::vector<FoliageDraw> draws;
	struct Upload { VkBuffer src, dst; VkBufferCopy region; };
	std::vector<Upload> uploads;
	for (const auto& [id, f] : scene->foliage)
	{
		if (!f || f->parts.empty()) continue;
		FoliageGPU& g = R.foliage[id];
		g.lastFrame = dev.FrameNumber();
		// Statistics of the frame that last used this slot (complete: its fence was waited).
		const uint32_t slot = dev.FrameNumber() % kFramesInFlight;
		if (g.readbackPending[slot] && g.readback[slot].mapped)
		{
			std::memcpy(g.stats, g.readback[slot].mapped, sizeof(g.stats));
			g.readbackPending[slot] = false;
		}

		// ---- Instances: upload the cells that changed --------------------------
		// Nothing to do on the CPU for foliage that did not change.
		if (g.instancesRevision != f->instances_revision || !g.instances || !g.cellTable)
		{
			if (g.cellsRevision != f->cells_revision && f->cells.size() < g.cells.size())
				FreeCells(g); // cells were removed (cleared): start again
			g.cellsRevision = f->cells_revision;
			// Room needed: every cell rounded up to its allocation size.
			uint64_t needed = 0;
			for (const HRL_FoliageCell& cell : f->cells)
				needed += std::max(NextPowerOfTwo((uint32_t)cell.instances.size()), kMinCellCapacity);
			bool reuploadAll = !g.instances;
			if (!g.instances || g.poolCapacity < needed + needed / 4)
			{
				// Twice the room, so painting does not reallocate every frame.
				const uint64_t capacity = std::max<uint64_t>(NextPowerOfTwo((uint32_t)std::min<uint64_t>(needed * 2, 1u << 24)), 4096u);
				FreeCells(g);
				if (needed > (1u << 24) || !CreatePool(g, (uint32_t)capacity))
				{
					ReportError("foliage: too many instances in one foliage kind (16 million at most)");
					R.foliage.erase(id);
					continue;
				}
				reuploadAll = true;
			}
			g.cells.resize(f->cells.size());
			bool full = false;
			for (size_t i = 0; i < f->cells.size(); ++i)
			{
				const HRL_FoliageCell& cell = f->cells[i];
				FoliageCellGPU& c = g.cells[i];
				if (!reuploadAll && c.revision == cell.revision) continue;
				const uint32_t count = (uint32_t)cell.instances.size();
				if (count > c.capacity || (c.capacity > kMinCellCapacity && count * 4 < c.capacity))
					if (!AllocateCell(g, c, count)) { full = true; break; }
				c.count = count;
				c.revision = cell.revision;
				if (!count) continue;
				const VkDeviceSize bytes = VkDeviceSize(count) * sizeof(HRL_FoliageInstanceData);
				const FrameSlice slice = dev.AllocFrame(bytes, 16);
				if (!slice.ptr) { c.revision = 0; continue; }
				std::memcpy(slice.ptr, cell.instances.data(), (size_t)bytes);
				uploads.push_back({slice.buffer, g.instances.handle, {slice.offset, VkDeviceSize(c.offset) * sizeof(HRL_FoliageInstanceData), bytes}});
			}
			if (full)
			{
				// Fragmented pool: rebuilt next frame.
				FreeCells(g);
				g.poolCapacity = 0;
				continue;
			}
			// Table of the non-empty cells, kept on the GPU until the next change.
			std::vector<FoliageCellData> table;
			table.reserve(f->cells.size());
			for (size_t i = 0; i < f->cells.size(); ++i)
			{
				const FoliageCellGPU& c = g.cells[i];
				if (!c.count || !c.alloc) continue;
				const HRL_FoliageCell& cell = f->cells[i];
				table.push_back({glm::vec4(cell.bounds_min, 0.f), glm::vec4(cell.bounds_max, 0.f), glm::uvec4(c.offset, c.count, 0u, 0u)});
			}
			const VkDeviceSize tableBytes = std::max<size_t>(table.size(), 1) * sizeof(FoliageCellData);
			if (!g.cellTable || g.cellTable.size < tableBytes)
			{
				dev.DestroyBuffer(g.cellTable);
				if (!dev.CreateBuffer(tableBytes * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false,
					g.cellTable, "foliage cells")) { R.foliage.erase(id); continue; }
			}
			if (!table.empty())
			{
				const FrameSlice slice = dev.AllocFrame(table.size() * sizeof(FoliageCellData), 16);
				if (!slice.ptr) continue;
				std::memcpy(slice.ptr, table.data(), table.size() * sizeof(FoliageCellData));
				uploads.push_back({slice.buffer, g.cellTable.handle, {slice.offset, 0, table.size() * sizeof(FoliageCellData)}});
			}
			g.cellCount = (uint32_t)table.size();
			g.instancesRevision = f->instances_revision;
		}
		if (!g.cellCount) continue;

		// ---- Visible lists and draw commands -------------------------------------
		const uint32_t total = (uint32_t)std::min<size_t>(f->instance_count, 0xFFFFFFu);
		const uint32_t regionCapacity = std::max(total, 64u);
		if (!g.visible || g.regionCapacity < regionCapacity)
		{
			dev.DestroyBuffer(g.visible);
			g.regionCapacity = NextPowerOfTwo(regionCapacity);
			if (!dev.CreateBuffer(VkDeviceSize(g.regionCapacity) * 4u * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				false, g.visible, "foliage visible")) { R.foliage.erase(id); continue; }
		}
		if (!g.draw && !dev.CreateBuffer(kDrawBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, g.draw, "foliage draw")) { R.foliage.erase(id); continue; }

		// ---- Uniform data ---------------------------------------------------------
		FoliageUniform u{};
		u.bounds = glm::vec4(f->bounds_center, f->bounds_radius);
		// Levels of detail: as many as the most detailed part has; a part with
		// fewer levels keeps its last one.
		int meshLods = 0;
		int partLods[kFoliageMaxParts]{};
		for (size_t p = 0; p < f->parts.size() && p < kFoliageMaxParts; ++p)
		{
			auto mesh = R.meshes.find(f->parts[p]->mesh_id);
			int valid = 0;
			if (mesh != R.meshes.end())
				while (valid < 3 && valid < (int)mesh->second.lods.size() && mesh->second.lods[(size_t)valid].valid) ++valid;
			partLods[p] = valid;
			meshLods = std::max(meshLods, valid);
		}
		if (meshLods <= 0) continue;
		u.heights = glm::vec4(f->bounds_min.y, std::max(f->bounds_max.y - f->bounds_min.y, 1e-3f), (float)meshLods, (float)kImpostorFrames);
		const float lod0 = LodDistance(f);
		const bool impostors = f->impostor_distance > 0.f && g.impostorReady;
		u.lodEnd = glm::vec4(lod0, lod0 * 2.f, lod0 * 4.f, impostors ? f->impostor_distance : 0.f);
		u.draw = glm::vec4(f->draw_distance, f->draw_distance * 0.9f, 0.1f, f->wind_response);
		u.ids = glm::uvec4((uint32_t)id, impostors ? g.albedoSlot : kNoSlot, impostors ? g.normalSlot : kNoSlot, g.regionCapacity);
		FoliageDraw d;
		d.id = id;
		d.foliage = f;
		d.cellCount = g.cellCount;
		d.cells = g.cellTable.handle;
		d.impostors = impostors;
		for (size_t p = 0; p < f->parts.size() && p < kFoliageMaxParts; ++p)
		{
			const uint32_t material = MaterialIndex(data, frame, f->parts[p]->material);
			u.materials[(int)p] = material;
			const Bucket bucket = data.materialBuckets[material];
			// Translucent foliage is drawn alpha tested.
			d.masked[p] = IsMasked(bucket) || IsTranslucent(bucket);
			d.twoSided[p] = IsTwoSided(bucket);
			auto mesh = R.meshes.find(f->parts[p]->mesh_id);
			if (mesh == R.meshes.end() || partLods[p] == 0) continue; // the part draws nothing (valid = 0)
			for (int lod = 0; lod < meshLods; ++lod)
			{
				const MeshLOD& m = mesh->second.lods[(size_t)std::min(lod, partLods[p] - 1)];
				u.lods[p * 3 + (size_t)lod] = glm::uvec4(m.indexCount, m.firstIndex, m.vertexOffset, 1u);
			}
		}
		u.impostor = glm::vec4(g.impostorCenter, g.impostorHalfSize);
		d.partCount = (uint32_t)std::min<size_t>(f->parts.size(), kFoliageMaxParts);
		d.uniform = dev.AllocFrame(sizeof(FoliageUniform), 256);
		if (!d.uniform.ptr) continue;
		std::memcpy(d.uniform.ptr, &u, sizeof u);

		// Set 2 of the foliage pipelines: uniform, instances, visible lists.
		d.set = AllocFrameSet(R.foliageSetLayout);
		if (!d.set) continue;
		const VkDescriptorBufferInfo infos[3] = {
			{d.uniform.buffer, d.uniform.offset, sizeof(FoliageUniform)},
			{g.instances.handle, 0, VK_WHOLE_SIZE},
			{g.visible.handle, 0, VK_WHOLE_SIZE}};
		VkWriteDescriptorSet writes[3]{};
		for (uint32_t i = 0; i < 3; ++i)
		{
			writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[i].dstSet = d.set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &infos[i];
		}
		vkUpdateDescriptorSets(dev.device, 3, writes, 0, nullptr);
		draws.push_back(d);
	}
	if (!uploads.empty())
		graph.AddPass("foliage upload", [uploads](VkCommandBuffer c)
		{
			// Earlier frames may still read the instances.
			Barrier(c, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
				VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			for (const Upload& u : uploads) vkCmdCopyBuffer(c, u.src, u.dst, 1, &u.region);
			Barrier(c, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
		});
	return draws;
}

void AddFoliageCull(FrameGraph& graph, const FoliageDraw& draw, const CullRequest& request, const glm::vec3& lodCamera,
	FrameGraph::Handle hzb, bool readback)
{
	Device& dev = Dev();
	auto it = R.foliage.find(draw.id);
	if (it == R.foliage.end()) return;
	FoliageGPU& g = it->second;
	VkBuffer readbackBuffer = VK_NULL_HANDLE;
	if (readback)
	{
		const uint32_t slot = dev.FrameNumber() % kFramesInFlight;
		if (g.readback[slot] || dev.CreateBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, g.readback[slot], "foliage stats"))
		{
			readbackBuffer = g.readback[slot].handle;
			g.readbackPending[slot] = true;
		}
	}
	CullView cv{};
	cv.viewProj = request.viewProj;
	for (int i = 0; i < 6; ++i) cv.planes[i] = request.frustum.planes[i];
	cv.camPos = glm::vec4(lodCamera, 0.f);
	cv.hzb = glm::vec4(request.hzbSize, (float)std::max(request.hzbMips, 1u), 0.f);
	uint32_t flags = request.flags;
	if (!request.hzbView) flags &= ~CullFlagOcclusion;
	cv.params = glm::uvec4(0u, 0u, 0u, flags);
	const FrameSlice ubo = dev.AllocFrame(sizeof(CullView), 256);
	const VkDescriptorSet set = AllocFrameSet(R.foliageCullSetLayout);
	if (!ubo.ptr || !set) return;
	std::memcpy(ubo.ptr, &cv, sizeof cv);
	const VkDescriptorBufferInfo buffers[7] = {
		{draw.uniform.buffer, draw.uniform.offset, sizeof(FoliageUniform)},
		{g.instances.handle, 0, VK_WHOLE_SIZE},
		{g.visible.handle, 0, VK_WHOLE_SIZE},
		{draw.cells, 0, VK_WHOLE_SIZE},
		{g.draw.handle, 0, VK_WHOLE_SIZE},
		{}, // HZB (image)
		{ubo.buffer, ubo.offset, sizeof(CullView)}};
	const VkDescriptorImageInfo image{R.linearClamp, request.hzbView ? request.hzbView : R.white.image.view,
		request.hzbView ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet writes[7]{};
	for (uint32_t i = 0; i < 7; ++i)
	{
		writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		if (i == 5)
		{
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i].pImageInfo = &image;
		}
		else
		{
			writes[i].descriptorType = (i == 0 || i == 6) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &buffers[i];
		}
	}
	vkUpdateDescriptorSets(dev.device, 7, writes, 0, nullptr);

	const VkBuffer drawBuffer = g.draw.handle;
	const uint32_t groups = draw.cellCount;
	auto& pass = graph.AddPass("foliage cull", [=](VkCommandBuffer c)
	{
		// The previous view of the frame drew from these lists.
		Barrier(c, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		vkCmdFillBuffer(c, drawBuffer, kDrawCountsOffset, 16, 0);
		Barrier(c, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, R.foliageCullLayout, 0, 1, &set, 0, nullptr);
		vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, R.foliageCullPipeline);
		vkCmdDispatch(c, groups, 1, 1);
		Barrier(c, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, R.foliageCommandsPipeline);
		vkCmdDispatch(c, 1, 1, 1);
		Barrier(c, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
			VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
		if (readbackBuffer)
		{
			const VkBufferCopy region{kDrawCountsOffset, 0, 16};
			vkCmdCopyBuffer(c, drawBuffer, readbackBuffer, 1, &region);
		}
	});
	if (request.hzbView) pass.Use(hzb, Access::Storage);
}

void DrawFoliage(VkCommandBuffer cmd, const FoliageDraw& draw, bool shadow, const glm::mat4& lightViewProj, const glm::vec3& lightDir)
{
	auto it = R.foliage.find(draw.id);
	if (it == R.foliage.end()) return;
	const FoliageGPU& g = it->second;
	const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.foliageLayout, 2, 1, &draw.set, 0, nullptr);
	VkDeviceSize zero = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &R.vertexBuffer.handle, &zero);
	vkCmdBindIndexBuffer(cmd, R.indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);
	for (uint32_t p = 0; p < draw.partCount; ++p)
	{
		VkPipeline pipeline;
		if (shadow) pipeline = draw.masked[p] ? R.foliageShadowMaskedPipeline : R.foliageShadowPipeline;
		else pipeline = R.foliagePipelines[(draw.masked[p] ? 2 : 0) + (draw.twoSided[p] ? 1 : 0)];
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		struct { glm::mat4 lightViewProj; uint32_t part; } push{lightViewProj, p};
		vkCmdPushConstants(cmd, R.foliageLayout, VF, 0, sizeof(push), &push);
		// One command per level of detail; levels without instances draw nothing.
		vkCmdDrawIndexedIndirectCount(cmd, g.draw.handle, kDrawCommandsOffset + VkDeviceSize(p) * 3u * 20u,
			g.draw.handle, kDrawThreeOffset, 3, 20);
	}
	if (draw.impostors) DrawImpostors(cmd, draw, shadow, lightViewProj, lightDir);
}

void CollectFoliageRayInstances(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame,
	const glm::vec3& camera, RayScene& out)
{
	uint32_t budget = kRayFoliageBudget;
	for (const auto& [id, f] : scene->foliage)
	{
		if (!f || f->parts.empty() || !f->cast_shadows) continue;
		auto gpuIt = R.foliage.find(id);
		if (gpuIt == R.foliage.end()) continue;
		FoliageGPU& g = gpuIt->second;
		// One structure per part and level of detail (built once per geometry).
		if (g.blasRevision != f->geometry_revision)
		{
			for (auto& part : g.lodBlas)
				for (Blas& blas : part) DestroyBlas(blas);
			for (size_t p = 0; p < f->parts.size() && p < kFoliageMaxParts; ++p)
			{
				auto mesh = R.meshes.find(f->parts[p]->mesh_id);
				if (mesh == R.meshes.end()) continue;
				for (size_t lod = 0; lod < 3 && lod < mesh->second.lods.size(); ++lod)
				{
					const MeshLOD& m = mesh->second.lods[lod];
					if (!m.valid) break;
					BuildBlas(g.lodBlas[p][lod], R.vertexBuffer.address + VkDeviceSize(m.vertexOffset) * sizeof(HRL_Vertex3D),
						sizeof(HRL_Vertex3D), m.vertexCount, R.indexBuffer.address + VkDeviceSize(m.firstIndex) * sizeof(uint32_t),
						m.indexCount, true);
				}
			}
			g.blasRevision = f->geometry_revision;
		}
		struct Part { MeshGPU lods[3]; int count; uint32_t material; bool masked; };
		std::vector<Part> parts;
		for (size_t p = 0; p < f->parts.size() && p < kFoliageMaxParts; ++p)
		{
			auto mesh = R.meshes.find(f->parts[p]->mesh_id);
			if (mesh == R.meshes.end()) continue;
			Part part{};
			for (int lod = 0; lod < 3 && g.lodBlas[p][lod].handle; ++lod)
			{
				// A MeshGPU view of one level: its range and its structure.
				part.lods[lod].lods = {mesh->second.lods[(size_t)lod]};
				part.lods[lod].blas = g.lodBlas[p][lod];
				part.count = lod + 1;
			}
			if (!part.count) continue;
			part.material = MaterialIndex(data, frame, f->parts[p]->material);
			const Bucket bucket = data.materialBuckets[part.material];
			part.masked = IsMasked(bucket) || IsTranslucent(bucket);
			parts.push_back(std::move(part));
		}
		if (parts.empty()) continue;
		// Up to the impostors: an impostor is not in the structure (its own
		// shadow rays would start inside the plant).
		float distance = std::min(f->draw_distance, kRayFoliageDistance);
		if (f->impostor_distance > 0.f) distance = std::min(distance, f->impostor_distance);
		const float lod0 = LodDistance(f);
		const float lodEnd[2] = {lod0, lod0 * 2.f};
		// Nearest cells first, so the budget keeps what is close.
		std::vector<std::pair<float, const HRL_FoliageCell*>> cells;
		for (const HRL_FoliageCell& cell : f->cells)
		{
			if (cell.instances.empty()) continue;
			const float d = glm::length(glm::clamp(camera, cell.bounds_min, cell.bounds_max) - camera);
			if (d <= distance) cells.push_back({d, &cell});
		}
		std::sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		for (const auto& [d, cell] : cells)
			for (const HRL_FoliageInstanceData& inst : cell->instances)
			{
				const glm::vec3 p(inst.position_scale);
				const glm::quat q(inst.rotation.w, inst.rotation.x, inst.rotation.y, inst.rotation.z);
				const glm::vec3 center = p + q * (f->bounds_center * inst.position_scale.w);
				const float dc = glm::length(center - camera);
				if (dc > distance) continue;
				if (budget < parts.size()) return;
				glm::mat4 model = glm::mat4_cast(q) * inst.position_scale.w;
				model[3] = glm::vec4(p, 1.f);
				// Same level as foliage_cull.comp chooses for the camera.
				int level = 0;
				while (level < 2 && dc >= lodEnd[level]) ++level;
				for (const Part& part : parts)
					AddRayInstance(out, model, part.lods[std::min(level, part.count - 1)], part.material, part.masked);
				budget -= (uint32_t)parts.size();
			}
	}
}

bool GetFoliageStats(HRL_id foliage, HRL_VulkanFoliageStats& stats)
{
	stats = HRL_VulkanFoliageStats{};
	auto it = R.foliage.find(foliage);
	if (it == R.foliage.end()) return false;
	for (int i = 0; i < 3; ++i) stats.lod_instances[i] = it->second.stats[i];
	stats.impostor_instances = it->second.stats[3];
	return true;
}

void ReleaseUnusedFoliage()
{
	const uint64_t frame = Dev().FrameNumber();
	for (auto it = R.foliage.begin(); it != R.foliage.end();)
	{
		if (it->second.lastFrame + 8 < frame)
		{
			DestroyFoliageGPU(it->second);
			it = R.foliage.erase(it);
		}
		else ++it;
	}
}

void DestroyAllFoliage()
{
	for (auto& [id, g] : R.foliage) DestroyFoliageGPU(g);
	R.foliage.clear();
}
} // namespace hrlvk
