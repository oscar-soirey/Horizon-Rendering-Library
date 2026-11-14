// HRL native Vulkan backend - programmable render pipeline.
//
// Render passes are application shaders (HRL_CreateComputeShader /
// HRL_CreateFullscreenShader) run at a stage of the frame of a viewport
// (HRL_ERenderStage). This file:
//   * turns their source into SPIR-V: the HRL_IMAGE* / HRL_TEXTURE* /
//     HRL_BUFFER declarations become set 2 bindings, `uniform` declarations
//     material parameters, and the shader sees the view, the lights and their
//     shadows through the same includes as the built-in surfaces (pass.glsl);
//   * keeps one instance of every render target per viewport (two images when
//     the target keeps a history), created and cleared when a frame first needs
//     it, recreated when the viewport changes size, released when unused;
//   * records the passes of a stage in the frame graph, with the barriers of
//     the images they read and write;
//   * provides the participating media volume of a viewport to its view set
//     (media.glsl), applied by every lit surface and the sky.
#include "vk_renderer_internal.h"

#include <cctype>
#include <cstdlib>
#include <set>
#include <sstream>

namespace hrlvk
{
namespace
{
// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
enum class ResKind { Image2D, Image3D, Texture2D, Texture3D, Buffer };
struct PassResource
{
	ResKind kind = ResKind::Texture2D;
	std::string name;
	std::string format; // storage images: GLSL format qualifier
	uint32_t binding = 0;
};

constexpr uint32_t kBindParams = 0, kBindDepth = 1, kBindSceneColor = 2, kBindSceneImage = 3, kFirstUserBinding = 4;
constexpr uint32_t kMaxPassOutputs = 4;

struct PassShader
{
	bool compute = false;
	std::vector<PassResource> resources;
	CustomShader table;                  // parameters (params / paramSlots) of the material
	std::vector<uint32_t> spirv;
	glm::uvec3 localSize{1u, 1u, 1u};
	bool usesDepth = false, usesSceneColor = false, usesSceneImage = false;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;                    // compute
	std::unordered_map<std::string, VkPipeline> graphics;   // fullscreen, by output formats and blends
};
std::unordered_map<HRL_id, PassShader> g_shaders;

// ---------------------------------------------------------------------------
// Render target instances (one per target and viewport)
// ---------------------------------------------------------------------------
struct TargetInstance
{
	Image images[2];
	uint32_t parity = 0;          // images[parity] : this frame, images[parity ^ 1] : the previous one
	bool historyValid = false;
	uint32_t width = 0, height = 0, depth = 0;
	VkFormat format = VK_FORMAT_UNDEFINED;
	bool history = false;
	uint64_t historyReset = 0;    // HRL_RenderTarget::history_reset_ seen
	uint64_t lastFrame = 0;       // last frame that used it
	bool written = false;         // written this frame (history flips at the end of the frame)
};
std::unordered_map<uint64_t, TargetInstance> g_targets;
uint64_t TargetKey(HRL_id target, HRL_id viewport) { return ((uint64_t)target << 32) | (uint64_t)viewport; }

// Camera of each viewport in the previous frame (reprojection).
struct ViewHistory
{
	glm::mat4 viewProj{1.f};
	glm::vec3 camera{0.f}, forward{0.f, 0.f, -1.f};
	glm::vec2 jitter{0.f};
	float time = 0.f;
	bool valid = false;
	// This frame, moved to the fields above by EndRenderPassFrame.
	glm::mat4 nextViewProj{1.f};
	glm::vec3 nextCamera{0.f}, nextForward{0.f, 0.f, -1.f};
	glm::vec2 nextJitter{0.f};
	float nextTime = 0.f;
	uint64_t lastFrame = 0;
};
std::unordered_map<uint64_t, ViewHistory> g_views;

Image g_dummyVolume;             // bound when a viewport has no participating media
bool g_dummyCleared = false;

std::set<std::string> g_reported;
void ReportOnce(const std::string& message, HRL_ESeverity severity = HRL_SEVERITY_ERROR)
{
	if (!g_reported.insert(message).second) return;
	SetErrorCode(HRL_INVALID_BACKEND_OPERATION, severity, "Vulkan render pipeline: " + message);
}

VkFormat TargetFormat(HRL_ERenderTargetFormat f)
{
	switch (f)
	{
	case HRL_RENDER_TARGET_RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
	case HRL_RENDER_TARGET_RG16F: return VK_FORMAT_R16G16_SFLOAT;
	case HRL_RENDER_TARGET_R16F: return VK_FORMAT_R16_SFLOAT;
	case HRL_RENDER_TARGET_R32F: return VK_FORMAT_R32_SFLOAT;
	case HRL_RENDER_TARGET_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
	default: return VK_FORMAT_R16G16B16A16_SFLOAT;
	}
}

const char* FormatQualifier(VkFormat f)
{
	switch (f)
	{
	case VK_FORMAT_R32G32B32A32_SFLOAT: return "rgba32f";
	case VK_FORMAT_R16G16_SFLOAT: return "rg16f";
	case VK_FORMAT_R16_SFLOAT: return "r16f";
	case VK_FORMAT_R32_SFLOAT: return "r32f";
	case VK_FORMAT_R8G8B8A8_UNORM: return "rgba8";
	default: return "rgba16f";
	}
}

bool IsIdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

bool ContainsIdentifier(const std::string& code, const char* name)
{
	const size_t n = std::strlen(name);
	for (size_t pos = 0; (pos = code.find(name, pos)) != std::string::npos; pos += n)
	{
		if (pos > 0 && IsIdentChar(code[pos - 1])) continue;
		if (pos + n < code.size() && IsIdentChar(code[pos + n])) continue;
		return true;
	}
	return false;
}

std::string Trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && std::isspace((unsigned char)s[a])) ++a;
	while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
	return s.substr(a, b - a);
}

