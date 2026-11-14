// HRL frontend - programmable render pipeline: render targets, render passes
// and the participating media of the viewports (Vulkan backend).
//
// Everything here is plain data read by the backend at frame time; GPU
// resources (per viewport instances of the targets, pipelines) are made by the
// backend when a frame first needs them.
#include "frontend_internal.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace
{
bool RequirePassBackend(const char* fn)
{
	if (g_Backend.RHI_CreatePassShader) return true;
	SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
		std::string(fn) + ": render passes need the Vulkan backend");
	return false;
}

HRL_RenderTarget* FindTarget(HRL_id id, const char* fn)
{
	auto it = ctx_.render_targets.find(id);
	if (it == ctx_.render_targets.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(fn) + ": invalid render target ID");
		return nullptr;
	}
	return it->second;
}

HRL_RenderPass* FindPass(HRL_id id, const char* fn)
{
	auto it = ctx_.render_passes.find(id);
	if (it == ctx_.render_passes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(fn) + ": invalid render pass ID");
		return nullptr;
	}
	return it->second;
}

HRL_id CreatePassShader(const char* source, size_t size, bool compute, const char* fn)
{
	if (!RequirePassBackend(fn)) return HRL_INVALID_ID;
	if (!source || !size)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, std::string(fn) + ": empty source");
		return HRL_INVALID_ID;
	}
	const HRL_id id = GenerateHRL_ID();
	return g_Backend.RHI_CreatePassShader(id, source, size, compute ? 1 : 0) ? id : HRL_INVALID_ID;
}

HRL_id CreateTarget(HRL_id scene, HRL_ERenderTargetFormat format, HRL_uint width, HRL_uint height, HRL_uint depth,
	float scale, const char* fn)
{
	auto sceneIt = ctx_.scenes.find(scene);
	if (sceneIt == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(fn) + ": invalid scene ID");
		return HRL_INVALID_ID;
	}
	if (format < HRL_RENDER_TARGET_RGBA16F || format > HRL_RENDER_TARGET_RGBA8)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, std::string(fn) + ": invalid format");
		return HRL_INVALID_ID;
	}
	auto* t = new HRL_RenderTarget();
	t->id_ = GenerateHRL_ID();
	t->scene_ = scene;
	t->format_ = format;
	t->width_ = std::max<HRL_uint>(width, 1u);
	t->height_ = std::max<HRL_uint>(height, 1u);
	t->depth_ = std::max<HRL_uint>(depth, 1u);
	t->scale_ = scale;
	sceneIt->second->render_targets.emplace(t->id_, t);
	ctx_.render_targets.emplace(t->id_, t);
	return t->id_;
}
} // namespace

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
HRL_id HRL_CreateComputeShader(const char* source, size_t size)
{
	return CreatePassShader(source, size, true, "HRL_CreateComputeShader");
}

HRL_id HRL_CreateFullscreenShader(const char* source, size_t size)
{
	return CreatePassShader(source, size, false, "HRL_CreateFullscreenShader");
}

// ---------------------------------------------------------------------------
// Render targets
// ---------------------------------------------------------------------------
HRL_id HRL_CreateRenderTarget(HRL_id scene, HRL_ERenderTargetFormat format, HRL_uint width, HRL_uint height, HRL_uint depth)
{
	if (!width || !height)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateRenderTarget: zero size");
		return HRL_INVALID_ID;
	}
	return CreateTarget(scene, format, width, height, depth, 0.f, "HRL_CreateRenderTarget");
}

HRL_id HRL_CreateViewportRenderTarget(HRL_id scene, HRL_ERenderTargetFormat format, float scale, HRL_uint depth)
{
	if (!(scale > 0.f) || scale > 4.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateViewportRenderTarget: scale must be in ]0, 4]");
		return HRL_INVALID_ID;
	}
	return CreateTarget(scene, format, 1, 1, depth, scale, "HRL_CreateViewportRenderTarget");
}

