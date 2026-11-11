// HRL native Vulkan backend - landscapes.
//
// Continuous distance-dependent level of detail (CDLOD, F. Strugar 2010):
// the height field is covered by a quadtree whose nodes are drawn with one
// shared patch of kPatch x kPatch quads, instanced once per selected node.
// Node selection runs on the CPU (a few hundred nodes, bounded by the min /
// max heights of each node); in the vertex shader each vertex slides towards
// the grid of the next level as it approaches the end of its LOD range, so
// neighbouring levels meet without cracks and nothing pops.
//
// The heights live in an R32F image sampled through the bindless array; the
// fragment shader computes the normal per pixel from it, so the lighting does
// not change with the LOD. With ray tracing, a decimated copy of the height
// field is in the scene acceleration structure.
#include "vk_renderer_internal.h"

namespace hrlvk
{
namespace
{
constexpr uint32_t kPatch = 32;            // quads per patch side
constexpr uint32_t kMaxRtResolution = 1024; // quads per side of the ray tracing mesh
constexpr float kMorphStart = 0.7f;        // share of a LOD range drawn without morphing

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

bool EnsurePatch()
{
	if (R.landscapePatchVertices) return true;
	Device& dev = Dev();
	std::vector<glm::vec2> vertices;
	for (uint32_t z = 0; z <= kPatch; ++z)
		for (uint32_t x = 0; x <= kPatch; ++x) vertices.emplace_back((float)x, (float)z);
	std::vector<uint32_t> indices;
	for (uint32_t z = 0; z < kPatch; ++z)
		for (uint32_t x = 0; x < kPatch; ++x)
		{
			const uint32_t a = z * (kPatch + 1) + x, b = a + 1, c = a + kPatch + 1, d = c + 1;
			// Same winding as BuildLandscapeGeometry: +Y facing.
			indices.insert(indices.end(), {a, c, b, b, c, d});
		}
	const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (!dev.CreateBuffer(vertices.size() * sizeof(glm::vec2), usage | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false, R.landscapePatchVertices, "landscape patch") ||
		!dev.CreateBuffer(indices.size() * sizeof(uint32_t), usage | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false, R.landscapePatchIndices, "landscape patch indices"))
		return false;
	R.landscapePatchIndexCount = (uint32_t)indices.size();
	return dev.UploadToBuffer(R.landscapePatchVertices, 0, vertices.data(), vertices.size() * sizeof(glm::vec2)) &&
		dev.UploadToBuffer(R.landscapePatchIndices, 0, indices.data(), indices.size() * sizeof(uint32_t));
}

void DestroyLandscapeGPU(LandscapeGPU& g)
{
	Device& dev = Dev();
	FreeBindlessSlot(g.slot);
	dev.DestroyImage(g.heights);
	DestroyBlas(g.blas);
	dev.DestroyBuffer(g.blasVertices);
	dev.DestroyBuffer(g.blasIndices);
	g = LandscapeGPU{};
}

// Min / max height of every quadtree node, level 0 = leaves of kPatch samples.
void UpdateMinMax(LandscapeGPU& g, const HRL_Landscape* l, uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1)
{
	const uint32_t w = g.width, h = g.height;
	if (g.minMax.empty())
	{
		uint32_t size = kPatch, levels = 1;
		while (size < std::max(w, h) - 1) { size *= 2; ++levels; }
		g.levels = levels;
		g.minMax.resize(levels);
		g.dims.resize(levels);
		for (uint32_t level = 0; level < levels; ++level)
		{
			const uint32_t nodeSize = kPatch << level;
			g.dims[level] = glm::uvec2((w - 1 + nodeSize - 1) / nodeSize, (h - 1 + nodeSize - 1) / nodeSize);
			g.dims[level] = glm::max(g.dims[level], glm::uvec2(1));
			g.minMax[level].assign(size_t(g.dims[level].x) * g.dims[level].y, glm::vec2(0.f));
		}
		x0 = z0 = 0;
		x1 = w;
		z1 = h;
	}
	// Leaves touching the changed samples (a leaf covers kPatch + 1 samples).
	const uint32_t lx0 = x0 > 0 ? (x0 - 1) / kPatch : 0, lz0 = z0 > 0 ? (z0 - 1) / kPatch : 0;
	const uint32_t lx1 = std::min((x1 + kPatch - 1) / kPatch, g.dims[0].x), lz1 = std::min((z1 + kPatch - 1) / kPatch, g.dims[0].y);
	for (uint32_t nz = lz0; nz < lz1; ++nz)
		for (uint32_t nx = lx0; nx < lx1; ++nx)
		{
			float lo = std::numeric_limits<float>::max(), hi = -lo;
			for (uint32_t z = nz * kPatch; z <= std::min((nz + 1) * kPatch, h - 1); ++z)
				for (uint32_t x = nx * kPatch; x <= std::min((nx + 1) * kPatch, w - 1); ++x)
				{
					const float v = l->heights_[size_t(z) * w + x];
					lo = std::min(lo, v);
					hi = std::max(hi, v);
				}
			g.minMax[0][size_t(nz) * g.dims[0].x + nx] = glm::vec2(lo, hi);
		}
	uint32_t ax0 = lx0, az0 = lz0, ax1 = lx1, az1 = lz1;
	for (uint32_t level = 1; level < g.levels; ++level)
	{
		ax0 /= 2; az0 /= 2;
		ax1 = std::min((ax1 + 1) / 2, g.dims[level].x);
		az1 = std::min((az1 + 1) / 2, g.dims[level].y);
		const glm::uvec2 child = g.dims[level - 1];
		for (uint32_t nz = az0; nz < az1; ++nz)
			for (uint32_t nx = ax0; nx < ax1; ++nx)
			{
				glm::vec2 r(std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
				for (uint32_t c = 0; c < 4; ++c)
				{
					const uint32_t cx = nx * 2 + (c & 1), cz = nz * 2 + (c >> 1);
					if (cx >= child.x || cz >= child.y) continue;
					const glm::vec2 m = g.minMax[level - 1][size_t(cz) * child.x + cx];
					r = glm::vec2(std::min(r.x, m.x), std::max(r.y, m.y));
				}
				g.minMax[level][size_t(nz) * g.dims[level].x + nx] = r;
			}
	}
}

// Ray tracing mesh: the height field decimated to at most kMaxRtResolution quads per side.
void RebuildLandscapeBlas(LandscapeGPU& g, const HRL_Landscape* l)
{
	Device& dev = Dev();
	DestroyBlas(g.blas);
	dev.DestroyBuffer(g.blasVertices);
	dev.DestroyBuffer(g.blasIndices);
	const uint32_t step = std::max(1u, (std::max(g.width, g.height) - 1 + kMaxRtResolution - 1) / kMaxRtResolution);
	const uint32_t nx = (g.width - 1 + step - 1) / step + 1, nz = (g.height - 1 + step - 1) / step + 1;
	std::vector<glm::vec3> vertices;
	vertices.reserve(size_t(nx) * nz);
	const float cellX = l->size_.x / float(g.width - 1), cellZ = l->size_.y / float(g.height - 1);
	for (uint32_t iz = 0; iz < nz; ++iz)
		for (uint32_t ix = 0; ix < nx; ++ix)
		{
			const uint32_t x = std::min(ix * step, g.width - 1), z = std::min(iz * step, g.height - 1);
			vertices.emplace_back((float)x * cellX - l->size_.x * 0.5f, l->heights_[size_t(z) * g.width + x] * l->height_scale_,
				(float)z * cellZ - l->size_.y * 0.5f);
		}
	std::vector<uint32_t> indices;
	for (uint32_t iz = 0; iz + 1 < nz; ++iz)
		for (uint32_t ix = 0; ix + 1 < nx; ++ix)
		{
			const uint32_t a = iz * nx + ix, b = a + 1, c = a + nx, d = c + 1;
			indices.insert(indices.end(), {a, c, b, b, c, d});
		}
	const VkBufferUsageFlags usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	if (!dev.CreateBuffer(vertices.size() * sizeof(glm::vec3), usage, true, g.blasVertices, "landscape rt vertices") ||
		!dev.CreateBuffer(indices.size() * sizeof(uint32_t), usage, true, g.blasIndices, "landscape rt indices"))
		return;
	std::memcpy(g.blasVertices.mapped, vertices.data(), vertices.size() * sizeof(glm::vec3));
	std::memcpy(g.blasIndices.mapped, indices.data(), indices.size() * sizeof(uint32_t));
	BuildBlas(g.blas, g.blasVertices.address, sizeof(glm::vec3), (uint32_t)vertices.size(),
		g.blasIndices.address, (uint32_t)indices.size(), true);
}

// LOD ranges: the most detailed level reaches `range0`, each next one twice as far.
float LodRange0(const LandscapeGPU& g, const HRL_Landscape* l)
{
	if (l->lod_distance_ > 0.f) return l->lod_distance_;
	const float spacing = std::max(l->size_.x / float(g.width - 1) * l->scale_.x, l->size_.y / float(g.height - 1) * l->scale_.z);
	return spacing * float(kPatch) * 2.5f;
}

struct Box { glm::vec3 lo, hi; };

Box NodeBox(const LandscapeGPU& g, const HRL_Landscape* l, const glm::mat4& model, uint32_t level, uint32_t nx, uint32_t nz)
{
	const uint32_t size = kPatch << level;
	const glm::vec2 mm = g.minMax[level][size_t(nz) * g.dims[level].x + nx];
	const float cellX = l->size_.x / float(g.width - 1), cellZ = l->size_.y / float(g.height - 1);
	const float x0 = (float)(nx * size) * cellX - l->size_.x * 0.5f;
	const float x1 = (float)std::min((nx + 1) * size, g.width - 1) * cellX - l->size_.x * 0.5f;
	const float z0 = (float)(nz * size) * cellZ - l->size_.y * 0.5f;
	const float z1 = (float)std::min((nz + 1) * size, g.height - 1) * cellZ - l->size_.y * 0.5f;
	Box b{glm::vec3(std::numeric_limits<float>::max()), glm::vec3(-std::numeric_limits<float>::max())};
	for (int i = 0; i < 8; ++i)
	{
		const glm::vec3 p((i & 1) ? x1 : x0, ((i & 2) ? mm.y : mm.x) * l->height_scale_, (i & 4) ? z1 : z0);
		const glm::vec3 w = glm::vec3(model * glm::vec4(p, 1.f));
		b.lo = glm::min(b.lo, w);
		b.hi = glm::max(b.hi, w);
	}
	return b;
}

bool BoxInFrustum(const FrustumPlaneSet& f, const Box& b)
{
	for (const glm::vec4& p : f.planes)
	{
		const glm::vec3 v(p.x >= 0.f ? b.hi.x : b.lo.x, p.y >= 0.f ? b.hi.y : b.lo.y, p.z >= 0.f ? b.hi.z : b.lo.z);
		if (glm::dot(glm::vec3(p), v) + p.w < 0.f) return false;
	}
	return true;
}

bool BoxTouchesSphere(const Box& b, const glm::vec3& c, float r)
{
	const glm::vec3 d = glm::max(glm::max(b.lo - c, c - b.hi), glm::vec3(0.f));
	return glm::dot(d, d) <= r * r;
}
} // namespace

// ---------------------------------------------------------------------------
void PrepareLandscapes(const hrl_scene_t* scene, FrameGraph& graph)
{
	Device& dev = Dev();
	if (scene->landscapes.empty() || !EnsurePatch()) return;
	struct Upload { VkBuffer src; VkDeviceSize offset; Image* image; VkBufferImageCopy region; };
	std::vector<Upload> uploads;
	for (const auto& [id, landscapeConst] : scene->landscapes)
	{
		HRL_Landscape* l = const_cast<HRL_Landscape*>(landscapeConst); // the renderer resets dirty_
		if (!l || l->heights_w_ < 2 || l->heights_h_ < 2 || l->heights_.size() < size_t(l->heights_w_) * l->heights_h_) continue;
		LandscapeGPU& g = R.landscapes[id];
		g.lastFrame = dev.FrameNumber();
		bool full = false;
		if (!g.heights || g.width != l->heights_w_ || g.height != l->heights_h_)
		{
			DestroyLandscapeGPU(g);
			g.lastFrame = dev.FrameNumber();
			ImageDesc d;
			d.format = VK_FORMAT_R32_SFLOAT;
			d.width = l->heights_w_;
			d.height = l->heights_h_;
			d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if (!dev.CreateImage(d, g.heights, "landscape heights")) { R.landscapes.erase(id); continue; }
			g.slot = AllocBindlessSlot();
			if (g.slot == kNoSlot) { DestroyLandscapeGPU(g); R.landscapes.erase(id); continue; }
			WriteBindlessView(g.slot, g.heights.view, R.linearClamp);
			g.width = l->heights_w_;
			g.height = l->heights_h_;
			full = true;
		}
		if (!full && g.revision == l->heights_revision_) continue;
		uint32_t x0 = 0, z0 = 0, x1 = g.width, z1 = g.height;
		if (!full && l->dirty_[0] < l->dirty_[2] && l->dirty_[1] < l->dirty_[3])
		{
			x0 = std::min(l->dirty_[0], g.width); z0 = std::min(l->dirty_[1], g.height);
			x1 = std::min(l->dirty_[2], g.width); z1 = std::min(l->dirty_[3], g.height);
		}
		l->dirty_[0] = l->dirty_[1] = l->dirty_[2] = l->dirty_[3] = 0;
		g.revision = l->heights_revision_;
		if (x0 >= x1 || z0 >= z1) continue;
		// Rows of the changed area, staged in this frame's buffer.
		const uint32_t rw = x1 - x0, rh = z1 - z0;
		const FrameSlice slice = dev.AllocFrame(VkDeviceSize(rw) * rh * sizeof(float), 16);
		if (!slice.ptr) continue;
		for (uint32_t z = 0; z < rh; ++z)
			std::memcpy(static_cast<float*>(slice.ptr) + size_t(z) * rw, &l->heights_[size_t(z0 + z) * g.width + x0], rw * sizeof(float));
		VkBufferImageCopy region{};
		region.bufferOffset = slice.offset;
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageOffset = {(int32_t)x0, (int32_t)z0, 0};
		region.imageExtent = {rw, rh, 1};
		uploads.push_back({slice.buffer, slice.offset, &g.heights, region});
		UpdateMinMax(g, l, x0, z0, x1, z1);
		if (dev.caps.rayQuery) g.blasDirty = true;
	}
	// Ray tracing meshes follow the heights (built in the upload command buffer).
	for (const auto& [id, l] : scene->landscapes)
	{
		auto it = R.landscapes.find(id);
		if (it == R.landscapes.end() || !it->second.blasDirty || !R.rtShadows) continue;
		RebuildLandscapeBlas(it->second, l);
		it->second.blasDirty = false;
	}
	if (uploads.empty()) return;
	graph.AddPass("landscape upload", [uploads](VkCommandBuffer c)
	{
		for (const Upload& u : uploads)
		{
			Dev().Transition(c, *u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			vkCmdCopyBufferToImage(c, u.src, u.image->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &u.region);
		}
		for (const Upload& u : uploads) Dev().Transition(c, *u.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		// The heights are read by vertex shaders (Transition targets the fragment / compute stages).
		Barrier(c, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	});
}

void CollectLandscapeRayInstances(const hrl_scene_t* scene, std::vector<VkAccelerationStructureInstanceKHR>& out)
{
	for (const auto& [id, l] : scene->landscapes)
	{
		auto it = R.landscapes.find(id);
		if (!l || it == R.landscapes.end() || !it->second.blas.handle) continue;
		VkAccelerationStructureInstanceKHR inst{};
		const glm::mat4 t = glm::transpose(CalculateLandscapeModelMatrix(l));
		std::memcpy(&inst.transform, &t, sizeof(inst.transform));
		inst.mask = 0xFF;
		inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		inst.accelerationStructureReference = it->second.blas.address;
		out.push_back(inst);
	}
}

std::vector<LandscapeDraw> PrepareLandscapeDraws(const hrl_scene_t* scene, SceneData& data,
	const HRL_SceneFrame& frame, const glm::vec3& camera)
{
	Device& dev = Dev();
	std::vector<LandscapeDraw> draws;
	for (const auto& [id, l] : scene->landscapes)
	{
		auto it = R.landscapes.find(id);
		if (!l || it == R.landscapes.end() || !it->second.heights) continue;
		const LandscapeGPU& g = it->second;
		LandscapeUniform u{};
		u.model = CalculateLandscapeModelMatrix(l);
		const glm::mat3 m3(u.model);
		u.normalMatrix = glm::mat4(glm::transpose(glm::inverse(m3)));
		u.samples = glm::vec4((float)g.width, (float)g.height, 1.f / (float)g.width, 1.f / (float)g.height);
		u.spacing = glm::vec4(l->size_.x / float(g.width - 1), l->size_.y / float(g.height - 1), l->height_scale_, (float)kPatch);
		u.uvScale = glm::vec4(l->uv_scale_, 0.f, 0.f);
		u.morphCamera = glm::vec4(camera, 0.f);
		u.ids = glm::uvec4(MaterialIndex(data, frame, l->material_), (uint32_t)id, g.slot, 0u);
		float range = LodRange0(g, l);
		for (uint32_t level = 0; level < kLandscapeMaxLods; ++level)
		{
			const bool last = level + 1 >= g.levels;
			const float end = last ? 1e30f : range;
			const float start = last ? 1e30f : range * kMorphStart;
			u.morph[level] = glm::vec4(start, 1.f / std::max(end - start, 1e-3f), end, 0.f);
			range *= 2.f;
		}
		const FrameSlice ubo = dev.AllocFrame(sizeof(LandscapeUniform), 256);
		const VkDescriptorSet set = AllocFrameSet(R.landscapeSetLayout);
		if (!ubo.ptr || !set) continue;
		std::memcpy(ubo.ptr, &u, sizeof u);
		const VkDescriptorBufferInfo info{ubo.buffer, ubo.offset, sizeof(LandscapeUniform)};
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = set;
		w.dstBinding = 0;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		w.pBufferInfo = &info;
		vkUpdateDescriptorSets(dev.device, 1, &w, 0, nullptr);
		LandscapeDraw d;
		d.id = id;
		d.landscape = l;
		d.set = set;
		d.model = u.model;
		for (uint32_t level = 0; level < kLandscapeMaxLods; ++level) d.ranges[level] = u.morph[level].z;
		draws.push_back(d);
	}
	return draws;
}

LandscapeNodes SelectLandscapeNodes(const LandscapeDraw& draw, const glm::vec3& camera, const FrustumPlaneSet& frustum)
{
	LandscapeNodes out;
	auto it = R.landscapes.find(draw.id);
	if (it == R.landscapes.end()) return out;
	const LandscapeGPU& g = it->second;
	const HRL_Landscape* l = draw.landscape;
	std::vector<glm::vec4> nodes;
	// Depth first from the root; children of a node closer than the range of
	// the next level are visited, the others drawn at the node's level.
	struct Item { uint32_t level, x, z; };
	std::vector<Item> stack;
	const uint32_t root = g.levels - 1;
	for (uint32_t z = 0; z < g.dims[root].y; ++z)
		for (uint32_t x = 0; x < g.dims[root].x; ++x) stack.push_back({root, x, z});
	while (!stack.empty())
	{
		const Item n = stack.back();
		stack.pop_back();
		if (n.x >= g.dims[n.level].x || n.z >= g.dims[n.level].y) continue;
		const Box box = NodeBox(g, l, draw.model, n.level, n.x, n.z);
		if (!BoxInFrustum(frustum, box)) continue;
		if (n.level > 0 && BoxTouchesSphere(box, camera, draw.ranges[n.level - 1]))
		{
			for (uint32_t c = 0; c < 4; ++c) stack.push_back({n.level - 1, n.x * 2 + (c & 1), n.z * 2 + (c >> 1)});
			continue;
		}
		const uint32_t size = kPatch << n.level;
		nodes.emplace_back((float)(n.x * size), (float)(n.z * size), (float)size, (float)n.level);
	}
	if (nodes.empty()) return out;
	out.slice = Dev().AllocFrame(nodes.size() * sizeof(glm::vec4), 16);
	if (!out.slice.ptr) return out;
	std::memcpy(out.slice.ptr, nodes.data(), nodes.size() * sizeof(glm::vec4));
	out.count = (uint32_t)nodes.size();
	return out;
}

void DrawLandscape(VkCommandBuffer cmd, const LandscapeDraw& draw, const LandscapeNodes& nodes)
{
	if (!nodes.count) return;
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.landscapeLayout, 2, 1, &draw.set, 0, nullptr);
	const VkBuffer buffers[2] = {R.landscapePatchVertices.handle, nodes.slice.buffer};
	const VkDeviceSize offsets[2] = {0, nodes.slice.offset};
	vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
	vkCmdBindIndexBuffer(cmd, R.landscapePatchIndices.handle, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, R.landscapePatchIndexCount, nodes.count, 0, 0, 0);
}

void ReleaseUnusedLandscapes()
{
	const uint64_t frame = Dev().FrameNumber();
	for (auto it = R.landscapes.begin(); it != R.landscapes.end();)
	{
		// Not drawn for a while: the landscape was deleted (or its scene is no longer rendered).
		if (it->second.lastFrame + 8 < frame)
		{
			DestroyLandscapeGPU(it->second);
			it = R.landscapes.erase(it);
		}
		else ++it;
	}
}

void DestroyAllLandscapes()
{
	for (auto& [id, g] : R.landscapes) DestroyLandscapeGPU(g);
	R.landscapes.clear();
	Dev().DestroyBuffer(R.landscapePatchVertices);
	Dev().DestroyBuffer(R.landscapePatchIndices);
}
} // namespace hrlvk