// Value of `local_size_x = N` (1 when absent).
uint32_t LocalSize(const std::string& code, const char* key)
{
	const size_t pos = code.find(key);
	if (pos == std::string::npos) return 1u;
	size_t i = code.find('=', pos);
	if (i == std::string::npos) return 1u;
	++i;
	while (i < code.size() && std::isspace((unsigned char)code[i])) ++i;
	const unsigned long v = std::strtoul(code.c_str() + i, nullptr, 10);
	return v ? (uint32_t)v : 1u;
}

// Replaces the HRL_IMAGE* / HRL_TEXTURE* / HRL_BUFFER declarations by set 2
// bindings. Returns false (and `error`) on a malformed declaration.
bool ParseResources(std::string& code, std::vector<PassResource>& out, std::string& error)
{
	struct Macro { const char* name; ResKind kind; bool format; };
	static const Macro macros[] = {
		{"HRL_IMAGE2D", ResKind::Image2D, true}, {"HRL_IMAGE3D", ResKind::Image3D, true},
		{"HRL_TEXTURE2D", ResKind::Texture2D, false}, {"HRL_TEXTURE3D", ResKind::Texture3D, false},
		{"HRL_BUFFER", ResKind::Buffer, false}};
	struct Found { size_t begin, end; PassResource res; };
	std::vector<Found> found;
	for (const Macro& m : macros)
	{
		const size_t n = std::strlen(m.name);
		for (size_t pos = 0; (pos = code.find(m.name, pos)) != std::string::npos; pos += n)
		{
			if (pos > 0 && IsIdentChar(code[pos - 1])) continue;
			size_t open = pos + n;
			if (open < code.size() && IsIdentChar(code[open])) continue;
			while (open < code.size() && std::isspace((unsigned char)code[open])) ++open;
			if (open >= code.size() || code[open] != '(') { error = std::string(m.name) + ": '(' expected"; return false; }
			const size_t close = code.find(')', open);
			if (close == std::string::npos) { error = std::string(m.name) + ": ')' expected"; return false; }
			const std::string args = code.substr(open + 1, close - open - 1);
			PassResource r;
			r.kind = m.kind;
			if (m.format)
			{
				const size_t comma = args.find(',');
				if (comma == std::string::npos) { error = std::string(m.name) + "(format, name) expected"; return false; }
				r.format = Trim(args.substr(0, comma));
				r.name = Trim(args.substr(comma + 1));
				static const char* formats[] = {"rgba16f", "rgba32f", "rg16f", "r16f", "r32f", "rgba8"};
				bool known = false;
				for (const char* f : formats) known = known || r.format == f;
				if (!known) { error = std::string(m.name) + ": unknown format '" + r.format + "'"; return false; }
			}
			else r.name = Trim(args);
			if (r.name.empty() || !std::all_of(r.name.begin(), r.name.end(), IsIdentChar))
			{
				error = std::string(m.name) + ": invalid name '" + r.name + "'";
				return false;
			}
			found.push_back({pos, close + 1, r});
		}
	}
	std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.begin < b.begin; });
	for (size_t i = 0; i < found.size(); ++i)
	{
		found[i].res.binding = kFirstUserBinding + (uint32_t)i;
		for (size_t j = 0; j < i; ++j)
			if (found[j].res.name == found[i].res.name) { error = "resource '" + found[i].res.name + "' declared twice"; return false; }
	}
	// Replaced from the end: the offsets of the earlier ones stay valid.
	for (size_t i = found.size(); i-- > 0;)
	{
		const PassResource& r = found[i].res;
		std::ostringstream d;
		d << "layout(set = 2, binding = " << r.binding;
		switch (r.kind)
		{
		case ResKind::Image2D: d << ", " << r.format << ") uniform image2D " << r.name; break;
		case ResKind::Image3D: d << ", " << r.format << ") uniform image3D " << r.name; break;
		case ResKind::Texture2D: d << ") uniform sampler2D " << r.name; break;
		case ResKind::Texture3D: d << ") uniform sampler3D " << r.name; break;
		case ResKind::Buffer: d << ", std430) readonly buffer HRL_Buffer_" << r.name; break;
		}
		code.replace(found[i].begin, found[i].end - found[i].begin, d.str());
	}
	for (const Found& f : found) out.push_back(f.res);
	return true;
}

VkDescriptorType DescriptorType(ResKind k)
{
	switch (k)
	{
	case ResKind::Image2D: case ResKind::Image3D: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	case ResKind::Buffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	default: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	}
}

