// HRL Vulkan backend - rendering context.
//
// A small explicit-state layer that gives the Vulkan renderer the same
// vocabulary as the OpenGL 3.3 renderer (framebuffers with draw buffers,
// texture units, uniform buffer binding points, depth/blend/cull state,
// blits, read-backs), while recording native Vulkan commands: render passes
// are opened/closed automatically, image layouts are tracked, pipelines and
// descriptor sets are cached. Porting a GL pass is then a 1:1 translation,
// which keeps both backends visually identical.
#ifndef HRL_VK_CONTEXT_H
#define HRL_VK_CONTEXT_H

#include "vk_core.h"
#include "vulkan_shader.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace hrlvk
{
// A texture object : image + default sampler parameters (glTexParameter).
struct Texture
{
	Image image;
	SamplerState sampler;
	explicit operator bool() const { return image.image != VK_NULL_HANDLE; }
};

bool CreateTexture(Texture& out, ImageKind kind, VkFormat format, uint32_t width, uint32_t height,
	uint32_t layers, uint32_t mips, VkSampleCountFlagBits samples, bool renderTarget);
void DestroyTexture(Texture& texture);

constexpr uint32_t kMaxColorAttachments = 5;

struct Framebuffer
{
	Texture* color[kMaxColorAttachments] = {};
	uint32_t colorLayer[kMaxColorAttachments] = {};
	Texture* depth = nullptr;
	uint32_t depthLayer = 0;
	uint32_t drawMask = 0x1F; // glDrawBuffers : enabled color attachments
	uint32_t ColorCount() const
	{
		uint32_t n = 0;
		for (uint32_t i = 0; i < kMaxColorAttachments; ++i) if (color[i]) n = i + 1;
		return n;
	}
};

struct VertexBinding
{
	uint32_t binding = 0;
	uint32_t stride = 0;
	bool perInstance = false;
};

struct VertexAttrib
{
	uint32_t location = 0;
	uint32_t binding = 0;
	VkFormat format = VK_FORMAT_R32G32B32_SFLOAT;
	uint32_t offset = 0;
};

// Equivalent of an OpenGL vertex array object.
struct VertexArray
{
	std::vector<VertexBinding> bindings;
	std::vector<VertexAttrib> attribs;
	VkBuffer buffers[12] = {};
	VkDeviceSize offsets[12] = {};
	VkBuffer indexBuffer = VK_NULL_HANDLE;
	VkDeviceSize indexOffset = 0;
	// Generic attribute values (glVertexAttrib4f) for locations without array.
	std::vector<std::pair<uint32_t, glm::vec4>> constants;
	uint64_t LayoutHash() const;
};

enum class Topology { Triangles, Lines };

struct RasterState
{
	bool depthTest = false;
	bool depthWrite = true;
	VkCompareOp depthFunc = VK_COMPARE_OP_LESS;
	bool blend = false;
	VkBlendFactor srcFactor = VK_BLEND_FACTOR_ONE;
	VkBlendFactor dstFactor = VK_BLEND_FACTOR_ZERO;
	bool cull = false;
	VkCullModeFlags cullFace = VK_CULL_MODE_BACK_BIT;
	bool wireframe = false;
	uint8_t colorMask[kMaxColorAttachments] = {0xF, 0xF, 0xF, 0xF, 0xF};
	bool polygonOffset = false;
	float offsetFactor = 0.f;
	float offsetUnits = 0.f;
	float lineWidth = 1.f;
	// helpers mirroring glBlendFunc
	void BlendFunc(VkBlendFactor src, VkBlendFactor dst) { srcFactor = src; dstFactor = dst; }
};

struct UniformBinding
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	VkDeviceSize range = 0;
};

class Context
{
public:
	bool Init();
	void Shutdown();

	// Frame
	void BeginFrame();   // after Core::BeginFrame
	void EndRenderPass(); // closes the open render pass, if any

	// glBindFramebuffer / glViewport
	void BindFramebuffer(Framebuffer* fb);
	Framebuffer* BoundFramebuffer() const { return fb_; }
	void Viewport(int x, int y, int width, int height);
	// glClear on the enabled draw buffers / depth
	void Clear(bool color, bool depth, const glm::vec4& clearColor = glm::vec4(0.f), float clearDepth = 1.f);
	// glClearBufferfv(GL_COLOR, index, value)
	void ClearColorBuffer(uint32_t index, const glm::vec4& value);

	RasterState state;

	// Texture units (glActiveTexture + glBindTexture) and sampler objects.
	// slot : 0 = GL_TEXTURE_2D / GL_TEXTURE_2D_MULTISAMPLE, 1 = cube map, 2 = 2D array.
	// The texture image may be (re)created after binding, like in OpenGL.
	void BindTexture(uint32_t unit, int slot, Texture* texture);
	void UnbindTexture(const Texture* texture);
	void BindSampler(uint32_t unit, const SamplerState* sampler);
	// glBindBufferBase(GL_UNIFORM_BUFFER, point, ...)
	void BindUniformBuffer(uint32_t point, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);
	// Copies data into the per-frame ring and binds it at the point.
	void BindUniformData(uint32_t point, const void* data, size_t size);

