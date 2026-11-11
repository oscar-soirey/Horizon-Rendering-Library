// HRL native Vulkan backend - 2D voxel worlds.
//
// The chunk geometry is built on the CPU by core/voxel_render.cpp (shared with
// OpenGL) for the chunks the viewports see, rebuilt when the world reports
// them changed (render_dirty_chunks_, revisions), and uploaded to one vertex /
// index buffer pair per chunk. They are drawn in the forward pass before the
// meshes, then the emissive voxels are added in a second pass.
#include "vk_renderer_internal.h"
#include "../../core/voxel_render.h"

#include <array>
#include <unordered_set>

namespace hrlvk
{
namespace
{
struct VoxelSubmesh
{
	Buffer vertices, indices;
	uint32_t indexCount = 0;
};
struct VoxelChunkGPU
{
	VoxelSubmesh regular, emissive;
	uint64_t geometryRevision = 0, colorRevision = 0;
	glm::vec3 center{0.f};
	float radius = 0.f;
};
struct VoxelSceneGPU
{
	std::unordered_map<uint64_t, VoxelChunkGPU> chunks;
	uint64_t lastFrame = 0;
};
std::unordered_map<HRL_id, VoxelSceneGPU> g_voxels;

void DestroySubmesh(VoxelSubmesh& s)
{
	Dev().DestroyBuffer(s.vertices);
	Dev().DestroyBuffer(s.indices);
	s = VoxelSubmesh{};
}

bool UploadSubmesh(VoxelSubmesh& s, const std::vector<HRL_VoxelMeshVertex>& vertices, const std::vector<HRL_uint>& indices)
{
	DestroySubmesh(s);
	if (vertices.empty() || indices.empty()) return true;
	Device& dev = Dev();
	const VkDeviceSize vb = vertices.size() * sizeof(HRL_VoxelMeshVertex), ib = indices.size() * sizeof(HRL_uint);
	const bool ok = dev.CreateBuffer(vb, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, s.vertices, "voxel vertices") &&
		dev.CreateBuffer(ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, s.indices, "voxel indices") &&
		dev.UploadToBuffer(s.vertices, 0, vertices.data(), vb) && dev.UploadToBuffer(s.indices, 0, indices.data(), ib);
	if (!ok) { DestroySubmesh(s); return false; }
	s.indexCount = (uint32_t)indices.size();
	return true;
}

void DestroyChunk(VoxelChunkGPU& c)
{
	DestroySubmesh(c.regular);
	DestroySubmesh(c.emissive);
}

bool RebuildChunk(HRL_VoxelWorld* world, int chunkX, int chunkY, VoxelChunkGPU& gpu)
{
	std::vector<HRL_VoxelMeshVertex> vertices, emissiveVertices;
	std::vector<HRL_uint> indices, emissiveIndices;
	if (!BuildVoxelChunkGeometry(world, chunkX, chunkY, vertices, indices, emissiveVertices, emissiveIndices))
	{
		DestroyChunk(gpu);
		return true; // empty chunk
	}
	if (!UploadSubmesh(gpu.regular, vertices, indices) || !UploadSubmesh(gpu.emissive, emissiveVertices, emissiveIndices))
		return false;
	gpu.geometryRevision = world->geometry_revision_;
	gpu.colorRevision = world->color_revision_;
	// Bounds (the smooth modes overflow by up to one voxel on the neighbours).
	const int cs = world->chunk_size_;
	const int x0 = chunkX * cs, y0 = chunkY * cs;
	const int x1 = std::min(x0 + cs, world->width_), y1 = std::min(y0 + cs, world->height_);
	const float margin = world->render_mode_ == HRL_VOXEL_FLAT ? 0.f : 1.f;
	gpu.center = glm::vec3(0.5f * (float)(x0 + x1) * world->voxel_size_, 0.5f * (float)(y0 + y1) * world->voxel_size_, 0.f);
	const float dx = (0.5f * (float)(x1 - x0) + margin) * world->voxel_size_;
	const float dy = (0.5f * (float)(y1 - y0) + margin) * world->voxel_size_;
	gpu.radius = std::sqrt(dx * dx + dy * dy);
	return true;
}
} // namespace

// Same streaming rules as the OpenGL backend (SyncVoxelChunks).
void PrepareVoxels(HRL_id sceneId, const hrl_scene_t* scene)
{
	HRL_VoxelWorld* world = scene->voxel_world;
	if (!world || world->width_ <= 0 || world->height_ <= 0)
	{
		auto it = g_voxels.find(sceneId);
		if (it != g_voxels.end())
		{
			for (auto& [key, chunk] : it->second.chunks) DestroyChunk(chunk);
			g_voxels.erase(it);
		}
		return;
	}
	VoxelSceneGPU& gpu = g_voxels[sceneId];
	gpu.lastFrame = Dev().FrameNumber();
	std::unordered_set<uint64_t> desired;
	for (const auto& [viewportId, viewport] : scene->viewports)
		CollectVoxelVisibleChunksForViewport(world, viewport, desired);

	for (uint64_t key : desired)
	{
		const int chunkX = VoxelChunkXFromKey(key), chunkY = VoxelChunkYFromKey(key);
		HRL_EnsureVoxelChunkLoaded(world, chunkX, chunkY);
		bool hasSource = world->FindVoxelChunk(chunkX, chunkY) != nullptr;
		if (world->render_mode_ != HRL_VOXEL_FLAT)
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int nx = chunkX + dx, ny = chunkY + dy;
					if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx * world->chunk_size_ >= world->width_ || ny * world->chunk_size_ >= world->height_)
						continue;
					HRL_EnsureVoxelChunkLoaded(world, nx, ny);
					if (world->render_mode_ == HRL_VOXEL_SMOOTH && world->FindVoxelChunk(nx, ny)) hasSource = true;
				}
		auto it = gpu.chunks.find(key);
		if (!hasSource)
		{
			if (it != gpu.chunks.end()) { DestroyChunk(it->second); gpu.chunks.erase(it); }
			continue;
		}
		const bool dirty = world->render_dirty_chunks_.count(key) != 0;
		if (it == gpu.chunks.end() || dirty || it->second.geometryRevision != world->geometry_revision_ ||
			it->second.colorRevision != world->color_revision_)
		{
			VoxelChunkGPU& chunk = gpu.chunks[key];
			if (RebuildChunk(world, chunkX, chunkY, chunk)) world->render_dirty_chunks_.erase(key);
		}
	}
	for (auto it = gpu.chunks.begin(); it != gpu.chunks.end();)
		if (!desired.count(it->first)) { DestroyChunk(it->second); it = gpu.chunks.erase(it); }
		else ++it;
}

