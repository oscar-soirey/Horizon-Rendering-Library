// HRL native Vulkan backend - GPU culling of the 3D meshes.
//
// The visibility of every mesh is decided on the GPU (cull.comp), which writes
// the indirect draw commands drawn with vkCmdDrawIndexedIndirectCount. Per
// camera view, occlusion culling runs in two phases:
//
//   phase 1  meshes visible in the last frame (and in the frustum) are drawn;
//            the LOD of every mesh is selected and kept in the state buffer
//   HZB      the depth of phase 1 is reduced to a hierarchical depth buffer
//   phase 2  every mesh in the frustum is tested against the HZB; the visible
//            ones not drawn by phase 1 are drawn, and the visibility is stored
//            for the next frame
//
// So a mesh hidden behind others costs nothing but its test, and a mesh that
// appears is drawn in the frame it appears (no one-frame hole). Shadow views
// use the frustum only, with the LOD of the camera.
//
// The instances, records and LOD ranges live in persistent device-local
// buffers: only the entries that changed are copied each frame.
#include "vk_renderer_internal.h"

namespace hrlvk
{
namespace
{
constexpr VkDeviceSize kCountStride = 16; // kCulledBuckets uint32_t per view

bool EnsureBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, const char* name)
{
	if (buffer && buffer.size >= size) return true;
	Dev().DestroyBuffer(buffer);
	return Dev().CreateBuffer(std::max<VkDeviceSize>(size + size / 2, 256), usage, false, buffer, name);
}

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

uint32_t PreviousPowerOfTwo(uint32_t v)
{
	uint32_t p = 1;
	while (p * 2 <= v) p *= 2;
	return p;
}
} // namespace

