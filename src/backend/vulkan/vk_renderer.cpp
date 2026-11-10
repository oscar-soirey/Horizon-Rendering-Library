// HRL native Vulkan backend - scene renderer: render targets, frame graph of a
// scene, picking and screenshots.
#include "vk_renderer.h"
#include "vk_renderer_internal.h"

namespace hrlvk
{
State R;

namespace
{
// ---------------------------------------------------------------------------
// Scene render targets
// ---------------------------------------------------------------------------
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
	d.format = kIdFormat;
	d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	bool ok = dev.CreateImage(d, s.ids, "scene ids");
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
// Draw submission
// ---------------------------------------------------------------------------
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
IndirectList BuildIndirect(const SceneData& data, Accept accept)
{
	IndirectList list;
	std::vector<const DrawItem*> selected[BucketCount];
	for (const DrawItem& item : data.items)
		if (accept(item)) selected[data.materialBuckets[item.material]].push_back(&item);
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
		R.screenshots.erase(R.screenshots.begin() + (std::ptrdiff_t)i);
	}
}

void SetGeometryState(VkCommandBuffer cmd, VkDescriptorSet viewSet)
{
	const VkDescriptorSet sets[2] = {viewSet, R.bindlessSet};
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.sceneLayout, 0, 2, sets, 0, nullptr);
	BindGeometry(cmd);
}

// The exposure of the first post-process material of the scene drives the tone mapper.
float SceneExposure(const HRL_SceneFrame& frame)
{
	for (const auto& [vid, viewport] : frame.scene->viewports)
		if (viewport)
			for (const auto& [pid, pp] : viewport->post_processes)
			{
				if (!pp) continue;
				auto m = frame.materials->find(pp->material_);
				if (m != frame.materials->end() && m->second)
					return MaterialFloat(m->second, "exposure", 0.f);
			}
	return 0.f;
}

// ---------------------------------------------------------------------------
// Ray tracing: top level acceleration structure of a scene
// ---------------------------------------------------------------------------
void TlasBuildInfo(const Tlas& tlas, VkAccelerationStructureGeometryKHR& geo, VkAccelerationStructureBuildGeometryInfoKHR& info)
{
	geo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
	geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geo.geometry.instances.data.deviceAddress = tlas.instances.address;
	info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geo;
	info.dstAccelerationStructure = tlas.handle;
	if (tlas.scratch) info.scratchData.deviceAddress = AlignUp(tlas.scratch.address, R.scratchAlignment);
}

// Sizes the acceleration structure for the draw list and writes its instance
// buffer. The build is recorded later by RecordTlasBuild.
bool PrepareTlas(Tlas& tlas, const std::vector<DrawItem>& items)
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

	if (!tlas.handle || tlas.capacity < count)
	{
		DestroyTlas(tlas);
		tlas.capacity = std::max(64u, count * 2u);
		VkAccelerationStructureGeometryKHR geo;
		VkAccelerationStructureBuildGeometryInfoKHR info;
		TlasBuildInfo(tlas, geo, info);
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
	tlas.count = count;
	return true;
}

