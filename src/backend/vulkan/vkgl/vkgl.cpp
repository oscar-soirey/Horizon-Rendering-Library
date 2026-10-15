// HRL Vulkan backend - OpenGL 3.3 object model implemented over Vulkan.
//
// The Vulkan backend runs the exact same renderer code as the OpenGL 3.3
// backend (gl33_renderer.cpp & co, compiled a second time with the VKGL_
// prefix). Every OpenGL call it makes lands here and is translated to Vulkan
// through hrlvk::Context: textures/renderbuffers become VkImages, buffers are
// versioned (a draw always sees the data that was current when it was
// recorded, like in OpenGL), framebuffers become render passes, programs are
// translated GLSL compiled to SPIR-V, and the fixed function state selects a
// cached VkPipeline. Both backends therefore make the same decisions and
// produce the same images.
#include "vkgl.h"
#include "vkgl_runtime.h"

#include "../vk_context.h"
#include "../vk_core.h"
#include "../vulkan_shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace hrlvk;

namespace
{
constexpr uint32_t kMaxAttribs = 12;
constexpr GLuint kDefaultColorId = 0xFFFFFF01u;
constexpr GLuint kDefaultDepthId = 0xFFFFFF02u;

struct GLTex
{
	GLenum target = 0;
	Texture tex;
	VkFormat format = VK_FORMAT_UNDEFINED;
	int width = 0, height = 0, layers = 1;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	GLint minFilter = GL_NEAREST_MIPMAP_LINEAR;
	GLint magFilter = GL_LINEAR;
	GLint wrapS = GL_REPEAT, wrapT = GL_REPEAT, wrapR = GL_REPEAT;
	bool borderWhite = false;
};

struct GLBuf
{
	std::vector<unsigned char> data;
	Buffer dedicated;
	bool dynamic = false;
	bool dirty = true;
	bool uploaded = false;
	VkBuffer current = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	uint64_t ringFrame = ~0ull;
	uint64_t lastWriteFrame = 0;
};

struct Attrib
{
	bool enabled = false;
	GLint size = 4;
	GLenum type = GL_FLOAT;
	bool normalized = false;
	bool integer = false;
	GLsizei stride = 0;
	uintptr_t offset = 0;
	GLuint buffer = 0;
	GLuint divisor = 0;
};

struct GLVao
{
	Attrib attribs[kMaxAttribs];
	GLuint elementBuffer = 0;
};

struct Attachment
{
	GLuint id = 0;
	bool renderbuffer = false;
	uint32_t layer = 0;
};

struct GLFbo
{
	Attachment color[kMaxColorAttachments];
	Attachment depth;
	uint32_t drawMask = 1;
	int readBuffer = 0; // -1 : GL_NONE
	Framebuffer fb;
};

struct GLShaderObj
{
	GLenum type = 0;
	std::string source;
};

struct GLProg
{
	std::vector<GLuint> shaders;
	std::unique_ptr<VulkanProgram> program;
	bool linked = false;
	std::string log;
	std::vector<std::string> locations;
	std::unordered_map<std::string, GLint> locationMap;
};

struct State
{
	bool initialized = false;
	GLuint nextId = 1;
	std::unordered_map<GLuint, std::unique_ptr<GLTex>> textures;
	std::unordered_map<GLuint, std::unique_ptr<GLTex>> renderbuffers;
	std::unordered_map<GLuint, std::unique_ptr<GLBuf>> buffers;
	std::unordered_map<GLuint, std::unique_ptr<GLVao>> vaos;
	std::unordered_map<GLuint, std::unique_ptr<GLFbo>> fbos;
	std::unordered_map<GLuint, std::unique_ptr<GLShaderObj>> shaders;
	std::unordered_map<GLuint, std::unique_ptr<GLProg>> programs;
	std::unordered_map<GLuint, std::unique_ptr<SamplerState>> samplers;