VoxelDraws PrepareVoxelDraws(HRL_id sceneId, const hrl_scene_t* scene, const FrustumPlaneSet& frustum)
{
	VoxelDraws out;
	auto it = g_voxels.find(sceneId);
	const HRL_VoxelWorld* world = scene->voxel_world;
	if (it == g_voxels.end() || !world) return out;
	for (const auto& [key, chunk] : it->second.chunks)
	{
		if (!chunk.regular.indexCount && !chunk.emissive.indexCount) continue;
		if (!SphereInsideFrustum(frustum, chunk.center, chunk.radius)) continue;
		if (chunk.regular.indexCount) out.regular.push_back({chunk.regular.vertices.handle, chunk.regular.indices.handle, chunk.regular.indexCount});
		if (chunk.emissive.indexCount) out.emissive.push_back({chunk.emissive.vertices.handle, chunk.emissive.indices.handle, chunk.emissive.indexCount});
	}
	if (out.regular.empty() && out.emissive.empty()) return out;
	// Textures of the types: bindless slot and tile size per texture layer.
	std::vector<glm::uvec2> layers(256, glm::uvec2(R.white.slot, 0x3F800000u));
	for (size_t type = 1; type < world->type_texture_layers_.size(); ++type)
	{
		const int layer = world->type_texture_layers_[type];
		if (layer < 0 || layer >= 256) continue;
		auto texture = R.textures.find(world->type_textures_[type]);
		const float tile = world->type_texture_tiles_[type] * world->voxel_size_;
		uint32_t tileBits = 0;
		std::memcpy(&tileBits, &tile, sizeof(tile));
		layers[(size_t)layer] = glm::uvec2(texture != R.textures.end() ? texture->second.slot : R.white.slot, tileBits);
	}
	const FrameSlice slice = Dev().AllocFrame(layers.size() * sizeof(glm::uvec2), 256);
	out.set = slice.ptr ? AllocFrameSet(R.voxelSetLayout) : VK_NULL_HANDLE;
	if (!out.set) { out.regular.clear(); out.emissive.clear(); return out; }
	std::memcpy(slice.ptr, layers.data(), layers.size() * sizeof(glm::uvec2));
	const VkDescriptorBufferInfo info{slice.buffer, slice.offset, slice.size};
	VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	write.dstSet = out.set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &info;
	vkUpdateDescriptorSets(Dev().device, 1, &write, 0, nullptr);
	return out;
}