void RecordTlasBuild(VkCommandBuffer cmd, const Tlas& tlas)
{
	VkAccelerationStructureGeometryKHR geo;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	TlasBuildInfo(tlas, geo, info);
	VkAccelerationStructureBuildRangeInfoKHR range{tlas.count, 0, 0, 0};
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
	R.transients.Clear();
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
// Scenes
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Rendering of one scene
//
// The CPU side (scene data, culling, descriptor sets) is prepared at once;
// the GPU commands are declared as frame graph passes and recorded by
// FrameGraph::Execute at the end:
//
//   [brdf lut] [environment] [tlas build]             when needed
//   per viewport: shadow map x N  ->  forward (+ sky)
//   tone mapping -> [debug] -> [screenshot] -> [present copy]
// ---------------------------------------------------------------------------
void RendererRenderScene(const HRL_SceneFrame& frame)
{
	Device& dev = Dev();
	const hrl_scene_t* scene = frame.scene;
	if (!R.initialized || !scene || !frame.materials) return;
	auto sceneIt = R.scenes.find(frame.sceneId);
	if (sceneIt == R.scenes.end()) return;
	SceneGPU& gpu = sceneIt->second;
	if (!dev.InFrame()) RendererBeginFrame();
	if (!dev.InFrame()) return;

	// Scene images follow the window unless the scene was resized explicitly.
	if (!gpu.explicitSize && (gpu.width != std::max(R.width, 1u) || gpu.height != std::max(R.height, 1u)))
		CreateSceneTargets(gpu, R.width, R.height);
	if (!gpu.ids) return;

	// ---- Scene data ----------------------------------------------------------
	UpdateSceneData(gpu.data, frame);
	SceneData& data = gpu.data;
	const bool rayTracedShadows = R.rtShadows && dev.caps.rayQuery;
	const LightSetup lighting = PackLights(scene, rayTracedShadows);
	if (lighting.shadowResolution)
		EnsureShadowMaps(lighting.shadowResolution, kCascades + (uint32_t)lighting.spotShadowLights.size());

	const FrameSlice instanceSlice = dev.AllocFrame(std::max<size_t>(data.instances.size(), 1) * sizeof(InstanceData), 256);
	const FrameSlice materialSlice = dev.AllocFrame(std::max<size_t>(data.materials.size(), 1) * sizeof(MaterialData), 256);
	const FrameSlice lightSlice = dev.AllocFrame(std::max<size_t>(lighting.lights.size(), 1) * sizeof(LightData), 256);
	if (!instanceSlice.ptr || !materialSlice.ptr || !lightSlice.ptr) return;
	if (!data.instances.empty()) std::memcpy(instanceSlice.ptr, data.instances.data(), data.instances.size() * sizeof(InstanceData));
	if (!data.materials.empty()) std::memcpy(materialSlice.ptr, data.materials.data(), data.materials.size() * sizeof(MaterialData));
	if (!lighting.lights.empty()) std::memcpy(lightSlice.ptr, lighting.lights.data(), lighting.lights.size() * sizeof(LightData));

	// ---- Frame graph ---------------------------------------------------------
	FrameGraph graph(dev, R.transients);
	ImageDesc hdrDesc;
	hdrDesc.format = kHdrFormat;
	hdrDesc.width = gpu.width;
	hdrDesc.height = gpu.height;
	hdrDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	const FrameGraph::Handle hdr = graph.CreateTransient(hdrDesc, "scene hdr");
	if (!graph.GetImage(hdr).handle) return;
	const FrameGraph::Handle ids = graph.Import(gpu.ids);
	const FrameGraph::Handle depth = graph.Import(gpu.depth);
	const FrameGraph::Handle ldr = graph.Import(gpu.ldr);
	const FrameGraph::Handle shadows = graph.Import(R.shadowMaps);

	if (!R.brdfLutReady)
		graph.AddPass("brdf lut", [](VkCommandBuffer c) { RecordBrdfLut(c); });
	bool environmentDirty = false;
	const bool hasEnvironment = PrepareEnvironment(gpu, scene, environmentDirty);
	if (environmentDirty)
		graph.AddPass("environment", [&gpu, scene](VkCommandBuffer c) { RecordEnvironment(c, gpu, scene); });

	// The shaders always declare the acceleration structure: keep a valid one
	// bound, but only rebuild it when shadow rays are actually traced.
	Tlas& tlas = gpu.tlas[dev.FrameNumber() % kFramesInFlight];
	if (dev.caps.rayQuery && (rayTracedShadows || !tlas.handle))
	{
		if (!PrepareTlas(tlas, data.items)) return;
		graph.AddPass("tlas build", [&tlas](VkCommandBuffer c) { RecordTlasBuild(c, tlas); });
	}

	const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	VkClearColorValue noObject{};
	noObject.uint32[0] = kNoObject;
	bool cleared = false;
	glm::mat4 lastViewProj(1.f);
	float lastRect[4] = {0.f, 0.f, (float)gpu.width, (float)gpu.height};
	gpu.stats = SceneGPU::Stats{};

	// Viewports in creation order (the scene stores them by ascending id).
	for (const auto& [viewportId, viewport] : scene->viewports)
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
		view.counts = glm::uvec4((uint32_t)lighting.lights.size(), flags, debugView, 0);

		std::vector<ShadowView> shadowViews;
		if (lighting.cascadeLight)
			ComputeCascades(cam, GetLightDirection(lighting.cascadeLight), (float)R.shadowResolution, view, shadowViews);
		for (size_t i = 0; i < lighting.spotShadowLights.size(); ++i)
		{
			const HRL_Light* l = lighting.spotShadowLights[i];
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

		// LOD and distance of every item for this camera.
		const FrustumPlaneSet frustum = BuildFrustum(cam.projGL * cam.view);
		for (DrawItem& item : data.items)
		{
			const glm::vec3 center = glm::vec3(item.model * glm::vec4(item.mesh->bounds_center_, 1.f));
			const glm::vec3 delta = center - cam.position;
			item.distance2 = glm::dot(delta, delta);
			int lod = SelectMeshLOD(item.mesh, item.model, cam.view, cam.projGL, vh, viewport);
			lod = glm::clamp(lod, 0, (int)item.gpu->lods.size() - 1);
			while (lod > 0 && !item.gpu->lods[(size_t)lod].valid) --lod;
			item.lod = (uint32_t)lod;
		}

		// View descriptor set.
		const FrameSlice viewSlice = dev.AllocFrame(sizeof(ViewData), 256);
		const VkDescriptorSet viewSet = AllocFrameSet(R.viewLayout);
		if (!viewSlice.ptr || !viewSet) continue;
		std::memcpy(viewSlice.ptr, &view, sizeof(view));
		const VkDescriptorBufferInfo buffers[4] = {
			{viewSlice.buffer, viewSlice.offset, sizeof(ViewData)},
			{instanceSlice.buffer, instanceSlice.offset, instanceSlice.size},
			{materialSlice.buffer, materialSlice.offset, materialSlice.size},
			{lightSlice.buffer, lightSlice.offset, lightSlice.size}};
		const Environment& env = gpu.env;
		const VkDescriptorImageInfo images[4] = {
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

		// Shadow maps: one pass per cascade / spot light, culled for the light.
		const float shadowSize = (float)R.shadowResolution;
		for (const ShadowView& sv : shadowViews)
		{
			const FrustumPlaneSet lightFrustum = BuildFrustum(sv.cullClip);
			const IndirectList list = BuildIndirect(data, [&](const DrawItem& item)
				{ return IsMeshVisible(item.mesh, item.model, lightFrustum); });
			const glm::mat4 lightViewProj = sv.viewProj;
			graph.AddPass("shadow map", [=](VkCommandBuffer c)
			{
				if (!list.total) return;
				SetViewport(c, 0, 0, shadowSize, shadowSize, false);
				vkCmdSetDepthBias(c, 1.5f, 0.f, 2.f);
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.shadowPipeline);
				SetGeometryState(c, viewSet);
				vkCmdPushConstants(c, R.sceneLayout, VF, 0, sizeof(glm::mat4), &lightViewProj);
				for (uint32_t b = 0; b < BucketCount; ++b) DrawIndirect(c, list, b);
			}).Depth(shadows, VK_ATTACHMENT_LOAD_OP_CLEAR, 1.f, R.shadowLayers[sv.layer]);
		}

		// Forward pass: HDR colour + object ids, then the sky where nothing was drawn.
		const IndirectList visible = BuildIndirect(data, [&](const DrawItem& item)
			{ return IsMeshVisible(item.mesh, item.model, frustum); });
		gpu.stats.drawn += visible.total;
		SkyPush sky{};
		const bool drawSky = scene->sky_sphere_enabled;
		if (drawSky)
		{
			sky.rotation = SkyRotation(scene->sky_rotation);
			sky.top = glm::vec4(scene->sky_top_color, 1.f);
			sky.horizon = glm::vec4(scene->sky_horizon_color, 1.f);
			sky.bottom = glm::vec4(scene->sky_bottom_color, 1.f);
			auto texture = R.textures.find(scene->sky_texture);
			sky.texture = texture != R.textures.end() ? texture->second.slot : 0xFFFFFFFFu;
		}
		// The first viewport clears the targets; the next ones only reset their
		// own rectangle (they may overlap an earlier viewport).
		const bool clearRectangle = cleared;
		const VkAttachmentLoadOp load = cleared ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
		graph.AddPass("forward", [=](VkCommandBuffer c)
		{
			SetViewport(c, vx, vy, vw, vh, true);
			if (clearRectangle)
			{
				VkClearAttachment clears[2]{};
				clears[0].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
				clears[0].clearValue.depthStencil = {0.f, 0};
				clears[1].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				clears[1].colorAttachment = 1;
				clears[1].clearValue.color = noObject;
				const VkClearRect rect{{{(int32_t)vx, (int32_t)vy}, {(uint32_t)vw, (uint32_t)vh}}, 0, 1};
				vkCmdClearAttachments(c, 2, clears, 1, &rect);
			}
			SetGeometryState(c, viewSet);
			for (uint32_t b = 0; b < BucketCount; ++b)
			{
				if (!visible.count[b]) continue;
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.meshPipelines[b]);
				DrawIndirect(c, visible, b);
			}
			if (drawSky)
			{
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.skyPipeline);
				vkCmdPushConstants(c, R.sceneLayout, VF, 0, sizeof(sky), &sky);
				vkCmdDraw(c, 3, 1, 0, 0);
			}
		}).Use(shadows, Access::Sampled)
		  .Color(hdr, load)
		  .Color(ids, load, noObject)
		  .Depth(depth, load, 0.f); // reversed-Z: 0 is the far plane
		cleared = true;

		lastViewProj = view.viewProj;
		lastRect[0] = vx; lastRect[1] = vy; lastRect[2] = vw; lastRect[3] = vh;
		R.lastView = cam.view;
		R.lastProjGL = cam.projGL;
	}

	// No camera: the scene is black and holds no object.
	if (!cleared)
		graph.AddPass("clear", nullptr)
			.Color(hdr, VK_ATTACHMENT_LOAD_OP_CLEAR)
			.Color(ids, VK_ATTACHMENT_LOAD_OP_CLEAR, noObject)
			.Depth(depth, VK_ATTACHMENT_LOAD_OP_CLEAR, 0.f);

	// Tone mapping into the LDR scene image.
	const float fullWidth = (float)gpu.width, fullHeight = (float)gpu.height;
	if (const VkDescriptorSet toneSet = AllocFrameSet(R.samplerLayout))
	{
		const VkDescriptorImageInfo source{R.linearClamp, graph.GetImage(hdr).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = toneSet;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &source;
		vkUpdateDescriptorSets(dev.device, 1, &w, 0, nullptr);
		struct TonePush { float exposure; uint32_t encode; };
		const TonePush push{SceneExposure(frame), R.ldrFormat == VK_FORMAT_R8G8B8A8_UNORM ? 1u : 0u};
		graph.AddPass("tone mapping", [=](VkCommandBuffer c)
		{
			SetViewport(c, 0, 0, fullWidth, fullHeight, false);
			vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.tonemapPipeline);
			vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.tonemapLayout, 0, 1, &toneSet, 0, nullptr);
			vkCmdPushConstants(c, R.tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
			vkCmdDraw(c, 3, 1, 0, 0);
		}).Use(hdr, Access::Sampled).Color(ldr, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	}

	// Debug primitives of the frame, over the tone mapped image, depth tested.
	if (frame.debug && cleared && (!frame.debug->lines.empty() || !frame.debug->triangles.empty()))
	{
		auto upload = [&](const std::vector<DebugVertex>& vertices)
		{
			FrameSlice slice{};
			if (vertices.empty()) return slice;
			slice = dev.AllocFrame(vertices.size() * sizeof(DebugVertex), 16);
			if (slice.ptr) std::memcpy(slice.ptr, vertices.data(), vertices.size() * sizeof(DebugVertex));
			return slice;
		};
		const FrameSlice triangles = upload(frame.debug->triangles), lines = upload(frame.debug->lines);
		const uint32_t triangleCount = (uint32_t)frame.debug->triangles.size(), lineCount = (uint32_t)frame.debug->lines.size();
		const float rx = lastRect[0], ry = lastRect[1], rw = lastRect[2], rh = lastRect[3];
		graph.AddPass("debug", [=](VkCommandBuffer c)
		{
			SetViewport(c, rx, ry, rw, rh, true);
			auto draw = [&](const FrameSlice& slice, uint32_t count, VkPipeline pipeline)
			{
				if (!slice.ptr || !count) return;
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				vkCmdPushConstants(c, R.debugLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &lastViewProj);
				vkCmdBindVertexBuffers(c, 0, 1, &slice.buffer, &slice.offset);
				vkCmdDraw(c, count, 1, 0, 0);
			};
			draw(triangles, triangleCount, R.debugTrianglePipeline);
			draw(lines, lineCount, R.debugLinePipeline);
		}).Color(ldr, VK_ATTACHMENT_LOAD_OP_LOAD).Depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD);
	}

	// Screenshot: copied to a buffer now, written to disk once the GPU is done.
	if (frame.screenshotPath)
	{
		PendingScreenshot shot;
		shot.path = frame.screenshotPath;
		shot.width = gpu.width;
		shot.height = gpu.height;
		shot.frame = dev.FrameNumber();
		if (dev.CreateBuffer(VkDeviceSize(gpu.width) * gpu.height * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, shot.buffer))
		{
			const VkBuffer target = shot.buffer.handle;
			graph.AddPass("screenshot", [=, &gpu](VkCommandBuffer c)
			{
				VkBufferImageCopy region{};
				region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				region.imageExtent = {gpu.width, gpu.height, 1};
				vkCmdCopyImageToBuffer(c, gpu.ldr.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, 1, &region);
			}).Use(ldr, Access::CopySource);
			R.screenshots.push_back(shot);
		}
	}

	// On-screen scenes are copied to the swapchain image.
	if (gpu.onScreen)
		if (Image* back = dev.Backbuffer())
		{
			const FrameGraph::Handle backbuffer = graph.Import(*back);
			graph.AddPass("present copy", [=, &gpu](VkCommandBuffer c)
			{
				VkImageBlit blit{};
				blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				blit.srcOffsets[1] = {(int32_t)gpu.width, (int32_t)gpu.height, 1};
				blit.dstSubresource = blit.srcSubresource;
				blit.dstOffsets[1] = {(int32_t)back->extent.width, (int32_t)back->extent.height, 1};
				const bool sameSize = gpu.width == back->extent.width && gpu.height == back->extent.height;
				vkCmdBlitImage(c, gpu.ldr.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, back->handle,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, sameSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
			}).Use(ldr, Access::CopySource).Use(backbuffer, Access::CopyDestination);
		}

	gpu.stats.passes = (uint32_t)graph.PassCount();
	graph.Execute(dev.Cmd());
	gpu.renderedFrame = dev.FrameNumber();
}