	GLuint activeUnit = 0;
	struct UnitBinding { GLuint tex2D = 0, cube = 0, array = 0, ms = 0; GLuint sampler = 0; };
	UnitBinding units[32];
	GLuint arrayBuffer = 0;
	GLuint uniformBuffer = 0;
	GLuint uboPoints[kUniformBlockPoints] = {};
	GLuint vao = 0;
	GLuint drawFbo = 0;
	GLuint readFbo = 0;
	GLuint renderbuffer = 0;
	GLuint program = 0;
	glm::vec4 clearColor{0.f};
	GLint viewport[4] = {0, 0, 0, 0};
	GLint packAlignment = 4, unpackAlignment = 4;
	bool scissor = false, multisample = true, dither = true;
	std::vector<std::pair<uint32_t, glm::vec4>> currentAttribs;
	int defaultWidth = 1, defaultHeight = 1;
	VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
	VkFormat r11g11b10 = VK_FORMAT_R16G16B16A16_SFLOAT;
};

State* g = nullptr;
GLVao g_defaultVao;
GLFbo g_defaultFbo;

GLuint NewId() { return g->nextId++; }

GLTex* FindTex(GLuint id)
{
	auto it = g->textures.find(id);
	return it == g->textures.end() ? nullptr : it->second.get();
}

GLTex* FindAttachmentTex(const Attachment& a)
{
	if (!a.id) return nullptr;
	if (a.renderbuffer)
	{
		auto it = g->renderbuffers.find(a.id);
		return it == g->renderbuffers.end() ? nullptr : it->second.get();
	}
	return FindTex(a.id);
}

GLFbo* FindFbo(GLuint id)
{
	if (id == 0) return &g_defaultFbo;
	auto it = g->fbos.find(id);
	return it == g->fbos.end() ? nullptr : it->second.get();
}

GLVao* CurrentVao()
{
	if (g->vao == 0) return &g_defaultVao;
	auto it = g->vaos.find(g->vao);
	return it == g->vaos.end() ? &g_defaultVao : it->second.get();
}

GLBuf* FindBuf(GLuint id)
{
	auto it = g->buffers.find(id);
	return it == g->buffers.end() ? nullptr : it->second.get();
}

GLProg* CurrentProgram()
{
	auto it = g->programs.find(g->program);
	return it == g->programs.end() ? nullptr : it->second.get();
}

Framebuffer* ResolveFbo(GLFbo* f)
{
	if (!f) return nullptr;
	Framebuffer fb{};
	for (uint32_t i = 0; i < kMaxColorAttachments; ++i)
	{
		GLTex* t = FindAttachmentTex(f->color[i]);
		if (t && t->tex.image.image)
		{
			fb.color[i] = &t->tex;
			fb.colorLayer[i] = f->color[i].layer;
		}
	}
	GLTex* d = FindAttachmentTex(f->depth);
	if (d && d->tex.image.image)
	{
		fb.depth = &d->tex;
		fb.depthLayer = f->depth.layer;
	}
	fb.drawMask = f->drawMask;
	f->fb = fb;
	return &f->fb;
}

Texture* ReadAttachment(GLFbo* f, uint32_t& layer)
{
	if (!f || f->readBuffer < 0) return nullptr;
	GLTex* t = FindAttachmentTex(f->color[f->readBuffer]);
	if (!t || !t->tex.image.image) return nullptr;
	layer = f->color[f->readBuffer].layer;
	return &t->tex;
}

void EnsureFrame()
{
	if (!GetCore().frameOpen)
	{
		GetCore().BeginFrame();
		GetContext().BeginFrame();
	}
}

// ---------------------------------------------------------------------------
// Formats / samplers
// ---------------------------------------------------------------------------
bool FormatSupports(VkFormat f, VkFormatFeatureFlags features)
{
	VkFormatProperties p{};
	vkGetPhysicalDeviceFormatProperties(GetCore().physical, f, &p);
	return (p.optimalTilingFeatures & features) == features;
}

VkFormat ToVkFormat(GLenum internalFormat)
{
	switch (internalFormat)
	{
	case GL_RGBA16F: case GL_RGB16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
	case GL_RGBA32F: case GL_RGB32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
	case GL_R11F_G11F_B10F: return g->r11g11b10;
	case GL_R32F: return VK_FORMAT_R32_SFLOAT;
	case GL_R16F: return VK_FORMAT_R16_SFLOAT;
	case GL_RG16F: return VK_FORMAT_R16G16_SFLOAT;
	case GL_R8: case GL_RED: return VK_FORMAT_R8_UNORM;
	case GL_RG8: case GL_RG: return VK_FORMAT_R8G8_UNORM;
	case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32:
	case GL_DEPTH_COMPONENT32F:
		return g->depthFormat;
	default: return VK_FORMAT_R8G8B8A8_UNORM;
	}
}

VkSamplerAddressMode ToWrap(GLint w)
{
	switch (w)
	{
	case GL_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case GL_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	case GL_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}
}

void ApplyFilter(SamplerState& s, GLint minFilter, GLint magFilter)
{
	s.magFilter = magFilter == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	switch (minFilter)
	{
	case GL_NEAREST: s.minFilter = VK_FILTER_NEAREST; s.useMips = false; break;
	case GL_LINEAR: s.minFilter = VK_FILTER_LINEAR; s.useMips = false; break;
	case GL_NEAREST_MIPMAP_NEAREST: s.minFilter = VK_FILTER_NEAREST; s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; s.useMips = true; break;
	case GL_LINEAR_MIPMAP_NEAREST: s.minFilter = VK_FILTER_LINEAR; s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; s.useMips = true; break;
	case GL_NEAREST_MIPMAP_LINEAR: s.minFilter = VK_FILTER_NEAREST; s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; s.useMips = true; break;
	default: s.minFilter = VK_FILTER_LINEAR; s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; s.useMips = true; break;
	}
	if (s.useMips) s.mipmapMode = s.mipmapMode; // explicit
	else s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

void UpdateSampler(GLTex& t)
{
	SamplerState s;
	ApplyFilter(s, t.minFilter, t.magFilter);
	s.wrapU = ToWrap(t.wrapS);
	s.wrapV = ToWrap(t.wrapT);
	s.wrapW = ToWrap(t.wrapR);
	s.borderWhite = t.borderWhite;
	// Depth/integer formats cannot be linearly filtered on every device.
	if (t.tex.image.image && !FormatSupports(t.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
	{
		s.magFilter = VK_FILTER_NEAREST;
		s.minFilter = VK_FILTER_NEAREST;
		s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	}
	t.tex.sampler = s;
}

uint32_t FullMipCount(int w, int h)
{
	uint32_t m = 1;
	int s = std::max(w, h);
	while (s > 1) { s >>= 1; ++m; }
	return m;
}

// (Re)creates the image of a texture object if its shape changed.
bool Specify(GLTex& t, ImageKind kind, VkFormat format, int width, int height, int layers, uint32_t mips, VkSampleCountFlagBits samples)
{
	width = std::max(1, width);
	height = std::max(1, height);
	layers = std::max(1, layers);
	Image& img = t.tex.image;
	if (img.image && img.kind == kind && img.format == format && (int)img.width == width && (int)img.height == height &&
		(kind == ImageKind::Cube || (int)img.layers == layers) && img.samples == samples && img.mips >= mips)
		return true;
	Context& ctx = GetContext();
	ctx.EndRenderPass();
	if (img.image)
	{
		ctx.ForgetImage(img.serial);
		GetCore().DestroyImage(img, true);
	}
	const SamplerState sampler = t.tex.sampler;
	ImageDesc desc;
	desc.kind = kind;
	desc.format = format;
	desc.width = (uint32_t)width;
	desc.height = (uint32_t)height;
	desc.layers = (uint32_t)layers;
	desc.mips = mips;
	desc.samples = samples;
	desc.renderTarget = true;
	if (!GetCore().CreateImage(desc, img))
		return false;
	t.tex.sampler = sampler;
	t.format = format;
	t.width = width;
	t.height = height;
	t.layers = (int)img.layers;
	t.samples = samples;
	UpdateSampler(t);
	return true;
}

// Uploads tightly packed pixels into a region of one layer, mip 0.
void UploadRegion(Image& img, uint32_t layer, int x, int y, int w, int h, const void* pixels, size_t bytes)
{
	Core& core = GetCore();
	Buffer staging;
	if (!core.CreateStaging(pixels, bytes, staging)) return;
	if (core.onBeforeTransfer) core.onBeforeTransfer();
	VkCommandBuffer cmd = core.Cmd();
	core.Transition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy region{};
	region.imageSubresource = {img.aspect, 0, layer, 1};
	region.imageOffset = {x, y, 0};
	region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
	vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	core.Transition(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	core.DestroyBuffer(staging, true);
}

// Converts client pixels to the texel layout of the Vulkan format (RGBA8 only : the renderer uploads RGBA/RGB bytes).
bool ConvertPixels(GLenum format, GLenum type, const void* src, int w, int h, std::vector<unsigned char>& out)
{
	if (type != GL_UNSIGNED_BYTE || !src) return false;
	const size_t n = (size_t)w * (size_t)h;
	out.resize(n * 4);
	const unsigned char* s = static_cast<const unsigned char*>(src);
	if (format == GL_RGBA) { std::memcpy(out.data(), s, n * 4); return true; }
	if (format == GL_RGB)
	{
		const size_t rowIn = (size_t)w * 3;
		const size_t stride = g->unpackAlignment > 1 ? (rowIn + g->unpackAlignment - 1) / g->unpackAlignment * g->unpackAlignment : rowIn;
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const unsigned char* p = s + (size_t)y * stride + (size_t)x * 3;
				unsigned char* d = out.data() + ((size_t)y * w + x) * 4;
				d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 255;
			}
		return true;
	}
	if (format == GL_RED)
	{
		for (size_t i = 0; i < n; ++i) { out[i * 4] = s[i]; out[i * 4 + 1] = 0; out[i * 4 + 2] = 0; out[i * 4 + 3] = 255; }
		return true;
	}
	return false;
}

GLTex* BoundTexture(GLenum target)
{
	State::UnitBinding& u = g->units[g->activeUnit];
	switch (target)
	{
	case GL_TEXTURE_CUBE_MAP:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_X: case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Y: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
		return FindTex(u.cube);
	case GL_TEXTURE_2D_ARRAY: return FindTex(u.array);
	case GL_TEXTURE_2D_MULTISAMPLE: return FindTex(u.ms);
	default: return FindTex(u.tex2D);
	}
}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------
VkBuffer ResolveBuffer(GLBuf* b, VkDeviceSize& offset)
{
	offset = 0;
	if (!b || b->data.empty()) return VK_NULL_HANDLE;
	Core& core = GetCore();
	// A buffer that stopped changing goes back to a device local copy.
	if (b->dynamic && !b->dirty && core.frameNumber > b->lastWriteFrame + 8)
	{
		b->dynamic = false;
		b->dirty = true;
		b->uploaded = false;
	}
	if (b->dynamic)
	{
		if (b->dirty || b->ringFrame != core.frameNumber)
		{
			const size_t align = std::max<size_t>(256, core.properties.limits.minUniformBufferOffsetAlignment);
			if (!GetContext().AllocateTransient(b->data.data(), b->data.size(), b->current, b->offset, align))
				return VK_NULL_HANDLE;
			b->ringFrame = core.frameNumber;
			b->dirty = false;
		}
	}
	else if (b->dirty)
	{
		if (b->uploaded)
		{
			// Second version of the data: switch to per-frame copies.
			b->dynamic = true;
			if (b->dedicated) core.DestroyBuffer(b->dedicated, true);
			return ResolveBuffer(b, offset);
		}
		if (b->dedicated) core.DestroyBuffer(b->dedicated, true);
		if (!core.CreateBufferWithData(b->data.data(), b->data.size(),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, b->dedicated))
			return VK_NULL_HANDLE;
		b->current = b->dedicated.buffer;
		b->offset = 0;
		b->dirty = false;
		b->uploaded = true;
	}
	offset = b->offset;
	return b->current;
}

void MarkWritten(GLBuf* b)
{
	const uint64_t frame = GetCore().frameNumber;
	if (b->uploaded && !b->dynamic && frame <= b->lastWriteFrame + 8) b->dynamic = true;
	b->dirty = true;
	b->lastWriteFrame = frame;
}

GLuint& BufferTarget(GLenum target)
{
	static GLuint dummy = 0;
	switch (target)
	{
	case GL_ARRAY_BUFFER: return g->arrayBuffer;
	case GL_ELEMENT_ARRAY_BUFFER: return CurrentVao()->elementBuffer;
	case GL_UNIFORM_BUFFER: return g->uniformBuffer;
	default: dummy = 0; return dummy;
	}
}

VkFormat AttribFormat(const Attrib& a)
{
	const int n = std::clamp(a.size, 1, 4);
	if (a.integer)
	{
		const bool sign = a.type == GL_INT;
		static const VkFormat u[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
		static const VkFormat s[4] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT};
		return sign ? s[n - 1] : u[n - 1];
	}
	if (a.type == GL_UNSIGNED_BYTE)
	{
		static const VkFormat un[4] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
		static const VkFormat us[4] = {VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED};
		return a.normalized ? un[n - 1] : us[n - 1];
	}
	static const VkFormat f[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
	return f[n - 1];
}

uint32_t AttribSize(const Attrib& a)
{
	const uint32_t comp = a.type == GL_UNSIGNED_BYTE || a.type == GL_BYTE ? 1u : 4u;
	return comp * (uint32_t)std::clamp(a.size, 1, 4);
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
void PrepareDraw(VertexArray& va, VulkanProgram*& program)
{
	EnsureFrame();
	Context& ctx = GetContext();
	ctx.BindFramebuffer(ResolveFbo(FindFbo(g->drawFbo)));
	GLProg* p = CurrentProgram();
	program = (p && p->linked) ? p->program.get() : nullptr;

	GLVao* vao = CurrentVao();
	for (uint32_t i = 0; i < kMaxAttribs; ++i)
	{
		const Attrib& a = vao->attribs[i];
		if (!a.enabled) continue;
		VkDeviceSize off = 0;
		VkBuffer buf = ResolveBuffer(FindBuf(a.buffer), off);
		if (!buf) continue;
		const uint32_t stride = a.stride ? (uint32_t)a.stride : AttribSize(a);
		va.bindings.push_back({i, stride, a.divisor > 0});
		va.attribs.push_back({i, i, AttribFormat(a), 0});
		va.buffers[i] = buf;
		va.offsets[i] = off + a.offset;
	}
	for (const auto& c : g->currentAttribs)
	{
		if (c.first < kMaxAttribs && vao->attribs[c.first].enabled) continue;
		va.constants.push_back(c);
	}
	if (vao->elementBuffer)
	{
		VkDeviceSize off = 0;
		va.indexBuffer = ResolveBuffer(FindBuf(vao->elementBuffer), off);
		va.indexOffset = off;
	}
	for (uint32_t pt = 0; pt < kUniformBlockPoints; ++pt)
	{
		GLBuf* b = FindBuf(g->uboPoints[pt]);
		VkDeviceSize off = 0;
		VkBuffer buf = ResolveBuffer(b, off);
		if (buf)
		{
			const VkDeviceSize range = std::min<VkDeviceSize>(b->data.size(), GetCore().properties.limits.maxUniformBufferRange);
			ctx.BindUniformBuffer(pt, buf, off, range);
		}
		else
			ctx.BindUniformBuffer(pt, VK_NULL_HANDLE, 0, 0);
	}
}

Topology ToTopology(GLenum mode) { return mode == GL_LINES ? Topology::Lines : Topology::Triangles; }

// ---------------------------------------------------------------------------
// Default framebuffer
// ---------------------------------------------------------------------------
void CreateDefaultFramebuffer(int width, int height)
{
	auto ensure = [](GLuint id) -> GLTex& {
		auto& p = g->textures[id];
		if (!p) { p = std::make_unique<GLTex>(); p->target = GL_TEXTURE_2D; p->minFilter = GL_NEAREST; p->magFilter = GL_NEAREST; }
		return *p;
	};
	GLTex& c = ensure(kDefaultColorId);
	GLTex& d = ensure(kDefaultDepthId);
	Specify(c, ImageKind::Tex2D, VK_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, VK_SAMPLE_COUNT_1_BIT);
	Specify(d, ImageKind::Tex2D, g->depthFormat, width, height, 1, 1, VK_SAMPLE_COUNT_1_BIT);
	g_defaultFbo = GLFbo{};
	g_defaultFbo.color[0].id = kDefaultColorId;
	g_defaultFbo.depth.id = kDefaultDepthId;
	g_defaultFbo.drawMask = 1;
	g_defaultFbo.readBuffer = 0;
	g->defaultWidth = width;
	g->defaultHeight = height;
}

void DestroyTex(GLTex& t)
{
	Context& ctx = GetContext();
	ctx.EndRenderPass();
	ctx.UnbindTexture(&t.tex);
	if (t.tex.image.image)
	{
		ctx.ForgetImage(t.tex.image.serial);
		GetCore().DestroyImage(t.tex.image, true);
	}
}
} // namespace

// ===========================================================================
// Runtime
// ===========================================================================
namespace vkgl
{
bool Init(uint32_t width, uint32_t height)
{
	if (g && g->initialized) return true;
	Core& core = GetCore();
	if (!core.Init(width, height)) return false;
	if (!GetContext().Init()) { core.Shutdown(); return false; }
	g = new State();
	g_defaultVao = GLVao{};
	const VkFormatFeatureFlags depthFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	// GL_DEPTH_COMPONENT24 : same precision as the OpenGL backend when available.
	g->depthFormat = FormatSupports(VK_FORMAT_X8_D24_UNORM_PACK32, depthFeatures) ? VK_FORMAT_X8_D24_UNORM_PACK32 : VK_FORMAT_D32_SFLOAT;
	const VkFormatFeatureFlags colorFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
	g->r11g11b10 = FormatSupports(VK_FORMAT_B10G11R11_UFLOAT_PACK32, colorFeatures) ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : VK_FORMAT_R16G16B16A16_SFLOAT;
	CreateDefaultFramebuffer((int)std::max(1u, width), (int)std::max(1u, height));
	g->viewport[2] = (GLint)width;
	g->viewport[3] = (GLint)height;
	GetContext().Viewport(0, 0, (int)width, (int)height);
	g->initialized = true;
	return true;
}

bool IsInitialized() { return g && g->initialized; }

void Shutdown()
{
	if (!g) return;
	Core& core = GetCore();
	if (core.frameOpen) core.EndFrame();
	if (core.device) vkDeviceWaitIdle(core.device);
	for (auto& [id, t] : g->textures) if (t->tex.image.image) core.DestroyImage(t->tex.image, false);
	for (auto& [id, t] : g->renderbuffers) if (t->tex.image.image) core.DestroyImage(t->tex.image, false);
	for (auto& [id, b] : g->buffers) if (b->dedicated) core.DestroyBuffer(b->dedicated, false);
	g->programs.clear();
	delete g;
	g = nullptr;
	g_defaultFbo = GLFbo{};
	GetContext().Shutdown();
	core.Shutdown();
}

void BeginFrame()
{
	if (!g) return;
	EnsureFrame();
}

void EndFrame()
{
	if (!g) return;
	Core& core = GetCore();
	if (!core.frameOpen)
	{
		if (!core.swapchain) return;
		EnsureFrame();
	}
	GLTex* color = FindTex(kDefaultColorId);
	if (color && core.swapchainImageAcquired)
		GetContext().PresentToSwapchain(color->tex, g->defaultWidth, g->defaultHeight);
	core.EndFrame();
}

void Resize(int width, int height)
{
	if (!g || width <= 0 || height <= 0) return;
	Core& core = GetCore();
	CreateDefaultFramebuffer(width, height);
	if (core.swapchain || core.surface)
	{
		if (core.frameOpen) core.EndFrame();
		core.RecreateSwapchain((uint32_t)width, (uint32_t)height);
	}
}
} // namespace vkgl

// ===========================================================================
// OpenGL entry points
// ===========================================================================
int vkgl_LoadLoader(GLADloadproc) { return vkgl::IsInitialized() ? 1 : 0; }

GLenum vkgl_GetError() { return GL_NO_ERROR; }
void vkgl_Finish() {}
void vkgl_Flush() {}

// ---- Enable / state -------------------------------------------------------
static void SetCap(GLenum cap, bool on)
{
	RasterState& s = GetContext().state;
	switch (cap)
	{
	case GL_DEPTH_TEST: s.depthTest = on; break;
	case GL_BLEND: s.blend = on; break;
	case GL_CULL_FACE: s.cull = on; break;
	case GL_POLYGON_OFFSET_FILL: s.polygonOffset = on; break;
	case GL_SCISSOR_TEST: g->scissor = on; break;
	case GL_MULTISAMPLE: g->multisample = on; break;
	case GL_DITHER: g->dither = on; break;
	default: break;
	}
}
void vkgl_Enable(GLenum cap) { SetCap(cap, true); }
void vkgl_Disable(GLenum cap) { SetCap(cap, false); }
GLboolean vkgl_IsEnabled(GLenum cap)
{
	const RasterState& s = GetContext().state;
	switch (cap)
	{
	case GL_DEPTH_TEST: return s.depthTest;
	case GL_BLEND: return s.blend;
	case GL_CULL_FACE: return s.cull;
	case GL_POLYGON_OFFSET_FILL: return s.polygonOffset;
	case GL_SCISSOR_TEST: return g->scissor;
	case GL_MULTISAMPLE: return g->multisample;
	case GL_DITHER: return g->dither;
	default: return GL_FALSE;
	}
}

static VkBlendFactor ToBlend(GLenum f)
{
	switch (f)
	{
	case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
	case GL_ONE: return VK_BLEND_FACTOR_ONE;
	case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	default: return VK_BLEND_FACTOR_ONE;
	}
}
void vkgl_BlendFunc(GLenum s, GLenum d) { GetContext().state.BlendFunc(ToBlend(s), ToBlend(d)); }
void vkgl_BlendFuncSeparate(GLenum s, GLenum d, GLenum, GLenum) { vkgl_BlendFunc(s, d); }
void vkgl_BlendEquation(GLenum) {}

void vkgl_DepthFunc(GLenum func)
{
	VkCompareOp op = VK_COMPARE_OP_LESS;
	switch (func)
	{
	case GL_NEVER: op = VK_COMPARE_OP_NEVER; break;
	case GL_LESS: op = VK_COMPARE_OP_LESS; break;
	case GL_EQUAL: op = VK_COMPARE_OP_EQUAL; break;
	case GL_LEQUAL: op = VK_COMPARE_OP_LESS_OR_EQUAL; break;
	case GL_GREATER: op = VK_COMPARE_OP_GREATER; break;
	case GL_NOTEQUAL: op = VK_COMPARE_OP_NOT_EQUAL; break;
	case GL_GEQUAL: op = VK_COMPARE_OP_GREATER_OR_EQUAL; break;
	case GL_ALWAYS: op = VK_COMPARE_OP_ALWAYS; break;
	}
	GetContext().state.depthFunc = op;
}
void vkgl_DepthMask(GLboolean flag) { GetContext().state.depthWrite = flag != GL_FALSE; }
void vkgl_CullFace(GLenum mode)
{
	GetContext().state.cullFace = mode == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : mode == GL_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
}
void vkgl_PolygonMode(GLenum, GLenum mode) { GetContext().state.wireframe = mode == GL_LINE; }
void vkgl_PolygonOffset(GLfloat factor, GLfloat units) { GetContext().state.offsetFactor = factor; GetContext().state.offsetUnits = units; }
void vkgl_LineWidth(GLfloat width) { GetContext().state.lineWidth = width; }
void vkgl_ColorMaski(GLuint index, GLboolean r, GLboolean gr, GLboolean b, GLboolean a)
{
	if (index >= kMaxColorAttachments) return;
	GetContext().state.colorMask[index] = (uint8_t)((r ? 1 : 0) | (gr ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
}
void vkgl_Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	g->viewport[0] = x; g->viewport[1] = y; g->viewport[2] = w; g->viewport[3] = h;
	GetContext().Viewport(x, y, w, h);
}
void vkgl_Scissor(GLint, GLint, GLsizei, GLsizei) {}
void vkgl_PixelStorei(GLenum pname, GLint param)
{
	if (pname == GL_PACK_ALIGNMENT) g->packAlignment = param;
	else if (pname == GL_UNPACK_ALIGNMENT) g->unpackAlignment = param;
}
void vkgl_ClearColor(GLfloat r, GLfloat gr, GLfloat b, GLfloat a) { g->clearColor = glm::vec4(r, gr, b, a); }

void vkgl_GetIntegerv(GLenum pname, GLint* data)
{
	if (!data) return;
	switch (pname)
	{
	case GL_MAX_TEXTURE_IMAGE_UNITS: *data = 32; break;
	case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *data = 32; break;
	case GL_MAX_SAMPLES:
	{
		const VkSampleCountFlags s = GetCore().supportedSamples;
		*data = (s & VK_SAMPLE_COUNT_8_BIT) ? 8 : (s & VK_SAMPLE_COUNT_4_BIT) ? 4 : (s & VK_SAMPLE_COUNT_2_BIT) ? 2 : 1;
		break;
	}
	case GL_DRAW_FRAMEBUFFER_BINDING: *data = (GLint)g->drawFbo; break;
	case GL_READ_FRAMEBUFFER_BINDING: *data = (GLint)g->readFbo; break;
	case GL_TEXTURE_BINDING_2D: *data = (GLint)g->units[g->activeUnit].tex2D; break;
	case GL_ACTIVE_TEXTURE: *data = (GLint)(GL_TEXTURE0 + g->activeUnit); break;
	case GL_READ_BUFFER:
	{
		GLFbo* f = FindFbo(g->readFbo);
		*data = !f || f->readBuffer < 0 ? GL_NONE : (g->readFbo == 0 ? GL_BACK : (GLint)(GL_COLOR_ATTACHMENT0 + f->readBuffer));
		break;
	}
	case GL_PACK_ALIGNMENT: *data = g->packAlignment; break;
	case GL_UNPACK_ALIGNMENT: *data = g->unpackAlignment; break;
	case GL_VIEWPORT: std::memcpy(data, g->viewport, sizeof(g->viewport)); break;
	case GL_CURRENT_PROGRAM: *data = (GLint)g->program; break;
	case GL_VERTEX_ARRAY_BINDING: *data = (GLint)g->vao; break;
	case GL_ARRAY_BUFFER_BINDING: *data = (GLint)g->arrayBuffer; break;
	case GL_ELEMENT_ARRAY_BUFFER_BINDING: *data = (GLint)CurrentVao()->elementBuffer; break;
	case GL_RENDERBUFFER_BINDING: *data = (GLint)g->renderbuffer; break;
	case GL_MAX_DRAW_BUFFERS: case GL_MAX_COLOR_ATTACHMENTS: *data = (GLint)kMaxColorAttachments; break;
	case GL_MAX_TEXTURE_SIZE: *data = (GLint)GetCore().properties.limits.maxImageDimension2D; break;
	case GL_MAX_UNIFORM_BLOCK_SIZE: *data = (GLint)std::min<uint32_t>(GetCore().properties.limits.maxUniformBufferRange, 0x7FFFFFFF); break;
	default: *data = 0; break;
	}
}

// ---- Textures -------------------------------------------------------------
void vkgl_GenTextures(GLsizei n, GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		ids[i] = NewId();
		g->textures[ids[i]] = std::make_unique<GLTex>();
	}
}

void vkgl_DeleteTextures(GLsizei n, const GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		const GLuint id = ids[i];
		auto it = g->textures.find(id);
		if (id == 0 || it == g->textures.end() || id == kDefaultColorId || id == kDefaultDepthId) continue;
		DestroyTex(*it->second);
		for (auto& u : g->units)
		{
			if (u.tex2D == id) u.tex2D = 0;
			if (u.cube == id) u.cube = 0;
			if (u.array == id) u.array = 0;
			if (u.ms == id) u.ms = 0;
		}
		for (auto& [fid, f] : g->fbos)
		{
			for (auto& a : f->color) if (!a.renderbuffer && a.id == id) a = Attachment{};
			if (!f->depth.renderbuffer && f->depth.id == id) f->depth = Attachment{};
		}
		g->textures.erase(it);
	}
}

void vkgl_ActiveTexture(GLenum texture) { g->activeUnit = std::min<GLuint>(texture - GL_TEXTURE0, 31); }

void vkgl_BindTexture(GLenum target, GLuint id)
{
	State::UnitBinding& u = g->units[g->activeUnit];
	GLTex* t = id ? FindTex(id) : nullptr;
	if (t && t->target == 0) t->target = target;
	Context& ctx = GetContext();
	switch (target)
	{
	case GL_TEXTURE_CUBE_MAP: u.cube = id; ctx.BindTexture(g->activeUnit, 1, t ? &t->tex : nullptr); break;
	case GL_TEXTURE_2D_ARRAY: u.array = id; ctx.BindTexture(g->activeUnit, 2, t ? &t->tex : nullptr); break;
	case GL_TEXTURE_2D_MULTISAMPLE: u.ms = id; ctx.BindTexture(g->activeUnit, 0, t ? &t->tex : nullptr); break;
	default: u.tex2D = id; ctx.BindTexture(g->activeUnit, 0, t ? &t->tex : nullptr); break;
	}
}

void vkgl_TexParameteri(GLenum target, GLenum pname, GLint param)
{
	GLTex* t = BoundTexture(target);
	if (!t) return;
	switch (pname)
	{
	case GL_TEXTURE_MIN_FILTER: t->minFilter = param; break;
	case GL_TEXTURE_MAG_FILTER: t->magFilter = param; break;
	case GL_TEXTURE_WRAP_S: t->wrapS = param; break;
	case GL_TEXTURE_WRAP_T: t->wrapT = param; break;
	case GL_TEXTURE_WRAP_R: t->wrapR = param; break;
	default: return;
	}
	UpdateSampler(*t);
}

void vkgl_TexParameterfv(GLenum target, GLenum pname, const GLfloat* params)
{
	GLTex* t = BoundTexture(target);
	if (!t || !params) return;
	if (pname == GL_TEXTURE_BORDER_COLOR)
	{
		t->borderWhite = params[0] > 0.5f || params[3] > 0.5f;
		UpdateSampler(*t);
	}
}

void vkgl_TexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint, GLenum format, GLenum type, const void* pixels)
{
	if (level != 0) return;
	GLTex* t = BoundTexture(target);
	if (!t) return;
	const VkFormat vf = ToVkFormat((GLenum)internalformat);
	const bool cubeFace = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
	const bool colorData = pixels && vf == VK_FORMAT_R8G8B8A8_UNORM;
	const uint32_t mips = colorData && !cubeFace ? FullMipCount(width, height) : 1;
	if (!Specify(*t, cubeFace ? ImageKind::Cube : ImageKind::Tex2D, vf, width, height, 1, mips, VK_SAMPLE_COUNT_1_BIT))
		return;
	const uint32_t layer = cubeFace ? (uint32_t)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0u;
	if (pixels && vf == VK_FORMAT_R32G32B32A32_SFLOAT && type == GL_FLOAT && format == GL_RGBA)
	{
		// Float data (GI scene textures) : same layout in Vulkan.
		UploadRegion(t->tex.image, layer, 0, 0, width, height, pixels, (size_t)width * (size_t)height * 16u);
		return;
	}
	std::vector<unsigned char> rgba;
	if (colorData && ConvertPixels(format, type, pixels, width, height, rgba))
		UploadRegion(t->tex.image, layer, 0, 0, width, height, rgba.data(), rgba.size());
}

void vkgl_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels)
{
	if (level != 0) return;
	GLTex* t = BoundTexture(target);
	if (!t || !t->tex.image.image) return;
	std::vector<unsigned char> rgba;
	if (ConvertPixels(format, type, pixels, w, h, rgba))
	{
		const bool cubeFace = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
		UploadRegion(t->tex.image, cubeFace ? (uint32_t)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0u, x, y, w, h, rgba.data(), rgba.size());
	}
}

void vkgl_TexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint, GLenum, GLenum, const void*)
{
	if (level != 0 || target != GL_TEXTURE_2D_ARRAY) return;
	GLTex* t = BoundTexture(target);
	if (!t) return;
	const VkFormat vf = ToVkFormat((GLenum)internalformat);
	Specify(*t, ImageKind::Array2D, vf, width, height, depth, vf == VK_FORMAT_R8G8B8A8_UNORM ? FullMipCount(width, height) : 1, VK_SAMPLE_COUNT_1_BIT);
}

void vkgl_TexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, const void* pixels)
{
	if (level != 0) return;
	GLTex* t = BoundTexture(target);
	if (!t || !t->tex.image.image) return;
	const size_t layerSize = (size_t)w * (size_t)h;
	for (GLsizei layer = 0; layer < d; ++layer)
	{
		std::vector<unsigned char> rgba;
		const unsigned char* src = static_cast<const unsigned char*>(pixels) + layerSize * 4 * (size_t)layer;
		if (ConvertPixels(format, type, src, w, h, rgba))
			UploadRegion(t->tex.image, (uint32_t)(z + layer), x, y, w, h, rgba.data(), rgba.size());
	}
}

