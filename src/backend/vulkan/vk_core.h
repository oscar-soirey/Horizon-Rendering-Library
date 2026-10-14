// HRL Vulkan backend - device, memory, frames and command recording.
//
// Conventions shared by the whole Vulkan backend:
//  * Every render target uses the OpenGL memory layout: framebuffer row 0 is
//    the bottom of the image (NDC y = -1). The vertex shaders are translated so
//    gl_Position.z is remapped from [-w, w] to [0, w]; no Y flip is applied.
//    Projection matrices, viewports, gl_FragCoord, texture coordinates and
//    depth values are therefore numerically identical to the OpenGL 3.3
//    backend. The image is flipped only when it is presented.
//  * All GPU work of a frame is recorded in a single command buffer, in the
//    same order as the OpenGL backend issues its calls.
#ifndef HRL_VK_CORE_H
#define HRL_VK_CORE_H

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hrlvk
{
constexpr uint32_t kFramesInFlight = 2;

void Error(const char* what, VkResult result);
void Error(const std::string& what);

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
struct Allocation
{
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	VkDeviceSize size = 0;
	void* mapped = nullptr; // host visible allocations are persistently mapped
	int32_t block = -1;     // -1 : dedicated allocation
	uint32_t type = 0;
};

struct Buffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	Allocation alloc;
	VkDeviceSize size = 0;
	void* mapped = nullptr;
	explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------
enum class ImageKind { Tex2D, Cube, Array2D };

struct Image
{
	VkImage image = VK_NULL_HANDLE;
	Allocation alloc;
	VkFormat format = VK_FORMAT_UNDEFINED;
	ImageKind kind = ImageKind::Tex2D;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t layers = 1;
	uint32_t mips = 1;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	VkImageUsageFlags usage = 0;
	VkImageView view = VK_NULL_HANDLE;                 // full view (sampling)
	std::vector<VkImageView> layerViews;               // mip 0, one per layer (attachments)
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // tracked at record time
	uint64_t serial = 0;                               // unique id (caches)
	bool IsDepth() const { return (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0; }
};

struct ImageDesc
{
	ImageKind kind = ImageKind::Tex2D;
	VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	uint32_t width = 1;
	uint32_t height = 1;
	uint32_t layers = 1;
	uint32_t mips = 1;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	bool renderTarget = false;
};

// Sampler parameters, mirrors glTexParameter / glSamplerParameter.
struct SamplerState
{
	VkFilter magFilter = VK_FILTER_LINEAR;
	VkFilter minFilter = VK_FILTER_LINEAR;
	VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	bool useMips = false; // false : GL_NEAREST/GL_LINEAR min filter (no mip sampling)
	VkSamplerAddressMode wrapU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VkSamplerAddressMode wrapV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VkSamplerAddressMode wrapW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	bool borderWhite = false;
	float anisotropy = 1.f;
	bool operator==(const SamplerState& o) const;
};

// ---------------------------------------------------------------------------
// Core device
// ---------------------------------------------------------------------------
struct FrameData
{
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkSemaphore imageAvailable = VK_NULL_HANDLE;
	VkSemaphore renderFinished = VK_NULL_HANDLE;
	std::vector<std::function<void()>> deletions;
	bool submitted = false;
};

class Core
{
public:
	// Configuration (before Init)
	std::vector<std::string> instanceExtensions;
	int (*surfaceCallback)(void* instance, void* surface, void* user) = nullptr;
	void* surfaceUser = nullptr;

	bool Init(uint32_t width, uint32_t height);
	void Shutdown();
	bool IsInitialized() const { return device != VK_NULL_HANDLE; }

	// Handles
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkQueue presentQueue = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	uint32_t presentFamily = 0;
	VkPhysicalDeviceProperties properties{};
	VkPhysicalDeviceFeatures enabledFeatures{};
	VkPipelineCache pipelineCache = VK_NULL_HANDLE;

	// Swapchain
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
	VkExtent2D swapchainExtent{0, 0};
	std::vector<VkImage> swapchainImages;
	uint32_t swapchainIndex = 0;
	bool swapchainImageAcquired = false;
	bool swapchainImageWritten = false;
	bool RecreateSwapchain(uint32_t width, uint32_t height);

	// Frames
	FrameData frames[kFramesInFlight];
	uint32_t frameIndex = 0;
	uint64_t frameNumber = 0;
	bool frameOpen = false;
	FrameData& Frame() { return frames[frameIndex]; }

	// Starts recording a frame. Waits for the frame slot fence, runs its
	// deferred deletions and acquires a swapchain image when presenting.
	bool BeginFrame();
	// Ends recording, submits and presents.
	void EndFrame();

	// Command buffer that receives transfer/rendering commands right now: the
	// frame command buffer during a frame, otherwise the setup command buffer
	// (submitted before the next frame, or by FlushSetup()).
	VkCommandBuffer Cmd();
	// Submits pending work and waits until the GPU is idle (readbacks).
	void FlushAndWait();

	// Resources whose GPU use may still be in flight are destroyed later.
	void Defer(std::function<void()> fn);

	// Buffers / images / memory
	bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, Buffer& out);
	void DestroyBuffer(Buffer& buffer, bool deferred = true);
	// Creates a device-local buffer initialized with data.
	bool CreateBufferWithData(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);

	bool CreateImage(const ImageDesc& desc, Image& out);
	void DestroyImage(Image& image, bool deferred = true);
	// Records a layout transition (and the matching memory dependency).
	void Transition(VkCommandBuffer cmd, Image& image, VkImageLayout newLayout);
	// Uploads RGBA8 pixels into one layer/mip 0 of an image. Leaves the image in SHADER_READ_ONLY.
	bool UploadImage(Image& image, uint32_t layer, const void* pixels, size_t size, uint32_t width, uint32_t height);
	void GenerateMips(Image& image);

	VkSampler GetSampler(const SamplerState& state);

	// Temporary host buffer released after the current frame.
	bool CreateStaging(const void* data, VkDeviceSize size, Buffer& out);

	VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
	VkSampleCountFlags supportedSamples = VK_SAMPLE_COUNT_1_BIT;
	uint64_t NextSerial() { return ++serialCounter_; }

	// Hooks installed by the rendering context: a transfer command cannot be
	// recorded inside a render pass, and every pass must be closed before a submit.
	std::function<void()> onBeforeTransfer;
	std::function<void()> onBeforeSubmit;

private:
	bool CreateInstance();
	bool PickDevice();
	bool CreateDevice();
	bool CreateFrames();
	bool CreateSwapchain(uint32_t width, uint32_t height);
	void DestroySwapchain();
	void SubmitSetup(bool wait);

	bool Allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags, bool hostVisible, Allocation& out);
	void Free(Allocation& alloc);
	uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;

	struct Block
	{
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		uint32_t type = 0;
		void* mapped = nullptr;
		struct Range { VkDeviceSize offset, size; };
		std::vector<Range> freeList;
	};
	std::vector<Block> blocks_;

	VkCommandPool pool_ = VK_NULL_HANDLE;
	VkCommandBuffer setupCmd_ = VK_NULL_HANDLE;
	VkFence setupFence_ = VK_NULL_HANDLE;
	bool setupOpen_ = false;
	std::vector<std::function<void()>> setupDeletions_;

	struct SamplerEntry { SamplerState state; VkSampler sampler; };
	std::vector<SamplerEntry> samplers_;
	uint64_t serialCounter_ = 0;
	VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
};

Core& GetCore();
} // namespace hrlvk

#endif
