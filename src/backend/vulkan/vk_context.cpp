#include "vk_context.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hrlvk
{
static Context g_context;
Context& GetContext() { return g_context; }

static uint64_t HashBytes(uint64_t h, const void* data, size_t size)
{
	const unsigned char* p = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
	return h;
}
template <typename T> static uint64_t HashValue(uint64_t h, const T& v) { return HashBytes(h, &v, sizeof(T)); }

constexpr uint32_t kConstantAttribBinding = 12; // 12, 13
constexpr uint32_t kDefaultFloatBinding = 14;
constexpr uint32_t kDefaultIntBinding = 15;
constexpr size_t kRingSize = 8u << 20;
constexpr uint32_t kSetsPerPool = 512;

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
bool CreateTexture(Texture& out, ImageKind kind, VkFormat format, uint32_t width, uint32_t height,
	uint32_t layers, uint32_t mips, VkSampleCountFlagBits samples, bool renderTarget)
{
	ImageDesc desc;
	desc.kind = kind;
	desc.format = format;
	desc.width = width;
	desc.height = height;
	desc.layers = layers;
	desc.mips = mips;
	desc.samples = samples;
	desc.renderTarget = renderTarget;
	out.sampler = SamplerState{};
	return GetCore().CreateImage(desc, out.image);
}

void DestroyTexture(Texture& texture)
{
	if (texture.image.image)
		GetContext().ForgetImage(texture.image.serial);
	GetCore().DestroyImage(texture.image, true);
	texture = Texture{};
}

uint64_t VertexArray::LayoutHash() const
{
	uint64_t h = 1469598103934665603ull;
	for (const auto& b : bindings) h = HashValue(h, b);
	for (const auto& a : attribs) h = HashValue(h, a);
	for (const auto& c : constants) h = HashValue(h, c.first);
	return h;
}

bool Context::PassKey::operator==(const PassKey& o) const
{
	return colorCount == o.colorCount && samples == o.samples &&
		std::memcmp(formats, o.formats, sizeof(formats)) == 0;
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------
bool Context::Init()
{
	Core& core = GetCore();
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	for (uint32_t i = 0; i <= kUniformBlockPoints; ++i)
		bindings.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
	for (uint32_t i = 0; i < kMaxSamplers; ++i)
		bindings.push_back({kSamplerBindingBase + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
	VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = (uint32_t)bindings.size();
	li.pBindings = bindings.data();
	if (vkCreateDescriptorSetLayout(core.device, &li, nullptr, &setLayout) != VK_SUCCESS) return false;
	VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &setLayout;
	if (vkCreatePipelineLayout(core.device, &pli, nullptr, &pipelineLayout) != VK_SUCCESS) return false;

	if (!core.CreateBuffer(64 * 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, dummyUbo_)) return false;
	std::memset(dummyUbo_.mapped, 0, 64 * 1024);
	if (!core.CreateBuffer(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, defaultAttribs_)) return false;
	{
		// float binding : (0,0,0,1) ; int binding : (0,0,0,0)
		const float f[4] = {0.f, 0.f, 0.f, 1.f};
		const uint32_t i[4] = {0, 0, 0, 0};
		std::memcpy(defaultAttribs_.mapped, f, 16);
		std::memcpy(static_cast<char*>(defaultAttribs_.mapped) + 16, i, 16);
	}

	// Unbound texture units sample (0,0,0,1) like an incomplete OpenGL texture.
	const unsigned char black[4 * 6] = {0,0,0,255, 0,0,0,255, 0,0,0,255, 0,0,0,255, 0,0,0,255, 0,0,0,255};
	if (!CreateTexture(fallback2D_, ImageKind::Tex2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, false)) return false;
	core.UploadImage(fallback2D_.image, 0, black, 4, 1, 1);
	if (!CreateTexture(fallbackCube_, ImageKind::Cube, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 6, 1, VK_SAMPLE_COUNT_1_BIT, false)) return false;
	for (uint32_t f = 0; f < 6; ++f) core.UploadImage(fallbackCube_.image, f, black, 4, 1, 1);
	if (!CreateTexture(fallbackArray_, ImageKind::Array2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, false)) return false;
	core.UploadImage(fallbackArray_.image, 0, black, 4, 1, 1);
	if (core.supportedSamples & VK_SAMPLE_COUNT_2_BIT)
	{
		if (CreateTexture(fallbackMS_, ImageKind::Tex2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, VK_SAMPLE_COUNT_2_BIT, true))
			core.Transition(core.Cmd(), fallbackMS_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	core.onBeforeTransfer = [this]() { EndRenderPass(); };
	core.onBeforeSubmit = [this]() { EndRenderPass(); };

	static const char* kResolveVS = "#version 330 core\nvoid main(){ vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0); gl_Position = vec4(p, 0.0, 1.0); }\n";
	static const char* kResolveFS = "#version 330 core\nuniform sampler2DMS uDepth;\nvoid main(){ gl_FragDepth = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r; }\n";
	depthResolveProgram_ = new VulkanProgram();
	std::string err;
	if (!depthResolveProgram_->Create(kResolveVS, std::strlen(kResolveVS), kResolveFS, std::strlen(kResolveFS), err))
	{
		Error("depth resolve shader: " + err);
		delete depthResolveProgram_;
		depthResolveProgram_ = nullptr;
	}
	return true;
}

void Context::Shutdown()
{
	Core& core = GetCore();
	if (!core.device) return;
	vkDeviceWaitIdle(core.device);
	delete depthResolveProgram_;
	depthResolveProgram_ = nullptr;
	for (auto& [k, p] : pipelines_) vkDestroyPipeline(core.device, p, nullptr);
	pipelines_.clear();
	for (auto& f : framebuffers_) vkDestroyFramebuffer(core.device, f.fb, nullptr);
	framebuffers_.clear();
	for (auto& r : renderPasses_) vkDestroyRenderPass(core.device, r.rp, nullptr);
	renderPasses_.clear();
	for (auto& fr : frames_)
	{
		for (auto& r : fr.rings) core.DestroyBuffer(r.buffer, false);
		fr.rings.clear();
		for (auto p : fr.pools) vkDestroyDescriptorPool(core.device, p, nullptr);
		fr.pools.clear();
		fr.sets.clear();
		for (auto& rb : fr.readbacks) core.DestroyBuffer(rb.first, false);
		fr.readbacks.clear();
		fr.readbackSizes.clear();
	}
	programUploads_.clear();
	core.DestroyBuffer(dummyUbo_, false);
	core.DestroyBuffer(defaultAttribs_, false);
	for (Texture* t : {&fallback2D_, &fallbackCube_, &fallbackArray_, &fallbackMS_})
	{
		core.DestroyImage(t->image, false);
		*t = Texture{};
	}
	if (pipelineLayout) vkDestroyPipelineLayout(core.device, pipelineLayout, nullptr);
	if (setLayout) vkDestroyDescriptorSetLayout(core.device, setLayout, nullptr);
	pipelineLayout = VK_NULL_HANDLE;
	setLayout = VK_NULL_HANDLE;
	core.onBeforeSubmit = nullptr;
	core.onBeforeTransfer = nullptr;
	fb_ = nullptr;
	passOpen_ = false;
	units_ = {};
	for (auto& u : ubos_) u = UniformBinding{};
}

void Context::BeginFrame()
{
	FrameResources& fr = FR();
	for (auto& r : fr.rings) r.cursor = 0;
	fr.ringIndex = 0;
	for (auto p : fr.pools) vkResetDescriptorPool(GetCore().device, p, 0);
	fr.poolIndex = 0;
	fr.setsInPool = 0;
	fr.sets.clear();
	programUploads_.clear();
	passOpen_ = false;
	fb_ = nullptr;
	viewportDirty_ = true;
}

// ---------------------------------------------------------------------------
// Render passes / framebuffers
// ---------------------------------------------------------------------------
VkRenderPass Context::GetRenderPass(const PassKey& key)
{
	for (const auto& e : renderPasses_) if (e.key == key) return e.rp;
	std::vector<VkAttachmentDescription> atts;
	std::vector<VkAttachmentReference> colorRefs;
	for (uint32_t i = 0; i < key.colorCount; ++i)
	{
		if (key.formats[i] == VK_FORMAT_UNDEFINED)
		{
			colorRefs.push_back({VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED});
			continue;
		}
		VkAttachmentDescription a{};
		a.format = key.formats[i];
		a.samples = key.samples;
		a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorRefs.push_back({(uint32_t)atts.size(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
		atts.push_back(a);
	}
	VkAttachmentReference depthRef{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
	if (key.formats[kMaxColorAttachments] != VK_FORMAT_UNDEFINED)
	{
		VkAttachmentDescription a{};
		a.format = key.formats[kMaxColorAttachments];
		a.samples = key.samples;
		a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		a.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthRef = {(uint32_t)atts.size(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
		atts.push_back(a);
	}
	VkSubpassDescription sub{};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = (uint32_t)colorRefs.size();
	sub.pColorAttachments = colorRefs.empty() ? nullptr : colorRefs.data();
	sub.pDepthStencilAttachment = depthRef.attachment != VK_ATTACHMENT_UNUSED ? &depthRef : nullptr;
	VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	ci.attachmentCount = (uint32_t)atts.size();
	ci.pAttachments = atts.data();
	ci.subpassCount = 1;
	ci.pSubpasses = &sub;
	VkRenderPass rp = VK_NULL_HANDLE;
	VkResult r = vkCreateRenderPass(GetCore().device, &ci, nullptr, &rp);
	if (r != VK_SUCCESS) { Error("vkCreateRenderPass", r); return VK_NULL_HANDLE; }
	renderPasses_.push_back({key, rp});
	return rp;
}

VkFramebuffer Context::GetFramebuffer(const Framebuffer& fb, VkRenderPass rp, PassKey& key, uint32_t& w, uint32_t& h)
{
	std::vector<uint64_t> k;
	std::vector<uint64_t> serials;
	std::vector<VkImageView> views;
	w = UINT32_MAX; h = UINT32_MAX;
	k.push_back((uint64_t)(uintptr_t)rp);
	for (uint32_t i = 0; i < key.colorCount; ++i)
	{
		const Texture* t = fb.color[i];
		if (!t) continue;
		k.push_back(t->image.serial);
		k.push_back(fb.colorLayer[i]);
		serials.push_back(t->image.serial);
		views.push_back(t->image.layerViews.empty() ? t->image.view : t->image.layerViews[std::min<size_t>(fb.colorLayer[i], t->image.layerViews.size() - 1)]);
		w = std::min(w, t->image.width);
		h = std::min(h, t->image.height);
	}
	if (fb.depth)
	{
		k.push_back(fb.depth->image.serial);
		k.push_back(fb.depthLayer + 1000);
		serials.push_back(fb.depth->image.serial);
		views.push_back(fb.depth->image.layerViews.empty() ? fb.depth->image.view : fb.depth->image.layerViews[std::min<size_t>(fb.depthLayer, fb.depth->image.layerViews.size() - 1)]);
		w = std::min(w, fb.depth->image.width);
		h = std::min(h, fb.depth->image.height);
	}
	if (w == UINT32_MAX) { w = h = 1; }
	for (const auto& e : framebuffers_) if (e.key == k) return e.fb;
	VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	ci.renderPass = rp;
	ci.attachmentCount = (uint32_t)views.size();
	ci.pAttachments = views.data();
	ci.width = w;
	ci.height = h;
	ci.layers = 1;
	VkFramebuffer f = VK_NULL_HANDLE;
	VkResult r = vkCreateFramebuffer(GetCore().device, &ci, nullptr, &f);
	if (r != VK_SUCCESS) { Error("vkCreateFramebuffer", r); return VK_NULL_HANDLE; }
	framebuffers_.push_back({k, f, serials});
	return f;
}

void Context::ForgetImage(uint64_t serial)
{
	Core& core = GetCore();
	for (auto it = framebuffers_.begin(); it != framebuffers_.end();)
	{
		if (std::find(it->serials.begin(), it->serials.end(), serial) != it->serials.end())
		{
			VkFramebuffer f = it->fb;
			VkDevice device = core.device;
			core.Defer([device, f]() { vkDestroyFramebuffer(device, f, nullptr); });
			it = framebuffers_.erase(it);
		}
		else ++it;
	}
}

void Context::BindFramebuffer(Framebuffer* fb)
{
	if (passOpen_ && fb != fb_) EndRenderPass();
	fb_ = fb;
}

void Context::EndRenderPass()
{
	if (!passOpen_) return;
	vkCmdEndRenderPass(GetCore().Cmd());
	passOpen_ = false;
}

void Context::Viewport(int x, int y, int width, int height)
{
	viewport_.x = (float)x;
	viewport_.y = (float)y;
	viewport_.width = (float)std::max(1, width);
	viewport_.height = (float)std::max(1, height);
	viewport_.minDepth = 0.f;
	viewport_.maxDepth = 1.f;
	viewportDirty_ = true;
	if (passOpen_)
	{
		vkCmdSetViewport(GetCore().Cmd(), 0, 1, &viewport_);
		viewportDirty_ = false;
	}
}

bool Context::BeginPass()
{
	if (passOpen_)
	{
		// The framebuffer object may have been edited since the pass started.
		if (std::memcmp(&passFbCopy_, fb_, sizeof(Framebuffer)) == 0) return true;
		EndRenderPass();
	}
	if (!fb_) return false;
	Core& core = GetCore();
	VkCommandBuffer cmd = core.Cmd();
	PassKey key{};
	for (auto& f : key.formats) f = VK_FORMAT_UNDEFINED;
	key.colorCount = fb_->ColorCount();
	key.samples = VK_SAMPLE_COUNT_1_BIT;
	for (uint32_t i = 0; i < key.colorCount; ++i)
		if (fb_->color[i])
		{
			key.formats[i] = fb_->color[i]->image.format;
			key.samples = fb_->color[i]->image.samples;
			core.Transition(cmd, fb_->color[i]->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		}
	if (fb_->depth)
	{
		key.formats[kMaxColorAttachments] = fb_->depth->image.format;
		key.samples = fb_->depth->image.samples;
		core.Transition(cmd, fb_->depth->image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}
	VkRenderPass rp = GetRenderPass(key);
	uint32_t w = 1, h = 1;
	VkFramebuffer f = GetFramebuffer(*fb_, rp, key, w, h);
	if (!rp || !f) return false;
	VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	bi.renderPass = rp;
	bi.framebuffer = f;
	bi.renderArea = {{0, 0}, {w, h}};
	vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
	passOpen_ = true;
	passRp_ = rp;
	passKey_ = key;
	passW_ = w;
	passH_ = h;
	passFbCopy_ = *fb_;
	VkRect2D scissor{{0, 0}, {w, h}};
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdSetViewport(cmd, 0, 1, &viewport_);
	viewportDirty_ = false;
	return true;
}

void Context::Clear(bool color, bool depth, const glm::vec4& clearColor, float clearDepth)
{
	if (!fb_ || !BeginPass()) return;
	std::vector<VkClearAttachment> clears;
	if (color)
		for (uint32_t i = 0; i < passKey_.colorCount; ++i)
			if (fb_->color[i] && (fb_->drawMask & (1u << i)))
			{
				VkClearAttachment c{};
				c.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				c.colorAttachment = i;
				c.clearValue.color = {{clearColor.r, clearColor.g, clearColor.b, clearColor.a}};
				clears.push_back(c);
			}
	if (depth && fb_->depth)
	{
		VkClearAttachment c{};
		c.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		c.clearValue.depthStencil = {clearDepth, 0};
		clears.push_back(c);
	}
	if (clears.empty()) return;
	VkClearRect rect{{{0, 0}, {passW_, passH_}}, 0, 1};
	vkCmdClearAttachments(GetCore().Cmd(), (uint32_t)clears.size(), clears.data(), 1, &rect);
}

void Context::ClearColorBuffer(uint32_t index, const glm::vec4& value)
{
	if (!fb_ || index >= kMaxColorAttachments || !fb_->color[index] || !BeginPass()) return;
	VkClearAttachment c{};
	c.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	c.colorAttachment = index;
	c.clearValue.color = {{value.r, value.g, value.b, value.a}};
	VkClearRect rect{{{0, 0}, {passW_, passH_}}, 0, 1};
	vkCmdClearAttachments(GetCore().Cmd(), 1, &c, 1, &rect);
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------
void Context::BindTexture(uint32_t unit, int slot, Texture* texture)
{
	if (unit >= units_.size()) return;
	Unit& u = units_[unit];
	switch (slot)
	{
	case 1: u.cube = texture; break;
	case 2: u.array2D = texture; break;
	default: u.tex2D = texture; break;
	}
}

void Context::UnbindTexture(const Texture* texture)
{
	for (auto& u : units_)
	{
		if (u.tex2D == texture) u.tex2D = nullptr;
		if (u.cube == texture) u.cube = nullptr;
		if (u.array2D == texture) u.array2D = nullptr;
	}
}

void Context::BindSampler(uint32_t unit, const SamplerState* sampler)
{
	if (unit < units_.size()) units_[unit].sampler = sampler;
}

void Context::BindUniformBuffer(uint32_t point, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range)
{
	if (point >= kUniformBlockPoints) return;
	ubos_[point] = {buffer, offset, range};
}

void Context::BindUniformData(uint32_t point, const void* data, size_t size)
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	if (UploadRing(data, size, GetCore().properties.limits.minUniformBufferOffsetAlignment, buffer, offset))
		BindUniformBuffer(point, buffer, offset, size);
}

bool Context::UploadRing(const void* data, size_t size, size_t alignment, VkBuffer& buffer, VkDeviceSize& offset)
{
	FrameResources& fr = FR();
	alignment = std::max<size_t>(alignment, 16);
	for (;;)
	{
		if (fr.ringIndex >= fr.rings.size())
		{
			RingBuffer rb;
			const size_t sz = std::max(kRingSize, size + alignment);
			if (!GetCore().CreateBuffer(sz, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, rb.buffer))
				return false;
			fr.rings.push_back(rb);
		}
		RingBuffer& rb = fr.rings[fr.ringIndex];
		const size_t start = (rb.cursor + alignment - 1) / alignment * alignment;
		if (start + size <= rb.buffer.size)
		{
			if (data) std::memcpy(static_cast<char*>(rb.buffer.mapped) + start, data, size);
			rb.cursor = start + size;
			buffer = rb.buffer.buffer;
			offset = start;
			return true;
		}
		++fr.ringIndex;
	}
}

bool Context::AllocateTransient(const void* data, size_t size, VkBuffer& buffer, VkDeviceSize& offset, size_t alignment)
{
	return UploadRing(data, size, alignment, buffer, offset);
}

Texture* Context::ResolveUnit(const ShaderSampler& s)
{
	const uint32_t unit = (uint32_t)std::clamp(s.unit, 0, (int)units_.size() - 1);
	const Unit& u = units_[unit];
	auto ok = [](const Texture* x) { return x && x->image.image != VK_NULL_HANDLE; };
	Texture* t = nullptr;
	switch (s.dim)
	{
	case SamplerDim::Cube: t = (ok(u.cube) && u.cube->image.kind == ImageKind::Cube) ? u.cube : &fallbackCube_; break;
	case SamplerDim::Array2D: t = (ok(u.array2D) && u.array2D->image.kind == ImageKind::Array2D) ? u.array2D : &fallbackArray_; break;
	case SamplerDim::Tex2DMS: t = (ok(u.tex2D) && u.tex2D->image.samples != VK_SAMPLE_COUNT_1_BIT) ? u.tex2D : &fallbackMS_; break;
	default: t = (ok(u.tex2D) && u.tex2D->image.samples == VK_SAMPLE_COUNT_1_BIT && u.tex2D->image.kind == ImageKind::Tex2D) ? u.tex2D : &fallback2D_; break;
	}
	return t;
}

void Context::TransitionForSampling(VulkanProgram* program)
{
	Core& core = GetCore();
	for (const auto& s : program->samplers)
	{
		Texture* t = ResolveUnit(s);
		if (!t || !t->image.image || t->image.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) continue;
		EndRenderPass();
		core.Transition(core.Cmd(), t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
}

VkDescriptorSet Context::GetDescriptorSet(VulkanProgram* program, uint32_t dynamicOffsets[6])
{
	Core& core = GetCore();
	FrameResources& fr = FR();
	// Default block data
	UniformBinding bindings[1 + kUniformBlockPoints];
	if (program->blockSize > 0)
	{
		auto it = programUploads_.find(program->id);
		if (it != programUploads_.end() && it->second.first == program->dataVersion)
		{
			bindings[0] = {it->second.second.first, it->second.second.second, program->blockSize};
		}
		else
		{
			VkBuffer b; VkDeviceSize o;
			if (!UploadRing(program->data.data(), program->blockSize, core.properties.limits.minUniformBufferOffsetAlignment, b, o))
				return VK_NULL_HANDLE;
			bindings[0] = {b, o, program->blockSize};
			programUploads_[program->id] = {program->dataVersion, {b, o}};
		}
	}
	else bindings[0] = {dummyUbo_.buffer, 0, 16};
	for (uint32_t p = 0; p < kUniformBlockPoints; ++p)
	{
		if ((program->uniformBlockMask & (1u << p)) && ubos_[p].buffer)
			bindings[1 + p] = ubos_[p];
		else
			bindings[1 + p] = {dummyUbo_.buffer, 0, std::min<VkDeviceSize>(dummyUbo_.size, 64 * 1024)};
		if (!(program->uniformBlockMask & (1u << p))) bindings[1 + p].range = 16;
	}

	uint64_t h = 1469598103934665603ull;
	for (const auto& b : bindings) { h = HashValue(h, b.buffer); h = HashValue(h, b.range); }
	struct ImageInfo { VkDescriptorImageInfo info; uint32_t binding; };
	std::vector<ImageInfo> images;
	images.reserve(program->samplers.size());
	for (const auto& s : program->samplers)
	{
		Texture* t = ResolveUnit(s);
		const uint32_t unit = (uint32_t)std::clamp(s.unit, 0, (int)units_.size() - 1);
		const SamplerState& st = (units_[unit].sampler && t != &fallback2D_ && t != &fallbackCube_ && t != &fallbackArray_)
			? *units_[unit].sampler : t->sampler;
		VkDescriptorImageInfo info{};
		info.sampler = core.GetSampler(st);
		info.imageView = t->image.view;
		info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		images.push_back({info, s.binding});
		h = HashValue(h, info.sampler);
		h = HashValue(h, t->image.serial);
		h = HashValue(h, s.binding);
	}
	for (uint32_t i = 0; i <= kUniformBlockPoints; ++i) dynamicOffsets[i] = (uint32_t)bindings[i].offset;

	auto it = fr.sets.find(h);
	if (it != fr.sets.end()) return it->second;

	if (fr.poolIndex >= fr.pools.size() || fr.setsInPool >= kSetsPerPool)
	{
		if (fr.poolIndex < fr.pools.size() && fr.setsInPool >= kSetsPerPool) ++fr.poolIndex;
		if (fr.poolIndex >= fr.pools.size())
		{
			VkDescriptorPoolSize sizes[2] = {
				{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSetsPerPool * (1 + kUniformBlockPoints)},
				{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSetsPerPool * kMaxSamplers}};
			VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pi.maxSets = kSetsPerPool;
			pi.poolSizeCount = 2;
			pi.pPoolSizes = sizes;
			VkDescriptorPool pool = VK_NULL_HANDLE;
			if (vkCreateDescriptorPool(core.device, &pi, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
			fr.pools.push_back(pool);
		}
		fr.setsInPool = 0;
	}
	VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	ai.descriptorPool = fr.pools[fr.poolIndex];
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &setLayout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (vkAllocateDescriptorSets(core.device, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
	++fr.setsInPool;

	VkDescriptorBufferInfo bufferInfos[1 + kUniformBlockPoints];
	std::vector<VkWriteDescriptorSet> writes;
	for (uint32_t i = 0; i <= kUniformBlockPoints; ++i)
	{
		bufferInfos[i] = {bindings[i].buffer, 0, std::max<VkDeviceSize>(16, bindings[i].range)};
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = set;
		w.dstBinding = i;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		w.pBufferInfo = &bufferInfos[i];
		writes.push_back(w);
	}
	for (const auto& img : images)
	{
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = set;
		w.dstBinding = img.binding;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &img.info;
		writes.push_back(w);
	}
	vkUpdateDescriptorSets(core.device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
	fr.sets.emplace(h, set);
	return set;
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------
VkPipeline Context::GetPipeline(VulkanProgram* program, const VertexArray& va, Topology topology, VkRenderPass rp, const PassKey& key)
{
	Core& core = GetCore();
	const RasterState& s = state;
	uint64_t h = 1469598103934665603ull;
	h = HashValue(h, program->id);
	h = HashValue(h, va.LayoutHash());
	h = HashValue(h, rp);
	h = HashValue(h, topology);
	h = HashValue(h, s.depthTest); h = HashValue(h, s.depthWrite); h = HashValue(h, s.depthFunc);
	h = HashValue(h, s.blend);
	if (s.blend) { h = HashValue(h, s.srcFactor); h = HashValue(h, s.dstFactor); }
	h = HashValue(h, s.cull);
	if (s.cull) h = HashValue(h, s.cullFace);
	h = HashValue(h, s.wireframe);
	h = HashValue(h, s.polygonOffset);
	uint8_t masks[kMaxColorAttachments];
	for (uint32_t i = 0; i < kMaxColorAttachments; ++i)
		masks[i] = (fb_ && (fb_->drawMask & (1u << i)) && i < program->fragmentOutputs) ? s.colorMask[i] : 0;
	h = HashBytes(h, masks, sizeof(masks));
	auto it = pipelines_.find(h);
	if (it != pipelines_.end()) return it->second;

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = program->vertexModule;
	stages[0].pName = "main";
	stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = program->fragmentModule;
	stages[1].pName = "main";

	std::vector<VkVertexInputBindingDescription> bindings;
	std::vector<VkVertexInputAttributeDescription> attribs;
	auto addBinding = [&](uint32_t binding, uint32_t stride, bool instance) {
		for (const auto& b : bindings) if (b.binding == binding) return;
		bindings.push_back({binding, stride, instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
	};
	for (const auto& in : program->vertexInputs)
	{
		const VertexAttrib* found = nullptr;
		for (const auto& a : va.attribs) if (a.location == in.location) { found = &a; break; }
		if (found)
		{
			for (const auto& b : va.bindings)
				if (b.binding == found->binding) addBinding(b.binding, b.stride, b.perInstance);
			attribs.push_back({found->location, found->binding, found->format, found->offset});
			continue;
		}
		int constantIndex = -1;
		for (size_t c = 0; c < va.constants.size(); ++c) if (va.constants[c].first == in.location) constantIndex = (int)c;
		if (constantIndex >= 0 && constantIndex < 2)
		{
			const uint32_t binding = kConstantAttribBinding + (uint32_t)constantIndex;
			addBinding(binding, 0, false);
			attribs.push_back({in.location, binding, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
			continue;
		}
		const bool integer = in.base == ShaderUniform::Base::Int || in.base == ShaderUniform::Base::UInt;
		const uint32_t binding = integer ? kDefaultIntBinding : kDefaultFloatBinding;
		addBinding(binding, 0, false);
		attribs.push_back({in.location, binding,
			integer ? (in.base == ShaderUniform::Base::Int ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT) : VK_FORMAT_R32G32B32A32_SFLOAT, 0});
	}
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = (uint32_t)bindings.size();
	vi.pVertexBindingDescriptions = bindings.data();
	vi.vertexAttributeDescriptionCount = (uint32_t)attribs.size();
	vi.pVertexAttributeDescriptions = attribs.data();

	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = topology == Topology::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = (s.wireframe && core.enabledFeatures.fillModeNonSolid && topology == Topology::Triangles) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	rs.cullMode = s.cull ? s.cullFace : VK_CULL_MODE_NONE;
	// Render targets keep the OpenGL row order (no Y flip), so the winding
	// seen by Vulkan is reversed : GL_CCW front faces are VK clockwise.
	rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rs.depthBiasEnable = s.polygonOffset ? VK_TRUE : VK_FALSE;
	rs.lineWidth = 1.f;

	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = key.samples;

	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	const bool hasDepth = key.formats[kMaxColorAttachments] != VK_FORMAT_UNDEFINED;
	// GL : with the depth test disabled, depth is never written either.
	ds.depthTestEnable = (hasDepth && s.depthTest) ? VK_TRUE : VK_FALSE;
	ds.depthWriteEnable = (hasDepth && s.depthTest && s.depthWrite) ? VK_TRUE : VK_FALSE;
	ds.depthCompareOp = s.depthFunc;

	std::vector<VkPipelineColorBlendAttachmentState> blends(key.colorCount);
	for (uint32_t i = 0; i < key.colorCount; ++i)
	{
		VkPipelineColorBlendAttachmentState& a = blends[i];
		a.blendEnable = s.blend ? VK_TRUE : VK_FALSE;
		a.srcColorBlendFactor = a.srcAlphaBlendFactor = s.srcFactor;
		a.dstColorBlendFactor = a.dstAlphaBlendFactor = s.dstFactor;
		a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
		a.colorWriteMask = masks[i];
		if (!core.enabledFeatures.independentBlend) a.colorWriteMask = masks[0];
	}
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = (uint32_t)blends.size();
	cb.pAttachments = blends.empty() ? nullptr : blends.data();

	std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
	if (core.enabledFeatures.wideLines) dyn.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = (uint32_t)dyn.size();
	dy.pDynamicStates = dyn.data();

	VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pi.stageCount = 2;
	pi.pStages = stages;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &cb;
	pi.pDynamicState = &dy;
	pi.layout = pipelineLayout;
	pi.renderPass = rp;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkResult r = vkCreateGraphicsPipelines(core.device, core.pipelineCache, 1, &pi, nullptr, &pipeline);
	if (r != VK_SUCCESS) { Error("vkCreateGraphicsPipelines", r); pipeline = VK_NULL_HANDLE; }
	pipelines_.emplace(h, pipeline);
	return pipeline;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
void Context::Draw(VulkanProgram* program, const VertexArray& va, Topology topology, uint32_t count,
	bool indexed, uint32_t instances, uint32_t first)
{
	if (!program || !program->vertexModule || !fb_ || count == 0 || instances == 0) return;
	Core& core = GetCore();
	TransitionForSampling(program);
	if (!BeginPass()) return;
	VkCommandBuffer cmd = core.Cmd();
	VkPipeline pipeline = GetPipeline(program, va, topology, passRp_, passKey_);
	if (!pipeline) return;
	uint32_t dynamicOffsets[6] = {};
	VkDescriptorSet set = GetDescriptorSet(program, dynamicOffsets);
	if (!set) return;

	if (viewportDirty_) { vkCmdSetViewport(cmd, 0, 1, &viewport_); viewportDirty_ = false; }
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 6, dynamicOffsets);
	vkCmdSetDepthBias(cmd, state.polygonOffset ? state.offsetUnits : 0.f, 0.f, state.polygonOffset ? state.offsetFactor : 0.f);
	if (core.enabledFeatures.wideLines)
		vkCmdSetLineWidth(cmd, std::clamp(state.lineWidth, core.properties.limits.lineWidthRange[0], core.properties.limits.lineWidthRange[1]));

	// Vertex buffers: the ones of the vertex array, constants and defaults.
	for (const auto& b : va.bindings)
		if (b.binding < 12 && va.buffers[b.binding])
			vkCmdBindVertexBuffers(cmd, b.binding, 1, &va.buffers[b.binding], &va.offsets[b.binding]);
	for (size_t c = 0; c < va.constants.size() && c < 2; ++c)
	{
		VkBuffer buf; VkDeviceSize off;
		if (UploadRing(&va.constants[c].second, sizeof(glm::vec4), 16, buf, off))
			vkCmdBindVertexBuffers(cmd, kConstantAttribBinding + (uint32_t)c, 1, &buf, &off);
	}
	VkDeviceSize zero = 0, sixteen = 16;
	vkCmdBindVertexBuffers(cmd, kDefaultFloatBinding, 1, &defaultAttribs_.buffer, &zero);
	vkCmdBindVertexBuffers(cmd, kDefaultIntBinding, 1, &defaultAttribs_.buffer, &sixteen);

	if (indexed && va.indexBuffer)
	{
		vkCmdBindIndexBuffer(cmd, va.indexBuffer, va.indexOffset, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(cmd, count, instances, first, 0, 0);
	}
	else
		vkCmdDraw(cmd, count, instances, first, 0);
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------
void Context::BlitColor(Texture& src, uint32_t srcLayer, Texture& dst, uint32_t dstLayer,
	int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, bool linear)
{
	if (!src.image.image || !dst.image.image) return;
	EndRenderPass();
	Core& core = GetCore();
	VkCommandBuffer cmd = core.Cmd();
	auto clampX = [](int v, uint32_t m) { return std::clamp(v, 0, (int)m); };
	sx0 = clampX(sx0, src.image.width); sx1 = clampX(sx1, src.image.width);
	sy0 = clampX(sy0, src.image.height); sy1 = clampX(sy1, src.image.height);
	dx0 = clampX(dx0, dst.image.width); dx1 = clampX(dx1, dst.image.width);
	dy0 = clampX(dy0, dst.image.height); dy1 = clampX(dy1, dst.image.height);
	if (sx0 == sx1 || sy0 == sy1 || dx0 == dx1 || dy0 == dy1) return;
	core.Transition(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	core.Transition(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	if (src.image.samples != VK_SAMPLE_COUNT_1_BIT)
	{
		VkImageResolve r{};
		r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, srcLayer, 1};
		r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, dstLayer, 1};
		r.srcOffset = {std::min(sx0, sx1), std::min(sy0, sy1), 0};
		r.dstOffset = {std::min(dx0, dx1), std::min(dy0, dy1), 0};
		r.extent = {(uint32_t)std::abs(sx1 - sx0), (uint32_t)std::abs(sy1 - sy0), 1};
		vkCmdResolveImage(cmd, src.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
		return;
	}
	if (src.image.format == dst.image.format && sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0 && sx1 > sx0 && sy1 > sy0)
	{
		VkImageCopy c{};
		c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, srcLayer, 1};
		c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, dstLayer, 1};
		c.srcOffset = {sx0, sy0, 0};
		c.dstOffset = {dx0, dy0, 0};
		c.extent = {(uint32_t)(sx1 - sx0), (uint32_t)(sy1 - sy0), 1};
		vkCmdCopyImage(cmd, src.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		return;
	}
	VkImageBlit b{};
	b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, srcLayer, 1};
	b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, dstLayer, 1};
	b.srcOffsets[0] = {sx0, sy0, 0}; b.srcOffsets[1] = {sx1, sy1, 1};
	b.dstOffsets[0] = {dx0, dy0, 0}; b.dstOffsets[1] = {dx1, dy1, 1};
	vkCmdBlitImage(cmd, src.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
		linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

void Context::BlitDepth(Texture& src, Texture& dst, int x0, int y0, int x1, int y1)
{
	if (!src.image.image || !dst.image.image) return;
	x0 = std::clamp(x0, 0, (int)std::min(src.image.width, dst.image.width));
	x1 = std::clamp(x1, 0, (int)std::min(src.image.width, dst.image.width));
	y0 = std::clamp(y0, 0, (int)std::min(src.image.height, dst.image.height));
	y1 = std::clamp(y1, 0, (int)std::min(src.image.height, dst.image.height));
	if (x1 <= x0 || y1 <= y0) return;
	Core& core = GetCore();
	if (src.image.samples == dst.image.samples && src.image.format == dst.image.format)
	{
		EndRenderPass();
		VkCommandBuffer cmd = core.Cmd();
		core.Transition(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		core.Transition(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageCopy c{};
		c.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
		c.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
		c.srcOffset = {x0, y0, 0};
		c.dstOffset = {x0, y0, 0};
		c.extent = {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0), 1};
		vkCmdCopyImage(cmd, src.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		return;
	}
	if (!depthResolveProgram_) return;
	// Shader resolve: sample 0 of the multisample depth -> gl_FragDepth.
	Framebuffer* previousFb = fb_;
	const RasterState previousState = state;
	const VkViewport previousViewport = viewport_;
	Unit previousUnit = units_[0];
	depthResolveFb_ = Framebuffer{};
	depthResolveFb_.depth = &dst;
	BindFramebuffer(&depthResolveFb_);
	Viewport(x0, y0, x1 - x0, y1 - y0);
	state = RasterState{};
	state.depthTest = true;
	state.depthWrite = true;
	state.depthFunc = VK_COMPARE_OP_ALWAYS;
	units_[0] = Unit{};
	units_[0].tex2D = &src;
	depthResolveProgram_->SetInt("uDepth", 0);
	VertexArray empty;
	Draw(depthResolveProgram_, empty, Topology::Triangles, 3, false);
	EndRenderPass();
	units_[0] = previousUnit;
	state = previousState;
	viewport_ = previousViewport;
	viewportDirty_ = true;
	fb_ = previousFb;
}

void Context::PresentToSwapchain(Texture& src, int width, int height)
{
	Core& core = GetCore();
	if (!core.swapchainImageAcquired || !src.image.image) return;
	EndRenderPass();
	VkCommandBuffer cmd = core.Cmd();
	core.Transition(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkImage swap = core.swapchainImages[core.swapchainIndex];
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = swap;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	b.oldLayout = core.swapchainImageWritten ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.srcAccessMask = core.swapchainImageWritten ? VK_ACCESS_TRANSFER_WRITE_BIT : 0;
	b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
	const int extW = (int)core.swapchainExtent.width, extH = (int)core.swapchainExtent.height;
	if (!core.swapchainImageWritten)
	{
		// Pixels outside the scene image are black (like an unwritten back buffer).
		VkClearColorValue black{{0.f, 0.f, 0.f, 1.f}};
		VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdClearColorImage(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
	}
	const int w = std::min({width, extW, (int)src.image.width});
	const int h = std::min({height, extH, (int)src.image.height});
	if (w > 0 && h > 0)
	{
		// GL row 0 is the bottom of the window, swapchain row 0 is the top.
		VkImageBlit blit{};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.srcOffsets[0] = {0, 0, 0};
		blit.srcOffsets[1] = {w, h, 1};
		blit.dstOffsets[0] = {0, extH, 0};
		blit.dstOffsets[1] = {w, extH - h, 1};
		vkCmdBlitImage(cmd, src.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
	}
	core.swapchainImageWritten = true;
}

static float HalfToFloat(uint16_t h)
{
	const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
	uint32_t exp = (h >> 10) & 0x1Fu;
	uint32_t mant = h & 0x3FFu;
	uint32_t bits;
	if (exp == 0)
	{
		if (mant == 0) bits = sign;
		else
		{
			exp = 127 - 15 + 1;
			while (!(mant & 0x400u)) { mant <<= 1; --exp; }
			mant &= 0x3FFu;
			bits = sign | (exp << 23) | (mant << 13);
		}
	}
	else if (exp == 31) bits = sign | 0x7F800000u | (mant << 13);
	else bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

static uint32_t FormatSize(VkFormat f)
{
	switch (f)
	{
	case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
	case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
	default: return 4;
	}
}

static void ConvertToRGBA8(VkFormat format, const unsigned char* src, size_t pixels, unsigned char* dst)
{
	auto unorm = [](float v) -> unsigned char {
		if (!(v > 0.f)) return 0;
		if (v >= 1.f) return 255;
		return (unsigned char)std::lround(v * 255.f);
	};
	for (size_t i = 0; i < pixels; ++i)
	{
		switch (format)
		{
		case VK_FORMAT_R16G16B16A16_SFLOAT:
		{
			uint16_t h[4];
			std::memcpy(h, src + i * 8, 8);
			for (int c = 0; c < 4; ++c) dst[i * 4 + c] = unorm(HalfToFloat(h[c]));
			break;
		}
		case VK_FORMAT_R32G32B32A32_SFLOAT:
		{
			float f[4];
			std::memcpy(f, src + i * 16, 16);
			for (int c = 0; c < 4; ++c) dst[i * 4 + c] = unorm(f[c]);
			break;
		}
		case VK_FORMAT_B8G8R8A8_UNORM:
			dst[i * 4 + 0] = src[i * 4 + 2];
			dst[i * 4 + 1] = src[i * 4 + 1];
			dst[i * 4 + 2] = src[i * 4 + 0];
			dst[i * 4 + 3] = src[i * 4 + 3];
			break;
		default:
			std::memcpy(dst + i * 4, src + i * 4, 4);
			break;
		}
	}
}

bool Context::ReadPixels(Texture& src, int x, int y, int width, int height, std::vector<unsigned char>& rgba)
{
	Core& core = GetCore();
	if (!src.image.image || width <= 0 || height <= 0) return false;
	x = std::clamp(x, 0, (int)src.image.width - 1);
	y = std::clamp(y, 0, (int)src.image.height - 1);
	width = std::min(width, (int)src.image.width - x);
	height = std::min(height, (int)src.image.height - y);
	const uint32_t bpp = FormatSize(src.image.format);
	Buffer staging;
	if (!core.CreateBuffer((VkDeviceSize)width * height * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, staging)) return false;
	EndRenderPass();
	VkCommandBuffer cmd = core.Cmd();
	core.Transition(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageOffset = {x, y, 0};
	region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
	vkCmdCopyImageToBuffer(cmd, src.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);
	core.FlushAndWait();
	rgba.resize((size_t)width * height * 4);
	ConvertToRGBA8(src.image.format, static_cast<const unsigned char*>(staging.mapped), (size_t)width * height, rgba.data());
	core.DestroyBuffer(staging, false);
	return true;
}

bool Context::QueueReadback(Texture& src, std::function<void(int, int, std::vector<unsigned char>&)> done)
{
	Core& core = GetCore();
	if (!src.image.image) return false;
	const uint32_t bpp = FormatSize(src.image.format);
	Buffer staging;
	if (!core.CreateBuffer((VkDeviceSize)src.image.width * src.image.height * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, staging)) return false;
	EndRenderPass();
	VkCommandBuffer cmd = core.Cmd();
	core.Transition(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {src.image.width, src.image.height, 1};
	vkCmdCopyImageToBuffer(cmd, src.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);
	FrameResources& fr = FR();
	fr.readbacks.push_back({staging, std::move(done)});
	fr.readbackSizes.push_back({(int)src.image.width, (int)src.image.height});
	// remember the format in the high bits of the size entry
	formatsQueued_.push_back(src.image.format);
	return true;
}

void Context::ProcessReadbacks()
{
	Core& core = GetCore();
	for (auto& fr : frames_)
	{
		if (fr.readbacks.empty()) continue;
		vkQueueWaitIdle(core.queue);
		for (size_t i = 0; i < fr.readbacks.size(); ++i)
		{
			auto& rb = fr.readbacks[i];
			const int w = fr.readbackSizes[i].first, h = fr.readbackSizes[i].second;
			std::vector<unsigned char> rgba((size_t)w * h * 4);
			const VkFormat format = i < formatsQueued_.size() ? formatsQueued_[i] : VK_FORMAT_R8G8B8A8_UNORM;
			ConvertToRGBA8(format, static_cast<const unsigned char*>(rb.first.mapped), (size_t)w * h, rgba.data());
			if (rb.second) rb.second(w, h, rgba);
			core.DestroyBuffer(rb.first, false);
		}
		fr.readbacks.clear();
		fr.readbackSizes.clear();
		formatsQueued_.clear();
	}
}
} // namespace hrlvk