static VkSampleCountFlagBits ToSamples(GLsizei samples)
{
	const VkSampleCountFlags s = GetCore().supportedSamples;
	if (samples >= 8 && (s & VK_SAMPLE_COUNT_8_BIT)) return VK_SAMPLE_COUNT_8_BIT;
	if (samples >= 4 && (s & VK_SAMPLE_COUNT_4_BIT)) return VK_SAMPLE_COUNT_4_BIT;
	if (samples >= 2 && (s & VK_SAMPLE_COUNT_2_BIT)) return VK_SAMPLE_COUNT_2_BIT;
	return VK_SAMPLE_COUNT_1_BIT;
}

void vkgl_TexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean)
{
	GLTex* t = BoundTexture(target);
	if (!t) return;
	Specify(*t, ImageKind::Tex2D, ToVkFormat(internalformat), width, height, 1, 1, ToSamples(samples));
}

void vkgl_GenerateMipmap(GLenum target)
{
	GLTex* t = BoundTexture(target);
	if (!t || !t->tex.image.image) return;
	Core& core = GetCore();
	Image& img = t->tex.image;
	const uint32_t full = FullMipCount((int)img.width, (int)img.height);
	if (full <= 1) return;
	if (img.mips < full)
	{
		// Reallocate with a full chain and keep level 0.
		Image old = img;
		img = Image{};
		ImageDesc desc;
		desc.kind = old.kind;
		desc.format = old.format;
		desc.width = old.width;
		desc.height = old.height;
		desc.layers = old.kind == ImageKind::Cube ? 6 : old.layers;
		desc.mips = full;
		desc.renderTarget = true;
		if (!core.CreateImage(desc, img)) { img = old; return; }
		if (core.onBeforeTransfer) core.onBeforeTransfer();
		VkCommandBuffer cmd = core.Cmd();
		core.Transition(cmd, old, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		core.Transition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageCopy c{};
		c.srcSubresource = {old.aspect, 0, 0, old.layers};
		c.dstSubresource = {img.aspect, 0, 0, old.layers};
		c.extent = {old.width, old.height, 1};
		vkCmdCopyImage(cmd, old.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		GetContext().ForgetImage(old.serial);
		core.DestroyImage(old, true);
	}
	core.GenerateMips(img);
}

void vkgl_CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
	if (level != 0) return;
	GLTex* t = BoundTexture(target);
	uint32_t layer = 0;
	Texture* src = ReadAttachment(FindFbo(g->readFbo), layer);
	if (!t || !t->tex.image.image || !src) return;
	EnsureFrame();
	GetContext().BlitColor(*src, layer, t->tex, 0, x, y, x + width, y + height, xoffset, yoffset, xoffset + width, yoffset + height, false);
}

// ---- Samplers -------------------------------------------------------------
void vkgl_GenSamplers(GLsizei n, GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		ids[i] = NewId();
		auto s = std::make_unique<SamplerState>();
		ApplyFilter(*s, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR);
		g->samplers[ids[i]] = std::move(s);
	}
}
void vkgl_DeleteSamplers(GLsizei n, const GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		auto it = g->samplers.find(ids[i]);
		if (it == g->samplers.end()) continue;
		for (GLuint u = 0; u < 32; ++u)
			if (g->units[u].sampler == ids[i]) { g->units[u].sampler = 0; GetContext().BindSampler(u, nullptr); }
		g->samplers.erase(it);
	}
}
void vkgl_SamplerParameteri(GLuint sampler, GLenum pname, GLint param)
{
	auto it = g->samplers.find(sampler);
	if (it == g->samplers.end()) return;
	SamplerState& s = *it->second;
	switch (pname)
	{
	case GL_TEXTURE_MIN_FILTER:
	{
		const VkFilter mag = s.magFilter;
		ApplyFilter(s, param, GL_LINEAR);
		s.magFilter = mag;
		break;
	}
	case GL_TEXTURE_MAG_FILTER: s.magFilter = param == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; break;
	case GL_TEXTURE_WRAP_S: s.wrapU = ToWrap(param); break;
	case GL_TEXTURE_WRAP_T: s.wrapV = ToWrap(param); break;
	case GL_TEXTURE_WRAP_R: s.wrapW = ToWrap(param); break;
	default: break;
	}
}
void vkgl_BindSampler(GLuint unit, GLuint sampler)
{
	if (unit >= 32) return;
	g->units[unit].sampler = sampler;
	auto it = g->samplers.find(sampler);
	GetContext().BindSampler(unit, it == g->samplers.end() ? nullptr : it->second.get());
}

