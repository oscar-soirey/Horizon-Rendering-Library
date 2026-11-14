// HRL extras - CustomShaderRenderer: a custom renderer for one viewport, built
// on the programmable render pipeline of HRL (Vulkan, see "PROGRAMMABLE RENDER
// PIPELINE" in hrl.h).
//
// It names and owns what a custom look is made of:
//   * render targets (2D / 3D, per viewport, optional history),
//   * render passes (compute / fullscreen shaders at a stage of the frame),
//   * surface shaders and their materials (HRL_CreateShader with Surface()),
//   * the participating media of the viewport and the built-in grading,
// shares code between all its shaders (SetSharedCode) and parameters between
// all its materials (SetGlobal*: one call reaches every pass and surface).
// Everything it made is released by its destructor.
//
//   hrl::CustomShaderRenderer r(scene, viewport);
//   r.AddTarget("Light", HRL_RENDER_TARGET_RGBA16F, 0.5f);
//   r.AddComputePass("light", HRL_STAGE_AFTER_OPAQUE, 0, source);
//   r.Bind("light", "Out", "Light", HRL_PASS_WRITE);
//   r.SetFloat("light", "Strength", 2.f);
//
// Header-only: it only uses the public C API.
#ifndef HRL_EXTRAS_CUSTOM_SHADER_RENDERER_HPP
#define HRL_EXTRAS_CUSTOM_SHADER_RENDERER_HPP

