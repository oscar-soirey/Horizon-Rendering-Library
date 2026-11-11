// HRL native Vulkan backend - global illumination (HRL_GI_DDGI).
//
// The irradiance probe volume of core/gi_probes.cpp (shared CPU code: probes
// traced against a BVH of the static meshes, a few probes per frame, until the
// volume converges) is uploaded as 6 directional lobes per probe and sampled
// by the lit shaders (lighting.glsl, SampleProbeGI): indirect diffuse light.
#include "vk_renderer_internal.h"
#include "../../core/gi_probes.h"

namespace hrlvk
{
namespace
{
class VulkanProbes final : public HRL_GIProbes
{
public:
	std::unordered_map<HRL_id, std::vector<glm::vec4>> lobes; // 6 per probe: +X -X +Y -Y +Z -Z, w = valid

protected:
	void OnUploadProbeBlocks(HRL_id sceneId, const ProbeBlocks& blocks) override
	{
		std::vector<glm::vec4>& out = lobes[sceneId];
		out.assign((size_t)kMaxProbeCount * kDirectionalLobes, glm::vec4(0.f));
		for (int block = 0; block < 3; ++block)
			for (size_t probe = 0; probe < (size_t)kMaxProbeCount; ++probe)
				for (int j = 0; j < kProbeLobesPerBlock; ++j)
				{
					const size_t source = probe * kProbeLobesPerBlock + (size_t)j;
					if (source < blocks[(size_t)block].size())
						out[probe * kDirectionalLobes + (size_t)(block * kProbeLobesPerBlock + j)] = blocks[(size_t)block][source];
				}
	}
	void OnReleaseSceneGPU(HRL_id sceneId) override { lobes.erase(sceneId); }
};
VulkanProbes g_probes;
} // namespace

GIFrame PrepareGI(HRL_id sceneId, const hrl_scene_t* scene)
{
	GIFrame out;
	if (!scene->global_illumination_enabled || scene->global_illumination_method != HRL_GI_DDGI)
	{
		g_probes.ReleaseScene(sceneId);
		return out;
	}
	g_probes.UpdateDDGI(sceneId);
	auto it = g_probes.lobes.find(sceneId);
	if (!g_probes.IsActive(sceneId) || it == g_probes.lobes.end()) return out;
	const HRL_GIProbes::ShaderParams p = g_probes.GetShaderParams(sceneId);
	out.probes = Dev().AllocFrame(it->second.size() * sizeof(glm::vec4), 256);
	if (!out.probes.ptr) return GIFrame{};
	std::memcpy(out.probes.ptr, it->second.data(), it->second.size() * sizeof(glm::vec4));
	out.origin = glm::vec4(p.origin, p.strength * std::max(scene->global_illumination_intensity, 0.f));
	out.spacing = glm::vec4(p.spacing, 0.f);
	out.resolution = glm::vec4(p.resolution, 1.f);
	return out;
}

void ReleaseGIScene(HRL_id sceneId) { g_probes.ReleaseScene(sceneId); }
void ShutdownGI() { g_probes.Shutdown(); g_probes.lobes.clear(); }
} // namespace hrlvk