void RendererEndFrame()
{
	Device& dev = Dev();
	if (!R.initialized || !dev.InFrame()) return;
	R.transients.EndFrame();
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

bool GetSceneStats(HRL_id scene, HRL_VulkanSceneStats& stats)
{
	stats = HRL_VulkanSceneStats{};
	auto it = R.scenes.find(scene);
	if (it == R.scenes.end()) return false;
	const SceneGPU& gpu = it->second;
	stats.instance_count = (uint32_t)gpu.data.items.size();
	stats.material_count = (uint32_t)gpu.data.materials.size();
	stats.drawn_instance_count = gpu.stats.drawn;
	stats.render_pass_count = gpu.stats.passes;
	stats.scene_rebuilds = gpu.data.rebuilds;
	stats.transform_updates = gpu.data.transformUpdates;
	stats.material_updates = gpu.data.materialUpdates;
	return true;
}

void GetViewMatrix(float* out) { if (out) std::memcpy(out, &R.lastView, sizeof(glm::mat4)); }
void GetProjectionMatrix(float* out) { if (out) std::memcpy(out, &R.lastProjGL, sizeof(glm::mat4)); }

bool RayTracingSupported() { return Dev().IsInitialized() && Dev().caps.rayQuery; }
void SetRayTracedShadows(bool enabled) { R.rtShadows = enabled; }
bool RayTracedShadowsEnabled() { return R.rtShadows && RayTracingSupported(); }
void SetShadowDistance(float distance) { if (distance > 0.f) R.shadowDistance = distance; }
} // namespace hrlvk
