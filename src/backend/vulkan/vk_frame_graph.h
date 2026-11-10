// HRL native Vulkan backend - frame graph.
//
// A frame is described as a list of passes. Each pass declares the images it
// reads and writes and how; the graph then, in declaration order:
//   * inserts the layout transitions / barriers every pass needs,
//   * opens and closes the dynamic rendering scope of raster passes,
//   * wraps each pass in a debug label (RenderDoc, validation messages),
//   * provides transient images recycled through a pool, so intermediate
//     targets (HDR colour...) are shared by the scenes and frames that need
//     the same description.
// Passes are neither reordered nor culled: every pass added is recorded.
#ifndef HRL_VK_FRAME_GRAPH_H
#define HRL_VK_FRAME_GRAPH_H

#include "vk_device.h"

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace hrlvk
{
// How a pass uses an image (decides its layout during the pass).
enum class Access { Sampled, ColorAttachment, DepthAttachment, CopySource, CopyDestination, Storage };

// Images handed out to the frame graphs and reused once released.
class TransientPool
{
public:
	Image* Acquire(const ImageDesc& desc, const char* name);
	// The image may be handed out again at once: commands are recorded in order.
	void Release(Image* image);
	// Called once per frame: destroys the images that stayed unused for a while.
	void EndFrame();
	void Clear();
	size_t ImageCount() const { return entries_.size(); }

private:
	struct Entry
	{
		ImageDesc desc;
		std::unique_ptr<Image> image;
		bool inUse = false;
		uint32_t idleFrames = 0;
	};
	std::vector<Entry> entries_;
};

class FrameGraph
{
public:
	using Handle = uint32_t;

	class Pass
	{
	public:
		// Image sampled, copied from, copied to or written as storage by the pass.
		Pass& Use(Handle image, Access access);
		// Colour attachment. `view` selects a layer / mip (default: the whole image).
		Pass& Color(Handle image, VkAttachmentLoadOp load, VkClearColorValue clear = {}, VkImageView view = VK_NULL_HANDLE);
		Pass& Depth(Handle image, VkAttachmentLoadOp load, float clear = 0.f, VkImageView view = VK_NULL_HANDLE);

	private:
		friend class FrameGraph;
		struct ImageUse { Handle image; Access access; };
		struct Attachment { Handle image; VkAttachmentLoadOp load; VkClearValue clear; VkImageView view; };
		const char* name = "";
		std::function<void(VkCommandBuffer)> record;
		std::vector<ImageUse> uses;
		std::vector<Attachment> colors;
		Attachment depth{};
		bool hasDepth = false;
	};

	FrameGraph(Device& device, TransientPool& pool) : device_(device), pool_(pool) {}
	~FrameGraph();
	FrameGraph(const FrameGraph&) = delete;
	FrameGraph& operator=(const FrameGraph&) = delete;

	// Image owned by the caller; it must stay alive until Execute() returns.
	Handle Import(Image& image);
	// Image owned by the graph, valid until the graph is destroyed. Its content
	// is undefined when first used.
	Handle CreateTransient(const ImageDesc& desc, const char* name);
	Image& GetImage(Handle handle) { return *images_[handle].image; }

	// `record` is called by Execute() with the pass images in the declared
	// state; for a raster pass, inside the rendering scope of its attachments.
	Pass& AddPass(const char* name, std::function<void(VkCommandBuffer)> record);
	void Execute(VkCommandBuffer cmd);
	size_t PassCount() const { return passes_.size(); }

private:
	struct ImageRef { Image* image; bool transient; };
	Device& device_;
	TransientPool& pool_;
	std::vector<ImageRef> images_;
	std::deque<Pass> passes_;
};
} // namespace hrlvk

#endif