bool PrepareCulling(SceneGPU& gpu, uint32_t views, FrameGraph& graph)
{
	Device& dev = Dev();
	SceneData& data = gpu.data;
	const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	const size_t itemCount = std::max<size_t>(data.items.size(), 1);
	const bool full = data.uploadAll || gpu.uploadedSerial != data.serial || !gpu.instances ||
		gpu.instances.size < itemCount * sizeof(InstanceData);

	bool ok = EnsureBuffer(gpu.instances, itemCount * sizeof(InstanceData), storage, "scene instances");
	ok = ok && EnsureBuffer(gpu.records, itemCount * sizeof(DrawRecord), storage, "scene draw records");
	ok = ok && EnsureBuffer(gpu.lodRanges, std::max<size_t>(data.lodRanges.size(), 1) * sizeof(LodRange), storage, "scene lod ranges");
	ok = ok && EnsureBuffer(gpu.state, itemCount * sizeof(uint32_t), storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "scene cull state");
	ok = ok && EnsureBuffer(gpu.counts, std::max(views, 1u) * kCountStride,
		storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "scene draw counts");
	const VkDeviceSize commandBytes = VkDeviceSize(std::max(data.culledItems, 1u)) * std::max(views, 1u) * sizeof(VkDrawIndexedIndirectCommand);
	ok = ok && EnsureBuffer(gpu.commands, commandBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, "scene draw commands");
	if (!ok) return false;
	gpu.commandViews = views;

	struct Upload { VkBuffer src; VkBuffer dst; VkBufferCopy region; };
	std::vector<Upload> uploads;
	auto add = [&](const Buffer& dst, VkDeviceSize dstOffset, const void* src, VkDeviceSize size)
	{
		if (!size) return true;
		const FrameSlice slice = dev.AllocFrame(size, 16);
		if (!slice.ptr) return false;
		std::memcpy(slice.ptr, src, (size_t)size);
		uploads.push_back({slice.buffer, dst.handle, {slice.offset, dstOffset, size}});
		return true;
	};

	if (full)
	{
		ok = add(gpu.instances, 0, data.instances.data(), data.instances.size() * sizeof(InstanceData));
		ok = ok && add(gpu.records, 0, data.records.data(), data.records.size() * sizeof(DrawRecord));
		ok = ok && add(gpu.lodRanges, 0, data.lodRanges.data(), data.lodRanges.size() * sizeof(LodRange));
		// Initial state: the LOD the mesh reports, visible in every viewport (the
		// first frame draws everything in the frustum during phase 1).
		std::vector<uint32_t> state(data.items.size());
		for (size_t i = 0; i < data.items.size(); ++i)
		{
			const int last = std::clamp(data.items[i].mesh->last_lod_level_, 0, (int)data.records[i].info.w - 1);
			state[i] = 0xFFFFFF00u | (uint32_t)last;
		}
		ok = ok && add(gpu.state, 0, state.data(), state.size() * sizeof(uint32_t));
		gpu.uploadedSerial = data.serial;
	}
	else if (!data.dirtyInstances.empty())
	{
		// Changed instances, in runs of consecutive indices.
		std::vector<uint32_t>& dirty = data.dirtyInstances;
		std::sort(dirty.begin(), dirty.end());
		dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
		for (size_t i = 0; i < dirty.size() && ok;)
		{
			size_t j = i + 1;
			while (j < dirty.size() && dirty[j] == dirty[j - 1] + 1) ++j;
			ok = add(gpu.instances, VkDeviceSize(dirty[i]) * sizeof(InstanceData), &data.instances[dirty[i]],
				VkDeviceSize(j - i) * sizeof(InstanceData));
			i = j;
		}
	}
	data.dirtyInstances.clear();
	data.uploadAll = !ok; // a failed upload is redone in full
	if (!ok) return false;

	const VkBuffer counts = gpu.counts.handle;
	const VkDeviceSize countBytes = VkDeviceSize(std::max(views, 1u)) * kCountStride;
	graph.AddPass("cull upload", [uploads, counts, countBytes](VkCommandBuffer c)
	{
		// The previous frames may still read these buffers (draws, culling, copies).
		Barrier(c, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		for (const Upload& u : uploads) vkCmdCopyBuffer(c, u.src, u.dst, 1, &u.region);
		vkCmdFillBuffer(c, counts, 0, countBytes, 0);
		Barrier(c, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	});
	return true;
}

CulledList AddCullPass(FrameGraph& graph, SceneGPU& gpu, uint32_t view, const CullRequest& request, FrameGraph::Handle hzb)
{
	Device& dev = Dev();
	const SceneData& data = gpu.data;
	CulledList list;
	list.view = view;
	for (uint32_t b = 0; b < kCulledBuckets; ++b)
	{
		const uint32_t end = b + 1 < kCulledBuckets ? data.bucketBase[b + 1] : data.culledItems;
		list.capacity[b] = end - data.bucketBase[b];
	}
	list.empty = data.culledItems == 0 || view >= gpu.commandViews;
	if (list.empty) return list;

	CullView cv{};
	cv.viewProj = request.viewProj;
	for (int i = 0; i < 6; ++i) cv.planes[i] = request.frustum.planes[i];
	cv.camPos = glm::vec4(request.camPos, request.projScale);
	cv.hzb = glm::vec4(request.hzbSize, (float)std::max(request.hzbMips, 1u), 0.f);
	uint32_t flags = request.flags;
	if (!request.hzbView) flags &= ~CullFlagOcclusion;
	cv.params = glm::uvec4((uint32_t)data.items.size(), request.phase, request.viewportBit, flags);
	const uint32_t viewBase = view * data.culledItems;
	cv.bucketBase = glm::uvec4(viewBase + data.bucketBase[0], viewBase + data.bucketBase[1],
		viewBase + data.bucketBase[2], viewBase + data.bucketBase[3]);
	cv.lodInfo = glm::vec4(request.camForward, 0.f);

	const FrameSlice ubo = dev.AllocFrame(sizeof(CullView), 256);
	const VkDescriptorSet set = AllocFrameSet(R.cullSetLayout);
	if (!ubo.ptr || !set) { list.empty = true; return list; }
	std::memcpy(ubo.ptr, &cv, sizeof cv);
	const VkDescriptorBufferInfo buffers[7] = {
		{gpu.instances.handle, 0, VK_WHOLE_SIZE},
		{gpu.records.handle, 0, VK_WHOLE_SIZE},
		{gpu.lodRanges.handle, 0, VK_WHOLE_SIZE},
		{gpu.state.handle, 0, VK_WHOLE_SIZE},
		{gpu.commands.handle, 0, VK_WHOLE_SIZE},
		{gpu.counts.handle, VkDeviceSize(view) * kCountStride, kCountStride},
		{ubo.buffer, ubo.offset, sizeof(CullView)}};
	const VkDescriptorImageInfo image{R.linearClamp, request.hzbView ? request.hzbView : R.white.image.view,
		request.hzbView ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet writes[8]{};
	for (uint32_t i = 0; i < 8; ++i)
	{
		writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		if (i == 6)
		{
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i].pImageInfo = &image;
		}
		else
		{
			writes[i].descriptorType = i == 7 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &buffers[i < 6 ? i : 6];
		}
	}
	vkUpdateDescriptorSets(dev.device, 8, writes, 0, nullptr);

	const uint32_t groups = ((uint32_t)data.items.size() + 63u) / 64u;
	auto& pass = graph.AddPass("cull", [set, groups](VkCommandBuffer c)
	{
		// Earlier draws read the commands / counts this pass rewrites.
		Barrier(c, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, R.cullPipeline);
		vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, R.cullLayout, 0, 1, &set, 0, nullptr);
		vkCmdDispatch(c, groups, 1, 1);
		Barrier(c, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
			VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
	});
	if (request.hzbView) pass.Use(hzb, Access::Storage);
	return list;
}

void DrawCulled(VkCommandBuffer cmd, const SceneGPU& gpu, const CulledList& list, uint32_t bucket)
{
	if (list.empty || bucket >= kCulledBuckets || !list.capacity[bucket]) return;
	const uint32_t first = list.view * gpu.data.culledItems + gpu.data.bucketBase[bucket];
	vkCmdDrawIndexedIndirectCount(cmd, gpu.commands.handle, VkDeviceSize(first) * sizeof(VkDrawIndexedIndirectCommand),
		gpu.counts.handle, VkDeviceSize(list.view) * kCountStride + bucket * sizeof(uint32_t),
		list.capacity[bucket], sizeof(VkDrawIndexedIndirectCommand));
}

// ---------------------------------------------------------------------------
// Hierarchical depth buffer
// ---------------------------------------------------------------------------
void DestroyHzb(HzbImage& hzb)
{
	Device& dev = Dev();
	if (!hzb.mips.empty())
	{
		VkDevice d = dev.device;
		const std::vector<VkImageView> views = hzb.mips;
		dev.Defer([d, views]() { for (VkImageView v : views) vkDestroyImageView(d, v, nullptr); });
	}
	dev.DestroyImage(hzb.image);
	hzb = HzbImage{};
}

Hzb AddHzbPasses(FrameGraph& graph, HzbImage& target, FrameGraph::Handle depth, VkImageView depthView,
	uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	Device& dev = Dev();
	Hzb hzb;
	const uint32_t w = PreviousPowerOfTwo(std::max(width, 2u));
	const uint32_t h = PreviousPowerOfTwo(std::max(height, 2u));
	uint32_t mips = 1;
	while ((std::max(w, h) >> mips) > 0) ++mips;
	if (!target.image || target.image.extent.width != w || target.image.extent.height != h)
	{
		DestroyHzb(target);
		ImageDesc desc;
		desc.format = VK_FORMAT_R32_SFLOAT;
		desc.width = w;
		desc.height = h;
		desc.mips = mips;
		desc.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		if (!dev.CreateImage(desc, target.image, "hzb")) return hzb;
		for (uint32_t m = 0; m < mips; ++m)
			target.mips.push_back(dev.CreateView(target.image, desc.format, VK_IMAGE_VIEW_TYPE_2D, m, 1, 0, 1));
	}
	Image& image = target.image;
	hzb.image = graph.Import(image);
	const std::vector<VkImageView>* views = &target.mips;

	struct Step { VkDescriptorSet set; int32_t push[7]; uint32_t gx, gy; };
	std::vector<Step> steps;
	for (uint32_t m = 0; m < mips; ++m)
	{
		const uint32_t dw = std::max(w >> m, 1u), dh = std::max(h >> m, 1u);
		const VkDescriptorSet set = AllocFrameSet(R.imageLayout);
		if (!set) return hzb;
		const VkDescriptorImageInfo src = m == 0
			? VkDescriptorImageInfo{R.linearClamp, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
			: VkDescriptorImageInfo{R.linearClamp, image.view, VK_IMAGE_LAYOUT_GENERAL};
		const VkDescriptorImageInfo dst{VK_NULL_HANDLE, (*views)[m], VK_IMAGE_LAYOUT_GENERAL};
		VkWriteDescriptorSet writes[2]{};
		writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		writes[0].dstSet = set;
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[0].pImageInfo = &src;
		writes[1] = writes[0];
		writes[1].dstBinding = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[1].pImageInfo = &dst;
		vkUpdateDescriptorSets(dev.device, 2, writes, 0, nullptr);
		Step s{};
		s.set = set;
		if (m == 0)
		{
			s.push[0] = (int32_t)x; s.push[1] = (int32_t)y;
			s.push[2] = (int32_t)width; s.push[3] = (int32_t)height;
			s.push[6] = 0;
		}
		else
		{
			s.push[2] = (int32_t)std::max(w >> (m - 1), 1u);
			s.push[3] = (int32_t)std::max(h >> (m - 1), 1u);
			s.push[6] = (int32_t)(m - 1);
		}
		s.push[4] = (int32_t)dw;
		s.push[5] = (int32_t)dh;
		s.gx = (dw + 7) / 8;
		s.gy = (dh + 7) / 8;
		steps.push_back(s);
	}

	graph.AddPass("hzb", [steps](VkCommandBuffer c)
	{
		// The image may come from the pool: its last readers are earlier culling passes.
		Barrier(c, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, R.hzbPipeline);
		for (size_t i = 0; i < steps.size(); ++i)
		{
			const Step& s = steps[i];
			if (i)
				Barrier(c, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
					VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
			vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, R.computeLayout, 0, 1, &s.set, 0, nullptr);
			vkCmdPushConstants(c, R.computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(s.push), s.push);
			vkCmdDispatch(c, s.gx, s.gy, 1);
		}
		Barrier(c, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}).Use(depth, Access::Sampled).Use(hzb.image, Access::Storage);

	hzb.view = image.view;
	hzb.size = glm::vec2((float)w, (float)h);
	hzb.mips = mips;
	return hzb;
}

// ---------------------------------------------------------------------------
// Results of earlier frames
// ---------------------------------------------------------------------------
void AddCullingReadback(SceneGPU& gpu, FrameGraph& graph, uint32_t mainViews, uint32_t translucentDraws)
{
	Device& dev = Dev();
	SceneGPU::Readback& rb = gpu.readback[dev.FrameNumber() % kFramesInFlight];
	const VkDeviceSize countBytes = VkDeviceSize(std::max(gpu.commandViews, 1u)) * kCountStride;
	const bool lods = !gpu.data.autoLodItems.empty();
	const VkDeviceSize stateBytes = VkDeviceSize(gpu.data.items.size()) * sizeof(uint32_t);
	auto ensure = [&](Buffer& b, VkDeviceSize size)
	{
		if (b && b.size >= size) return true;
		dev.DestroyBuffer(b);
		return dev.CreateBuffer(std::max<VkDeviceSize>(size, 64), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, b, "cull readback");
	};
	if (!ensure(rb.counts, countBytes) || (lods && stateBytes && !ensure(rb.state, stateBytes))) { rb.serial = 0; return; }
	rb.serial = gpu.data.serial;
	rb.mainViews = mainViews;
	rb.translucent = translucentDraws;
	const VkBuffer counts = gpu.counts.handle, state = gpu.state.handle;
	const VkBuffer countsDst = rb.counts.handle, stateDst = lods && stateBytes ? rb.state.handle : VK_NULL_HANDLE;
	graph.AddPass("cull readback", [=](VkCommandBuffer c)
	{
		Barrier(c, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		const VkBufferCopy countRegion{0, 0, countBytes};
		vkCmdCopyBuffer(c, counts, countsDst, 1, &countRegion);
		if (stateDst)
		{
			const VkBufferCopy stateRegion{0, 0, stateBytes};
			vkCmdCopyBuffer(c, state, stateDst, 1, &stateRegion);
		}
		Barrier(c, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
	});
}

void ReadCullingResults(SceneGPU& gpu)
{
	Device& dev = Dev();
	// This frame slot was waited for by BeginFrame: its last copies are complete.
	SceneGPU::Readback& rb = gpu.readback[dev.FrameNumber() % kFramesInFlight];
	if (!rb.serial) return;
	if (rb.counts.mapped)
	{
		const uint32_t* counts = static_cast<const uint32_t*>(rb.counts.mapped);
		uint32_t drawn = rb.translucent;
		for (uint32_t v = 0; v < rb.mainViews; ++v)
			for (uint32_t b = 0; b < kCulledBuckets; ++b) drawn += counts[v * 4 + b];
		gpu.stats.drawn = drawn;
	}
	// The LOD chosen on the GPU, reported by HRL_GetMeshLODLevel.
	if (rb.serial == gpu.data.serial && rb.state.mapped)
	{
		const uint32_t* state = static_cast<const uint32_t*>(rb.state.mapped);
		for (uint32_t i : gpu.data.autoLodItems)
			const_cast<HRL_Mesh*>(gpu.data.items[i].mesh)->last_lod_level_ = (int)(state[i] & 15u);
	}
	rb.serial = 0;
}
} // namespace hrlvk
