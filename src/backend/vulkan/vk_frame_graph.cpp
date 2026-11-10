// HRL native Vulkan backend - frame graph implementation.
#include "vk_frame_graph.h"

#include <algorithm>

namespace hrlvk
{
namespace
{
constexpr uint32_t kMaxIdleFrames = 8;

bool SameDesc(const ImageDesc& a, const ImageDesc& b)
{
	return a.format == b.format && a.width == b.width && a.height == b.height && a.mips == b.mips &&
		a.layers == b.layers && a.usage == b.usage && a.cube == b.cube && a.arrayView == b.arrayView &&
		a.mutableFormat == b.mutableFormat;
}

VkImageLayout LayoutFor(Access access)
{
	switch (access)
	{
	case Access::Sampled: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	case Access::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	case Access::DepthAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	case Access::CopySource: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	case Access::CopyDestination: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	case Access::Storage: return VK_IMAGE_LAYOUT_GENERAL;
	}
	return VK_IMAGE_LAYOUT_GENERAL;
}
} // namespace

// ---------------------------------------------------------------------------
// Transient pool
// ---------------------------------------------------------------------------
Image* TransientPool::Acquire(const ImageDesc& desc, const char* name)
{
	for (Entry& e : entries_)
		if (!e.inUse && SameDesc(e.desc, desc))
		{
			e.inUse = true;
			e.idleFrames = 0;
			return e.image.get();
		}
	Entry e;
	e.desc = desc;
	e.image = std::make_unique<Image>();
	if (!GetDevice().CreateImage(desc, *e.image, name)) return nullptr;
	e.inUse = true;
	entries_.push_back(std::move(e));
	return entries_.back().image.get();
}

void TransientPool::Release(Image* image)
{
	for (Entry& e : entries_)
		if (e.image.get() == image) e.inUse = false;
}

void TransientPool::EndFrame()
{
	for (size_t i = 0; i < entries_.size();)
	{
		Entry& e = entries_[i];
		if (!e.inUse && ++e.idleFrames > kMaxIdleFrames)
		{
			GetDevice().DestroyImage(*e.image);
			entries_.erase(entries_.begin() + (std::ptrdiff_t)i);
		}
		else ++i;
	}
}

void TransientPool::Clear()
{
	for (Entry& e : entries_) GetDevice().DestroyImage(*e.image);
	entries_.clear();
}

// ---------------------------------------------------------------------------
// Frame graph
// ---------------------------------------------------------------------------
FrameGraph::~FrameGraph()
{
	for (ImageRef& ref : images_)
		if (ref.transient) pool_.Release(ref.image);
}

FrameGraph::Handle FrameGraph::Import(Image& image)
{
	images_.push_back({&image, false});
	return (Handle)images_.size() - 1;
}

FrameGraph::Handle FrameGraph::CreateTransient(const ImageDesc& desc, const char* name)
{
	Image* image = pool_.Acquire(desc, name);
	if (!image)
	{
		// Allocation failed (already reported): keep the graph usable with an
		// empty image; passes using it are skipped by Execute().
		static Image empty;
		images_.push_back({&empty, false});
	}
	else images_.push_back({image, true});
	return (Handle)images_.size() - 1;
}

FrameGraph::Pass& FrameGraph::Pass::Use(Handle image, Access access)
{
	uses.push_back({image, access});
	return *this;
}

FrameGraph::Pass& FrameGraph::Pass::Color(Handle image, VkAttachmentLoadOp load, VkClearColorValue clear, VkImageView view)
{
	Attachment a{image, load, {}, view};
	a.clear.color = clear;
	colors.push_back(a);
	return *this;
}

FrameGraph::Pass& FrameGraph::Pass::Depth(Handle image, VkAttachmentLoadOp load, float clear, VkImageView view)
{
	depth = Attachment{image, load, {}, view};
	depth.clear.depthStencil = {clear, 0};
	hasDepth = true;
	return *this;
}

FrameGraph::Pass& FrameGraph::AddPass(const char* name, std::function<void(VkCommandBuffer)> record)
{
	passes_.emplace_back();
	passes_.back().name = name;
	passes_.back().record = std::move(record);
	return passes_.back();
}

void FrameGraph::Execute(VkCommandBuffer cmd)
{
	for (Pass& pass : passes_)
	{
		// A pass whose image could not be allocated is skipped.
		bool usable = true;
		auto check = [&](Handle h) { if (!images_[h].image->handle) usable = false; };
		for (const auto& u : pass.uses) check(u.image);
		for (const auto& a : pass.colors) check(a.image);
		if (pass.hasDepth) check(pass.depth.image);
		if (!usable) continue;

		device_.BeginLabel(cmd, pass.name);
		for (const auto& u : pass.uses)
			device_.Transition(cmd, *images_[u.image].image, LayoutFor(u.access));
		for (const auto& a : pass.colors)
			device_.Transition(cmd, *images_[a.image].image, LayoutFor(Access::ColorAttachment));
		if (pass.hasDepth)
			device_.Transition(cmd, *images_[pass.depth.image].image, LayoutFor(Access::DepthAttachment));

		const bool raster = !pass.colors.empty() || pass.hasDepth;
		if (raster)
		{
			VkRenderingAttachmentInfo colors[8]{};
			const uint32_t colorCount = (uint32_t)std::min<size_t>(pass.colors.size(), 8);
			VkExtent2D extent{0, 0};
			for (uint32_t i = 0; i < colorCount; ++i)
			{
				const auto& a = pass.colors[i];
				const Image& image = *images_[a.image].image;
				colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
				colors[i].imageView = a.view ? a.view : image.view;
				colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				colors[i].loadOp = a.load;
				colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				colors[i].clearValue = a.clear;
				extent = image.extent;
			}
			VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
			if (pass.hasDepth)
			{
				const Image& image = *images_[pass.depth.image].image;
				depth.imageView = pass.depth.view ? pass.depth.view : image.view;
				depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				depth.loadOp = pass.depth.load;
				depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				depth.clearValue = pass.depth.clear;
				extent = image.extent;
			}
			VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
			info.renderArea = {{0, 0}, extent};
			info.layerCount = 1;
			info.colorAttachmentCount = colorCount;
			info.pColorAttachments = colors;
			info.pDepthAttachment = pass.hasDepth ? &depth : nullptr;
			vkCmdBeginRendering(cmd, &info);
		}
		if (pass.record) pass.record(cmd);
		if (raster) vkCmdEndRendering(cmd);
		device_.EndLabel(cmd);
	}
}
} // namespace hrlvk