bool MakeLayouts(PassShader& s)
{
	Device& dev = Dev();
	const VkShaderStageFlags stage = s.compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
	std::vector<VkDescriptorSetLayoutBinding> bindings = {
		{kBindParams, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stage, nullptr},
		{kBindDepth, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stage, nullptr},
		{kBindSceneColor, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stage, nullptr},
		{kBindSceneImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stage, nullptr}};
	for (const PassResource& r : s.resources) bindings.push_back({r.binding, DescriptorType(r.kind), 1, stage, nullptr});
	// The unused built-in resources are left unwritten.
	std::vector<VkDescriptorBindingFlags> flags(bindings.size(), VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
	VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
	fi.bindingCount = (uint32_t)flags.size();
	fi.pBindingFlags = flags.data();
	VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &fi};
	ci.bindingCount = (uint32_t)bindings.size();
	ci.pBindings = bindings.data();
	VkResult r = vkCreateDescriptorSetLayout(dev.device, &ci, nullptr, &s.setLayout);
	if (r != VK_SUCCESS) { ReportError("render pass descriptor set layout", r); return false; }
	const VkDescriptorSetLayout sets[3] = {R.viewLayout, R.bindlessLayout, s.setLayout};
	VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	li.setLayoutCount = 3;
	li.pSetLayouts = sets;
	r = vkCreatePipelineLayout(dev.device, &li, nullptr, &s.layout);
	if (r != VK_SUCCESS) { ReportError("render pass pipeline layout", r); return false; }
	return true;
}

void DestroyPipelineDeferred(VkPipeline p)
{
	if (!p) return;
	auto it = std::find(R.ownedPipelines.begin(), R.ownedPipelines.end(), p);
	if (it != R.ownedPipelines.end()) R.ownedPipelines.erase(it);
	VkDevice d = Dev().device;
	Dev().Defer([d, p]() { vkDestroyPipeline(d, p, nullptr); });
}

