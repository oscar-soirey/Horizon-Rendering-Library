// HRL native Vulkan backend - screen space effects and custom post-processes.
//
// The effects of the scene (decals, screen space reflections, ambient
// occlusion, local and global volumetric fog, god rays, volumetric clouds) run
// the very shaders of the OpenGL backend (backend/common/hrl_builtin_glsl.h),
// compiled at run time with the rules of the OpenGL post-processes
// (vk_custom.cpp), so both backends give the same image. They read:
//   * the HDR image, as a copy with the rows bottom first (OpenGL order),
//   * the depth in the OpenGL convention and world normals rebuilt from the
//     depth (glcompat.frag), in the same order,
// and write their viewport back into the image, in the order of the OpenGL
// renderer. The custom post-processes of the viewports follow.
#include "vk_renderer_internal.h"
#include "../common/hrl_builtin_glsl.h"

namespace hrlvk
{
namespace
{
struct EffectStep
{
	float x = 0.f, y = 0.f, w = 1.f, h = 1.f; // viewport, origin top-left
	bool fullFrame = false;                     // custom post-process: uv over the whole image
	const CustomShader* shader = nullptr;
	VkPipeline pipeline = VK_NULL_HANDLE;
	const HRL_Material* material = nullptr;     // custom post-process
	EffectParams values;                        // built-in effect
	glm::vec2 screenSize{1.f};
};

void Put(EffectParams& p, const char* name, float v) { p[name] = {v}; }
void Put(EffectParams& p, const char* name, const glm::vec2& v) { p[name] = {v.x, v.y}; }
void Put(EffectParams& p, const char* name, const glm::vec3& v) { p[name] = {v.x, v.y, v.z}; }
void Put(EffectParams& p, const char* name, const glm::vec4& v) { p[name] = {v.x, v.y, v.z, v.w}; }
void Put(EffectParams& p, const char* name, const glm::mat4& m)
{
	std::vector<float>& f = p[name];
	f.assign(&m[0][0], &m[0][0] + 16);
}

bool HasVolumetricFog(const hrl_scene_t* scene)
{
	if (scene->global_volumetric_fog.enabled && scene->global_volumetric_fog.density > 0.f) return true;
	for (const auto& [id, fog] : scene->volumetric_fogs)
		if (fog && fog->enabled && fog->density > 0.f && fog->radius > 0.f) return true;
	return false;
}

// The first directional light is the sun of the clouds (as on OpenGL).
void CloudSun(const hrl_scene_t* scene, glm::vec3& direction, glm::vec3& color, float& intensity)
{
	direction = glm::normalize(glm::vec3(0.35f, -0.85f, 0.2f));
	color = scene->volumetric_cloud_light_color;
	intensity = scene->volumetric_cloud_light_intensity;
	for (const auto& [id, light] : scene->lights)
	{
		if (!light || light->type_ != HRL_DIRECTIONAL_LIGHT || light->intensity_ <= 0.f) continue;
		direction = GetLightDirection(light);
		color = light->color_;
		intensity *= std::max(0.f, light->intensity_);
		break;
	}
}

void AddEffect(std::vector<EffectStep>& steps, const EffectView& v, const char* name, const char* source,
	const EffectParams& values, glm::vec2 screen)
{
	const CustomShader* shader = EffectShader(name, source);
	if (!shader)
	{
		static bool reported = false;
		if (!RuntimeShadersAvailable() && !reported)
		{
			reported = true;
			SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING,
				"Vulkan: screen space effects need HRL built with glslang (HRL_VULKAN_RUNTIME_SHADERS)");
		}
		return;
	}
	// Effect ids are private: find the pipeline through the shader itself.
	for (auto& [id, s] : R.customShaders)
		if (&s == shader)
		{
			EffectStep step;
			step.x = v.x; step.y = v.y; step.w = v.w; step.h = v.h;
			step.shader = shader;
			step.pipeline = CustomPostPipeline(id);
			step.values = values;
			step.screenSize = screen;
			if (step.pipeline) steps.push_back(std::move(step));
			return;
		}
}
} // namespace