// ---- Renderbuffers --------------------------------------------------------
void vkgl_GenRenderbuffers(GLsizei n, GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		ids[i] = NewId();
		g->renderbuffers[ids[i]] = std::make_unique<GLTex>();
	}
}
void vkgl_DeleteRenderbuffers(GLsizei n, const GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		auto it = g->renderbuffers.find(ids[i]);
		if (it == g->renderbuffers.end()) continue;
		DestroyTex(*it->second);
		for (auto& [fid, f] : g->fbos)
		{
			for (auto& a : f->color) if (a.renderbuffer && a.id == ids[i]) a = Attachment{};
			if (f->depth.renderbuffer && f->depth.id == ids[i]) f->depth = Attachment{};
		}
		g->renderbuffers.erase(it);
	}
}
void vkgl_BindRenderbuffer(GLenum, GLuint id) { g->renderbuffer = id; }
void vkgl_RenderbufferStorageMultisample(GLenum, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height)
{
	auto it = g->renderbuffers.find(g->renderbuffer);
	if (it == g->renderbuffers.end()) return;
	Specify(*it->second, ImageKind::Tex2D, ToVkFormat(internalformat), width, height, 1, 1, ToSamples(samples));
}
void vkgl_RenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height)
{
	vkgl_RenderbufferStorageMultisample(target, 0, internalformat, width, height);
}