void DestroyShader(PassShader& s)
{
	DestroyPipelineDeferred(s.pipeline);
	for (auto& [key, p] : s.graphics) DestroyPipelineDeferred(p);
	s.pipeline = VK_NULL_HANDLE;
	s.graphics.clear();
	VkDevice d = Dev().device;
	VkPipelineLayout layout = s.layout;
	VkDescriptorSetLayout setLayout = s.setLayout;
	Dev().Defer([d, layout, setLayout]()
	{
		if (layout) vkDestroyPipelineLayout(d, layout, nullptr);
		if (setLayout) vkDestroyDescriptorSetLayout(d, setLayout, nullptr);
	});
	s.layout = VK_NULL_HANDLE;
	s.setLayout = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Target instances
// ---------------------------------------------------------------------------
void DestroyInstance(TargetInstance& t)
{
	Dev().DestroyImage(t.images[0]);
	Dev().DestroyImage(t.images[1]);
	t = TargetInstance{};
}

// Instance of `target` for a viewport of vw x vh pixels, created / resized when
// needed (and cleared to zero by a pass added to `graph`). Null on failure.
TargetInstance* EnsureInstance(FrameGraph& graph, const HRL_RenderTarget* target, HRL_id viewport, uint32_t vw, uint32_t vh)
{
	Device& dev = Dev();
	const uint32_t w = target->scale_ > 0.f ? std::max(1u, (uint32_t)std::lround((float)vw * target->scale_)) : target->width_;
	const uint32_t h = target->scale_ > 0.f ? std::max(1u, (uint32_t)std::lround((float)vh * target->scale_)) : target->height_;
	const uint32_t d = std::max(target->depth_, 1u);
	const VkFormat format = TargetFormat(target->format_);
	TargetInstance& t = g_targets[TargetKey(target->id_, viewport)];
	const uint32_t count = target->history_ ? 2u : 1u;
	const bool recreate = !t.images[0] || t.width != w || t.height != h || t.depth != d || t.format != format ||
		(count == 2u) != (bool)t.images[1];
	if (recreate)
	{
		DestroyInstance(t);
		ImageDesc desc;
		desc.format = format;
		desc.width = w;
		desc.height = h;
		desc.depth = d;
		desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (d == 1 ? (VkImageUsageFlags)VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u);
		for (uint32_t i = 0; i < count; ++i)
			if (!dev.CreateImage(desc, t.images[i], "render target"))
			{
				DestroyInstance(t);
				g_targets.erase(TargetKey(target->id_, viewport));
				return nullptr;
			}
		t.width = w; t.height = h; t.depth = d; t.format = format;
		t.history = count == 2u;
		t.historyValid = false;
		t.historyReset = target->history_reset_;
		t.parity = 0;
		// Zero, so a first read (history, media) sees defined content.
		std::vector<FrameGraph::Handle> handles;
		for (uint32_t i = 0; i < count; ++i) handles.push_back(graph.Import(t.images[i]));
		Image* images[2] = {&t.images[0], count == 2u ? &t.images[1] : nullptr};
		auto& pass = graph.AddPass("render target clear", [images](VkCommandBuffer c)
		{
			const VkClearColorValue zero{};
			const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			for (Image* image : images)
				if (image) vkCmdClearColorImage(c, image->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
		});
		for (FrameGraph::Handle hnd : handles) pass.Use(hnd, Access::CopyDestination);
	}
	if (t.historyReset != target->history_reset_)
	{
		t.historyReset = target->history_reset_;
		t.historyValid = false;
	}
	t.lastFrame = dev.FrameNumber();
	return &t;
}

const HRL_RenderTarget* FindTarget(const hrl_scene_t* scene, HRL_id id)
{
	if (!scene) return nullptr;
	auto it = scene->render_targets.find(id);
	return it != scene->render_targets.end() ? it->second : nullptr;
}

bool EnsureDummyVolume(FrameGraph& graph)
{
	if (!g_dummyVolume)
	{
		ImageDesc desc;
		desc.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		desc.depth = 2; // a 3D image (1 x 1 x 2)
		desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if (!Dev().CreateImage(desc, g_dummyVolume, "media placeholder")) return false;
		g_dummyCleared = false;
	}
	if (!g_dummyCleared)
	{
		Image* image = &g_dummyVolume;
		graph.AddPass("media placeholder clear", [image](VkCommandBuffer c)
		{
			VkClearColorValue value{};
			value.float32[3] = 1.f; // no light scattered, everything transmitted
			const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdClearColorImage(c, image->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
		}).Use(graph.Import(g_dummyVolume), Access::CopyDestination);
		g_dummyCleared = true;
	}
	return true;
}

struct PassDataGPU
{
	glm::vec4 viewportRect, sceneSize, dispatchSize, frame, jitter, previousCamera, previousForward;
	glm::mat4 viewProjNoJitter, previousViewProj;
};
static_assert(sizeof(PassDataGPU) == 7 * 16 + 2 * 64, "PassDataGPU must match HRL_PassData (pass.glsl)");

std::string BlendKey(const std::vector<VkFormat>& formats, const std::vector<uint32_t>& blends)
{
	std::string key;
	for (size_t i = 0; i < formats.size(); ++i) key += std::to_string((int)formats[i]) + ":" + std::to_string(blends[i]) + ";";
	return key;
}
} // namespace

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
bool CreatePassShader(HRL_id id, const std::string& source, bool compute)
{
	auto fail = [id](const std::string& what)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
			"Vulkan render pass shader " + std::to_string((unsigned long long)id) + ": " + what);
		return false;
	};
	if (!RuntimeShadersAvailable())
		return fail("render pass shaders need HRL built with glslang (HRL_VULKAN_RUNTIME_SHADERS)");
	PassShader s;
	s.compute = compute;
	// Comments become spaces (same offsets, same lines): the declarations are found reliably.
	// Material parameters first: the resource declarations made below are `uniform` too.
	std::string body;
	ExtractPassParams(StripGlslComments(source), body, s.table.params);
	s.table.paramSlots = LayoutPassParams(s.table.params);
	std::string error;
	if (!ParseResources(body, s.resources, error)) return fail(error);
	for (const PassResource& r : s.resources)
		if (!compute && (r.kind == ResKind::Image2D || r.kind == ResKind::Image3D))
			return fail("storage images (" + r.name + ") are for compute passes; fullscreen passes write their outputs");
	s.usesDepth = ContainsIdentifier(body, "hrl_Depth") || ContainsIdentifier(body, "HRL_SceneDepth");
	s.usesSceneColor = ContainsIdentifier(body, "hrl_SceneColor");
	s.usesSceneImage = ContainsIdentifier(body, "hrl_SceneImage");
	if (s.usesSceneImage && !compute) return fail("hrl_SceneImage is for compute passes");
	if (compute)
		s.localSize = glm::uvec3(LocalSize(body, "local_size_x"), LocalSize(body, "local_size_y"), LocalSize(body, "local_size_z"));

	std::ostringstream src;
	src << "#version 460\n"
		<< "#extension GL_GOOGLE_include_directive : require\n"
		<< "#extension GL_EXT_nonuniform_qualifier : require\n"
		<< (compute ? "#define HRL_COMPUTE_STAGE 1\n" : "#define HRL_FULLSCREEN_STAGE 1\n");
	if (Dev().caps.rayQuery) src << "#define HRL_RAY_QUERY 1\n";
	src << "#include \"common.glsl\"\n#include \"brdf.glsl\"\n"
		<< "layout(set = 1, binding = 0) uniform sampler2D uTextures[];\n"
		<< "#include \"material_alpha.glsl\"\n#include \"lighting.glsl\"\n#include \"pass.glsl\"\n";
	if (!compute) src << "layout(location = 0) in vec2 vUV;\n";
	src << PassParamGlobals(s.table.params)
		<< "#define main hrlUserMain\n#line 1\n" << body << "\n#undef main\n"
		<< "void main()\n{\n\thrlLoadParameters();\n\thrlUserMain();\n}\n";
	std::string log;
	if (!CompilePassGlsl(compute, src.str(), s.spirv, log)) return fail("does not compile\n" + log);
	if (!MakeLayouts(s)) return false;
	const HRL_VkShaderBlob blob{s.spirv.data(), s.spirv.size() * sizeof(uint32_t)};
	if (compute)
	{
		s.pipeline = MakeComputePassPipeline(&blob, s.layout);
		if (!s.pipeline) { DestroyShader(s); return fail("pipeline creation failed"); }
	}
	DeletePassShader(id);
	g_shaders[id] = std::move(s);
	return true;
}

bool IsPassShader(HRL_id id) { return g_shaders.count(id) != 0; }

void DeletePassShader(HRL_id id)
{
	auto it = g_shaders.find(id);
	if (it == g_shaders.end()) return;
	DestroyShader(it->second);
	g_shaders.erase(it);
}