void HRL_DeleteRenderTarget(HRL_id target)
{
	auto it = ctx_.render_targets.find(target);
	if (it == ctx_.render_targets.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteRenderTarget: invalid ID");
		return;
	}
	HRL_RenderTarget* t = it->second;
	if (t)
	{
		auto sceneIt = ctx_.scenes.find(t->scene_);
		if (sceneIt != ctx_.scenes.end()) sceneIt->second->render_targets.erase(target);
	}
	// Bindings that name the target are left as they are: the backend skips a
	// pass whose resources do not exist (and reports it once).
	for (auto& [vid, viewport] : ctx_.viewports)
	{
		(void)vid;
		if (!viewport) continue;
		if (viewport->media_integrated_ == target) viewport->media_integrated_ = HRL_INVALID_ID;
		if (viewport->media_visibility_ == target) viewport->media_visibility_ = HRL_INVALID_ID;
	}
	delete t;
	ctx_.render_targets.erase(it);
}

int HRL_IsValidRenderTarget(HRL_id target)
{
	return ctx_.render_targets.find(target) != ctx_.render_targets.end() ? HRL_TRUE : HRL_FALSE;
}

void HRL_SetRenderTargetHistory(HRL_id target, int enable)
{
	if (HRL_RenderTarget* t = FindTarget(target, "HRL_SetRenderTargetHistory")) t->history_ = enable != 0;
}

void HRL_ResetRenderTargetHistory(HRL_id target)
{
	if (HRL_RenderTarget* t = FindTarget(target, "HRL_ResetRenderTargetHistory")) ++t->history_reset_;
}

void HRL_SetRenderTargetLinearFilter(HRL_id target, int linear)
{
	if (HRL_RenderTarget* t = FindTarget(target, "HRL_SetRenderTargetLinearFilter")) t->linear_ = linear != 0;
}

// ---------------------------------------------------------------------------
// Render passes
// ---------------------------------------------------------------------------
HRL_id HRL_CreateRenderPass(HRL_id viewport, HRL_ERenderStage stage, int priority, HRL_id material)
{
	if (!RequirePassBackend("HRL_CreateRenderPass")) return HRL_INVALID_ID;
	auto vit = ctx_.viewports.find(viewport);
	if (vit == ctx_.viewports.end() || !vit->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateRenderPass: invalid viewport ID");
		return HRL_INVALID_ID;
	}
	if (ctx_.materials.find(material) == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateRenderPass: invalid material ID");
		return HRL_INVALID_ID;
	}
	if (stage < HRL_STAGE_BEFORE_OPAQUE || stage > HRL_STAGE_BEFORE_TONEMAP)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateRenderPass: invalid stage");
		return HRL_INVALID_ID;
	}
	auto* p = new HRL_RenderPass();
	p->id_ = GenerateHRL_ID();
	p->viewport_ = viewport;
	p->stage_ = stage;
	p->priority_ = priority;
	p->material_ = material;
	vit->second->render_passes.emplace(p->id_, p);
	ctx_.render_passes.emplace(p->id_, p);
	return p->id_;
}

void HRL_DeleteRenderPass(HRL_id pass)
{
	auto it = ctx_.render_passes.find(pass);
	if (it == ctx_.render_passes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteRenderPass: invalid ID");
		return;
	}
	if (it->second)
	{
		auto vit = ctx_.viewports.find(it->second->viewport_);
		if (vit != ctx_.viewports.end() && vit->second) vit->second->render_passes.erase(pass);
	}
	delete it->second;
	ctx_.render_passes.erase(it);
}

int HRL_IsValidRenderPass(HRL_id pass)
{
	return ctx_.render_passes.find(pass) != ctx_.render_passes.end() ? HRL_TRUE : HRL_FALSE;
}

void HRL_SetRenderPassEnabled(HRL_id pass, int enable)
{
	if (HRL_RenderPass* p = FindPass(pass, "HRL_SetRenderPassEnabled")) p->enabled_ = enable != 0;
}

void HRL_SetRenderPassTarget(HRL_id pass, const char* name, HRL_id target, HRL_ERenderPassAccess access)
{
	HRL_RenderPass* p = FindPass(pass, "HRL_SetRenderPassTarget");
	if (!p) return;
	if (!name || !*name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetRenderPassTarget: empty name");
		return;
	}
	if (target == HRL_INVALID_ID) { p->bindings_.erase(name); return; }
	if (!FindTarget(target, "HRL_SetRenderPassTarget")) return;
	if (access < HRL_PASS_READ || access > HRL_PASS_READ_PREVIOUS)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetRenderPassTarget: invalid access");
		return;
	}
	p->bindings_[name] = HRL_RenderPassBinding{target, access};
}