// ---- Framebuffers ---------------------------------------------------------
void vkgl_GenFramebuffers(GLsizei n, GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		ids[i] = NewId();
		g->fbos[ids[i]] = std::make_unique<GLFbo>();
	}
}
void vkgl_DeleteFramebuffers(GLsizei n, const GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		auto it = g->fbos.find(ids[i]);
		if (it == g->fbos.end()) continue;
		GetContext().EndRenderPass();
		if (GetContext().BoundFramebuffer() == &it->second->fb) GetContext().BindFramebuffer(nullptr);
		if (g->drawFbo == ids[i]) g->drawFbo = 0;
		if (g->readFbo == ids[i]) g->readFbo = 0;
		g->fbos.erase(it);
	}
}
void vkgl_BindFramebuffer(GLenum target, GLuint id)
{
	if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) g->drawFbo = id;
	if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) g->readFbo = id;
}
static GLFbo* TargetFbo(GLenum target)
{
	return FindFbo(target == GL_READ_FRAMEBUFFER ? g->readFbo : g->drawFbo);
}
static void Attach(GLenum target, GLenum attachment, Attachment a)
{
	GLFbo* f = TargetFbo(target);
	if (!f || f == &g_defaultFbo) return;
	if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT) f->depth = a;
	else if (attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + kMaxColorAttachments)
		f->color[attachment - GL_COLOR_ATTACHMENT0] = a;
}
void vkgl_FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint)
{
	Attachment a;
	a.id = texture;
	if (textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
		a.layer = textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
	Attach(target, attachment, a);
}
void vkgl_FramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint, GLint layer)
{
	Attachment a;
	a.id = texture;
	a.layer = (uint32_t)std::max(0, layer);
	Attach(target, attachment, a);
}
void vkgl_FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum, GLuint rb)
{
	Attachment a;
	a.id = rb;
	a.renderbuffer = true;
	Attach(target, attachment, a);
}
GLenum vkgl_CheckFramebufferStatus(GLenum) { return GL_FRAMEBUFFER_COMPLETE; }