	// Draw calls
	void Draw(VulkanProgram* program, const VertexArray& va, Topology topology, uint32_t count,
		bool indexed, uint32_t instances = 1, uint32_t first = 0);

	// Transient vertex data for this frame (UI quads, debug lines, instances).
	bool AllocateTransient(const void* data, size_t size, VkBuffer& buffer, VkDeviceSize& offset, size_t alignment = 16);

	// glBlitFramebuffer between single attachments (color). MSAA sources are resolved.
	void BlitColor(Texture& src, uint32_t srcLayer, Texture& dst, uint32_t dstLayer,
		int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, bool linear = false);
	// Depth copy / resolve (MSAA depth is resolved with sample 0 like GL).
	void BlitDepth(Texture& src, Texture& dst, int x0, int y0, int x1, int y1);
	// Copies a rectangle of a color texture into a 8-bit swapchain image (flipped
	// vertically: GL bottom-up -> window top-down).
	void PresentToSwapchain(Texture& src, int width, int height);

	// glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE) : rows bottom-up like OpenGL.
	bool ReadPixels(Texture& src, int x, int y, int width, int height, std::vector<unsigned char>& rgba);
	// Records a read-back copied after the frame completes (screenshots).
	bool QueueReadback(Texture& src, std::function<void(int, int, std::vector<unsigned char>&)> done);
	void ProcessReadbacks(); // after the frame fence

	// Called before an image is destroyed (framebuffer cache).
	void ForgetImage(uint64_t serial);

	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

private:
	struct PassKey
	{
		VkFormat formats[kMaxColorAttachments + 1];
		uint32_t colorCount;
		VkSampleCountFlagBits samples;
		bool operator==(const PassKey& o) const;
	};
	VkRenderPass GetRenderPass(const PassKey& key);
	VkFramebuffer GetFramebuffer(const Framebuffer& fb, VkRenderPass rp, PassKey& key, uint32_t& w, uint32_t& h);
	bool BeginPass();
	VkPipeline GetPipeline(VulkanProgram* program, const VertexArray& va, Topology topology, VkRenderPass rp, const PassKey& key);
	VkDescriptorSet GetDescriptorSet(VulkanProgram* program, uint32_t dynamicOffsets[6]);
	bool UploadRing(const void* data, size_t size, size_t alignment, VkBuffer& buffer, VkDeviceSize& offset);
	void TransitionForSampling(VulkanProgram* program);
	Texture* ResolveUnit(const ShaderSampler& s);

	Framebuffer* fb_ = nullptr;
	Framebuffer passFbCopy_{};
	bool passOpen_ = false;
	VkRenderPass passRp_ = VK_NULL_HANDLE;
	PassKey passKey_{};
	uint32_t passW_ = 0, passH_ = 0;
	VkViewport viewport_{0, 0, 1, 1, 0, 1};
	bool viewportDirty_ = true;

	struct Unit
	{
		Texture* tex2D = nullptr;
		Texture* cube = nullptr;
		Texture* array2D = nullptr;
		const SamplerState* sampler = nullptr;
	};
	std::array<Unit, 32> units_{};
	UniformBinding ubos_[kUniformBlockPoints]{};

	struct RenderPassEntry { PassKey key; VkRenderPass rp; };
	std::vector<RenderPassEntry> renderPasses_;
	struct FramebufferEntry { std::vector<uint64_t> key; VkFramebuffer fb; std::vector<uint64_t> serials; };
	std::vector<FramebufferEntry> framebuffers_;
	std::unordered_map<uint64_t, VkPipeline> pipelines_;

	// Per frame-in-flight transient resources
	struct RingBuffer { Buffer buffer; size_t cursor = 0; };
	struct FrameResources
	{
		std::vector<RingBuffer> rings;
		size_t ringIndex = 0;
		std::vector<VkDescriptorPool> pools;
		size_t poolIndex = 0;
		uint32_t setsInPool = 0;
		std::unordered_map<uint64_t, VkDescriptorSet> sets;
		std::vector<std::pair<Buffer, std::function<void(int, int, std::vector<unsigned char>&)>>> readbacks;
		std::vector<std::pair<int, int>> readbackSizes;
	};
	FrameResources frames_[kFramesInFlight];
	FrameResources& FR() { return frames_[GetCore().frameIndex]; }

	// Last program data uploaded in this frame (avoid duplicate copies)
	std::unordered_map<uint64_t, std::pair<uint64_t, std::pair<VkBuffer, VkDeviceSize>>> programUploads_;

	std::vector<VkFormat> formatsQueued_;
	Buffer dummyUbo_;
	Buffer defaultAttribs_;
	Texture fallback2D_, fallbackCube_, fallbackArray_, fallbackMS_;
	VulkanProgram* depthResolveProgram_ = nullptr;
	Framebuffer depthResolveFb_;
};

Context& GetContext();
} // namespace hrlvk

#endif
