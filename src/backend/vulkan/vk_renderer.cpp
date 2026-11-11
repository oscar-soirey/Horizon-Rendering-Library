// HRL native Vulkan backend - scene renderer: render targets, frame graph of a
// scene, picking and screenshots.
#include "vk_renderer.h"
#include "vk_renderer_internal.h"
#include "vk_jobs.h"

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

void DestroySceneTargets(SceneGPU& s);

// Everything a scene owns on the GPU.
void DestroySceneResources(SceneGPU& s)
{
	Device& dev = Dev();
	DestroySceneTargets(s);
	DestroyEnvironment(s.env);
	for (Tlas& t : s.tlas) DestroyTlas(t);
	for (HzbImage& h : s.hzb) DestroyHzb(h);
	s.hzb.clear();
	for (Buffer* b : {&s.instances, &s.records, &s.lodRanges, &s.state, &s.commands, &s.counts}) dev.DestroyBuffer(*b);
	for (SceneGPU::Readback& rb : s.readback)
	{
		dev.DestroyBuffer(rb.counts);
		dev.DestroyBuffer(rb.state);
	}
}

void DestroySceneTargets(SceneGPU& s)
{
	Device& dev = Dev();
	dev.DestroyImage(s.ids);
	dev.DestroyImage(s.depth);
	dev.DestroyImage(s.ldr);
	dev.DestroyImage(s.history[0]);
	dev.DestroyImage(s.history[1]);
	s.historyValid = false;
}

// History images of the temporal anti-aliasing, at the size of the scene.
bool EnsureHistory(SceneGPU& s)
{
	if (s.history[0] && s.history[1]) return true;
	ImageDesc d;
	d.width = s.width;
	d.height = s.height;
	d.format = kHdrFormat;
	d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	s.historyValid = false;
	if (Dev().CreateImage(d, s.history[0], "taa history") && Dev().CreateImage(d, s.history[1], "taa history")) return true;
	Dev().DestroyImage(s.history[0]);
	Dev().DestroyImage(s.history[1]);
	return false;
}

