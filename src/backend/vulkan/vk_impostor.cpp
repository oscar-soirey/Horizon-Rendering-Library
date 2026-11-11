// HRL native Vulkan backend - foliage impostors.
//
// When the parts of a foliage kind change, the plant is rendered from
// kImpostorFrames x kImpostorFrames directions of the upper hemisphere
// (hemi-octahedral layout) into two atlases: albedo with coverage, and local
// normal with roughness. Far instances are then drawn as one quad each;
// impostor.frag blends the four views closest to the viewing direction, so the
// plant turns smoothly as the camera moves around it. Mips are generated so
// distant impostors do not shimmer.
#include "vk_renderer_internal.h"

namespace hrlvk
{
namespace
{
constexpr uint32_t kAtlasSize = kImpostorFrames * kImpostorFrameSize;
constexpr uint32_t kAtlasMips = 5; // down to 8 pixels per view

struct BakePush
{
	glm::vec4 right, up, dir, center;
	uint32_t material;
};

glm::vec3 HemiOctDecode(glm::vec2 uv)
{
	const glm::vec2 p = uv * 2.f - 1.f;
	const glm::vec2 xz = glm::vec2(p.x + p.y, p.x - p.y) * 0.5f;
	return glm::normalize(glm::vec3(xz.x, 1.f - std::abs(xz.x) - std::abs(xz.y), xz.y));
}
} // namespace

void DestroyImpostor(FoliageGPU& g)
{
	Device& dev = Dev();
	FreeBindlessSlot(g.albedoSlot);
	FreeBindlessSlot(g.normalSlot);
	g.albedoSlot = g.normalSlot = kNoSlot;
	for (VkImageView view : {g.impostorAlbedoMip0, g.impostorNormalMip0})
		if (view)
		{
			VkDevice d = dev.device;
			dev.Defer([d, view]() { vkDestroyImageView(d, view, nullptr); });
		}
	g.impostorAlbedoMip0 = g.impostorNormalMip0 = VK_NULL_HANDLE;
	dev.DestroyImage(g.impostorAlbedo);
	dev.DestroyImage(g.impostorNormal);
	g.impostorReady = false;
	g.impostorRevision = 0;
}

void PrepareImpostors(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame, FrameGraph& graph, VkDescriptorSet viewSet)
{
	Device& dev = Dev();
	for (const auto& [id, f] : scene->foliage)
	{
		if (!f || f->parts.empty() || f->impostor_distance <= 0.f) continue;
		auto it = R.foliage.find(id);
		if (it == R.foliage.end()) continue;
		FoliageGPU& g = it->second;
		if (g.impostorReady && g.impostorRevision == f->geometry_revision) continue;

		// Parts and their LOD 0 ranges.
		struct Part { uint32_t indexCount, firstIndex; int32_t vertexOffset; uint32_t material; bool masked; };
		std::vector<Part> parts;
		for (const auto& part : f->parts)
		{
			auto mesh = R.meshes.find(part->mesh_id);
			if (mesh == R.meshes.end() || mesh->second.lods.empty() || !mesh->second.lods[0].valid) continue;
			const MeshLOD& lod = mesh->second.lods[0];
			const uint32_t material = MaterialIndex(data, frame, part->material);
			const Bucket bucket = data.materialBuckets[material];
			parts.push_back({lod.indexCount, lod.firstIndex, (int32_t)lod.vertexOffset, material, IsMasked(bucket) || IsTranslucent(bucket)});
		}
		if (parts.empty()) continue;

		if (!g.impostorAlbedo)
		{
			ImageDesc d;
			d.width = d.height = kAtlasSize;
			d.mips = kAtlasMips;
			d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			d.format = VK_FORMAT_R8G8B8A8_SRGB;
			bool ok = dev.CreateImage(d, g.impostorAlbedo, "impostor albedo");
			d.format = VK_FORMAT_R8G8B8A8_UNORM;
			ok = ok && dev.CreateImage(d, g.impostorNormal, "impostor normal");
			if (ok)
			{
				// Rendered through views of mip 0, sampled with every mip.
				g.impostorAlbedoMip0 = dev.CreateView(g.impostorAlbedo, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1);
				g.impostorNormalMip0 = dev.CreateView(g.impostorNormal, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1);
				g.albedoSlot = AllocBindlessSlot();
				g.normalSlot = AllocBindlessSlot();
			}
			if (!ok || g.albedoSlot == kNoSlot || g.normalSlot == kNoSlot) { DestroyImpostor(g); continue; }
			const VkSampler sampler = GetTextureSampler(HRL_FILTER_TRILINEAR, HRL_FILTER_LINEAR);
			WriteBindlessView(g.albedoSlot, g.impostorAlbedo.view, sampler);
			WriteBindlessView(g.normalSlot, g.impostorNormal.view, sampler);
		}
		g.impostorCenter = f->bounds_center;
		g.impostorHalfSize = std::max(f->bounds_radius, 1e-3f);

		ImageDesc depthDesc;
		depthDesc.format = kDepthFormat;
		depthDesc.width = depthDesc.height = kAtlasSize;
		depthDesc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		const FrameGraph::Handle depth = graph.CreateTransient(depthDesc, "impostor depth");
		const FrameGraph::Handle albedo = graph.Import(g.impostorAlbedo);
		const FrameGraph::Handle normal = graph.Import(g.impostorNormal);
		const glm::vec3 center = g.impostorCenter;
		const float invHalf = 1.f / g.impostorHalfSize;
		const VkShaderStageFlags VF = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		graph.AddPass("impostor bake", [=](VkCommandBuffer c)
		{
			const VkDescriptorSet sets[2] = {viewSet, R.bindlessSet};
			vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.sceneLayout, 0, 2, sets, 0, nullptr);
			VkDeviceSize zero = 0;
			vkCmdBindVertexBuffers(c, 0, 1, &R.vertexBuffer.handle, &zero);
			vkCmdBindIndexBuffer(c, R.indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);
			for (uint32_t fy = 0; fy < kImpostorFrames; ++fy)
				for (uint32_t fx = 0; fx < kImpostorFrames; ++fx)
				{
					const glm::vec3 d = HemiOctDecode((glm::vec2((float)fx, (float)fy) + 0.5f) / (float)kImpostorFrames);
					const glm::vec3 right = d.y > 0.999f ? glm::vec3(1.f, 0.f, 0.f) : glm::normalize(glm::cross(glm::vec3(0.f, 1.f, 0.f), d));
					const glm::vec3 up = glm::cross(d, right);
					VkViewport vp{(float)(fx * kImpostorFrameSize), (float)(fy * kImpostorFrameSize),
						(float)kImpostorFrameSize, (float)kImpostorFrameSize, 0.f, 1.f};
					vkCmdSetViewport(c, 0, 1, &vp);
					const VkRect2D sc{{(int32_t)(fx * kImpostorFrameSize), (int32_t)(fy * kImpostorFrameSize)}, {kImpostorFrameSize, kImpostorFrameSize}};
					vkCmdSetScissor(c, 0, 1, &sc);
					for (const Part& p : parts)
					{
						vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.impostorBakePipelines[p.masked ? 1 : 0]);
						const BakePush push{glm::vec4(right, invHalf), glm::vec4(up, 0.f), glm::vec4(d, 0.f), glm::vec4(center, 0.f), p.material};
						vkCmdPushConstants(c, R.sceneLayout, VF, 0, sizeof(push), &push);
						vkCmdDrawIndexed(c, p.indexCount, 1, p.firstIndex, p.vertexOffset, 0);
					}
				}
		}).Color(albedo, VK_ATTACHMENT_LOAD_OP_CLEAR, VkClearColorValue{{0.f, 0.f, 0.f, 0.f}}, g.impostorAlbedoMip0)
		  .Color(normal, VK_ATTACHMENT_LOAD_OP_CLEAR, VkClearColorValue{{0.f, 0.f, 0.f, 0.f}}, g.impostorNormalMip0)
		  .Depth(depth, VK_ATTACHMENT_LOAD_OP_CLEAR, 1.f);
		Image* albedoImage = &g.impostorAlbedo;
		Image* normalImage = &g.impostorNormal;
		graph.AddPass("impostor mips", [albedoImage, normalImage](VkCommandBuffer c)
		{
			for (Image* image : {albedoImage, normalImage})
			{
				Dev().Transition(c, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				Dev().GenerateMips(c, *image);
			}
			// Sampled by the vertex / fragment shaders of the views that follow.
			VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
			mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
			mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
			VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			di.memoryBarrierCount = 1;
			di.pMemoryBarriers = &mb;
			vkCmdPipelineBarrier2(c, &di);
		});
		g.impostorReady = true;
		g.impostorRevision = f->geometry_revision;
	}
}

void DrawImpostors(VkCommandBuffer cmd, const FoliageDraw& draw, bool shadow, const glm::mat4& lightViewProj, const glm::vec3& lightDir)
{
	auto it = R.foliage.find(draw.id);
	if (it == R.foliage.end() || !it->second.impostorReady) return;
	const FoliageGPU& g = it->second;
	struct { glm::mat4 lightViewProj; uint32_t part, pad[3]; glm::vec4 lightDir; } push{lightViewProj, 0u, {}, glm::vec4(lightDir, 0.f)};
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow ? R.impostorShadowPipeline : R.impostorPipeline);
	vkCmdPushConstants(cmd, R.foliageLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	vkCmdDrawIndirectCount(cmd, g.draw.handle, 32 + 12 * 20, g.draw.handle, 16, 1, sizeof(VkDrawIndirectCommand));
}
} // namespace hrlvk
