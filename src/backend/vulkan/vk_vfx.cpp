// HRL native Vulkan backend - particle systems (VFX). The simulation is done
// by the frontend (SimulateVFXSystem); this draws the particles of a camera
// with the rules of the OpenGL backend:
//   * billboards / stretched billboards: unlit tinted quads, texture of the
//     emitter (or the first texture of its material);
//   * mesh particles: the template mesh with its material (or the material of
//     the emitter) and the particle colour as tint, lit like the meshes;
//   * blending per emitter (alpha: sorted back to front, additive, multiply),
//     depth tested against the scene, no depth written.
#include "vk_renderer_internal.h"

namespace hrlvk
{
namespace
{
struct ParticleGPU { glm::mat4 model; glm::vec4 color; }; // vfx.vert

uint32_t BlendIndex(HRL_EVFXBlendMode mode)
{
	return mode == HRL_VFX_BLEND_ADDITIVE ? 1u : (mode == HRL_VFX_BLEND_MULTIPLY ? 2u : 0u);
}

uint32_t BillboardTexture(const HRL_VFXEmitter* emitter, const HRL_SceneFrame& frame)
{
	HRL_id texture = emitter->texture_;
	if (texture == HRL_INVALID_ID && emitter->material_ != HRL_INVALID_ID)
	{
		auto m = frame.materials->find(emitter->material_);
		if (m != frame.materials->end() && m->second && !m->second->textureParams_.empty())
			texture = m->second->textureParams_.begin()->second;
	}
	auto it = R.textures.find(texture);
	return it != R.textures.end() ? it->second.slot : 0xFFFFFFFFu;
}

// Template mesh and material of a mesh emitter (null when unusable).
const HRL_Mesh* TemplateMesh(const hrl_scene_t* scene, const HRL_VFXEmitter* emitter, const HRL_SceneFrame& frame, HRL_id& material)
{
	auto mesh = scene->meshes.find(emitter->mesh_);
	if (mesh == scene->meshes.end() || !mesh->second || mesh->second->type_ != HRL_3D_MESH) return nullptr;
	auto gpu = R.meshes.find(emitter->mesh_);
	if (gpu == R.meshes.end() || gpu->second.lods.empty() || !gpu->second.lods[0].valid) return nullptr;
	material = HRL_INVALID_ID;
	auto usable = [&](HRL_id id)
	{
		auto m = frame.materials->find(id);
		return m != frame.materials->end() && m->second && m->second->shader_ == HRL_MESH_3D_SHADER;
	};
	if (emitter->material_ != HRL_INVALID_ID && usable(emitter->material_)) material = emitter->material_;
	else if (usable(mesh->second->material_)) material = mesh->second->material_;
	return material != HRL_INVALID_ID ? mesh->second : nullptr;
}

template <typename F> void ForEachEmitter(const hrl_scene_t* scene, F f)
{
	for (const auto& [sid, system] : scene->vfx_systems)
	{
		if (!system || !system->enabled_) continue;
		for (const auto& [eid, emitter] : system->emitters_)
			if (emitter && emitter->enabled_ && !emitter->particles_.empty()) f(system, emitter);
	}
}
} // namespace

void ResolveVFXMaterials(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame)
{
	ForEachEmitter(scene, [&](const HRL_VFXSystem*, const HRL_VFXEmitter* emitter)
	{
		if (emitter->render_mode_ != HRL_VFX_RENDER_MESH) return;
		HRL_id material = HRL_INVALID_ID;
		if (TemplateMesh(scene, emitter, frame, material)) MaterialIndex(data, frame, material);
	});
}

VFXFrame PrepareVFX(const hrl_scene_t* scene, SceneData& data, const HRL_SceneFrame& frame, const Camera& cam)
{
	VFXFrame out;
	if (scene->vfx_systems.empty()) return out;
	const glm::mat4 billboardBasis = glm::mat4(glm::mat3(glm::inverse(cam.view)));
	std::vector<ParticleGPU> particles;
	std::vector<InstanceData> meshInstances;
	std::vector<std::pair<float, uint32_t>> order;
	ForEachEmitter(scene, [&](const HRL_VFXSystem* system, const HRL_VFXEmitter* emitter)
	{
		const glm::mat4 systemMatrix = MakeVFXSystemMatrix(system);
		const bool local = emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL;
		const glm::mat4* particleSystem = (local && systemMatrix != glm::mat4(1.f)) ? &systemMatrix : nullptr;
		const bool sorted = emitter->blend_mode_ == HRL_VFX_BLEND_ALPHA;
		// Order of the particles: back to front when alpha blended.
		order.clear();
		for (uint32_t i = 0; i < (uint32_t)emitter->particles_.size(); ++i)
		{
			glm::vec3 p = emitter->particles_[i].position;
			if (local) p = glm::vec3(systemMatrix * glm::vec4(p, 1.f));
			order.emplace_back(glm::dot(p - cam.position, p - cam.position), i);
		}
		if (sorted)
			std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

		if (emitter->render_mode_ != HRL_VFX_RENDER_MESH)
		{
			const bool stretched = emitter->render_mode_ == HRL_VFX_RENDER_STRETCHED_BILLBOARD;
			VFXFrame::Batch batch;
			batch.first = (uint32_t)particles.size();
			batch.blend = BlendIndex(emitter->blend_mode_);
			batch.texture = BillboardTexture(emitter, frame);
			for (const auto& [distance, index] : order)
			{
				const HRL_VFXParticle& p = emitter->particles_[index];
				particles.push_back({MakeVFXParticleModel(p, particleSystem, billboardBasis, stretched, emitter->stretch_), p.color});
			}
			batch.count = (uint32_t)particles.size() - batch.first;
			if (batch.count) out.billboards.push_back(batch);
			return;
		}

		HRL_id materialId = HRL_INVALID_ID;
		const HRL_Mesh* mesh = TemplateMesh(scene, emitter, frame, materialId);
		if (!mesh) return;
		const MeshLOD& lod = R.meshes[emitter->mesh_].lods[0];
		const uint32_t material = MaterialIndex(data, frame, materialId);
		VFXFrame::MeshBatch batch;
		batch.first = (uint32_t)meshInstances.size();
		batch.blend = BlendIndex(emitter->blend_mode_);
		batch.indexCount = lod.indexCount;
		batch.firstIndex = lod.firstIndex;
		batch.vertexOffset = (int32_t)lod.vertexOffset;
		for (const auto& [distance, index] : order)
		{
			const HRL_VFXParticle& p = emitter->particles_[index];
			glm::vec3 position = p.position;
			if (local) position = glm::vec3(systemMatrix * glm::vec4(position, 1.f));
			glm::mat4 model = glm::translate(glm::mat4(1.f), position);
			model = glm::translate(model, mesh->pivot_point_);
			model = glm::rotate(model, glm::radians(p.rotation.x + emitter->mesh_rotation_offset_.x), glm::vec3(1.f, 0.f, 0.f));
			model = glm::rotate(model, glm::radians(p.rotation.y + emitter->mesh_rotation_offset_.y), glm::vec3(0.f, 1.f, 0.f));
			model = glm::rotate(model, glm::radians(p.rotation.z + emitter->mesh_rotation_offset_.z), glm::vec3(0.f, 0.f, 1.f));
			model = glm::translate(model, -mesh->pivot_point_);
			model = glm::scale(model, emitter->mesh_scale_ * std::max(0.f, p.size.x));
			InstanceData inst;
			inst.model = model;
			const glm::mat3 m3(model);
			const float det = glm::determinant(m3);
			inst.normalMatrix = (std::isfinite(det) && std::abs(det) > 1e-12f) ? glm::mat4(glm::transpose(glm::inverse(m3))) : glm::mat4(1.f);
			const glm::vec4 c = glm::clamp(p.color, 0.f, 1.f);
			const uint32_t tint = (uint32_t)(c.r * 255.f + 0.5f) | ((uint32_t)(c.g * 255.f + 0.5f) << 8) |
				((uint32_t)(c.b * 255.f + 0.5f) << 16) | ((uint32_t)(c.a * 255.f + 0.5f) << 24);
			inst.ids = glm::uvec4(material, emitter->id_, 2u /* INSTANCE_TINT */, tint);
			meshInstances.push_back(inst);
		}
		batch.count = (uint32_t)meshInstances.size() - batch.first;
		if (batch.count) out.meshes.push_back(batch);
	});

	Device& dev = Dev();
	if (!particles.empty())
	{
		const FrameSlice slice = dev.AllocFrame(particles.size() * sizeof(ParticleGPU), 256);
		out.particleSet = slice.ptr ? AllocFrameSet(R.vfxSetLayout) : VK_NULL_HANDLE;
		if (out.particleSet)
		{
			std::memcpy(slice.ptr, particles.data(), particles.size() * sizeof(ParticleGPU));
			const VkDescriptorBufferInfo info{slice.buffer, slice.offset, slice.size};
			VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			write.dstSet = out.particleSet;
			write.dstBinding = 0;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			write.pBufferInfo = &info;
			vkUpdateDescriptorSets(dev.device, 1, &write, 0, nullptr);
		}
		else out.billboards.clear();
	}
	if (!meshInstances.empty())
	{
		out.meshInstances = dev.AllocFrame(meshInstances.size() * sizeof(InstanceData), 256);
		if (out.meshInstances.ptr) std::memcpy(out.meshInstances.ptr, meshInstances.data(), meshInstances.size() * sizeof(InstanceData));
		else out.meshes.clear();
	}
	return out;
}

void DrawVFX(VkCommandBuffer cmd, const VFXFrame& f, VkDescriptorSet viewSet)
{
	if (!f.billboards.empty() && f.particleSet)
	{
		const VkDescriptorSet sets[3] = {viewSet, R.bindlessSet, f.particleSet};
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.vfxLayout, 0, 3, sets, 0, nullptr);
		uint32_t bound = ~0u;
		for (const VFXFrame::Batch& b : f.billboards)
		{
			if (b.blend != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.vfxPipelines[bound = b.blend]);
			vkCmdPushConstants(cmd, R.vfxLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t), &b.texture);
			vkCmdDraw(cmd, 6, b.count, 0, b.first);
		}
	}
	if (!f.meshes.empty() && f.meshViewSet)
	{
		const VkDescriptorSet sets[2] = {f.meshViewSet, R.bindlessSet};
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.sceneLayout, 0, 2, sets, 0, nullptr);
		VkDeviceSize zero = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &R.vertexBuffer.handle, &zero);
		vkCmdBindIndexBuffer(cmd, R.indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);
		uint32_t bound = ~0u;
		for (const VFXFrame::MeshBatch& b : f.meshes)
		{
			if (b.blend != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.vfxMeshPipelines[bound = b.blend]);
			vkCmdDrawIndexed(cmd, b.indexCount, b.count, b.firstIndex, b.vertexOffset, b.first);
		}
	}
}
} // namespace hrlvk