void DrawVoxels(VkCommandBuffer cmd, const VoxelDraws& draws, VkDescriptorSet viewSet)
{
	if (draws.regular.empty() && draws.emissive.empty()) return;
	const VkDescriptorSet sets[3] = {viewSet, R.bindlessSet, draws.set};
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.voxelLayout, 0, 3, sets, 0, nullptr);
	auto draw = [&](const std::vector<VoxelDraws::Chunk>& list, VkPipeline pipeline, uint32_t emission)
	{
		if (list.empty()) return;
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdPushConstants(cmd, R.voxelLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(emission), &emission);
		for (const VoxelDraws::Chunk& c : list)
		{
			const VkDeviceSize zero = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &c.vertices, &zero);
			vkCmdBindIndexBuffer(cmd, c.indices, 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexed(cmd, c.indexCount, 1, 0, 0, 0);
		}
	};
	draw(draws.regular, R.voxelPipeline, 0u);
	draw(draws.emissive, R.voxelEmissivePipeline, 1u);
}

// Same construction as the OpenGL backend (BuildVoxelLightField): emission of
// the loaded chunks, box filtered pyramid, weighted sum of the levels.
VoxelLightField AddVoxelLightField(FrameGraph& graph, HRL_id sceneId, const hrl_scene_t* scene, const HRL_Viewport* viewport)
{
	VoxelLightField out;
	const HRL_VoxelWorld* world = scene->voxel_world;
	auto sceneIt = g_voxels.find(sceneId);
	if (!world || !viewport || sceneIt == g_voxels.end() || sceneIt->second.chunks.empty() ||
		scene->debug_view == HRL_DEBUG_VIEW_UNLIT || scene->debug_view == HRL_DEBUG_VIEW_WIREFRAME)
		return out;
	const float reach = VoxelLightReachVoxels(world);
	if (reach <= 0.f || world->voxel_size_ <= 0.f || world->chunk_size_ <= 0) return out;
	float minWX, minWY, maxWX, maxWY;
	if (!ComputeVoxelViewWorldRect(world, viewport, minWX, minWY, maxWX, maxWY)) return out;
	const float vs = world->voxel_size_;
	const float rx0 = minWX / vs - reach, ry0 = minWY / vs - reach;
	const float rx1 = maxWX / vs + reach, ry1 = maxWY / vs + reach;
	if (!(rx1 > rx0) || !(ry1 > ry0) || !std::isfinite(rx0 + ry0 + rx1 + ry1)) return out;
	int texelVoxels = 1;
	while (std::max(rx1 - rx0, ry1 - ry0) / (float)texelVoxels > 1024.f && texelVoxels < 64) texelVoxels *= 2;
	int levels = 1;
	while (levels < 10 && 0.75f * (float)(texelVoxels << (levels - 1)) < reach) ++levels;
	const int coarse = texelVoxels << (levels - 1);
	const int sizeStep = std::max(coarse, texelVoxels * 32);
	const int x0 = (int)std::floor(rx0 / (float)coarse) * coarse;
	const int y0 = (int)std::floor(ry0 / (float)coarse) * coarse;
	const int spanX = std::max((int)std::ceil((rx1 - (float)x0) / (float)sizeStep) * sizeStep, coarse);
	const int spanY = std::max((int)std::ceil((ry1 - (float)y0) / (float)sizeStep) * sizeStep, coarse);
	const int width0 = spanX / texelVoxels, height0 = spanY / texelVoxels;
	if (width0 <= 0 || height0 <= 0 || width0 > 4096 || height0 > 4096) return out;

	const glm::vec4 region((float)x0 * vs, (float)y0 * vs, 1.f / ((float)spanX * vs), 1.f / ((float)spanY * vs));
	// Emissive chunks inside the region.
	std::vector<VoxelDraws::Chunk> emitters;
	const float chunkWorld = (float)world->chunk_size_ * vs;
	for (const auto& [key, chunk] : sceneIt->second.chunks)
	{
		if (!chunk.emissive.indexCount) continue;
		const float cx0 = (float)VoxelChunkXFromKey(key) * chunkWorld - vs, cy0 = (float)VoxelChunkYFromKey(key) * chunkWorld - vs;
		if (cx0 > region.x + 1.f / region.z || cy0 > region.y + 1.f / region.w ||
			cx0 + chunkWorld + 2.f * vs < region.x || cy0 + chunkWorld + 2.f * vs < region.y)
			continue;
		emitters.push_back({chunk.emissive.vertices.handle, chunk.emissive.indices.handle, chunk.emissive.indexCount});
	}
	if (emitters.empty()) return out;

	std::vector<FrameGraph::Handle> down((size_t)levels), up((size_t)levels);
	std::vector<glm::ivec2> sizes((size_t)levels);
	for (int l = 0; l < levels; ++l)
	{
		sizes[(size_t)l] = glm::max(glm::ivec2(width0 >> l, height0 >> l), glm::ivec2(1));
		ImageDesc desc;
		desc.format = kHdrFormat;
		desc.width = (uint32_t)sizes[(size_t)l].x;
		desc.height = (uint32_t)sizes[(size_t)l].y;
		desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		down[(size_t)l] = graph.CreateTransient(desc, "voxel light down");
		up[(size_t)l] = graph.CreateTransient(desc, "voxel light");
		if (!graph.GetImage(down[(size_t)l]).handle || !graph.GetImage(up[(size_t)l]).handle) return out;
	}
	const glm::ivec2 size0 = sizes[0];
	graph.AddPass("voxel light emission", [=](VkCommandBuffer c)
	{
		const VkViewport vp{0.f, 0.f, (float)size0.x, (float)size0.y, 0.f, 1.f};
		const VkRect2D sc{{0, 0}, {(uint32_t)size0.x, (uint32_t)size0.y}};
		vkCmdSetViewport(c, 0, 1, &vp);
		vkCmdSetScissor(c, 0, 1, &sc);
		vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.voxelEmitPipeline);
		vkCmdPushConstants(c, R.voxelEmitLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(region), &region);
		for (const VoxelDraws::Chunk& e : emitters)
		{
			const VkDeviceSize zero = 0;
			vkCmdBindVertexBuffers(c, 0, 1, &e.vertices, &zero);
			vkCmdBindIndexBuffer(c, e.indices, 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexed(c, e.indexCount, 1, 0, 0, 0);
		}
	}).Color(down[0], VK_ATTACHMENT_LOAD_OP_CLEAR);

	struct LightPush { glm::vec2 texel; float weight; uint32_t mode; };
	auto addPass = [&](const char* name, FrameGraph::Handle target, glm::ivec2 size, FrameGraph::Handle current,
		FrameGraph::Handle low, const LightPush& push)
	{
		const VkImageView views[2] = {graph.GetImage(current).view, graph.GetImage(low ? low : current).view};
		const VkDescriptorSet set = AllocFrameSet(R.voxelLightSetLayout);
		if (!set) return;
		VkDescriptorImageInfo infos[2];
		VkWriteDescriptorSet writes[2]{};
		for (uint32_t i = 0; i < 2; ++i)
		{
			infos[i] = {R.linearClamp, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[i].dstSet = set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i].pImageInfo = &infos[i];
		}
		vkUpdateDescriptorSets(Dev().device, 2, writes, 0, nullptr);
		auto& pass = graph.AddPass(name, [=](VkCommandBuffer c)
		{
			const VkViewport vp{0.f, 0.f, (float)size.x, (float)size.y, 0.f, 1.f};
			const VkRect2D sc{{0, 0}, {(uint32_t)size.x, (uint32_t)size.y}};
			vkCmdSetViewport(c, 0, 1, &vp);
			vkCmdSetScissor(c, 0, 1, &sc);
			vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.voxelLightPipeline);
			vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.voxelLightLayout, 0, 1, &set, 0, nullptr);
			vkCmdPushConstants(c, R.voxelLightLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
			vkCmdDraw(c, 3, 1, 0, 0);
		});
		pass.Use(current, Access::Sampled);
		if (low) pass.Use(low, Access::Sampled);
		pass.Color(target, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	};
	for (int l = 1; l < levels; ++l)
	{
		const glm::ivec2 src = sizes[(size_t)l - 1];
		addPass("voxel light pyramid", down[(size_t)l], sizes[(size_t)l], down[(size_t)l - 1], 0,
			{glm::vec2(1.f / (float)src.x, 1.f / (float)src.y), 0.f, 0u});
	}
	const std::array<float, 10>& weights = VoxelLightLevelWeights(world->emissive_light_falloff_, texelVoxels, levels);
	for (int l = levels - 1; l >= 0; --l)
	{
		const bool hasLow = l + 1 < levels;
		const glm::ivec2 low = hasLow ? sizes[(size_t)l + 1] : glm::ivec2(1);
		addPass("voxel light sum", up[(size_t)l], sizes[(size_t)l], down[(size_t)l], hasLow ? up[(size_t)l + 1] : 0,
			{glm::vec2(1.f / (float)low.x, 1.f / (float)low.y), world->emissive_light_intensity_ * weights[(size_t)l], hasLow ? 1u : 2u});
	}
	out.image = up[0];
	out.view = graph.GetImage(up[0]).view;
	out.region = region;
	out.falloff = world->emissive_light_falloff_ * vs;
	out.active = true;
	return out;
}

void ReleaseUnusedVoxels()
{
	const uint64_t frame = Dev().FrameNumber();
	for (auto it = g_voxels.begin(); it != g_voxels.end();)
		if (it->second.lastFrame + 8 < frame)
		{
			for (auto& [key, chunk] : it->second.chunks) DestroyChunk(chunk);
			it = g_voxels.erase(it);
		}
		else ++it;
}

void DestroyAllVoxels()
{
	for (auto& [scene, gpu] : g_voxels)
		for (auto& [key, chunk] : gpu.chunks) DestroyChunk(chunk);
	g_voxels.clear();
}
} // namespace hrlvk