static int BufferIndex(GLenum buf)
{
	if (buf == GL_NONE) return -1;
	if (buf == GL_BACK || buf == GL_FRONT || buf == GL_BACK_LEFT || buf == GL_FRONT_LEFT) return 0;
	if (buf >= GL_COLOR_ATTACHMENT0 && buf < GL_COLOR_ATTACHMENT0 + kMaxColorAttachments) return (int)(buf - GL_COLOR_ATTACHMENT0);
	return -1;
}
void vkgl_DrawBuffer(GLenum buf)
{
	GLFbo* f = FindFbo(g->drawFbo);
	if (!f) return;
	const int i = BufferIndex(buf);
	f->drawMask = i < 0 ? 0u : (1u << i);
}
void vkgl_DrawBuffers(GLsizei n, const GLenum* bufs)
{
	GLFbo* f = FindFbo(g->drawFbo);
	if (!f) return;
	uint32_t mask = 0;
	for (GLsizei i = 0; i < n; ++i)
	{
		const int idx = BufferIndex(bufs[i]);
		if (idx >= 0) mask |= 1u << idx;
	}
	f->drawMask = mask;
}
void vkgl_ReadBuffer(GLenum buf)
{
	GLFbo* f = FindFbo(g->readFbo);
	if (f) f->readBuffer = BufferIndex(buf);
}

void vkgl_Clear(GLbitfield mask)
{
	EnsureFrame();
	Context& ctx = GetContext();
	Framebuffer* fb = ResolveFbo(FindFbo(g->drawFbo));
	ctx.BindFramebuffer(fb);
	if (!fb) return;
	if (mask & GL_COLOR_BUFFER_BIT)
		for (uint32_t i = 0; i < kMaxColorAttachments; ++i)
			if (fb->color[i] && (fb->drawMask & (1u << i)) && ctx.state.colorMask[i])
				ctx.ClearColorBuffer(i, g->clearColor);
	if ((mask & GL_DEPTH_BUFFER_BIT) && fb->depth && ctx.state.depthWrite)
		ctx.Clear(false, true, glm::vec4(0.f), 1.f);
}

void vkgl_ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value)
{
	EnsureFrame();
	Context& ctx = GetContext();
	Framebuffer* fb = ResolveFbo(FindFbo(g->drawFbo));
	ctx.BindFramebuffer(fb);
	if (!fb || !value) return;
	if (buffer == GL_COLOR && drawbuffer >= 0)
		ctx.ClearColorBuffer((uint32_t)drawbuffer, glm::vec4(value[0], value[1], value[2], value[3]));
	else if (buffer == GL_DEPTH && fb->depth)
		ctx.Clear(false, true, glm::vec4(0.f), value[0]);
}