// ---------------------------------------------------------------------------
// Participating media
// ---------------------------------------------------------------------------
MediaBinding PrepareViewportMedia(FrameGraph& graph, const hrl_scene_t* scene, HRL_id viewportId, const HRL_Viewport* viewport,
	uint32_t vw, uint32_t vh)
{
	MediaBinding m;
	if (!EnsureDummyVolume(graph)) return m;
	m.integrated = m.visibility = &g_dummyVolume;
	m.params = glm::vec4(1.f, 1.f, 0.f, 0.f);
	if (!viewport || viewport->media_integrated_ == HRL_INVALID_ID) return m;
	const HRL_RenderTarget* integrated = FindTarget(scene, viewport->media_integrated_);
	if (!integrated) return m;
	if (integrated->depth_ < 2)
	{
		ReportOnce("the media volume of a viewport must be a 3D render target");
		return m;
	}
	TargetInstance* ti = EnsureInstance(graph, integrated, viewportId, vw, vh);
	if (!ti) return m;
	m.integrated = &ti->images[ti->parity];
	m.params = glm::vec4(viewport->media_max_distance_, viewport->media_exponent_, 1.f, 0.f);
	m.active = true;
	if (viewport->media_visibility_ != HRL_INVALID_ID)
		if (const HRL_RenderTarget* visibility = FindTarget(scene, viewport->media_visibility_))
		{
			if (visibility->depth_ < 2) ReportOnce("the visibility volume of a viewport must be a 3D render target");
			else if (TargetInstance* tv = EnsureInstance(graph, visibility, viewportId, vw, vh))
			{
				m.visibility = &tv->images[tv->parity];
				m.params.w = 1.f;
			}
		}
	return m;
}

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------
bool HasRenderPasses(const HRL_Viewport* viewport, HRL_ERenderStage stage)
{
	if (!viewport) return false;
	for (const auto& [id, p] : viewport->render_passes)
		if (p && p->enabled_ && p->stage_ == stage) return true;
	return false;
}

void BeginRenderPassView(const PassView& v)
{
	ViewHistory& h = g_views[TargetKey(v.sceneId, v.viewportId)];
	h.nextViewProj = v.viewProjNoJitter;
	h.nextCamera = v.camera;
	h.nextForward = v.forward;
	h.nextJitter = v.jitter;
	h.nextTime = HRL_InternalRenderTimeSeconds();
	h.lastFrame = Dev().FrameNumber();
}