#include <hrl.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace hrl
{
class CustomShaderRenderer
{
public:
	CustomShaderRenderer(HRL_id scene, HRL_id viewport) : scene_(scene), viewport_(viewport) {}
	~CustomShaderRenderer() { Release(); }
	CustomShaderRenderer(const CustomShaderRenderer&) = delete;
	CustomShaderRenderer& operator=(const CustomShaderRenderer&) = delete;

	HRL_id Scene() const { return scene_; }
	HRL_id Viewport() const { return viewport_; }
	// False once a shader failed to compile or a name was unknown (see LastError()).
	bool Ok() const { return error_.empty(); }
	const std::string& LastError() const { return error_; }

	// Code prepended to every shader made afterwards (after its #version line if any).
	void SetSharedCode(std::string code) { shared_ = std::move(code); }

	// ---- Render targets ---------------------------------------------------------
	// Follows the viewport size (times `scale`); depth > 1 : 3D target.
	HRL_id AddTarget(const std::string& name, HRL_ERenderTargetFormat format, float scale, HRL_uint depth = 1, bool history = false)
	{
		return Keep(name, HRL_CreateViewportRenderTarget(scene_, format, scale, depth), history);
	}
	HRL_id AddFixedTarget(const std::string& name, HRL_ERenderTargetFormat format, HRL_uint width, HRL_uint height,
		HRL_uint depth = 1, bool history = false)
	{
		return Keep(name, HRL_CreateRenderTarget(scene_, format, width, height, depth), history);
	}
	HRL_id Target(const std::string& name) const
	{
		auto it = targets_.find(name);
		return it != targets_.end() ? it->second : HRL_INVALID_ID;
	}
	void ResetHistory(const std::string& target) { if (HRL_id t = Find(targets_, target, "target")) HRL_ResetRenderTargetHistory(t); }

	// ---- Render passes ----------------------------------------------------------
	HRL_id AddComputePass(const std::string& name, HRL_ERenderStage stage, int priority, const std::string& source)
	{
		return AddPass(name, stage, priority, source, true);
	}
	HRL_id AddFullscreenPass(const std::string& name, HRL_ERenderStage stage, int priority, const std::string& source)
	{
		return AddPass(name, stage, priority, source, false);
	}
	HRL_id Pass(const std::string& name) const
	{
		auto it = passes_.find(name);
		return it != passes_.end() ? it->second.pass : HRL_INVALID_ID;
	}
	// Material of a pass (its parameters) or of a surface (CreateSurfaceMaterial).
	HRL_id Material(const std::string& name) const
	{
		auto it = passes_.find(name);
		if (it != passes_.end()) return it->second.material;
		auto m = surfaceMaterials_.find(name);
		return m != surfaceMaterials_.end() ? m->second : HRL_INVALID_ID;
	}
	void Enable(const std::string& pass, bool enable) { if (HRL_id p = FindPass(pass)) HRL_SetRenderPassEnabled(p, enable); }
	bool Bind(const std::string& pass, const std::string& resource, const std::string& target, HRL_ERenderPassAccess access)
	{
		const HRL_id p = FindPass(pass), t = Find(targets_, target, "target");
		if (!p || !t) return false;
		HRL_SetRenderPassTarget(p, resource.c_str(), t, access);
		return true;
	}
	void DispatchOver(const std::string& pass, const std::string& target)
	{
		const HRL_id p = FindPass(pass), t = Find(targets_, target, "target");
		if (p && t) HRL_SetRenderPassDispatchTarget(p, t);
	}
	void DispatchSize(const std::string& pass, HRL_uint x, HRL_uint y, HRL_uint z)
	{
		if (HRL_id p = FindPass(pass)) HRL_SetRenderPassDispatchSize(p, x, y, z);
	}
	// Fullscreen passes: `target` is a target name or "scene" (the HDR image of the viewport).
	void Output(const std::string& pass, HRL_uint index, const std::string& target, HRL_EPassBlend blend)
	{
		const HRL_id p = FindPass(pass);
		const HRL_id t = target == "scene" ? HRL_SCENE_COLOR_TARGET : Find(targets_, target, "target");
		if (p && t) HRL_SetRenderPassOutput(p, index, t, blend);
	}
	void SetBuffer(const std::string& pass, const std::string& name, const void* data, size_t size)
	{
		if (HRL_id p = FindPass(pass)) HRL_SetRenderPassBuffer(p, name.c_str(), data, size);
	}
	template <class T>
	void SetBuffer(const std::string& pass, const std::string& name, const std::vector<T>& values)
	{
		SetBuffer(pass, name, values.data(), values.size() * sizeof(T));
	}

	// ---- Surface shaders --------------------------------------------------------
	// A surface function (and optionally a vertex function), see custom.glsl.
	HRL_id AddSurfaceShader(const std::string& name, const std::string& vertex, const std::string& surface)
	{
		const std::string v = vertex.empty() ? std::string() : WithShared(vertex);
		const std::string f = WithShared(surface);
		const HRL_id shader = HRL_CreateShader(v.empty() ? nullptr : v.c_str(), v.size(), f.c_str(), f.size());
		if (shader == HRL_INVALID_ID) { Fail("surface shader '" + name + "' does not compile (see the HRL error callback)"); return shader; }
		surfaceShaders_[name] = shader;
		return shader;
	}
	// A material of a surface shader, registered under `name` (receives the globals).
	HRL_id CreateSurfaceMaterial(const std::string& name, const std::string& shader)
	{
		const HRL_id s = Find(surfaceShaders_, shader, "surface shader");
		if (!s) return HRL_INVALID_ID;
		const HRL_id m = HRL_CreateMaterial(s);
		surfaceMaterials_[name] = m;
		ApplyGlobals(m);
		return m;
	}

	// ---- Parameters -------------------------------------------------------------
	// `owner` is a pass name or a surface material name.
	void SetFloat(const std::string& owner, const char* name, float v) { if (HRL_id m = FindMaterial(owner)) HRL_MaterialSetFloat(m, name, v); }
	void SetInt(const std::string& owner, const char* name, int v) { if (HRL_id m = FindMaterial(owner)) HRL_MaterialSetInt(m, name, v); }
	void SetVec2(const std::string& owner, const char* name, float x, float y) { if (HRL_id m = FindMaterial(owner)) HRL_MaterialSetVec2(m, name, x, y); }
	void SetVec3(const std::string& owner, const char* name, float x, float y, float z) { if (HRL_id m = FindMaterial(owner)) HRL_MaterialSetVec3(m, name, x, y, z); }
	void SetVec4(const std::string& owner, const char* name, float x, float y, float z, float w) { if (HRL_id m = FindMaterial(owner)) HRL_MaterialSetVec4(m, name, x, y, z, w); }
	void SetTexture(const std::string& owner, const char* name, HRL_id texture) { if (HRL_id m = FindMaterial(owner)) HRL_MaterialSetTexture(m, name, texture); }

	// Set on every pass and surface material of the renderer, present and future.
	void SetGlobalFloat(const std::string& name, float v) { SetGlobal(name, Value{Value::Float, {v, 0, 0, 0}, 0}); }
	void SetGlobalVec3(const std::string& name, float x, float y, float z) { SetGlobal(name, Value{Value::Vec3, {x, y, z, 0}, 0}); }
	void SetGlobalVec4(const std::string& name, float x, float y, float z, float w) { SetGlobal(name, Value{Value::Vec4, {x, y, z, w}, 0}); }
	void SetGlobalTexture(const std::string& name, HRL_id texture) { SetGlobal(name, Value{Value::Texture, {0, 0, 0, 0}, texture}); }

	// ---- Viewport ---------------------------------------------------------------
	// Participating media applied by HRL to every surface (see HRL_SetViewportMediaVolume).
	void SetMediaVolume(const std::string& integrated, const std::string& visibility, float maxDistance, float exponent)
	{
		const HRL_id i = Find(targets_, integrated, "target");
		const HRL_id v = visibility.empty() ? HRL_INVALID_ID : Find(targets_, visibility, "target");
		if (i) HRL_SetViewportMediaVolume(viewport_, i, v, maxDistance, exponent);
	}
	// Material of the built-in post-process of the viewport (exposure, bloom, tone mapping,
	// contrast, saturation, vignette, film grain...), created on first use.
	HRL_id Grading()
	{
		if (!grading_)
		{
			grading_ = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
			gradingPass_ = HRL_CreatePostProcess(viewport_, grading_, 0);
		}
		return grading_;
	}

	void Release()
	{
		if (HRL_IsValidViewport(viewport_)) HRL_SetViewportMediaVolume(viewport_, HRL_INVALID_ID, HRL_INVALID_ID, 1.f, 1.f);
		for (auto& [name, p] : passes_)
		{
			if (HRL_IsValidRenderPass(p.pass)) HRL_DeleteRenderPass(p.pass);
			if (HRL_IsValidMaterial(p.material)) HRL_DeleteMaterial(p.material);
			if (HRL_IsValidShader(p.shader)) HRL_DeleteShader(p.shader);
		}
		for (auto& [name, m] : surfaceMaterials_) if (HRL_IsValidMaterial(m)) HRL_DeleteMaterial(m);
		for (auto& [name, s] : surfaceShaders_) if (HRL_IsValidShader(s)) HRL_DeleteShader(s);
		for (auto& [name, t] : targets_) if (HRL_IsValidRenderTarget(t)) HRL_DeleteRenderTarget(t);
		if (gradingPass_ && HRL_IsValidPostProcess(gradingPass_)) HRL_DeletePostProcess(gradingPass_);
		if (grading_ && HRL_IsValidMaterial(grading_)) HRL_DeleteMaterial(grading_);
		passes_.clear(); surfaceMaterials_.clear(); surfaceShaders_.clear(); targets_.clear();
		grading_ = gradingPass_ = 0;
	}

private:
	struct PassEntry { HRL_id shader = 0, material = 0, pass = 0; };
	struct Value
	{
		enum Kind { Float, Vec3, Vec4, Texture } kind;
		float v[4];
		HRL_id texture;
	};

	void Fail(const std::string& message) { error_ = message; }

	HRL_id Keep(const std::string& name, HRL_id target, bool history)
	{
		if (target == HRL_INVALID_ID) { Fail("render target '" + name + "' could not be created"); return target; }
		if (history) HRL_SetRenderTargetHistory(target, HRL_TRUE);
		targets_[name] = target;
		return target;
	}

	std::string WithShared(const std::string& source) const
	{
		if (shared_.empty()) return source;
		// After a #version line, so the shader still starts with it.
		const size_t version = source.find("#version");
		if (version != std::string::npos)
		{
			const size_t eol = source.find('\n', version);
			if (eol != std::string::npos) return source.substr(0, eol + 1) + shared_ + "\n" + source.substr(eol + 1);
		}
		return shared_ + "\n" + source;
	}

	HRL_id AddPass(const std::string& name, HRL_ERenderStage stage, int priority, const std::string& source, bool compute)
	{
		const std::string code = WithShared(source);
		PassEntry e;
		e.shader = compute ? HRL_CreateComputeShader(code.c_str(), code.size()) : HRL_CreateFullscreenShader(code.c_str(), code.size());
		if (e.shader == HRL_INVALID_ID) { Fail("pass '" + name + "' does not compile (see the HRL error callback)"); return HRL_INVALID_ID; }
		e.material = HRL_CreateMaterial(e.shader);
		e.pass = HRL_CreateRenderPass(viewport_, stage, priority, e.material);
		if (e.pass == HRL_INVALID_ID) { Fail("pass '" + name + "' could not be created"); return HRL_INVALID_ID; }
		passes_[name] = e;
		ApplyGlobals(e.material);
		return e.pass;
	}

	template <class Map>
	HRL_id Find(const Map& map, const std::string& name, const char* kind)
	{
		auto it = map.find(name);
		if (it == map.end()) { Fail(std::string("unknown ") + kind + " '" + name + "'"); return 0; }
		return it->second;
	}
	HRL_id FindPass(const std::string& name)
	{
		auto it = passes_.find(name);
		if (it == passes_.end()) { Fail("unknown pass '" + name + "'"); return 0; }
		return it->second.pass;
	}
	HRL_id FindMaterial(const std::string& owner)
	{
		const HRL_id m = Material(owner);
		if (!m) Fail("unknown pass or surface material '" + owner + "'");
		return m;
	}

	static void Apply(HRL_id material, const std::string& name, const Value& v)
	{
		switch (v.kind)
		{
		case Value::Float: HRL_MaterialSetFloat(material, name.c_str(), v.v[0]); break;
		case Value::Vec3: HRL_MaterialSetVec3(material, name.c_str(), v.v[0], v.v[1], v.v[2]); break;
		case Value::Vec4: HRL_MaterialSetVec4(material, name.c_str(), v.v[0], v.v[1], v.v[2], v.v[3]); break;
		case Value::Texture: HRL_MaterialSetTexture(material, name.c_str(), v.texture); break;
		}
	}
	void ApplyGlobals(HRL_id material)
	{
		for (const auto& [name, value] : globals_) Apply(material, name, value);
	}
	void SetGlobal(const std::string& name, const Value& value)
	{
		globals_[name] = value;
		for (auto& [n, p] : passes_) Apply(p.material, name, value);
		for (auto& [n, m] : surfaceMaterials_) Apply(m, name, value);
	}

	HRL_id scene_, viewport_;
	std::string shared_, error_;
	std::unordered_map<std::string, HRL_id> targets_, surfaceShaders_, surfaceMaterials_;
	std::unordered_map<std::string, PassEntry> passes_;
	std::unordered_map<std::string, Value> globals_;
	HRL_id grading_ = 0, gradingPass_ = 0;
};
} // namespace hrl

#endif
