// HRL native Vulkan backend - device layer.
//
// Owns the instance, the device, the memory allocator, the swapchain and the
// per-frame resources. Requires Vulkan 1.3 (dynamic rendering,
// synchronization2, descriptor indexing). Hardware ray tracing is optional and
// reported through Caps.
#ifndef HRL_VK_DEVICE_H
#define HRL_VK_DEVICE_H

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hrlvk
{
constexpr uint32_t kFramesInFlight = 2;

struct Caps
{
	std::string deviceName;
	bool rayQuery = false;          // VK_KHR_acceleration_structure + VK_KHR_ray_query
	bool anisotropy = false;
	float maxAnisotropy = 1.f;
	uint32_t maxBindlessTextures = 0;
};

struct Buffer
{
	VkBuffer handle = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
	void* mapped = nullptr;
	VkDeviceAddress address = 0;
	explicit operator bool() const { return handle != VK_NULL_HANDLE; }
};

struct Image
{
	VkImage handle = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkExtent2D extent{0, 0};
	uint32_t mips = 1;
	uint32_t layers = 1;
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; // tracked at record time
	explicit operator bool() const { return handle != VK_NULL_HANDLE; }
};

struct ImageDesc
{
	VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	uint32_t width = 1, height = 1;
	uint32_t mips = 1;
	uint32_t layers = 1;
	VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	bool cube = false;          // 6 layers, cube view
	bool arrayView = false;     // 2D array view even with one layer
	bool mutableFormat = false; // allows views with another (compatible) format
};

// Slice of the per-frame linear buffer (uniforms, instances, indirect commands).
struct FrameSlice
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	VkDeviceSize size = 0;
	void* ptr = nullptr;
};

class Device
{
public:
	// Configuration, set before Init().
	std::vector<std::string> instanceExtensions;
	int (*surfaceCallback)(void* instance, void* surface, void* user) = nullptr;
	void* surfaceUser = nullptr;

	bool Init(uint32_t width, uint32_t height);
	void Shutdown();
	bool IsInitialized() const { return device != VK_NULL_HANDLE; }

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	VmaAllocator allocator = VK_NULL_HANDLE;
	Caps caps;

	// Swapchain (absent when no surface callback was given: off-screen only).
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D swapchainExtent{0, 0};
	bool HasSwapchain() const { return swapchain != VK_NULL_HANDLE; }
	// Image acquired for the current frame, null when none (minimized window...).
	Image* Backbuffer() { return backbufferValid_ ? &swapchainImages_[imageIndex_] : nullptr; }
	void Resize(uint32_t width, uint32_t height);
	// Vertical synchronisation: FIFO when on, else MAILBOX (or IMMEDIATE) when
	// the surface offers it. Takes effect at the next frame.
	void SetVSync(bool enabled);
	bool VSync() const { return vsync_; }

	// Frames
	bool BeginFrame();
	void EndFrame();
	bool InFrame() const { return inFrame_; }
	VkCommandBuffer Cmd() { return frames_[frame_].cmd; }
	uint64_t FrameNumber() const { return frameNumber_; }
	VkDescriptorPool FrameDescriptorPool() { return frames_[frame_].descriptorPool; }
	FrameSlice AllocFrame(VkDeviceSize size, VkDeviceSize alignment = 64);

	// Command buffer for resource uploads. It is submitted before the frame
	// command buffer, so uploads never block the calling thread.
	VkCommandBuffer UploadCmd();
	// Submits pending uploads now. wait: block until the GPU has executed them.
	void FlushUploads(bool wait);
	// Submits everything recorded so far and waits (read-backs, shutdown).
	void WaitIdle();

	// Destroys a resource once no submitted command buffer can still use it.
	void Defer(std::function<void()> fn);
	// GPU memory used by this process and the amount it can use before the
	// system starts evicting (VK_EXT_memory_budget when available, else estimates).
	void MemoryUsage(uint64_t& usedBytes, uint64_t& budgetBytes);
	// Waits for the GPU and runs every deferred destruction now.
	void DrainDeletions();

	// Buffers
	bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, Buffer& out, const char* name = nullptr);
	void DestroyBuffer(Buffer& buffer, bool deferred = true);
	// Copies data into a device buffer through a staging buffer (asynchronous).
	bool UploadToBuffer(const Buffer& dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size);

	// Images
	bool CreateImage(const ImageDesc& desc, Image& out, const char* name = nullptr);
	void DestroyImage(Image& image, bool deferred = true);
	VkImageView CreateView(const Image& image, VkFormat format, VkImageViewType type,
		uint32_t baseMip, uint32_t mipCount, uint32_t baseLayer, uint32_t layerCount);
	// Layout transition with the matching synchronization2 barrier.
	void Transition(VkCommandBuffer cmd, Image& image, VkImageLayout newLayout);
	// Uploads tightly packed pixels into mip 0 / layer and leaves the image in TRANSFER_DST.
	bool UploadToImage(Image& image, uint32_t layer, const void* pixels, VkDeviceSize size);
	// Builds the mip chain with linear blits; leaves the image in SHADER_READ_ONLY.
	void GenerateMips(VkCommandBuffer cmd, Image& image);

	void SetName(VkObjectType type, uint64_t handle, const char* name);
	void BeginLabel(VkCommandBuffer cmd, const char* name);
	void EndLabel(VkCommandBuffer cmd);

private:
	struct Frame
	{
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkSemaphore acquired = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		Buffer linear;
		VkDeviceSize linearOffset = 0;
		bool submitted = false;
	};

	bool CreateInstance();
	bool PickPhysicalDevice();
	bool CreateDevice();
	bool CreateFrames();
	bool CreateSwapchain(uint32_t width, uint32_t height);
	void DestroySwapchain();
	// Runs the deferred deletions whose last possible use is frame <= completed.
	void RunDeletions(uint64_t completedFrame, bool all);
	VkCommandBuffer TakeUploadCmd();

	Frame frames_[kFramesInFlight];
	uint32_t frame_ = 0;
	uint64_t frameNumber_ = 0;
	bool inFrame_ = false;

	std::vector<Image> swapchainImages_;
	std::vector<VkSemaphore> presentReady_; // one per swapchain image
	uint32_t imageIndex_ = 0;
	bool backbufferValid_ = false;
	uint32_t wantedWidth_ = 0, wantedHeight_ = 0;
	bool swapchainDirty_ = false;
	bool vsync_ = true;

	VkCommandPool uploadPool_ = VK_NULL_HANDLE;
	VkCommandBuffer uploadCmd_ = VK_NULL_HANDLE;
	VkFence uploadFence_ = VK_NULL_HANDLE;
	VkDeviceSize pendingStagingBytes_ = 0;
	std::vector<std::function<void()>> uploadDeletions_;
	// (last frame that may use the resource, destructor)
	std::vector<std::pair<uint64_t, std::function<void()>>> deletions_;

	std::vector<const char*> deviceExtensions_;
	VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
	bool debugUtils_ = false;
};

Device& GetDevice();

// Reports a Vulkan failure through the HRL error callback.
void ReportError(const char* what, VkResult result);
void ReportError(const std::string& what);
void ReportWarning(const std::string& what);
} // namespace hrlvk

#endif