void vkgl_BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
	EnsureFrame();
	Context& ctx = GetContext();
	GLFbo* rf = FindFbo(g->readFbo);
	GLFbo* df = FindFbo(g->drawFbo);
	if (!rf || !df) return;
	Framebuffer src = *ResolveFbo(rf);
	Framebuffer dst = *ResolveFbo(df);
	ctx.EndRenderPass();
	if (mask & GL_COLOR_BUFFER_BIT)
	{
		uint32_t srcLayer = 0;
		Texture* s = ReadAttachment(rf, srcLayer);
		if (s)
			for (uint32_t i = 0; i < kMaxColorAttachments; ++i)
				if (dst.color[i] && (dst.drawMask & (1u << i)) && dst.color[i] != s)
					ctx.BlitColor(*s, srcLayer, *dst.color[i], dst.colorLayer[i], sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, filter == GL_LINEAR);
	}
	if ((mask & GL_DEPTH_BUFFER_BIT) && src.depth && dst.depth && src.depth != dst.depth)
		ctx.BlitDepth(*src.depth, *dst.depth, std::min(dx0, dx1), std::min(dy0, dy1), std::max(dx0, dx1), std::max(dy0, dy1));
}

void vkgl_ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
{
	uint32_t layer = 0;
	Texture* src = ReadAttachment(FindFbo(g->readFbo), layer);
	if (!src || !pixels || format != GL_RGBA || type != GL_UNSIGNED_BYTE) return;
	std::vector<unsigned char> rgba;
	if (!GetContext().ReadPixels(*src, x, y, width, height, rgba)) return;
	const int w = std::min<int>(width, (int)src->image.width - std::clamp(x, 0, (int)src->image.width - 1));
	const int h = (int)(rgba.size() / 4 / std::max(1, w));
	const size_t rowOut = (size_t)width * 4;
	for (int row = 0; row < h && row < height; ++row)
		std::memcpy(static_cast<unsigned char*>(pixels) + rowOut * (size_t)row, rgba.data() + (size_t)row * (size_t)w * 4, (size_t)w * 4);
}

// ---- Buffers --------------------------------------------------------------
void vkgl_GenBuffers(GLsizei n, GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		ids[i] = NewId();
		g->buffers[ids[i]] = std::make_unique<GLBuf>();
	}
}
void vkgl_DeleteBuffers(GLsizei n, const GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		auto it = g->buffers.find(ids[i]);
		if (it == g->buffers.end()) continue;
		if (it->second->dedicated) GetCore().DestroyBuffer(it->second->dedicated, true);
		g->buffers.erase(it);
		if (g->arrayBuffer == ids[i]) g->arrayBuffer = 0;
		if (g->uniformBuffer == ids[i]) g->uniformBuffer = 0;
		for (auto& p : g->uboPoints) if (p == ids[i]) p = 0;
	}
}
void vkgl_BindBuffer(GLenum target, GLuint id) { BufferTarget(target) = id; }
void vkgl_BindBufferBase(GLenum target, GLuint index, GLuint id)
{
	if (target != GL_UNIFORM_BUFFER) return;
	g->uniformBuffer = id;
	if (index < kUniformBlockPoints) g->uboPoints[index] = id;
}
void vkgl_BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum)
{
	GLBuf* b = FindBuf(BufferTarget(target));
	if (!b) return;
	b->data.assign((size_t)std::max<GLsizeiptr>(0, size), 0);
	if (data && size > 0) std::memcpy(b->data.data(), data, (size_t)size);
	MarkWritten(b);
}
void vkgl_BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
	GLBuf* b = FindBuf(BufferTarget(target));
	if (!b || !data || offset < 0 || size <= 0 || (size_t)(offset + size) > b->data.size()) return;
	std::memcpy(b->data.data() + offset, data, (size_t)size);
	MarkWritten(b);
}

// ---- Vertex arrays --------------------------------------------------------
void vkgl_GenVertexArrays(GLsizei n, GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		ids[i] = NewId();
		g->vaos[ids[i]] = std::make_unique<GLVao>();
	}
}
void vkgl_DeleteVertexArrays(GLsizei n, const GLuint* ids)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		g->vaos.erase(ids[i]);
		if (g->vao == ids[i]) g->vao = 0;
	}
}
void vkgl_BindVertexArray(GLuint id) { g->vao = id; }
void vkgl_EnableVertexAttribArray(GLuint index) { if (index < kMaxAttribs) CurrentVao()->attribs[index].enabled = true; }
void vkgl_DisableVertexAttribArray(GLuint index) { if (index < kMaxAttribs) CurrentVao()->attribs[index].enabled = false; }
void vkgl_VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer)
{
	if (index >= kMaxAttribs) return;
	Attrib& a = CurrentVao()->attribs[index];
	a.size = size; a.type = type; a.normalized = normalized != GL_FALSE; a.integer = false;
	a.stride = stride; a.offset = (uintptr_t)pointer; a.buffer = g->arrayBuffer;
}
void vkgl_VertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer)
{
	if (index >= kMaxAttribs) return;
	Attrib& a = CurrentVao()->attribs[index];
	a.size = size; a.type = type; a.normalized = false; a.integer = true;
	a.stride = stride; a.offset = (uintptr_t)pointer; a.buffer = g->arrayBuffer;
}
void vkgl_VertexAttribDivisor(GLuint index, GLuint divisor) { if (index < kMaxAttribs) CurrentVao()->attribs[index].divisor = divisor; }
void vkgl_VertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
	for (auto& c : g->currentAttribs) if (c.first == index) { c.second = glm::vec4(x, y, z, w); return; }
	g->currentAttribs.push_back({index, glm::vec4(x, y, z, w)});
}
void vkgl_VertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z) { vkgl_VertexAttrib4f(index, x, y, z, 1.f); }

// ---- Draw -----------------------------------------------------------------
void vkgl_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
	VertexArray va;
	VulkanProgram* program = nullptr;
	PrepareDraw(va, program);
	if (program) GetContext().Draw(program, va, ToTopology(mode), (uint32_t)count, false, 1, (uint32_t)first);
}
void vkgl_DrawElements(GLenum mode, GLsizei count, GLenum, const void* indices)
{
	VertexArray va;
	VulkanProgram* program = nullptr;
	PrepareDraw(va, program);
	if (!program || !va.indexBuffer) return;
	va.indexOffset += (VkDeviceSize)(uintptr_t)indices;
	GetContext().Draw(program, va, ToTopology(mode), (uint32_t)count, true, 1, 0);
}
void vkgl_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum, const void* indices, GLsizei instances)
{
	VertexArray va;
	VulkanProgram* program = nullptr;
	PrepareDraw(va, program);
	if (!program || !va.indexBuffer) return;
	va.indexOffset += (VkDeviceSize)(uintptr_t)indices;
	GetContext().Draw(program, va, ToTopology(mode), (uint32_t)count, true, (uint32_t)instances, 0);
}