FrameGraph::Handle AddScreenEffects(FrameGraph& graph, SceneGPU& gpu, const HRL_SceneFrame& frame,
	const std::vector<EffectView>& views, FrameGraph::Handle color, FrameGraph::Handle depth)
{
	Device& dev = Dev();
	const hrl_scene_t* scene = frame.scene;
	const float W = (float)gpu.width, H = (float)gpu.height;
	const glm::vec2 screen(W, H);
	std::vector<EffectStep> steps;
	for (const EffectView& v : views)
	{
		// Uniforms shared by the OpenGL effect shaders. OpenGL viewports start at the bottom.
		EffectParams common;
		const glm::mat4 viewProj = v.projGL * v.viewGL;
		Put(common, "uViewProjection", viewProj);
		Put(common, "uInvViewProjection", glm::inverse(viewProj));
		Put(common, "uCameraPos", v.camera);
		Put(common, "uViewportOrigin", glm::vec2(v.x / W, (H - v.y - v.h) / H));
		Put(common, "uViewportSize", glm::vec2(v.w / W, v.h / H));
		Put(common, "uScreenSize", screen);

		// Decals, by id.
		for (const auto& [id, decal] : scene->decals)
		{
			if (!decal || !decal->enabled) continue;
			auto texture = R.textures.find(decal->texture);
			if (texture == R.textures.end()) continue;
			EffectParams p = common;
			glm::mat4 model = glm::translate(glm::mat4(1.f), decal->position);
			model = glm::rotate(model, glm::radians(decal->rotation.x), glm::vec3(1, 0, 0));
			model = glm::rotate(model, glm::radians(decal->rotation.y), glm::vec3(0, 1, 0));
			model = glm::rotate(model, glm::radians(decal->rotation.z), glm::vec3(0, 0, 1));
			model = glm::scale(model, decal->size);
			Put(p, "uDecalInvModel", glm::inverse(model));
			Put(p, "uDecalNormal", glm::normalize(glm::vec3(model * glm::vec4(0, 1, 0, 0))));
			Put(p, "uColor", glm::vec4(decal->color, 1.f));
			Put(p, "uOpacity", decal->opacity);
			Put(p, "uNormalFadeMin", decal->normal_fade_min);
			Put(p, "uNormalFadeMax", decal->normal_fade_max);
			p["uDecal"] = {(float)texture->second.slot};
			AddEffect(steps, v, "decal", kDecalFragmentShader, p, screen);
		}
		if (scene->screen_space_reflections_enabled)
		{
			EffectParams p = common;
			Put(p, "uStrength", scene->screen_space_reflections_strength);
			Put(p, "uMaxDistance", scene->screen_space_reflections_max_distance);
			Put(p, "uThickness", scene->screen_space_reflections_thickness);
			Put(p, "uFadeStart", scene->screen_space_reflections_fade_start);
			Put(p, "uFadeEnd", scene->screen_space_reflections_fade_end);
			Put(p, "uSteps", (float)scene->screen_space_reflections_steps);
			AddEffect(steps, v, "ssr", kSSRFragmentShader, p, screen);
		}
		if (scene->ambient_occlusion_enabled)
		{
			EffectParams p = common;
			Put(p, "uRadius", scene->ambient_occlusion_radius);
			Put(p, "uBias", scene->ambient_occlusion_bias);
			Put(p, "uStrength", scene->ambient_occlusion_strength);
			Put(p, "uPower", scene->ambient_occlusion_power);
			AddEffect(steps, v, "ambient occlusion", kAmbientOcclusionFragmentShader, p, screen);
		}
		glm::vec3 sunDirection, sunColor;
		float sunIntensity = 0.f;
		CloudSun(scene, sunDirection, sunColor, sunIntensity);
		if (HasVolumetricFog(scene) || scene->god_rays.enabled)
		{
			EffectParams p = common;
			const auto& global = scene->global_volumetric_fog;
			Put(p, "uGlobalVolumetricFogEnabled", global.enabled ? 1.f : 0.f);
			Put(p, "uGlobalFogColor", global.color);
			Put(p, "uGlobalFogDensity", global.density);
			Put(p, "uGlobalFogSteps", (float)global.steps);
			std::vector<float>& positions = p["uFogPositions"];
			std::vector<float>& radii = p["uFogRadii"];
			std::vector<float>& colors = p["uFogColors"];
			std::vector<float>& densities = p["uFogDensities"];
			uint32_t count = 0, steps_ = 4;
			for (const auto& [id, fog] : scene->volumetric_fogs)
			{
				if (!fog || !fog->enabled || fog->density <= 0.f || fog->radius <= 0.f) continue;
				if (count >= 64) break;
				positions.insert(positions.end(), {fog->position.x, fog->position.y, fog->position.z});
				radii.push_back(fog->radius);
				colors.insert(colors.end(), {fog->color.x, fog->color.y, fog->color.z});
				densities.push_back(fog->density);
				steps_ = std::max(steps_, (uint32_t)fog->steps);
				++count;
			}
			Put(p, "uVolumetricFogCount", (float)count);
			Put(p, "uVolumetricFogSteps", (float)steps_);
			Put(p, "uVolumetricCloudEnabled", 0.f); // clouds: their own pass below
			const auto& rays = scene->god_rays;
			glm::vec2 lightUV(0.5f);
			const glm::vec4 lightClip = viewProj * glm::vec4(rays.position, 1.f);
			if (std::abs(lightClip.w) > 1e-5f) lightUV = glm::vec2(lightClip) / lightClip.w * 0.5f + 0.5f;
			Put(p, "uGodRaysEnabled", rays.enabled ? 1.f : 0.f);
			Put(p, "uGodRaysLightUV", lightUV);
			Put(p, "uGodRaysColor", rays.color);
			Put(p, "uGodRaysDensity", rays.density);
			Put(p, "uGodRaysDecay", rays.decay);
			Put(p, "uGodRaysWeight", rays.weight);
			Put(p, "uGodRaysSamples", (float)rays.samples);
			AddEffect(steps, v, "scene effects", kSceneEffectsFragmentShader, p, screen);
		}
		if (scene->volumetric_cloud_enabled)
		{
			EffectParams p = common;
			Put(p, "uEnabled", 1.f);
			Put(p, "uCoverage", scene->volumetric_cloud_coverage);
			Put(p, "uDensity", scene->volumetric_cloud_density);
			Put(p, "uHeightMin", scene->volumetric_cloud_height_min);
			Put(p, "uHeightMax", scene->volumetric_cloud_height_max);
			Put(p, "uScale", scene->volumetric_cloud_scale);
			Put(p, "uDetail", scene->volumetric_cloud_detail);
			Put(p, "uWind", scene->volumetric_cloud_wind);
			Put(p, "uWindSpeed", scene->volumetric_cloud_wind_speed);
			Put(p, "uCloudColor", scene->volumetric_cloud_color);
			Put(p, "uLightAbsorption", scene->volumetric_cloud_light_absorption);
			Put(p, "uSteps", (float)scene->volumetric_cloud_steps);
			Put(p, "uMaxDistance", scene->volumetric_cloud_max_distance);
			Put(p, "uSunDirection", sunDirection);
			Put(p, "uLightColor", sunColor);
			Put(p, "uLightIntensity", sunIntensity);
			Put(p, "uTime", HRL_InternalRenderTimeSeconds());
			AddEffect(steps, v, "volumetric clouds", kVolumetricCloudFragmentShader, p, screen);
		}

		// Custom post-processes of the viewport, by priority (full frame, as on OpenGL).
		std::vector<const HRL_PostProcess*> posts;
		for (const auto& [pid, pp] : v.viewport->post_processes)
			if (pp) posts.push_back(pp);
		std::sort(posts.begin(), posts.end(), [](const HRL_PostProcess* a, const HRL_PostProcess* b)
		{
			return a->priority_ != b->priority_ ? a->priority_ < b->priority_ : a->id_ < b->id_;
		});
		// Values every custom post-process can read by declaring the uniform
		// (camera, sun, planes): see HRL_CreatePostProcess in hrl.h.
		EffectParams autoValues = common;
		Put(autoValues, "uView", v.viewGL);
		Put(autoValues, "uProjection", v.projGL);
		Put(autoValues, "uInvProjection", glm::inverse(v.projGL));
		Put(autoValues, "uInvView", glm::inverse(v.viewGL));
		{
			const glm::mat4 invView = glm::inverse(v.viewGL);
			Put(autoValues, "uCameraForward", glm::normalize(-glm::vec3(invView[2])));
			const HRL_Camera* cam = v.viewport ? v.viewport->camera_ : nullptr;
			Put(autoValues, "uNearFar", glm::vec2(cam ? cam->near_plane_ : 0.1f, cam ? cam->far_plane_ : 1000.f));
			// First directional light: direction the light travels, colour * intensity,
			// and where the sun is on screen (uSunScreen.z: 1 when it is in front of the camera).
			glm::vec3 dir(0.f, -1.f, 0.f), col(0.f);
			for (const auto& [lid, light] : scene->lights)
			{
				if (!light || light->type_ != HRL_DIRECTIONAL_LIGHT) continue;
				dir = GetLightDirection(light);
				col = light->color_ * std::max(0.f, light->intensity_);
				break;
			}
			Put(autoValues, "uSunDirection", dir);
			Put(autoValues, "uSunColor", col);
			const glm::vec4 sunClip = viewProj * glm::vec4(v.camera - dir * 1000.f, 1.f);
			glm::vec3 sunScreen(0.5f, 0.5f, 0.f);
			if (sunClip.w > 1e-5f) sunScreen = glm::vec3(glm::vec2(sunClip) / sunClip.w * 0.5f + 0.5f, 1.f);
			Put(autoValues, "uSunScreen", sunScreen);
		}
		for (const HRL_PostProcess* pp : posts)
		{
			auto m = frame.materials->find(pp->material_);
			if (m == frame.materials->end() || !m->second) continue;
			const VkPipeline pipeline = CustomPostPipeline(m->second->shader_);
			if (!pipeline) continue;
			EffectStep step;
			step.x = v.x; step.y = v.y; step.w = v.w; step.h = v.h;
			step.fullFrame = true;
			step.shader = FindCustomShader(m->second->shader_);
			step.pipeline = pipeline;
			step.material = m->second;
			step.values = autoValues;
			step.screenSize = glm::vec2(v.w, v.h);
			steps.push_back(std::move(step));
		}
	}
	if (steps.empty()) return color;

	ImageDesc desc;
	desc.format = kHdrFormat;
	desc.width = gpu.width;
	desc.height = gpu.height;
	desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	const FrameGraph::Handle work = graph.CreateTransient(desc, "effects image");
	const FrameGraph::Handle flipped = graph.CreateTransient(desc, "effects source");
	desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	const FrameGraph::Handle normals = graph.CreateTransient(desc, "effects normals");
	desc.format = VK_FORMAT_R32_SFLOAT;
	const FrameGraph::Handle depthGL = graph.CreateTransient(desc, "effects depth");
	if (!graph.GetImage(work).handle || !graph.GetImage(flipped).handle || !graph.GetImage(normals).handle || !graph.GetImage(depthGL).handle)
		return color;
	const uint32_t w = gpu.width, h = gpu.height;
	Image* source = &graph.GetImage(color);
	Image* target = &graph.GetImage(work);
	Image* flip = &graph.GetImage(flipped);
	graph.AddPass("effects copy", [=](VkCommandBuffer c)
	{
		VkImageCopy copy{};
		copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.extent = {w, h, 1};
		vkCmdCopyImage(c, source->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	}).Use(color, Access::CopySource).Use(work, Access::CopyDestination);

	// Depth and normals in the OpenGL conventions, per viewport.
	struct CompatView { VkDescriptorSet set; VkRect2D scissor; };
	std::vector<CompatView> compat;
	for (const EffectView& v : views)
	{
		struct { glm::mat4 invViewProj, viewProjGL; glm::vec4 rect; } data{
			glm::inverse(v.viewProj), v.projGL * v.viewGL, glm::vec4(v.x, v.y, v.w, v.h)};
		const FrameSlice slice = dev.AllocFrame(sizeof(data), 256);
		const VkDescriptorSet set = slice.ptr ? AllocFrameSet(R.glCompatSetLayout) : VK_NULL_HANDLE;
		if (!set) continue;
		std::memcpy(slice.ptr, &data, sizeof(data));
		const VkDescriptorImageInfo image{R.pointClamp, gpu.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		const VkDescriptorBufferInfo buffer{slice.buffer, slice.offset, sizeof(data)};
		VkWriteDescriptorSet writes[2]{};
		for (uint32_t i = 0; i < 2; ++i)
		{
			writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[i].dstSet = set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
		}
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[0].pImageInfo = &image;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[1].pBufferInfo = &buffer;
		vkUpdateDescriptorSets(dev.device, 2, writes, 0, nullptr);
		// The flipped image holds the viewport at the mirrored rows.
		compat.push_back({set, {{(int32_t)v.x, (int32_t)(H - v.y - v.h)}, {(uint32_t)v.w, (uint32_t)v.h}}});
	}
	VkClearColorValue farDepth{};
	farDepth.float32[0] = 1.f;
	graph.AddPass("effects depth and normals", [=](VkCommandBuffer c)
	{
		const VkViewport vp{0.f, 0.f, (float)w, (float)h, 0.f, 1.f};
		vkCmdSetViewport(c, 0, 1, &vp);
		vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.glCompatPipeline);
		for (const CompatView& cv : compat)
		{
			vkCmdSetScissor(c, 0, 1, &cv.scissor);
			vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.glCompatLayout, 0, 1, &cv.set, 0, nullptr);
			vkCmdDraw(c, 3, 1, 0, 0);
		}
	}).Use(depth, Access::Sampled).Color(depthGL, VK_ATTACHMENT_LOAD_OP_CLEAR, farDepth).Color(normals, VK_ATTACHMENT_LOAD_OP_CLEAR);

	const VkImageView depthView = graph.GetImage(depthGL).view, normalView = graph.GetImage(normals).view;
	const float seconds = HRL_InternalRenderTimeSeconds();
	for (const EffectStep& step : steps)
	{
		const size_t count = std::max<size_t>(step.shader->paramSlots, 1u);
		const FrameSlice params = dev.AllocFrame(count * sizeof(glm::vec4), 256);
		const VkDescriptorSet set = params.ptr ? AllocFrameSet(R.postSetLayout) : VK_NULL_HANDLE;
		if (!set) continue;
		auto* p = static_cast<glm::vec4*>(params.ptr);
		if (step.material)
		{
			WriteCustomParams(*step.shader, step.material, p);
			WriteAutoParams(*step.shader, step.values, step.material, p);
		}
		else
		{
			WriteEffectParams(*step.shader, step.values, p);
			// Sampler parameters of the effects (decal texture) hold a bindless slot.
			for (const CustomParam& cp : step.shader->params)
				if (cp.kind == CustomParam::Sampler)
				{
					auto it = step.values.find(cp.name);
					const uint32_t slot = it != step.values.end() && !it->second.empty() ? (uint32_t)it->second[0] : R.white.slot;
					std::memcpy(&p[cp.first].x, &slot, sizeof(slot));
				}
		}
		p[0] = glm::vec4(step.screenSize, seconds, 0.f);
		const VkDescriptorImageInfo images[4] = {
			{R.linearClamp, flip->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{R.linearClamp, flip->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{R.pointClamp, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{R.pointClamp, normalView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
		const VkDescriptorBufferInfo buffer{params.buffer, params.offset, params.size};
		VkWriteDescriptorSet writes[5]{};
		for (uint32_t i = 0; i < 5; ++i)
		{
			writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			writes[i].dstSet = set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			if (i == 3)
			{
				writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				writes[i].pBufferInfo = &buffer;
			}
			else
			{
				writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				writes[i].pImageInfo = &images[i < 3 ? i : 3];
			}
		}
		vkUpdateDescriptorSets(dev.device, 5, writes, 0, nullptr);
		graph.AddPass("effects flip", [=](VkCommandBuffer c)
		{
			VkImageBlit blit{};
			blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			blit.srcOffsets[1] = {(int32_t)w, (int32_t)h, 1};
			blit.dstOffsets[0] = {0, (int32_t)h, 0};
			blit.dstOffsets[1] = {(int32_t)w, 0, 1};
			vkCmdBlitImage(c, target->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, flip->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
		}).Use(work, Access::CopySource).Use(flipped, Access::CopyDestination);
		const VkPipeline pipeline = step.pipeline;
		const VkRect2D scissor{{(int32_t)step.x, (int32_t)step.y}, {(uint32_t)step.w, (uint32_t)step.h}};
		const VkViewport vp = step.fullFrame ? VkViewport{0.f, 0.f, (float)w, (float)h, 0.f, 1.f}
			: VkViewport{step.x, step.y, step.w, step.h, 0.f, 1.f};
		graph.AddPass("screen effect", [=](VkCommandBuffer c)
		{
			vkCmdSetViewport(c, 0, 1, &vp);
			vkCmdSetScissor(c, 0, 1, &scissor);
			vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			const VkDescriptorSet sets[2] = {set, R.bindlessSet};
			vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, R.postLayout, 0, 2, sets, 0, nullptr);
			vkCmdDraw(c, 3, 1, 0, 0);
		}).Use(flipped, Access::Sampled).Use(depthGL, Access::Sampled).Use(normals, Access::Sampled)
		  .Color(work, VK_ATTACHMENT_LOAD_OP_LOAD);
	}
	return work;
}
} // namespace hrlvk
