// HRL native Vulkan backend - 2D overlays: gizmos, widgets, texts and
// on-screen messages, drawn over the tone mapped image of each viewport.
//
// The frontend runs the widget logic and hands textured quads
// (HRL_SceneFrame::overlays); the gizmo lines are built here from the scene
// with the camera of the viewport (BuildGizmoOverlay, shared with OpenGL).
#include "vk_renderer_internal.h"

namespace hrlvk
{
namespace
{
struct OverlayQuadGPU // overlay.glsl
{
	glm::vec4 rect, uv, color;
	glm::uvec4 info;
};

uint32_t OverlayTextureSlot(HRL_id id)
{
	auto it = R.textures.find(id);
	return it != R.textures.end() ? it->second.slot : R.white.slot;
}

struct ViewDraw
{
	float x = 0.f, y = 0.f, w = 1.f, h = 1.f;
	glm::mat4 viewProj{1.f};
	FrameSlice lines;
	uint32_t lineCount = 0;
	uint32_t firstQuad = 0, quadCount = 0;
};
} // namespace

void AddOverlayPasses(FrameGraph& graph, const HRL_SceneFrame& frame, const std::vector<OverlayView>& views,
	FrameGraph::Handle target)
{
	Device& dev = Dev();
	std::vector<ViewDraw> draws;
	std::vector<OverlayQuadGPU> quads;
	for (const OverlayView& v : views)
	{
		ViewDraw d;
		d.x = v.x; d.y = v.y; d.w = v.w; d.h = v.h;
		d.viewProj = v.viewProj;
		if (!frame.scene->gizmos.empty())
		{
			DebugRenderer gizmos;
			if (BuildGizmoOverlay(frame.scene, v.viewport, v.viewportId, v.view, gizmos) && !gizmos.lines.empty())
			{
				const size_t bytes = gizmos.lines.size() * sizeof(DebugVertex);
				d.lines = dev.AllocFrame(bytes, 16);
				if (d.lines.ptr)
				{
					std::memcpy(d.lines.ptr, gizmos.lines.data(), bytes);
					d.lineCount = (uint32_t)gizmos.lines.size();
				}
			}
		}
		if (frame.overlays)
			for (const HRL_ViewportOverlay& overlay : *frame.overlays)
			{
				if (overlay.viewport != v.viewportId) continue;
				d.firstQuad = (uint32_t)quads.size();
				for (const HRL_OverlayQuad& q : overlay.quads)
					quads.push_back({glm::vec4(q.x, q.y, q.w, q.h), glm::vec4(q.u0, q.v0, q.u1, q.v1), q.color,
						glm::uvec4(OverlayTextureSlot(q.texture), q.sdf ? 1u : 0u, 0u, 0u)});
				d.quadCount = (uint32_t)quads.size() - d.firstQuad;
			}
		if (d.lineCount || d.quadCount) draws.push_back(d);
	}
	if (draws.empty()) return;

	VkDescriptorSet set = VK_NULL_HANDLE;
	if (!quads.empty())
	{
		const FrameSlice slice = dev.AllocFrame(quads.size() * sizeof(OverlayQuadGPU), 256);
		set = slice.ptr ? AllocFrameSet(R.overlaySetLayout) : VK_NULL_HANDLE;
		if (set)
		{
			std::memcpy(slice.ptr, quads.data(), quads.size() * sizeof(OverlayQuadGPU));
			const VkDescriptorBufferInfo info{slice.buffer, slice.offset, slice.size};
			VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			write.dstSet = set;
			write.dstBinding = 0;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			write.pBufferInfo = &info;
			vkUpdateDescriptorSets(dev.device, 1, &write, 0, nullptr);
		}
	}
	// A UNORM target stores the display values as given; an sRGB one encodes them.
	const uint32_t encoded = R.ldrFormat == VK_FORMAT_R8G8B8A8_UNORM ? 1u : 0u;
	graph.AddPass("overlay", [=](VkCommandBuffer c)
	{
		for (const ViewDraw& d : draws)
		{
			const VkRect2D scissor{{(int32_t)d.x, (int32_t)d.y}, {(uint32_t)d.w, (uint32_t)d.h}};
			vkCmdSetScissor(c, 0, 1, &scissor);
			if (d.lineCount)
			{
				// Same flipped viewport as the 3D passes (OpenGL conventions of the matrices).
				const VkViewport vp{d.x, d.y + d.h, d.w, -d.h, 0.f, 1.f};
				vkCmdSetViewport(c, 0, 1, &vp);
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.gizmoLinePipeline);
				vkCmdPushConstants(c, R.debugLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &d.viewProj);
				vkCmdBindVertexBuffers(c, 0, 1, &d.lines.buffer, &d.lines.offset);
				vkCmdDraw(c, d.lineCount, 1, 0, 0);
			}
			if (d.quadCount && set)
			{
				const VkViewport vp{d.x, d.y, d.w, d.h, 0.f, 1.f};
				vkCmdSetViewport(c, 0, 1, &vp);
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.overlayPipeline);
				const VkDescriptorSet sets[2] = {set, R.bindlessSet};
				vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.overlayLayout, 0, 2, sets, 0, nullptr);
				vkCmdPushConstants(c, R.overlayLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					0, sizeof(encoded), &encoded);
				vkCmdDraw(c, 6, d.quadCount, 0, d.firstQuad);
			}
		}
	}).Color(target, VK_ATTACHMENT_LOAD_OP_LOAD);
}
} // namespace hrlvk