void HRL_SetRenderPassBuffer(HRL_id pass, const char* name, const void* data, size_t size)
{
	HRL_RenderPass* p = FindPass(pass, "HRL_SetRenderPassBuffer");
	if (!p) return;
	if (!name || !*name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetRenderPassBuffer: empty name");
		return;
	}
	HRL_RenderPassBuffer& b = p->buffers_[name];
	b.data.assign(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + (data ? size : 0));
}

void HRL_SetRenderPassDispatchTarget(HRL_id pass, HRL_id target)
{
	HRL_RenderPass* p = FindPass(pass, "HRL_SetRenderPassDispatchTarget");
	if (!p) return;
	if (target != HRL_INVALID_ID && !FindTarget(target, "HRL_SetRenderPassDispatchTarget")) return;
	p->dispatch_target_ = target;
	p->dispatch_size_[0] = p->dispatch_size_[1] = p->dispatch_size_[2] = 0;
}

void HRL_SetRenderPassDispatchSize(HRL_id pass, HRL_uint x, HRL_uint y, HRL_uint z)
{
	HRL_RenderPass* p = FindPass(pass, "HRL_SetRenderPassDispatchSize");
	if (!p) return;
	p->dispatch_target_ = HRL_INVALID_ID;
	p->dispatch_size_[0] = std::max<HRL_uint>(x, 1u);
	p->dispatch_size_[1] = std::max<HRL_uint>(y, 1u);
	p->dispatch_size_[2] = std::max<HRL_uint>(z, 1u);
}

void HRL_SetRenderPassDispatchDivisor(HRL_id pass, HRL_uint x, HRL_uint y, HRL_uint z)
{
	HRL_RenderPass* p = FindPass(pass, "HRL_SetRenderPassDispatchDivisor");
	if (!p) return;
	p->dispatch_divisor_[0] = std::max<HRL_uint>(x, 1u);
	p->dispatch_divisor_[1] = std::max<HRL_uint>(y, 1u);
	p->dispatch_divisor_[2] = std::max<HRL_uint>(z, 1u);
}

void HRL_SetRenderPassOutput(HRL_id pass, HRL_uint index, HRL_id target, HRL_EPassBlend blend)
{
	HRL_RenderPass* p = FindPass(pass, "HRL_SetRenderPassOutput");
	if (!p) return;
	if (index > 3)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetRenderPassOutput: index must be 0..3");
		return;
	}
	if (target != HRL_INVALID_ID && target != HRL_SCENE_COLOR_TARGET && !FindTarget(target, "HRL_SetRenderPassOutput")) return;
	if (blend < HRL_PASS_BLEND_NONE || blend > HRL_PASS_BLEND_PREMULTIPLIED)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetRenderPassOutput: invalid blend mode");
		return;
	}
	p->outputs_[index] = HRL_RenderPassOutput{target, blend};
}

// ---------------------------------------------------------------------------
// Participating media
// ---------------------------------------------------------------------------
void HRL_SetViewportMediaVolume(HRL_id viewport, HRL_id integrated, HRL_id visibility, float maxDistance, float exponent)
{
	auto vit = ctx_.viewports.find(viewport);
	if (vit == ctx_.viewports.end() || !vit->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportMediaVolume: invalid viewport ID");
		return;
	}
	if (integrated != HRL_INVALID_ID && !FindTarget(integrated, "HRL_SetViewportMediaVolume")) return;
	if (visibility != HRL_INVALID_ID && !FindTarget(visibility, "HRL_SetViewportMediaVolume")) return;
	if (!(maxDistance > 0.f) || !(exponent >= 1.f))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetViewportMediaVolume: the distance must be > 0 and the exponent >= 1");
		return;
	}
	HRL_Viewport* v = vit->second;
	v->media_integrated_ = integrated;
	v->media_visibility_ = integrated != HRL_INVALID_ID ? visibility : HRL_INVALID_ID;
	v->media_max_distance_ = maxDistance;
	v->media_exponent_ = exponent;
}