FrameGraph::Handle AddRenderPasses(FrameGraph& graph, const HRL_SceneFrame& frame, const PassView& v, HRL_ERenderStage stage,
	FrameGraph::Handle color, FrameGraph::Handle depth, FrameGraph::Handle shadows, FrameGraph::Handle pointShadows)
{
	if (!v.viewport) return color;
	Device& dev = Dev();
	std::vector<const HRL_RenderPass*> passes;
	for (const auto& [id, p] : v.viewport->render_passes)
		if (p && p->enabled_ && p->stage_ == stage) passes.push_back(p);
	if (passes.empty()) return color;
	std::sort(passes.begin(), passes.end(), [](const HRL_RenderPass* a, const HRL_RenderPass* b)
	{
		return a->priority_ != b->priority_ ? a->priority_ < b->priority_ : a->id_ < b->id_;
	});
	const hrl_scene_t* scene = frame.scene;
	const uint32_t vw = (uint32_t)v.w, vh = (uint32_t)v.h;
	const ViewHistory& history = g_views[TargetKey(v.sceneId, v.viewportId)];

	// After the anti-aliasing, the HDR image may be one the passes cannot write:
	// a copy that can is made once for the stage when a pass writes the scene.
	if (stage == HRL_STAGE_BEFORE_TONEMAP)
	{
		bool writesScene = false;
		for (const HRL_RenderPass* p : passes)
		{
			auto m = frame.materials->find(p->material_);
			if (m == frame.materials->end() || !m->second) continue;
			auto s = g_shaders.find(m->second->shader_);
			if (s == g_shaders.end()) continue;
			writesScene = writesScene || s->second.usesSceneImage;
		}
		if (writesScene)
		{
			ImageDesc desc;
			desc.format = kHdrFormat;
			desc.width = v.sceneWidth;
			desc.height = v.sceneHeight;
			desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			const FrameGraph::Handle writable = graph.CreateTransient(desc, "scene hdr (render passes)");
			if (graph.GetImage(writable).handle)
			{
				Image* src = &graph.GetImage(color);
				Image* dst = &graph.GetImage(writable);
				const uint32_t w = v.sceneWidth, h = v.sceneHeight;
				graph.AddPass("render passes scene copy", [=](VkCommandBuffer c)
				{
					VkImageCopy region{};
					region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
					region.extent = {w, h, 1};
					vkCmdCopyImage(c, src->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
				}).Use(color, Access::CopySource).Use(writable, Access::CopyDestination);
				color = writable;
			}
		}
	}

	for (const HRL_RenderPass* p : passes)
	{
		const std::string passName = "render pass " + std::to_string((unsigned long long)p->id_);
		auto mit = frame.materials->find(p->material_);
		if (mit == frame.materials->end() || !mit->second) { ReportOnce(passName + ": its material does not exist"); continue; }
		const HRL_Material* material = mit->second;
		auto sit = g_shaders.find(material->shader_);
		if (sit == g_shaders.end()) { ReportOnce(passName + ": the shader of its material is not a render pass shader"); continue; }
		PassShader& shader = sit->second;
		if (stage == HRL_STAGE_BEFORE_OPAQUE && (shader.usesDepth || shader.usesSceneColor || shader.usesSceneImage))
		{
			ReportOnce(passName + ": the scene depth and colour do not exist before the opaque surfaces (HRL_STAGE_BEFORE_OPAQUE)");
			continue;
		}

		// ---- Resources of the shader --------------------------------------------
		struct ImageUse { FrameGraph::Handle handle; Access access; };
		std::vector<ImageUse> uses;
		std::vector<VkDescriptorImageInfo> imageInfos;
		std::vector<VkDescriptorBufferInfo> bufferInfos;
		imageInfos.reserve(shader.resources.size() + 4);
		bufferInfos.reserve(shader.resources.size() + 1);
		struct Write { uint32_t binding; VkDescriptorType type; size_t index; bool image; };
		std::vector<Write> writes;
		bool ok = true, historyValid = true;
		for (const PassResource& r : shader.resources)
		{
			if (r.kind == ResKind::Buffer)
			{
				auto b = p->buffers_.find(r.name);
				const size_t size = b != p->buffers_.end() ? b->second.data.size() : 0;
				const FrameSlice slice = dev.AllocFrame(std::max<size_t>(size, 16), 256);
				if (!slice.ptr) { ok = false; break; }
				std::memset(slice.ptr, 0, std::max<size_t>(size, 16));
				if (size) std::memcpy(slice.ptr, b->second.data.data(), size);
				bufferInfos.push_back({slice.buffer, slice.offset, std::max<VkDeviceSize>(size, 16)});
				writes.push_back({r.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferInfos.size() - 1, false});
				continue;
			}
			auto bind = p->bindings_.find(r.name);
			if (bind == p->bindings_.end())
			{
				ReportOnce(passName + ": no render target bound to '" + r.name + "' (HRL_SetRenderPassTarget)");
				ok = false;
				break;
			}
			const HRL_RenderTarget* target = FindTarget(scene, bind->second.target);
			if (!target) { ReportOnce(passName + ": the render target bound to '" + r.name + "' does not exist in this scene"); ok = false; break; }
			const bool volume = target->depth_ > 1;
			const bool storage = r.kind == ResKind::Image2D || r.kind == ResKind::Image3D;
			const bool wants3D = r.kind == ResKind::Image3D || r.kind == ResKind::Texture3D;
			if (volume != wants3D)
			{
				ReportOnce(passName + ": '" + r.name + "' is declared " + (wants3D ? "3D" : "2D") + " but its render target is not");
				ok = false;
				break;
			}
			if (storage != (bind->second.access == HRL_PASS_WRITE))
			{
				ReportOnce(passName + ": '" + r.name + (storage ? "' is a storage image: bind it with HRL_PASS_WRITE"
					: "' is sampled: bind it with HRL_PASS_READ or HRL_PASS_READ_PREVIOUS"));
				ok = false;
				break;
			}
			if (storage && r.format != FormatQualifier(TargetFormat(target->format_)))
			{
				ReportOnce(passName + ": '" + r.name + "' is declared " + r.format + " but its render target is " +
					FormatQualifier(TargetFormat(target->format_)));
				ok = false;
				break;
			}
			TargetInstance* t = EnsureInstance(graph, target, v.viewportId, vw, vh);
			if (!t) { ok = false; break; }
			Image* image = &t->images[t->parity];
			if (bind->second.access == HRL_PASS_READ_PREVIOUS)
			{
				if (!t->history)
				{
					ReportOnce(passName + ": '" + r.name + "' reads the previous frame of a target without history (HRL_SetRenderTargetHistory)");
					ok = false;
					break;
				}
				image = &t->images[t->parity ^ 1u];
				historyValid = historyValid && t->historyValid;
			}
			if (storage) t->written = true;
			const FrameGraph::Handle h = graph.Import(*image);
			uses.push_back({h, storage ? Access::Storage : Access::Sampled});
			const VkSampler sampler = target->linear_ ? R.linearClamp : R.pointClamp;
			imageInfos.push_back({storage ? VK_NULL_HANDLE : sampler, image->view,
				storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
			writes.push_back({r.binding, storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				imageInfos.size() - 1, true});
		}
		if (!ok) continue;

		// ---- Dispatch size / outputs ---------------------------------------------
		glm::uvec3 size(vw, vh, 1u);
		struct Output { FrameGraph::Handle handle; VkFormat format; uint32_t blend; bool scene; uint32_t w, h; };
		std::vector<Output> outputs;
		if (shader.compute)
		{
			if (p->dispatch_target_ != HRL_INVALID_ID)
			{
				const HRL_RenderTarget* target = FindTarget(scene, p->dispatch_target_);
				TargetInstance* t = target ? EnsureInstance(graph, target, v.viewportId, vw, vh) : nullptr;
				if (!t) { ReportOnce(passName + ": its dispatch target does not exist"); continue; }
				size = glm::uvec3(t->width, t->height, t->depth);
			}
			else if (p->dispatch_size_[0])
				size = glm::uvec3(p->dispatch_size_[0], p->dispatch_size_[1], p->dispatch_size_[2]);
			const glm::uvec3 divisor(p->dispatch_divisor_[0], p->dispatch_divisor_[1], p->dispatch_divisor_[2]);
			size = (size + divisor - 1u) / divisor;
		}
		else
		{
			for (uint32_t i = 0; i < kMaxPassOutputs; ++i)
			{
				const HRL_RenderPassOutput& o = p->outputs_[i];
				if (o.target == HRL_INVALID_ID) break; // outputs are consecutive from 0
				if (o.target == HRL_SCENE_COLOR_TARGET)
				{
					outputs.push_back({color, kHdrFormat, (uint32_t)o.blend, true, vw, vh});
					continue;
				}
				const HRL_RenderTarget* target = FindTarget(scene, o.target);
				if (!target || target->depth_ > 1)
				{
					ReportOnce(passName + ": output " + std::to_string(i) + " must be a 2D render target of the scene or HRL_SCENE_COLOR_TARGET");
					ok = false;
					break;
				}
				TargetInstance* t = EnsureInstance(graph, target, v.viewportId, vw, vh);
				if (!t) { ok = false; break; }
				t->written = true;
				outputs.push_back({graph.Import(t->images[t->parity]), t->format, (uint32_t)o.blend, false, t->width, t->height});
			}
			if (!ok) continue;
			if (outputs.empty()) { ReportOnce(passName + ": a fullscreen pass needs an output (HRL_SetRenderPassOutput)"); continue; }
			for (const Output& o : outputs)
				if (o.w != outputs[0].w || o.h != outputs[0].h || o.scene != outputs[0].scene)
				{
					ReportOnce(passName + ": the outputs of a fullscreen pass must have the same size");
					ok = false;
				}
			if (!ok) continue;
			size = glm::uvec3(outputs[0].w, outputs[0].h, 1u);
		}

		// ---- Built-in resources -----------------------------------------------------
		if (shader.usesDepth)
		{
			uses.push_back({depth, Access::Sampled});
			imageInfos.push_back({R.pointClamp, graph.GetImage(depth).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
			writes.push_back({kBindDepth, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageInfos.size() - 1, true});
		}
		if (shader.usesSceneColor)
		{
			ImageDesc desc;
			desc.format = kHdrFormat;
			desc.width = v.sceneWidth;
			desc.height = v.sceneHeight;
			desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			const FrameGraph::Handle copy = graph.CreateTransient(desc, "scene colour (render pass)");
			if (!graph.GetImage(copy).handle) continue;
			Image* src = &graph.GetImage(color);
			Image* dst = &graph.GetImage(copy);
			const uint32_t w = v.sceneWidth, h = v.sceneHeight;
			graph.AddPass("render pass scene colour copy", [=](VkCommandBuffer c)
			{
				VkImageCopy region{};
				region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				region.extent = {w, h, 1};
				vkCmdCopyImage(c, src->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			}).Use(color, Access::CopySource).Use(copy, Access::CopyDestination);
			uses.push_back({copy, Access::Sampled});
			imageInfos.push_back({R.linearClamp, dst->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
			writes.push_back({kBindSceneColor, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageInfos.size() - 1, true});
		}
		if (shader.usesSceneImage)
		{
			uses.push_back({color, Access::Storage});
			imageInfos.push_back({VK_NULL_HANDLE, graph.GetImage(color).view, VK_IMAGE_LAYOUT_GENERAL});
			writes.push_back({kBindSceneImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, imageInfos.size() - 1, true});
		}

		// ---- Frame data and material parameters ------------------------------------
		const size_t paramBytes = sizeof(PassDataGPU) + std::max<size_t>(shader.table.paramSlots, 1u) * sizeof(glm::vec4);
		const FrameSlice params = dev.AllocFrame(paramBytes, 256);
		if (!params.ptr) continue;
		std::memset(params.ptr, 0, paramBytes);
		PassDataGPU data{};
		data.viewportRect = glm::vec4(v.x, v.y, v.w, v.h);
		data.sceneSize = glm::vec4((float)v.sceneWidth, (float)v.sceneHeight, 1.f / (float)std::max(v.sceneWidth, 1u), 1.f / (float)std::max(v.sceneHeight, 1u));
		data.dispatchSize = glm::vec4(glm::vec3(size), 0.f);
		const float now = HRL_InternalRenderTimeSeconds();
		const bool viewHistory = history.valid;
		data.frame = glm::vec4((float)(dev.FrameNumber() % 16777216ull), now, viewHistory ? std::max(now - history.time, 0.f) : 0.f,
			(historyValid && viewHistory) ? 1.f : 0.f);
		data.jitter = glm::vec4(v.jitter, viewHistory ? history.jitter : v.jitter);
		data.previousCamera = glm::vec4(viewHistory ? history.camera : v.camera, 0.f);
		data.previousForward = glm::vec4(viewHistory ? history.forward : v.forward, 0.f);
		data.viewProjNoJitter = v.viewProjNoJitter;
		data.previousViewProj = viewHistory ? history.viewProj : v.viewProjNoJitter;
		std::memcpy(params.ptr, &data, sizeof(data));
		WriteCustomParams(shader.table, material, reinterpret_cast<glm::vec4*>(static_cast<unsigned char*>(params.ptr) + sizeof(PassDataGPU)));
		bufferInfos.push_back({params.buffer, params.offset, paramBytes});
		writes.push_back({kBindParams, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferInfos.size() - 1, false});

		const VkDescriptorSet set = AllocFrameSet(shader.setLayout);
		if (!set) continue;
		std::vector<VkWriteDescriptorSet> vkWrites;
		for (const Write& w : writes)
		{
			VkWriteDescriptorSet d{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			d.dstSet = set;
			d.dstBinding = w.binding;
			d.descriptorCount = 1;
			d.descriptorType = w.type;
			if (w.image) d.pImageInfo = &imageInfos[w.index];
			else d.pBufferInfo = &bufferInfos[w.index];
			vkWrites.push_back(d);
		}
		vkUpdateDescriptorSets(dev.device, (uint32_t)vkWrites.size(), vkWrites.data(), 0, nullptr);

		const VkDescriptorSet sets[3] = {v.viewSet, R.bindlessSet, set};
		const VkPipelineLayout layout = shader.layout;
		if (shader.compute)
		{
			const VkPipeline pipeline = shader.pipeline;
			const glm::uvec3 groups = (size + shader.localSize - 1u) / shader.localSize;
			auto& pass = graph.AddPass("render pass (compute)", [=](VkCommandBuffer c)
			{
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
				vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 3, sets, 0, nullptr);
				vkCmdDispatch(c, groups.x, groups.y, groups.z);
			});
			for (const ImageUse& u : uses) pass.Use(u.handle, u.access);
			pass.Use(shadows, Access::Sampled).Use(pointShadows, Access::Sampled);
			// After the surfaces, the media volumes of the view set may be read (HRL_SampleMedia).
			if (stage != HRL_STAGE_BEFORE_OPAQUE)
				for (Image* media : {v.mediaIntegrated, v.mediaVisibility})
					if (media) pass.Use(graph.Import(*media), Access::Sampled);
		}
		else
		{
			std::vector<VkFormat> formats;
			std::vector<uint32_t> blends;
			for (const Output& o : outputs) { formats.push_back(o.format); blends.push_back(o.blend); }
			const std::string key = BlendKey(formats, blends);
			VkPipeline& pipeline = shader.graphics[key];
			if (!pipeline)
			{
				const HRL_VkShaderBlob blob{shader.spirv.data(), shader.spirv.size() * sizeof(uint32_t)};
				pipeline = MakeFullscreenPassPipeline(&blob, shader.layout, formats, blends);
			}
			if (!pipeline) { ReportOnce(passName + ": pipeline creation failed"); continue; }
			const VkPipeline pl = pipeline;
			const bool scene = outputs[0].scene;
			const float x = scene ? v.x : 0.f, y = scene ? v.y : 0.f;
			const float w = (float)size.x, h = (float)size.y;
			auto& pass = graph.AddPass("render pass (fullscreen)", [=](VkCommandBuffer c)
			{
				const VkViewport vp{x, y, w, h, 0.f, 1.f};
				const VkRect2D sc{{(int32_t)x, (int32_t)y}, {(uint32_t)w, (uint32_t)h}};
				vkCmdSetViewport(c, 0, 1, &vp);
				vkCmdSetScissor(c, 0, 1, &sc);
				vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
				vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, sets, 0, nullptr);
				vkCmdDraw(c, 3, 1, 0, 0);
			});
			for (const ImageUse& u : uses) pass.Use(u.handle, u.access);
			pass.Use(shadows, Access::Sampled).Use(pointShadows, Access::Sampled);
			if (stage != HRL_STAGE_BEFORE_OPAQUE)
				for (Image* media : {v.mediaIntegrated, v.mediaVisibility})
					if (media) pass.Use(graph.Import(*media), Access::Sampled);
			for (const Output& o : outputs) pass.Color(o.handle, VK_ATTACHMENT_LOAD_OP_LOAD);
		}
	}
	return color;
}

void EndRenderPassFrame(HRL_id sceneId, HRL_id viewportId)
{
	auto it = g_views.find(TargetKey(sceneId, viewportId));
	if (it != g_views.end())
	{
		ViewHistory& h = it->second;
		h.viewProj = h.nextViewProj;
		h.camera = h.nextCamera;
		h.forward = h.nextForward;
		h.jitter = h.nextJitter;
		h.time = h.nextTime;
		h.valid = true;
	}
	// Targets with a history written this frame: this frame becomes the previous one.
	for (auto& [key, t] : g_targets)
	{
		if ((HRL_id)(key & 0xFFFFFFFFull) != viewportId || !t.written) continue;
		t.written = false;
		if (!t.history) continue;
		t.parity ^= 1u;
		t.historyValid = true;
	}
}

void ReleaseUnusedRenderTargets()
{
	const uint64_t frame = Dev().FrameNumber();
	for (auto it = g_targets.begin(); it != g_targets.end();)
	{
		if (frame > it->second.lastFrame + 8)
		{
			DestroyInstance(it->second);
			it = g_targets.erase(it);
		}
		else ++it;
	}
	for (auto it = g_views.begin(); it != g_views.end();)
	{
		if (frame > it->second.lastFrame + 8) it = g_views.erase(it);
		else ++it;
	}
}

void ShutdownRenderPipeline()
{
	for (auto& [key, t] : g_targets) DestroyInstance(t);
	g_targets.clear();
	g_views.clear();
	for (auto& [id, s] : g_shaders) DestroyShader(s);
	g_shaders.clear();
	Dev().DestroyImage(g_dummyVolume, false);
	g_dummyCleared = false;
	g_reported.clear();
}
} // namespace hrlvk