float Halton(uint32_t index, uint32_t base)
{
	float f = 1.f, r = 0.f;
	for (uint32_t i = index; i > 0; i /= base)
	{
		f /= (float)base;
		r += f * (float)(i % base);
	}
	return r;
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

// Depth array of the point light shadow maps: 6 faces per light.
bool EnsurePointShadowMaps(uint32_t resolution)
{
	Device& dev = Dev();
	if (R.pointShadowMaps && R.pointShadowResolution == resolution) return true;
	VkDevice d = dev.device;
	std::vector<VkImageView> old = R.pointShadowLayers;
	dev.Defer([d, old]() { for (VkImageView v : old) vkDestroyImageView(d, v, nullptr); });
	R.pointShadowLayers.clear();
	dev.DestroyImage(R.pointShadowMaps);
	ImageDesc desc;
	desc.format = kDepthFormat;
	desc.width = desc.height = resolution;
	desc.layers = kMaxPointShadows * 6;
	desc.arrayView = true;
	desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (!dev.CreateImage(desc, R.pointShadowMaps, "point light shadow maps")) return false;
	for (uint32_t i = 0; i < desc.layers; ++i)
		R.pointShadowLayers.push_back(dev.CreateView(R.pointShadowMaps, kDepthFormat, VK_IMAGE_VIEW_TYPE_2D, 0, 1, i, 1));
	R.pointShadowResolution = resolution;
	return true;
}

// The 6 faces of a point light, with a margin of 2 texels so the PCF taps of
// a face stay inside it.
void PointShadowViews(const HRL_Light* light, uint32_t index, float resolution, std::vector<ShadowView>& out)
{
	static const glm::vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	static const glm::vec3 ups[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
	const float fov = 2.f * std::atan(1.f + 2.f / resolution);
	for (uint32_t f = 0; f < 6; ++f)
	{
		const glm::mat4 lv = glm::lookAt(light->position_, light->position_ + dirs[f], ups[f]);
		ShadowView sv;
		sv.lightDir = dirs[f];
		sv.viewProj = glm::perspectiveRH_ZO(fov, 1.f, HRL_POINT_SHADOW_NEAR, HRL_POINT_SHADOW_FAR) * lv;
		sv.cullClip = glm::perspective(fov, 1.f, HRL_POINT_SHADOW_NEAR, HRL_POINT_SHADOW_FAR) * lv;
		sv.layer = index * 6 + f;
		sv.point = true;
		out.push_back(sv);
	}
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

// Indirect commands written on the CPU, grouped by bucket.
struct IndirectList
{
	FrameSlice slice;
	uint32_t first[BucketCount]{};
	uint32_t count[BucketCount]{};
	uint32_t total = 0;
	VkDeviceSize countOffset = 0; // counts of the buckets, in the same buffer
};

// Translucent meshes visible from a camera, sorted back to front. They are
// few and their order matters, so they stay on the CPU.
IndirectList BuildTranslucent(SceneData& data, const Camera& cam, const FrustumPlaneSet& frustum,
	const HRL_Viewport* viewport, float viewportHeight)
{
	IndirectList list;
	std::vector<const DrawItem*> selected[BucketCount];
	for (uint32_t index : data.translucentItems)
	{
		DrawItem& item = data.items[index];
		if (!item.noCull && !IsMeshVisible(item.mesh, item.model, frustum)) continue;
		const glm::vec3 center = glm::vec3(item.model * glm::vec4(item.mesh->bounds_center_, 1.f));
		const glm::vec3 delta = center - cam.position;
		item.distance2 = glm::dot(delta, delta);
		int lod = SelectMeshLOD(item.mesh, item.model, cam.view, cam.projGL, viewportHeight, viewport);
		lod = glm::clamp(lod, 0, (int)item.gpu->lods.size() - 1);
		while (lod > 0 && !item.gpu->lods[(size_t)lod].valid) --lod;
		item.lod = (uint32_t)lod;
		const_cast<HRL_Mesh*>(item.mesh)->last_lod_level_ = lod;
		selected[ItemBucket(data, item)].push_back(&item);
	}
	for (uint32_t b = 0; b < BucketCount; ++b)
	{
		// Back to front; sprites at the same distance follow their draw order (HRL_SetSpriteDrawOrder).
		std::stable_sort(selected[b].begin(), selected[b].end(), [](const DrawItem* x, const DrawItem* y)
		{
			if (std::abs(x->distance2 - y->distance2) > 1e-6f) return x->distance2 > y->distance2;
			return x->mesh->draw_order_ < y->mesh->draw_order_;
		});
		list.first[b] = list.total;
		list.count[b] = (uint32_t)selected[b].size();
		list.total += list.count[b];
	}
	if (!list.total) return list;
	// Commands, then the draw count of each bucket (see DrawIndirect).
	const VkDeviceSize commandBytes = VkDeviceSize(list.total) * sizeof(VkDrawIndexedIndirectCommand);
	list.slice = Dev().AllocFrame(commandBytes + BucketCount * sizeof(uint32_t), 16);
	if (!list.slice.ptr) { list = IndirectList{}; return list; }
	list.countOffset = list.slice.offset + commandBytes;
	std::memcpy(static_cast<char*>(list.slice.ptr) + commandBytes, list.count, sizeof(list.count));
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

// Meshes with a custom surface shader: few, drawn from the CPU with their pipelines.
struct CustomDraw
{
	VkPipeline pipeline = VK_NULL_HANDLE;
	uint32_t indexCount = 0, firstIndex = 0;
	int32_t vertexOffset = 0;
	uint32_t instance = 0;
	float distance2 = 0.f;
};
struct CustomDraws { std::vector<CustomDraw> opaque, translucent; };

enum class CustomPass { Forward, Shadow };
CustomDraws CollectCustomDraws(SceneData& data, CustomPass pass, const FrustumPlaneSet& frustum, const Camera& cam,
	const HRL_Viewport* viewport, float viewportHeight)
{
	CustomDraws out;
	for (uint32_t index : data.customItems)
	{
		DrawItem& item = data.items[index];
		const uint32_t bucket = ItemBucket(data, item);
		if (pass == CustomPass::Shadow && IsTranslucent(bucket)) continue; // no shadow, as the built-in material
		if (!item.noCull && !IsMeshVisible(item.mesh, item.model, frustum)) continue;
		int lod = SelectMeshLOD(item.mesh, item.model, cam.view, cam.projGL, viewportHeight, viewport);
		lod = glm::clamp(lod, 0, (int)item.gpu->lods.size() - 1);
		while (lod > 0 && !item.gpu->lods[(size_t)lod].valid) --lod;
		const MeshLOD& range = item.gpu->lods[(size_t)lod];
		CustomDraw d;
		d.pipeline = pass == CustomPass::Shadow ? CustomShadowPipeline(item.custom, IsMasked(bucket))
			: CustomForwardPipeline(item.custom, bucket);
		if (!d.pipeline) continue;
		d.indexCount = range.indexCount;
		d.firstIndex = range.firstIndex;
		d.vertexOffset = (int32_t)range.vertexOffset;
		d.instance = item.instance;
		const glm::vec3 center = glm::vec3(item.model * glm::vec4(item.mesh->bounds_center_, 1.f));
		d.distance2 = glm::dot(center - cam.position, center - cam.position);
		(IsTranslucent(bucket) ? out.translucent : out.opaque).push_back(d);
	}
	// Opaque: by pipeline (fewer binds); translucent: back to front.
	std::sort(out.opaque.begin(), out.opaque.end(), [](const CustomDraw& a, const CustomDraw& b) { return a.pipeline < b.pipeline; });
	std::sort(out.translucent.begin(), out.translucent.end(), [](const CustomDraw& a, const CustomDraw& b) { return a.distance2 > b.distance2; });
	return out;
}

// Needs the scene layout bound (view, bindless sets, geometry).
void DrawCustom(VkCommandBuffer cmd, const std::vector<CustomDraw>& draws)
{
	VkPipeline bound = VK_NULL_HANDLE;
	for (const CustomDraw& d : draws)
	{
		if (d.pipeline != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = d.pipeline);
		vkCmdDrawIndexed(cmd, d.indexCount, 1, d.firstIndex, d.vertexOffset, d.instance);
	}
}

void DrawIndirect(VkCommandBuffer cmd, const IndirectList& list, uint32_t bucket)
{
	if (!list.count[bucket]) return;
	// Same entry point as the GPU culled draws. (Mixing it with plain indirect
	// draws in one render pass loses the plain draws on Mesa lavapipe 25.)
	vkCmdDrawIndexedIndirectCount(cmd, list.slice.buffer,
		list.slice.offset + VkDeviceSize(list.first[bucket]) * sizeof(VkDrawIndexedIndirectCommand),
		list.slice.buffer, list.countOffset + bucket * sizeof(uint32_t),
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

// Post-processing settings of a scene, read from the first post-process
// material of its viewports ("exposure" in stops, "bloomStrength").
struct PostSettings
{
	float exposure = 0.f;
	float bloom = 0.f; // share of the blurred image mixed into the scene, 0 : off
	bool toneMap = true; // ACES, unless the material sets toneMappingEnabled to 0
	// Controls of the built-in post-process material (see tonemap.frag).
	struct Grading { glm::vec4 color, tint, vignette, vignetteColor, grain, fadeColor, screen; } grading{
		glm::vec4(1.f, 1.f, 1.f, 0.f), glm::vec4(1.f, 1.f, 1.f, 0.f), glm::vec4(0.f, 0.75f, 0.25f, 0.f),
		glm::vec4(0.f), glm::vec4(0.f, 1.f, 0.f, 0.f), glm::vec4(0.f), glm::vec4(1.f)};
};

PostSettings ScenePostSettings(const HRL_SceneFrame& frame)
{
	PostSettings post;
	for (const auto& [vid, viewport] : frame.scene->viewports)
		if (viewport)
			for (const auto& [pid, pp] : viewport->post_processes)
			{
				if (!pp) continue;
				auto m = frame.materials->find(pp->material_);
				if (m == frame.materials->end() || !m->second) continue;
				if (m->second->shader_ != HRL_DEFAULT_POST_PROCESS_SHADER) continue; // custom: their own pass
				post.exposure = MaterialFloat(m->second, "exposure", 0.f);
				// Energy conserving bloom: 4 % of the image is replaced by its blur
				// at strength 1 (the default of the post-process material).
				post.bloom = glm::clamp(0.04f * MaterialFloat(m->second, "bloomStrength", 1.f), 0.f, 1.f);
				auto toneMapping = m->second->intParams_.find("toneMappingEnabled");
				if (toneMapping != m->second->intParams_.end()) post.toneMap = toneMapping->second != 0;
				const HRL_Material* mat = m->second;
				auto vec3Param = [mat](const char* name, glm::vec3 fallback)
				{
					auto it = mat->vec3Params_.find(name);
					return it == mat->vec3Params_.end() ? fallback : it->second;
				};
				auto intParam = [mat](const char* name)
				{
					auto it = mat->intParams_.find(name);
					return it != mat->intParams_.end() && it->second != 0;
				};
				PostSettings::Grading& g = post.grading;
				g.color = glm::vec4(MaterialFloat(mat, "brightness", 1.f), MaterialFloat(mat, "contrast", 1.f),
					MaterialFloat(mat, "saturation", 1.f), MaterialFloat(mat, "hueShift", 0.f));
				g.tint = glm::vec4(vec3Param("tintColor", glm::vec3(1.f)), intParam("invertColor") ? 1.f : 0.f);
				g.vignette = glm::vec4(MaterialFloat(mat, "vignetteStrength", 0.f), MaterialFloat(mat, "vignetteRadius", 0.75f),
					MaterialFloat(mat, "vignetteSoftness", 0.25f), MaterialFloat(mat, "chromaticAberration", 0.f));
				g.vignetteColor = glm::vec4(vec3Param("vignetteColor", glm::vec3(0.f)), MaterialFloat(mat, "sharpenStrength", 0.f));
				g.grain = glm::vec4(MaterialFloat(mat, "filmGrainStrength", 0.f), MaterialFloat(mat, "filmGrainScale", 1.f),
					HRL_InternalRenderTimeSeconds(), MaterialFloat(mat, "fadeAmount", 0.f));
				const bool display = g.color.y != 1.f || g.color.z != 1.f || g.color.w != 0.f || g.tint != glm::vec4(1.f, 1.f, 1.f, 0.f) ||
					g.vignette.x > 0.f || g.grain.x > 0.f || g.grain.w > 0.f;
				g.fadeColor = glm::vec4(vec3Param("fadeColor", glm::vec3(0.f)), display ? 1.f : 0.f);
				return post;
			}
	return post;
}

// One combined image sampler per binding (4 at most), linear unless `samplers` is given.
VkDescriptorSet MakeSamplerSet(VkDescriptorSetLayout layout, const VkImageView* views, uint32_t count,
	const VkSampler* samplers = nullptr)
{
	const VkDescriptorSet set = AllocFrameSet(layout);
	if (!set) return set;
	VkDescriptorImageInfo infos[4];
	VkWriteDescriptorSet writes[4]{};
	for (uint32_t i = 0; i < count; ++i)
	{
		infos[i] = {samplers ? samplers[i] : R.linearClamp, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &infos[i];
	}
	vkUpdateDescriptorSets(Dev().device, count, writes, 0, nullptr);
	return set;
}

struct BloomPush { float texelX, texelY; uint32_t mode; };

// Adds the bloom passes: the HDR image is downsampled level by level, then the
// levels are upsampled and accumulated back. Returns the half resolution
// result, or `hdr` itself when the image is too small.
FrameGraph::Handle AddBloomPasses(FrameGraph& graph, FrameGraph::Handle hdr, uint32_t width, uint32_t height)
{
	std::vector<FrameGraph::Handle> levels;
	for (uint32_t i = 0; i < kMaxBloomLevels; ++i)
	{
		ImageDesc desc;
		desc.format = kBloomFormat;
		desc.width = width >> (i + 1);
		desc.height = height >> (i + 1);
		if (desc.width < 4 || desc.height < 4) break;
		desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		const FrameGraph::Handle level = graph.CreateTransient(desc, "bloom level");
		if (!graph.GetImage(level).handle) break;
		levels.push_back(level);
	}
	if (levels.empty()) return hdr;

	auto addPass = [&](const char* name, FrameGraph::Handle source, FrameGraph::Handle target, uint32_t mode)
	{
		const Image& src = graph.GetImage(source);
		const Image& dst = graph.GetImage(target);
		const VkDescriptorSet set = MakeSamplerSet(R.samplerLayout, &src.view, 1);
		if (!set) return;
		const BloomPush push{1.f / (float)src.extent.width, 1.f / (float)src.extent.height, mode};
		const float w = (float)dst.extent.width, h = (float)dst.extent.height;
		const VkPipeline pipeline = mode == 2 ? R.bloomUpPipeline : R.bloomDownPipeline;
		graph.AddPass(name, [=](VkCommandBuffer c)
		{
			SetViewport(c, 0, 0, w, h, false);
			vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.bloomLayout, 0, 1, &set, 0, nullptr);
			vkCmdPushConstants(c, R.bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
			vkCmdDraw(c, 3, 1, 0, 0);
		}).Use(source, Access::Sampled)
		  .Color(target, mode == 2 ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	};
	addPass("bloom downsample", hdr, levels[0], 0);
	for (size_t i = 1; i < levels.size(); ++i) addPass("bloom downsample", levels[i - 1], levels[i], 1);
	for (size_t i = levels.size() - 1; i > 0; --i) addPass("bloom upsample", levels[i], levels[i - 1], 2);
	return levels[0];
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
// Everything that casts ray traced shadows: meshes, landscapes, and the
// foliage instances near the camera (kRayFoliageDistance, kRayFoliageBudget).
RayScene CollectRayScene(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame, const glm::vec3& camera)
{
	RayScene out;
	out.table.push_back({0u, 0, 0xFFFFFFFFu, 0u}); // entry 0: opaque geometry
	CollectLandscapeRayInstances(scene, out.instances);
	for (const DrawItem& item : data.items)
	{
		const uint32_t bucket = ItemBucket(data, item);
		// Translucent surfaces and sprites cast no shadow (as with shadow maps).
		if (!item.gpu->blas.handle || IsTranslucent(bucket)) continue;
		AddRayInstance(out, item.model, *item.gpu, item.material, IsMasked(bucket));
	}
	CollectFoliageRayInstances(scene, data, frame, camera, out);
	return out;
}

bool PrepareTlas(Tlas& tlas, const std::vector<VkAccelerationStructureInstanceKHR>& instances)
{
	Device& dev = Dev();
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
	{
		// Bottom level structures built by the upload command buffer of this frame.
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
		mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
		mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
		VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.memoryBarrierCount = 1;
		di.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2(cmd, &di);
	}
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
	// Settings that may be chosen before the initialization.
	const AntiAliasing antiAliasing = R.antiAliasing;
	R = State{};
	R.antiAliasing = antiAliasing;
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

	if (!CreatePipelineCache() || !CreatePipelines()) return false;

	VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
	sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sc.maxLod = VK_LOD_CLAMP_NONE;
	if (vkCreateSampler(dev.device, &sc, nullptr, &R.linearClamp) != VK_SUCCESS) return false;
	sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
	sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	if (vkCreateSampler(dev.device, &sc, nullptr, &R.pointClamp) != VK_SUCCESS) return false;
	sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
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
	if (!EnsureShadowMaps(16, kCascades) || !EnsurePointShadowMaps(16)) return false;

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

	for (auto& [id, s] : R.scenes) DestroySceneResources(s);
	R.scenes.clear();
	DestroyAllLandscapes();
	DestroyAllFoliage();
	DestroyAllGrass();
	DestroyAllVoxels();
	ShutdownGI();
	for (auto& [id, m] : R.meshes)
	{
		DestroyBlas(m.blas);
		dev.DestroyBuffer(m.skinSource);
	}
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
	for (VkImageView v : R.pointShadowLayers) vkDestroyImageView(dev.device, v, nullptr);
	R.pointShadowLayers.clear();
	dev.DestroyImage(R.pointShadowMaps);
	dev.DestroyBuffer(R.vertexBuffer);
	dev.DestroyBuffer(R.indexBuffer);
	R.transients.Clear();
	// The deferred destructors use the renderer state (texture slots, geometry
	// blocks, ray tracing entry points): run them before that state goes away.
	dev.DrainDeletions();
	JobsShutdown();
	if (R.vertexBlock) { vmaClearVirtualBlock(R.vertexBlock); vmaDestroyVirtualBlock(R.vertexBlock); }
	if (R.indexBlock) { vmaClearVirtualBlock(R.indexBlock); vmaDestroyVirtualBlock(R.indexBlock); }

	VkDevice d = dev.device;
	for (auto& [key, s] : R.samplers) vkDestroySampler(d, s, nullptr);
	if (R.linearClamp) vkDestroySampler(d, R.linearClamp, nullptr);
	if (R.pointClamp) vkDestroySampler(d, R.pointClamp, nullptr);
	if (R.shadowSampler) vkDestroySampler(d, R.shadowSampler, nullptr);
	// Every pipeline / layout made by vk_pipelines.cpp is registered for destruction.
	for (VkPipeline p : R.ownedPipelines) vkDestroyPipeline(d, p, nullptr);
	if (R.pipelineCache)
	{
		SavePipelineCache();
		vkDestroyPipelineCache(d, R.pipelineCache, nullptr);
	}
	for (VkPipelineLayout l : R.ownedPipelineLayouts) vkDestroyPipelineLayout(d, l, nullptr);
	if (R.bindlessPool) vkDestroyDescriptorPool(d, R.bindlessPool, nullptr);
	for (VkDescriptorSetLayout l : R.ownedSetLayouts) vkDestroyDescriptorSetLayout(d, l, nullptr);
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
	ReleaseGIScene(id);
	auto it = R.scenes.find(id);
	if (it == R.scenes.end()) return;
	DestroySceneResources(it->second);
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
	// The scene data cache follows the changes of each frame (moved meshes):
	// a frame it does not see forces a rebuild.
	if (!dev.InFrame()) RendererBeginFrame();
	if (!dev.InFrame()) { gpu.data.built = false; return; }

	// Scene images follow the window unless the scene was resized explicitly.
	if (!gpu.explicitSize && (gpu.width != std::max(R.width, 1u) || gpu.height != std::max(R.height, 1u)))
		CreateSceneTargets(gpu, R.width, R.height);
	if (!gpu.ids) { gpu.data.built = false; return; }

	// ---- Scene data ----------------------------------------------------------
	ReadCullingResults(gpu);
	UpdateSceneData(gpu.data, frame);
	SceneData& data = gpu.data;
	const bool rayTracedShadows = R.rtShadows && dev.caps.rayQuery;
	const LightSetup lighting = PackLights(scene, rayTracedShadows);
	if (lighting.shadowResolution)
		EnsureShadowMaps(lighting.shadowResolution, kCascades + (uint32_t)lighting.spotShadowLights.size());
	if (lighting.pointShadowResolution) EnsurePointShadowMaps(lighting.pointShadowResolution);

	// Materials of the landscapes (and other non-mesh geometry) join the table.
	for (const auto& [landscapeId, landscape] : scene->landscapes)
		if (landscape) MaterialIndex(data, frame, landscape->material_);
	ResolveFoliageMaterials(scene, data, frame);
	ResolveVFXMaterials(scene, data, frame);
	// Parameters of the materials with a custom shader: a vec4 table, the
	// first entry of each material in MaterialData::flags.y.
	std::vector<glm::vec4> customParams(1, glm::vec4(0.f));
	for (size_t i = 0; i < data.materials.size(); ++i)
	{
		const HRL_Material* m = data.materialKeys[i].material;
		const CustomShader* custom = m ? FindCustomShader(m->shader_) : nullptr;
		data.materials[i].flags.y = 0;
		if (!custom || !custom->surface || !custom->paramSlots) continue;
		data.materials[i].flags.y = (uint32_t)customParams.size();
		customParams.resize(customParams.size() + std::max(custom->paramSlots, 1u));
		WriteCustomParams(*custom, m, &customParams[data.materials[i].flags.y]);
	}
	const FrameSlice customSlice = dev.AllocFrame(customParams.size() * sizeof(glm::vec4), 256);
	if (!customSlice.ptr) return;
	std::memcpy(customSlice.ptr, customParams.data(), customParams.size() * sizeof(glm::vec4));
	// Global illumination probes (a dummy entry when the scene has none).
	GIFrame gi = PrepareGI(frame.sceneId, scene);
	if (!gi.probes.ptr)
	{
		gi.probes = dev.AllocFrame(sizeof(glm::vec4) * 6, 256);
		if (!gi.probes.ptr) return;
		std::memset(gi.probes.ptr, 0, sizeof(glm::vec4) * 6);
	}
	const FrameSlice materialSlice = dev.AllocFrame(std::max<size_t>(data.materials.size(), 1) * sizeof(MaterialData), 256);
	const FrameSlice lightSlice = dev.AllocFrame(std::max<size_t>(lighting.lights.size(), 1) * sizeof(LightData), 256);
	if (!materialSlice.ptr || !lightSlice.ptr) return;
	if (!data.materials.empty()) std::memcpy(materialSlice.ptr, data.materials.data(), data.materials.size() * sizeof(MaterialData));
	if (!lighting.lights.empty()) std::memcpy(lightSlice.ptr, lighting.lights.data(), lighting.lights.size() * sizeof(LightData));

	// ---- Frame graph ---------------------------------------------------------
	gpu.stats.passes = gpu.stats.skinned = 0; // `drawn` comes from ReadCullingResults
	FrameGraph graph(dev, R.transients);
	ImageDesc hdrDesc;
	hdrDesc.format = kHdrFormat;
	hdrDesc.width = gpu.width;
	hdrDesc.height = gpu.height;
	hdrDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	const FrameGraph::Handle hdr = graph.CreateTransient(hdrDesc, "scene hdr");
	if (!graph.GetImage(hdr).handle) return;
	const FrameGraph::Handle ids = graph.Import(gpu.ids);
	const FrameGraph::Handle depth = graph.Import(gpu.depth);
	const FrameGraph::Handle ldr = graph.Import(gpu.ldr);
	const FrameGraph::Handle shadows = graph.Import(R.shadowMaps);
	const FrameGraph::Handle pointShadows = graph.Import(R.pointShadowMaps);

	if (!R.brdfLutReady)
		graph.AddPass("brdf lut", [](VkCommandBuffer c) { RecordBrdfLut(c); });
	bool environmentDirty = false;
	const bool hasEnvironment = PrepareEnvironment(gpu, scene, environmentDirty);
	if (environmentDirty)
		graph.AddPass("environment", [&gpu, scene](VkCommandBuffer c) { RecordEnvironment(c, gpu, scene); });

	// Skinning: one compute dispatch per skeletal mesh whose pose changed.
	struct SkinJob { VkDescriptorSet set; uint32_t push[3]; };
	std::vector<SkinJob> skinJobs;
	for (uint32_t skinnedIndex : data.skinnedItems)
	{
		const DrawItem& item = data.items[skinnedIndex];
		if (!item.gpu->skinSource || item.mesh->type_ != HRL_3D_SKELETAL_MESH) continue;
		const HRL_SkeletalMesh* skeletal = static_cast<const HRL_SkeletalMesh*>(item.mesh);
		if (skeletal->bone_matrices_.empty() || item.gpu->poseSerial == skeletal->pose_serial_) continue;
		const size_t boneBytes = skeletal->bone_matrices_.size() * sizeof(glm::mat4);
		const FrameSlice bones = dev.AllocFrame(boneBytes, 256);
		const VkDescriptorSet set = AllocFrameSet(R.skinLayout);
		if (!bones.ptr || !set) continue;
		std::memcpy(bones.ptr, skeletal->bone_matrices_.data(), boneBytes);
		const VkDescriptorBufferInfo infos[3] = {
			{item.gpu->skinSource.handle, 0, VK_WHOLE_SIZE},
			{bones.buffer, bones.offset, bones.size},
			// Only the vertex range of this mesh is bound for writing.
			{R.vertexBuffer.handle, VkDeviceSize(item.gpu->lods[0].vertexOffset) * sizeof(HRL_Vertex3D),
				VkDeviceSize(item.gpu->skinVertexCount) * sizeof(HRL_Vertex3D)}};
		VkWriteDescriptorSet writes[3]{};
		for (uint32_t i = 0; i < 3; ++i)
		{
			writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[i].dstSet = set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &infos[i];
		}
		vkUpdateDescriptorSets(dev.device, 3, writes, 0, nullptr);
		skinJobs.push_back({set, {item.gpu->skinVertexCount, 0u, (uint32_t)skeletal->bone_matrices_.size()}});
		item.gpu->poseSerial = skeletal->pose_serial_;
	}
	gpu.stats.skinned = (uint32_t)skinJobs.size();
	if (!skinJobs.empty())
		graph.AddPass("skinning", [skinJobs](VkCommandBuffer c)
		{
			// Earlier draws (previous frame, other scenes) may still read these vertices.
			VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
			mb.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
			mb.srcAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
			mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			mb.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
			VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			di.memoryBarrierCount = 1;
			di.pMemoryBarriers = &mb;
			vkCmdPipelineBarrier2(c, &di);

			vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, R.skinPipeline);
			for (const SkinJob& job : skinJobs)
			{
				vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, R.skinPipelineLayout, 0, 1, &job.set, 0, nullptr);
				vkCmdPushConstants(c, R.skinPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(job.push), job.push);
				vkCmdDispatch(c, (job.push[0] + 63u) / 64u, 1, 1);
			}
			// The skinned vertices are read as vertex attributes by the passes that follow.
			mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
			mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
			mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
			vkCmdPipelineBarrier2(c, &di);
		});

	PrepareLandscapes(scene, graph);
	PrepareVoxels(frame.sceneId, scene);
	const std::vector<FoliageDraw> foliage = PrepareFoliage(scene, data, frame, graph);

	RayScene rayScene;
	rayScene.table.push_back({0u, 0, 0xFFFFFFFFu, 0u});
	// The shaders always declare the acceleration structure: keep a valid one
	// bound, but only rebuild it when shadow rays are actually traced.
	Tlas& tlas = gpu.tlas[dev.FrameNumber() % kFramesInFlight];
	if (dev.caps.rayQuery && (rayTracedShadows || !tlas.handle))
	{
		if (rayTracedShadows)
		{
			// Foliage near the first camera of the scene.
			glm::vec3 camera(0.f);
			for (const auto& [viewportId, viewport] : scene->viewports)
				if (viewport && viewport->camera_) { camera = viewport->camera_->position_; break; }
			rayScene = CollectRayScene(scene, data, frame, camera);
		}
		if (!PrepareTlas(tlas, rayScene.instances)) return;
		graph.AddPass("tlas build", [&tlas](VkCommandBuffer c) { RecordTlasBuild(c, tlas); });
	}

	const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	VkClearColorValue noObject{};
	noObject.uint32[0] = kNoObject;
	bool cleared = false;
	glm::mat4 lastViewProj(1.f);
	float lastRect[4] = {0.f, 0.f, (float)gpu.width, (float)gpu.height};

	// Viewports in creation order (the scene stores them by ascending id).
	// Table of the alpha tested ray tracing instances (lighting.glsl).
	const FrameSlice rayTableSlice = dev.AllocFrame(rayScene.table.size() * sizeof(RtGeometry), 256);
	if (!rayTableSlice.ptr) return;
	std::memcpy(rayTableSlice.ptr, rayScene.table.data(), rayScene.table.size() * sizeof(RtGeometry));

	struct ViewportRect { HRL_id id; const HRL_Viewport* viewport; float x, y, w, h; };
	std::vector<ViewportRect> viewports;
	for (const auto& [viewportId, viewport] : scene->viewports)
	{
		if (!viewport || !viewport->camera_) continue;
		const float vw = std::round(viewport->width_ * (float)gpu.width);
		const float vh = std::round(viewport->height_ * (float)gpu.height);
		const float vx = std::round(viewport->x_ * (float)gpu.width);
		// HRL viewport rectangles have their origin at the bottom left.
		const float vy = (float)gpu.height - std::round(viewport->y_ * (float)gpu.height) - vh;
		if (vw < 1.f || vh < 1.f || vx < 0.f || vy < 0.f || vx + vw > (float)gpu.width || vy + vh > (float)gpu.height) continue;
		viewports.push_back({viewportId, viewport, vx, vy, vw, vh});
	}
	// Culling views of the frame: two per viewport (occlusion phases), then
	// one per shadow map of each viewport.
	const uint32_t shadowViewCount = (lighting.cascadeLight ? kCascades : 0u) + (uint32_t)lighting.spotShadowLights.size();
	const uint32_t mainViewSlots = 2u * (uint32_t)viewports.size();
	// Point light faces do not depend on the camera: rendered once, with the first viewport.
	std::vector<ShadowView> pointViews;
	for (size_t i = 0; i < lighting.pointShadowLights.size(); ++i)
		PointShadowViews(lighting.pointShadowLights[i], (uint32_t)i, (float)R.pointShadowResolution, pointViews);
	if (!PrepareCulling(gpu, mainViewSlots + shadowViewCount * (uint32_t)viewports.size() + (uint32_t)pointViews.size(), graph)) return;
	if (gpu.hzb.size() < viewports.size()) gpu.hzb.resize(viewports.size());
	uint32_t nextShadowSlot = mainViewSlots;
	uint32_t translucentDraws = 0;
	std::vector<OverlayView> overlayViews;
	std::vector<EffectView> effectViews;
	// Temporal anti-aliasing: sub-pixel jitter of every camera, then a resolve per viewport.
	const bool taa = R.antiAliasing == AntiAliasing::Taa;
	struct TaaView { float x, y, w, h; glm::mat4 reproject; bool history; };
	std::vector<TaaView> taaViews;
	const uint32_t taaSample = (uint32_t)(dev.FrameNumber() % kTaaSamples) + 1u;
	const glm::vec2 jitter = taa ? glm::vec2(Halton(taaSample, 2), Halton(taaSample, 3)) - 0.5f : glm::vec2(0.f);

	for (size_t vi = 0; vi < viewports.size(); ++vi)
	{
		const HRL_Viewport* viewport = viewports[vi].viewport;
		const float vx = viewports[vi].x, vy = viewports[vi].y, vw = viewports[vi].w, vh = viewports[vi].h;
		const Camera cam = MakeCamera(viewport->camera_, vw, vh);
		ViewData view{};
		view.view = cam.view;
		view.viewProj = cam.proj * cam.view;
		if (taa)
		{
			// Sub-pixel offset of the whole image (in NDC), different every frame.
			const glm::mat4 offset = glm::translate(glm::mat4(1.f), glm::vec3(jitter.x * 2.f / vw, jitter.y * 2.f / vh, 0.f));
			const glm::mat4 unjittered = view.viewProj;
			view.viewProj = offset * view.viewProj;
			auto previous = gpu.previousViewProj.find(viewports[vi].id);
			const bool history = previous != gpu.previousViewProj.end();
			taaViews.push_back({vx, vy, vw, vh, history ? previous->second * glm::inverse(view.viewProj) : glm::mat4(1.f), history});
			gpu.previousViewProj[viewports[vi].id] = unjittered;
		}
		view.invViewProj = glm::inverse(view.viewProj);
		view.camPos = glm::vec4(cam.position, HRL_InternalRenderTimeSeconds());
		view.viewportSize = glm::vec4(vw, vh, 1.f / vw, 1.f / vh);
		view.shadowInfo = glm::vec4(1.f / (float)std::max(R.shadowResolution, 1u), 0.1f, 1.f / (float)std::max(R.pointShadowResolution, 1u), 0.f);
		for (const ShadowView& pv : pointViews) view.pointViewProj[pv.layer] = pv.viewProj;
		// Light of the emissive voxels around this viewport.
		const VoxelLightField voxelLight = scene->voxel_world ? AddVoxelLightField(graph, frame.sceneId, scene, viewport) : VoxelLightField();
		view.voxelLightRegion = voxelLight.region;
		view.voxelLight = glm::vec4(voxelLight.falloff, voxelLight.active ? 1.f : 0.f, 0.f, 0.f);
		view.giOrigin = gi.origin;
		view.giSpacing = gi.spacing;
		view.giResolution = gi.resolution;
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
		uint32_t interactorCount = 0;
		for (const HRL_FoliageInteractor& it : scene->foliage_interactors)
			if (it.radius > 0.f) view.interactors[interactorCount++] = glm::vec4(it.position, it.radius);
		view.wind = scene->wind;
		view.counts = glm::uvec4((uint32_t)lighting.lights.size(), flags, debugView, interactorCount);

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
			sv.lightDir = dir;
			sv.viewProj = glm::perspectiveRH_ZO(fov, 1.f, 0.1f, 100.f) * lv;
			sv.cullClip = glm::perspective(fov, 1.f, 0.1f, 100.f) * lv;
			sv.layer = kCascades + (uint32_t)i;
			view.spotViewProj[i] = sv.viewProj;
			shadowViews.push_back(sv);
		}
		if (vi == 0) shadowViews.insert(shadowViews.end(), pointViews.begin(), pointViews.end());

		// View descriptor set (makeViewSet: also with another instance table, for the mesh particles).
		const FrameSlice viewSlice = dev.AllocFrame(sizeof(ViewData), 256);
		if (!viewSlice.ptr) continue;
		std::memcpy(viewSlice.ptr, &view, sizeof(view));
		auto makeViewSet = [&](const VkDescriptorBufferInfo& instanceTable) -> VkDescriptorSet
		{
		const VkDescriptorSet viewSet = AllocFrameSet(R.viewLayout);
		if (!viewSet) return VK_NULL_HANDLE;
		const VkDescriptorBufferInfo buffers[4] = {
			{viewSlice.buffer, viewSlice.offset, sizeof(ViewData)},
			instanceTable,
			{materialSlice.buffer, materialSlice.offset, materialSlice.size},
			{lightSlice.buffer, lightSlice.offset, lightSlice.size}};
		const Environment& env = gpu.env;
		const VkDescriptorImageInfo images[4] = {
			{R.shadowSampler, R.shadowMaps.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{R.linearClamp, hasEnvironment ? env.irradiance.view : R.dummyCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{R.linearClamp, hasEnvironment ? env.prefiltered.view : R.dummyCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{R.linearClamp, R.brdfLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
		VkWriteDescriptorSet writes[16]{};
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
		VkDescriptorBufferInfo rayBuffers[3]{};
		if (dev.caps.rayQuery)
		{
			asWrite.accelerationStructureCount = 1;
			asWrite.pAccelerationStructures = &tlas.handle;
			writes[8] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite};
			writes[8].dstSet = viewSet;
			writes[8].dstBinding = 8;
			writes[8].descriptorCount = 1;
			writes[8].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
			// Triangles of the alpha tested shadow casters.
			rayBuffers[0] = {R.vertexBuffer.handle, 0, VK_WHOLE_SIZE};
			rayBuffers[1] = {R.indexBuffer.handle, 0, VK_WHOLE_SIZE};
			rayBuffers[2] = {rayTableSlice.buffer, rayTableSlice.offset, rayTableSlice.size};
			for (uint32_t i = 0; i < 3; ++i)
			{
				writes[9 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				writes[9 + i].dstSet = viewSet;
				writes[9 + i].dstBinding = 9 + i;
				writes[9 + i].descriptorCount = 1;
				writes[9 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				writes[9 + i].pBufferInfo = &rayBuffers[i];
			}
			writeCount = 12;
		}
		const VkDescriptorImageInfo pointShadowInfo{R.shadowSampler, R.pointShadowMaps.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		writes[writeCount] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		writes[writeCount].dstSet = viewSet;
		writes[writeCount].dstBinding = 12;
		writes[writeCount].descriptorCount = 1;
		writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[writeCount].pImageInfo = &pointShadowInfo;
		++writeCount;
		const VkDescriptorImageInfo voxelLightInfo{R.linearClamp, voxelLight.active ? voxelLight.view : R.black.image.view,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		writes[writeCount] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		writes[writeCount].dstSet = viewSet;
		writes[writeCount].dstBinding = 15;
		writes[writeCount].descriptorCount = 1;
		writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[writeCount].pImageInfo = &voxelLightInfo;
		++writeCount;
		const VkDescriptorBufferInfo extraBuffers[2] = {{customSlice.buffer, customSlice.offset, customSlice.size},
			{gi.probes.buffer, gi.probes.offset, gi.probes.size}};
		for (uint32_t e = 0; e < 2; ++e)
		{
			writes[writeCount] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[writeCount].dstSet = viewSet;
			writes[writeCount].dstBinding = 13 + e; // custom material parameters, GI probes
			writes[writeCount].descriptorCount = 1;
			writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[writeCount].pBufferInfo = &extraBuffers[e];
			++writeCount;
		}
		vkUpdateDescriptorSets(dev.device, writeCount, writes, 0, nullptr);
		return viewSet;
		};
		const VkDescriptorSet viewSet = makeViewSet({gpu.instances.handle, 0, VK_WHOLE_SIZE});
		if (!viewSet) continue;
		if (vi == 0) PrepareImpostors(scene, data, frame, graph, viewSet);

		// ---- Landscapes: LOD follows this camera in every view of the viewport ----
		const std::vector<LandscapeDraw> landscapes = PrepareLandscapeDraws(scene, data, frame, cam.position);
		struct LandscapeView { LandscapeDraw draw; LandscapeNodes nodes; };
		auto selectLandscapes = [&](const FrustumPlaneSet& frustum)
		{
			std::vector<LandscapeView> out;
			for (const LandscapeDraw& d : landscapes)
			{
				const LandscapeNodes nodes = SelectLandscapeNodes(d, cam.position, frustum);
				if (nodes.count) out.push_back({d, nodes});
			}
			return out;
		};
		auto drawLandscapes = [viewSet](VkCommandBuffer c, const std::vector<LandscapeView>& list, bool shadow)
		{
			if (list.empty()) return;
			vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow ? R.landscapeShadowPipeline : R.landscapePipeline);
			const VkDescriptorSet sets[2] = {viewSet, R.bindlessSet};
			vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.landscapeLayout, 0, 2, sets, 0, nullptr);
			for (const LandscapeView& v : list) DrawLandscape(c, v.draw, v.nodes);
			// Back to the mesh geometry and layout.
			SetGeometryState(c, viewSet);
		};

		// ---- Culling, phase 1: LOD of every mesh, meshes visible last frame ----
		const bool occlusion = R.occlusionCulling && vi < 24;
		CullRequest request;
		request.viewProj = view.viewProj;
		request.frustum = BuildFrustum(cam.projGL * cam.view);
		request.camPos = cam.position;
		request.camForward = GetForwardVector(viewport->camera_->rotation_);
		request.projScale = std::abs(cam.projGL[1][1]);
		request.flags = cam.perspective ? CullFlagPerspective : 0u;
		request.phase = occlusion ? 1u : 0u;
		request.viewportBit = occlusion ? 1u << (kStateVisibleShift + (uint32_t)vi) : 0u;
		const CulledList phase1 = AddCullPass(graph, gpu, 2u * (uint32_t)vi, request, 0);

		const std::vector<GrassDraw> grass = PrepareGrass(scene, cam.position);

		// ---- Shadow maps: one pass per cascade / spot light, culled for the light ----
		for (const ShadowView& sv : shadowViews)
		{
			const float shadowSize = (float)(sv.point ? R.pointShadowResolution : R.shadowResolution);
			CullRequest shadowRequest;
			shadowRequest.viewProj = sv.viewProj;
			shadowRequest.frustum = BuildFrustum(sv.cullClip);
			shadowRequest.flags = CullFlagShadow;
			// Translucent surfaces cast no shadow: they are not in the culled buckets.
			const CulledList casters = AddCullPass(graph, gpu, nextShadowSlot++, shadowRequest, 0);
			const std::vector<LandscapeView> landscapeCasters = selectLandscapes(shadowRequest.frustum);
			std::vector<FoliageDraw> foliageCasters;
			for (const FoliageDraw& fd : foliage)
				if (fd.foliage->cast_shadows)
				{
					AddFoliageCull(graph, fd, shadowRequest, cam.position, 0);
					foliageCasters.push_back(fd);
				}
			// Grass shadows (when enabled) only in the two nearest cascades: further away they are below a texel.
			std::vector<GrassDraw> grassCasters;
			if (sv.layer < 2 && !sv.point)
				for (const GrassDraw& gd : grass)
					if (gd.grass->cast_shadows)
					{
						AddGrassCull(graph, gd, shadowRequest, cam.position, 0);
						grassCasters.push_back(gd);
					}
			const glm::mat4 lightViewProj = sv.viewProj;
			const glm::vec3 lightDir = sv.lightDir;
			const std::vector<CustomDraw> customCasters = data.customItems.empty() ? std::vector<CustomDraw>() :
				CollectCustomDraws(data, CustomPass::Shadow, shadowRequest.frustum, cam, viewport, vh).opaque;
			graph.AddPass("shadow map", [=, &gpu](VkCommandBuffer c)
			{
				if (casters.empty && landscapeCasters.empty() && foliageCasters.empty() && grassCasters.empty() && customCasters.empty()) return;
				SetViewport(c, 0, 0, shadowSize, shadowSize, false);
				vkCmdSetDepthBias(c, 1.5f, 0.f, 2.f);
				if (!landscapeCasters.empty())
				{
					vkCmdPushConstants(c, R.landscapeLayout, VF, 0, sizeof(glm::mat4), &lightViewProj);
					drawLandscapes(c, landscapeCasters, true);
				}
				SetGeometryState(c, viewSet);
				vkCmdPushConstants(c, R.sceneLayout, VF, 0, sizeof(glm::mat4), &lightViewProj);
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.shadowPipeline);
				DrawCulled(c, gpu, casters, BucketOpaque);
				DrawCulled(c, gpu, casters, BucketOpaqueTwoSided);
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.shadowMaskedPipeline);
				DrawCulled(c, gpu, casters, BucketMasked);
				DrawCulled(c, gpu, casters, BucketMaskedTwoSided);
				for (const FoliageDraw& fd : foliageCasters) DrawFoliage(c, fd, true, lightViewProj, lightDir);
				for (const GrassDraw& gd : grassCasters) DrawGrass(c, gd, true, lightViewProj);
				if (!customCasters.empty())
				{
					SetGeometryState(c, viewSet);
					vkCmdPushConstants(c, R.sceneLayout, VF, 0, sizeof(glm::mat4), &lightViewProj);
					DrawCustom(c, customCasters);
				}
			}).Depth(sv.point ? pointShadows : shadows, VK_ATTACHMENT_LOAD_OP_CLEAR, 1.f,
				sv.point ? R.pointShadowLayers[sv.layer] : R.shadowLayers[sv.layer]);
		}

		// ---- Translucent meshes: few, sorted back to front on the CPU ----
		const IndirectList translucent = BuildTranslucent(data, cam, request.frustum, viewport, vh);
		translucentDraws += translucent.total;

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
		auto drawOpaque = [&gpu](VkCommandBuffer c, const CulledList& list)
		{
			for (uint32_t b = 0; b < kCulledBuckets; ++b)
			{
				if (list.empty || !list.capacity[b]) continue;
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.meshPipelines[b]);
				DrawCulled(c, gpu, list, b);
			}
		};
		const CustomDraws custom = data.customItems.empty() ? CustomDraws() :
			CollectCustomDraws(data, CustomPass::Forward, request.frustum, cam, viewport, vh);
		VFXFrame vfx = PrepareVFX(scene, data, frame, cam);
		if (!vfx.meshes.empty())
			vfx.meshViewSet = makeViewSet({vfx.meshInstances.buffer, vfx.meshInstances.offset, vfx.meshInstances.size});
		auto drawEnd = [=](VkCommandBuffer c)
		{
			if (drawSky)
			{
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.skyPipeline);
				vkCmdPushConstants(c, R.sceneLayout, VF, 0, sizeof(sky), &sky);
				vkCmdDraw(c, 3, 1, 0, 0);
			}
			// Translucent surfaces blend over the opaque scene and the sky.
			for (uint32_t b = BucketTranslucent; b < BucketCount; ++b)
			{
				if (!translucent.count[b]) continue;
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.meshPipelines[b]);
				DrawIndirect(c, translucent, b);
			}
			DrawCustom(c, custom.translucent);
			DrawVFX(c, vfx, viewSet);
		};

		const std::vector<LandscapeView> visibleLandscapes = selectLandscapes(request.frustum);
		const VoxelDraws voxels = scene->voxel_world ? PrepareVoxelDraws(frame.sceneId, scene, request.frustum) : VoxelDraws();
		auto drawFoliage = [foliage](VkCommandBuffer c)
		{
			for (const FoliageDraw& fd : foliage) DrawFoliage(c, fd, false, glm::mat4(1.f), glm::vec3(0.f));
		};
		auto drawGrass = [grass](VkCommandBuffer c)
		{
			for (const GrassDraw& gd : grass) DrawGrass(c, gd, false, glm::mat4(1.f));
		};
		if (!occlusion)
		{
			for (const FoliageDraw& fd : foliage) AddFoliageCull(graph, fd, request, cam.position, 0, vi == 0);
			for (const GrassDraw& gd : grass) AddGrassCull(graph, gd, request, cam.position, 0);
		}

		// ---- Forward pass (phase 1): HDR colour + object ids ----
		// The first viewport clears the targets; the next ones only reset their
		// own rectangle (they may overlap an earlier viewport).
		const bool clearRectangle = cleared;
		const VkAttachmentLoadOp load = cleared ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
		auto& forwardPass = graph.AddPass("forward", [=](VkCommandBuffer c)
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
			// Voxel worlds and landscapes first: the largest occluders, drawn every frame.
			DrawVoxels(c, voxels, viewSet);
			drawLandscapes(c, visibleLandscapes, false);
			SetGeometryState(c, viewSet);
			drawOpaque(c, phase1);
			DrawCustom(c, custom.opaque);
			if (!occlusion)
			{
				drawFoliage(c);
				drawGrass(c);
				SetGeometryState(c, viewSet);
				drawEnd(c);
			}
		});
		forwardPass.Use(shadows, Access::Sampled).Use(pointShadows, Access::Sampled)
		  .Color(hdr, load)
		  .Color(ids, load, noObject)
		  .Depth(depth, load, 0.f); // reversed-Z: 0 is the far plane
		if (voxelLight.active) forwardPass.Use(voxelLight.image, Access::Sampled);
		cleared = true;

		// ---- Occlusion: HZB of phase 1, phase 2 culling and drawing ----
		if (occlusion)
		{
			const Hzb hzb = AddHzbPasses(graph, gpu.hzb[vi], depth, gpu.depth.view, (uint32_t)vx, (uint32_t)vy, (uint32_t)vw, (uint32_t)vh);
			request.phase = 2;
			if (hzb.mips)
			{
				request.flags |= CullFlagOcclusion;
				request.hzbView = hzb.view;
				request.hzbSize = hzb.size;
				request.hzbMips = hzb.mips;
			}
			const CulledList phase2 = AddCullPass(graph, gpu, 2u * (uint32_t)vi + 1u, request, hzb.image);
			// Foliage is tested against the depth of phase 1 (landscapes, large meshes).
			for (const FoliageDraw& fd : foliage) AddFoliageCull(graph, fd, request, cam.position, hzb.image, vi == 0);
			for (const GrassDraw& gd : grass) AddGrassCull(graph, gd, request, cam.position, hzb.image);
			auto& phase2Pass = graph.AddPass("forward (phase 2)", [=](VkCommandBuffer c)
			{
				SetViewport(c, vx, vy, vw, vh, true);
				SetGeometryState(c, viewSet);
				drawOpaque(c, phase2);
				drawFoliage(c);
				drawGrass(c);
				SetGeometryState(c, viewSet);
				drawEnd(c);
			});
			phase2Pass.Use(shadows, Access::Sampled).Use(pointShadows, Access::Sampled)
			  .Color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD)
			  .Color(ids, VK_ATTACHMENT_LOAD_OP_LOAD)
			  .Depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD);
			if (voxelLight.active) phase2Pass.Use(voxelLight.image, Access::Sampled);
		}

		// ---- Screen space displacement: over a copy of the scene of this viewport ----
		if (!data.displacementItems.empty() && scene->debug_view == HRL_DEBUG_VIEW_NONE)
		{
			struct DisplacementDraw { uint32_t indexCount, firstIndex; int32_t vertexOffset; uint32_t instance; float push[6]; uint32_t texture; float distance2; };
			std::vector<DisplacementDraw> draws;
			for (uint32_t index : data.displacementItems)
			{
				DrawItem& item = data.items[index];
				if (!item.noCull && !IsMeshVisible(item.mesh, item.model, request.frustum)) continue;
				const HRL_Material* m = data.materialKeys[item.material].material;
				if (!m) continue;
				int lod = SelectMeshLOD(item.mesh, item.model, cam.view, cam.projGL, vh, viewport);
				lod = glm::clamp(lod, 0, (int)item.gpu->lods.size() - 1);
				while (lod > 0 && !item.gpu->lods[(size_t)lod].valid) --lod;
				const MeshLOD& range = item.gpu->lods[(size_t)lod];
				DisplacementDraw d{range.indexCount, range.firstIndex, (int32_t)range.vertexOffset, item.instance, {}, 0, 0.f};
				d.texture = TextureSlot(m, HRL_MATERIAL_TEXTURE_SS_DISPLACEMENT_MAPPING, R.flatNormal.slot);
				d.push[0] = 1.f / (float)gpu.width;
				d.push[1] = 1.f / (float)gpu.height;
				d.push[2] = MaterialFloat(m, HRL_MATERIAL_PARAM_SS_DISPLACEMENT_STRENGTH, 12.f);
				d.push[3] = MaterialFloat(m, HRL_MATERIAL_PARAM_SS_DISPLACEMENT_SCALE, 1.f);
				d.push[4] = MaterialFloat(m, HRL_MATERIAL_PARAM_SS_DISPLACEMENT_OPACITY, 1.f);
				const glm::vec3 center = glm::vec3(item.model * glm::vec4(item.mesh->bounds_center_, 1.f));
				d.distance2 = glm::dot(center - cam.position, center - cam.position);
				draws.push_back(d);
			}
			std::stable_sort(draws.begin(), draws.end(), [](const DisplacementDraw& a, const DisplacementDraw& b) { return a.distance2 > b.distance2; });
			ImageDesc copyDesc = hdrDesc;
			copyDesc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			const FrameGraph::Handle copy = draws.empty() ? 0 : graph.CreateTransient(copyDesc, "scene copy (displacement)");
			if (copy && graph.GetImage(copy).handle)
			{
				Image* src = &graph.GetImage(hdr);
				Image* dst = &graph.GetImage(copy);
				const uint32_t w = gpu.width, h = gpu.height;
				graph.AddPass("displacement copy", [=](VkCommandBuffer c)
				{
					VkImageCopy region{};
					region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
					region.extent = {w, h, 1};
					vkCmdCopyImage(c, src->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
				}).Use(hdr, Access::CopySource).Use(copy, Access::CopyDestination);
				const VkImageView copyView = dst->view;
				if (const VkDescriptorSet copySet = MakeSamplerSet(R.samplerLayout, &copyView, 1))
					graph.AddPass("screen space displacement", [=](VkCommandBuffer c)
					{
						SetViewport(c, vx, vy, vw, vh, true);
						vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.displacementPipeline);
						const VkDescriptorSet sets[3] = {viewSet, R.bindlessSet, copySet};
						vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.displacementLayout, 0, 3, sets, 0, nullptr);
						BindGeometry(c);
						for (const DisplacementDraw& d : draws)
						{
							struct { float v[5]; uint32_t texture; } push{{d.push[0], d.push[1], d.push[2], d.push[3], d.push[4]}, d.texture};
							vkCmdPushConstants(c, R.displacementLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24, &push);
							vkCmdDrawIndexed(c, d.indexCount, 1, d.firstIndex, d.vertexOffset, d.instance);
						}
					}).Use(copy, Access::Sampled).Color(hdr, VK_ATTACHMENT_LOAD_OP_LOAD).Color(ids, VK_ATTACHMENT_LOAD_OP_LOAD)
					  .Depth(depth, VK_ATTACHMENT_LOAD_OP_LOAD);
			}
		}

		lastViewProj = view.viewProj;
		lastRect[0] = vx; lastRect[1] = vy; lastRect[2] = vw; lastRect[3] = vh;
		overlayViews.push_back({viewports[vi].id, viewport, vx, vy, vw, vh, view.viewProj, cam.view});
		{
			EffectView ev;
			ev.viewport = viewport;
			ev.x = vx; ev.y = vy; ev.w = vw; ev.h = vh;
			ev.viewProj = view.viewProj;
			ev.viewGL = cam.view;
			ev.projGL = cam.projGL;
			ev.camera = cam.position;
			effectViews.push_back(ev);
		}
		R.lastView = cam.view;
		R.lastProjGL = cam.projGL;
	}
	AddCullingReadback(gpu, graph, mainViewSlots, translucentDraws);

	// No camera: the scene is black and holds no object.
	if (!cleared)
		graph.AddPass("clear", nullptr)
			.Color(hdr, VK_ATTACHMENT_LOAD_OP_CLEAR)
			.Color(ids, VK_ATTACHMENT_LOAD_OP_CLEAR, noObject)
			.Depth(depth, VK_ATTACHMENT_LOAD_OP_CLEAR, 0.f);

	const float fullWidth = (float)gpu.width, fullHeight = (float)gpu.height;
	// Temporal anti-aliasing: this frame blended with the reprojected history.
	FrameGraph::Handle resolved = hdr;
	if (!taa)
	{
		gpu.previousViewProj.clear();
		gpu.historyValid = false;
	}
	else if (cleared && !taaViews.empty() && EnsureHistory(gpu))
	{
		const uint32_t current = gpu.historyIndex ^ 1u;
		const FrameGraph::Handle target = graph.Import(gpu.history[current]);
		const FrameGraph::Handle previous = graph.Import(gpu.history[gpu.historyIndex]);
		const bool reset = !gpu.historyValid;
		// The previous history is undefined before its first frame.
		if (reset)
			graph.AddPass("taa history clear", nullptr).Color(previous, VK_ATTACHMENT_LOAD_OP_CLEAR);
		const VkImageView views[3] = {graph.GetImage(hdr).view, gpu.history[gpu.historyIndex].view, gpu.depth.view};
		const VkSampler samplers[3] = {R.pointClamp, R.linearClamp, R.pointClamp};
		if (const VkDescriptorSet set = MakeSamplerSet(R.taaSetLayout, views, 3, samplers))
		{
			const glm::vec2 rcp(1.f / fullWidth, 1.f / fullHeight);
			graph.AddPass("taa resolve", [=](VkCommandBuffer c)
			{
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.taaPipeline);
				vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.taaLayout, 0, 1, &set, 0, nullptr);
				for (const TaaView& v : taaViews)
				{
					SetViewport(c, v.x, v.y, v.w, v.h, false);
					struct { glm::mat4 reproject; glm::vec4 rect, params; } push{v.reproject, glm::vec4(v.x, v.y, v.w, v.h),
						glm::vec4(rcp, (reset || !v.history) ? 1.f : 0.f, 0.1f)};
					vkCmdPushConstants(c, R.taaLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
					vkCmdDraw(c, 3, 1, 0, 0);
				}
			}).Use(hdr, Access::Sampled).Use(previous, Access::Sampled).Use(depth, Access::Sampled)
			  .Color(target, VK_ATTACHMENT_LOAD_OP_CLEAR);
			resolved = target;
			gpu.historyIndex = current;
			gpu.historyValid = true;
		}
	}

	// Screen space effects and custom post-processes, in HDR, before the tone mapping.
	if (cleared) resolved = AddScreenEffects(graph, gpu, frame, effectViews, resolved, depth);

	// Tone mapping into the LDR scene image (FXAA: into a temporary image first).
	const PostSettings post = ScenePostSettings(frame);
	const FrameGraph::Handle bloom = post.bloom > 0.f ? AddBloomPasses(graph, resolved, gpu.width, gpu.height) : resolved;
	const bool fxaa = R.antiAliasing == AntiAliasing::Fxaa;
	FrameGraph::Handle toneTarget = ldr;
	if (fxaa)
	{
		ImageDesc desc;
		desc.format = R.ldrFormat;
		desc.width = gpu.width;
		desc.height = gpu.height;
		desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		toneTarget = graph.CreateTransient(desc, "tone mapped");
		if (!graph.GetImage(toneTarget).handle) toneTarget = ldr;
	}
	const VkImageView toneViews[2] = {graph.GetImage(resolved).view, graph.GetImage(bloom).view};
	PostSettings::Grading grading = post.grading;
	grading.screen = glm::vec4(fullWidth, fullHeight, 1.f / fullWidth, 1.f / fullHeight);
	const FrameSlice gradingSlice = dev.AllocFrame(sizeof(grading), 256);
	const VkDescriptorSet toneSet = gradingSlice.ptr ? MakeSamplerSet(R.tonemapSetLayout, toneViews, 2) : VK_NULL_HANDLE;
	if (toneSet)
	{
		std::memcpy(gradingSlice.ptr, &grading, sizeof(grading));
		const VkDescriptorBufferInfo gradingInfo{gradingSlice.buffer, gradingSlice.offset, sizeof(grading)};
		VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		write.dstSet = toneSet;
		write.dstBinding = 2;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.pBufferInfo = &gradingInfo;
		vkUpdateDescriptorSets(dev.device, 1, &write, 0, nullptr);
		struct TonePush { float exposure; uint32_t encode; float bloom; uint32_t toneMap; };
		const TonePush push{post.exposure, R.ldrFormat == VK_FORMAT_R8G8B8A8_UNORM ? 1u : 0u, bloom != resolved ? post.bloom : 0.f,
			post.toneMap ? 1u : 0u};
		auto& pass = graph.AddPass("tone mapping", [=](VkCommandBuffer c)
		{
			SetViewport(c, 0, 0, fullWidth, fullHeight, false);
			vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.tonemapPipeline);
			vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.tonemapLayout, 0, 1, &toneSet, 0, nullptr);
			vkCmdPushConstants(c, R.tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
			vkCmdDraw(c, 3, 1, 0, 0);
		});
		pass.Use(resolved, Access::Sampled).Color(toneTarget, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
		if (bloom != resolved) pass.Use(bloom, Access::Sampled);
	}
	if (toneTarget != ldr)
	{
		const VkImageView source = graph.GetImage(toneTarget).view;
		if (const VkDescriptorSet set = MakeSamplerSet(R.samplerLayout, &source, 1))
		{
			const glm::vec2 rcp(1.f / fullWidth, 1.f / fullHeight);
			graph.AddPass("fxaa", [=](VkCommandBuffer c)
			{
				SetViewport(c, 0, 0, fullWidth, fullHeight, false);
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.fxaaPipeline);
				vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.bloomLayout, 0, 1, &set, 0, nullptr);
				vkCmdPushConstants(c, R.bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rcp), &rcp);
				vkCmdDraw(c, 3, 1, 0, 0);
			}).Use(toneTarget, Access::Sampled).Color(ldr, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
		}
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

	// Gizmos, widgets, texts: over the finished image of each viewport.
	if (cleared) AddOverlayPasses(graph, frame, overlayViews, ldr);

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
	ReleaseUnusedLandscapes();
	ReleaseUnusedFoliage();
	ReleaseUnusedGrass();
	ReleaseUnusedVoxels();
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
	if (type)
	{
		*type = R.landscapes.count(id) ? HRL_LANDSCAPE : (R.foliage.count(id) ? HRL_FOLIAGE : HRL_3D_MESH);
		auto item = gpu.data.itemOfMesh.find(id);
		if (item != gpu.data.itemOfMesh.end() && gpu.data.items[item->second].sprite) *type = HRL_SPRITE;
	}
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
	stats.skinned_mesh_count = gpu.stats.skinned;
	return true;
}

void GetViewMatrix(float* out) { if (out) std::memcpy(out, &R.lastView, sizeof(glm::mat4)); }
void GetProjectionMatrix(float* out) { if (out) std::memcpy(out, &R.lastProjGL, sizeof(glm::mat4)); }

bool RayTracingSupported() { return Dev().IsInitialized() && Dev().caps.rayQuery; }
void SetRayTracedShadows(bool enabled) { R.rtShadows = enabled; }
bool RayTracedShadowsEnabled() { return R.rtShadows && RayTracingSupported(); }
void SetShadowDistance(float distance) { if (distance > 0.f) R.shadowDistance = distance; }
void SetOcclusionCulling(bool enabled) { R.occlusionCulling = enabled; }
void SetAntiAliasing(uint32_t mode) { R.antiAliasing = (AntiAliasing)std::min(mode, 2u); }
uint32_t GetAntiAliasing() { return (uint32_t)R.antiAliasing; }
bool OcclusionCulling() { return R.occlusionCulling; }
} // namespace hrlvk