// ---- Shaders / programs ---------------------------------------------------
GLuint vkgl_CreateShader(GLenum type)
{
	const GLuint id = NewId();
	auto s = std::make_unique<GLShaderObj>();
	s->type = type;
	g->shaders[id] = std::move(s);
	return id;
}
void vkgl_DeleteShader(GLuint id) { g->shaders.erase(id); }
void vkgl_ShaderSource(GLuint id, GLsizei count, const GLchar* const* strings, const GLint* lengths)
{
	auto it = g->shaders.find(id);
	if (it == g->shaders.end()) return;
	std::string src;
	for (GLsizei i = 0; i < count; ++i)
	{
		if (!strings[i]) continue;
		if (lengths && lengths[i] >= 0) src.append(strings[i], (size_t)lengths[i]);
		else src.append(strings[i]);
	}
	it->second->source = std::move(src);
}
void vkgl_CompileShader(GLuint) {}
void vkgl_GetShaderiv(GLuint id, GLenum pname, GLint* params)
{
	if (!params) return;
	if (pname == GL_COMPILE_STATUS) *params = g->shaders.count(id) ? GL_TRUE : GL_FALSE;
	else *params = 0;
}
void vkgl_GetShaderInfoLog(GLuint, GLsizei bufSize, GLsizei* length, GLchar* log)
{
	if (length) *length = 0;
	if (log && bufSize > 0) log[0] = 0;
}
GLuint vkgl_CreateProgram()
{
	const GLuint id = NewId();
	g->programs[id] = std::make_unique<GLProg>();
	return id;
}
void vkgl_DeleteProgram(GLuint id)
{
	if (id == 0) return;
	g->programs.erase(id);
	if (g->program == id) g->program = 0;
}
void vkgl_AttachShader(GLuint program, GLuint shader)
{
	auto it = g->programs.find(program);
	if (it != g->programs.end()) it->second->shaders.push_back(shader);
}
void vkgl_LinkProgram(GLuint id)
{
	auto it = g->programs.find(id);
	if (it == g->programs.end()) return;
	GLProg& p = *it->second;
	std::string vs, fs;
	for (GLuint s : p.shaders)
	{
		auto si = g->shaders.find(s);
		if (si == g->shaders.end()) continue;
		if (si->second->type == GL_VERTEX_SHADER) vs = si->second->source;
		else if (si->second->type == GL_FRAGMENT_SHADER) fs = si->second->source;
	}
	p.program = std::make_unique<VulkanProgram>();
	p.linked = p.program->Create(vs.data(), vs.size(), fs.data(), fs.size(), p.log);
	if (!p.linked) p.program.reset();
	p.locations.clear();
	p.locationMap.clear();
}
void vkgl_GetProgramiv(GLuint id, GLenum pname, GLint* params)
{
	if (!params) return;
	auto it = g->programs.find(id);
	if (pname == GL_LINK_STATUS) *params = (it != g->programs.end() && it->second->linked) ? GL_TRUE : GL_FALSE;
	else if (pname == GL_INFO_LOG_LENGTH) *params = it != g->programs.end() ? (GLint)it->second->log.size() + 1 : 0;
	else *params = 0;
}
void vkgl_GetProgramInfoLog(GLuint id, GLsizei bufSize, GLsizei* length, GLchar* log)
{
	auto it = g->programs.find(id);
	const std::string msg = it != g->programs.end() ? it->second->log : std::string();
	if (log && bufSize > 0)
	{
		const size_t n = std::min<size_t>(msg.size(), (size_t)bufSize - 1);
		std::memcpy(log, msg.data(), n);
		log[n] = 0;
		if (length) *length = (GLsizei)n;
	}
	else if (length) *length = 0;
}
void vkgl_UseProgram(GLuint id) { g->program = id; }
GLuint vkgl_GetUniformBlockIndex(GLuint id, const GLchar*)
{
	auto it = g->programs.find(id);
	return (it != g->programs.end() && it->second->linked) ? 0u : GL_INVALID_INDEX;
}
// Uniform blocks use the fixed HRL binding points (LightBlock 0, BoneBlock 1,
// DDGIBlock0..2 : 2..4) in both backends; the translator assigns them by name.
void vkgl_UniformBlockBinding(GLuint, GLuint, GLuint) {}

GLint vkgl_GetUniformLocation(GLuint id, const GLchar* name)
{
	auto it = g->programs.find(id);
	if (it == g->programs.end() || !it->second->linked || !name) return -1;
	GLProg& p = *it->second;
	auto li = p.locationMap.find(name);
	if (li != p.locationMap.end()) return li->second;
	GLint loc = -1;
	if (p.program->Has(name))
	{
		loc = (GLint)p.locations.size();
		p.locations.push_back(name);
	}
	p.locationMap.emplace(name, loc);
	return loc;
}

static bool UniformTarget(GLint location, VulkanProgram*& program, const std::string*& name)
{
	GLProg* p = CurrentProgram();
	if (!p || !p->linked || location < 0 || (size_t)location >= p->locations.size()) return false;
	program = p->program.get();
	name = &p->locations[(size_t)location];
	return true;
}
static std::string Element(const std::string& name, GLsizei i)
{
	if (i == 0) return name;
	std::string base = name;
	if (base.size() > 3 && base.compare(base.size() - 3, 3, "[0]") == 0) base.resize(base.size() - 3);
	return base + "[" + std::to_string(i) + "]";
}
void vkgl_Uniform1i(GLint loc, GLint v) { VulkanProgram* p; const std::string* n; if (UniformTarget(loc, p, n)) p->SetInt(n->c_str(), v); }
void vkgl_Uniform1ui(GLint loc, GLuint v) { VulkanProgram* p; const std::string* n; if (UniformTarget(loc, p, n)) p->SetUint(n->c_str(), v); }
void vkgl_Uniform1f(GLint loc, GLfloat v) { VulkanProgram* p; const std::string* n; if (UniformTarget(loc, p, n)) p->SetFloat(n->c_str(), v); }
void vkgl_Uniform2f(GLint loc, GLfloat x, GLfloat y) { VulkanProgram* p; const std::string* n; if (UniformTarget(loc, p, n)) p->SetVec2(n->c_str(), glm::vec2(x, y)); }
void vkgl_Uniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z) { VulkanProgram* p; const std::string* n; if (UniformTarget(loc, p, n)) p->SetVec3(n->c_str(), glm::vec3(x, y, z)); }
void vkgl_Uniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { VulkanProgram* p; const std::string* n; if (UniformTarget(loc, p, n)) p->SetVec4(n->c_str(), glm::vec4(x, y, z, w)); }
void vkgl_Uniform1iv(GLint loc, GLsizei count, const GLint* v)
{
	VulkanProgram* p; const std::string* n;
	if (UniformTarget(loc, p, n)) for (GLsizei i = 0; i < count; ++i) p->SetInt(Element(*n, i).c_str(), v[i]);
}
void vkgl_Uniform1fv(GLint loc, GLsizei count, const GLfloat* v)
{
	VulkanProgram* p; const std::string* n;
	if (UniformTarget(loc, p, n)) for (GLsizei i = 0; i < count; ++i) p->SetFloat(Element(*n, i).c_str(), v[i]);
}
void vkgl_Uniform2fv(GLint loc, GLsizei count, const GLfloat* v)
{
	VulkanProgram* p; const std::string* n;
	if (UniformTarget(loc, p, n)) for (GLsizei i = 0; i < count; ++i) p->SetVec2(Element(*n, i).c_str(), glm::vec2(v[i * 2], v[i * 2 + 1]));
}
void vkgl_Uniform3fv(GLint loc, GLsizei count, const GLfloat* v)
{
	VulkanProgram* p; const std::string* n;
	if (UniformTarget(loc, p, n)) for (GLsizei i = 0; i < count; ++i) p->SetVec3(Element(*n, i).c_str(), glm::vec3(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]));
}
void vkgl_Uniform4fv(GLint loc, GLsizei count, const GLfloat* v)
{
	VulkanProgram* p; const std::string* n;
	if (UniformTarget(loc, p, n))
		for (GLsizei i = 0; i < count; ++i) p->SetVec4(Element(*n, i).c_str(), glm::vec4(v[i * 4], v[i * 4 + 1], v[i * 4 + 2], v[i * 4 + 3]));
}
void vkgl_UniformMatrix3fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v)
{
	VulkanProgram* p; const std::string* n;
	if (!UniformTarget(loc, p, n)) return;
	for (GLsizei i = 0; i < count; ++i)
	{
		glm::mat3 m;
		std::memcpy(&m[0][0], v + i * 9, sizeof(float) * 9);
		if (transpose) m = glm::transpose(m);
		p->SetMat3(Element(*n, i).c_str(), m);
	}
}
void vkgl_UniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v)
{
	VulkanProgram* p; const std::string* n;
	if (!UniformTarget(loc, p, n)) return;
	for (GLsizei i = 0; i < count; ++i)
	{
		glm::mat4 m;
		std::memcpy(&m[0][0], v + i * 16, sizeof(float) * 16);
		if (transpose) m = glm::transpose(m);
		p->SetMat4(Element(*n, i).c_str(), m);
	}
}
