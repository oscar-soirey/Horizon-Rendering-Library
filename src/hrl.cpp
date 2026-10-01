/**
 * Contient l'implémentation des fichiers :
 * hrl.h, hrl_gl.h, hrl_vulkan.h, hrl_d3d.h
 */

#include "hrl.h"
#include "hrl_gl.h"
#include "hrl_vulkan.h"

#include "core/backend_vtable.h"
#include "core/object_types.h"
#include "core/utils_functions.h"
#include "core/widgets.h"

#include "backend/opengl33/gl33_backend.h"
#include "backend/vulkan/vulkan_backend.h"
#include <vulkan/vulkan.h>

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>
#include <array>
#include <vector>
#include <cmath>
#include <new>
#include <cstdio>
#include <cstdarg>
#include <limits>
#include <set>
#include <tuple>
#include <cstring>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

// Internal FBX decoding dependency. Add ufbx to the build; HRL only consumes
// its public header here and converts the decoded data to HRL_Vertex3D.
#include <ufbx/ufbx.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

//pour le texte
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>
#include <stb/stb_image.h>


static HRL_Context ctx_;

namespace
{
struct AsyncResourceResult
{
    enum class Type { Texture, Shader };
    Type type = Type::Texture;
    HRL_id id = HRL_INVALID_ID;
    BitmapResult bitmap{};
    std::string vert;
    std::string frag;
    std::string error;
    bool success = false;
};

struct AsyncResourceTask
{
    AsyncResourceResult::Type type = AsyncResourceResult::Type::Texture;
    HRL_id id = HRL_INVALID_ID;
    std::vector<unsigned char> bytes;
    std::string vert;
    std::string frag;
};

class AsyncResourceLoader
{
public:
    void Start()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!workers_.empty()) return;
        stop_ = false;
        unsigned int hw = std::thread::hardware_concurrency();
        unsigned int count = hw > 1 ? hw - 1 : 1;
        count = std::max(1u, std::min(count, 4u));
        workers_.reserve(count);
        for (unsigned int i = 0; i < count; ++i)
            workers_.emplace_back([this]{ WorkerLoop(); });
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_)
            if (worker.joinable()) worker.join();
        workers_.clear();
    }

    void Enqueue(AsyncResourceTask task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(task));
        }
        condition_.notify_one();
    }

    bool TryPopResult(AsyncResourceResult& out)
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        if (results_.empty()) return false;
        out = std::move(results_.front());
        results_.pop();
        return true;
    }

    bool HasPendingWork() const
    {
        bool pending = false;
        { std::lock_guard<std::mutex> lock(mutex_); pending = !queue_.empty() || active_workers_ != 0; }
        if (pending) return true;
        { std::lock_guard<std::mutex> lock(result_mutex_); return !results_.empty(); }
    }

private:
    static bool DecodeTexture(const std::vector<unsigned char>& bytes, BitmapResult& bmp, std::string& error)
    {
        if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        { error = "Async texture load: invalid image buffer"; return false; }
        int w = 0, h = 0, channels = 0;
        stbi_uc* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, STBI_rgb_alpha);
        if (!data || w <= 0 || h <= 0)
        { error = "Async texture load: image decode failed"; stbi_image_free(data); return false; }
        bmp.width = w;
        bmp.height = h;
        bmp.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
        std::memcpy(bmp.pixels.data(), data, bmp.pixels.size());
        stbi_image_free(data);
        return true;
    }

    void WorkerLoop()
    {
        for (;;)
        {
            AsyncResourceTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]{ return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
                ++active_workers_;
            }

            AsyncResourceResult result;
            result.type = task.type;
            result.id = task.id;
            if (task.type == AsyncResourceResult::Type::Texture)
            {
                result.success = DecodeTexture(task.bytes, result.bitmap, result.error);
            }
            else
            {
                result.vert = std::move(task.vert);
                result.frag = std::move(task.frag);
                result.success = !result.vert.empty() && !result.frag.empty();
                if (!result.success) result.error = "Async shader load: empty vertex or fragment source";
            }

            {
                std::lock_guard<std::mutex> lock(result_mutex_);
                results_.push(std::move(result));
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --active_workers_;
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<AsyncResourceTask> queue_;
    std::vector<std::thread> workers_;
    unsigned int active_workers_ = 0;
    bool stop_ = false;
    mutable std::mutex result_mutex_;
    std::queue<AsyncResourceResult> results_;
};

AsyncResourceLoader g_AsyncResourceLoader;
enum class AsyncResourceState : int { Pending = 0, Ready = 1, Failed = 2, Cancelled = 3 };
static void ProcessGizmoInput();
}

//vtable utilisée pour appeller les fonctions, ne doit jamais etre modifiée apres Init()
static HRL_vtable g_Backend;

/**
 * Owns the ufbx scene kept alive for the duration of the public FBX resource API.
 * HRL_FBXTextureInfo/HRL_FBXMaterialInfo intentionally expose borrowed strings/data
 * from this scene, so callers must keep this object alive while using those pointers.
 */
struct HRL_FBXResources
{
	ufbx_scene* scene = nullptr;
	std::vector<HRL_FBXTextureInfo> textures;
	std::vector<HRL_FBXMaterialInfo> materials;
	std::vector<const ufbx_material*> material_sources;
	std::unordered_map<const ufbx_texture*, HRL_uint> texture_indices;
};


HRL_Context* GetPrivateContext()
{
	return &ctx_;
}

void HRL_Vulkan_SetSurfaceCallback(HRL_VulkanCreateSurfaceCallback callback, void* user_data)
{
	VulkanSetSurfaceCallback(callback, user_data);
}

void HRL_Vulkan_SetInstanceExtensions(const char* const* extensions, HRL_uint extension_count)
{
	VulkanSetInstanceExtensions(extensions, extension_count);
}

void* HRL_Vulkan_GetInstance(void)
{
	return reinterpret_cast<void*>(VulkanGetInstance());
}

void* HRL_Vulkan_GetPhysicalDevice(void)
{
	return reinterpret_cast<void*>(VulkanGetPhysicalDevice());
}

void* HRL_Vulkan_GetDevice(void)
{
	return reinterpret_cast<void*>(VulkanGetDevice());
}

void* HRL_Vulkan_GetGraphicsQueue(void)
{
	return reinterpret_cast<void*>(VulkanGetGraphicsQueue());
}

uint64_t HRL_Vulkan_GetSurface(void)
{
#if defined(VK_USE_64_BIT_PTR_DEFINES) && VK_USE_64_BIT_PTR_DEFINES
	return reinterpret_cast<uint64_t>(VulkanGetSurface());
#else
	return static_cast<uint64_t>(VulkanGetSurface());
#endif
}



//Utils Non-API Functions :
std::vector<HRL_Mesh*> GetSortedSprites(hrl_scene_t* _scene)
{
	std::vector<HRL_Mesh*> sprites_;
	for (const auto& [id, mesh] : _scene->meshes)
	{
		if (mesh->type_ == HRL_SPRITE)
		{
			sprites_.push_back(mesh);
		}
	}

	std::stable_sort(sprites_.begin(), sprites_.end(), [](const HRL_Mesh* a, const HRL_Mesh* b)
	{
		if (a->position_.z == b->position_.z)
		{
			return a->draw_order_ < b->draw_order_;
		}
		return a->position_.z < b->position_.z;
	});

	return sprites_;
}

std::vector<HRL_Light*> GetLightsVector(HRL_id _scene)
{
	auto it = ctx_.scenes.find(_scene);
	if (it == ctx_.scenes.end())
	{
		return {};
	}

	std::vector<HRL_Light*> lvector;
	lvector.reserve(it->second->lights.size());

	for (const auto& [id, light] : it->second->lights)
	{
		lvector.push_back(light);
	}
	return lvector;
}

static void UpdateSceneLights(HRL_id _scene)
{
	if (ctx_.scenes.find(_scene) == ctx_.scenes.end())
		return;

	g_Backend.RHI_UpdateLights(GetLightsVector(_scene));
}

static void MarkSceneGIGeometryDirty(hrl_scene_t* scene)
{
	if (!scene)
		return;
	++scene->gi_geometry_revision;
	scene->shadows_dirty = true;
}

static void MarkSceneGILightingDirty(hrl_scene_t* scene)
{
	if (scene)
		++scene->gi_lighting_revision;
}

static void MarkAllScenesGILightingDirty()
{
	for (auto& [sceneId, scene] : ctx_.scenes)
	{
		(void)sceneId;
		MarkSceneGILightingDirty(scene);
	}
}


namespace
{
static float VFXRand01(uint32_t& state)
{
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return static_cast<float>(state & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

static float VFXRandRange(uint32_t& state, float a, float b)
{
	return a + (b - a) * VFXRand01(state);
}

static glm::vec3 VFXRandVec3(uint32_t& state, const glm::vec3& a, const glm::vec3& b)
{
	return glm::vec3(
		VFXRandRange(state, a.x, b.x),
		VFXRandRange(state, a.y, b.y),
		VFXRandRange(state, a.z, b.z));
}

static glm::mat4 VFXEulerMatrix(const glm::vec3& degrees)
{
	glm::mat4 m(1.f);
	m = glm::rotate(m, glm::radians(degrees.x), glm::vec3(1.f, 0.f, 0.f));
	m = glm::rotate(m, glm::radians(degrees.y), glm::vec3(0.f, 1.f, 0.f));
	m = glm::rotate(m, glm::radians(degrees.z), glm::vec3(0.f, 0.f, 1.f));
	return m;
}

static glm::mat4 VFXSystemMatrix(const HRL_VFXSystem* system)
{
	glm::mat4 m(1.f);
	m = glm::translate(m, system->position_);
	m *= VFXEulerMatrix(system->rotation_);
	m = glm::scale(m, system->scale_);
	return m;
}

static glm::mat4 VFXEmitterMatrix(const HRL_VFXEmitter* emitter)
{
	glm::mat4 m(1.f);
	m = glm::translate(m, emitter->position_);
	m *= VFXEulerMatrix(emitter->rotation_);
	return m;
}

static glm::vec3 VFXTransformDirection(const glm::mat4& m, const glm::vec3& v)
{
	const glm::vec3 r = glm::vec3(m * glm::vec4(v, 0.f));
	const float len = glm::length(r);
	return len > 1e-6f ? r / len : glm::vec3(0.f);
}

static glm::vec3 VFXSpawnOffset(HRL_VFXEmitter* emitter, uint32_t& rng)
{
	const float u = VFXRand01(rng);
	const float v = VFXRand01(rng);
	const float w = VFXRand01(rng);
	const float angle = glm::radians(glm::clamp(emitter->shape_angle_degrees_, 0.f, 89.f));

	switch (emitter->spawn_shape_)
	{
	case HRL_VFX_SHAPE_SPHERE:
	{
		glm::vec3 p;
		for (int i = 0; i < 8; ++i)
		{
			p = glm::vec3(VFXRandRange(rng, -1.f, 1.f), VFXRandRange(rng, -1.f, 1.f), VFXRandRange(rng, -1.f, 1.f));
			if (glm::dot(p,p) <= 1.f) return p * emitter->shape_radius_;
		}
		return glm::vec3(0.f);
	}
	case HRL_VFX_SHAPE_BOX:
		return glm::vec3(
			VFXRandRange(rng, -emitter->shape_size_.x * 0.5f, emitter->shape_size_.x * 0.5f),
			VFXRandRange(rng, -emitter->shape_size_.y * 0.5f, emitter->shape_size_.y * 0.5f),
			VFXRandRange(rng, -emitter->shape_size_.z * 0.5f, emitter->shape_size_.z * 0.5f));
	case HRL_VFX_SHAPE_CYLINDER:
	{
		const float a = 6.28318530718f * u;
		const float r = std::sqrt(v) * emitter->shape_radius_;
		return glm::vec3(std::cos(a) * r, (w - 0.5f) * emitter->shape_size_.y, std::sin(a) * r);
	}
	case HRL_VFX_SHAPE_CONE:
	{
		const float y = v * std::max(emitter->shape_size_.y, 1e-4f);
		const float maxR = std::tan(angle) * y;
		const float r = std::sqrt(u) * maxR;
		const float a = 6.28318530718f * w;
		return glm::vec3(std::cos(a) * r, y, std::sin(a) * r);
	}
	case HRL_VFX_SHAPE_POINT:
	default:
		return glm::vec3(0.f);
	}
}

static glm::vec4 EvalVFXColor(const HRL_VFXCurve* curve, float t)
{
	if (!curve || !curve->color_ || curve->color_keys_.empty()) return glm::vec4(1.f);
	if (t <= curve->color_keys_.front().time) return curve->color_keys_.front().value;
	if (t >= curve->color_keys_.back().time) return curve->color_keys_.back().value;
	for (size_t i = 1; i < curve->color_keys_.size(); ++i)
	{
		const auto& a = curve->color_keys_[i-1];
		const auto& b = curve->color_keys_[i];
		if (t <= b.time)
		{
			const float span = std::max(1e-6f, b.time - a.time);
			const float u = (t - a.time) / span;
			return glm::mix(a.value, b.value, glm::clamp(u, 0.f, 1.f));
		}
	}
	return curve->color_keys_.back().value;
}

static float EvalVFXFloat(const HRL_VFXCurve* curve, float t, float fallback)
{
	if (!curve || curve->color_ || curve->float_keys_.empty()) return fallback;
	if (t <= curve->float_keys_.front().time) return curve->float_keys_.front().value;
	if (t >= curve->float_keys_.back().time) return curve->float_keys_.back().value;
	for (size_t i = 1; i < curve->float_keys_.size(); ++i)
	{
		const auto& a = curve->float_keys_[i-1];
		const auto& b = curve->float_keys_[i];
		if (t <= b.time)
		{
			const float span = std::max(1e-6f, b.time - a.time);
			return glm::mix(a.value, b.value, glm::clamp((t - a.time) / span, 0.f, 1.f));
		}
	}
	return curve->float_keys_.back().value;
}

static HRL_VFXSystem* FindVFXSystem(HRL_id id)
{
	auto it = ctx_.vfx_systems.find(id);
	return it == ctx_.vfx_systems.end() ? nullptr : it->second;
}

static HRL_VFXEmitter* FindVFXEmitter(HRL_id id)
{
	auto it = ctx_.vfx_emitters.find(id);
	return it == ctx_.vfx_emitters.end() ? nullptr : it->second;
}

static HRL_VFXCurve* FindVFXCurve(HRL_id id)
{
	auto it = ctx_.vfx_curves.find(id);
	return it == ctx_.vfx_curves.end() ? nullptr : it->second;
}

static void SpawnVFXParticle(HRL_VFXEmitter* emitter)
{
	if (!emitter || emitter->particles_.size() >= emitter->max_particles_ || emitter->system_ == HRL_INVALID_ID)
		return;

	auto* system = FindVFXSystem(emitter->system_);
	if (!system) return;

	uint32_t rng = emitter->random_seed_ + static_cast<uint32_t>(++emitter->spawned_total_) * 747796405u;
	HRL_VFXParticle p;
	p.seed = rng;
	p.lifetime = std::max(0.001f, VFXRandRange(rng, emitter->min_lifetime_, emitter->max_lifetime_));

	const glm::mat4 em = VFXEmitterMatrix(emitter);
	const glm::vec3 localOffset = VFXSpawnOffset(emitter, rng);
	p.position = glm::vec3(em * glm::vec4(localOffset, 1.f));
	p.velocity = VFXRandVec3(rng, emitter->min_initial_velocity_, emitter->max_initial_velocity_);
	p.velocity = VFXTransformDirection(em, p.velocity) * glm::length(p.velocity);
	if (emitter->min_initial_speed_ != 0.f || emitter->max_initial_speed_ != 0.f)
	{
		const float speed = VFXRandRange(rng, emitter->min_initial_speed_, emitter->max_initial_speed_);
		const glm::vec3 baseDir = glm::length(p.velocity) > 1e-5f ? glm::normalize(p.velocity) : glm::vec3(0.f, 1.f, 0.f);
		p.velocity = baseDir * speed;
	}
	p.base_rotation = VFXRandVec3(rng, emitter->min_initial_rotation_, emitter->max_initial_rotation_);
	p.rotation = p.base_rotation;
	p.angular_velocity = VFXRandVec3(rng, emitter->min_angular_velocity_, emitter->max_angular_velocity_);
	p.size = emitter->particle_size_;
	p.color = glm::vec4(1.f);

	if (emitter->simulation_space_ == HRL_VFX_SIMULATION_WORLD)
	{
		p.position = glm::vec3(VFXSystemMatrix(system) * glm::vec4(p.position, 1.f));
		p.velocity = VFXTransformDirection(VFXSystemMatrix(system), p.velocity) * glm::length(p.velocity);
	}

	emitter->particles_.push_back(p);
}

static void ResetVFXEmitter(HRL_VFXEmitter* emitter)
{
	if (!emitter) return;
	emitter->particles_.clear();
	emitter->spawn_accumulator_ = 0.f;
	emitter->next_burst_ = 0;
	emitter->spawned_total_ = 0;
}

static void SimulateVFXEmitter(HRL_VFXEmitter* emitter, HRL_VFXSystem* system, float dt)
{
	if (!emitter || !system || !emitter->enabled_) return;

	if (system->playing_ && !system->paused_)
	{
		const float rate = std::max(0.f, emitter->spawn_rate_);
		emitter->spawn_accumulator_ += rate * dt;
		while (emitter->spawn_accumulator_ >= 1.f && emitter->particles_.size() < emitter->max_particles_)
		{
			SpawnVFXParticle(emitter);
			emitter->spawn_accumulator_ -= 1.f;
		}

		while (emitter->next_burst_ < emitter->bursts_.size() && system->time_ + 1e-6f >= emitter->bursts_[emitter->next_burst_].time)
		{
			const HRL_uint count = emitter->bursts_[emitter->next_burst_].count;
			for (HRL_uint i = 0; i < count && emitter->particles_.size() < emitter->max_particles_; ++i)
				SpawnVFXParticle(emitter);
			++emitter->next_burst_;
		}
	}

	for (auto& p : emitter->particles_)
	{
		if (p.lifetime <= 0.f) continue;
		p.age += dt;
		if (p.age >= p.lifetime) continue;
		const float normalizedLife = glm::clamp(p.age / p.lifetime, 0.f, 1.f);

		glm::vec3 accel = emitter->gravity_ + emitter->force_;
		if (emitter->noise_strength_ > 0.f)
		{
			const float t = (system->time_ + p.age) * emitter->noise_frequency_ + static_cast<float>(p.seed % 97u);
			accel += glm::vec3(
				std::sin(t * 1.37f),
				std::cos(t * 1.91f),
				std::sin(t * 2.43f + 0.7f)) * emitter->noise_strength_ * emitter->noise_scroll_speed_;
		}
		p.velocity += accel * dt;
		if (emitter->drag_ > 0.f)
			p.velocity *= std::max(0.f, 1.f - emitter->drag_ * dt);
		p.position += p.velocity * dt;
		p.rotation = p.base_rotation + p.angular_velocity * p.age;

		if (emitter->collision_enabled_)
		{
			const bool localSimulation = emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL;
			const glm::mat4 systemMatrix = localSimulation ? VFXSystemMatrix(system) : glm::mat4(1.f);
			const glm::mat4 worldToLocal = localSimulation ? glm::inverse(systemMatrix) : glm::mat4(1.f);
			glm::vec3 worldPosition = glm::vec3(systemMatrix * glm::vec4(p.position, 1.f));
			glm::vec3 worldVelocity = VFXTransformDirection(systemMatrix, p.velocity) * glm::length(p.velocity);
			bool hit = false;

			if (emitter->collision_scene_ != HRL_INVALID_ID)
			{
				auto collisionSceneIt = ctx_.scenes.find(emitter->collision_scene_);
				if (collisionSceneIt != ctx_.scenes.end() && collisionSceneIt->second)
				{
					for (const auto& [meshId, mesh] : collisionSceneIt->second->meshes)
					{
						(void)meshId;
						if (!mesh || mesh->type_ == HRL_SPRITE || mesh->bounds_radius_ <= 0.f) continue;
						glm::mat4 meshMatrix(1.f);
						meshMatrix = glm::translate(meshMatrix, mesh->position_);
						meshMatrix = glm::translate(meshMatrix, mesh->pivot_point_);
						meshMatrix = glm::rotate(meshMatrix, glm::radians(mesh->rotation_.x), glm::vec3(1.f,0.f,0.f));
						meshMatrix = glm::rotate(meshMatrix, glm::radians(mesh->rotation_.y), glm::vec3(0.f,1.f,0.f));
						meshMatrix = glm::rotate(meshMatrix, glm::radians(mesh->rotation_.z), glm::vec3(0.f,0.f,1.f));
						meshMatrix = glm::translate(meshMatrix, -mesh->pivot_point_);
						meshMatrix = glm::scale(meshMatrix, mesh->scale_);
						const glm::vec3 center = glm::vec3(meshMatrix * glm::vec4(mesh->bounds_center_, 1.f));
						const float scale = std::max({std::abs(mesh->scale_.x), std::abs(mesh->scale_.y), std::abs(mesh->scale_.z)});
						const float radius = std::max(0.001f, mesh->bounds_radius_ * scale);
						const glm::vec3 delta = worldPosition - center;
						const float distance = glm::length(delta);
						if (distance < radius)
						{
							const glm::vec3 normal = distance > 1e-5f ? delta / distance : glm::vec3(0.f,1.f,0.f);
							worldPosition = center + normal * radius;
							const float normalVelocity = glm::dot(worldVelocity, normal);
							if (normalVelocity < 0.f) worldVelocity -= (1.f + glm::clamp(emitter->collision_restitution_, 0.f, 1.f)) * normalVelocity * normal;
							const glm::vec3 tangent = worldVelocity - normal * glm::dot(worldVelocity, normal);
							worldVelocity = normal * glm::dot(worldVelocity, normal) + tangent * std::max(0.f, 1.f - glm::clamp(emitter->collision_friction_, 0.f, 1.f) * dt * 8.f);
							hit = true;
							break;
						}
					}
				}
			}
			else if (worldPosition.y < 0.f)
			{
				worldPosition.y = 0.f;
				if (worldVelocity.y < 0.f) worldVelocity.y = -worldVelocity.y * glm::clamp(emitter->collision_restitution_, 0.f, 1.f);
				worldVelocity.x *= std::max(0.f, 1.f - glm::clamp(emitter->collision_friction_, 0.f, 1.f) * dt * 8.f);
				worldVelocity.z *= std::max(0.f, 1.f - glm::clamp(emitter->collision_friction_, 0.f, 1.f) * dt * 8.f);
				hit = true;
			}

			if (hit)
			{
				p.position = glm::vec3(worldToLocal * glm::vec4(worldPosition, 1.f));
				const float speed = glm::length(worldVelocity);
				p.velocity = speed > 1e-6f ? VFXTransformDirection(worldToLocal, worldVelocity) * speed : glm::vec3(0.f);
			}
		}

		const HRL_VFXCurve* colorCurve = FindVFXCurve(emitter->color_curve_);
		if (colorCurve) p.color = EvalVFXColor(colorCurve, normalizedLife);
		const HRL_VFXCurve* sizeCurve = FindVFXCurve(emitter->size_curve_);
		if (sizeCurve)
		{
			const float scale = std::max(0.f, EvalVFXFloat(sizeCurve, normalizedLife, 1.f));
			p.size = emitter->particle_size_ * scale;
		}
		const HRL_VFXCurve* rotationCurve = FindVFXCurve(emitter->rotation_curve_);
		if (rotationCurve)
		{
			const float z = EvalVFXFloat(rotationCurve, normalizedLife, 0.f);
			p.rotation.z += z;
		}
	}

	emitter->particles_.erase(
		std::remove_if(emitter->particles_.begin(), emitter->particles_.end(), [](const HRL_VFXParticle& p){ return p.age >= p.lifetime; }),
		emitter->particles_.end());
}

static void SimulateVFXSystem(HRL_VFXSystem* system, float dt)
{
	if (!system || !system->enabled_ || !system->playing_ || system->paused_ || dt <= 0.f) return;
	const float scaledDt = dt * std::max(0.f, system->time_scale_);
	if (scaledDt <= 0.f) return;

	const float previousTime = system->time_;
	system->time_ += scaledDt;

	if (system->duration_ > 0.f && system->time_ >= system->duration_)
	{
		if (system->looping_)
		{
			system->time_ = std::fmod(system->time_, system->duration_);
			for (auto& [id, emitter] : system->emitters_)
			{
				(void)id;
				if (emitter) emitter->next_burst_ = 0;
			}
		}
		else
		{
			system->time_ = system->duration_;
			system->playing_ = false;
		}
	}

	// If a system loops without an explicit duration, bursts remain one-shot.
	(void)previousTime;
	for (auto& [id, emitter] : system->emitters_)
	{
		(void)id;
		SimulateVFXEmitter(emitter, system, scaledDt);
	}
}
}


/// API Implementation ///

void HRL_Init(HRL_E_APIs _api)
{
	g_AsyncResourceLoader.Start();
	if (_api != HRL_OPENGL_33 && _api != HRL_VULKAN)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_Init: selected backend is not implemented");
		return;
	}

	switch (_api)
	{
	case HRL_OPENGL_33 :
	{
		g_Backend = GetOpenGL33Backend();
		g_Backend.RHI_Init();
		break;
	}
	case HRL_OPENGL_45 :
	{
		break;
	}
	case HRL_VULKAN :
	{
		g_Backend = GetVulkanBackend();
		g_Backend.RHI_Init();
		break;
	}
	case HRL_D3D11 :
	{
		break;
	}
	case HRL_D3D12 :
	{
		break;
	}
	case HRL_METAL :
	{
		break;
	}
	case HRL_NVN :
	{
		break;
	}
	case HRL_GNM :
	{
		break;
	}
	default :
	{
		assert(false && "HRL : Backend not supported");
		break;
	}
	}
}

void HRL_InitContext(HRL_uint _width, HRL_uint _height, void* _loader)
{
	ctx_.window_width  = _width;
	ctx_.window_height = _height;
	// The third parameter is still the OpenGL loader for the OpenGL backend.
	// Vulkan presentation is configured through hrl_vulkan.h and never casts the
	// loader argument to a Vulkan-specific private structure.
	if (g_Backend.RHI_InitContext)
	{
		g_Backend.RHI_InitContext(_width, _height, _loader);
	}
}

void HRL_Shutdown()
{
	HRL_WaitForAllAsyncResources();
	g_AsyncResourceLoader.Stop();
	// Destroy flat resources that are referenced by scene viewports first.
	{
		std::vector<HRL_id> ids;
		ids.reserve(ctx_.post_processes.size());
		for (const auto& [id, pp] : ctx_.post_processes)
		{ (void)pp; ids.push_back(id); }
		for (HRL_id id : ids) HRL_DeletePostProcess(id);
	}
	{
		std::vector<HRL_id> ids;
		ids.reserve(ctx_.widgets.size());
		for (const auto& [id, widget] : ctx_.widgets)
		{ (void)widget; ids.push_back(id); }
		for (HRL_id id : ids) HRL_DeleteWidget(id);
	}

	//supprimer tous les objets de toutes les scenes
	for (const auto& [scene_id, scene] : ctx_.scenes)
	{
		for (const auto& [id, mesh] : scene->meshes)
		{
			g_Backend.RHI_DeleteMesh(id);
			delete mesh;
		}
		scene->meshes.clear();

		for (const auto& [id, light] : scene->lights)
		{
			g_Backend.RHI_DeleteLight(id);
			delete light;
		}
		scene->lights.clear();

		for (const auto& [id, landscape] : scene->landscapes)
		{
			(void)id;
			delete landscape;
		}
		scene->landscapes.clear();

		std::vector<HRL_id> vfx_system_ids;
		vfx_system_ids.reserve(scene->vfx_systems.size());
		for (const auto& [id, system] : scene->vfx_systems) { (void)system; vfx_system_ids.push_back(id); }
		for (HRL_id id : vfx_system_ids) HRL_DeleteVFXSystem(id);

		for (const auto& [id, fog] : scene->volumetric_fogs)
		{
			delete fog;
		}
		scene->volumetric_fogs.clear();

		for (const auto& [id, decal] : scene->decals)
		{
			delete decal;
		}
		scene->decals.clear();

		for (const auto& [id, viewport] : scene->viewports)
		{
			delete viewport;
		}
		scene->viewports.clear();

		for (const auto& [id, camera] : scene->cameras)
		{
			delete camera;
		}
		scene->cameras.clear();

		// Release backend scene resources before the scene object itself disappears.
		g_Backend.RHI_DeleteScene(scene_id);
		delete scene;
	}
	ctx_.scenes.clear();
	ctx_.debug_renderers.clear();

	//nettoyer les caches flat
	ctx_.meshes.clear();
	ctx_.landscapes.clear();
	ctx_.lights.clear();
	ctx_.volumetric_fogs.clear();
	ctx_.decals.clear();
	ctx_.vfx_systems.clear();
	ctx_.vfx_emitters.clear();
	ctx_.vfx_curves.clear();
	ctx_.vfx_has_frame_time = false;
	ctx_.vfx_last_frame_time = 0.0;
	ctx_.viewports.clear();
	ctx_.cameras.clear();
	ctx_.post_processes.clear();
	ctx_.pending_screenshots.clear();

	for (const auto& [id, material] : ctx_.materials)
	{
		if (material)
		{
			for (HRL_id textureId : material->owned_textures_)
			{
				if (textureId != HRL_INVALID_ID)
					HRL_DeleteTexture(textureId);
			}
		}
		delete material;
	}
	ctx_.materials.clear();

	for (const auto& [id, font] : ctx_.fonts)
	{
		delete font;
	}
	ctx_.fonts.clear();
	ctx_.async_resource_states.clear();

	g_Backend.RHI_Shutdown();
}

void ProcessAsyncResourceUploads()
{
  AsyncResourceResult result;
  while (g_AsyncResourceLoader.TryPopResult(result))
  {
    auto it = ctx_.async_resource_states.find(result.id);
    if (it == ctx_.async_resource_states.end()) continue;
    if (!result.success)
    {
      it->second = static_cast<int>(AsyncResourceState::Failed);
      SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, result.error.empty() ? "Async resource preparation failed" : result.error);
      continue;
    }
    if (result.type == AsyncResourceResult::Type::Texture)
    {
      const HRL_id created = g_Backend.RHI_CreateTextureFromBitmapWithId ? g_Backend.RHI_CreateTextureFromBitmapWithId(result.id, std::move(result.bitmap)) : HRL_INVALID_ID;
      it->second = created == result.id ? static_cast<int>(AsyncResourceState::Ready) : static_cast<int>(AsyncResourceState::Failed);
      if (created != result.id) SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "Async texture GPU upload failed");
    }
    else
    {
      const HRL_id created = g_Backend.RHI_CreateShaderWithId ? g_Backend.RHI_CreateShaderWithId(result.id, result.vert.data(), result.vert.size(), result.frag.data(), result.frag.size()) : HRL_INVALID_ID;
      it->second = created == result.id ? static_cast<int>(AsyncResourceState::Ready) : static_cast<int>(AsyncResourceState::Failed);
      if (created != result.id) SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "Async shader compilation/upload failed");
    }
  }
}

void HRL_BeginFrame()
{
	// Completed CPU-side loads are uploaded to the active OpenGL context here.
	ProcessAsyncResourceUploads();
	if (g_Backend.RHI_BeginFrame)
		g_Backend.RHI_BeginFrame();
	const auto now = std::chrono::steady_clock::now();
	const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
	if (ctx_.vfx_has_frame_time)
	{
		const float dt = static_cast<float>(glm::clamp(seconds - ctx_.vfx_last_frame_time, 0.0, 0.1));
		for (auto& [scene_id, scene] : ctx_.scenes)
		{
			(void)scene_id;
			if (!scene || scene->screen_messages.empty())
				continue;
			for (auto& message : scene->screen_messages)
				message.remaining_seconds -= dt;
			scene->screen_messages.erase(
				std::remove_if(scene->screen_messages.begin(), scene->screen_messages.end(),
					[](const HRL_ScreenMessage& message) { return message.remaining_seconds <= 0.0f; }),
				scene->screen_messages.end());
		}
		for (const auto& [id, system] : ctx_.vfx_systems)
		{
			(void)id;
			if (system && system->auto_update_)
				SimulateVFXSystem(system, dt);
		}
	}
	ctx_.vfx_last_frame_time = seconds;
	ctx_.vfx_has_frame_time = true;
}


void HRL_EndFrame()
{
	ProcessGizmoInput();
	//appels à RHI_DrawMesh, HRI_BindMaterial, etc...
	for (const auto& [scene_id, scene] : ctx_.scenes)
	{
		if (!scene)
		{
			SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_EndFrame: Tried to bind an invalid scene");
			continue;
		}
		/**
				g_Backend.RHI_BindScene(scene_id);
				g_Backend.RHI_ClearScene();

				for (const auto& [id, viewport] : scene->viewports)
				{
					//camera can be nullptr, just continue if not initialized
					if (!viewport->camera_)
					{
						continue;
					}

					g_Backend.RHI_BindViewport(viewport);
					g_Backend.RHI_ComputeFrameMatrices();

					// --- Draw Sprites --- //
					for (const auto& sprite : GetSortedSprites(scene))
					{
						auto mat_it = ctx_.materials.find(sprite->material_);
						if (mat_it == ctx_.materials.end())
						{
							SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_EndFrame: tried to draw mesh, material not found");
							continue;
						}
						g_Backend.RHI_BindMaterial(mat_it->second);

						g_Backend.RHI_DrawMesh(sprite);
					}

					auto it_debug = ctx_.debug_renderers.find(scene_id);
					if (it_debug != ctx_.debug_renderers.end())
					{
						g_Backend.RHI_DrawDebug(it_debug->second, ctx_.debug_line_thickness);
					}

					//clear le debug a chaque frame
					ctx_.debug_renderers.clear();

					// --- Mettre ici le draw des mesh 3D --- //
				}
			}*/
		// Each scene owns its own light list. The backend UBO is shared by the
		// context, so refresh it immediately before rendering this scene.
		UpdateSceneLights(scene_id);
		g_Backend.RHI_RenderScene(scene, scene_id);

		// Debug primitives are owned by the HRL context, while the backend
		// renderer has its own private context. Draw them explicitly here so
		// they are not lost between HRL and RHI_RenderScene().
		auto debugIt = ctx_.debug_renderers.find(scene_id);
		if (debugIt != ctx_.debug_renderers.end())
		{
			g_Backend.RHI_DrawDebug(debugIt->second, ctx_.debug_line_thickness);
		}

		// Screenshot requests are captured after the scene has completed its
		// post-process/UI passes for the frame.
		auto screenshotIt = ctx_.pending_screenshots.find(scene_id);
		if (screenshotIt != ctx_.pending_screenshots.end())
		{
			if (g_Backend.RHI_TakeScreenshot)
				g_Backend.RHI_TakeScreenshot(scene_id, screenshotIt->second.c_str());
			ctx_.pending_screenshots.erase(screenshotIt);
		}

		// Debug primitives are one-frame submissions. The backend consumes them
		// while rendering the complete scene (all viewports) above.
		ctx_.debug_renderers.erase(scene_id);
	}

	// Mouse transitions are frame-scoped. Keep the held state until the next input event.
	ctx_.mouseLeftPressed = false;
	ctx_.mouseLeftReleased = false;
	if (!ctx_.mouseLeftDown)
		ctx_.mouseCaptureWidget = HRL_INVALID_ID;
	if (!ctx_.mouseLeftDown && ctx_.mouseCaptureGizmo == HRL_INVALID_ID)
		ctx_.mouseCaptureGizmoPart = HRL_GIZMO_PART_NONE;
	if (g_Backend.RHI_ResetFramebuffer)
		g_Backend.RHI_ResetFramebuffer();
}

void HRL_WindowResizeCallback(int _width, int _height)
{
	ctx_.window_width  = _width;
	ctx_.window_height = _height;
	g_Backend.RHI_WindowResizeCallback(_width, _height);
}


HRL_EError HRL_GetLastError(const char** _detail, HRL_ESeverity* _severity)
{
	//on stocke dans une var statique pour eviter un use after free
	static std::string detail;
	detail     = ctx_.last_error.detail;
	*_detail   = detail.c_str();
	*_severity = ctx_.last_error.severity;
	return ctx_.last_error.code;
}

constexpr const char* errors_str[]={
	"HRL_NO_ERROR",
	"HRL_INVALID_ID",
	"HRL_INVALID_ENUM",
	"HRL_INVALID_VALUE",
	"HRL_INVALID_OPERATION",
	"HRL_INVALID_BACKEND_OPERATION",
	"HRL_SHADER_COMPILE_FAIL",
	"HRL_OUT_OF_MEMORY",
	"HRL_INVALID_FILE_FORMAT"
};
constexpr int HRL_ERROR_BASE = 0x0070;
constexpr int HRL_ERROR_COUNT = sizeof(errors_str) / sizeof(errors_str[0]);

const char* HRL_ErrorEnumToString(HRL_EError err)
{
	int index = static_cast<int>(err) - HRL_ERROR_BASE;

	if (index < 0 || index >= HRL_ERROR_COUNT)
		return "UNKNOWN_ERROR";

	return errors_str[index];
}

constexpr const char* severity_str[]={
	"HRL_SEVERITY_WEAK_WARNING",
	"HRL_SEVERITY_WARNING",
	"HRL_SEVERITY_ERROR",
	"HRL_SEVERITY_FATAL"
};
constexpr int HRL_SEVERITY_BASE = 0x0080;
constexpr int HRL_SEVERITY_COUNT = sizeof(severity_str) / sizeof(severity_str[0]);

const char* HRL_SeverityEnumToString(HRL_ESeverity sev)
{
	int index = static_cast<int>(sev) - HRL_SEVERITY_BASE;

	if (index < 0 || index >= HRL_SEVERITY_COUNT)
		return "UNKNOWN_SEVERITY";

	return severity_str[index];
}

void HRL_RegisterErrorCallback(HRL_CErrorCallback _callback)
{
	ctx_.error_callback = _callback;
}


//Meshes//
HRL_id HRL_CreateMeshSprite(HRL_id _sceneid)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateMeshSprite: invalid scene ID");
		return HRL_INVALID_ID;
	}
	//A sprite is the same internal object type as every other mesh.
	//Its geometry is a small 3D plane and its sprite-specific state is only the UV region.
	const HRL_Vertex3D vertices[4] = {
		{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{ 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}
	};
	const HRL_uint indices[6] = {0, 1, 2, 2, 3, 0};

	auto* m = new HRL_Mesh();
	m->scene_ = _sceneid;
	m->type_  = HRL_SPRITE;
	m->triangle_count_ = 2;
	m->bounds_center_ = glm::vec3(0.f);
	m->bounds_radius_ = std::sqrt(0.5f);

	HRL_id newId = GenerateHRL_ID();
	const bool spriteCreated = g_Backend.RHI_CreateSpriteMesh
		? (g_Backend.RHI_CreateSpriteMesh(newId) == HRL_TRUE)
		: (g_Backend.RHI_CreateMesh(newId, vertices, 4, indices, 6) == HRL_TRUE);
	if (!spriteCreated)
	{
		delete m;
		return HRL_INVALID_ID;
	}
	it_scene->second->meshes.emplace(newId, m);
	ctx_.meshes.emplace(newId, m);

	return newId;
}

void HRL_SetMeshPivotPoint(HRL_id _meshid, float x, float y, float z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshPivotPoint: invalid ID");
		return;
	}
	it->second->pivot_point_ = {x, y, z};
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE)
		MarkSceneGIGeometryDirty(scene_it->second);
}

void HRL_SetSpriteRegion(HRL_id _meshid, float min_u, float min_v, float max_u, float max_v)
{
	if (min_u > max_u || min_v > max_v)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetSpriteRegion: minimum cannot be greater than maximum");
		return;
	}
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpriteRegion: invalid ID");
		return;
	}
	if (it->second->type_ != HRL_SPRITE)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSpriteRegion: trying to set region on a non-sprite mesh");
		return;
	}
	it->second->region_[0] = min_u;
	it->second->region_[1] = min_v;
	it->second->region_[2] = max_u;
	it->second->region_[3] = max_v;
}

void HRL_DeleteMesh(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteMesh: invalid ID");
		return;
	}

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
	{
		scene_it->second->meshes.erase(_meshid);
		if (it->second->type_ != HRL_SPRITE)
			MarkSceneGIGeometryDirty(scene_it->second);
	}

	g_Backend.RHI_DeleteMesh(_meshid);
	delete it->second;
	ctx_.meshes.erase(it);
}

void HRL_SetMeshMaterial(HRL_id _meshid, HRL_id _matid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshMaterial: invalid ID");
		return;
	}
	it->second->material_ = _matid;
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ == HRL_3D_MESH)
		MarkSceneGILightingDirty(scene_it->second);
}

void HRL_SetMeshUserHandle(HRL_id _meshid, void* _handle)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshUserHandle: invalid ID");
		return;
	}

	it->second->user_handle_ = _handle;
}

void* HRL_GetMeshUserHandle(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetMeshUserHandle: invalid ID");
		return nullptr;
	}

	return it->second->user_handle_;
}

void HRL_SetMeshLocation(HRL_id _meshid, float x, float y, float z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshLocation: invalid ID");
		return;
	}
	it->second->position_ = glm::vec3(x, y, z);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE)
		MarkSceneGIGeometryDirty(scene_it->second);
}

void HRL_GetMeshLocation(HRL_id _meshid, float* x, float* y, float* z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetMeshLocation: invalid ID");
		return;
	}

	if (x) *x = it->second->position_.x;
	if (y) *y = it->second->position_.y;
	if (z) *z = it->second->position_.z;
}

void HRL_SetMeshRotation(HRL_id _meshid, float pitch, float yaw, float roll)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshRotation: invalid ID");
		return;
	}
	//glm attend : X-pitch, Y-yaw, Z-roll.
	it->second->rotation_ = glm::vec3(pitch, yaw, roll);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE)
		MarkSceneGIGeometryDirty(scene_it->second);
}

void HRL_SetMeshScale(HRL_id _meshid, float x, float y, float z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshScale: invalid ID");
		return;
	}
	it->second->scale_ = glm::vec3(x, y, z);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE)
		MarkSceneGIGeometryDirty(scene_it->second);
}


static glm::mat4 HRL_InternalMeshModelMatrix(const HRL_Mesh* mesh)
{
	glm::mat4 model(1.f);
	model = glm::translate(model, mesh->position_);
	model = glm::translate(model, mesh->pivot_point_);
	model = glm::rotate(model, glm::radians(mesh->rotation_.x), glm::vec3(1.f, 0.f, 0.f));
	model = glm::rotate(model, glm::radians(mesh->rotation_.y), glm::vec3(0.f, 1.f, 0.f));
	model = glm::rotate(model, glm::radians(mesh->rotation_.z), glm::vec3(0.f, 0.f, 1.f));
	model = glm::translate(model, -mesh->pivot_point_);
	model = glm::scale(model, mesh->scale_);
	return model;
}

float HRL_GetMeshCameraDistance(HRL_id _meshid)
{
	auto meshIt = ctx_.meshes.find(_meshid);
	if (meshIt == ctx_.meshes.end() || !meshIt->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetMeshCameraDistance: invalid mesh ID");
		return -1.f;
	}

	auto sceneIt = ctx_.scenes.find(meshIt->second->scene_);
	if (sceneIt == ctx_.scenes.end() || !sceneIt->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetMeshCameraDistance: mesh scene no longer exists");
		return -1.f;
	}

	const glm::mat4 model = HRL_InternalMeshModelMatrix(meshIt->second);
	const glm::vec3 center = glm::vec3(model * glm::vec4(meshIt->second->bounds_center_, 1.f));
	float minimumDistance = std::numeric_limits<float>::infinity();

	for (const auto& [viewportId, viewport] : sceneIt->second->viewports)
	{
		(void)viewportId;
		if (!viewport || !viewport->camera_)
			continue;
		const float distance = glm::length(center - viewport->camera_->position_);
		minimumDistance = std::min(minimumDistance, distance);
	}

	if (!std::isfinite(minimumDistance))
	{
		for (const auto& [cameraId, camera] : sceneIt->second->cameras)
		{
			(void)cameraId;
			if (!camera) continue;
			minimumDistance = std::min(minimumDistance, glm::length(center - camera->position_));
		}
	}

	if (!std::isfinite(minimumDistance))
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetMeshCameraDistance: scene has no camera");
		return -1.f;
	}

	return minimumDistance;
}


namespace { static bool RebuildLODs(HRL_Mesh* mesh); }

static HRL_Mesh* GetMeshForLOD(HRL_id id, const char* errorMessage)
{
	auto it = ctx_.meshes.find(id);
	if (it == ctx_.meshes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, errorMessage);
		return nullptr;
	}
	if (it->second->type_ == HRL_SPRITE || it->second->type_ == HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"LOD generation is currently supported for static 3D meshes only");
		return nullptr;
	}
	return it->second;
}

static void MarkMeshSceneShadowsDirty(HRL_Mesh* mesh)
{
	if (!mesh) return;
	auto sceneIt = ctx_.scenes.find(mesh->scene_);
	if (sceneIt != ctx_.scenes.end())
		sceneIt->second->shadows_dirty = true;
}

void HRL_SetMeshLODAutomatic(HRL_id id, int enabled)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODAutomatic: invalid mesh ID");
	if (!mesh) return;
	mesh->lod_automatic_ = enabled != HRL_FALSE;
	mesh->last_lod_level_ = 0;
	if (mesh->lod_automatic_ && mesh->lods_.size() < mesh->lod_levels_)
		HRL_ForceMeshLODRebuild(id);
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODMode(HRL_id id, HRL_ELODMode mode)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODMode: invalid mesh ID");
	if (!mesh) return;
	if (mode != HRL_LOD_DISTANCE && mode != HRL_LOD_SCREEN_SIZE)
	{
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODMode: invalid mode");
		return;
	}
	mesh->lod_mode_ = mode;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODLevels(HRL_id id, HRL_uint levels)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODLevels: invalid mesh ID");
	if (!mesh) return;
	if (levels < 1 || levels > 8)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODLevels: levels must be in [1,8]");
		return;
	}
	mesh->lod_levels_ = levels;
	HRL_ForceMeshLODRebuild(id);
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODDistance(HRL_id id, float distance)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODDistance: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(distance) || distance <= 0.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODDistance: expected finite value > 0");
		return;
	}
	if (distance >= mesh->lod_max_distance_)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODDistance: base distance must be smaller than max distance");
		return;
	}
	mesh->lod_base_distance_ = distance;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODScale(HRL_id id, float scale)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODScale: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(scale) || scale <= 1.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODScale: expected finite value > 1");
		return;
	}
	mesh->lod_distance_scale_ = scale;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODMinDistance(HRL_id id, float distance)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODMinDistance: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(distance) || distance < 0.0f || distance >= mesh->lod_max_distance_)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODMinDistance: expected 0 <= value < max distance");
		return;
	}
	mesh->lod_min_distance_ = distance;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODMaxDistance(HRL_id id, float distance)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODMaxDistance: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(distance) || distance <= 0.0f || distance <= mesh->lod_min_distance_ || distance <= mesh->lod_base_distance_)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODMaxDistance: expected value > min distance and base distance");
		return;
	}
	mesh->lod_max_distance_ = distance;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODScreenThreshold(HRL_id id, float threshold)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODScreenThreshold: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(threshold) || threshold <= 0.0f || threshold > 1.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODScreenThreshold: expected value in (0,1]");
		return;
	}
	mesh->lod_screen_threshold_ = threshold;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODScreenScale(HRL_id id, float scale)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODScreenScale: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(scale) || scale <= 0.0f || scale >= 1.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODScreenScale: expected value in (0,1)");
		return;
	}
	mesh->lod_screen_scale_ = scale;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODHysteresis(HRL_id id, float hysteresis)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODHysteresis: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(hysteresis) || hysteresis < 0.0f || hysteresis > 0.49f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODHysteresis: expected value in [0,0.49]");
		return;
	}
	mesh->lod_hysteresis_ = hysteresis;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODOverride(HRL_id id, int level)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODOverride: invalid mesh ID");
	if (!mesh) return;
	if (level < -1 || level > 7)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODOverride: expected -1 or [0,7]");
		return;
	}
	mesh->lod_override_ = level;
	mesh->last_lod_level_ = level < 0 ? 0 : std::min(level, (int)mesh->lods_.size() - 1);
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_ForceMeshLODRebuild(HRL_id id)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_ForceMeshLODRebuild: invalid mesh ID");
	if (!mesh) return;
	if (!RebuildLODs(mesh))
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_ForceMeshLODRebuild: unable to generate LODs");
		return;
	}

	if (g_Backend.RHI_DeleteMeshLODs)
		g_Backend.RHI_DeleteMeshLODs(id);

	size_t uploaded = 1; // LOD 0 already exists in the GPU resource.
	for (size_t level = 1; level < mesh->lods_.size(); ++level)
	{
		auto& data = mesh->lods_[level];
		if (!g_Backend.RHI_CreateMeshLOD ||
			g_Backend.RHI_CreateMeshLOD(id, (HRL_uint)level,
				data.vertices.data(), data.vertices.size(),
				data.indices.data(), data.indices.size()) != HRL_TRUE)
		{
			SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
				"HRL_ForceMeshLODRebuild: failed to upload generated LOD");
			break;
		}
		++uploaded;
	}
	if (uploaded < mesh->lods_.size())
		mesh->lods_.resize(uploaded);

	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

HRL_uint HRL_GetMeshLODCount(HRL_id id)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_GetMeshLODCount: invalid mesh ID");
	return mesh ? (HRL_uint)mesh->lods_.size() : 0;
}

size_t HRL_GetMeshLODVertexCount(HRL_id id, HRL_uint level)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_GetMeshLODVertexCount: invalid mesh ID");
	if (!mesh) return 0;
	if (level >= mesh->lods_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_GetMeshLODVertexCount: invalid level");
		return 0;
	}
	return mesh->lods_[level].vertices.size();
}

size_t HRL_GetMeshLODTriangleCount(HRL_id id, HRL_uint level)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_GetMeshLODTriangleCount: invalid mesh ID");
	if (!mesh) return 0;
	if (level >= mesh->lods_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_GetMeshLODTriangleCount: invalid level");
		return 0;
	}
	return mesh->lods_[level].indices.size() / 3u;
}

int HRL_GetMeshLODLevel(HRL_id id)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_GetMeshLODLevel: invalid mesh ID");
	return mesh ? mesh->last_lod_level_ : 0;
}

void HRL_SetSpriteDrawOrder(HRL_id _meshid, float _draworder)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpriteDrawOrder: invalid ID");
		return;
	}
	it->second->draw_order_ = _draworder;
}



//Lights//
HRL_id HRL_CreateLight(HRL_id _sceneid, HRL_ELightType _type)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateLight: invalid scene ID");
		return HRL_INVALID_ID;
	}

	auto* l = new HRL_Light();
	l->scene_ = _sceneid;
	l->type_ = _type;
	if (_type == HRL_SKY_LIGHT)
		l->intensity_ = 0.15f;

	HRL_id newId = GenerateHRL_ID();
	l->id_ = newId;
	it_scene->second->lights.emplace(newId, l);
	ctx_.lights.emplace(newId, l);
	MarkSceneGILightingDirty(it_scene->second);

	UpdateSceneLights(_sceneid);

	return newId;
}

void HRL_DeleteLight(HRL_id _lightid)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteLight: invalid ID");
		return;
	}

	const HRL_id owner_scene = it->second->scene_;
	g_Backend.RHI_DeleteLight(_lightid);
	auto scene_it = ctx_.scenes.find(owner_scene);
	if (scene_it != ctx_.scenes.end())
	{
		scene_it->second->lights.erase(_lightid);
		MarkSceneGILightingDirty(scene_it->second);
		scene_it->second->shadows_dirty = true;
	}

	delete it->second;
	ctx_.lights.erase(it);

	UpdateSceneLights(owner_scene);
}

void HRL_SetLightColor(HRL_id _lightid, float x, float y, float z)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightColor: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	it->second->color_ = glm::vec4(x, y, z, 0.f);

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
		MarkSceneGILightingDirty(scene_it->second);

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightIntensity(HRL_id _lightid, float i)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightIntensity: invalid ID");
		return;
	}
	it->second->intensity_ = i;

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
		MarkSceneGILightingDirty(scene_it->second);

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightAttenuation(HRL_id _lightid, float a)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightAttenuation: invalid ID");
		return;
	}
	it->second->attenuation_ = a;

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
		MarkSceneGILightingDirty(scene_it->second);

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightLocation(HRL_id _lightid, float x, float y, float z)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightLocation: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	it->second->position_ = glm::vec4(x, y, z, 0.f);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightRotation(HRL_id _lightid, float pitch, float yaw, float roll)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightRotation: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	it->second->rotation_ = glm::vec4(pitch, yaw, roll, 0.f);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightCastShadows(HRL_id _lightid, int _enable)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightCastShadows: invalid ID");
		return;
	}
	it->second->cast_shadows_ = (_enable != HRL_FALSE);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }
	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightShadowBias(HRL_id _lightid, float _bias)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightShadowBias: invalid ID");
		return;
	}
	if (!std::isfinite(_bias) || _bias < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLightShadowBias: bias must be finite and non-negative");
		return;
	}
	it->second->shadow_bias_ = _bias;
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
	{
		MarkSceneGILightingDirty(scene_it->second);
		scene_it->second->shadows_dirty = true;
	}
}

void HRL_SetLightShadowStrength(HRL_id _lightid, float _strength)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightShadowStrength: invalid ID");
		return;
	}
	if (!std::isfinite(_strength) || _strength < 0.f || _strength > 1.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLightShadowStrength: strength must be finite and in [0, 1]");
		return;
	}
	it->second->shadow_strength_ = _strength;
	MarkSceneGILightingDirty(ctx_.scenes.at(it->second->scene_));
	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightShadowResolution(HRL_id _lightid, int _resolution)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightShadowResolution: invalid ID");
		return;
	}
	if (_resolution < 128 || (_resolution & (_resolution - 1)) != 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLightShadowResolution: resolution must be a power of two and at least 128");
		return;
	}
	it->second->shadow_resolution_ = _resolution;
}

void HRL_SetSpotLightInnerCutoff(HRL_id _lightid, float inner_cutoff)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpotLightInnerCutoff: invalid ID");
		return;
	}
	if (it->second->type_ != HRL_SPOT_LIGHT)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSpotLightInnerCutoff: light is not of type SpotLight");
		return;
	}

	it->second->innerCutoff = inner_cutoff;
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetSpotLightOuterCutoff(HRL_id _lightid, float outer_cutoff)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpotLightOuterCutoff: invalid ID");
		return;
	}
	if (it->second->type_ != HRL_SPOT_LIGHT)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSpotLightOuterCutoff: light is not of type SpotLight");
		return;
	}

	it->second->outerCutoff = outer_cutoff;
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}




//Textures//
HRL_id HRL_CreateTexture(const char* _fileContent, size_t _bufferSize)
{
	//gestion des erreurs auto par le backend
	return g_Backend.RHI_CreateTexture(_fileContent, _bufferSize);
}


HRL_id HRL_CreateTextureAsync(const char* data, size_t size)
{
  if (!data || size == 0) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateTextureAsync: invalid data buffer"); return HRL_INVALID_ID; }
  const HRL_id id = GenerateHRL_ID();
  ctx_.async_resource_states[id] = static_cast<int>(AsyncResourceState::Pending);
  AsyncResourceTask task; task.type = AsyncResourceResult::Type::Texture; task.id = id;
  task.bytes.assign(reinterpret_cast<const unsigned char*>(data), reinterpret_cast<const unsigned char*>(data) + size);
  g_AsyncResourceLoader.Enqueue(std::move(task));
  return id;
}


int HRL_IsTextureReady(HRL_id id)
{
  auto it = ctx_.async_resource_states.find(id);
  if (it == ctx_.async_resource_states.end()) return g_Backend.RHI_IsValidTexture ? g_Backend.RHI_IsValidTexture(id) : HRL_FALSE;
  return it->second == static_cast<int>(AsyncResourceState::Ready) && g_Backend.RHI_IsValidTexture(id);
}

void HRL_WaitForTexture(HRL_id id)
{
  for (;;) { ProcessAsyncResourceUploads(); auto it = ctx_.async_resource_states.find(id); if (it == ctx_.async_resource_states.end()) return; if (it->second != static_cast<int>(AsyncResourceState::Pending)) return; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
void HRL_DeleteTexture(HRL_id _textureid)
{
  auto asyncIt = ctx_.async_resource_states.find(_textureid);
  if (asyncIt != ctx_.async_resource_states.end()) {
    if (asyncIt->second == static_cast<int>(AsyncResourceState::Pending)) { asyncIt->second = static_cast<int>(AsyncResourceState::Cancelled); ctx_.async_resource_states.erase(asyncIt); return; }
    ctx_.async_resource_states.erase(asyncIt);
  }
  g_Backend.RHI_DeleteTexture(_textureid);
	MarkAllScenesGILightingDirty();
}


void HRL_GetTextureSize(HRL_id _textureid, int *_width, int *_height)
{
	if (_width && _height)
	{
		g_Backend.RHI_GetTextureSize(_textureid, _width, _height);
	}
	else
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetTextureSize: width or height are not a valid pointer");
	}
}

void HRL_SetTextureMinFilter(HRL_id _textureid, HRL_EFilterType _filter)
{
	g_Backend.RHI_SetTextureMinFilter(_textureid, _filter);
}
void HRL_SetTextureMagFilter(HRL_id _textureid, HRL_EFilterType _filter)
{
	g_Backend.RHI_SetTextureMaxFilter(_textureid, _filter);
}

HRL_id HRL_InternalCreateSDFTextTexture(const char* _text, HRL_id _fontid, float _font_size)
{
	auto it = ctx_.fonts.find(_fontid);
	if (it == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_InternalCreateSDFTextTexture: invalid font ID");
		return HRL_INVALID_ID;
	}

	BitmapResult bmp = GenerateSDFBitmap(_text, &it->second->info, it->second->ttf_buffer, _font_size, 0.0f);
	if (bmp.pixels.empty())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_InternalCreateSDFTextTexture: SDF generation failed");
		return HRL_INVALID_ID;
	}

	return g_Backend.RHI_CreateTextureFromBitmap(bmp);
}

HRL_API HRL_id HRL_CreateTextureFromText(const char* _text, HRL_id _fontid,
	float _font_size, float _wrap_width,
	float r, float g, float b,
	float bg_r, float bg_g, float bg_b, float bg_a
)
{
	auto it = ctx_.fonts.find(_fontid);
	if (it == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateTextureFromText: invalid font ID");
		return HRL_INVALID_ID;
	}

	BitmapResult bmp = GenerateBitmap(_text, &it->second->info, it->second->ttf_buffer,
			_font_size, _wrap_width,
			r, g, b,
			bg_r, bg_g, bg_b, bg_a
	);
	if (bmp.pixels.empty())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateTextureFromText: bitmap generation failed, pixels is empty");
		return HRL_INVALID_ID;
	}

	return g_Backend.RHI_CreateTextureFromBitmap(bmp);
}


void HRL_ClearScreen()
{
	/**for (const auto& s : ctx_.scenes)
	{
		if (!s.second)
		{
			SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_ClearScreen: tried to clear an invalid scene object");
			return;
		}
		g_Backend.RHI_BindScene(s.first);
		g_Backend.RHI_ResetFramebuffer();
	}*/
}



//Scenes//
HRL_id HRL_CreateScene(int _renderOnScreen)
{
	auto* scene = new hrl_scene_t();
	scene->draw_on_screen = _renderOnScreen;

	HRL_id newId = GenerateHRL_ID();
	ctx_.scenes.emplace(newId, scene);

	g_Backend.RHI_CreateScene(newId, _renderOnScreen);
	return newId;


}

void HRL_DeleteScene(HRL_id _sceneid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteScene: invalid scene ID");
		return;
	}

	//on collecte les IDs d'abord pour eviter l'invalidation d'iterateur
	std::vector<HRL_id> mesh_ids, landscape_ids, light_ids, fog_ids, gizmo_ids, vfx_system_ids, viewport_ids, camera_ids;
	for (const auto& [id, mesh]     : it->second->meshes)          mesh_ids.push_back(id);
	for (const auto& [id, landscape] : it->second->landscapes)   { (void)landscape; landscape_ids.push_back(id); }
	for (const auto& [id, light]    : it->second->lights)         light_ids.push_back(id);
	for (const auto& [id, fog]      : it->second->volumetric_fogs) fog_ids.push_back(id);
	for (const auto& [id, gizmo]   : it->second->gizmos)          gizmo_ids.push_back(id);
	for (const auto& [id, system] : it->second->vfx_systems)        { (void)system; vfx_system_ids.push_back(id); }
	for (const auto& [id, viewport] : it->second->viewports)      viewport_ids.push_back(id);
	for (const auto& [id, camera]   : it->second->cameras)        camera_ids.push_back(id);

	//delete every objects that owns the scene
	for (auto id : mesh_ids)      HRL_DeleteMesh(id);
	for (auto id : landscape_ids) HRL_DeleteLandscape(id);
	for (auto id : light_ids)     HRL_DeleteLight(id);
	for (auto id : fog_ids)       HRL_DeleteVolumetricFog(id);
	for (auto id : gizmo_ids)     HRL_DeleteGizmo(id);
	for (auto id : vfx_system_ids) HRL_DeleteVFXSystem(id);
	for (auto id : viewport_ids)  HRL_DeleteViewport(id);
	for (auto id : camera_ids)    HRL_DeleteCamera(id);

	delete it->second->voxel_world;
	it->second->voxel_world = nullptr;
	delete it->second;

	ctx_.pending_screenshots.erase(_sceneid);
	g_Backend.RHI_DeleteScene(_sceneid);

	ctx_.scenes.erase(it);
}


namespace
{
static HRL_VoxelWorld* FindVoxelWorld(HRL_id sceneid)
{
	auto it = ctx_.scenes.find(sceneid);
	if (it == ctx_.scenes.end() || !it->second)
		return nullptr;
	return it->second->voxel_world;
}

static bool VoxelWorldElementCount(int width, int height, size_t& outCount)
{
	if (width <= 0 || height <= 0)
		return false;
	const size_t w = static_cast<size_t>(width);
	const size_t h = static_cast<size_t>(height);
	if (h != 0 && w > std::numeric_limits<size_t>::max() / h)
		return false;
	outCount = w * h;
	return true;
}

static void EnsureVoxelWorld(HRL_id sceneid)
{
	auto it = ctx_.scenes.find(sceneid);
	if (it == ctx_.scenes.end() || !it->second)
		return;
	if (!it->second->voxel_world)
		it->second->voxel_world = new HRL_VoxelWorld();
}

static uint64_t VoxelChunkKey(int chunkX, int chunkY)
{
	return (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32u) |
		static_cast<uint32_t>(chunkY);
}

static void MarkVoxelChunkDirty(HRL_VoxelWorld* world, int voxelX, int voxelY)
{
	if (!world || world->chunk_size_ <= 0 || voxelX < 0 || voxelY < 0)
		return;

	const int chunkX = voxelX / world->chunk_size_;
	const int chunkY = voxelY / world->chunk_size_;
	world->dirty_chunks_.insert(VoxelChunkKey(chunkX, chunkY));
	world->render_dirty_chunks_.insert(VoxelChunkKey(chunkX, chunkY));

	// Keep local voxel edits separate from the global geometry revision.
	// Otherwise changing one voxel makes every visible chunk look stale.
	world->voxel_edit_dirty_ = true;
	if (world->voxel_edit_depth_ == 0)
	{
		++world->voxel_revision_;
		world->voxel_edit_dirty_ = false;
	}
}

static constexpr uint32_t kVoxelWorldSaveMagic = 0x31564C48u; // "HLV1" on little-endian files.
static constexpr uint16_t kVoxelWorldSaveVersion = 1u;
static constexpr size_t kVoxelWorldSaveHeaderSize = 28u;
static constexpr uint32_t kVoxelWorldDeltaMagic = 0x31504448u; // "HDP1" little-endian.
static constexpr uint16_t kVoxelWorldDeltaVersion = 1u;
static constexpr uint8_t kVoxelWorldDeltaDeleteEncoding = 0xffu;

enum class VoxelSaveEncoding : uint8_t
{
	Constant = 0,
	Palette = 1,
	Raw = 2,
	Rle = 3
};

class VoxelSaveWriter
{
public:
	void WriteU8(uint8_t v) { data_.push_back(v); }
	void WriteU16(uint16_t v)
	{
		data_.push_back(static_cast<uint8_t>(v & 0xffu));
		data_.push_back(static_cast<uint8_t>((v >> 8u) & 0xffu));
	}
	void WriteU32(uint32_t v)
	{
		for (unsigned i = 0; i < 4; ++i)
			data_.push_back(static_cast<uint8_t>((v >> (8u * i)) & 0xffu));
	}
	void WriteF32(float v)
	{
		uint32_t bits = 0;
		static_assert(sizeof(bits) == sizeof(v), "float must be 32-bit");
		std::memcpy(&bits, &v, sizeof(bits));
		WriteU32(bits);
	}
	void WriteBytes(const uint8_t* bytes, size_t count)
	{
		if (count == 0) return;
		data_.insert(data_.end(), bytes, bytes + count);
	}
	void WriteVarUInt(uint32_t value)
	{
		while (value >= 0x80u)
		{
			WriteU8(static_cast<uint8_t>(value) | 0x80u);
			value >>= 7u;
		}
		WriteU8(static_cast<uint8_t>(value));
	}
	const std::vector<uint8_t>& Data() const { return data_; }
private:
	std::vector<uint8_t> data_;
};

class VoxelSaveReader
{
public:
	VoxelSaveReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

	bool ReadU8(uint8_t& out)
	{
		if (pos_ >= size_) return false;
		out = data_[pos_++];
		return true;
	}
	bool ReadU16(uint16_t& out)
	{
		uint8_t b0 = 0, b1 = 0;
		if (!ReadU8(b0) || !ReadU8(b1)) return false;
		out = static_cast<uint16_t>(b0 | (static_cast<uint16_t>(b1) << 8u));
		return true;
	}
	bool ReadU32(uint32_t& out)
	{
		uint8_t b[4] = {};
		for (unsigned i = 0; i < 4; ++i)
			if (!ReadU8(b[i])) return false;
		out = static_cast<uint32_t>(b[0]) |
			(static_cast<uint32_t>(b[1]) << 8u) |
			(static_cast<uint32_t>(b[2]) << 16u) |
			(static_cast<uint32_t>(b[3]) << 24u);
		return true;
	}
	bool ReadF32(float& out)
	{
		uint32_t bits = 0;
		if (!ReadU32(bits)) return false;
		std::memcpy(&out, &bits, sizeof(out));
		return true;
	}
	bool ReadBytes(const uint8_t*& out, size_t count)
	{
		if (count > Remaining()) return false;
		out = data_ + pos_;
		pos_ += count;
		return true;
	}
	bool ReadVarUInt(uint32_t& out)
	{
		out = 0;
		unsigned shift = 0;
		for (unsigned i = 0; i < 5; ++i)
		{
			uint8_t b = 0;
			if (!ReadU8(b)) return false;
			if (i == 4 && (b & 0xF0u) != 0) return false;
			out |= static_cast<uint32_t>(b & 0x7Fu) << shift;
			if ((b & 0x80u) == 0) return true;
			shift += 7u;
		}
		return false;
	}
	size_t Position() const { return pos_; }
	size_t Remaining() const { return size_ - pos_; }
private:
	const uint8_t* data_ = nullptr;
	size_t size_ = 0;
	size_t pos_ = 0;
};

static unsigned VoxelSaveBitsForPalette(size_t count)
{
	unsigned bits = 0;
	size_t v = count > 1 ? count - 1 : 0;
	while (v != 0)
	{
		++bits;
		v >>= 1u;
	}
	return std::max(1u, bits);
}

static void VoxelSaveAppendPackedIndices(
	const HRL_VoxelWorld* world,
	int chunkX, int chunkY,
	const std::array<int16_t, 256>& paletteIndex,
	unsigned bits,
	std::vector<uint8_t>& out)
{
	const int chunkSize = world->chunk_size_;
	const int minX = chunkX * chunkSize;
	const int minY = chunkY * chunkSize;
	const int maxX = std::min(minX + chunkSize, world->width_);
	const int maxY = std::min(minY + chunkSize, world->height_);
	const int chunkW = maxX - minX;
	const HRL_VoxelWorld::VoxelChunk* chunk = world->FindVoxelChunk(chunkX, chunkY);
	if (!chunk || chunkW <= 0)
		return;

	uint32_t accumulator = 0;
	unsigned accumulatorBits = 0;
	for (int y = minY; y < maxY; ++y)
	{
		for (int x = minX; x < maxX; ++x)
		{
			const int localX = x - minX;
			const int localY = y - minY;
			const HRL_VoxelType type = (*chunk)[static_cast<size_t>(localY) * static_cast<size_t>(chunkW) +
				static_cast<size_t>(localX)].type;
			const int16_t palette = paletteIndex[type];
			if (palette < 0) return;
			accumulator |= static_cast<uint32_t>(palette) << accumulatorBits;
			accumulatorBits += bits;
			while (accumulatorBits >= 8u)
			{
				out.push_back(static_cast<uint8_t>(accumulator & 0xffu));
				accumulator >>= 8u;
				accumulatorBits -= 8u;
			}
		}
	}
	if (accumulatorBits != 0)
		out.push_back(static_cast<uint8_t>(accumulator & 0xffu));
}

static void VoxelSaveBuildRle(
	const HRL_VoxelWorld* world,
	int chunkX, int chunkY,
	std::vector<uint8_t>& out)
{
	const int chunkSize = world->chunk_size_;
	const int minX = chunkX * chunkSize;
	const int minY = chunkY * chunkSize;
	const int maxX = std::min(minX + chunkSize, world->width_);
	const int maxY = std::min(minY + chunkSize, world->height_);
	const int chunkW = maxX - minX;
	const HRL_VoxelWorld::VoxelChunk* chunk = world->FindVoxelChunk(chunkX, chunkY);
	if (!chunk || chunkW <= 0)
		return;

	for (int y = minY; y < maxY; ++y)
	{
		int x = minX;
		while (x < maxX)
		{
			const int localY = y - minY;
			const int localX = x - minX;
			const HRL_VoxelType type = (*chunk)[static_cast<size_t>(localY) * static_cast<size_t>(chunkW) +
				static_cast<size_t>(localX)].type;
			int run = 1;
			while (x + run < maxX &&
				(*chunk)[static_cast<size_t>(localY) * static_cast<size_t>(chunkW) +
					static_cast<size_t>(localX + run)].type == type)
				++run;
			out.push_back(type);
			uint32_t remaining = static_cast<uint32_t>(run);
			while (remaining >= 0x80u)
			{
				out.push_back(static_cast<uint8_t>(remaining) | 0x80u);
				remaining >>= 7u;
			}
			out.push_back(static_cast<uint8_t>(remaining));
			x += run;
		}
	}
}

static bool VoxelSaveBuildChunk(
	const HRL_VoxelWorld* world,
	int chunkX, int chunkY,
	VoxelSaveEncoding& outEncoding,
	uint8_t& outBits,
	std::vector<uint8_t>& outPalette,
	std::vector<uint8_t>& outPayload)
{
	outEncoding = VoxelSaveEncoding::Raw;
	outBits = 0;
	outPalette.clear();
	outPayload.clear();

	std::array<bool, 256> seen{};
	std::vector<HRL_VoxelType> palette;
	palette.reserve(16);
	HRL_VoxelType first = 0;
	bool firstSet = false;
	bool uniform = true;

	const int chunkSize = world->chunk_size_;
	const int minX = chunkX * chunkSize;
	const int minY = chunkY * chunkSize;
	const int maxX = std::min(minX + chunkSize, world->width_);
	const int maxY = std::min(minY + chunkSize, world->height_);
	const int chunkW = maxX - minX;
	const HRL_VoxelWorld::VoxelChunk* chunk = world->FindVoxelChunk(chunkX, chunkY);
	if (!chunk || chunkW <= 0)
		return false;
	for (int y = minY; y < maxY; ++y)
	{
		for (int x = minX; x < maxX; ++x)
		{
			const int localX = x - minX;
			const int localY = y - minY;
			const HRL_VoxelType type = (*chunk)[static_cast<size_t>(localY) * static_cast<size_t>(chunkW) +
				static_cast<size_t>(localX)].type;
			if (!firstSet) { first = type; firstSet = true; }
			else if (type != first) uniform = false;
			if (!seen[type])
			{
				seen[type] = true;
				palette.push_back(type);
			}
		}
	}
	if (!firstSet) return false;
	if (uniform)
	{
		if (first == 0) return false;
		outEncoding = VoxelSaveEncoding::Constant;
		outPalette.push_back(first);
		return true;
	}

	std::array<int16_t, 256> paletteIndex{};
	paletteIndex.fill(-1);
	for (size_t i = 0; i < palette.size(); ++i)
		paletteIndex[palette[i]] = static_cast<int16_t>(i);

	std::vector<uint8_t> palettePayload;
	const unsigned bits = VoxelSaveBitsForPalette(palette.size());
	VoxelSaveAppendPackedIndices(world, chunkX, chunkY, paletteIndex, bits, palettePayload);

	std::vector<uint8_t> rawPayload;
	rawPayload.reserve(static_cast<size_t>(maxX - minX) * static_cast<size_t>(maxY - minY));
	for (int y = minY; y < maxY; ++y)
		for (int x = minX; x < maxX; ++x)
			rawPayload.push_back((*chunk)[static_cast<size_t>(y - minY) * static_cast<size_t>(chunkW) +
				static_cast<size_t>(x - minX)].type);

	std::vector<uint8_t> rlePayload;
	VoxelSaveBuildRle(world, chunkX, chunkY, rlePayload);

	const size_t paletteTotal = palette.size() + palettePayload.size();
	const size_t rawTotal = rawPayload.size();
	const size_t rleTotal = rlePayload.size();
	if (paletteTotal <= rawTotal && paletteTotal <= rleTotal)
	{
		outEncoding = VoxelSaveEncoding::Palette;
		outBits = static_cast<uint8_t>(bits);
		outPalette.assign(palette.begin(), palette.end());
		outPayload = std::move(palettePayload);
	}
	else if (rleTotal <= rawTotal)
	{
		outEncoding = VoxelSaveEncoding::Rle;
		outPayload = std::move(rlePayload);
	}
	else
	{
		outEncoding = VoxelSaveEncoding::Raw;
		outPayload = std::move(rawPayload);
	}
	return true;
}

static bool VoxelSaveBuildAll(const HRL_VoxelWorld* world, std::vector<uint8_t>& out)
{
	if (!world || world->width_ <= 0 || world->height_ <= 0 || world->chunk_size_ <= 0 ||
		!std::isfinite(world->voxel_size_) || world->voxel_size_ <= 0.f)
		return false;

	const uint64_t chunkColumns64 =
		(static_cast<uint64_t>(world->width_) + static_cast<uint64_t>(world->chunk_size_) - 1u) /
		static_cast<uint64_t>(world->chunk_size_);
	const uint64_t chunkRows64 =
		(static_cast<uint64_t>(world->height_) + static_cast<uint64_t>(world->chunk_size_) - 1u) /
		static_cast<uint64_t>(world->chunk_size_);
	if (chunkColumns64 == 0 || chunkRows64 == 0 ||
		chunkColumns64 * chunkRows64 > std::numeric_limits<uint32_t>::max())
		return false;

	VoxelSaveWriter writer;
	writer.WriteU32(kVoxelWorldSaveMagic);
	writer.WriteU16(kVoxelWorldSaveVersion);
	writer.WriteU16(0u);
	writer.WriteU32(static_cast<uint32_t>(world->width_));
	writer.WriteU32(static_cast<uint32_t>(world->height_));
	writer.WriteF32(world->voxel_size_);
	writer.WriteU32(static_cast<uint32_t>(world->chunk_size_));

	const size_t chunkCountOffset = 24u;
	writer.WriteU32(0u);

	uint32_t nonEmptyChunks = 0;

	// The world is sparse: only chunks that actually contain data exist.
	// Iterating the complete chunk grid was the main cost for huge empty worlds.
	for (const auto& [key, chunk] : world->voxel_chunks_)
	{
		(void)chunk;
		const int chunkX = static_cast<int32_t>(key >> 32u);
		const int chunkY = static_cast<int32_t>(key & 0xffffffffu);
		if (chunkX < 0 || chunkY < 0 ||
			static_cast<uint64_t>(chunkX) >= chunkColumns64 ||
			static_cast<uint64_t>(chunkY) >= chunkRows64)
			return false;

		VoxelSaveEncoding encoding = VoxelSaveEncoding::Raw;
		uint8_t bits = 0;
		std::vector<uint8_t> palette;
		std::vector<uint8_t> payload;
		if (!VoxelSaveBuildChunk(world, chunkX, chunkY, encoding, bits, palette, payload))
			continue;

		if (palette.size() > 255u || payload.size() > std::numeric_limits<uint32_t>::max())
			return false;

		writer.WriteU32(static_cast<uint32_t>(static_cast<uint64_t>(chunkY) * chunkColumns64 +
			static_cast<uint64_t>(chunkX)));
		writer.WriteU8(static_cast<uint8_t>(encoding));
		writer.WriteU8(bits);
		writer.WriteU8(static_cast<uint8_t>(palette.size()));
		writer.WriteU8(0u);
		writer.WriteU32(static_cast<uint32_t>(payload.size()));
		writer.WriteBytes(palette.data(), palette.size());
		writer.WriteBytes(payload.data(), payload.size());
		++nonEmptyChunks;
	}

	std::vector<uint8_t> result = writer.Data();
	if (result.size() < kVoxelWorldSaveHeaderSize)
		return false;

	const uint32_t count = nonEmptyChunks;
	result[chunkCountOffset + 0] = static_cast<uint8_t>(count & 0xffu);
	result[chunkCountOffset + 1] = static_cast<uint8_t>((count >> 8u) & 0xffu);
	result[chunkCountOffset + 2] = static_cast<uint8_t>((count >> 16u) & 0xffu);
	result[chunkCountOffset + 3] = static_cast<uint8_t>((count >> 24u) & 0xffu);
	out = std::move(result);
	return true;
}

static bool VoxelSaveDecodeChunk(
	const uint8_t* payload, size_t payloadSize,
	VoxelSaveEncoding encoding, uint8_t bits,
	const std::vector<HRL_VoxelType>& palette,
	int chunkW, int chunkH,
	std::vector<HRL_VoxelType>& destination)
{
	const size_t voxelCount = static_cast<size_t>(chunkW) * static_cast<size_t>(chunkH);
	destination.assign(voxelCount, 0);
	if (encoding == VoxelSaveEncoding::Constant)
	{
		if (palette.size() != 1 || payloadSize != 0) return false;
		std::fill(destination.begin(), destination.end(), palette[0]);
		return true;
	}
	if (encoding == VoxelSaveEncoding::Raw)
	{
		if (!palette.empty() || payloadSize != voxelCount) return false;
		std::memcpy(destination.data(), payload, voxelCount);
		return true;
	}
	if (encoding == VoxelSaveEncoding::Palette)
	{
		if (palette.empty() || palette.size() > 255u || bits == 0 || bits > 8) return false;
		const size_t expectedBytes = (voxelCount * static_cast<size_t>(bits) + 7u) / 8u;
		if (payloadSize != expectedBytes) return false;
		uint32_t accumulator = 0;
		unsigned accumulatorBits = 0;
		size_t input = 0;
		for (size_t i = 0; i < voxelCount; ++i)
		{
			while (accumulatorBits < bits)
			{
				if (input >= payloadSize) return false;
				accumulator |= static_cast<uint32_t>(payload[input++]) << accumulatorBits;
				accumulatorBits += 8u;
			}
			const uint32_t index = accumulator & ((1u << bits) - 1u);
			accumulator >>= bits;
			accumulatorBits -= bits;
			if (index >= palette.size()) return false;
			destination[i] = palette[index];
		}
		return true;
	}
	if (encoding == VoxelSaveEncoding::Rle)
	{
		VoxelSaveReader reader(payload, payloadSize);
		size_t dst = 0;
		for (int y = 0; y < chunkH; ++y)
		{
			size_t row = 0;
			while (row < static_cast<size_t>(chunkW))
			{
				uint8_t type = 0;
				uint32_t run = 0;
				if (!reader.ReadU8(type) || !reader.ReadVarUInt(run) || run == 0)
					return false;
				if (run > static_cast<uint32_t>(chunkW) - row)
					return false;
				std::fill(destination.begin() + static_cast<std::ptrdiff_t>(dst),
					destination.begin() + static_cast<std::ptrdiff_t>(dst + run), type);
				row += run;
				dst += run;
			}
		}
		return reader.Remaining() == 0;
	}
	return false;
}
 }

bool HRL_EnsureVoxelChunkLoaded(HRL_VoxelWorld* world, int chunkX, int chunkY)
{
	if (!world || chunkX < 0 || chunkY < 0)
		return false;
	if (world->FindVoxelChunk(chunkX, chunkY))
		return true;

	const uint64_t key = HRL_VoxelWorld::MakeVoxelChunkKey(chunkX, chunkY);
	auto it = world->serialized_chunks_.find(key);
	if (it == world->serialized_chunks_.end() || it->second.deleted)
		return false;
	const auto& rec = it->second;
	if (rec.payload_offset > world->serialized_source_.size() ||
		rec.payload_size > world->serialized_source_.size() - rec.payload_offset)
		return false;

	const int minX = chunkX * world->chunk_size_;
	const int minY = chunkY * world->chunk_size_;
	const int chunkW = std::min(world->chunk_size_, world->width_ - minX);
	const int chunkH = std::min(world->chunk_size_, world->height_ - minY);
	if (chunkW <= 0 || chunkH <= 0)
		return false;

	std::vector<HRL_VoxelType> decoded;
	if (!VoxelSaveDecodeChunk(world->serialized_source_.data() + rec.payload_offset,
		rec.payload_size, static_cast<VoxelSaveEncoding>(rec.encoding), rec.bits,
		rec.palette, chunkW, chunkH, decoded))
		return false;

	bool nonEmpty = false;
	for (HRL_VoxelType type : decoded)
	{
		if (type != 0) { nonEmpty = true; break; }
	}
	if (!nonEmpty)
		return false;

	auto& chunk = world->EnsureVoxelChunk(chunkX, chunkY);
	for (size_t i = 0; i < decoded.size(); ++i)
		chunk[i].type = decoded[i];
	return true;
}

void HRL_SetVoxelSize(HRL_id _sceneid, int _width, int _height)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelSize: invalid scene ID");
		return;
	}

	if (_width <= 0 || _height <= 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetVoxelSize: width and height must be positive");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	world->width_ = _width;
	world->height_ = _height;
	world->voxel_chunks_.clear();
	world->serialized_source_.clear();
	world->serialized_chunks_.clear();
	world->dirty_chunks_.clear();
	world->render_dirty_chunks_.clear();
	++world->geometry_revision_;
	++world->voxel_revision_;
}

int HRL_CreateVoxelWorld(HRL_id _sceneid, int _width, int _height)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateVoxelWorld: invalid scene ID");
		return HRL_FALSE;
	}

	if (_width <= 0 || _height <= 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_CreateVoxelWorld: width and height must be positive");
		return HRL_FALSE;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;

	world->width_ = _width;
	world->height_ = _height;
	world->voxel_chunks_.clear();
	world->serialized_source_.clear();
	world->serialized_chunks_.clear();
	world->dirty_chunks_.clear();
	world->render_dirty_chunks_.clear();
	++world->geometry_revision_;
	++world->voxel_revision_;

	return HRL_TRUE;
}

void HRL_SetVoxelPhysicalSize(HRL_id _sceneid, float _size)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelPhysicalSize: invalid scene ID");
		return;
	}
	if (!std::isfinite(_size) || _size <= 0.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVoxelPhysicalSize: size must be finite and greater than zero");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	world->voxel_size_ = _size;
	++world->geometry_revision_;
	++world->voxel_revision_;
}

int HRL_WorldToVoxelCoordinates(
	HRL_id _sceneid,
	float _world_x, float _world_y,
	float* _voxel_x, float* _voxel_y)
{
	if (_voxel_x) *_voxel_x = 0.f;
	if (_voxel_y) *_voxel_y = 0.f;

	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_WorldToVoxelCoordinates: invalid scene ID");
		return HRL_FALSE;
	}

	const HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || !std::isfinite(world->voxel_size_) || world->voxel_size_ <= 0.f)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_WorldToVoxelCoordinates: voxel physical size is not configured");
		return HRL_FALSE;
	}
	if (!_voxel_x || !_voxel_y)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_WorldToVoxelCoordinates: output pointer is null");
		return HRL_FALSE;
	}
	if (!std::isfinite(_world_x) || !std::isfinite(_world_y))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_WorldToVoxelCoordinates: input coordinates must be finite");
		return HRL_FALSE;
	}

	const float invVoxelSize = 1.f / world->voxel_size_;
	*_voxel_x = _world_x * invVoxelSize;
	*_voxel_y = _world_y * invVoxelSize;
	return HRL_TRUE;
}

int HRL_VoxelToWorldCoordinates(
	HRL_id _sceneid,
	float _voxel_x, float _voxel_y,
	float* _world_x, float* _world_y)
{
	if (_world_x) *_world_x = 0.f;
	if (_world_y) *_world_y = 0.f;

	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_VoxelToWorldCoordinates: invalid scene ID");
		return HRL_FALSE;
	}

	const HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || !std::isfinite(world->voxel_size_) || world->voxel_size_ <= 0.f)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_VoxelToWorldCoordinates: voxel physical size is not configured");
		return HRL_FALSE;
	}
	if (!_world_x || !_world_y)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_VoxelToWorldCoordinates: output pointer is null");
		return HRL_FALSE;
	}
	if (!std::isfinite(_voxel_x) || !std::isfinite(_voxel_y))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_VoxelToWorldCoordinates: input coordinates must be finite");
		return HRL_FALSE;
	}

	*_world_x = _voxel_x * world->voxel_size_;
	*_world_y = _voxel_y * world->voxel_size_;
	return HRL_TRUE;
}

void HRL_SetVoxelChunkSize(HRL_id _sceneid, int _chunkSize)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelChunkSize: invalid scene ID");
		return;
	}
	if (_chunkSize <= 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVoxelChunkSize: chunk size must be greater than zero");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	if (world->chunk_size_ == _chunkSize)
		return;
	const int oldChunkSize = world->chunk_size_;
	auto oldChunks = std::move(world->voxel_chunks_);
	world->voxel_chunks_.clear();
	world->serialized_source_.clear();
	world->serialized_chunks_.clear();
	world->chunk_size_ = _chunkSize;

	// Re-bucket the sparse storage when the chunk size changes. This is an
	// explicit configuration operation, so paying the cost once here is much
	// better than keeping a dense world just to make this case cheap.
	const int oldChunkColumns = (world->width_ + oldChunkSize - 1) / oldChunkSize;
	for (const auto& [key, oldChunk] : oldChunks)
	{
		const int oldChunkX = static_cast<int32_t>(key >> 32u);
		const int oldChunkY = static_cast<int32_t>(key & 0xffffffffu);
		const int oldMinX = oldChunkX * oldChunkSize;
		const int oldMinY = oldChunkY * oldChunkSize;
		const int oldChunkW = std::min(oldChunkSize, world->width_ - oldMinX);
		const int oldChunkH = std::min(oldChunkSize, world->height_ - oldMinY);
		if (oldChunkX < 0 || oldChunkY < 0 || oldChunkX >= oldChunkColumns ||
			oldChunkW <= 0 || oldChunkH <= 0)
			continue;

		for (int y = 0; y < oldChunkH; ++y)
		{
			for (int x = 0; x < oldChunkW; ++x)
			{
				const HRL_VoxelType type =
					oldChunk[static_cast<size_t>(y) * static_cast<size_t>(oldChunkW) +
					static_cast<size_t>(x)].type;
				if (type == 0)
					continue;
				*world->GetVoxelTypeMutable(oldMinX + x, oldMinY + y) = type;
			}
		}
	}
	world->dirty_chunks_.clear();
	world->render_dirty_chunks_.clear();
	++world->geometry_revision_;
	++world->voxel_revision_;
}

void HRL_SetVoxelTypeColor(HRL_id _sceneid, uint32_t _type, float _r, float _g, float _b, float _a)
{
	if (_type > HRL_VOXEL_TYPE_MAX)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_SetVoxelTypeColor: voxel type must be in range 0..255");
		return;
	}
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelTypeColor: invalid scene ID");
		return;
	}
	if (!std::isfinite(_r) || !std::isfinite(_g) || !std::isfinite(_b) || !std::isfinite(_a))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVoxelTypeColor: color contains NaN or infinity");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	// Base color is display color only. Values above 1 are intentionally ignored
	// instead of implicitly turning the voxel into an emitter. Emission is
	// configured independently with HRL_SetVoxelTypeEmissiveColor().
	world->type_colors_[_type] = glm::vec4(
		glm::clamp(_r, 0.0f, 1.0f),
		glm::clamp(_g, 0.0f, 1.0f),
		glm::clamp(_b, 0.0f, 1.0f),
		glm::clamp(_a, 0.0f, 1.0f)
	);
	++world->color_revision_;
}

void HRL_SetVoxelTypeEmissiveColor(HRL_id _sceneid, uint32_t _type, float _r, float _g, float _b)
{
	if (_type > HRL_VOXEL_TYPE_MAX)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_SetVoxelTypeEmissiveColor: voxel type must be in range 0..255");
		return;
	}
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelTypeEmissiveColor: invalid scene ID");
		return;
	}
	if (!std::isfinite(_r) || !std::isfinite(_g) || !std::isfinite(_b))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVoxelTypeEmissiveColor: color contains NaN or infinity");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	const glm::vec3 emissive(
		std::max(_r, 0.0f),
		std::max(_g, 0.0f),
		std::max(_b, 0.0f)
	);

	if (std::max(emissive.r, std::max(emissive.g, emissive.b)) <= 0.0f)
		world->type_emissive_colors_.erase(_type);
	else
		world->type_emissive_colors_[_type] = emissive;

	// Emissive geometry and global emitter lists are rebuilt with the same
	// revision used by the baked voxel colors.
	++world->color_revision_;
}

int HRL_LoadVoxelWorld(HRL_id _sceneid, const HRL_Voxel* _voxels, size_t _count)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorld: invalid scene ID");
		return HRL_FALSE;
	}

	HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || world->width_ <= 0 || world->height_ <= 0)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_LoadVoxelWorld: call HRL_SetVoxelSize before loading voxel data");
		return HRL_FALSE;
	}

	size_t expectedCount = 0;
	if (!VoxelWorldElementCount(world->width_, world->height_, expectedCount) || expectedCount != _count)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_LoadVoxelWorld: count must equal voxel world width * height");
		return HRL_FALSE;
	}
	if (expectedCount > 0 && !_voxels)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_LoadVoxelWorld: voxel array is null");
		return HRL_FALSE;
	}

	world->voxel_chunks_.clear();
	world->serialized_source_.clear();
	world->serialized_chunks_.clear();

	const int chunkSize = world->chunk_size_;
	const int chunkColumns = (world->width_ + chunkSize - 1) / chunkSize;
	const int chunkRows = (world->height_ + chunkSize - 1) / chunkSize;

	// Convert the caller's dense array into the sparse chunk representation.
	for (int chunkY = 0; chunkY < chunkRows; ++chunkY)
	{
		for (int chunkX = 0; chunkX < chunkColumns; ++chunkX)
		{
			const int minX = chunkX * chunkSize;
			const int minY = chunkY * chunkSize;
			const int chunkW = std::min(chunkSize, world->width_ - minX);
			const int chunkH = std::min(chunkSize, world->height_ - minY);

			bool nonEmpty = false;
			for (int y = 0; y < chunkH && !nonEmpty; ++y)
			{
				const size_t row = static_cast<size_t>(minY + y) * static_cast<size_t>(world->width_);
				for (int x = 0; x < chunkW; ++x)
				{
					if (_voxels[row + static_cast<size_t>(minX + x)].type != 0)
					{
						nonEmpty = true;
						break;
					}
				}
			}
			if (!nonEmpty)
				continue;

			auto& chunk = world->EnsureVoxelChunk(chunkX, chunkY);
			for (int y = 0; y < chunkH; ++y)
			{
				const size_t src = static_cast<size_t>(minY + y) * static_cast<size_t>(world->width_) +
					static_cast<size_t>(minX);
				const size_t dst = static_cast<size_t>(y) * static_cast<size_t>(chunkW);
				std::memcpy(chunk.data() + dst, _voxels + src, static_cast<size_t>(chunkW) * sizeof(HRL_Voxel));
			}
		}
	}

	world->dirty_chunks_.clear();
	world->render_dirty_chunks_.clear();
	++world->geometry_revision_;
	++world->voxel_revision_;
	return HRL_TRUE;
}

size_t HRL_GetVoxelWorldSaveAllSize(HRL_id _sceneid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetVoxelWorldSaveAllSize: invalid scene ID");
		return 0;
	}
	const HRL_VoxelWorld* world = it->second->voxel_world;
	std::vector<uint8_t> data;
	if (!VoxelSaveBuildAll(world, data))
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_GetVoxelWorldSaveAllSize: voxel world is not valid for serialization");
		return 0;
	}
	return data.size();
}

size_t HRL_SaveVoxelWorldAll(HRL_id _sceneid, void* _buffer, size_t _capacity)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SaveVoxelWorldAll: invalid scene ID");
		return 0;
	}
	const HRL_VoxelWorld* world = it->second->voxel_world;
	std::vector<uint8_t> data;
	if (!VoxelSaveBuildAll(world, data))
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_SaveVoxelWorldAll: voxel world is not valid for serialization");
		return 0;
	}
	if (!_buffer || _capacity < data.size())
		return data.size();
	std::memcpy(_buffer, data.data(), data.size());
	return data.size();
}

int HRL_LoadVoxelWorldBuffer(HRL_id _sceneid, const void* _buffer, size_t _size)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid scene ID");
		return HRL_FALSE;
	}
	if (!_buffer || _size < kVoxelWorldSaveHeaderSize)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid or empty buffer");
		return HRL_FALSE;
	}

	const auto* bytes = static_cast<const uint8_t*>(_buffer);
	VoxelSaveReader reader(bytes, _size);
	uint32_t magic = 0, width = 0, height = 0, chunkSize = 0, chunkCount = 0;
	uint16_t version = 0, flags = 0;
	float voxelSize = 0.f;
	if (!reader.ReadU32(magic) || !reader.ReadU16(version) || !reader.ReadU16(flags) ||
		!reader.ReadU32(width) || !reader.ReadU32(height) || !reader.ReadF32(voxelSize) ||
		!reader.ReadU32(chunkSize) || !reader.ReadU32(chunkCount))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: truncated header");
		return HRL_FALSE;
	}
	if (magic != kVoxelWorldSaveMagic || version != kVoxelWorldSaveVersion || flags != 0u)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: unsupported voxel world format");
		return HRL_FALSE;
	}
	if (width == 0 || height == 0 || chunkSize == 0 || !std::isfinite(voxelSize) || voxelSize <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid world dimensions or voxel size");
		return HRL_FALSE;
	}
	if (width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
		height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
		chunkSize > static_cast<uint32_t>(std::numeric_limits<int>::max()))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: world dimensions exceed HRL limits");
		return HRL_FALSE;
	}

	const uint64_t chunkColumns = (static_cast<uint64_t>(width) + chunkSize - 1u) / chunkSize;
	const uint64_t chunkRows = (static_cast<uint64_t>(height) + chunkSize - 1u) / chunkSize;
	const uint64_t totalChunks = chunkColumns * chunkRows;
	if (totalChunks == 0 || totalChunks > std::numeric_limits<uint32_t>::max() || chunkCount > totalChunks)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid chunk count");
		return HRL_FALSE;
	}

	// Keep the compressed source around.  The initial load only parses record
	// metadata; voxel payloads are decoded lazily by the renderer when a chunk
	// enters the visible streaming set.
	HRL_VoxelWorld* newWorld = new HRL_VoxelWorld();
	newWorld->width_ = static_cast<int>(width);
	newWorld->height_ = static_cast<int>(height);
	newWorld->voxel_size_ = voxelSize;
	newWorld->chunk_size_ = static_cast<int>(chunkSize);
	newWorld->serialized_source_.assign(bytes, bytes + _size);

	auto readRecord = [&](VoxelSaveReader& r, bool allowDelete, bool& ok) -> uint64_t
	{
		ok = false;
		uint32_t chunkIndex = 0;
		uint8_t encodingByte = 0, bits = 0, paletteCount = 0, reserved = 0;
		uint32_t payloadSize = 0;
		if (!r.ReadU32(chunkIndex) || !r.ReadU8(encodingByte) || !r.ReadU8(bits) ||
			!r.ReadU8(paletteCount) || !r.ReadU8(reserved) || !r.ReadU32(payloadSize))
			return 0;
		if (reserved != 0u || chunkIndex >= totalChunks)
			return 0;
		if (encodingByte == kVoxelWorldDeltaDeleteEncoding)
		{
			if (!allowDelete || bits != 0 || paletteCount != 0 || payloadSize != 0)
				return 0;
		}
		else if (encodingByte > static_cast<uint8_t>(VoxelSaveEncoding::Rle))
			return 0;

		std::vector<uint8_t> palette(paletteCount);
		for (uint8_t& value : palette)
			if (!r.ReadU8(value)) return 0;
		const size_t payloadOffset = r.Position();
		const uint8_t* payload = nullptr;
		if (!r.ReadBytes(payload, payloadSize)) return 0;
		(void)payload;

		const uint32_t chunkX = chunkIndex % static_cast<uint32_t>(chunkColumns);
		const uint32_t chunkY = chunkIndex / static_cast<uint32_t>(chunkColumns);
		const int chunkW = std::min(static_cast<int>(chunkSize), static_cast<int>(width) - static_cast<int>(chunkX * chunkSize));
		const int chunkH = std::min(static_cast<int>(chunkSize), static_cast<int>(height) - static_cast<int>(chunkY * chunkSize));
		if (chunkW <= 0 || chunkH <= 0) return 0;
		if (encodingByte == static_cast<uint8_t>(VoxelSaveEncoding::Constant) &&
			(palette.size() != 1 || payloadSize != 0)) return 0;
		if (encodingByte == static_cast<uint8_t>(VoxelSaveEncoding::Raw) &&
			(!palette.empty() || payloadSize != static_cast<size_t>(chunkW) * static_cast<size_t>(chunkH))) return 0;
		if (encodingByte == static_cast<uint8_t>(VoxelSaveEncoding::Palette) &&
			(palette.empty() || bits == 0 || bits > 8 ||
			 payloadSize != (static_cast<size_t>(chunkW) * static_cast<size_t>(chunkH) * bits + 7u) / 8u)) return 0;

		const uint64_t key = HRL_VoxelWorld::MakeVoxelChunkKey(static_cast<int>(chunkX), static_cast<int>(chunkY));
		HRL_VoxelWorld::SerializedChunkRecord rec;
		rec.encoding = encodingByte;
		rec.bits = bits;
		rec.palette = std::move(palette);
		rec.payload_offset = payloadOffset;
		rec.payload_size = payloadSize;
		rec.deleted = encodingByte == kVoxelWorldDeltaDeleteEncoding;
		newWorld->serialized_chunks_[key] = std::move(rec);
		ok = true;
		return key;
	};

	for (uint32_t record = 0; record < chunkCount; ++record)
	{
		bool ok = false;
		readRecord(reader, false, ok);
		if (!ok)
		{
			delete newWorld;
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid chunk record");
			return HRL_FALSE;
		}
	}

	// A save to an existing world file is append-only: each delta contains only
	// chunks changed since the previous save. The last record for a chunk wins.
	while (reader.Remaining() != 0)
	{
		uint32_t deltaMagic = 0, deltaCount = 0;
		uint16_t deltaVersion = 0, deltaFlags = 0;
		if (!reader.ReadU32(deltaMagic) || !reader.ReadU16(deltaVersion) || !reader.ReadU16(deltaFlags) ||
			!reader.ReadU32(deltaCount) || deltaMagic != kVoxelWorldDeltaMagic ||
			deltaVersion != kVoxelWorldDeltaVersion || deltaFlags != 0u || deltaCount > totalChunks)
		{
			delete newWorld;
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid voxel delta section");
			return HRL_FALSE;
		}
		for (uint32_t record = 0; record < deltaCount; ++record)
		{
			bool ok = false;
			readRecord(reader, true, ok);
			if (!ok)
			{
				delete newWorld;
				SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid voxel delta record");
				return HRL_FALSE;
			}
		}
	}

	HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world)
		world = new HRL_VoxelWorld();
	const auto colors = std::move(world->type_colors_);
	const auto collisionFlags = std::move(world->type_collision_flags_);
	const auto emissiveColors = std::move(world->type_emissive_colors_);
	const uint64_t geometryRevision = world->geometry_revision_;
	const uint64_t voxelRevision = world->voxel_revision_;
	const uint64_t colorRevision = world->color_revision_;
	newWorld->type_colors_ = colors;
	newWorld->type_collision_flags_ = collisionFlags;
	newWorld->type_emissive_colors_ = emissiveColors;
	newWorld->geometry_revision_ = geometryRevision + 1;
	newWorld->voxel_revision_ = voxelRevision + 1;
	newWorld->color_revision_ = colorRevision;
	newWorld->dirty_chunks_.clear();
	newWorld->render_dirty_chunks_.clear();
	it->second->voxel_world = newWorld;
	delete world;
	return HRL_TRUE;
}


int HRL_SaveVoxelWorldAllFile(HRL_id _sceneid, const char* _path)
{
	if (!_path || _path[0] == '\0')
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SaveVoxelWorldAllFile: file path is null or empty");
		return HRL_FALSE;
	}

	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SaveVoxelWorldAllFile: invalid scene ID");
		return HRL_FALSE;
	}

	HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || world->width_ <= 0 || world->height_ <= 0 ||
		world->chunk_size_ <= 0 || !std::isfinite(world->voxel_size_) ||
		world->voxel_size_ <= 0.0f)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_SaveVoxelWorldAllFile: voxel world is not valid for serialization");
		return HRL_FALSE;
	}

	/*
	 * Always write a complete HLV1 snapshot.
	 *
	 * The previous implementation appended HDP1 delta records when the
	 * destination already contained a matching world. That makes the file
	 * depend on the complete history of lazy chunks and dirty-chunk state.
	 * For a full-world save this is unnecessary and, more importantly, makes
	 * it very easy for a lazy chunk to be missing from the resulting snapshot.
	 *
	 * Before building the snapshot, materialize every chunk that still exists
	 * only in serialized_chunks_. This guarantees that VoxelSaveBuildAll sees
	 * the complete current world.
	 */
	for (const auto& [key, record] : world->serialized_chunks_)
	{
		if (record.deleted)
			continue;

		const int chunkX = static_cast<int32_t>(key >> 32u);
		const int chunkY = static_cast<int32_t>(key & 0xffffffffu);

		HRL_EnsureVoxelChunkLoaded(world, chunkX, chunkY);
	}

	std::vector<uint8_t> data;
	if (!VoxelSaveBuildAll(world, data))
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_SaveVoxelWorldAllFile: voxel world is not valid for serialization");
		return HRL_FALSE;
	}

	FILE* file = std::fopen(_path, "wb");
	if (!file)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SaveVoxelWorldAllFile: could not open destination file");
		return HRL_FALSE;
	}

	const bool wroteAll =
		data.empty() || std::fwrite(data.data(), 1, data.size(), file) == data.size();

	const bool closed = std::fclose(file) == 0;

	if (!wroteAll || !closed)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SaveVoxelWorldAllFile: file write failed");
		return HRL_FALSE;
	}

	world->dirty_chunks_.clear();
	return HRL_TRUE;
}

void HRL_BeginVoxelEdit(HRL_id _sceneid)
{
	HRL_VoxelWorld* world = FindVoxelWorld(_sceneid);
	if (!world)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_BeginVoxelEdit: invalid scene or voxel world");
		return;
	}
	++world->voxel_edit_depth_;
}

void HRL_EndVoxelEdit(HRL_id _sceneid)
{
	HRL_VoxelWorld* world = FindVoxelWorld(_sceneid);
	if (!world)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_EndVoxelEdit: invalid scene or voxel world");
		return;
	}
	if (world->voxel_edit_depth_ == 0)
		return;

	--world->voxel_edit_depth_;
	if (world->voxel_edit_depth_ == 0 && world->voxel_edit_dirty_)
	{
		++world->voxel_revision_;
		world->voxel_edit_dirty_ = false;
	}
}

void HRL_SetVoxelType(HRL_id _sceneid, int _pos_x, int _pos_y, uint32_t _type)
{
	if (_type > HRL_VOXEL_TYPE_MAX)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_SetVoxelType: voxel type must be in range 0..255");
		return;
	}

	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelType: invalid scene ID");
		return;
	}

	HRL_VoxelWorld* world = FindVoxelWorld(_sceneid);
	if (!world || world->width_ <= 0 || world->height_ <= 0)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SetVoxelType: voxel world is not loaded");
		return;
	}

	if (_pos_x < 0 || _pos_y < 0 || _pos_x >= world->width_ || _pos_y >= world->height_)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_SetVoxelType: position is outside the voxel world");
		return;
	}

	const HRL_VoxelType requested = static_cast<HRL_VoxelType>(_type);

	const int chunkX = _pos_x / world->chunk_size_;
	const int chunkY = _pos_y / world->chunk_size_;
	const uint64_t key = HRL_VoxelWorld::MakeVoxelChunkKey(chunkX, chunkY);

	// A loaded world keeps off-screen chunks in serialized_chunks_. Before any
	// write, materialize the target chunk so editing it never replaces an
	// existing lazy chunk with a newly-created empty one.
	if (!world->FindVoxelChunk(chunkX, chunkY))
		HRL_EnsureVoxelChunkLoaded(world, chunkX, chunkY);

	const HRL_VoxelType old = world->GetVoxelType(_pos_x, _pos_y);
	if (old == requested)
		return;

	// Always edit the materialized chunk directly, including when setting a
	// voxel to 0. This changes only the requested voxel.
	HRL_VoxelType* voxel = world->GetVoxelTypeMutable(_pos_x, _pos_y);
	if (!voxel)
		return;

	*voxel = requested;

	if (requested == 0)
	{
		auto* chunk = world->FindVoxelChunk(chunkX, chunkY);
		if (chunk)
		{
			bool anyNonEmpty = false;
			for (const HRL_Voxel& current : *chunk)
			{
				if (current.type != 0)
				{
					anyNonEmpty = true;
					break;
				}
			}

			if (!anyNonEmpty)
			{
				// Keep the empty chunk in voxel_chunks_. This is important:
				// removing it would make GetVoxelType/EnsureVoxelChunkLoaded
				// able to fall back to the old serialized chunk.
				auto serializedIt = world->serialized_chunks_.find(key);
				if (serializedIt != world->serialized_chunks_.end())
					serializedIt->second.deleted = true;
			}
		}
	}
	else
	{
		// The current in-memory chunk is authoritative after an edit.
		// Its old serialized record must not be used as a fallback.
		auto serializedIt = world->serialized_chunks_.find(key);
		if (serializedIt != world->serialized_chunks_.end())
			serializedIt->second.deleted = true;
	}

	MarkVoxelChunkDirty(world, _pos_x, _pos_y);
}

uint32_t HRL_GetVoxelType(HRL_id _sceneid, int _pos_x, int _pos_y)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetVoxelType: invalid scene ID");
		return 0;
	}

	HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || world->width_ <= 0 || world->height_ <= 0)
		return 0;

	if (_pos_x < 0 || _pos_y < 0 || _pos_x >= world->width_ || _pos_y >= world->height_)
		return 0;

	HRL_EnsureVoxelChunkLoaded(world, _pos_x / world->chunk_size_, _pos_y / world->chunk_size_);
	return world->GetVoxelType(_pos_x, _pos_y);
}

void HRL_SetVoxelTypeCollisionFlags(HRL_id _sceneid, uint32_t _type, uint32_t _flags)
{
	if (_type > HRL_VOXEL_TYPE_MAX)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_SetVoxelTypeCollisionFlags: voxel type must be in range 0..255");
		return;
	}
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVoxelTypeCollisionFlags: invalid scene ID");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	if (_flags == 0u)
		world->type_collision_flags_.erase(_type);
	else
		world->type_collision_flags_[_type] = _flags;
}

uint32_t HRL_GetVoxelTypeCollisionFlags(HRL_id _sceneid, uint32_t _type)
{
	if (_type > HRL_VOXEL_TYPE_MAX)
		return 0u;
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_GetVoxelTypeCollisionFlags: invalid scene ID");
		return 0u;
	}

	const HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world)
		return 0u;

	auto flagsIt = world->type_collision_flags_.find(_type);
	return flagsIt != world->type_collision_flags_.end() ? flagsIt->second : 0u;
}

namespace
{
	static uint32_t HRL_GetVoxelCollisionFlags(const HRL_VoxelWorld* _world, HRL_VoxelType _type)
	{
		if (!_world || _type == 0)
			return 0u;
		auto it = _world->type_collision_flags_.find(_type);
		return it != _world->type_collision_flags_.end() ? it->second : 0u;
	}

	static void HRL_RecordVoxelCollisionType(
		uint32_t& _slot, float& _bestMetric, uint32_t _type, float _metric)
	{
		if (_type == 0)
			return;
		if (_slot == 0 || _metric < _bestMetric)
		{
			_slot = _type;
			_bestMetric = _metric;
		}
	}
}

int HRL_VoxelCheckCollision(
	HRL_id _sceneid,
	float _x, float _y,
	float _width, float _height,
	uint32_t _mask,
	HRL_VoxelCollision* _out_collision)
{
	if (_out_collision)
	{
		*_out_collision = HRL_VoxelCollision{};
	}

	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_VoxelCheckCollision: invalid scene ID");
		return HRL_FALSE;
	}

	const HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || world->width_ <= 0 || world->height_ <= 0)
		return HRL_FALSE;
	if (!std::isfinite(_x) || !std::isfinite(_y) ||
		!std::isfinite(_width) || !std::isfinite(_height) ||
		_width <= 0.f || _height <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_VoxelCheckCollision: position and size must be finite; size must be greater than zero");
		return HRL_FALSE;
	}
	if (!_out_collision)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_VoxelCheckCollision: output collision pointer is null");
		return HRL_FALSE;
	}

	// Coordinates are expressed in voxel-space units, independent of the
	// physical render size of a voxel. The shape is centered on (_x, _y).
	const float halfWidth = _width * 0.5f;
	const float halfHeight = _height * 0.5f;
	const float minX = _x - halfWidth;
	const float maxX = _x + halfWidth;
	const float minY = _y - halfHeight;
	const float maxY = _y + halfHeight;

	constexpr float kEpsilon = 1e-5f;

	// Expand the integer search range by one epsilon so exact face contact is
	// reported as a collision rather than being lost to floor/ceil rounding.
	int minVX = static_cast<int>(std::floor(minX - kEpsilon));
	int maxVX = static_cast<int>(std::floor(maxX + kEpsilon));
	int minVY = static_cast<int>(std::floor(minY - kEpsilon));
	int maxVY = static_cast<int>(std::floor(maxY + kEpsilon));

	minVX = std::max(0, minVX);
	minVY = std::max(0, minVY);
	maxVX = std::min(world->width_ - 1, maxVX);
	maxVY = std::min(world->height_ - 1, maxVY);
	if (minVX > maxVX || minVY > maxVY)
		return HRL_FALSE;

	float bestLeft = std::numeric_limits<float>::max();
	float bestRight = std::numeric_limits<float>::max();
	float bestTop = std::numeric_limits<float>::max();
	float bestBottom = std::numeric_limits<float>::max();
	float bestInside = std::numeric_limits<float>::max();

	for (int vy = minVY; vy <= maxVY; ++vy)
	{
		for (int vx = minVX; vx <= maxVX; ++vx)
		{
			const HRL_VoxelType type = world->GetVoxelType(vx, vy);
			const uint32_t voxelFlags = HRL_GetVoxelCollisionFlags(world, type);
			if (_mask == 0u || (voxelFlags & _mask) == 0u)
				continue;

			const float voxelMinX = static_cast<float>(vx);
			const float voxelMaxX = voxelMinX + 1.f;
			const float voxelMinY = static_cast<float>(vy);
			const float voxelMaxY = voxelMinY + 1.f;

			const float overlapX = std::min(maxX, voxelMaxX) - std::max(minX, voxelMinX);
			const float overlapY = std::min(maxY, voxelMaxY) - std::max(minY, voxelMinY);
			if (overlapX < -kEpsilon || overlapY < -kEpsilon)
				continue;

			_out_collision->flags |= HRL_VOXEL_COLLISION_NONE;

			const bool containsShape =
				voxelMinX <= minX + kEpsilon && voxelMaxX >= maxX - kEpsilon &&
				voxelMinY <= minY + kEpsilon && voxelMaxY >= maxY - kEpsilon;
			if (containsShape)
			{
				_out_collision->flags |= HRL_VOXEL_COLLISION_INSIDE;
				HRL_RecordVoxelCollisionType(_out_collision->inside_type, bestInside, type,
					std::max(0.f, std::min(overlapX, overlapY)));
				continue;
			}

			// Exact corner contact: report both axes. For real penetration, use
			// the smaller overlap as the most plausible contact axis.
			if (overlapX <= kEpsilon && overlapY <= kEpsilon)
			{
				const float centerX = voxelMinX + 0.5f;
				const float centerY = voxelMinY + 0.5f;
				if (centerX < _x)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_LEFT;
					HRL_RecordVoxelCollisionType(_out_collision->left_type, bestLeft, type, 0.f);
				}
				else if (centerX > _x)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_RIGHT;
					HRL_RecordVoxelCollisionType(_out_collision->right_type, bestRight, type, 0.f);
				}
				if (centerY < _y)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_BOTTOM;
					HRL_RecordVoxelCollisionType(_out_collision->bottom_type, bestBottom, type, 0.f);
				}
				else if (centerY > _y)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_TOP;
					HRL_RecordVoxelCollisionType(_out_collision->top_type, bestTop, type, 0.f);
				}
				continue;
			}

			const float centerX = voxelMinX + 0.5f;
			const float centerY = voxelMinY + 0.5f;
			if (overlapX <= overlapY)
			{
				if (centerX < _x)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_LEFT;
					HRL_RecordVoxelCollisionType(_out_collision->left_type, bestLeft, type, overlapX);
				}
				else
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_RIGHT;
					HRL_RecordVoxelCollisionType(_out_collision->right_type, bestRight, type, overlapX);
				}
			}
			else
			{
				if (centerY < _y)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_BOTTOM;
					HRL_RecordVoxelCollisionType(_out_collision->bottom_type, bestBottom, type, overlapY);
				}
				else
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_TOP;
					HRL_RecordVoxelCollisionType(_out_collision->top_type, bestTop, type, overlapY);
				}
			}
		}
	}

	return _out_collision->flags != HRL_VOXEL_COLLISION_NONE ? HRL_TRUE : HRL_FALSE;
}

namespace {

static bool HRL_GetVoxelScreenRay(const HRL_Viewport* viewport, float mouseX, float mouseY,
    glm::vec3& origin, glm::vec3& direction)
{
    if (!viewport || !viewport->camera_)
        return false;

    const float winW = static_cast<float>(GetWindowWidth());
    const float winH = static_cast<float>(GetWindowHeight());
    if (winW <= 0.f || winH <= 0.f)
        return false;

    // HRL mouse coordinates are top-left origin. OpenGL viewports use a
    // bottom-left origin, so convert Y before testing the viewport and building
    // NDC coordinates.
    const float framebufferY = winH - mouseY;
    const float viewportX = viewport->x_ * winW;
    const float viewportY = viewport->y_ * winH;
    const float viewportW = std::max(1.f, viewport->width_ * winW);
    const float viewportH = std::max(1.f, viewport->height_ * winH);

    if (mouseX < viewportX || mouseX >= viewportX + viewportW ||
        framebufferY < viewportY || framebufferY >= viewportY + viewportH)
        return false;

    const float u = (mouseX - viewportX) / viewportW;
    const float v = (framebufferY - viewportY) / viewportH;
    const float ndcX = u * 2.f - 1.f;
    const float ndcY = v * 2.f - 1.f;

    const HRL_Camera* camera = viewport->camera_;
    const float aspect = viewportW / viewportH;

    glm::mat4 projection(1.f);
    if (camera->type_ == HRL_PERSPECTIVE)
    {
        projection = glm::perspective(
            glm::radians(camera->value_), aspect, camera->near_plane_, camera->far_plane_);
    }
    else
    {
        const float halfHeight = camera->value_ * 0.5f;
        const float halfWidth = halfHeight * aspect;
        projection = glm::ortho(
            -halfWidth, halfWidth, -halfHeight, halfHeight,
            camera->near_plane_, camera->far_plane_);
    }

    const glm::mat4 view = glm::lookAt(
        camera->position_,
        camera->position_ + GetForwardVector(camera->rotation_),
        GetUpVector(camera->rotation_));

    const glm::mat4 inverseVP = glm::inverse(projection * view);
    glm::vec4 nearPoint = inverseVP * glm::vec4(ndcX, ndcY, -1.f, 1.f);
    glm::vec4 farPoint  = inverseVP * glm::vec4(ndcX, ndcY,  1.f, 1.f);

    if (std::abs(nearPoint.w) < 1e-6f || std::abs(farPoint.w) < 1e-6f)
        return false;

    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    origin = glm::vec3(nearPoint);
    direction = glm::vec3(farPoint - nearPoint);
    const float directionLength = glm::length(direction);
    if (directionLength <= 1e-6f || !std::isfinite(directionLength))
        return false;
    direction /= directionLength;

    return true;
}

} // namespace

int HRL_GetVoxelAtScreenPosition(HRL_id _sceneid, int _loc_x, int _loc_y, int* _vx, int* _vy)
{
    if (_vx) *_vx = -1;
    if (_vy) *_vy = -1;

    auto sceneIt = ctx_.scenes.find(_sceneid);
    if (sceneIt == ctx_.scenes.end() || !sceneIt->second)
    {
        SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
            "HRL_GetVoxelAtScreenPosition: invalid scene ID");
        return HRL_FALSE;
    }

    hrl_scene_t* scene = sceneIt->second;
    HRL_VoxelWorld* world = scene->voxel_world;
    if (!world || world->width_ <= 0 || world->height_ <= 0 ||
        world->voxel_size_ <= 0.f)
        return HRL_FALSE;

    // A scene can have several viewports. Select the one containing the mouse.
    const HRL_Viewport* viewport = nullptr;
    const float winW = static_cast<float>(GetWindowWidth());
    const float winH = static_cast<float>(GetWindowHeight());
    for (const auto& [viewportId, candidate] : scene->viewports)
    {
        (void)viewportId;
        if (!candidate || !candidate->camera_)
            continue;

        const float viewportX = candidate->x_ * winW;
        const float viewportYTop = winH - (candidate->y_ + candidate->height_) * winH;
        const float viewportW = candidate->width_ * winW;
        const float viewportH = candidate->height_ * winH;
        if (_loc_x >= viewportX && _loc_x < viewportX + viewportW &&
            _loc_y >= viewportYTop && _loc_y < viewportYTop + viewportH)
        {
            viewport = candidate;
            break;
        }
    }

    if (!viewport)
        return HRL_FALSE;

    glm::vec3 rayOrigin(0.f);
    glm::vec3 rayDirection(0.f);
    if (!HRL_GetVoxelScreenRay(viewport, static_cast<float>(_loc_x), static_cast<float>(_loc_y),
        rayOrigin, rayDirection))
        return HRL_FALSE;

    // The voxel world is a finite rectangle in the XY plane at Z = 0.
    if (std::abs(rayDirection.z) <= 1e-6f)
        return HRL_FALSE;

    const float t = -rayOrigin.z / rayDirection.z;
    if (t < 0.f || !std::isfinite(t))
        return HRL_FALSE;

    const glm::vec3 hit = rayOrigin + rayDirection * t;
    if (!std::isfinite(hit.x) || !std::isfinite(hit.y))
        return HRL_FALSE;

    const float worldWidth = static_cast<float>(world->width_) * world->voxel_size_;
    const float worldHeight = static_cast<float>(world->height_) * world->voxel_size_;
    if (hit.x < 0.f || hit.y < 0.f || hit.x >= worldWidth || hit.y >= worldHeight)
        return HRL_FALSE;

    const int vx = static_cast<int>(std::floor(hit.x / world->voxel_size_));
    const int vy = static_cast<int>(std::floor(hit.y / world->voxel_size_));
    if (vx < 0 || vy < 0 || vx >= world->width_ || vy >= world->height_)
        return HRL_FALSE;

    if (_vx) *_vx = vx;
    if (_vy) *_vy = vy;
    return HRL_TRUE;
}

static bool IsValidGlobalIlluminationMethod(HRL_EGlobalIlluminationMethod method)
{
	return method >= HRL_GI_NONE && method <= HRL_GI_RAY_TRACING;
}

void HRL_SetGlobalIlluminationEnabled(HRL_id _sceneid, int _enable)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGlobalIlluminationEnabled: invalid scene ID");
		return;
	}

	if (_enable == HRL_FALSE)
	{
		it->second->global_illumination_enabled = false;
		return;
	}

	if (it->second->global_illumination_method == HRL_GI_NONE)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalIlluminationEnabled: scene GI method is HRL_GI_NONE");
		return;
	}

	if (!g_Backend.RHI_IsGlobalIlluminationMethodSupported ||
		!g_Backend.RHI_IsGlobalIlluminationMethodSupported((int)it->second->global_illumination_method))
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalIlluminationEnabled: selected GI method is not supported by the active backend");
		return;
	}

	it->second->global_illumination_enabled = true;
}

void HRL_SetGlobalIlluminationMethod(HRL_id _sceneid, HRL_EGlobalIlluminationMethod _method)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGlobalIlluminationMethod: invalid scene ID");
		return;
	}

	if (!IsValidGlobalIlluminationMethod(_method))
	{
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetGlobalIlluminationMethod: invalid GI method");
		return;
	}

	if (_method == HRL_GI_NONE)
	{
		it->second->global_illumination_method = HRL_GI_NONE;
		it->second->global_illumination_enabled = false;
		return;
	}

	if (!g_Backend.RHI_IsGlobalIlluminationMethodSupported ||
		!g_Backend.RHI_IsGlobalIlluminationMethodSupported((int)_method))
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalIlluminationMethod: selected GI method is not supported by the active backend");
		return;
	}

	it->second->global_illumination_method = _method;
}

int HRL_IsGlobalIlluminationEnabled(HRL_id _sceneid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_IsGlobalIlluminationEnabled: invalid scene ID");
		return HRL_FALSE;
	}
	return it->second->global_illumination_enabled ? HRL_TRUE : HRL_FALSE;
}

HRL_EGlobalIlluminationMethod HRL_GetGlobalIlluminationMethod(HRL_id _sceneid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetGlobalIlluminationMethod: invalid scene ID");
		return HRL_GI_NONE;
	}
	return it->second->global_illumination_method;
}

int HRL_IsGlobalIlluminationMethodSupported(HRL_EGlobalIlluminationMethod _method)
{
	if (!IsValidGlobalIlluminationMethod(_method) || _method == HRL_GI_NONE)
		return HRL_FALSE;
	if (!g_Backend.RHI_IsGlobalIlluminationMethodSupported)
		return HRL_FALSE;
	return g_Backend.RHI_IsGlobalIlluminationMethodSupported((int)_method) ? HRL_TRUE : HRL_FALSE;
}

uint32_t HRL_GetGlobalIlluminationSupportedMethods()
{
	return g_Backend.RHI_GetGlobalIlluminationSupportedMethods
		? g_Backend.RHI_GetGlobalIlluminationSupportedMethods()
		: 0u;
}

void HRL_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height)
{
	g_Backend.RHI_ResizeSceneTexture(_sceneid, _width, _height);
}

void HRL_EnableColorPickingBuffer(HRL_id _sceneid, int _enable)
{
	g_Backend.RHI_EnableColorPickingBuffer(_sceneid, _enable);
}

void HRL_SetSkySphereEnabled(HRL_id _sceneid, int _enable)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSkySphereEnabled: invalid scene ID");
		return;
	}
	it->second->sky_sphere_enabled = (_enable != HRL_FALSE);
	MarkSceneGILightingDirty(it->second);
}

void HRL_SetSkySphereColors(
	HRL_id _sceneid,
	float top_r, float top_g, float top_b,
	float horizon_r, float horizon_g, float horizon_b,
	float bottom_r, float bottom_g, float bottom_b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSkySphereColors: invalid scene ID");
		return;
	}

	it->second->sky_top_color = glm::vec3(top_r, top_g, top_b);
	it->second->sky_horizon_color = glm::vec3(horizon_r, horizon_g, horizon_b);
	it->second->sky_bottom_color = glm::vec3(bottom_r, bottom_g, bottom_b);
	MarkSceneGILightingDirty(it->second);
}

void HRL_SetSkySphereRotation(HRL_id _sceneid, float pitch, float yaw, float roll)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSkySphereRotation: invalid scene ID");
		return;
	}
	it->second->sky_rotation = glm::vec3(pitch, yaw, roll);
	MarkSceneGILightingDirty(it->second);
}

void HRL_SetSkySphereTexture(HRL_id _sceneid, HRL_id _textureid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSkySphereTexture: invalid scene ID");
		return;
	}
	it->second->sky_texture = _textureid;
	MarkSceneGILightingDirty(it->second);
}

void HRL_SetEnvironmentMappingEnabled(HRL_id _sceneid, int _enable)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetEnvironmentMappingEnabled: invalid scene ID");
		return;
	}
	it->second->environment_mapping_enabled = (_enable != HRL_FALSE);
}

void HRL_SetEnvironmentMap(HRL_id _sceneid, HRL_id _textureid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetEnvironmentMap: invalid scene ID");
		return;
	}
	it->second->environment_texture = _textureid;
}


//Post Process//
HRL_id HRL_CreatePostProcess(HRL_id _viewport, HRL_id _matid, int priority)
{
	auto it_viewport = ctx_.viewports.find(_viewport);
	if (it_viewport == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreatePostProcess: invalid viewport ID");
		return HRL_INVALID_ID;
	}

	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreatePostProcess: invalid material ID");
		return HRL_INVALID_ID;
	}

	auto pp = new HRL_PostProcess();
	pp->id_ = GenerateHRL_ID();
	pp->material_ = _matid;
	pp->priority_ = priority;

	HRL_id newId = pp->id_;

	ctx_.post_processes.emplace(newId, pp);
	it_viewport->second->post_processes.emplace(newId, pp);

	g_Backend.RHI_CreatePostProcess(_matid, priority);

	return newId;
}
void HRL_DeletePostProcess(HRL_id _postid)
{
	auto it = ctx_.post_processes.find(_postid);
	if (it == ctx_.post_processes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeletePostProcess: invalid ID");
		return;
	}

	// Remove all viewport references before freeing the object.
	for (auto& [viewport_id, viewport] : ctx_.viewports)
	{
		(void)viewport_id;
		if (viewport) viewport->post_processes.erase(_postid);
	}

	g_Backend.RHI_DeletePostProcess(_postid);
	delete it->second;
	ctx_.post_processes.erase(it);
}




//Shaders//
HRL_id HRL_CreateShader(const char *_vertContent, size_t _vertSize, const char *_fragContent, size_t _fragSize)
{
	//on return directement l'id, le backend gere les erreurs et retourne InvalidID en cas d'erreur
	return g_Backend.RHI_CreateShader(_vertContent, _vertSize, _fragContent, _fragSize);
}


HRL_id HRL_CreateShaderAsync(const char* vertData, size_t vertSize, const char* fragData, size_t fragSize)
{
  if (!vertData || !fragData || vertSize == 0 || fragSize == 0) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateShaderAsync: invalid shader source buffer"); return HRL_INVALID_ID; }
  const HRL_id id = GenerateHRL_ID();
  ctx_.async_resource_states[id] = static_cast<int>(AsyncResourceState::Pending);
  AsyncResourceTask task; task.type = AsyncResourceResult::Type::Shader; task.id = id; task.vert.assign(vertData, vertSize); task.frag.assign(fragData, fragSize);
  g_AsyncResourceLoader.Enqueue(std::move(task));
  return id;
}


int HRL_IsShaderReady(HRL_id id)
{
  auto it = ctx_.async_resource_states.find(id);
  if (it == ctx_.async_resource_states.end()) return g_Backend.RHI_IsValidShader ? g_Backend.RHI_IsValidShader(id) : HRL_FALSE;
  return it->second == static_cast<int>(AsyncResourceState::Ready) && g_Backend.RHI_IsValidShader(id);
}

void HRL_WaitForShader(HRL_id id)
{
  for (;;) { ProcessAsyncResourceUploads(); auto it = ctx_.async_resource_states.find(id); if (it == ctx_.async_resource_states.end()) return; if (it->second != static_cast<int>(AsyncResourceState::Pending)) return; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}

void HRL_WaitForAllAsyncResources()
{
  for (;;) {
    ProcessAsyncResourceUploads();
    bool pending = false;
    for (const auto& [id, state] : ctx_.async_resource_states) { (void)id; if (state == static_cast<int>(AsyncResourceState::Pending)) { pending = true; break; } }
    if (!pending && !g_AsyncResourceLoader.HasPendingWork()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void HRL_DeleteShader(HRL_id _shaderid)
{
  auto asyncIt = ctx_.async_resource_states.find(_shaderid);
  if (asyncIt != ctx_.async_resource_states.end()) {
    if (asyncIt->second == static_cast<int>(AsyncResourceState::Pending)) { asyncIt->second = static_cast<int>(AsyncResourceState::Cancelled); ctx_.async_resource_states.erase(asyncIt); return; }
    ctx_.async_resource_states.erase(asyncIt);
  }
  g_Backend.RHI_DeleteShader(_shaderid);
}


//Materials//
HRL_id HRL_CreateMaterial(HRL_id _shaderid)
{
	auto* m = new HRL_Material();
	m->shader_ = _shaderid;

	HRL_id newId = GenerateHRL_ID();
	ctx_.materials.emplace(newId, m);

	return newId;
}

void HRL_DeleteMaterial(HRL_id _matid)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteMaterial: invalid ID");
		return;
	}
	if (it->second)
	{
		for (HRL_id textureId : it->second->owned_textures_)
		{
			if (textureId != HRL_INVALID_ID)
				HRL_DeleteTexture(textureId);
		}
	}
	delete it->second;
	ctx_.materials.erase(it);
	MarkAllScenesGILightingDirty();
}

void HRL_SetMaterialUserHandle(HRL_id _matid, void* _handle)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMaterialUserHandle: invalid ID");
		return;
	}

	it->second->user_handle_ = _handle;
}

void* HRL_GetMaterialUserHandle(HRL_id _matid)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetMaterialUserHandle: invalid ID");
		return nullptr;
	}

	return it->second->user_handle_;
}

void HRL_MaterialSetInt(HRL_id _matid, const char* _uniformName, int a)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetInt: invalid ID");
		return;
	}
	//si la clée n'existe pas, elle est créée
	it->second->intParams_[_uniformName] = a;
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetTexture(HRL_id _matid, const char* _uniformName, HRL_id _textureid)
{
	auto it_mat = ctx_.materials.find(_matid);
	if (it_mat == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetTexture: invalid material ID");
		return;
	}

	//on vérifie que la texture existe au moment de RHI_BindMaterial, car ici on a pas acces aux textures
	it_mat->second->textureParams_[_uniformName] = _textureid;
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetBool(HRL_id _matid, const char* _uniformName, int a)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetBool: invalid ID");
		return;
	}
	it->second->intParams_[_uniformName] = a;
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetFloat(HRL_id _matid, const char* _uniformName, float a)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetFloat: invalid ID");
		return;
	}
	it->second->floatParams_[_uniformName] = a;
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetVec2(HRL_id _matid, const char* _uniformName, float x, float y)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec2: invalid ID");
		return;
	}
	it->second->vec2Params_[_uniformName] = glm::vec2(x, y);
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetVec3(HRL_id _matid, const char* _uniformName, float x, float y, float z)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec3: invalid ID");
		return;
	}
	it->second->vec3Params_[_uniformName] = glm::vec3(x, y, z);
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetVec4(HRL_id _matid, const char* _uniformName, float x, float y, float z, float w)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec4: invalid ID");
		return;
	}
	it->second->vec4Params_[_uniformName] = glm::vec4(x, y, z, w);
	MarkAllScenesGILightingDirty();
}



HRL_id HRL_CreateViewport(HRL_id _sceneid, HRL_id _cameraid, float x, float y, float _width, float _height)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateViewport: invalid scene ID");
		return HRL_INVALID_ID;
	}

	auto it = ctx_.cameras.find(_cameraid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateViewport: invalid camera ID");
		return HRL_INVALID_ID;
	}
	HRL_Camera* cam = it->second;

	if (cam->scene_ != _sceneid)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateViewport: camera belongs to another scene");
		return HRL_INVALID_ID;
	}

	auto* v = new HRL_Viewport(cam, x, y, _width, _height);

	HRL_id newId = GenerateHRL_ID();
	v->scene_ = _sceneid;
	it_scene->second->viewports.emplace(newId, v);
	ctx_.viewports.emplace(newId, v);

	return newId;
}

void HRL_DeleteViewport(HRL_id _viewportid)
{
	auto it = ctx_.viewports.find(_viewportid);
	if (it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteViewport: invalid ID");
		return;
	}

	// Destroy viewport-owned resources before the viewport itself.
	std::vector<HRL_id> gizmoIds;
	for (const auto& [gizmoId, gizmo] : ctx_.gizmos)
	{
		if (gizmo && gizmo->viewport_ == _viewportid) gizmoIds.push_back(gizmoId);
	}
	for (HRL_id id : gizmoIds) HRL_DeleteGizmo(id);

	std::vector<HRL_id> postIds;
	for (const auto& [id, pp] : it->second->post_processes)
	{ (void)pp; postIds.push_back(id); }
	for (HRL_id id : postIds) HRL_DeletePostProcess(id);

	std::vector<HRL_id> widgetIds;
	for (const auto& [id, widget] : it->second->widgets)
	{ (void)widget; widgetIds.push_back(id); }
	for (HRL_id id : widgetIds) HRL_DeleteWidget(id);

	//retire de la scene propriétaire
	for (auto& [scene_id, scene] : ctx_.scenes)
	{
		auto sit = scene->viewports.find(_viewportid);
		if (sit != scene->viewports.end())
		{
			scene->viewports.erase(sit);
			break;
		}
	}

	delete it->second;
	ctx_.viewports.erase(it);
}

void HRL_SetViewportCamera(HRL_id _viewportid, HRL_id _camid)
{
	auto viewport_it = ctx_.viewports.find(_viewportid);
	if (viewport_it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportCamera: invalid viewport ID");
		return;
	}

	auto cam_it = ctx_.cameras.find(_camid);
	if (cam_it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportCamera: invalid camera ID");
		return;
	}
	if (viewport_it->second->scene_ != HRL_INVALID_ID &&
		cam_it->second->scene_ != viewport_it->second->scene_)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetViewportCamera: camera belongs to another scene");
		return;
	}

	viewport_it->second->camera_ = cam_it->second;
}

void HRL_SetViewportRect(HRL_id _viewportid, float x, float y, float _width, float _height)
{
	auto it = ctx_.viewports.find(_viewportid);
	if (it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportRect: invalid ID");
		return;
	}
	it->second->x_      = x;
	it->second->y_      = y;
	it->second->width_  = _width;
	it->second->height_ = _height;
}



HRL_id HRL_CreateCamera(HRL_id _sceneid, HRL_ECameraType _type)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateCamera: invalid scene ID");
		return HRL_INVALID_ID;
	}

	if (_type == HRL_ORTHO || _type == HRL_PERSPECTIVE)
	{
		auto* cam = new HRL_Camera(
			//type, position, rotation
			_type,
			glm::vec3(1.f),
			glm::vec3(0.f),

			//fov vertical, near plane, far plane
			1000.f,
			0.01f,
			1000.f
			);
		HRL_id newId = GenerateHRL_ID();
		cam->scene_ = _sceneid;
		it_scene->second->cameras.emplace(newId, cam);
		ctx_.cameras.emplace(newId, cam);
		return newId;
	}
	else
	{
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_CreateCamera: invalid camera type");
		return HRL_INVALID_ID;
	}
}

void HRL_DeleteCamera(HRL_id _camid)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteCamera: invalid ID");
		return;
	}

	// Invalidate every viewport reference before deleting the camera.
	for (auto& [viewportId, viewport] : ctx_.viewports)
	{
		(void)viewportId;
		if (viewport && viewport->camera_ == it->second)
			viewport->camera_ = nullptr;
	}

	//retire de la scene propriétaire
	for (auto& [scene_id, scene] : ctx_.scenes)
	{
		auto sit = scene->cameras.find(_camid);
		if (sit != scene->cameras.end())
		{
			scene->cameras.erase(sit);
			break;
		}
	}

	delete it->second;
	ctx_.cameras.erase(it);
}


void HRL_SetCameraUserHandle(HRL_id _camid, void* _handle)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraUserHandle: invalid ID");
		return;
	}

	it->second->user_handle_ = _handle;
}

void* HRL_GetCameraUserHandle(HRL_id _camid)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetCameraUserHandle: invalid ID");
		return nullptr;
	}

	return it->second->user_handle_;
}

void HRL_SetCameraType(HRL_id _camid, HRL_ECameraType _type)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraType: invalid ID");
		return;
	}
	it->second->type_ = _type;
}

void HRL_SetCameraOrthoVertical(HRL_id _camid, float _height)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraOrthoVertical: invalid ID");
		return;
	}
	if (it->second->type_ == HRL_ORTHO)
	{
		it->second->value_ = _height;
	}
	else
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING, "HRL_SetCameraOrthoVertical: camera is not of type Ortho");
	}
}

void HRL_SetCameraPerspectiveFov(HRL_id _camid, float _fov)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraPerspectiveFov: invalid ID");
		return;
	}
	if (it->second->type_ == HRL_PERSPECTIVE)
	{
		it->second->value_ = _fov;
	}
	else
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING, "HRL_SetCameraPerspectiveFov: camera is not of type Perspective");
	}
}

void HRL_SetCameraNearPlane(HRL_id _camid, float _nearPlane)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraNearPlane: invalid ID");
		return;
	}
	it->second->near_plane_ = _nearPlane;
}

void HRL_SetCameraFarPlane(HRL_id _camid, float _farPlane)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraFarPlane: invalid ID");
		return;
	}
	it->second->far_plane_ = _farPlane;
}

void HRL_SetCameraLocation(HRL_id _camid, float x, float y, float z)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraLocation: invalid ID");
		return;
	}
	it->second->position_ = glm::vec3(x, y, z);
}

void HRL_SetCameraRotation(HRL_id _camid, float pitch, float yaw, float roll)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraRotation: invalid ID");
		return;
	}
	it->second->rotation_ = glm::vec3(pitch, yaw, roll);
}



// ============================================================================
// GIZMOS
// ============================================================================
namespace {

static HRL_Gizmo* FindGizmo(HRL_id id)
{
    auto it = ctx_.gizmos.find(id);
    return it == ctx_.gizmos.end() ? nullptr : it->second;
}

static const HRL_Gizmo* FindGizmoConst(HRL_id id)
{
    auto it = ctx_.gizmos.find(id);
    return it == ctx_.gizmos.end() ? nullptr : it->second;
}

static glm::mat4 GizmoProjection(const HRL_Viewport* viewport)
{
    if (!viewport || !viewport->camera_)
        return glm::mat4(1.f);
    const float width = std::max(1.f, static_cast<float>(GetWindowWidth()) * viewport->width_);
    const float height = std::max(1.f, static_cast<float>(GetWindowHeight()) * viewport->height_);
    const float aspect = width / height;
    if (viewport->camera_->type_ == HRL_PERSPECTIVE)
        return glm::perspective(glm::radians(viewport->camera_->value_), aspect,
            viewport->camera_->near_plane_, viewport->camera_->far_plane_);
    const float halfHeight = viewport->camera_->value_ * 0.5f;
    const float halfWidth = halfHeight * aspect;
    return glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight,
        viewport->camera_->near_plane_, viewport->camera_->far_plane_);
}

static glm::mat4 GizmoView(const HRL_Viewport* viewport)
{
    if (!viewport || !viewport->camera_)
        return glm::mat4(1.f);
    const HRL_Camera* cam = viewport->camera_;
    return glm::lookAt(cam->position_, cam->position_ + GetForwardVector(cam->rotation_), GetUpVector(cam->rotation_));
}

static bool GizmoScreenRay(const HRL_Viewport* viewport, float mouseX, float mouseY,
    glm::vec3& origin, glm::vec3& direction)
{
    if (!viewport || !viewport->camera_)
        return false;
    const float winW = static_cast<float>(GetWindowWidth());
    const float winH = static_cast<float>(GetWindowHeight());
    if (winW <= 1.f || winH <= 1.f)
        return false;
    const float vx = viewport->x_ * winW;
    const float vy = viewport->y_ * winH;
    const float vw = std::max(1.f, viewport->width_ * winW);
    const float vh = std::max(1.f, viewport->height_ * winH);
    if (mouseX < vx || mouseX > vx + vw || mouseY < vy || mouseY > vy + vh)
        return false;

    const float u = (mouseX - vx) / vw;
    const float v = (mouseY - vy) / vh;
    const float ndcX = u * 2.f - 1.f;
    const float ndcY = 1.f - v * 2.f;

    const glm::mat4 vp = GizmoProjection(viewport) * GizmoView(viewport);
    const glm::mat4 inv = glm::inverse(vp);
    glm::vec4 nearP = inv * glm::vec4(ndcX, ndcY, -1.f, 1.f);
    glm::vec4 farP  = inv * glm::vec4(ndcX, ndcY,  1.f, 1.f);
    if (std::abs(nearP.w) < 1e-6f || std::abs(farP.w) < 1e-6f)
        return false;
    nearP /= nearP.w;
    farP /= farP.w;
    origin = glm::vec3(nearP);
    direction = glm::normalize(glm::vec3(farP - nearP));
    return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z) &&
           std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z);
}

static float GizmoWorldSize(const HRL_Gizmo* gizmo, const HRL_Viewport* viewport)
{
    if (!gizmo || !viewport || !viewport->camera_)
        return 1.f;
    if (!gizmo->use_screen_size_)
        return std::max(0.0001f, gizmo->world_size_);

    const glm::vec3 viewSpace = glm::vec3(GizmoView(viewport) * glm::vec4(gizmo->position_, 1.f));
    const float depth = std::max(0.001f, std::abs(viewSpace.z));
    const float viewportHeight = std::max(1.f, static_cast<float>(GetWindowHeight()) * viewport->height_);
    if (viewport->camera_->type_ == HRL_PERSPECTIVE) {
        const float worldHeight = 2.f * depth * std::tan(glm::radians(viewport->camera_->value_) * 0.5f);
        return std::max(0.0001f, gizmo->screen_size_pixels_ * worldHeight / viewportHeight);
    }
    return std::max(0.0001f, gizmo->screen_size_pixels_ * viewport->camera_->value_ / viewportHeight);
}

static glm::vec3 GizmoAxisVector(const HRL_Gizmo* gizmo, int axis)
{
    glm::vec3 local(0.f);
    if (axis == 0) local.x = 1.f;
    if (axis == 1) local.y = 1.f;
    if (axis == 2) local.z = 1.f;
    if (gizmo->space_ == HRL_GIZMO_SPACE_WORLD)
        return local;
    glm::mat4 r(1.f);
    r = glm::rotate(r, glm::radians(gizmo->rotation_.x), glm::vec3(1,0,0));
    r = glm::rotate(r, glm::radians(gizmo->rotation_.y), glm::vec3(0,1,0));
    r = glm::rotate(r, glm::radians(gizmo->rotation_.z), glm::vec3(0,0,1));
    return glm::normalize(glm::vec3(r * glm::vec4(local, 0.f)));
}

static float RaySegmentDistance(const glm::vec3& ro, const glm::vec3& rd,
    const glm::vec3& a, const glm::vec3& b, float* outSegT = nullptr, float* outRayT = nullptr)
{
    const glm::vec3 u = rd;
    const glm::vec3 v = b - a;
    const glm::vec3 w = ro - a;
    const float a0 = glm::dot(u,u);
    const float b0 = glm::dot(u,v);
    const float c0 = glm::dot(v,v);
    const float d0 = glm::dot(u,w);
    const float e0 = glm::dot(v,w);
    const float denom = a0*c0 - b0*b0;
    float sc, tc;
    if (denom < 1e-8f) {
        sc = 0.f;
        tc = c0 > 1e-8f ? glm::clamp(e0 / c0, 0.f, 1.f) : 0.f;
    } else {
        sc = (b0*e0 - c0*d0) / denom;
        tc = (a0*e0 - b0*d0) / denom;
        if (sc < 0.f) { sc = 0.f; tc = glm::clamp(e0 / c0, 0.f, 1.f); }
        else if (tc < 0.f) { tc = 0.f; sc = glm::max(0.f, -d0 / a0); }
        else if (tc > 1.f) { tc = 1.f; sc = glm::max(0.f, (b0 - d0) / a0); }
    }
    if (outSegT) *outSegT = tc;
    if (outRayT) *outRayT = sc;
    const glm::vec3 p = ro + sc*u;
    const glm::vec3 q = a + tc*v;
    return glm::length(p-q);
}

static bool RayPlaneIntersection(const glm::vec3& ro, const glm::vec3& rd,
    const glm::vec3& point, const glm::vec3& normal, glm::vec3& hit)
{
    const float denom = glm::dot(rd, normal);
    if (std::abs(denom) < 1e-6f)
        return false;
    const float t = glm::dot(point-ro, normal) / denom;
    if (t < 0.f)
        return false;
    hit = ro + rd*t;
    return true;
}

struct GizmoPickResult {
    HRL_EGizmoPart part = HRL_GIZMO_PART_NONE;
    HRL_EGizmoOperation operation = HRL_GIZMO_OPERATION_NONE;
    float score = 1e30f;
};

static bool GizmoOperationVisible(const HRL_Gizmo* gizmo, HRL_EGizmoOperation op)
{
    if (!gizmo) return false;
    if ((static_cast<int>(gizmo->mode_) & static_cast<int>(op)) == 0) return false;
    if (op == HRL_GIZMO_OPERATION_TRANSLATE) return gizmo->show_translate_;
    if (op == HRL_GIZMO_OPERATION_ROTATE) return gizmo->show_rotate_;
    if (op == HRL_GIZMO_OPERATION_SCALE) return gizmo->show_scale_;
    return false;
}

static int GizmoOperationAxisMask(const HRL_Gizmo* gizmo, HRL_EGizmoOperation op)
{
    if (op == HRL_GIZMO_OPERATION_TRANSLATE) return gizmo->translate_axes_;
    if (op == HRL_GIZMO_OPERATION_ROTATE) return gizmo->rotate_axes_;
    if (op == HRL_GIZMO_OPERATION_SCALE) return gizmo->scale_axes_;
    return 0;
}

// Rotation arcs deliberately use the positive neighboring axes as their endpoints:
// X rotation: +Y -> +Z
// Y rotation: +Z -> +X
// Z rotation: +X -> +Y
// This makes the three quarter-wheels meet on the same positive axis handles.
static glm::vec3 GizmoRotateBasisU(const HRL_Gizmo* gizmo, int axis)
{
    const int nextAxis = (axis + 1) % 3;
    return GizmoAxisVector(gizmo, nextAxis);
}

static float GizmoRotateArcStart(int /*axis*/)
{
    return 0.f;
}

static bool GizmoAngleOnArc(float angle, float start, float sweep)
{
    static constexpr float kTwoPi = 6.28318530717958647692f;
    angle = std::fmod(angle + kTwoPi, kTwoPi);
    start = std::fmod(start + kTwoPi, kTwoPi);
    const float delta = std::fmod(angle - start + kTwoPi, kTwoPi);
    return delta <= sweep + 1e-4f;
}

static GizmoPickResult PickGizmoPart(const HRL_Gizmo* gizmo, const HRL_Viewport* viewport, float mouseX, float mouseY)
{
    GizmoPickResult result;
    if (!gizmo || !viewport || !gizmo->visible_ || !gizmo->enabled_)
        return result;
    glm::vec3 ro, rd;
    if (!GizmoScreenRay(viewport, mouseX, mouseY, ro, rd))
        return result;
    const float size = GizmoWorldSize(gizmo, viewport);
    const float pickRadius = size * 0.12f;
    const glm::vec3 center = gizmo->position_;

    if (GizmoOperationVisible(gizmo, HRL_GIZMO_OPERATION_TRANSLATE)) {
        const float centerDist = glm::length(glm::cross(rd, center-ro));
        if (centerDist < size * 0.22f && glm::dot(center-ro, rd) > 0.f) {
            result.part = HRL_GIZMO_PART_CENTER;
            result.operation = HRL_GIZMO_OPERATION_TRANSLATE;
            result.score = centerDist;
        }
    }

    // Translation owns the shaft, while scale owns the square tip. This keeps both modes
    // independently usable when their visuals are shown together.
    if (GizmoOperationVisible(gizmo, HRL_GIZMO_OPERATION_TRANSLATE)) {
        const int mask = gizmo->translate_axes_;
        for (int axis=0; axis<3; ++axis) {
            if (!(mask & (1<<axis))) continue;
            const glm::vec3 dir = GizmoAxisVector(gizmo, axis);
            const glm::vec3 end = center + dir * size;
            float segT = 0.f, rayT = 0.f;
            const float d = RaySegmentDistance(ro, rd, center + dir*(size*0.12f), center + dir*(size*0.80f), &segT, &rayT);
            if (d < pickRadius && rayT > 0.f && rayT < result.score) {
                result.part = static_cast<HRL_EGizmoPart>(axis+1);
                result.operation = HRL_GIZMO_OPERATION_TRANSLATE;
                result.score = rayT;
            }
            (void)end;
        }
    }

    if (GizmoOperationVisible(gizmo, HRL_GIZMO_OPERATION_SCALE)) {
        const int mask = gizmo->scale_axes_;
        for (int axis=0; axis<3; ++axis) {
            if (!(mask & (1<<axis))) continue;
            const glm::vec3 dir = GizmoAxisVector(gizmo, axis);
            const glm::vec3 tip = center + dir * size;
            const float half = size * 0.14f;
            float segT = 0.f, rayT = 0.f;
            const float d = RaySegmentDistance(ro, rd, tip - dir*half*1.3f, tip + dir*half*0.15f, &segT, &rayT);
            if (d < pickRadius * 1.15f && rayT > 0.f && rayT < result.score) {
                result.part = static_cast<HRL_EGizmoPart>(axis+1);
                result.operation = HRL_GIZMO_OPERATION_SCALE;
                result.score = rayT;
            }
        }
    }

    if (GizmoOperationVisible(gizmo, HRL_GIZMO_OPERATION_ROTATE)) {
        const float sweep = glm::radians(glm::clamp(gizmo->rotate_arc_degrees_, 15.f, 170.f));
        for (int axis=0; axis<3; ++axis) {
            if (!(gizmo->rotate_axes_ & (1<<axis))) continue;
            const glm::vec3 n = GizmoAxisVector(gizmo, axis);
            glm::vec3 hit;
            if (!RayPlaneIntersection(ro, rd, center, n, hit)) continue;
            const glm::vec3 radial = hit - center;
            const float radialLen = glm::length(radial);
            const float radialError = std::abs(radialLen - size);
            if (radialLen < 1e-5f || radialError > size * 0.11f) continue;
            const glm::vec3 u = GizmoRotateBasisU(gizmo, axis);
            const glm::vec3 v = glm::normalize(glm::cross(n, u));
            const float angle = std::atan2(glm::dot(radial, v), glm::dot(radial, u));
            if (!GizmoAngleOnArc(angle, GizmoRotateArcStart(axis), sweep)) continue;
            if (radialError < result.score) {
                result.part = static_cast<HRL_EGizmoPart>(axis+1);
                result.operation = HRL_GIZMO_OPERATION_ROTATE;
                result.score = radialError;
            }
        }
    }
    return result;
}

static void GizmoNotify(HRL_Gizmo* gizmo)
{
    if (gizmo && gizmo->changed_callback_)
        gizmo->changed_callback_(gizmo->id_, gizmo->active_part_, gizmo->changed_user_data_);
}

static void GizmoBeginDrag(HRL_Gizmo* gizmo, const HRL_Viewport* viewport)
{
    if (!gizmo || !viewport || gizmo->hovered_part_ == HRL_GIZMO_PART_NONE || gizmo->hovered_operation_ == HRL_GIZMO_OPERATION_NONE)
        return;
    glm::vec3 ro, rd;
    if (!GizmoScreenRay(viewport, ctx_.mouseX, ctx_.mouseY, ro, rd)) return;

    gizmo->active_part_ = gizmo->hovered_part_;
    gizmo->active_operation_ = gizmo->hovered_operation_;
    gizmo->dragging_ = true;
    gizmo->drag_viewport_ = [&]() -> HRL_id {
        for (const auto& [id, vp] : ctx_.viewports) if (vp == viewport) return id;
        return HRL_INVALID_ID;
    }();
    gizmo->drag_start_position_ = gizmo->position_;
    gizmo->drag_start_rotation_ = gizmo->rotation_;
    gizmo->drag_start_scale_ = gizmo->scale_;
    const int axisIndex = static_cast<int>(gizmo->active_part_) - 1;
    const glm::vec3 viewDir = glm::normalize(viewport->camera_->position_ - gizmo->position_);

    if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_TRANSLATE) {
        if (gizmo->active_part_ == HRL_GIZMO_PART_CENTER) {
            gizmo->drag_plane_normal_ = glm::normalize(GetForwardVector(viewport->camera_->rotation_));
            glm::vec3 hit;
            if (!RayPlaneIntersection(ro, rd, gizmo->position_, gizmo->drag_plane_normal_, hit))
                gizmo->dragging_ = false;
            else
                gizmo->drag_start_vector_ = hit - gizmo->position_;
        } else {
            const glm::vec3 axis = GizmoAxisVector(gizmo, axisIndex);
            glm::vec3 normal = glm::cross(axis, viewDir);
            if (glm::length(normal) < 1e-4f) normal = glm::cross(axis, glm::vec3(0,1,0));
            gizmo->drag_plane_normal_ = glm::normalize(glm::cross(normal, axis));
            glm::vec3 hit;
            if (!RayPlaneIntersection(ro, rd, gizmo->position_, gizmo->drag_plane_normal_, hit))
                gizmo->dragging_ = false;
            else
                gizmo->drag_start_axis_value_ = glm::dot(hit - gizmo->position_, axis);
            gizmo->drag_start_axis_ = axis;
        }
    } else if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_ROTATE) {
        const glm::vec3 axis = GizmoAxisVector(gizmo, axisIndex);
        glm::vec3 hit;
        if (!RayPlaneIntersection(ro, rd, gizmo->position_, axis, hit)) {
            gizmo->dragging_ = false;
        } else {
            const glm::vec3 start = hit - gizmo->position_;
            if (glm::length(start) < 1e-5f) gizmo->dragging_ = false;
            else {
                gizmo->drag_start_vector_ = glm::normalize(start);
                gizmo->drag_start_axis_ = axis;
                gizmo->drag_start_angle_ = 0.f;
            }
        }
    } else if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_SCALE) {
        const glm::vec3 axis = GizmoAxisVector(gizmo, axisIndex);
        glm::vec3 normal = glm::cross(axis, viewDir);
        if (glm::length(normal) < 1e-4f) normal = glm::cross(axis, glm::vec3(0,1,0));
        gizmo->drag_plane_normal_ = glm::normalize(glm::cross(normal, axis));
        glm::vec3 hit;
        if (!RayPlaneIntersection(ro, rd, gizmo->position_, gizmo->drag_plane_normal_, hit))
            gizmo->dragging_ = false;
        else {
            gizmo->drag_start_axis_value_ = glm::dot(hit - gizmo->position_, axis);
            gizmo->drag_start_axis_ = axis;
        }
    }
}

static void GizmoUpdateDrag(HRL_Gizmo* gizmo, const HRL_Viewport* viewport)
{
    if (!gizmo || !viewport || !gizmo->dragging_ || gizmo->active_operation_ == HRL_GIZMO_OPERATION_NONE)
        return;
    glm::vec3 ro, rd;
    if (!GizmoScreenRay(viewport, ctx_.mouseX, ctx_.mouseY, ro, rd)) return;
    const int axisIndex = static_cast<int>(gizmo->active_part_) - 1;
    const float size = GizmoWorldSize(gizmo, viewport);

    if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_TRANSLATE) {
        glm::vec3 hit;
        if (gizmo->active_part_ == HRL_GIZMO_PART_CENTER) {
            if (RayPlaneIntersection(ro, rd, gizmo->drag_start_position_, gizmo->drag_plane_normal_, hit))
                gizmo->position_ = gizmo->drag_start_position_ + (hit - gizmo->drag_start_position_) - gizmo->drag_start_vector_;
        } else if (RayPlaneIntersection(ro, rd, gizmo->drag_start_position_, gizmo->drag_plane_normal_, hit)) {
            const float axisValue = glm::dot(hit - gizmo->drag_start_position_, gizmo->drag_start_axis_);
            gizmo->position_ = gizmo->drag_start_position_ + gizmo->drag_start_axis_ * (axisValue - gizmo->drag_start_axis_value_);
        }
    } else if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_ROTATE) {
        glm::vec3 hit;
        if (RayPlaneIntersection(ro, rd, gizmo->drag_start_position_, gizmo->drag_start_axis_, hit)) {
            const glm::vec3 currentVector = hit - gizmo->position_;
            if (glm::length(currentVector) > 1e-5f) {
                const glm::vec3 current = glm::normalize(currentVector);
                const float angle = std::atan2(glm::dot(glm::cross(gizmo->drag_start_vector_, current), gizmo->drag_start_axis_),
                                               glm::dot(gizmo->drag_start_vector_, current));
                gizmo->rotation_ = gizmo->drag_start_rotation_;
                if (axisIndex == 0) gizmo->rotation_.x += glm::degrees(angle);
                if (axisIndex == 1) gizmo->rotation_.y += glm::degrees(angle);
                if (axisIndex == 2) gizmo->rotation_.z += glm::degrees(angle);
            }
        }
    } else if (gizmo->active_operation_ == HRL_GIZMO_OPERATION_SCALE) {
        glm::vec3 hit;
        if (RayPlaneIntersection(ro, rd, gizmo->drag_start_position_, gizmo->drag_plane_normal_, hit)) {
            const float axisValue = glm::dot(hit - gizmo->drag_start_position_, gizmo->drag_start_axis_);
            const float delta = axisValue - gizmo->drag_start_axis_value_;
            const float factor = std::max(0.01f, 1.f + delta / std::max(size, 1e-4f));
            gizmo->scale_ = gizmo->drag_start_scale_;
            if (axisIndex == 0) gizmo->scale_.x *= factor;
            if (axisIndex == 1) gizmo->scale_.y *= factor;
            if (axisIndex == 2) gizmo->scale_.z *= factor;
        }
    }
    GizmoNotify(gizmo);
}

static void UpdateAllGizmoHover()
{
    for (auto& [id, gizmo] : ctx_.gizmos) {
        (void)id;
        if (!gizmo) continue;
        GizmoPickResult pick;
        auto vpIt = ctx_.viewports.find(gizmo->viewport_);
        if (vpIt != ctx_.viewports.end())
            pick = PickGizmoPart(gizmo, vpIt->second, ctx_.mouseX, ctx_.mouseY);
        gizmo->hovered_part_ = pick.part;
        gizmo->hovered_operation_ = pick.operation;
    }
}

static void ProcessGizmoInput()
{
    if (ctx_.mouseLeftPressed && ctx_.mouseCaptureGizmo == HRL_INVALID_ID) {
        UpdateAllGizmoHover();
        HRL_id chosen = HRL_INVALID_ID;
        GizmoPickResult chosenPick;
        for (const auto& [id, gizmo] : ctx_.gizmos) {
            if (!gizmo || !gizmo->enabled_ || !gizmo->visible_ || gizmo->hovered_part_ == HRL_GIZMO_PART_NONE) continue;
            auto vpIt = ctx_.viewports.find(gizmo->viewport_);
            if (vpIt == ctx_.viewports.end()) continue;
            glm::vec3 ro, rd;
            if (!GizmoScreenRay(vpIt->second, ctx_.mouseX, ctx_.mouseY, ro, rd)) continue;
            const float distance = glm::length(gizmo->position_ - ro);
            if (distance < chosenPick.score) {
                chosenPick.part = gizmo->hovered_part_;
                chosenPick.operation = gizmo->hovered_operation_;
                chosenPick.score = distance;
                chosen = id;
            }
        }
        if (chosen != HRL_INVALID_ID) {
            ctx_.mouseCaptureGizmo = chosen;
            ctx_.mouseCaptureGizmoPart = chosenPick.part;
            HRL_Gizmo* gizmo = FindGizmo(chosen);
            auto vpIt = ctx_.viewports.find(gizmo->viewport_);
            if (vpIt != ctx_.viewports.end()) GizmoBeginDrag(gizmo, vpIt->second);
        }
    }

    if (ctx_.mouseCaptureGizmo != HRL_INVALID_ID) {
        HRL_Gizmo* gizmo = FindGizmo(ctx_.mouseCaptureGizmo);
        if (!gizmo) {
            ctx_.mouseCaptureGizmo = HRL_INVALID_ID;
            ctx_.mouseCaptureGizmoPart = HRL_GIZMO_PART_NONE;
        } else {
            auto vpIt = ctx_.viewports.find(gizmo->viewport_);
            if (ctx_.mouseLeftDown && vpIt != ctx_.viewports.end()) GizmoUpdateDrag(gizmo, vpIt->second);
            if (ctx_.mouseLeftReleased) {
                gizmo->dragging_ = false;
                gizmo->active_part_ = HRL_GIZMO_PART_NONE;
                gizmo->active_operation_ = HRL_GIZMO_OPERATION_NONE;
                ctx_.mouseCaptureGizmo = HRL_INVALID_ID;
                ctx_.mouseCaptureGizmoPart = HRL_GIZMO_PART_NONE;
            }
        }
    } else {
        UpdateAllGizmoHover();
    }
}

}

HRL_id HRL_CreateGizmo(HRL_id viewportid)
{
    auto vpIt = ctx_.viewports.find(viewportid);
    if (vpIt == ctx_.viewports.end() || !vpIt->second) {
        SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateGizmo: invalid viewport ID");
        return HRL_INVALID_ID;
    }
    auto sceneIt = ctx_.scenes.find(vpIt->second->scene_);
    if (sceneIt == ctx_.scenes.end()) {
        SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateGizmo: viewport scene is invalid");
        return HRL_INVALID_ID;
    }
    auto* gizmo = new (std::nothrow) HRL_Gizmo();
    if (!gizmo) {
        SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateGizmo: failed to allocate gizmo");
        return HRL_INVALID_ID;
    }
    const HRL_id id = GenerateHRL_ID();
    gizmo->id_ = id;
    gizmo->scene_ = vpIt->second->scene_;
    gizmo->viewport_ = viewportid;
    sceneIt->second->gizmos.emplace(id, gizmo);
    ctx_.gizmos.emplace(id, gizmo);
    return id;
}

void HRL_DeleteGizmo(HRL_id gizmoid)
{
    auto it = ctx_.gizmos.find(gizmoid);
    if (it == ctx_.gizmos.end()) {
        SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteGizmo: invalid ID");
        return;
    }
    HRL_Gizmo* gizmo = it->second;
    if (ctx_.mouseCaptureGizmo == gizmoid) {
        ctx_.mouseCaptureGizmo = HRL_INVALID_ID;
        ctx_.mouseCaptureGizmoPart = HRL_GIZMO_PART_NONE;
    }
    auto sceneIt = ctx_.scenes.find(gizmo->scene_);
    if (sceneIt != ctx_.scenes.end()) sceneIt->second->gizmos.erase(gizmoid);
    delete gizmo;
    ctx_.gizmos.erase(it);
}

int HRL_IsValidGizmo(HRL_id gizmoid) { return FindGizmoConst(gizmoid) ? HRL_TRUE : HRL_FALSE; }

void HRL_SetGizmoPosition(HRL_id id, float x, float y, float z) { if (auto* g=FindGizmo(id)) g->position_={x,y,z}; else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGizmoPosition: invalid ID"); }
void HRL_GetGizmoPosition(HRL_id id, float* x, float* y, float* z) { auto* g=FindGizmoConst(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoPosition: invalid ID"); return;} if(x)*x=g->position_.x; if(y)*y=g->position_.y; if(z)*z=g->position_.z; }
void HRL_SetGizmoRotation(HRL_id id, float p, float y, float r) { if(auto* g=FindGizmo(id)) g->rotation_={p,y,r}; else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoRotation: invalid ID"); }
void HRL_GetGizmoRotation(HRL_id id, float* p, float* y, float* r) { auto* g=FindGizmoConst(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoRotation: invalid ID"); return;} if(p)*p=g->rotation_.x; if(y)*y=g->rotation_.y; if(r)*r=g->rotation_.z; }
void HRL_SetGizmoScale(HRL_id id, float x, float y, float z) { if(auto* g=FindGizmo(id)) g->scale_={x,y,z}; else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoScale: invalid ID"); }
void HRL_GetGizmoScale(HRL_id id, float* x, float* y, float* z) { auto* g=FindGizmoConst(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoScale: invalid ID"); return;} if(x)*x=g->scale_.x; if(y)*y=g->scale_.y; if(z)*z=g->scale_.z; }
void HRL_SetGizmoMode(HRL_id id, HRL_EGizmoMode mode) { auto* g=FindGizmo(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoMode: invalid ID"); return;} const int allowed=HRL_GIZMO_MODE_TRANSLATE|HRL_GIZMO_MODE_ROTATE|HRL_GIZMO_MODE_SCALE; const int value=static_cast<int>(mode); if((value&~allowed)!=0 || value==0){SetErrorCode(HRL_INVALID_ENUM,HRL_SEVERITY_ERROR,"HRL_SetGizmoMode: invalid mode flags"); return;} g->mode_=mode; }
HRL_EGizmoMode HRL_GetGizmoMode(HRL_id id) { auto* g=FindGizmoConst(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoMode: invalid ID"); return HRL_GIZMO_MODE_TRANSLATE;} return g->mode_; }
void HRL_SetGizmoSpace(HRL_id id, HRL_EGizmoSpace space) { auto* g=FindGizmo(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoSpace: invalid ID"); return;} if(space<HRL_GIZMO_SPACE_WORLD||space>HRL_GIZMO_SPACE_LOCAL){SetErrorCode(HRL_INVALID_ENUM,HRL_SEVERITY_ERROR,"HRL_SetGizmoSpace: invalid space"); return;} g->space_=space; }
HRL_EGizmoSpace HRL_GetGizmoSpace(HRL_id id) { auto* g=FindGizmoConst(id); if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoSpace: invalid ID"); return HRL_GIZMO_SPACE_WORLD;} return g->space_; }
void HRL_SetGizmoTranslateVisible(HRL_id id,int v){if(auto*g=FindGizmo(id))g->show_translate_=v!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoTranslateVisible: invalid ID");}
void HRL_SetGizmoRotateVisible(HRL_id id,int v){if(auto*g=FindGizmo(id))g->show_rotate_=v!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoRotateVisible: invalid ID");}
void HRL_SetGizmoScaleVisible(HRL_id id,int v){if(auto*g=FindGizmo(id))g->show_scale_=v!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoScaleVisible: invalid ID");}
void HRL_SetGizmoTranslateAxes(HRL_id id,int mask){if(auto*g=FindGizmo(id))g->translate_axes_=mask&7;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoTranslateAxes: invalid ID");}
void HRL_SetGizmoRotateAxes(HRL_id id,int mask){if(auto*g=FindGizmo(id))g->rotate_axes_=mask&7;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoRotateAxes: invalid ID");}
void HRL_SetGizmoScaleAxes(HRL_id id,int mask){if(auto*g=FindGizmo(id))g->scale_axes_=mask&7;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoScaleAxes: invalid ID");}
void HRL_SetGizmoSize(HRL_id id,float size){if(auto*g=FindGizmo(id)){if(size<=0||!std::isfinite(size)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetGizmoSize: size must be > 0");return;}g->world_size_=size;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoSize: invalid ID");}
void HRL_SetGizmoScreenSize(HRL_id id,float px){if(auto*g=FindGizmo(id)){if(px<=0||!std::isfinite(px)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetGizmoScreenSize: pixels must be > 0");return;}g->screen_size_pixels_=px;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoScreenSize: invalid ID");}
void HRL_SetGizmoUseScreenSize(HRL_id id,int use){if(auto*g=FindGizmo(id))g->use_screen_size_=use!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoUseScreenSize: invalid ID");}
void HRL_SetGizmoVisible(HRL_id id,int v){if(auto*g=FindGizmo(id))g->visible_=v!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoVisible: invalid ID");}
void HRL_SetGizmoEnabled(HRL_id id,int e){if(auto*g=FindGizmo(id))g->enabled_=e!=HRL_FALSE;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoEnabled: invalid ID");}
void HRL_SetGizmoAxisColor(HRL_id id,int axis,float r,float g,float b,float a){auto* z=FindGizmo(id);if(!z){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoAxisColor: invalid ID");return;}if(axis!=HRL_GIZMO_AXIS_X&&axis!=HRL_GIZMO_AXIS_Y&&axis!=HRL_GIZMO_AXIS_Z){SetErrorCode(HRL_INVALID_ENUM,HRL_SEVERITY_ERROR,"HRL_SetGizmoAxisColor: axis must be X, Y or Z");return;}int idx=axis==HRL_GIZMO_AXIS_X?0:(axis==HRL_GIZMO_AXIS_Y?1:2);z->axis_colors_[idx]=glm::clamp(glm::vec4(r,g,b,a),glm::vec4(0.f),glm::vec4(1.f));}
void HRL_SetGizmoCenterColor(HRL_id id,float r,float g,float b,float a){if(auto* gizmo=FindGizmo(id))gizmo->center_color_=glm::clamp(glm::vec4(r,g,b,a),glm::vec4(0.f),glm::vec4(1.f));else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoCenterColor: invalid ID");}
void HRL_SetGizmoHoverColor(HRL_id id,float r,float g,float b,float a){if(auto* gizmo=FindGizmo(id))gizmo->hover_color_=glm::clamp(glm::vec4(r,g,b,a),glm::vec4(0.f),glm::vec4(1.f));else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoHoverColor: invalid ID");}
HRL_EGizmoPart HRL_GetGizmoHoveredPart(HRL_id id){auto*g=FindGizmoConst(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoHoveredPart: invalid ID");return HRL_GIZMO_PART_NONE;}return g->hovered_part_;}
HRL_EGizmoPart HRL_GetGizmoActivePart(HRL_id id){auto*g=FindGizmoConst(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoActivePart: invalid ID");return HRL_GIZMO_PART_NONE;}return g->active_part_;}
HRL_EGizmoOperation HRL_GetGizmoHoveredOperation(HRL_id id){auto*g=FindGizmoConst(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoHoveredOperation: invalid ID");return HRL_GIZMO_OPERATION_NONE;}return g->hovered_operation_;}
HRL_EGizmoOperation HRL_GetGizmoActiveOperation(HRL_id id){auto*g=FindGizmoConst(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_GetGizmoActiveOperation: invalid ID");return HRL_GIZMO_OPERATION_NONE;}return g->active_operation_;}
void HRL_SetGizmoRotateArcDegrees(HRL_id id,float degrees){auto*g=FindGizmo(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoRotateArcDegrees: invalid ID");return;}if(!std::isfinite(degrees)||degrees<=0.f||degrees>170.f){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetGizmoRotateArcDegrees: degrees must be in (0,170]");return;}g->rotate_arc_degrees_=degrees;}
void HRL_SetGizmoChangedCallback(HRL_id id, HRL_CGizmoChanged cb, void* ud){auto*g=FindGizmo(id);if(!g){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetGizmoChangedCallback: invalid ID");return;}g->changed_callback_=cb;g->changed_user_data_=ud;}

//EFFECTS
//BLOOM

//FOG
HRL_id HRL_CreateVFXSystem(HRL_id _sceneid)
{
	auto sceneIt = ctx_.scenes.find(_sceneid);
	if (sceneIt == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateVFXSystem: invalid scene ID");
		return HRL_INVALID_ID;
	}
	const HRL_id id = GenerateHRL_ID();
	auto* system = new (std::nothrow) HRL_VFXSystem();
	if (!system)
	{
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateVFXSystem: allocation failed");
		return HRL_INVALID_ID;
	}
	system->id_ = id;
	system->scene_ = _sceneid;
	sceneIt->second->vfx_systems.emplace(id, system);
	ctx_.vfx_systems.emplace(id, system);
	return id;
}

void HRL_DeleteVFXSystem(HRL_id _systemid)
{
	auto it = ctx_.vfx_systems.find(_systemid);
	if (it == ctx_.vfx_systems.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteVFXSystem: invalid ID");
		return;
	}
	HRL_VFXSystem* system = it->second;
	std::vector<HRL_id> emitterIds;
	emitterIds.reserve(system->emitters_.size());
	for (const auto& [id, emitter] : system->emitters_) { (void)emitter; emitterIds.push_back(id); }
	for (HRL_id id : emitterIds) HRL_DeleteVFXEmitter(id);
	if (auto sceneIt = ctx_.scenes.find(system->scene_); sceneIt != ctx_.scenes.end())
		sceneIt->second->vfx_systems.erase(_systemid);
	ctx_.vfx_systems.erase(it);
	delete system;
}

int HRL_IsValidVFXSystem(HRL_id _systemid)
{
	return FindVFXSystem(_systemid) ? HRL_TRUE : HRL_FALSE;
}

static bool VFXFinite(float v) { return std::isfinite(v); }
static bool VFXFinite3(float x, float y, float z) { return VFXFinite(x) && VFXFinite(y) && VFXFinite(z); }

void HRL_SetVFXSystemPosition(HRL_id _systemid, float x, float y, float z)
{
	if (auto* s = FindVFXSystem(_systemid))
	{
		if (!VFXFinite3(x,y,z)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemPosition: non-finite value"); return; }
		s->position_ = glm::vec3(x,y,z);
	}
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemPosition: invalid ID");
}

void HRL_GetVFXSystemPosition(HRL_id _systemid, float* x, float* y, float* z)
{
	if (auto* s = FindVFXSystem(_systemid))
	{
		if (x) *x = s->position_.x; if (y) *y = s->position_.y; if (z) *z = s->position_.z;
	}
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetVFXSystemPosition: invalid ID");
}

void HRL_SetVFXSystemRotation(HRL_id _systemid, float pitch, float yaw, float roll)
{
	if (auto* s = FindVFXSystem(_systemid))
	{
		if (!VFXFinite3(pitch,yaw,roll)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemRotation: non-finite value"); return; }
		s->rotation_ = glm::vec3(pitch,yaw,roll);
	}
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemRotation: invalid ID");
}

void HRL_GetVFXSystemRotation(HRL_id _systemid, float* pitch, float* yaw, float* roll)
{
	if (auto* s = FindVFXSystem(_systemid))
	{
		if (pitch) *pitch = s->rotation_.x; if (yaw) *yaw = s->rotation_.y; if (roll) *roll = s->rotation_.z;
	}
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetVFXSystemRotation: invalid ID");
}

void HRL_SetVFXSystemScale(HRL_id _systemid, float x, float y, float z)
{
	if (auto* s = FindVFXSystem(_systemid))
	{
		if (!VFXFinite3(x,y,z)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemScale: non-finite value"); return; }
		s->scale_ = glm::vec3(x,y,z);
	}
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemScale: invalid ID");
}

void HRL_GetVFXSystemScale(HRL_id _systemid, float* x, float* y, float* z)
{
	if (auto* s = FindVFXSystem(_systemid))
	{
		if (x) *x = s->scale_.x; if (y) *y = s->scale_.y; if (z) *z = s->scale_.z;
	}
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetVFXSystemScale: invalid ID");
}

void HRL_PlayVFXSystem(HRL_id _systemid)
{
	if (auto* s = FindVFXSystem(_systemid)) { s->playing_ = true; s->paused_ = false; }
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_PlayVFXSystem: invalid ID");
}

void HRL_StopVFXSystem(HRL_id _systemid)
{
	if (auto* s = FindVFXSystem(_systemid)) { s->playing_ = false; s->paused_ = false; }
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_StopVFXSystem: invalid ID");
}

void HRL_PauseVFXSystem(HRL_id _systemid)
{
	if (auto* s = FindVFXSystem(_systemid)) s->paused_ = true;
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_PauseVFXSystem: invalid ID");
}

void HRL_ResetVFXSystem(HRL_id _systemid)
{
	if (auto* s = FindVFXSystem(_systemid))
	{
		s->time_ = 0.f;
		s->playing_ = false;
		s->paused_ = false;
		for (auto& [id, emitter] : s->emitters_) { (void)id; ResetVFXEmitter(emitter); }
	}
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_ResetVFXSystem: invalid ID");
}

void HRL_SetVFXSystemLooping(HRL_id _systemid, int _looping)
{
	if (auto* s = FindVFXSystem(_systemid)) s->looping_ = (_looping != HRL_FALSE);
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemLooping: invalid ID");
}

void HRL_SetVFXSystemTimeScale(HRL_id _systemid, float _scale)
{
	if (auto* s = FindVFXSystem(_systemid))
	{
		if (!VFXFinite(_scale) || _scale < 0.f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemTimeScale: invalid scale"); return; }
		s->time_scale_ = _scale;
	}
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemTimeScale: invalid ID");
}

void HRL_SetVFXSystemEnabled(HRL_id _systemid, int _enabled)
{
	if (auto* s = FindVFXSystem(_systemid)) s->enabled_ = (_enabled != HRL_FALSE);
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemEnabled: invalid ID");
}

void HRL_SetVFXSystemAutoUpdate(HRL_id _systemid, int _auto_update)
{
	if (auto* s = FindVFXSystem(_systemid)) s->auto_update_ = (_auto_update != HRL_FALSE);
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemAutoUpdate: invalid ID");
}

void HRL_SetVFXSystemDuration(HRL_id _systemid, float _duration)
{
	if (auto* s = FindVFXSystem(_systemid))
	{
		if (!VFXFinite(_duration) || _duration < 0.f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemDuration: invalid duration"); return; }
		s->duration_ = _duration;
	}
	else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVFXSystemDuration: invalid ID");
}

HRL_id HRL_CreateVFXEmitter(HRL_id _systemid)
{
	auto* system = FindVFXSystem(_systemid);
	if (!system)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateVFXEmitter: invalid system ID");
		return HRL_INVALID_ID;
	}
	const HRL_id id = GenerateHRL_ID();
	auto* emitter = new (std::nothrow) HRL_VFXEmitter();
	if (!emitter) { SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateVFXEmitter: allocation failed"); return HRL_INVALID_ID; }
	emitter->id_ = id;
	emitter->system_ = _systemid;
	system->emitters_.emplace(id, emitter);
	ctx_.vfx_emitters.emplace(id, emitter);
	return id;
}

void HRL_DeleteVFXEmitter(HRL_id _emitterid)
{
	auto it = ctx_.vfx_emitters.find(_emitterid);
	if (it == ctx_.vfx_emitters.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteVFXEmitter: invalid ID"); return; }
	HRL_VFXEmitter* emitter = it->second;
	std::vector<HRL_id> curveIds;
	for (HRL_id id : {emitter->color_curve_, emitter->size_curve_, emitter->rotation_curve_}) if (id != HRL_INVALID_ID) curveIds.push_back(id);
	for (HRL_id id : curveIds) HRL_DeleteVFXCurve(id);
	if (auto system = FindVFXSystem(emitter->system_)) system->emitters_.erase(_emitterid);
	ctx_.vfx_emitters.erase(it);
	delete emitter;
}

int HRL_IsValidVFXEmitter(HRL_id _emitterid) { return FindVFXEmitter(_emitterid) ? HRL_TRUE : HRL_FALSE; }

#define HRL_VFX_SIMPLE_SETTER_FLOAT(name, field, minval) \
void name(HRL_id id, float value) { if (auto* e=FindVFXEmitter(id)) { if (!std::isfinite(value) || value < (minval)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, #name ": invalid value"); return; } e->field=value; } else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, #name ": invalid ID"); }
HRL_VFX_SIMPLE_SETTER_FLOAT(HRL_SetVFXEmitterSpawnRate, spawn_rate_, 0.f)
HRL_VFX_SIMPLE_SETTER_FLOAT(HRL_SetVFXEmitterShapeRadius, shape_radius_, 0.f)
HRL_VFX_SIMPLE_SETTER_FLOAT(HRL_SetVFXEmitterShapeAngle, shape_angle_degrees_, 0.f)
HRL_VFX_SIMPLE_SETTER_FLOAT(HRL_SetVFXDrag, drag_, 0.f)
HRL_VFX_SIMPLE_SETTER_FLOAT(HRL_SetVFXEmitterStretch, stretch_, 0.f)
#undef HRL_VFX_SIMPLE_SETTER_FLOAT

void HRL_SetVFXEmitterEnabled(HRL_id id, int enabled) { if (auto* e=FindVFXEmitter(id)) e->enabled_=(enabled!=HRL_FALSE); else SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVFXEmitterEnabled: invalid ID"); }
void HRL_SetVFXEmitterPosition(HRL_id id,float x,float y,float z) { if (auto* e=FindVFXEmitter(id)) { if(!VFXFinite3(x,y,z)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterPosition: non-finite value");return;} e->position_=glm::vec3(x,y,z);} else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterPosition: invalid ID"); }
void HRL_SetVFXEmitterRotation(HRL_id id,float x,float y,float z) { if (auto* e=FindVFXEmitter(id)) { if(!VFXFinite3(x,y,z)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterRotation: non-finite value");return;} e->rotation_=glm::vec3(x,y,z);} else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterRotation: invalid ID"); }
void HRL_SetVFXEmitterMaxParticles(HRL_id id,HRL_uint maxp) { if(auto* e=FindVFXEmitter(id)){e->max_particles_=std::max<HRL_uint>(1,maxp);if(e->particles_.size()>e->max_particles_)e->particles_.resize(e->max_particles_);} else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterMaxParticles: invalid ID"); }
void HRL_SetVFXEmitterBurst(HRL_id id,HRL_uint count){if(auto*e=FindVFXEmitter(id)){auto*s=FindVFXSystem(e->system_);if(!s){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterBurst: invalid system ID");return;}if(s->playing_&&!s->paused_){for(HRL_uint i=0;i<count&&e->particles_.size()<e->max_particles_;++i)SpawnVFXParticle(e);}else{e->bursts_.push_back({s->time_,count});std::sort(e->bursts_.begin(),e->bursts_.end(),[](const auto&a,const auto&b){return a.time<b.time;});}}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterBurst: invalid ID");}
void HRL_AddVFXBurst(HRL_id id,float time,HRL_uint count){if(auto*e=FindVFXEmitter(id)){if(!std::isfinite(time)||time<0.f){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_AddVFXBurst: invalid time");return;}e->bursts_.push_back({time,count});std::sort(e->bursts_.begin(),e->bursts_.end(),[](const auto&a,const auto&b){return a.time<b.time;});}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_AddVFXBurst: invalid ID");}
void HRL_ClearVFXBursts(HRL_id id){if(auto*e=FindVFXEmitter(id)){e->bursts_.clear();e->next_burst_=0;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_ClearVFXBursts: invalid ID");}
void HRL_SetVFXEmitterLifetime(HRL_id id,float a,float b){if(auto*e=FindVFXEmitter(id)){if(!std::isfinite(a)||!std::isfinite(b)||a<=0.f||b<=0.f){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterLifetime: invalid lifetime");return;}e->min_lifetime_=std::min(a,b);e->max_lifetime_=std::max(a,b);}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterLifetime: invalid ID");}
void HRL_SetVFXEmitterSpawnShape(HRL_id id,HRL_EVFXSpawnShape shape){if(auto*e=FindVFXEmitter(id)){if(shape<HRL_VFX_SHAPE_POINT||shape>HRL_VFX_SHAPE_CONE){SetErrorCode(HRL_INVALID_ENUM,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterSpawnShape: invalid enum");return;}e->spawn_shape_=shape;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterSpawnShape: invalid ID");}
void HRL_SetVFXEmitterShapeSize(HRL_id id,float x,float y,float z){if(auto*e=FindVFXEmitter(id)){if(!VFXFinite3(x,y,z)||x<0.f||y<0.f||z<0.f){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterShapeSize: invalid size");return;}e->shape_size_=glm::vec3(x,y,z);}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterShapeSize: invalid ID");}
void HRL_SetVFXEmitterInitialVelocity(HRL_id id,float ax,float ay,float az,float bx,float by,float bz){if(auto*e=FindVFXEmitter(id)){if(!VFXFinite3(ax,ay,az)||!VFXFinite3(bx,by,bz)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterInitialVelocity: non-finite value");return;}e->min_initial_velocity_=glm::vec3(std::min(ax,bx),std::min(ay,by),std::min(az,bz));e->max_initial_velocity_=glm::vec3(std::max(ax,bx),std::max(ay,by),std::max(az,bz));}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterInitialVelocity: invalid ID");}
void HRL_SetVFXEmitterInitialSpeed(HRL_id id,float a,float b){if(auto*e=FindVFXEmitter(id)){if(!VFXFinite(a)||!VFXFinite(b)||a<0.f||b<0.f){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterInitialSpeed: invalid speed");return;}e->min_initial_speed_=std::min(a,b);e->max_initial_speed_=std::max(a,b);}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterInitialSpeed: invalid ID");}
void HRL_SetVFXEmitterInitialRotation(HRL_id id,float ax,float ay,float az,float bx,float by,float bz){if(auto*e=FindVFXEmitter(id)){if(!VFXFinite3(ax,ay,az)||!VFXFinite3(bx,by,bz)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterInitialRotation: non-finite value");return;}e->min_initial_rotation_=glm::vec3(std::min(ax,bx),std::min(ay,by),std::min(az,bz));e->max_initial_rotation_=glm::vec3(std::max(ax,bx),std::max(ay,by),std::max(az,bz));}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterInitialRotation: invalid ID");}
void HRL_SetVFXEmitterAngularVelocity(HRL_id id,float ax,float ay,float az,float bx,float by,float bz){if(auto*e=FindVFXEmitter(id)){if(!VFXFinite3(ax,ay,az)||!VFXFinite3(bx,by,bz)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterAngularVelocity: non-finite value");return;}e->min_angular_velocity_=glm::vec3(std::min(ax,bx),std::min(ay,by),std::min(az,bz));e->max_angular_velocity_=glm::vec3(std::max(ax,bx),std::max(ay,by),std::max(az,bz));}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterAngularVelocity: invalid ID");}
void HRL_SetVFXGravity(HRL_id id,float x,float y,float z){if(auto*e=FindVFXEmitter(id)){e->gravity_=glm::vec3(x,y,z);}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXGravity: invalid ID");}
void HRL_SetVFXForce(HRL_id id,float x,float y,float z){if(auto*e=FindVFXEmitter(id)){e->force_=glm::vec3(x,y,z);}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXForce: invalid ID");}
void HRL_SetVFXNoise(HRL_id id,float strength,float frequency,float scroll){if(auto*e=FindVFXEmitter(id)){if(strength<0.f||frequency<0.f||scroll<0.f||!VFXFinite3(strength,frequency,scroll)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXNoise: invalid parameters");return;}e->noise_strength_=strength;e->noise_frequency_=frequency;e->noise_scroll_speed_=scroll;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXNoise: invalid ID");}
void HRL_SetVFXEmitterRenderMode(HRL_id id,HRL_EVFXRenderMode mode){if(auto*e=FindVFXEmitter(id)){if(mode<HRL_VFX_RENDER_BILLBOARD||mode>HRL_VFX_RENDER_MESH){SetErrorCode(HRL_INVALID_ENUM,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterRenderMode: invalid enum");return;}e->render_mode_=mode;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterRenderMode: invalid ID");}
void HRL_SetVFXEmitterTexture(HRL_id id,HRL_id tex){if(auto*e=FindVFXEmitter(id))e->texture_=tex;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterTexture: invalid ID");}
void HRL_SetVFXEmitterMaterial(HRL_id id,HRL_id mat){if(auto*e=FindVFXEmitter(id))e->material_=mat;else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterMaterial: invalid ID");}
void HRL_SetVFXEmitterMesh(HRL_id id,HRL_id mesh){if(auto*e=FindVFXEmitter(id)){if(mesh!=HRL_INVALID_ID){auto it=ctx_.meshes.find(mesh);if(it==ctx_.meshes.end()||!it->second||it->second->type_!=HRL_3D_MESH){SetErrorCode(HRL_INVALID_OPERATION,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterMesh: mesh must reference a valid static 3D mesh");return;}}e->mesh_=mesh;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterMesh: invalid ID");}
void HRL_SetVFXEmitterMeshScale(HRL_id id,float x,float y,float z){if(auto*e=FindVFXEmitter(id)){if(!VFXFinite3(x,y,z)||x<0.f||y<0.f||z<0.f){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterMeshScale: invalid scale");return;}e->mesh_scale_=glm::vec3(x,y,z);}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterMeshScale: invalid ID");}
void HRL_SetVFXEmitterMeshRotation(HRL_id id,float x,float y,float z){if(auto*e=FindVFXEmitter(id)){if(!VFXFinite3(x,y,z)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterMeshRotation: non-finite value");return;}e->mesh_rotation_offset_=glm::vec3(x,y,z);}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterMeshRotation: invalid ID");}
void HRL_SetVFXEmitterBlendMode(HRL_id id,HRL_EVFXBlendMode mode){if(auto*e=FindVFXEmitter(id)){if(mode<HRL_VFX_BLEND_ALPHA||mode>HRL_VFX_BLEND_MULTIPLY){SetErrorCode(HRL_INVALID_ENUM,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterBlendMode: invalid enum");return;}e->blend_mode_=mode;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterBlendMode: invalid ID");}
void HRL_SetVFXEmitterParticleSize(HRL_id id,float x,float y){if(auto*e=FindVFXEmitter(id)){if(!VFXFinite(x)||!VFXFinite(y)||x<0.f||y<0.f){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterParticleSize: invalid size");return;}e->particle_size_=glm::vec2(x,y);}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterParticleSize: invalid ID");}
void HRL_SetVFXEmitterSimulationSpace(HRL_id id,HRL_EVFXSimulationSpace space){if(auto*e=FindVFXEmitter(id)){if(space<HRL_VFX_SIMULATION_LOCAL||space>HRL_VFX_SIMULATION_WORLD){SetErrorCode(HRL_INVALID_ENUM,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterSimulationSpace: invalid enum");return;}e->simulation_space_=space;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterSimulationSpace: invalid ID");}

HRL_id HRL_CreateVFXColorCurve(HRL_id emitterid){auto*e=FindVFXEmitter(emitterid);if(!e){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_CreateVFXColorCurve: invalid emitter ID");return HRL_INVALID_ID;}const HRL_id id=GenerateHRL_ID();auto*c=new(std::nothrow)HRL_VFXCurve();if(!c){SetErrorCode(HRL_OUT_OF_MEMORY,HRL_SEVERITY_ERROR,"HRL_CreateVFXColorCurve: allocation failed");return HRL_INVALID_ID;}c->id_=id;c->emitter_=emitterid;c->color_=true;e->color_curve_=id;ctx_.vfx_curves.emplace(id,c);return id;}
HRL_id HRL_CreateVFXFloatCurve(HRL_id emitterid){auto*e=FindVFXEmitter(emitterid);if(!e){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_CreateVFXFloatCurve: invalid emitter ID");return HRL_INVALID_ID;}const HRL_id id=GenerateHRL_ID();auto*c=new(std::nothrow)HRL_VFXCurve();if(!c){SetErrorCode(HRL_OUT_OF_MEMORY,HRL_SEVERITY_ERROR,"HRL_CreateVFXFloatCurve: allocation failed");return HRL_INVALID_ID;}c->id_=id;c->emitter_=emitterid;c->color_=false;ctx_.vfx_curves.emplace(id,c);return id;}
void HRL_DeleteVFXCurve(HRL_id id){auto it=ctx_.vfx_curves.find(id);if(it==ctx_.vfx_curves.end()){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_DeleteVFXCurve: invalid ID");return;}auto*c=it->second;if(auto*e=FindVFXEmitter(c->emitter_)){if(e->color_curve_==id)e->color_curve_=HRL_INVALID_ID;if(e->size_curve_==id)e->size_curve_=HRL_INVALID_ID;if(e->rotation_curve_==id)e->rotation_curve_=HRL_INVALID_ID;}ctx_.vfx_curves.erase(it);delete c;}
int HRL_IsValidVFXCurve(HRL_id id){return FindVFXCurve(id)?HRL_TRUE:HRL_FALSE;}
void HRL_AddVFXColorKey(HRL_id id,float t,float r,float g,float b,float a){if(auto*c=FindVFXCurve(id)){if(!c->color_||!VFXFinite(t)||t<0.f||!VFXFinite3(r,g,b)||!VFXFinite(a)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_AddVFXColorKey: invalid key");return;}c->color_keys_.push_back({t,glm::vec4(r,g,b,a)});std::sort(c->color_keys_.begin(),c->color_keys_.end(),[](const auto&A,const auto&B){return A.time<B.time;});}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_AddVFXColorKey: invalid ID");}
void HRL_AddVFXFloatKey(HRL_id id,float t,float value){if(auto*c=FindVFXCurve(id)){if(c->color_||!VFXFinite(t)||t<0.f||!VFXFinite(value)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_AddVFXFloatKey: invalid key");return;}c->float_keys_.push_back({t,value});std::sort(c->float_keys_.begin(),c->float_keys_.end(),[](const auto&A,const auto&B){return A.time<B.time;});}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_AddVFXFloatKey: invalid ID");}
void HRL_ClearVFXColorKeys(HRL_id id){if(auto*c=FindVFXCurve(id)){if(!c->color_){SetErrorCode(HRL_INVALID_OPERATION,HRL_SEVERITY_ERROR,"HRL_ClearVFXColorKeys: curve is not a color curve");return;}c->color_keys_.clear();}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_ClearVFXColorKeys: invalid ID");}
void HRL_ClearVFXFloatKeys(HRL_id id){if(auto*c=FindVFXCurve(id)){if(c->color_){SetErrorCode(HRL_INVALID_OPERATION,HRL_SEVERITY_ERROR,"HRL_ClearVFXFloatKeys: curve is not a float curve");return;}c->float_keys_.clear();}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_ClearVFXFloatKeys: invalid ID");}
void HRL_SetVFXEmitterColorCurve(HRL_id emitterid,HRL_id curveid){if(auto*e=FindVFXEmitter(emitterid)){auto*c=FindVFXCurve(curveid);if(!c||!c->color_||c->emitter_!=emitterid){SetErrorCode(HRL_INVALID_OPERATION,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterColorCurve: incompatible curve");return;}e->color_curve_=curveid;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterColorCurve: invalid ID");}
void HRL_SetVFXEmitterSizeCurve(HRL_id emitterid,HRL_id curveid){if(auto*e=FindVFXEmitter(emitterid)){auto*c=FindVFXCurve(curveid);if(!c||c->color_||c->emitter_!=emitterid){SetErrorCode(HRL_INVALID_OPERATION,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterSizeCurve: incompatible curve");return;}e->size_curve_=curveid;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterSizeCurve: invalid ID");}
void HRL_SetVFXEmitterRotationCurve(HRL_id emitterid,HRL_id curveid){if(auto*e=FindVFXEmitter(emitterid)){auto*c=FindVFXCurve(curveid);if(!c||c->color_||c->emitter_!=emitterid){SetErrorCode(HRL_INVALID_OPERATION,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterRotationCurve: incompatible curve");return;}e->rotation_curve_=curveid;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXEmitterRotationCurve: invalid ID");}
void HRL_SetVFXCollisionEnabled(HRL_id id,int enabled){if(auto*e=FindVFXEmitter(id))e->collision_enabled_=(enabled!=HRL_FALSE);else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXCollisionEnabled: invalid ID");}
void HRL_SetVFXCollisionRestitution(HRL_id id,float v){if(auto*e=FindVFXEmitter(id)){if(!VFXFinite(v)||v<0.f){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXCollisionRestitution: invalid value");return;}e->collision_restitution_=glm::clamp(v,0.f,1.f);}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXCollisionRestitution: invalid ID");}
void HRL_SetVFXCollisionFriction(HRL_id id,float v){if(auto*e=FindVFXEmitter(id)){if(!VFXFinite(v)||v<0.f){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"HRL_SetVFXCollisionFriction: invalid value");return;}e->collision_friction_=glm::clamp(v,0.f,1.f);}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXCollisionFriction: invalid ID");}
void HRL_SetVFXCollisionScene(HRL_id id,HRL_id sceneid,int enabled){if(auto*e=FindVFXEmitter(id)){if(enabled!=HRL_FALSE&&ctx_.scenes.find(sceneid)==ctx_.scenes.end()){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXCollisionScene: invalid scene ID");return;}e->collision_scene_=enabled!=HRL_FALSE?sceneid:HRL_INVALID_ID;e->collision_enabled_=enabled!=HRL_FALSE;}else SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"HRL_SetVFXCollisionScene: invalid ID");}

void HRL_UpdateVFX(float delta)
{
	if (!VFXFinite(delta) || delta < 0.f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_UpdateVFX: invalid delta time"); return; }
	delta = std::min(delta, 0.25f);
	for (const auto& [id, system] : ctx_.vfx_systems)
	{
		(void)id;
		if (system && !system->auto_update_)
			SimulateVFXSystem(system, delta);
	}
}

void HRL_SetFogEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogEnabled: invalid scene ID");
		return;
	}
	it->second->fog.enabled = enable;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogMode(HRL_id scene, HRL_EFogType mode)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogMode: invalid scene ID");
		return;
	}
	it->second->fog.mode = mode;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogColor: invalid scene ID");
		return;
	}
	it->second->fog.r = r;
	it->second->fog.g = g;
	it->second->fog.b = b;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogDensity(HRL_id scene, float density)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogColor: invalid scene ID");
		return;
	}
	it->second->fog.density = density;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogLinearRange(HRL_id scene, float start, float end)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogColor: invalid scene ID");
		return;
	}
	it->second->fog.range_start = start;
	it->second->fog.range_end = end;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

HRL_id HRL_CreateVolumetricFog(HRL_id scene)
{
	auto scene_it = ctx_.scenes.find(scene);
	if (scene_it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_CreateVolumetricFog: invalid scene ID");
		return HRL_INVALID_ID;
	}

	if (scene_it->second->volumetric_fogs.size() >= HRL_MAX_VOLUMETRIC_FOGS)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_CreateVolumetricFog: scene reached HRL_MAX_VOLUMETRIC_FOGS");
		return HRL_INVALID_ID;
	}

	auto* fog = new (std::nothrow) HRL_VolumetricFog();
	if (!fog)
	{
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
			"HRL_CreateVolumetricFog: failed to allocate volumetric fog");
		return HRL_INVALID_ID;
	}

	const HRL_id newId = GenerateHRL_ID();
	fog->id_ = newId;
	fog->scene_ = scene;

	scene_it->second->volumetric_fogs.emplace(newId, fog);
	ctx_.volumetric_fogs.emplace(newId, fog);
	return newId;
}

void HRL_DeleteVolumetricFog(HRL_id fogid)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_DeleteVolumetricFog: invalid ID");
		return;
	}

	HRL_VolumetricFog* fog = it->second;
	const HRL_id sceneId = fog->scene_;
	auto scene_it = ctx_.scenes.find(sceneId);
	if (scene_it != ctx_.scenes.end())
		scene_it->second->volumetric_fogs.erase(fogid);

	delete fog;
	ctx_.volumetric_fogs.erase(it);
}

void HRL_SetVolumetricFogEnabled(HRL_id fogid, int enable)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogEnabled: invalid volumetric fog ID");
		return;
	}
	it->second->enabled = (enable != HRL_FALSE);
}

void HRL_SetVolumetricFogPosition(HRL_id fogid, float x, float y, float z)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogPosition: invalid volumetric fog ID");
		return;
	}
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogPosition: position must contain finite values");
		return;
	}
	it->second->position = glm::vec3(x, y, z);
}

void HRL_SetVolumetricFogRadius(HRL_id fogid, float radius)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogRadius: invalid volumetric fog ID");
		return;
	}
	if (!std::isfinite(radius) || radius <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogRadius: radius must be > 0");
		return;
	}
	it->second->radius = radius;
}

void HRL_SetVolumetricFogDensity(HRL_id fogid, float density)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogDensity: invalid volumetric fog ID");
		return;
	}
	if (!std::isfinite(density) || density < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogDensity: density must be >= 0");
		return;
	}
	it->second->density = density;
}

void HRL_SetVolumetricFogColor(HRL_id fogid, float r, float g, float b)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogColor: invalid volumetric fog ID");
		return;
	}
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogColor: color must contain finite values");
		return;
	}
	it->second->color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.f), glm::vec3(1.f));
}

void HRL_SetVolumetricFogSteps(HRL_id fogid, HRL_uint steps)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogSteps: invalid volumetric fog ID");
		return;
	}
	it->second->steps = std::max<HRL_uint>(4u, std::min<HRL_uint>(64u, steps));
}

void HRL_SetGlobalVolumetricFogEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogEnabled: invalid scene ID");
		return;
	}
	it->second->global_volumetric_fog.enabled = (enable != HRL_FALSE);
}

void HRL_SetGlobalVolumetricFogDensity(HRL_id scene, float density)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogDensity: invalid scene ID");
		return;
	}
	if (!std::isfinite(density) || density < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogDensity: density must be >= 0");
		return;
	}
	it->second->global_volumetric_fog.density = density;
}

void HRL_SetGlobalVolumetricFogColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogColor: invalid scene ID");
		return;
	}
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogColor: color must contain finite values");
		return;
	}
	it->second->global_volumetric_fog.color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.f), glm::vec3(1.f));
}

void HRL_SetGlobalVolumetricFogSteps(HRL_id scene, HRL_uint steps)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogSteps: invalid scene ID");
		return;
	}
	it->second->global_volumetric_fog.steps = std::max<HRL_uint>(4u, std::min<HRL_uint>(64u, steps));
}

void HRL_SetGodRaysEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysEnabled: invalid scene ID"); return; }
	it->second->god_rays.enabled = (enable != HRL_FALSE);
}

void HRL_SetGodRaysPosition(HRL_id scene, float x, float y, float z)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysPosition: invalid scene ID"); return; }
	it->second->god_rays.position = glm::vec3(x, y, z);
}

void HRL_SetGodRaysColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysColor: invalid scene ID"); return; }
	it->second->god_rays.color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.f), glm::vec3(1.f));
}

void HRL_SetGodRaysDensity(HRL_id scene, float density)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysDensity: invalid scene ID"); return; }
	it->second->god_rays.density = glm::clamp(density, 0.f, 2.f);
}

void HRL_SetGodRaysDecay(HRL_id scene, float decay)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysDecay: invalid scene ID"); return; }
	it->second->god_rays.decay = glm::clamp(decay, 0.8f, 0.999f);
}

void HRL_SetGodRaysWeight(HRL_id scene, float weight)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysWeight: invalid scene ID"); return; }
	it->second->god_rays.weight = glm::clamp(weight, 0.f, 4.f);
}

void HRL_SetGodRaysSamples(HRL_id scene, HRL_uint samples)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysSamples: invalid scene ID"); return; }
	it->second->god_rays.samples = std::max<HRL_uint>(8u, std::min<HRL_uint>(96u, samples));
}







void HRL_SetAmbientOcclusionEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionEnabled: invalid scene ID"); return; }
	it->second->ambient_occlusion_enabled = enable != 0;
}

void HRL_SetAmbientOcclusionStrength(HRL_id scene, float strength)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionStrength: invalid scene ID"); return; }
	if (!std::isfinite(strength) || strength < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionStrength: strength must be finite and >= 0"); return; }
	it->second->ambient_occlusion_strength = std::min(strength, 1.0f);
}

void HRL_SetAmbientOcclusionRadius(HRL_id scene, float radius)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionRadius: invalid scene ID"); return; }
	if (!std::isfinite(radius) || radius <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionRadius: radius must be finite and > 0"); return; }
	it->second->ambient_occlusion_radius = radius;
}

void HRL_SetAmbientOcclusionBias(HRL_id scene, float bias)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionBias: invalid scene ID"); return; }
	if (!std::isfinite(bias) || bias < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionBias: bias must be finite and >= 0"); return; }
	it->second->ambient_occlusion_bias = bias;
}

void HRL_SetAmbientOcclusionPower(HRL_id scene, float power)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionPower: invalid scene ID"); return; }
	if (!std::isfinite(power) || power <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionPower: power must be finite and > 0"); return; }
	it->second->ambient_occlusion_power = power;
}

void HRL_SetScreenSpaceReflectionsEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsEnabled: invalid scene ID"); return; }
	it->second->screen_space_reflections_enabled = (enable != 0);
}

void HRL_SetScreenSpaceReflectionsStrength(HRL_id scene, float strength)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsStrength: invalid scene ID"); return; }
	if (!std::isfinite(strength)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsStrength: strength must be finite"); return; }
	it->second->screen_space_reflections_strength = std::max(0.0f, strength);
}

void HRL_SetScreenSpaceReflectionsMaxDistance(HRL_id scene, float distance)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsMaxDistance: invalid scene ID"); return; }
	if (!std::isfinite(distance) || distance <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsMaxDistance: distance must be > 0"); return; }
	it->second->screen_space_reflections_max_distance = distance;
}

void HRL_SetScreenSpaceReflectionsThickness(HRL_id scene, float thickness)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsThickness: invalid scene ID"); return; }
	if (!std::isfinite(thickness) || thickness <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsThickness: thickness must be > 0"); return; }
	it->second->screen_space_reflections_thickness = thickness;
}

void HRL_SetScreenSpaceReflectionsFade(HRL_id scene, float start, float end)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsFade: invalid scene ID"); return; }
	if (!std::isfinite(start) || !std::isfinite(end) || end <= start) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsFade: invalid fade range"); return; }
	it->second->screen_space_reflections_fade_start = glm::clamp(start, 0.0f, 0.99f);
	it->second->screen_space_reflections_fade_end = glm::clamp(end, 0.0f, 1.0f);
	if (it->second->screen_space_reflections_fade_end <= it->second->screen_space_reflections_fade_start)
		it->second->screen_space_reflections_fade_end = std::min(1.0f, it->second->screen_space_reflections_fade_start + 0.01f);
}

void HRL_SetScreenSpaceReflectionsSteps(HRL_id scene, HRL_uint steps)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsSteps: invalid scene ID"); return; }
	it->second->screen_space_reflections_steps = std::max<HRL_uint>(8u, std::min<HRL_uint>(96u, steps));
}

void HRL_SetVolumetricCloudEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudEnabled: invalid scene ID"); return; }
	it->second->volumetric_cloud_enabled = (enable != HRL_FALSE);
}

void HRL_SetVolumetricCloudCoverage(HRL_id scene, float coverage)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudCoverage: invalid scene ID"); return; }
	if (!std::isfinite(coverage)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudCoverage: coverage must be finite"); return; }
	it->second->volumetric_cloud_coverage = glm::clamp(coverage, 0.0f, 1.0f);
}

void HRL_SetVolumetricCloudDensity(HRL_id scene, float density)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudDensity: invalid scene ID"); return; }
	if (!std::isfinite(density) || density < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudDensity: density must be finite and >= 0"); return; }
	it->second->volumetric_cloud_density = density;
}

void HRL_SetVolumetricCloudHeight(HRL_id scene, float min_height, float max_height)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudHeight: invalid scene ID"); return; }
	if (!std::isfinite(min_height) || !std::isfinite(max_height) || max_height <= min_height + 0.001f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudHeight: max height must be greater than min height"); return; }
	it->second->volumetric_cloud_height_min = min_height;
	it->second->volumetric_cloud_height_max = max_height;
}

void HRL_SetVolumetricCloudScale(HRL_id scene, float scale)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudScale: invalid scene ID"); return; }
	if (!std::isfinite(scale) || scale <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudScale: scale must be > 0"); return; }
	it->second->volumetric_cloud_scale = scale;
}

void HRL_SetVolumetricCloudDetail(HRL_id scene, float detail)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudDetail: invalid scene ID"); return; }
	if (!std::isfinite(detail)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudDetail: detail must be finite"); return; }
	it->second->volumetric_cloud_detail = glm::clamp(detail, 0.0f, 1.0f);
}

void HRL_SetVolumetricCloudWind(HRL_id scene, float wind_x, float wind_z, float speed)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudWind: invalid scene ID"); return; }
	if (!std::isfinite(wind_x) || !std::isfinite(wind_z) || !std::isfinite(speed) || speed < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudWind: invalid wind parameters"); return; }
	it->second->volumetric_cloud_wind = glm::vec2(wind_x, wind_z);
	it->second->volumetric_cloud_wind_speed = speed;
}

void HRL_SetVolumetricCloudColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudColor: invalid scene ID"); return; }
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudColor: color must contain finite values"); return; }
	it->second->volumetric_cloud_color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.0f), glm::vec3(1.0f));
}

void HRL_SetVolumetricCloudLightColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightColor: invalid scene ID"); return; }
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightColor: color must contain finite values"); return; }
	it->second->volumetric_cloud_light_color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.0f), glm::vec3(8.0f));
}

void HRL_SetVolumetricCloudLightAbsorption(HRL_id scene, float absorption)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightAbsorption: invalid scene ID"); return; }
	if (!std::isfinite(absorption) || absorption < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightAbsorption: absorption must be finite and >= 0"); return; }
	it->second->volumetric_cloud_light_absorption = absorption;
}

void HRL_SetVolumetricCloudLightIntensity(HRL_id scene, float intensity)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightIntensity: invalid scene ID"); return; }
	if (!std::isfinite(intensity) || intensity < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightIntensity: intensity must be finite and >= 0"); return; }
	it->second->volumetric_cloud_light_intensity = intensity;
}

void HRL_SetVolumetricCloudSteps(HRL_id scene, HRL_uint steps)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudSteps: invalid scene ID"); return; }
	it->second->volumetric_cloud_steps = std::max<HRL_uint>(8u, std::min<HRL_uint>(96u, steps));
}

void HRL_SetVolumetricCloudMaxDistance(HRL_id scene, float distance)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudMaxDistance: invalid scene ID"); return; }
	if (!std::isfinite(distance) || distance <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudMaxDistance: distance must be > 0"); return; }
	it->second->volumetric_cloud_max_distance = distance;
}


HRL_id HRL_CreateDecal(HRL_id scene)
{
	auto sceneIt = ctx_.scenes.find(scene);
	if (sceneIt == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateDecal: invalid scene ID"); return HRL_INVALID_ID; }
	auto* decal = new (std::nothrow) HRL_Decal();
	if (!decal) { SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateDecal: failed to allocate decal"); return HRL_INVALID_ID; }
	const HRL_id id = GenerateHRL_ID();
	decal->id_ = id;
	decal->scene_ = scene;
	sceneIt->second->decals.emplace(id, decal);
	ctx_.decals.emplace(id, decal);
	return id;
}

void HRL_DeleteDecal(HRL_id decalId)
{
	auto it = ctx_.decals.find(decalId);
	if (it == ctx_.decals.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteDecal: invalid ID"); return; }
	if (auto sceneIt = ctx_.scenes.find(it->second->scene_); sceneIt != ctx_.scenes.end())
		sceneIt->second->decals.erase(decalId);
	delete it->second;
	ctx_.decals.erase(it);
}

static HRL_Decal* GetDecalForEdit(HRL_id id, const char* fn)
{
	auto it = ctx_.decals.find(id);
	if (it == ctx_.decals.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(fn) + ": invalid decal ID"); return nullptr; }
	return it->second;
}

void HRL_SetDecalEnabled(HRL_id id, int enable)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalEnabled")) d->enabled = (enable != 0);
}
void HRL_SetDecalPosition(HRL_id id, float x, float y, float z)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalPosition")) { if (!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalPosition: invalid position"); return; } d->position = glm::vec3(x,y,z); }
}
void HRL_SetDecalRotation(HRL_id id, float pitch, float yaw, float roll)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalRotation")) { if (!std::isfinite(pitch)||!std::isfinite(yaw)||!std::isfinite(roll)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalRotation: invalid rotation"); return; } d->rotation = glm::vec3(pitch,yaw,roll); }
}
void HRL_SetDecalSize(HRL_id id, float x, float y, float z)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalSize")) { if (!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||x<=0||y<=0||z<=0) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalSize: size must be > 0"); return; } d->size=glm::vec3(x,y,z); }
}
void HRL_SetDecalTexture(HRL_id id, HRL_id texture)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalTexture")) d->texture = texture;
}
void HRL_SetDecalColor(HRL_id id, float r, float g, float b)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalColor")) { if (!std::isfinite(r)||!std::isfinite(g)||!std::isfinite(b)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalColor: invalid color"); return; } d->color=glm::clamp(glm::vec3(r,g,b), glm::vec3(0.0f), glm::vec3(1.0f)); }
}
void HRL_SetDecalOpacity(HRL_id id, float opacity)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalOpacity")) { if (!std::isfinite(opacity)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalOpacity: opacity must be finite"); return; } d->opacity=glm::clamp(opacity,0.0f,1.0f); }
}
void HRL_SetDecalNormalFade(HRL_id id, float min_dot, float max_dot)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalNormalFade")) { if (!std::isfinite(min_dot)||!std::isfinite(max_dot)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalNormalFade: values must be finite"); return; } d->normal_fade_min=glm::clamp(min_dot,0.0f,1.0f); d->normal_fade_max=glm::clamp(max_dot,0.0f,1.0f); if (d->normal_fade_max <= d->normal_fade_min) d->normal_fade_max=std::min(1.0f,d->normal_fade_min+0.01f); }
}

int HRL_IsValidDecal(HRL_id id)
{ return ctx_.decals.find(id) != ctx_.decals.end() ? 1 : 0; }

//MATRICES
void HRL_GetProjectionMatrix(float *aa)
{
	g_Backend.RHI_GetProjectionMatrix(aa);
}

void HRL_GetViewMatrix(float *aa)
{
	g_Backend.RHI_GetViewMatrix(aa);
}

void HRL_GetModelMatrix(HRL_id _meshid, float *aa)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetModelMatrix: Invalid ID");
		return;
	}
	g_Backend.RHI_GetModelMatrix(it->second, aa);
}

//Debug

void HRL_SetDebugLineThickness(float a)
{
	ctx_.debug_line_thickness = a;
}

void HRL_DrawDebugSegment(HRL_id _sceneid, float a_x, float a_y, float a_z, float b_x, float b_y, float b_z, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugSegment: invalid scene ID");
		return;
	}

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x, a_y, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x, b_y, b_z, r, g, b);
}

void HRL_DrawDebugPolygon(HRL_id _sceneid, HRL_EDebugRenderingType _mode, const float *vertices_x, const float *vertices_y, const float *vertices_z, int vertices_count, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugPolygon: invalid scene ID");
		return;
	}

	if (_mode == HRL_DEBUG_SOLID)
	{
		for (int i=1; i < vertices_count-1; i++)
		{
			//pivot
			ctx_.debug_renderers[_sceneid].triangles.emplace_back(vertices_x[0], vertices_y[0], vertices_z[0], r, g, b);
			//i
			ctx_.debug_renderers[_sceneid].triangles.emplace_back(vertices_x[i], vertices_y[i], vertices_z[i], r, g, b);
			//i+1
			ctx_.debug_renderers[_sceneid].triangles.emplace_back(vertices_x[i+1], vertices_y[i+1], vertices_z[i+1], r, g, b);
		}
	}
	else if (_mode == HRL_DEBUG_HOLLOW)
	{
		for (int i=0; i < vertices_count - 1; i++)
		{
			ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[i], vertices_y[i], vertices_z[i], r, g, b);
			ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[i+1], vertices_y[i+1], vertices_z[i+1], r, g, b);
		}

		//line entre le dernier et le 0 pour refermer
		ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[vertices_count-1], vertices_y[vertices_count-1], vertices_z[vertices_count-1], r, g, b);
		ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[0], vertices_y[0], vertices_z[0], r, g, b);
	}
	else
	{
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_DrawDebugPolygon: invalid mode, expected HRL_DebugSolid or HRL_DebugHollow");
	}
}

void HRL_DrawDebugCircle(HRL_id _sceneid, HRL_EDebugRenderingType _mode, float center_x, float center_y, float center_z, float radius, int segments, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugCircle: invalid scene ID"); return; }

	int seg = segments;
	auto* vx = (float*)alloca(seg * sizeof(float));
	auto* vy = (float*)alloca(seg * sizeof(float));
	auto* vz = (float*)alloca(seg * sizeof(float));

	for (int i = 0; i < seg; i++)
	{
		float angle = (float)(2.f * M_PI * i) / seg;
		vx[i] = center_x + radius * cosf(angle);
		vy[i] = center_y + radius * sinf(angle);
		vz[i] = center_z;
	}

	HRL_DrawDebugPolygon(_sceneid, _mode, vx, vy, vz, seg, r, g, b);
}

void HRL_DrawDebugCapsule(HRL_id _sceneid, HRL_EDebugRenderingType _mode, float a_x, float a_y, float a_z, float b_x, float b_y, float b_z, float radius, int segments, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugCapsule: invalid scene ID"); return; }

	int half_seg = segments / 2;

	// direction A→B
	float dx = b_x - a_x;
	float dy = b_y - a_y;
	float len = sqrtf(dx * dx + dy * dy);

	// vecteur unitaire perpendiculaire
	float nx = 0.f, ny = 0.f;
	if (len > 1e-6f) { nx = -dy / len; ny = dx / len; }

	// angle de l'axe A→B
	float base_angle = atan2f(dy, dx);

	// demi-cercle autour de B — face opposée à A
	// plage : [base_angle - PI/2 → base_angle + PI/2]
	for (int i = 0; i < half_seg; i++)
	{
		float a0 = base_angle - (float)M_PI / 2.f + (float)M_PI * (float)i       / (float)half_seg;
		float a1 = base_angle - (float)M_PI / 2.f + (float)M_PI * (float)(i + 1) / (float)half_seg;

		ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x + radius * cosf(a0), b_y + radius * sinf(a0), b_z, r, g, b);
		ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x + radius * cosf(a1), b_y + radius * sinf(a1), b_z, r, g, b);
	}

	// demi-cercle autour de A — face opposée à B
	// plage : [base_angle + PI/2 → base_angle + 3PI/2]
	for (int i = 0; i < half_seg; i++)
	{
		float a0 = base_angle + (float)M_PI / 2.f + (float)M_PI * (float)i       / (float)half_seg;
		float a1 = base_angle + (float)M_PI / 2.f + (float)M_PI * (float)(i + 1) / (float)half_seg;

		ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + radius * cosf(a0), a_y + radius * sinf(a0), a_z, r, g, b);
		ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + radius * cosf(a1), a_y + radius * sinf(a1), a_z, r, g, b);
	}

	// deux segments latéraux reliant les demi-cercles
	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + nx * radius, a_y + ny * radius, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x + nx * radius, b_y + ny * radius, b_z, r, g, b);

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x - nx * radius, a_y - ny * radius, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x - nx * radius, b_y - ny * radius, b_z, r, g, b);

}

void HRL_DrawDebugPoint(HRL_id _sceneid, float a_x, float a_y, float a_z, float size, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugPoint: invalid scene ID"); return; }

	float h = size * 0.5f;

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x - h, a_y,     a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + h, a_y,     a_z, r, g, b);

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x,     a_y - h, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x,     a_y + h, a_z, r, g, b);
}


//Text//
HRL_id HRL_CreateFont(const char *data, size_t _data_size)
{
	HRL_id newId = GenerateHRL_ID();
	auto* font = new HRL_Font();

	font->ttf_buffer.assign(data, data+_data_size);

	int ok = stbtt_InitFont(
		&font->info,
		font->ttf_buffer.data(),
		stbtt_GetFontOffsetForIndex(font->ttf_buffer.data(), 0)
	);

	if (!ok)
	{
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_CreateFont: failed to parse TTF data");
		delete font;
		return HRL_INVALID_ID;
	}

	ctx_.fonts.emplace(newId, font);

	return newId;
}

void HRL_DeleteFont(HRL_id _fontid)
{
	auto it = ctx_.fonts.find(_fontid);
	if (it == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteFont: invalid ID");
		return;
	}
	delete it->second;
	ctx_.fonts.erase(it);
}





//Is Valid Functions
int HRL_IsValidMesh(HRL_id _id)
{
	auto it = ctx_.meshes.find(_id);
	if (it == ctx_.meshes.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidLight(HRL_id _id)
{
	auto it = ctx_.lights.find(_id);
	if (it == ctx_.lights.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidVolumetricFog(HRL_id _id)
{
	auto it = ctx_.volumetric_fogs.find(_id);
	if (it == ctx_.volumetric_fogs.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidTexture(HRL_id _id)
{
	//Backend request
	return g_Backend.RHI_IsValidTexture(_id);
}

int HRL_IsValidScene(HRL_id _id)
{
	auto it = ctx_.scenes.find(_id);
	if (it == ctx_.scenes.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidPostProcess(HRL_id _id)
{
	auto it = ctx_.post_processes.find(_id);
	if (it == ctx_.post_processes.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidShader(HRL_id _id)
{
	//Backend request
	return g_Backend.RHI_IsValidShader(_id);
}

int HRL_IsValidMaterial(HRL_id _id)
{
	auto it = ctx_.materials.find(_id);
	if (it == ctx_.materials.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidViewport(HRL_id _id)
{
	auto it = ctx_.viewports.find(_id);
	if (it == ctx_.viewports.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidCamera(HRL_id _id)
{
	auto it = ctx_.cameras.find(_id);
	if (it == ctx_.cameras.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidFont(HRL_id _id)
{
	auto it = ctx_.fonts.find(_id);
	if (it == ctx_.fonts.end())
	{
		return 0;
	}
	return 1;
}



void HRL_TakeScreenshot(HRL_id _sceneid, const char *_target_path)
{
	if (_target_path == nullptr || *_target_path == '\0')
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_TakeScreenshot: target path is empty");
		return;
	}
	if (ctx_.scenes.find(_sceneid) == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_TakeScreenshot: invalid scene ID");
		return;
	}
	ctx_.pending_screenshots[_sceneid] = _target_path;
}

void HRL_ReloadTexture(HRL_id _textureid, const char *_data, size_t _bufferSize)
{

}

void HRL_SetAntialiasingMode(HRL_uint _mode)
{
	if (_mode != HRL_ANTIALIASING_OFF && _mode != HRL_ANTIALIASING_2X &&
		_mode != HRL_ANTIALIASING_4X && _mode != HRL_ANTIALIASING_8X)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetAntialiasingMode: expected OFF, 2X, 4X or 8X");
		return;
	}
	if (g_Backend.RHI_SetAntialiasingMode)
		g_Backend.RHI_SetAntialiasingMode((int)_mode);
}

void HRL_MaterialSetEmissiveColor(HRL_id matid, float r, float g, float b, float a)
{

}

HRL_id HRL_CreateMesh3D(HRL_id _sceneid, const HRL_Vertex3D* _vertices, size_t _vertexCount, const HRL_uint* _indices, size_t _indexCount)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: invalid scene ID");
		return HRL_INVALID_ID;
	}

	if (!_vertices || _vertexCount == 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertices must be non-null and vertex count must be greater than zero");
		return HRL_INVALID_ID;
	}

	if (_indexCount == 0 && (_vertexCount % 3) != 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: non-indexed vertex count must be a multiple of 3");
		return HRL_INVALID_ID;
	}

	if (_indexCount > 0)
	{
		if (!_indices || (_indexCount % 3) != 0)
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: indexed meshes require a non-null index array and an index count multiple of 3");
			return HRL_INVALID_ID;
		}
		for (size_t i = 0; i < _indexCount; ++i)
		{
			if ((size_t)_indices[i] >= _vertexCount)
			{
				SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: index references a vertex outside the vertex array");
				return HRL_INVALID_ID;
			}
		}
	}

	for (size_t i = 0; i < _vertexCount; ++i)
	{
		const HRL_Vertex3D& v = _vertices[i];
		for (float value : v.position)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertex position contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : v.normal)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertex normal contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : v.uv)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertex UV contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : v.tangent)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertex tangent contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : v.bitangent)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertex bitangent contains NaN or infinity"); return HRL_INVALID_ID; }
	}

	HRL_id newId = GenerateHRL_ID();
	if (g_Backend.RHI_CreateMesh(newId, _vertices, _vertexCount, _indices, _indexCount) != HRL_TRUE)
	{
		return HRL_INVALID_ID;
	}

	auto* mesh = new HRL_Mesh();
	mesh->scene_ = _sceneid;
	mesh->type_ = HRL_3D_MESH;
	mesh->triangle_count_ = _indexCount > 0 ? (_indexCount / 3u) : (_vertexCount / 3u);

	HRL_MeshLODData baseLOD;
	baseLOD.vertices.assign(_vertices, _vertices + _vertexCount);
	if (_indexCount > 0) baseLOD.indices.assign(_indices, _indices + _indexCount);
	else {
		baseLOD.indices.resize(_vertexCount);
		for (size_t i = 0; i < _vertexCount; ++i) baseLOD.indices[i] = (HRL_uint)i;
	}
	mesh->lods_.push_back(std::move(baseLOD));

	glm::vec3 minPoint(std::numeric_limits<float>::max());
	glm::vec3 maxPoint(std::numeric_limits<float>::lowest());
	bool validBounds = true;
	for (size_t i = 0; i < _vertexCount; ++i)
	{
		const glm::vec3 p(_vertices[i].position[0], _vertices[i].position[1], _vertices[i].position[2]);
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
		{
			validBounds = false;
			break;
		}
		minPoint = glm::min(minPoint, p);
		maxPoint = glm::max(maxPoint, p);
	}
	if (validBounds)
	{
		mesh->bounds_center_ = (minPoint + maxPoint) * 0.5f;
		float radius2 = 0.f;
		for (size_t i = 0; i < _vertexCount; ++i)
		{
			const glm::vec3 p(_vertices[i].position[0], _vertices[i].position[1], _vertices[i].position[2]);
			radius2 = std::max(radius2, glm::dot(p - mesh->bounds_center_, p - mesh->bounds_center_));
		}
		mesh->bounds_radius_ = std::sqrt(radius2);
	}
	it_scene->second->meshes.emplace(newId, mesh);
	MarkSceneGIGeometryDirty(it_scene->second);
	ctx_.meshes.emplace(newId, mesh);

	return newId;
}

namespace {

static glm::vec3 LODPosition(const HRL_Vertex3D& v) { return glm::vec3(v.position[0],v.position[1],v.position[2]); }
static glm::vec3 LODNormal(const HRL_Vertex3D& v) { return glm::vec3(v.normal[0],v.normal[1],v.normal[2]); }
static glm::vec3 LODTangent(const HRL_Vertex3D& v) { return glm::vec3(v.tangent[0],v.tangent[1],v.tangent[2]); }
static glm::vec3 LODBitangent(const HRL_Vertex3D& v) { return glm::vec3(v.bitangent[0],v.bitangent[1],v.bitangent[2]); }
static glm::vec3 LODSafeNormalize(const glm::vec3& v,const glm::vec3& fallback){float l2=glm::dot(v,v);if(!std::isfinite(l2)||l2<1e-10f)return fallback;return v/std::sqrt(l2);}
static void LODSetVertex(HRL_Vertex3D& d,const glm::vec3&p,const glm::vec3&n,const glm::vec2&uv,const glm::vec3&t,const glm::vec3&b){d.position[0]=p.x;d.position[1]=p.y;d.position[2]=p.z;d.normal[0]=n.x;d.normal[1]=n.y;d.normal[2]=n.z;d.uv[0]=uv.x;d.uv[1]=uv.y;d.tangent[0]=t.x;d.tangent[1]=t.y;d.tangent[2]=t.z;d.bitangent[0]=b.x;d.bitangent[1]=b.y;d.bitangent[2]=b.z;}

static bool GenerateLODLevel(const HRL_MeshLODData& source, float ratio, HRL_MeshLODData& out)
{
	out.vertices.clear();
	out.indices.clear();
	if (source.vertices.size() < 3 || source.indices.size() < 3)
		return false;

	const double r = std::clamp((double)ratio, 0.01, 1.0);
	const size_t target = std::max<size_t>(
		3,
		std::min(source.vertices.size(),
			(size_t)std::llround((double)source.vertices.size() * r)));
	if (target >= source.vertices.size())
	{
		out = source;
		return true;
	}

	glm::vec3 minP(std::numeric_limits<float>::max());
	glm::vec3 maxP(std::numeric_limits<float>::lowest());
	for (const auto& v : source.vertices)
	{
		const glm::vec3 p = LODPosition(v);
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
			return false;
		minP = glm::min(minP, p);
		maxP = glm::max(maxP, p);
	}

	const glm::vec3 extent = glm::max(maxP - minP, glm::vec3(1e-5f));
	const double e[3] = { extent.x, extent.y, extent.z };
	int dims[3] = { 1, 1, 1 };
	bool fixed[3] = { false, false, false };

	// Allocate the target number of spatial cells according to object aspect
	// ratio. Thin geometry (planes/lines) keeps its thin axis at one cell rather
	// than wasting the 3D grid budget on empty volume.
	for (;;)
	{
		double activeProduct = 1.0;
		int activeCount = 0;
		double activeExtentProduct = 1.0;
		for (int axis = 0; axis < 3; ++axis)
		{
			if (!fixed[axis])
			{
				++activeCount;
				activeExtentProduct *= e[axis];
			}
		}
		(void)activeProduct;
		if (activeCount == 0)
			break;

		const double scale = std::pow((double)target / std::max(activeExtentProduct, 1e-30), 1.0 / activeCount);
		bool clampedAxis = false;
		for (int axis = 0; axis < 3; ++axis)
		{
			if (fixed[axis])
				continue;
			const double raw = scale * e[axis];
			if (raw < 1.0)
			{
				dims[axis] = 1;
				fixed[axis] = true;
				clampedAxis = true;
			}
		}
		if (!clampedAxis)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				if (!fixed[axis])
					dims[axis] = std::max(1, (int)std::lround(scale * e[axis]));
			}
			break;
		}
	}

	// Make the actual grid product close to the requested cell count. Rounding
	// can leave a few cells unused or create slightly more cells than requested;
	// that is intentional and keeps aspect ratio stable.
	const size_t maxDim = (size_t)std::numeric_limits<int>::max();
	for (int axis = 0; axis < 3; ++axis)
		dims[axis] = std::clamp(dims[axis], 1, (int)std::min(target, maxDim));

	struct Accumulator
	{
		glm::vec3 p{0}, n{0}, t{0}, b{0};
		glm::vec2 uv{0};
		uint32_t count = 0;
	};

	struct CellKey
	{
		int x, y, z;
		bool operator==(const CellKey& other) const
		{
			return x == other.x && y == other.y && z == other.z;
		}
	};
	struct CellKeyHash
	{
		size_t operator()(const CellKey& key) const noexcept
		{
			size_t h = std::hash<int>{}(key.x);
			h ^= std::hash<int>{}(key.y) + (h << 6) + (h >> 2);
			h ^= std::hash<int>{}(key.z) + (h << 6) + (h >> 2);
			return h;
		}
	};

	std::unordered_map<CellKey, uint32_t, CellKeyHash> cells;
	std::vector<Accumulator> acc;
	std::vector<uint32_t> map(source.vertices.size());
	cells.reserve(std::min(target * 2u, source.vertices.size() * 2u));
	acc.reserve(std::min(target, source.vertices.size()));

	auto key = [&](const glm::vec3& p)
	{
		const glm::vec3 q = (p - minP) / extent;
		const int x = std::clamp((int)std::floor(q.x * dims[0]), 0, dims[0] - 1);
		const int y = std::clamp((int)std::floor(q.y * dims[1]), 0, dims[1] - 1);
		const int z = std::clamp((int)std::floor(q.z * dims[2]), 0, dims[2] - 1);
		return CellKey{x, y, z};
	};

	for (size_t i = 0; i < source.vertices.size(); ++i)
	{
		const auto& v = source.vertices[i];
		auto [it, inserted] = cells.emplace(key(LODPosition(v)), (uint32_t)acc.size());
		if (inserted)
			acc.emplace_back();
		map[i] = it->second;
		Accumulator& a = acc[it->second];
		a.p += LODPosition(v);
		a.n += LODNormal(v);
		a.t += LODTangent(v);
		a.b += LODBitangent(v);
		a.uv += glm::vec2(v.uv[0], v.uv[1]);
		++a.count;
	}

	out.vertices.resize(acc.size());
	for (size_t i = 0; i < acc.size(); ++i)
	{
		const auto& a = acc[i];
		const float inv = 1.0f / std::max(1u, a.count);
		const glm::vec3 n = LODSafeNormalize(a.n * inv, {0, 1, 0});
		glm::vec3 t = a.t * inv;
		t -= n * glm::dot(n, t);
		t = LODSafeNormalize(t, {1, 0, 0});
		const glm::vec3 b = LODSafeNormalize(glm::cross(n, t), {0, 0, 1});
		LODSetVertex(out.vertices[i], a.p * inv, n, a.uv * inv, t, b);
	}

	std::set<std::tuple<HRL_uint, HRL_uint, HRL_uint>> seen;
	out.indices.reserve(source.indices.size());
	for (size_t i = 0; i + 2 < source.indices.size(); i += 3)
	{
		const HRL_uint a = map[source.indices[i]];
		const HRL_uint b = map[source.indices[i + 1]];
		const HRL_uint c = map[source.indices[i + 2]];
		if (a == b || b == c || a == c)
			continue;

		HRL_uint x = a, y = b, z = c;
		if (x > y) std::swap(x, y);
		if (y > z) std::swap(y, z);
		if (x > y) std::swap(x, y);
		if (!seen.emplace(x, y, z).second)
			continue;

		out.indices.push_back(a);
		out.indices.push_back(b);
		out.indices.push_back(c);
	}

	return out.indices.size() >= 3 && out.vertices.size() >= 3;
}

static bool RebuildLODs(HRL_Mesh* mesh)
{
	if(!mesh||mesh->type_==HRL_SPRITE||mesh->lods_.empty())return false;const size_t desired=std::max<size_t>(1,mesh->lod_levels_);HRL_MeshLODData base=mesh->lods_[0];mesh->lods_.clear();mesh->lods_.push_back(std::move(base));for(size_t level=1;level<desired;++level){HRL_MeshLODData lod;if(!GenerateLODLevel(mesh->lods_[0],std::pow(0.5f,(float)level),lod))break;if(lod.indices.size()>=mesh->lods_.back().indices.size())break;mesh->lods_.push_back(std::move(lod));}mesh->last_lod_level_=std::clamp(mesh->last_lod_level_,0,(int)mesh->lods_.size()-1);return true;
}

static glm::vec3 HRLFBX_ToVec3(const ufbx_vec3& v)
{
	return glm::vec3((float)v.x, (float)v.y, (float)v.z);
}

static glm::vec2 HRLFBX_ToVec2(const ufbx_vec2& v)
{
	return glm::vec2((float)v.x, (float)v.y);
}

static glm::vec3 HRLFBX_TransformPosition(const ufbx_matrix& matrix, const ufbx_vec3& value)
{
	return HRLFBX_ToVec3(ufbx_transform_position(&matrix, value));
}

static glm::vec3 HRLFBX_TransformDirection(const ufbx_matrix& matrix, const ufbx_vec3& value)
{
	return HRLFBX_ToVec3(ufbx_transform_direction(&matrix, value));
}

static glm::vec3 HRLFBX_NormalizeOrFallback(const glm::vec3& value, const glm::vec3& fallback)
{
	const float length2 = glm::dot(value, value);
	if (!std::isfinite(length2) || length2 <= 0.0000001f)
		return fallback;
	return glm::normalize(value);
}

static void HRLFBX_BuildFallbackTangentSpace(
	const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
	const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec2& uv2,
	const glm::vec3& normal,
	glm::vec3& tangent,
	glm::vec3& bitangent)
{
	const glm::vec3 edge1 = p1 - p0;
	const glm::vec3 edge2 = p2 - p0;
	const glm::vec2 duv1 = uv1 - uv0;
	const glm::vec2 duv2 = uv2 - uv0;

	const float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
	if (std::isfinite(determinant) && std::fabs(determinant) > 0.000001f)
	{
		const float inv = 1.0f / determinant;
		tangent = (edge1 * duv2.y - edge2 * duv1.y) * inv;
		bitangent = (edge2 * duv1.x - edge1 * duv2.x) * inv;
	}
	else
	{
		const glm::vec3 reference = std::fabs(normal.y) < 0.99f
			? glm::vec3(0.f, 1.f, 0.f)
			: glm::vec3(1.f, 0.f, 0.f);
		tangent = glm::cross(reference, normal);
		bitangent = glm::cross(normal, tangent);
	}

	tangent = HRLFBX_NormalizeOrFallback(tangent, glm::vec3(1.f, 0.f, 0.f));
	tangent = HRLFBX_NormalizeOrFallback(
		tangent - normal * glm::dot(normal, tangent),
		glm::vec3(1.f, 0.f, 0.f));
	bitangent = HRLFBX_NormalizeOrFallback(bitangent, glm::cross(normal, tangent));
}

static bool HRLFBX_AppendMesh(
	std::vector<HRL_Vertex3D>& output,
	const ufbx_mesh* mesh,
	const ufbx_matrix& geometry_to_world)
{
	if (!mesh)
		return false;

	const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&geometry_to_world);

	if (mesh->num_triangles > 0)
		output.reserve(output.size() + mesh->num_triangles * 3);

	if (mesh->max_face_triangles == 0)
		return false;

	std::vector<uint32_t> triangleIndices((size_t)mesh->max_face_triangles * 3u);

	// HRL_Vertex3D has no material information, so the converter deliberately
	// walks the complete polygon list instead of splitting by FBX material parts.
	// ufbx_triangulate_face() returns the number of triangles, not the number of
	// written indices.
	for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
	{
		const ufbx_face& face = mesh->faces.data[faceIndex];
		if (face.num_indices < 3)
			continue;

		const uint32_t triangleCount = ufbx_triangulate_face(
			triangleIndices.data(), triangleIndices.size(), mesh, face);
		if (triangleCount == 0)
			continue;

		for (uint32_t tri = 0; tri < triangleCount; ++tri)
		{
			const size_t triangleBase = (size_t)tri * 3u;
			HRL_Vertex3D vertices[3]{};
			glm::vec3 positions[3];
			glm::vec3 normals[3];
			glm::vec2 uvs[3];
			glm::vec3 tangents[3];
			glm::vec3 bitangents[3];

			for (size_t corner = 0; corner < 3; ++corner)
			{
				const uint32_t index = triangleIndices[triangleBase + corner];
				if ((size_t)index >= mesh->num_indices)
				{
					positions[corner] = glm::vec3(0.f);
					normals[corner] = glm::vec3(0.f);
					uvs[corner] = glm::vec2(0.f);
					tangents[corner] = glm::vec3(0.f);
					bitangents[corner] = glm::vec3(0.f);
					continue;
				}

				ufbx_vec3 position{};
				if (mesh->vertex_position.exists)
				{
					position = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
				}
				else
				{
					const uint32_t vertexIndex = mesh->vertex_indices.data[index];
					if ((size_t)vertexIndex >= mesh->vertices.count)
						return false;
					position = mesh->vertices.data[vertexIndex];
				}

				positions[corner] = HRLFBX_TransformPosition(geometry_to_world, position);

				if (mesh->vertex_normal.exists)
				{
					normals[corner] = HRLFBX_TransformDirection(
						normalMatrix,
						ufbx_get_vertex_vec3(&mesh->vertex_normal, index));
				}
				else
				{
					normals[corner] = glm::vec3(0.f);
				}

				uvs[corner] = mesh->vertex_uv.exists
					? HRLFBX_ToVec2(ufbx_get_vertex_vec2(&mesh->vertex_uv, index))
					: glm::vec2(0.f);

				tangents[corner] = mesh->vertex_tangent.exists
					? HRLFBX_TransformDirection(
						geometry_to_world,
						ufbx_get_vertex_vec3(&mesh->vertex_tangent, index))
					: glm::vec3(0.f);

				bitangents[corner] = mesh->vertex_bitangent.exists
					? HRLFBX_TransformDirection(
						geometry_to_world,
						ufbx_get_vertex_vec3(&mesh->vertex_bitangent, index))
					: glm::vec3(0.f);
			}

			const glm::vec3 faceNormal = HRLFBX_NormalizeOrFallback(
				glm::cross(positions[1] - positions[0], positions[2] - positions[0]),
				glm::vec3(0.f, 1.f, 0.f));

			for (size_t corner = 0; corner < 3; ++corner)
				normals[corner] = HRLFBX_NormalizeOrFallback(normals[corner], faceNormal);

			glm::vec3 fallbackTangent;
			glm::vec3 fallbackBitangent;
			HRLFBX_BuildFallbackTangentSpace(
				positions[0], positions[1], positions[2],
				uvs[0], uvs[1], uvs[2], normals[0],
				fallbackTangent, fallbackBitangent);

			for (size_t corner = 0; corner < 3; ++corner)
			{
				glm::vec3 tangent = mesh->vertex_tangent.exists
					? tangents[corner] : fallbackTangent;
				tangent = HRLFBX_NormalizeOrFallback(tangent, fallbackTangent);
				tangent = HRLFBX_NormalizeOrFallback(
					tangent - normals[corner] * glm::dot(normals[corner], tangent),
					fallbackTangent);

				const glm::vec3 expectedBitangent = glm::cross(normals[corner], tangent);
				glm::vec3 bitangent = mesh->vertex_bitangent.exists
					? bitangents[corner] : fallbackBitangent;
				bitangent = HRLFBX_NormalizeOrFallback(bitangent, expectedBitangent);
				if (glm::dot(expectedBitangent, bitangent) < 0.f)
					bitangent = -expectedBitangent;

				vertices[corner].position[0] = positions[corner].x;
				vertices[corner].position[1] = positions[corner].y;
				vertices[corner].position[2] = positions[corner].z;
				vertices[corner].normal[0] = normals[corner].x;
				vertices[corner].normal[1] = normals[corner].y;
				vertices[corner].normal[2] = normals[corner].z;
				vertices[corner].uv[0] = uvs[corner].x;
				vertices[corner].uv[1] = uvs[corner].y;
				vertices[corner].tangent[0] = tangent.x;
				vertices[corner].tangent[1] = tangent.y;
				vertices[corner].tangent[2] = tangent.z;
				vertices[corner].bitangent[0] = bitangent.x;
				vertices[corner].bitangent[1] = bitangent.y;
				vertices[corner].bitangent[2] = bitangent.z;
			}

			output.push_back(vertices[0]);
			output.push_back(vertices[1]);
			output.push_back(vertices[2]);
		}
	}

	return true;
}

}

HRL_Vertex3D* HRL_GetVertex3DFromFBX(const char* _data, size_t _bufferSize, size_t* _vertexCount)
{
	if (_vertexCount)
		*_vertexCount = 0;

	if (!_data || _bufferSize == 0 || !_vertexCount)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_GetVertex3DFromFBX: data, buffer size and vertex count output must be valid");
		return nullptr;
	}

	ufbx_load_opts options{};
	options.file_format = UFBX_FILE_FORMAT_FBX;
	options.generate_missing_normals = true;
	options.normalize_normals = true;
	options.load_external_files = true;
	options.ignore_missing_external_files = true;
	options.evaluate_skinning = true;
	options.evaluate_caches = true;
	options.ignore_geometry = false;
	options.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
	options.target_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
	options.target_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;
	options.target_unit_meters = 1.0;

	ufbx_error error{};
	ufbx_scene* scene = ufbx_load_memory(_data, _bufferSize, &options, &error);
	if (!scene)
	{
		std::string detail = "HRL_GetVertex3DFromFBX: failed to decode FBX file";
		if (error.description.data && error.description.data[0] != '\0')
		{
			detail += " (";
			detail += error.description.data;
			detail += ")";
		}
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, detail.c_str());
		return nullptr;
	}

	HRL_Vertex3D* result = nullptr;
	try
	{
		std::vector<HRL_Vertex3D> vertices;
		size_t meshesWithFaces = 0;
		size_t meshesWithTriangles = 0;
		size_t meshParts = 0;
		size_t totalFaces = 0;
		size_t totalTriangles = 0;

		std::unordered_set<const ufbx_mesh*> convertedMeshes;

		// Convert every mesh attached to a node. Visibility is a rendering
		// concern and must not prevent extraction from an FBX file.
		for (size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex)
		{
			const ufbx_node* node = scene->nodes.data[nodeIndex];
			if (!node || !node->mesh)
				continue;

			const ufbx_mesh* mesh = node->mesh;
			convertedMeshes.insert(mesh);
			meshesWithFaces += mesh->num_faces > 0 ? 1 : 0;
			meshesWithTriangles += mesh->num_triangles > 0 ? 1 : 0;
			totalFaces += mesh->num_faces;
			totalTriangles += mesh->num_triangles;
			meshParts += mesh->material_parts.count;

			HRLFBX_AppendMesh(vertices, mesh, node->geometry_to_world);
		}

		// Handle mesh elements which have no node connection.
		for (size_t meshIndex = 0; meshIndex < scene->meshes.count; ++meshIndex)
		{
			const ufbx_mesh* mesh = scene->meshes.data[meshIndex];
			if (!mesh || convertedMeshes.find(mesh) != convertedMeshes.end())
				continue;

			meshesWithFaces += mesh->num_faces > 0 ? 1 : 0;
			meshesWithTriangles += mesh->num_triangles > 0 ? 1 : 0;
			totalFaces += mesh->num_faces;
			totalTriangles += mesh->num_triangles;
			meshParts += mesh->material_parts.count;

			HRLFBX_AppendMesh(vertices, mesh, ufbx_identity_matrix);
		}

		if (vertices.empty())
		{
			char detail[512];
			snprintf(detail, sizeof(detail),
				"HRL_GetVertex3DFromFBX: no convertible polygon geometry "
				"(meshes=%zu, nodes=%zu, meshes_with_faces=%zu, meshes_with_triangles=%zu, "
				"mesh_parts=%zu, faces=%zu, triangles=%zu)",
				scene->meshes.count, scene->nodes.count,
				meshesWithFaces, meshesWithTriangles, meshParts, totalFaces, totalTriangles);
			ufbx_free_scene(scene);
			SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, detail);
			return nullptr;
		}

		result = new (std::nothrow) HRL_Vertex3D[vertices.size()];
		if (!result)
		{
			ufbx_free_scene(scene);
			SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
				"HRL_GetVertex3DFromFBX: failed to allocate vertex buffer");
			return nullptr;
		}

		std::copy(vertices.begin(), vertices.end(), result);
		*_vertexCount = vertices.size();
	}
	catch (const std::bad_alloc&)
	{
		delete[] result;
		ufbx_free_scene(scene);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
			"HRL_GetVertex3DFromFBX: failed to allocate temporary conversion data");
		return nullptr;
	}

	ufbx_free_scene(scene);
	return result;
}

void HRL_FreeVertex3DFromFBX(HRL_Vertex3D* _vertices)
{
	delete[] _vertices;
}

namespace {

static HRL_SkeletalBoneTransform HRLSkeletal_TransformFromUFBX(const ufbx_transform& transform)
{
	HRL_SkeletalBoneTransform result{};
	result.translation[0] = (float)transform.translation.x;
	result.translation[1] = (float)transform.translation.y;
	result.translation[2] = (float)transform.translation.z;
	result.rotation[0] = (float)transform.rotation.x;
	result.rotation[1] = (float)transform.rotation.y;
	result.rotation[2] = (float)transform.rotation.z;
	result.rotation[3] = (float)transform.rotation.w;
	result.scale[0] = (float)transform.scale.x;
	result.scale[1] = (float)transform.scale.y;
	result.scale[2] = (float)transform.scale.z;
	return result;
}

static glm::mat4 HRLSkeletal_TransformToMatrix(const HRL_SkeletalBoneTransform& transform)
{
	const glm::vec3 translation(transform.translation[0], transform.translation[1], transform.translation[2]);
	const glm::quat rotation(
		transform.rotation[3],
		transform.rotation[0],
		transform.rotation[1],
		transform.rotation[2]);
	const glm::vec3 scale(transform.scale[0], transform.scale[1], transform.scale[2]);
	glm::mat4 matrix(1.f);
	matrix = glm::translate(matrix, translation);
	matrix *= glm::mat4_cast(rotation);
	matrix = glm::scale(matrix, scale);
	return matrix;
}

static glm::mat4 HRLSkeletal_UFBXMatrixToGLM(const ufbx_matrix& source)
{
	glm::mat4 result(1.f);
	result[0] = glm::vec4((float)source.cols[0].x, (float)source.cols[0].y, (float)source.cols[0].z, 0.f);
	result[1] = glm::vec4((float)source.cols[1].x, (float)source.cols[1].y, (float)source.cols[1].z, 0.f);
	result[2] = glm::vec4((float)source.cols[2].x, (float)source.cols[2].y, (float)source.cols[2].z, 0.f);
	result[3] = glm::vec4((float)source.cols[3].x, (float)source.cols[3].y, (float)source.cols[3].z, 1.f);
	return result;
}

static glm::mat4 HRLSkeletal_ArrayToGLM(const float source[16])
{
	glm::mat4 result(1.f);
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			result[col][row] = source[col * 4 + row];
	return result;
}

static glm::mat4 HRLSkeletal_LerpMatrix(const glm::mat4& a, const glm::mat4& b, float alpha)
{
	const float invAlpha = 1.0f - alpha;
	glm::mat4 result(0.f);
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			result[col][row] = a[col][row] * invAlpha + b[col][row] * alpha;
	return result;
}

static void HRLSkeletal_SetDefaultTransform(HRL_SkeletalBoneTransform& transform)
{
	transform.translation[0] = 0.f;
	transform.translation[1] = 0.f;
	transform.translation[2] = 0.f;
	transform.rotation[0] = 0.f;
	transform.rotation[1] = 0.f;
	transform.rotation[2] = 0.f;
	transform.rotation[3] = 1.f;
	transform.scale[0] = 1.f;
	transform.scale[1] = 1.f;
	transform.scale[2] = 1.f;
}

static HRL_SkeletalBoneTransform HRLSkeletal_LerpTransform(
	const HRL_SkeletalBoneTransform& a,
	const HRL_SkeletalBoneTransform& b,
	float alpha)
{
	HRL_SkeletalBoneTransform result{};
	const glm::vec3 ta(a.translation[0], a.translation[1], a.translation[2]);
	const glm::vec3 tb(b.translation[0], b.translation[1], b.translation[2]);
	const glm::vec3 sa(a.scale[0], a.scale[1], a.scale[2]);
	const glm::vec3 sb(b.scale[0], b.scale[1], b.scale[2]);
	const glm::quat qa(a.rotation[3], a.rotation[0], a.rotation[1], a.rotation[2]);
	const glm::quat qb(b.rotation[3], b.rotation[0], b.rotation[1], b.rotation[2]);
	const glm::vec3 t = glm::mix(ta, tb, alpha);
	const glm::vec3 s = glm::mix(sa, sb, alpha);
	const glm::quat q = glm::normalize(glm::slerp(qa, qb, alpha));

	result.translation[0] = t.x;
	result.translation[1] = t.y;
	result.translation[2] = t.z;
	result.rotation[0] = q.x;
	result.rotation[1] = q.y;
	result.rotation[2] = q.z;
	result.rotation[3] = q.w;
	result.scale[0] = s.x;
	result.scale[1] = s.y;
	result.scale[2] = s.z;
	return result;
}

static void HRLSkeletal_BuildPoseFramesForAnimation(
    HRL_SkeletalMesh* mesh,
    HRL_SkeletalAnimationInternal& animation)
{
    if (!mesh || mesh->bones_.empty() || animation.public_.frameCount == 0 || animation.frames_.empty())
    {
        animation.pose_frames_.clear();
        return;
    }

    const size_t boneCount = mesh->bones_.size();
    const size_t frameCount = animation.public_.frameCount;
    animation.pose_frames_.assign(frameCount * boneCount, glm::mat4(1.f));

    if (animation.pose_matrices_storage_.size() == frameCount * boneCount * 16u)
    {
        for (size_t frame = 0; frame < frameCount; ++frame)
        {
            for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
            {
                const float* src = animation.pose_matrices_storage_.data() + (frame * boneCount + boneIndex) * 16u;
                animation.pose_frames_[frame * boneCount + boneIndex] = HRLSkeletal_ArrayToGLM(src);
            }
        }
        return;
    }

    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        std::vector<glm::mat4> world(boneCount, glm::mat4(1.f));
        std::vector<unsigned char> built(boneCount, 0);

        std::function<glm::mat4(size_t)> buildBone = [&](size_t boneIndex) -> glm::mat4
        {
            if (built[boneIndex])
                return world[boneIndex];

            const HRL_SkeletalBoneInternal& bone = mesh->bones_[boneIndex];
            const size_t offset = frame * boneCount + boneIndex;
            glm::mat4 local = HRLSkeletal_TransformToMatrix(animation.frames_[offset]);
            glm::mat4 parentWorld(1.f);
            if (bone.public_.parentIndex < boneCount && bone.public_.parentIndex != boneIndex)
                parentWorld = buildBone((size_t)bone.public_.parentIndex);

            world[boneIndex] = parentWorld * local;
            built[boneIndex] = 1;
            return world[boneIndex];
        };

        for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
        {
            const glm::mat4 boneWorld = buildBone(boneIndex);
            animation.pose_frames_[frame * boneCount + boneIndex] =
                boneWorld * mesh->bones_[boneIndex].inverse_bind_;
        }
    }
}

static bool HRLSkeletal_UpdatePose(HRL_SkeletalMesh* mesh)
{
	if (!mesh || mesh->bones_.empty())
		return false;

	mesh->bone_matrices_.resize(mesh->bones_.size(), glm::mat4(1.f));
	const HRL_SkeletalAnimationInternal* animation = nullptr;
	if (mesh->current_animation_ >= 0 && (size_t)mesh->current_animation_ < mesh->animations_.size())
		animation = &mesh->animations_[(size_t)mesh->current_animation_];

	if (animation && !animation->pose_frames_.empty() && animation->public_.frameCount > 0)
	{
		const float duration = std::max(animation->public_.duration, 0.f);
		float time = std::clamp(mesh->animation_time_, 0.f, duration);
		const float framePosition = duration > 0.f
			? time * animation->public_.frameRate
			: 0.f;
		const size_t frame0 = std::min((size_t)std::floor(framePosition), animation->public_.frameCount - 1);
		const size_t frame1 = std::min(frame0 + 1, animation->public_.frameCount - 1);
		const float alpha = frame1 == frame0 ? 0.f : framePosition - (float)frame0;
		for (size_t boneIndex = 0; boneIndex < mesh->bones_.size(); ++boneIndex)
		{
			const size_t offset0 = frame0 * mesh->bones_.size() + boneIndex;
			const size_t offset1 = frame1 * mesh->bones_.size() + boneIndex;
			mesh->bone_matrices_[boneIndex] = HRLSkeletal_LerpMatrix(animation->pose_frames_[offset0], animation->pose_frames_[offset1], alpha);
		}
	}
	else if (!mesh->animations_.empty() && !mesh->animations_.front().pose_frames_.empty())
	{
		// Imported skeletal meshes keep their bind/first-frame skin matrices in
		// world-space form, matching ufbx's geometry_to_world skinning convention.
		const auto& firstPose = mesh->animations_.front().pose_frames_;
		for (size_t boneIndex = 0; boneIndex < mesh->bones_.size(); ++boneIndex)
			mesh->bone_matrices_[boneIndex] = firstPose[boneIndex];
	}
	else
	{
		for (size_t boneIndex = 0; boneIndex < mesh->bones_.size(); ++boneIndex)
			mesh->bone_matrices_[boneIndex] = mesh->bones_[boneIndex].bind_world_ * mesh->bones_[boneIndex].inverse_bind_;
	}

	++mesh->pose_serial_;
	if (mesh->pose_serial_ == 0)
		mesh->pose_serial_ = 1;
	return true;
}

static char* HRLSkeletal_CopyCString(const ufbx_string& string)
{
	char* result = new char[string.length + 1];
	if (string.length > 0)
		std::memcpy(result, string.data, string.length);
	result[string.length] = '\0';
	return result;
}

static void HRLSkeletal_DestroyMeshData(HRL_SkeletalMeshData* data)
{
	if (!data) return;
	if (data->bones)
	{
		for (size_t i = 0; i < data->boneCount; ++i)
			delete[] const_cast<char*>(data->bones[i].name);
		delete[] data->bones;
	}
	if (data->animations)
	{
		for (size_t i = 0; i < data->animationCount; ++i)
		{
			delete[] const_cast<char*>(data->animations[i].name);
			delete[] data->animations[i].frames;
			delete[] data->animations[i].poseMatrices;
		}
		delete[] data->animations;
	}
	delete[] data->vertices;
	delete[] data->indices;
	delete data;
}

static bool HRLSkeletal_FillVertex(
	HRL_SkeletalVertex& output,
	const ufbx_mesh* mesh,
	const ufbx_skin_deformer* skin,
	const std::vector<HRL_uint>& clusterToBone,
	uint32_t meshIndex)
{
	std::memset(&output, 0, sizeof(output));
	const uint32_t vertexIndex = mesh->vertex_indices.data[meshIndex];
	if ((size_t)vertexIndex >= mesh->vertices.count)
		return false;

	ufbx_vec3 position = mesh->vertices.data[vertexIndex];
	if (mesh->vertex_position.exists)
		position = ufbx_get_vertex_vec3(&mesh->vertex_position, meshIndex);
	output.vertex.position[0] = (float)position.x;
	output.vertex.position[1] = (float)position.y;
	output.vertex.position[2] = (float)position.z;

	if (mesh->vertex_normal.exists)
	{
		const ufbx_vec3 normal = ufbx_get_vertex_vec3(&mesh->vertex_normal, meshIndex);
		output.vertex.normal[0] = (float)normal.x;
		output.vertex.normal[1] = (float)normal.y;
		output.vertex.normal[2] = (float)normal.z;
	}
	else
	{
		output.vertex.normal[1] = 1.f;
	}

	if (mesh->vertex_uv.exists)
	{
		const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, meshIndex);
		output.vertex.uv[0] = (float)uv.x;
		output.vertex.uv[1] = (float)uv.y;
	}

	if (mesh->vertex_tangent.exists)
	{
		const ufbx_vec3 tangent = ufbx_get_vertex_vec3(&mesh->vertex_tangent, meshIndex);
		output.vertex.tangent[0] = (float)tangent.x;
		output.vertex.tangent[1] = (float)tangent.y;
		output.vertex.tangent[2] = (float)tangent.z;
	}
	if (mesh->vertex_bitangent.exists)
	{
		const ufbx_vec3 bitangent = ufbx_get_vertex_vec3(&mesh->vertex_bitangent, meshIndex);
		output.vertex.bitangent[0] = (float)bitangent.x;
		output.vertex.bitangent[1] = (float)bitangent.y;
		output.vertex.bitangent[2] = (float)bitangent.z;
	}

	if (!mesh->vertex_tangent.exists || !mesh->vertex_bitangent.exists)
	{
		HRL_Vertex3D tri[3]{};
		// Tangent fallback is completed at the triangle level by the caller. The
		// zeroed tangent space here makes the absence explicit without inventing UVs.
		(void)tri;
	}

	if (skin && (size_t)vertexIndex < skin->vertices.count)
	{
		const ufbx_skin_vertex& skinVertex = skin->vertices.data[vertexIndex];
		const size_t weightEnd = std::min(
			(size_t)skinVertex.weight_begin + (size_t)skinVertex.num_weights,
			skin->weights.count);

		struct Influence {
			HRL_uint bone;
			float weight;
		};
		std::vector<Influence> influences;
		influences.reserve((size_t)skinVertex.num_weights);
		for (size_t weightIndex = skinVertex.weight_begin; weightIndex < weightEnd; ++weightIndex)
		{
			const ufbx_skin_weight& weight = skin->weights.data[weightIndex];
			if ((size_t)weight.cluster_index >= clusterToBone.size())
				continue;
			const HRL_uint boneIndex = clusterToBone[weight.cluster_index];
			if (boneIndex == HRL_INVALID_ID)
				continue;
			const float w = (float)weight.weight;
			if (!std::isfinite(w) || w <= 0.f)
				continue;
			influences.push_back({boneIndex, w});
		}

		std::sort(influences.begin(), influences.end(), [](const Influence& a, const Influence& b) {
			return a.weight > b.weight;
		});

		const size_t influenceCount = std::min(influences.size(), (size_t)HRL_SKELETAL_MAX_INFLUENCES);
		float weightSum = 0.f;
		for (size_t i = 0; i < influenceCount; ++i)
		{
			output.boneIndices[i] = influences[i].bone;
			output.boneWeights[i] = influences[i].weight;
			weightSum += influences[i].weight;
		}
		if (weightSum > 0.f)
		{
			for (size_t i = 0; i < influenceCount; ++i)
				output.boneWeights[i] /= weightSum;
		}
		else if (skin->clusters.count > 0 && !clusterToBone.empty())
		{
			output.boneIndices[0] = clusterToBone[0] == HRL_INVALID_ID ? 0u : clusterToBone[0];
			output.boneWeights[0] = 1.f;
		}
	}
	return true;
}

static void HRLSkeletal_ComputeMissingTangentSpace(
	HRL_SkeletalVertex& a, HRL_SkeletalVertex& b, HRL_SkeletalVertex& c)
{
	const glm::vec3 p0(a.vertex.position[0], a.vertex.position[1], a.vertex.position[2]);
	const glm::vec3 p1(b.vertex.position[0], b.vertex.position[1], b.vertex.position[2]);
	const glm::vec3 p2(c.vertex.position[0], c.vertex.position[1], c.vertex.position[2]);
	const glm::vec2 uv0(a.vertex.uv[0], a.vertex.uv[1]);
	const glm::vec2 uv1(b.vertex.uv[0], b.vertex.uv[1]);
	const glm::vec2 uv2(c.vertex.uv[0], c.vertex.uv[1]);
	glm::vec3 normalA(a.vertex.normal[0], a.vertex.normal[1], a.vertex.normal[2]);
	const glm::vec3 faceNormal = HRLFBX_NormalizeOrFallback(glm::cross(p1-p0, p2-p0), glm::vec3(0,1,0));
	for (glm::vec3* n : {&normalA})
		*n = HRLFBX_NormalizeOrFallback(*n, faceNormal);

	glm::vec3 tangent, bitangent;
	HRLFBX_BuildFallbackTangentSpace(p0,p1,p2,uv0,uv1,uv2,normalA,tangent,bitangent);
	for (HRL_SkeletalVertex* v : {&a,&b,&c})
	{
		glm::vec3 n(v->vertex.normal[0], v->vertex.normal[1], v->vertex.normal[2]);
		n = HRLFBX_NormalizeOrFallback(n, faceNormal);
		v->vertex.normal[0]=n.x; v->vertex.normal[1]=n.y; v->vertex.normal[2]=n.z;
		glm::vec3 t(v->vertex.tangent[0], v->vertex.tangent[1], v->vertex.tangent[2]);
		if (glm::dot(t,t) < 1e-8f) t=tangent;
		t = HRLFBX_NormalizeOrFallback(t - n*glm::dot(n,t), tangent);
		glm::vec3 bt(v->vertex.bitangent[0], v->vertex.bitangent[1], v->vertex.bitangent[2]);
		const glm::vec3 expected = glm::normalize(glm::cross(n,t));
		if (glm::dot(bt,bt) < 1e-8f) bt=expected;
		bt = HRLFBX_NormalizeOrFallback(bt, expected);
		if (glm::dot(bt, expected) < 0.f) bt=-expected;
		v->vertex.tangent[0]=t.x; v->vertex.tangent[1]=t.y; v->vertex.tangent[2]=t.z;
		v->vertex.bitangent[0]=bt.x; v->vertex.bitangent[1]=bt.y; v->vertex.bitangent[2]=bt.z;
	}
}

} // anonymous namespace

HRL_SkeletalMeshData* HRL_GetSkeletalMeshFromFBX(const char* _data, size_t _bufferSize)
{
	if (!_data || _bufferSize == 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: data and buffer size must be valid");
		return nullptr;
	}

	ufbx_load_opts options{};
	options.file_format = UFBX_FILE_FORMAT_FBX;
	options.generate_missing_normals = true;
	options.normalize_normals = true;
	options.load_external_files = true;
	options.ignore_missing_external_files = true;
	options.evaluate_skinning = true;
	options.evaluate_caches = false;
	options.ignore_geometry = false;
	options.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
	options.target_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
	options.target_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;
	options.target_unit_meters = 1.0;

	ufbx_error error{};
	ufbx_scene* scene = ufbx_load_memory(_data, _bufferSize, &options, &error);
	if (!scene)
	{
		std::string detail = "HRL_GetSkeletalMeshFromFBX: failed to decode FBX file";
		if (error.description.data && error.description.data[0] != '\0')
		{
			detail += " (";
			detail += error.description.data;
			detail += ")";
		}
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, detail.c_str());
		return nullptr;
	}

	// A FBX file may contain several meshes attached to the same skeleton.
	// This API represents a single HRL skeletal mesh, so pick the largest skinned
	// geometry instead of depending on FBX object ordering (which often puts
	// small accessory meshes before the main character). The diagnostic below
	// makes the limitation explicit instead of silently hiding it.
	const ufbx_mesh* mesh = nullptr;
	size_t skinnedMeshCount = 0;
	size_t largestTriangles = 0;
	for (size_t i = 0; i < scene->meshes.count; ++i)
	{
		const ufbx_mesh* candidate = scene->meshes.data[i];
		if (!candidate || candidate->skin_deformers.count == 0 || candidate->num_triangles == 0)
			continue;
		++skinnedMeshCount;
		if (!mesh || candidate->num_triangles > largestTriangles)
		{
			mesh = candidate;
			largestTriangles = candidate->num_triangles;
		}
	}
	if (skinnedMeshCount > 1)
	{
		std::printf(
			"HRL_GetSkeletalMeshFromFBX: FBX contains %zu skinned meshes; importing the largest (%zu triangles) with the single-mesh API.\n",
			skinnedMeshCount, largestTriangles);
	}
	if (!mesh)
	{
		ufbx_free_scene(scene);
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: FBX scene contains no skinned mesh geometry");
		return nullptr;
	}

	const ufbx_node* meshNode = nullptr;
	for (size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex)
	{
		const ufbx_node* candidate = scene->nodes.data[nodeIndex];
		if (candidate && candidate->mesh == mesh)
		{
			meshNode = candidate;
			break;
		}
	}
	if (!meshNode && mesh->instances.count > 0)
		meshNode = mesh->instances.data[0];

	const ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
	if (!skin || skin->clusters.count == 0)
	{
		ufbx_free_scene(scene);
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: mesh has no skin clusters");
		return nullptr;
	}

	try
	{
		std::unordered_map<const ufbx_node*, bool> nodeSet;
		std::unordered_map<const ufbx_node*, const ufbx_skin_cluster*> clusterByNode;
		std::vector<const ufbx_node*> boneNodes;
		for (size_t i = 0; i < skin->clusters.count; ++i)
		{
			const ufbx_skin_cluster* cluster = skin->clusters.data[i];
			if (!cluster || !cluster->bone_node)
				continue;
			clusterByNode.emplace(cluster->bone_node, cluster);
			for (ufbx_node* node = cluster->bone_node; node; node = node->parent)
			{
				if (!nodeSet.emplace(node, true).second)
					break;
				boneNodes.push_back(node);
			}
		}
		if (boneNodes.empty())
		{
			ufbx_free_scene(scene);
			SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: skin has no valid bone nodes");
			return nullptr;
		}
		std::stable_sort(boneNodes.begin(), boneNodes.end(), [](const ufbx_node* a, const ufbx_node* b){
			if (a->node_depth != b->node_depth) return a->node_depth < b->node_depth;
			if (a->name.length != b->name.length) return a->name.length < b->name.length;
			return std::strncmp(a->name.data, b->name.data, a->name.length) < 0;
		});
		if (boneNodes.size() > HRL_MAX_SKELETAL_BONES)
		{
			ufbx_free_scene(scene);
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: skeleton exceeds HRL_MAX_SKELETAL_BONES");
			return nullptr;
		}

		std::unordered_map<const ufbx_node*, HRL_uint> nodeToBone;
		for (size_t i = 0; i < boneNodes.size(); ++i)
			nodeToBone[boneNodes[i]] = (HRL_uint)i;

		std::vector<HRL_uint> clusterToBone(skin->clusters.count, HRL_INVALID_ID);
		for (size_t i = 0; i < skin->clusters.count; ++i)
		{
			const ufbx_skin_cluster* cluster = skin->clusters.data[i];
			if (!cluster || !cluster->bone_node) continue;
			auto it = nodeToBone.find(cluster->bone_node);
			if (it != nodeToBone.end()) clusterToBone[i] = it->second;
		}

		// Keep mesh vertices in ufbx's native geometry space. IMPORTANT: the
		// skinning matrices themselves are world-space matrices. This is the same
		// convention used by the official ufbx viewer: for each influence,
		// currentBoneWorld * geometry_to_bone is applied directly to the geometry
		// vertex. Therefore the HRL object/model transform must NOT apply the FBX
		// geometry_to_world a second time.

		std::vector<HRL_SkeletalVertex> vertices;
		std::vector<HRL_uint> indices;
		vertices.reserve(mesh->num_triangles * 3u);
		indices.reserve(mesh->num_triangles * 3u);
		const size_t triangleCapacity = (size_t)std::max<uint32_t>(1u, mesh->max_face_triangles) * 3u;
		std::vector<uint32_t> triangles(triangleCapacity);
		for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
		{
			const ufbx_face& face = mesh->faces.data[faceIndex];
			if (face.num_indices < 3) continue;
			const uint32_t triangleCount = ufbx_triangulate_face(triangles.data(), triangles.size(), mesh, face);
			for (uint32_t tri = 0; tri < triangleCount; ++tri)
			{
				HRL_SkeletalVertex triVertices[3]{};
				bool valid = true;
				for (size_t corner = 0; corner < 3; ++corner)
				{
					const uint32_t meshIndex = triangles[(size_t)tri * 3u + corner];
					if ((size_t)meshIndex >= mesh->num_indices ||
						!HRLSkeletal_FillVertex(triVertices[corner], mesh, skin, clusterToBone, meshIndex))
					{
						valid = false;
						break;
					}
				}
				if (!valid) continue;
				HRLSkeletal_ComputeMissingTangentSpace(triVertices[0], triVertices[1], triVertices[2]);
				for (int corner = 0; corner < 3; ++corner)
				{
					const HRL_uint index = (HRL_uint)vertices.size();
					vertices.push_back(triVertices[corner]);
					indices.push_back(index);
				}
			}
		}

		if (vertices.empty())
		{
			ufbx_free_scene(scene);
			SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: skinned mesh contains no convertible triangles");
			return nullptr;
		}

		HRL_SkeletalMeshData* result = new HRL_SkeletalMeshData{};
		// Skeletal vertices are deformed all the way to world space by the skin
		// matrices. Keep the generic geometry transform field neutral so the
		// renderer cannot apply the FBX node transform a second time.
		for (int i = 0; i < 16; ++i)
			result->geometryToWorldMatrix[i] = 0.f;
		result->geometryToWorldMatrix[0] = 1.f;
		result->geometryToWorldMatrix[5] = 1.f;
		result->geometryToWorldMatrix[10] = 1.f;
		result->geometryToWorldMatrix[15] = 1.f;
		result->vertexCount = vertices.size();
		result->indexCount = indices.size();
		result->boneCount = boneNodes.size();
		result->animationCount = scene->anim_stacks.count > 0 ? scene->anim_stacks.count : 1;
		result->vertices = new HRL_SkeletalVertex[result->vertexCount];
		result->indices = new HRL_uint[result->indexCount];
		result->bones = new HRL_SkeletalBone[result->boneCount]{};
		result->animations = new HRL_SkeletalAnimation[result->animationCount]{};
		std::copy(vertices.begin(), vertices.end(), result->vertices);
		std::copy(indices.begin(), indices.end(), result->indices);

		for (size_t i = 0; i < boneNodes.size(); ++i)
		{
			const ufbx_node* node = boneNodes[i];
			HRL_SkeletalBone& bone = result->bones[i];
			bone.name = HRLSkeletal_CopyCString(node->name);
			bone.parentIndex = HRL_INVALID_ID;
			for (ufbx_node* parent = node->parent; parent; parent = parent->parent)
			{
				auto pit = nodeToBone.find(parent);
				if (pit != nodeToBone.end()) { bone.parentIndex = pit->second; break; }
			}
			const auto clusterIt = clusterByNode.find(node);
			const glm::mat4 inverseBind = clusterIt != clusterByNode.end()
				? HRLSkeletal_UFBXMatrixToGLM(clusterIt->second->geometry_to_bone)
				: glm::mat4(1.f);
			const HRL_SkeletalBoneTransform bindTransform = HRLSkeletal_TransformFromUFBX(node->local_transform);
			bone.bindTransform = bindTransform;
			const glm::mat4 bindWorld = HRLSkeletal_UFBXMatrixToGLM(node->node_to_world);
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
				{
					bone.bindWorldMatrix[c * 4 + r] = bindWorld[c][r];
					bone.inverseBindMatrix[c * 4 + r] = inverseBind[c][r];
				}
		}

		for (size_t animationIndex = 0; animationIndex < result->animationCount; ++animationIndex)
		{
			HRL_SkeletalAnimation& animation = result->animations[animationIndex];
			const bool bindPose = scene->anim_stacks.count == 0;
			const ufbx_anim_stack* stack = bindPose ? nullptr : scene->anim_stacks.data[animationIndex];
			if (bindPose)
			{
				const char* name = "BindPose";
				animation.name = new char[std::strlen(name) + 1];
				std::strcpy(const_cast<char*>(animation.name), name);
				animation.duration = 0.f;
				animation.frameRate = 30.f;
				animation.frameCount = 1;
			}
			else
			{
				animation.name = HRLSkeletal_CopyCString(stack->name);
				animation.duration = (float)std::max(0.0, stack->time_end - stack->time_begin);
				animation.frameRate = 30.f;
				const size_t maxFrames = 4096;
				animation.frameCount = animation.duration > 0.f
					? std::min(maxFrames, (size_t)std::ceil(animation.duration * animation.frameRate) + 1u)
					: 1u;
			}
			if (result->boneCount != 0 && animation.frameCount > std::numeric_limits<size_t>::max() / result->boneCount)
			{
				ufbx_free_scene(scene);
				HRLSkeletal_DestroyMeshData(result);
				SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: animation frame data is too large");
				return nullptr;
			}
			const size_t frameValueCount = animation.frameCount * result->boneCount;
			if (frameValueCount > std::numeric_limits<size_t>::max() / 16u)
			{
				ufbx_free_scene(scene);
				HRLSkeletal_DestroyMeshData(result);
				SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated pose data is too large");
				return nullptr;
			}
			animation.frames = new HRL_SkeletalBoneTransform[frameValueCount]{};
			animation.poseMatrices = new float[frameValueCount * 16u]{};
			for (size_t frame = 0; frame < animation.frameCount; ++frame)
			{
				const double time = bindPose || !stack
					? 0.0
					: std::min(stack->time_end, stack->time_begin + (double)frame / (double)animation.frameRate);
				ufbx_scene* evaluatedScene = nullptr;
				if (!bindPose && stack && stack->anim)
				{
					ufbx_error evalError{};
					evaluatedScene = ufbx_evaluate_scene(scene, stack->anim, time, nullptr, &evalError);
					if (!evaluatedScene)
					{
						ufbx_free_scene(scene);
						HRLSkeletal_DestroyMeshData(result);
						SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: failed to evaluate animation scene");
						return nullptr;
					}
				}

				// ufbx directly provides the current skinning transform as
				// cluster->geometry_to_world = bone_node->node_to_world * geometry_to_bone.
				// Use that instead of rebuilding the FBX bone hierarchy manually.
				std::unordered_map<uint32_t, const ufbx_skin_cluster*> evaluatedClusterByNode;
				if (evaluatedScene)
				{
					const uint32_t meshTypedId = mesh->typed_id;
					if ((size_t)meshTypedId >= evaluatedScene->meshes.count || !evaluatedScene->meshes.data[meshTypedId])
					{
						ufbx_free_scene(evaluatedScene);
						ufbx_free_scene(scene);
						HRLSkeletal_DestroyMeshData(result);
						SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated mesh lookup failed");
						return nullptr;
					}
					const ufbx_mesh* evaluatedMesh = evaluatedScene->meshes.data[meshTypedId];
					if (!evaluatedMesh || evaluatedMesh->skin_deformers.count == 0 || !evaluatedMesh->skin_deformers.data[0])
					{
						ufbx_free_scene(evaluatedScene);
						ufbx_free_scene(scene);
						HRLSkeletal_DestroyMeshData(result);
						SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated mesh has no skin deformer");
						return nullptr;
					}
					const ufbx_skin_deformer* evaluatedSkin = evaluatedMesh->skin_deformers.data[0];
					for (size_t clusterIndex = 0; clusterIndex < evaluatedSkin->clusters.count; ++clusterIndex)
					{
						const ufbx_skin_cluster* cluster = evaluatedSkin->clusters.data[clusterIndex];
						if (cluster && cluster->bone_node)
							evaluatedClusterByNode[cluster->bone_node->typed_id] = cluster;
					}
				}


				for (size_t boneIndex = 0; boneIndex < boneNodes.size(); ++boneIndex)
				{
					const size_t offset = frame * boneNodes.size() + boneIndex;
					const ufbx_skin_cluster* cluster = nullptr;
					const auto clusterIt = clusterByNode.find(boneNodes[boneIndex]);
					if (clusterIt != clusterByNode.end()) cluster = clusterIt->second;
					const ufbx_skin_cluster* poseCluster = cluster;
					if (evaluatedScene)
					{
						auto evaluatedIt = evaluatedClusterByNode.find(boneNodes[boneIndex]->typed_id);
						if (evaluatedIt != evaluatedClusterByNode.end()) poseCluster = evaluatedIt->second;
					}

					HRL_SkeletalBoneTransform transform = HRLSkeletal_TransformFromUFBX(boneNodes[boneIndex]->local_transform);
					if (evaluatedScene)
					{
						const uint32_t typedId = boneNodes[boneIndex]->typed_id;
						if ((size_t)typedId < evaluatedScene->nodes.count && evaluatedScene->nodes.data[typedId])
							transform = HRLSkeletal_TransformFromUFBX(evaluatedScene->nodes.data[typedId]->local_transform);
						else
						{
							ufbx_free_scene(evaluatedScene);
							ufbx_free_scene(scene);
							HRLSkeletal_DestroyMeshData(result);
							SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated bone node lookup failed");
							return nullptr;
						}
					}
					animation.frames[offset] = transform;
					// ufbx's cluster->geometry_to_world is already the complete current
					// skin matrix:
					//     bone_node->node_to_world * geometry_to_bone
					// Keep it in world space exactly as provided by ufbx. Do not pre/post-
					// multiply it by the mesh node transform: doing so is the source of
					// the disappearing/partial-mesh regression between the previous fixes.
					const glm::mat4 poseMatrix = poseCluster
						? HRLSkeletal_UFBXMatrixToGLM(poseCluster->geometry_to_world)
						: glm::mat4(1.f);
					for (int col = 0; col < 4; ++col)
						for (int row = 0; row < 4; ++row)
							animation.poseMatrices[offset * 16u + (size_t)col * 4u + (size_t)row] = poseMatrix[col][row];
				}
				if (evaluatedScene)
					ufbx_free_scene(evaluatedScene);
			}
		}

		ufbx_free_scene(scene);
		return result;
	}
	catch (const std::bad_alloc&)
	{
		ufbx_free_scene(scene);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: out of memory while converting skeleton");
		return nullptr;
	}
}

void HRL_FreeSkeletalMeshData(HRL_SkeletalMeshData* _data)
{
	HRLSkeletal_DestroyMeshData(_data);
}

// ============================================================================
// FBX resource/material helpers
// ============================================================================

namespace {

static ufbx_load_opts HRLFBX_MakeResourceLoadOptions()
{
	ufbx_load_opts options{};
	options.file_format = UFBX_FILE_FORMAT_FBX;
	options.load_external_files = false;
	options.ignore_missing_external_files = true;
	options.ignore_geometry = true;
	options.ignore_animation = true;
	options.ignore_embedded = false;
	options.evaluate_skinning = false;
	options.evaluate_caches = false;
	options.use_blender_pbr_material = true;
	return options;
}

static bool HRLFBX_HasString(const ufbx_string& value)
{
	return value.data && value.length > 0;
}

struct HRLFBX_TexturePayload
{
	const ufbx_texture* source = nullptr;
	ufbx_blob blob{};
};

static HRLFBX_TexturePayload HRLFBX_FindTexturePayload(
	const HRL_FBXResources* resources,
	const ufbx_texture* texture,
	int depth = 0)
{
	HRLFBX_TexturePayload result{};
	if (!resources || !resources->scene || !texture || depth > 16)
		return result;

	if (texture->content.data && texture->content.size > 0)
	{
		result.source = texture;
		result.blob = texture->content;
		return result;
	}

	if (texture->has_file && texture->file_index < resources->scene->texture_files.count)
	{
		const ufbx_texture_file& file = resources->scene->texture_files.data[texture->file_index];
		if (file.content.data && file.content.size > 0)
		{
			result.source = texture;
			result.blob = file.content;
			return result;
		}
	}

	for (size_t i = 0; i < texture->file_textures.count; ++i)
	{
		HRLFBX_TexturePayload nested = HRLFBX_FindTexturePayload(
			resources, texture->file_textures.data[i], depth + 1);
		if (nested.blob.data && nested.blob.size > 0)
			return nested;
	}

	return result;
}

static const char* HRLFBX_TextureFilename(const ufbx_texture* texture)
{
	if (!texture)
		return "";
	if (HRLFBX_HasString(texture->filename))
		return texture->filename.data;
	if (HRLFBX_HasString(texture->relative_filename))
		return texture->relative_filename.data;
	return "";
}

static const ufbx_material_map* HRLFBX_SelectMap(
	const ufbx_material_map& primary,
	const ufbx_material_map& fallback)
{
	if (primary.texture && primary.texture_enabled)
		return &primary;
	if (primary.has_value)
		return &primary;
	if (fallback.texture && fallback.texture_enabled)
		return &fallback;
	return &fallback;
}

static const ufbx_material_map* HRLFBX_SelectTextureMap(
	const ufbx_material_map& primary,
	const ufbx_material_map& fallback)
{
	if (primary.texture && primary.texture_enabled)
		return &primary;
	if (fallback.texture && fallback.texture_enabled)
		return &fallback;
	return nullptr;
}

static glm::vec4 HRLFBX_ReadColor(const ufbx_material_map& map, const glm::vec4& fallback)
{
	if (!map.has_value && map.value_components == 0)
		return fallback;

	switch (map.value_components)
	{
	case 1:
		return glm::vec4((float)map.value_real, (float)map.value_real, (float)map.value_real, 1.f);
	case 2:
		return glm::vec4((float)map.value_vec2.x, (float)map.value_vec2.y, 0.f, 1.f);
	case 3:
		return glm::vec4(
			(float)map.value_vec3.x,
			(float)map.value_vec3.y,
			(float)map.value_vec3.z,
			1.f);
	case 4:
		return glm::vec4(
			(float)map.value_vec4.x,
			(float)map.value_vec4.y,
			(float)map.value_vec4.z,
			(float)map.value_vec4.w);
	default:
		return fallback;
	}
}

static float HRLFBX_ReadScalar(const ufbx_material_map& map, float fallback)
{
	if (!map.has_value && map.value_components == 0)
		return fallback;
	if (map.value_components == 1)
		return (float)map.value_real;
	if (map.value_components == 2)
		return (float)map.value_vec2.x;
	if (map.value_components == 3)
		return (float)map.value_vec3.x;
	if (map.value_components == 4)
		return (float)map.value_vec4.x;
	return fallback;
}

static HRL_uint HRLFBX_TextureIndex(HRL_FBXResources* resources, const ufbx_texture* texture)
{
	if (!resources || !texture)
		return (HRL_uint)HRL_INVALID_ID;

	auto found = resources->texture_indices.find(texture);
	if (found != resources->texture_indices.end())
		return found->second;

	HRL_FBXTextureInfo info{};
	info.name = HRLFBX_HasString(texture->name) ? texture->name.data : "";
	info.filename = HRLFBX_TextureFilename(texture);
	switch (texture->type)
	{
	case UFBX_TEXTURE_FILE:       info.type = HRL_FBX_TEXTURE_FILE; break;
	case UFBX_TEXTURE_LAYERED:    info.type = HRL_FBX_TEXTURE_LAYERED; break;
	case UFBX_TEXTURE_PROCEDURAL: info.type = HRL_FBX_TEXTURE_PROCEDURAL; break;
	case UFBX_TEXTURE_SHADER:     info.type = HRL_FBX_TEXTURE_SHADER; break;
	default:                      info.type = HRL_FBX_TEXTURE_FILE; break;
	}

	const HRLFBX_TexturePayload payload = HRLFBX_FindTexturePayload(resources, texture);
	if (payload.blob.data && payload.blob.size > 0)
	{
		info.data = (const unsigned char*)payload.blob.data;
		info.size = payload.blob.size;
		info.embedded = 1;
	}

	if (resources->textures.size() >= (size_t)HRL_INVALID_ID)
		return (HRL_uint)HRL_INVALID_ID;

	const HRL_uint index = (HRL_uint)resources->textures.size();
	resources->textures.push_back(info);
	resources->texture_indices.emplace(texture, index);
	return index;
}

static void HRLFBX_AssignTextureIndex(
	HRL_FBXResources* resources,
	HRL_uint& destination,
	const ufbx_material_map& primary,
	const ufbx_material_map& fallback)
{
	const ufbx_material_map* map = HRLFBX_SelectTextureMap(primary, fallback);
	if (!map || !map->texture)
		return;
	destination = HRLFBX_TextureIndex(resources, map->texture);
}

static void HRLFBX_BuildTextureInfo(
	HRL_FBXResources* resources,
	const ufbx_texture* texture,
	HRL_FBXTextureInfo& info)
{
	info = {};
	info.name = texture && HRLFBX_HasString(texture->name) ? texture->name.data : "";
	info.filename = HRLFBX_TextureFilename(texture);
	info.type = HRL_FBX_TEXTURE_FILE;
	if (texture)
	{
		switch (texture->type)
		{
		case UFBX_TEXTURE_FILE:       info.type = HRL_FBX_TEXTURE_FILE; break;
		case UFBX_TEXTURE_LAYERED:    info.type = HRL_FBX_TEXTURE_LAYERED; break;
		case UFBX_TEXTURE_PROCEDURAL: info.type = HRL_FBX_TEXTURE_PROCEDURAL; break;
		case UFBX_TEXTURE_SHADER:     info.type = HRL_FBX_TEXTURE_SHADER; break;
		default:                      info.type = HRL_FBX_TEXTURE_FILE; break;
		}
	}
	const HRLFBX_TexturePayload payload = HRLFBX_FindTexturePayload(resources, texture);
	if (payload.blob.data && payload.blob.size > 0)
	{
		info.data = (const unsigned char*)payload.blob.data;
		info.size = payload.blob.size;
		info.embedded = 1;
	}
}

static void HRLFBX_BuildMaterialInfo(
	HRL_FBXResources* resources,
	const ufbx_material* material,
	HRL_FBXMaterialInfo& info)
{
	info = {};
	for (HRL_uint& index : info.textureIndices)
		index = (HRL_uint)HRL_INVALID_ID;

	info.name = material && HRLFBX_HasString(material->name) ? material->name.data : "";
	info.baseColor[0] = 1.f;
	info.baseColor[1] = 1.f;
	info.baseColor[2] = 1.f;
	info.baseColor[3] = 1.f;
	info.roughness = 0.5f;
	info.metallic = 0.f;
	info.specular = 0.5f;
	info.opacity = 1.f;

	if (!material)
		return;

	const bool hasPbrBase =
		(material->pbr.base_color.texture && material->pbr.base_color.texture_enabled) ||
		material->pbr.base_color.has_value;
	const ufbx_material_map* baseColor = hasPbrBase
		? &material->pbr.base_color
		: &material->fbx.diffuse_color;
	glm::vec4 color = HRLFBX_ReadColor(*baseColor, glm::vec4(1.f));
	if (hasPbrBase)
	{
		if (material->pbr.base_factor.has_value)
			color *= HRLFBX_ReadColor(material->pbr.base_factor, glm::vec4(1.f));
	}
	else if (material->fbx.diffuse_factor.has_value)
	{
		color *= HRLFBX_ReadColor(material->fbx.diffuse_factor, glm::vec4(1.f));
	}

	info.baseColor[0] = std::clamp(color.r, 0.f, 1.f);
	info.baseColor[1] = std::clamp(color.g, 0.f, 1.f);
	info.baseColor[2] = std::clamp(color.b, 0.f, 1.f);
	info.baseColor[3] = std::clamp(color.a, 0.f, 1.f);

	if (material->pbr.roughness.has_value || material->pbr.roughness.texture)
		info.roughness = std::clamp(HRLFBX_ReadScalar(material->pbr.roughness, 0.5f), 0.f, 1.f);
	else if (material->pbr.glossiness.has_value || material->pbr.glossiness.texture)
		info.roughness = 1.f - std::clamp(HRLFBX_ReadScalar(material->pbr.glossiness, 0.5f), 0.f, 1.f);

	if (material->pbr.metalness.has_value || material->pbr.metalness.texture)
		info.metallic = std::clamp(HRLFBX_ReadScalar(material->pbr.metalness, 0.f), 0.f, 1.f);

	if (material->pbr.specular_factor.has_value || material->pbr.specular_factor.texture)
		info.specular = std::clamp(HRLFBX_ReadScalar(material->pbr.specular_factor, 0.5f), 0.f, 1.f);
	else if (material->fbx.specular_factor.has_value || material->fbx.specular_factor.texture)
		info.specular = std::clamp(HRLFBX_ReadScalar(material->fbx.specular_factor, 0.5f), 0.f, 1.f);

	// ufbx exposes PBR maps for all shading models, and for legacy Phong/
	// Lambert materials some of those maps can contain synthesized default
	// values even when the material does not actually use opacity. Do not treat
	// such a scalar as authoritative: a synthesized opacity of 0 would make the
	// entire material disappear in the fragment shader (which discards alpha).
	const bool pbrOpacityTexture =
		material->pbr.opacity.texture && material->pbr.opacity.texture_enabled;
	const bool fbxTransparencyTexture =
		material->fbx.transparency_factor.texture && material->fbx.transparency_factor.texture_enabled;
	const bool opacityFeatureEnabled = material->features.opacity.enabled;
	if (pbrOpacityTexture)
		info.opacity = std::clamp(HRLFBX_ReadScalar(material->pbr.opacity, 1.f), 0.f, 1.f);
	else if (fbxTransparencyTexture)
		info.opacity = 1.f - std::clamp(HRLFBX_ReadScalar(material->fbx.transparency_factor, 0.f), 0.f, 1.f);
	else if (opacityFeatureEnabled && material->pbr.opacity.has_value)
		info.opacity = std::clamp(HRLFBX_ReadScalar(material->pbr.opacity, 1.f), 0.f, 1.f);
	else if (opacityFeatureEnabled && material->fbx.transparency_factor.has_value)
		info.opacity = 1.f - std::clamp(HRLFBX_ReadScalar(material->fbx.transparency_factor, 0.f), 0.f, 1.f);
	else
		info.opacity = 1.f;

	HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ALBEDO],
		material->pbr.base_color, material->fbx.diffuse_color);
	HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_NORMAL],
		material->pbr.normal_map, material->fbx.normal_map);
	HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_SPECULAR],
		material->pbr.specular_color, material->fbx.specular_color);
	HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ROUGHNESS],
		material->pbr.roughness, material->pbr.roughness);
	if (info.textureIndices[HRL_FBX_MATERIAL_ROUGHNESS] == (HRL_uint)HRL_INVALID_ID)
		HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ROUGHNESS],
			material->pbr.glossiness, material->pbr.glossiness);
	HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_METALLIC],
		material->pbr.metalness, material->pbr.metalness);
	if (material->pbr.opacity.texture && material->pbr.opacity.texture_enabled)
	{
		HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ALPHA],
			material->pbr.opacity, material->pbr.opacity);
	}
	if (info.textureIndices[HRL_FBX_MATERIAL_ALPHA] == (HRL_uint)HRL_INVALID_ID &&
		material->fbx.transparency_factor.texture && material->fbx.transparency_factor.texture_enabled)
	{
		HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ALPHA],
			material->fbx.transparency_factor, material->fbx.transparency_factor);
	}
	if (info.textureIndices[HRL_FBX_MATERIAL_ALPHA] == (HRL_uint)HRL_INVALID_ID &&
		material->fbx.transparency_color.texture && material->fbx.transparency_color.texture_enabled)
	{
		HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ALPHA],
			material->fbx.transparency_color, material->fbx.transparency_color);
	}
}

static HRL_id HRLFBX_CreateMaterialFromResource(
	HRL_FBXResources* resources,
	size_t materialIndex,
	HRL_id shaderid)
{
	if (!resources || materialIndex >= resources->materials.size())
		return HRL_INVALID_ID;
	if (!HRL_IsValidShader(shaderid))
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_CreateMaterialFromFBX: invalid shader ID");
		return HRL_INVALID_ID;
	}

	HRL_id materialId = HRL_INVALID_ID;
	try
	{
		const HRL_FBXMaterialInfo& info = resources->materials[materialIndex];
		materialId = HRL_CreateMaterial(shaderid);
		if (materialId == HRL_INVALID_ID)
			return HRL_INVALID_ID;

		HRL_MaterialSetVec3(materialId, "TintColor", info.baseColor[0], info.baseColor[1], info.baseColor[2]);
		HRL_MaterialSetFloat(materialId, "BaseColorAlpha", info.baseColor[3]);
		HRL_MaterialSetFloat(materialId, "RoughnessValue", info.roughness);
		HRL_MaterialSetFloat(materialId, "MetallicValue", info.metallic);
		HRL_MaterialSetFloat(materialId, "SpecularValue", info.specular);
		HRL_MaterialSetFloat(materialId, "OpacityValue", info.opacity);
		HRL_MaterialSetInt(materialId, "RoughnessUseValue", info.textureIndices[HRL_FBX_MATERIAL_ROUGHNESS] == (HRL_uint)HRL_INVALID_ID ? 1 : 0);
		HRL_MaterialSetInt(materialId, "MetallicUseValue", info.textureIndices[HRL_FBX_MATERIAL_METALLIC] == (HRL_uint)HRL_INVALID_ID ? 1 : 0);
		HRL_MaterialSetInt(materialId, "SpecularUseValue", info.textureIndices[HRL_FBX_MATERIAL_SPECULAR] == (HRL_uint)HRL_INVALID_ID ? 1 : 0);
		HRL_MaterialSetInt(materialId, "OpacityUseValue", info.textureIndices[HRL_FBX_MATERIAL_ALPHA] == (HRL_uint)HRL_INVALID_ID ? 1 : 0);

		if (materialIndex < resources->material_sources.size())
		{
			const ufbx_material* fbxMaterial = resources->material_sources[materialIndex];
			if (fbxMaterial)
			{
				if (!fbxMaterial->pbr.roughness.texture && fbxMaterial->pbr.glossiness.texture)
					HRL_MaterialSetInt(materialId, "RoughnessInvert", 1);
				if (!fbxMaterial->pbr.opacity.texture &&
					((fbxMaterial->fbx.transparency_factor.texture && fbxMaterial->fbx.transparency_factor.texture_enabled) ||
					 (fbxMaterial->fbx.transparency_color.texture && fbxMaterial->fbx.transparency_color.texture_enabled)))
					HRL_MaterialSetInt(materialId, "AlphaInvert", 1);
			}
		}

		HRL_Material* materialObject = nullptr;
		auto materialIt = ctx_.materials.find(materialId);
		if (materialIt != ctx_.materials.end())
			materialObject = materialIt->second;
		if (!materialObject)
		{
			SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
				"HRL_CreateMaterialFromFBX: material was not registered");
			return HRL_INVALID_ID;
		}

		materialObject->owned_textures_.reserve(
			materialObject->owned_textures_.size() + HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT);

		HRL_uint createdTextureIndices[HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT]{};
		HRL_id createdTextureIds[HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT]{};
		size_t createdTextureCount = 0;
		static const char* const slots[] = {
			HRL_T_ALBEDO,
			HRL_T_NORMAL,
			HRL_T_SPECULAR,
			HRL_T_ROUGHNESS,
			HRL_T_METALLIC,
			HRL_T_ALPHA
		};

		for (int slot = 0; slot < (int)HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT; ++slot)
		{
			const HRL_uint textureIndex = info.textureIndices[slot];
			if (textureIndex == (HRL_uint)HRL_INVALID_ID)
				continue;

			const HRL_FBXTextureInfo* textureInfo = HRL_GetFBXTexture(resources, textureIndex);
			if (!textureInfo || !textureInfo->embedded || !textureInfo->data || textureInfo->size == 0)
				continue;

			HRL_id textureId = HRL_INVALID_ID;
			for (size_t i = 0; i < createdTextureCount; ++i)
			{
				if (createdTextureIndices[i] == textureIndex)
				{
					textureId = createdTextureIds[i];
					break;
				}
			}

			if (textureId == HRL_INVALID_ID)
			{
				textureId = HRL_CreateTextureFromFBX(resources, textureIndex);
				if (textureId == HRL_INVALID_ID)
					continue;
				createdTextureIndices[createdTextureCount] = textureIndex;
				createdTextureIds[createdTextureCount] = textureId;
				++createdTextureCount;
				materialObject->owned_textures_.push_back(textureId);
			}

			HRL_MaterialSetTexture(materialId, slots[slot], textureId);
		}

		return materialId;
	}
	catch (const std::bad_alloc&)
	{
		if (materialId != HRL_INVALID_ID)
			HRL_DeleteMaterial(materialId);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
			"HRL_CreateMaterialFromFBX: out of memory while creating material resources");
		return HRL_INVALID_ID;
	}
}
} // anonymous namespace


HRL_FBXResources* HRL_LoadFBXResources(const char* _data, size_t _bufferSize)
{
	if (!_data || _bufferSize == 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_LoadFBXResources: data and buffer size must be valid");
		return nullptr;
	}

	ufbx_load_opts options = HRLFBX_MakeResourceLoadOptions();
	ufbx_error error{};
	ufbx_scene* scene = ufbx_load_memory(_data, _bufferSize, &options, &error);
	if (!scene)
	{
		std::string detail = "HRL_LoadFBXResources: failed to decode FBX file";
		if (error.description.data && error.description.data[0] != '\0')
		{
			detail += " (";
			detail += error.description.data;
			detail += ")";
		}
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, detail.c_str());
		return nullptr;
	}

	HRL_FBXResources* resources = nullptr;
	try
	{
		resources = new HRL_FBXResources();
		resources->scene = scene;
		resources->textures.reserve(scene->textures.count);
		resources->materials.reserve(scene->materials.count);
		resources->material_sources.reserve(scene->materials.count);

		for (size_t i = 0; i < scene->textures.count; ++i)
		{
			const ufbx_texture* texture = scene->textures.data[i];
			if (!texture)
				continue;
			HRL_FBXTextureInfo info{};
			HRLFBX_BuildTextureInfo(resources, texture, info);
			const HRL_uint index = (HRL_uint)resources->textures.size();
			resources->textures.push_back(info);
			resources->texture_indices.emplace(texture, index);
		}

		for (size_t i = 0; i < scene->materials.count; ++i)
		{
			const ufbx_material* material = scene->materials.data[i];
			if (!material)
				continue;
			HRL_FBXMaterialInfo info{};
			HRLFBX_BuildMaterialInfo(resources, material, info);
			resources->materials.push_back(info);
			resources->material_sources.push_back(material);
		}
	}
	catch (const std::bad_alloc&)
	{
		delete resources;
		ufbx_free_scene(scene);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
			"HRL_LoadFBXResources: out of memory while building resource tables");
		return nullptr;
	}

	return resources;
}

void HRL_FreeFBXResources(HRL_FBXResources* _resources)
{
	if (!_resources)
		return;
	if (_resources->scene)
		ufbx_free_scene(_resources->scene);
	delete _resources;
}

size_t HRL_GetFBXMaterialCount(const HRL_FBXResources* _resources)
{
	return _resources ? _resources->materials.size() : 0;
}

const HRL_FBXMaterialInfo* HRL_GetFBXMaterial(const HRL_FBXResources* _resources, size_t _index)
{
	if (!_resources || _index >= _resources->materials.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetFBXMaterial: invalid material index");
		return nullptr;
	}
	return &_resources->materials[_index];
}

HRL_id HRL_FindFBXMaterial(const HRL_FBXResources* _resources, const char* _name)
{
	if (!_resources || !_name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_FindFBXMaterial: invalid resources or name");
		return HRL_INVALID_ID;
	}
	for (size_t i = 0; i < _resources->materials.size(); ++i)
	{
		const char* name = _resources->materials[i].name ? _resources->materials[i].name : "";
		if (std::strcmp(name, _name) == 0)
			return (HRL_id)i;
	}
	return HRL_INVALID_ID;
}

size_t HRL_GetFBXTextureCount(const HRL_FBXResources* _resources)
{
	return _resources ? _resources->textures.size() : 0;
}

const HRL_FBXTextureInfo* HRL_GetFBXTexture(const HRL_FBXResources* _resources, size_t _index)
{
	if (!_resources || _index >= _resources->textures.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetFBXTexture: invalid texture index");
		return nullptr;
	}
	return &_resources->textures[_index];
}

HRL_id HRL_FindFBXTexture(const HRL_FBXResources* _resources, const char* _name)
{
	if (!_resources || !_name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_FindFBXTexture: invalid resources or name");
		return HRL_INVALID_ID;
	}
	for (size_t i = 0; i < _resources->textures.size(); ++i)
	{
		const char* name = _resources->textures[i].name ? _resources->textures[i].name : "";
		const char* filename = _resources->textures[i].filename ? _resources->textures[i].filename : "";
		if (std::strcmp(name, _name) == 0 || std::strcmp(filename, _name) == 0)
			return (HRL_id)i;
	}
	return HRL_INVALID_ID;
}

const HRL_FBXTextureInfo* HRL_GetFBXMaterialTexture(
	const HRL_FBXResources* _resources,
	size_t _materialIndex,
	HRL_EFBXMaterialTextureSlot _slot)
{
	if (!_resources || _materialIndex >= _resources->materials.size() ||
		_slot < HRL_FBX_MATERIAL_ALBEDO || _slot >= HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetFBXMaterialTexture: invalid material or texture slot");
		return nullptr;
	}
	const HRL_uint index = _resources->materials[_materialIndex].textureIndices[(size_t)_slot];
	if (index == (HRL_uint)HRL_INVALID_ID)
		return nullptr;
	return HRL_GetFBXTexture(_resources, index);
}

HRL_id HRL_CreateTextureFromFBX(const HRL_FBXResources* _resources, size_t _textureIndex)
{
	const HRL_FBXTextureInfo* texture = HRL_GetFBXTexture(_resources, _textureIndex);
	if (!texture)
		return HRL_INVALID_ID;
	if (!texture->embedded || !texture->data || texture->size == 0)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_CreateTextureFromFBX: texture has no embedded image data");
		return HRL_INVALID_ID;
	}
	return HRL_CreateTexture(reinterpret_cast<const char*>(texture->data), texture->size);
}

static HRL_id HRLFBX_SelectAutomaticMaterialShader(const HRL_FBXResources* resources)
{
	if (resources && resources->scene)
	{
		for (size_t i = 0; i < resources->scene->meshes.count; ++i)
		{
			const ufbx_mesh* mesh = resources->scene->meshes.data[i];
			if (mesh && mesh->skin_deformers.count > 0)
				return HRL_SKINNED_3D_MESH_SHADER;
		}
	}
	return HRL_MESH_3D_SHADER;
}

static HRL_id HRLFBX_CreateAutomaticMaterial(
	const char* _data, size_t _bufferSize, size_t _materialIndex)
{
	HRL_FBXResources* resources = HRL_LoadFBXResources(_data, _bufferSize);
	if (!resources)
		return HRL_INVALID_ID;

	if (_materialIndex >= HRL_GetFBXMaterialCount(resources))
	{
		HRL_FreeFBXResources(resources);
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_CreateMaterialFromFBX: FBX contains no material at the requested index");
		return HRL_INVALID_ID;
	}

	const HRL_id shader = HRLFBX_SelectAutomaticMaterialShader(resources);
	const HRL_id material = HRLFBX_CreateMaterialFromResource(resources, _materialIndex, shader);
	HRL_FreeFBXResources(resources);
	return material;
}

HRL_id HRL_CreateMaterialFromFBX(const char* _data, size_t _bufferSize)
{
	return HRLFBX_CreateAutomaticMaterial(_data, _bufferSize, 0);
}

HRL_id HRL_CreateMaterialFromFBXWithShader(const char* _data, size_t _bufferSize, HRL_id _shaderid)
{
	return HRL_CreateMaterialFromFBXIndexedWithShader(
		_data, _bufferSize, 0, _shaderid);
}

HRL_id HRL_CreateMaterialFromFBXIndexed(
	const char* _data, size_t _bufferSize, size_t _materialIndex)
{
	return HRLFBX_CreateAutomaticMaterial(_data, _bufferSize, _materialIndex);
}

HRL_id HRL_CreateMaterialFromFBXIndexedWithShader(
	const char* _data, size_t _bufferSize, size_t _materialIndex, HRL_id _shaderid)
{
	HRL_FBXResources* resources = HRL_LoadFBXResources(_data, _bufferSize);
	if (!resources)
		return HRL_INVALID_ID;

	if (_materialIndex >= HRL_GetFBXMaterialCount(resources))
	{
		HRL_FreeFBXResources(resources);
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_CreateMaterialFromFBX: FBX contains no material at the requested index");
		return HRL_INVALID_ID;
	}

	const HRL_id material = HRLFBX_CreateMaterialFromResource(resources, _materialIndex, _shaderid);
	HRL_FreeFBXResources(resources);
	return material;
}

static HRL_Landscape* GetLandscapeForEdit(HRL_id id, const char* fn)
{
	auto it = ctx_.landscapes.find(id);
	if (it == ctx_.landscapes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(fn) + ": invalid landscape ID");
		return nullptr;
	}
	return it->second;
}

HRL_id HRL_CreateLandscape(HRL_id _sceneid, HRL_id _heightmap)
{
	auto sceneIt = ctx_.scenes.find(_sceneid);
	if (sceneIt == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateLandscape: invalid scene ID");
		return HRL_INVALID_ID;
	}
	if (_heightmap == HRL_INVALID_ID || !g_Backend.RHI_IsValidTexture || !g_Backend.RHI_IsValidTexture(_heightmap))
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateLandscape: invalid heightmap texture ID");
		return HRL_INVALID_ID;
	}
	auto* landscape = new (std::nothrow) HRL_Landscape();
	if (!landscape)
	{
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateLandscape: allocation failed");
		return HRL_INVALID_ID;
	}
	const HRL_id id = GenerateHRL_ID();
	landscape->id_ = id;
	landscape->scene_ = _sceneid;
	landscape->heightmap_ = _heightmap;
	sceneIt->second->landscapes.emplace(id, landscape);
	ctx_.landscapes.emplace(id, landscape);
	sceneIt->second->shadows_dirty = true;
	++sceneIt->second->gi_geometry_revision;
	return id;
}

void HRL_DeleteLandscape(HRL_id _landscape)
{
	auto it = ctx_.landscapes.find(_landscape);
	if (it == ctx_.landscapes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteLandscape: invalid landscape ID");
		return;
	}
	HRL_Landscape* landscape = it->second;
	if (auto sceneIt = ctx_.scenes.find(landscape->scene_); sceneIt != ctx_.scenes.end())
	{
		sceneIt->second->landscapes.erase(_landscape);
		sceneIt->second->shadows_dirty = true;
		++sceneIt->second->gi_geometry_revision;
	}
	ctx_.landscapes.erase(it);
	delete landscape;
}

int HRL_IsValidLandscape(HRL_id _landscape)
{
	return ctx_.landscapes.find(_landscape) != ctx_.landscapes.end() ? HRL_TRUE : HRL_FALSE;
}

void HRL_SetLandscapeHeightmap(HRL_id _landscape, HRL_id _heightmap)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeHeightmap");
	if (!landscape) return;
	if (_heightmap == HRL_INVALID_ID || !g_Backend.RHI_IsValidTexture || !g_Backend.RHI_IsValidTexture(_heightmap))
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLandscapeHeightmap: invalid heightmap texture ID");
		return;
	}
	landscape->heightmap_ = _heightmap;
	++landscape->revision_;
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end())
	{
		it->second->shadows_dirty = true;
		++it->second->gi_geometry_revision;
	}
}

void HRL_SetLandscapePosition(HRL_id _landscape, float x, float y, float z)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapePosition");
	if (!landscape) return;
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapePosition: values must be finite");
		return;
	}
	landscape->position_ = glm::vec3(x, y, z);
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end()) MarkSceneGIGeometryDirty(it->second);
}

void HRL_SetLandscapeRotation(HRL_id _landscape, float pitch, float yaw, float roll)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeRotation");
	if (!landscape) return;
	if (!std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(roll))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeRotation: values must be finite");
		return;
	}
	landscape->rotation_ = glm::vec3(pitch, yaw, roll);
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end()) MarkSceneGIGeometryDirty(it->second);
}

void HRL_SetLandscapeScale(HRL_id _landscape, float x, float y, float z)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeScale");
	if (!landscape) return;
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || x <= 0.f || y <= 0.f || z <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeScale: scale components must be finite and > 0");
		return;
	}
	landscape->scale_ = glm::vec3(x, y, z);
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end()) MarkSceneGIGeometryDirty(it->second);
}

void HRL_SetLandscapeSize(HRL_id _landscape, float width, float depth)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeSize");
	if (!landscape) return;
	if (!std::isfinite(width) || !std::isfinite(depth) || width <= 0.f || depth <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeSize: width/depth must be finite and > 0");
		return;
	}
	landscape->size_ = glm::vec2(width, depth);
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end()) MarkSceneGIGeometryDirty(it->second);
}

void HRL_SetLandscapeHeight(HRL_id _landscape, float height)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeHeight");
	if (!landscape) return;
	if (!std::isfinite(height) || height <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeHeight: height must be finite and > 0");
		return;
	}
	landscape->height_scale_ = height;
	++landscape->revision_;
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end())
	{
		it->second->shadows_dirty = true;
		++it->second->gi_geometry_revision;
	}
}

void HRL_SetLandscapeResolution(HRL_id _landscape, HRL_uint x, HRL_uint z)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeResolution");
	if (!landscape) return;
	x = std::clamp<HRL_uint>(x, 2u, 512u);
	z = std::clamp<HRL_uint>(z, 2u, 512u);
	if (landscape->resolution_x_ == x && landscape->resolution_z_ == z) return;
	landscape->resolution_x_ = x;
	landscape->resolution_z_ = z;
	++landscape->revision_;
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end())
	{
		it->second->shadows_dirty = true;
		++it->second->gi_geometry_revision;
	}
}

void HRL_SetLandscapeUVScale(HRL_id _landscape, float u, float v)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeUVScale");
	if (!landscape) return;
	if (!std::isfinite(u) || !std::isfinite(v) || u <= 0.f || v <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeUVScale: values must be finite and > 0");
		return;
	}
	landscape->uv_scale_ = glm::vec2(u, v);
}

void HRL_SetLandscapeMaterial(HRL_id _landscape, HRL_id _material)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeMaterial");
	if (!landscape) return;
	if (_material != HRL_INVALID_ID && ctx_.materials.find(_material) == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLandscapeMaterial: invalid material ID");
		return;
	}
	landscape->material_ = _material;
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end())
		it->second->shadows_dirty = true;
	MarkAllScenesGILightingDirty();
}

HRL_id HRL_CreateSkeletalMesh(HRL_id _sceneid, const HRL_SkeletalMeshData* _data)
{
	auto sceneIt = ctx_.scenes.find(_sceneid);
	if (sceneIt == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: invalid scene ID");
		return HRL_INVALID_ID;
	}
	if (!_data || !_data->vertices || _data->vertexCount < 3 || !_data->bones || _data->boneCount == 0 || _data->boneCount > HRL_MAX_SKELETAL_BONES)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: invalid skeletal mesh data");
		return HRL_INVALID_ID;
	}
	if (_data->indexCount > 0 && !_data->indices)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: index count is non-zero but indices are null");
		return HRL_INVALID_ID;
	}
	if (_data->vertexCount > (size_t)std::numeric_limits<HRL_uint>::max())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex count exceeds HRL_uint index range");
		return HRL_INVALID_ID;
	}
	for (size_t vertexIndex = 0; vertexIndex < _data->vertexCount; ++vertexIndex)
	{
		const HRL_Vertex3D& vertex = _data->vertices[vertexIndex].vertex;
		for (float value : vertex.position)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex position contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.normal)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex normal contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.uv)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex UV contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.tangent)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex tangent contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.bitangent)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex bitangent contains NaN or infinity"); return HRL_INVALID_ID; }
		float weightSum = 0.f;
		for (size_t influence = 0; influence < HRL_SKELETAL_MAX_INFLUENCES; ++influence)
		{
			const float weight = _data->vertices[vertexIndex].boneWeights[influence];
			if (!std::isfinite(weight) || weight < 0.f)
			{
				SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: bone weights must be finite and non-negative");
				return HRL_INVALID_ID;
			}
			if (weight > 0.f && _data->vertices[vertexIndex].boneIndices[influence] >= _data->boneCount)
			{
				SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: bone index is outside the skeleton");
				return HRL_INVALID_ID;
			}
			weightSum += weight;
		}
		if (!(weightSum > 0.f) || !std::isfinite(weightSum))
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: every vertex must have at least one positive bone weight");
			return HRL_INVALID_ID;
		}
	}
	for (size_t i = 0; i < _data->indexCount; ++i)
	{
		if ((size_t)_data->indices[i] >= _data->vertexCount)
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: index references a vertex outside the vertex array");
			return HRL_INVALID_ID;
		}
	}
	if (_data->indexCount > 0 && (_data->indexCount % 3u) != 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: index count must be a multiple of 3");
		return HRL_INVALID_ID;
	}

	std::vector<HRL_uint> generatedIndices;
	const HRL_uint* indices = _data->indices;
	if (_data->indexCount == 0)
	{
		if ((_data->vertexCount % 3u) != 0)
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: non-indexed vertex count must be a multiple of 3");
			return HRL_INVALID_ID;
		}
		generatedIndices.resize(_data->vertexCount);
		for (size_t i = 0; i < generatedIndices.size(); ++i)
			generatedIndices[i] = (HRL_uint)i;
		indices = generatedIndices.data();
	}

	HRL_id newId = GenerateHRL_ID();
	if (!g_Backend.RHI_CreateSkeletalMesh ||
		g_Backend.RHI_CreateSkeletalMesh(newId, _data->vertices, _data->vertexCount, indices,
			_data->indexCount > 0 ? _data->indexCount : generatedIndices.size(), (HRL_uint)_data->boneCount) != HRL_TRUE)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: backend failed to create skeletal mesh");
		return HRL_INVALID_ID;
	}

	std::unique_ptr<HRL_SkeletalMesh> mesh(new (std::nothrow) HRL_SkeletalMesh());
	if (!mesh)
	{
		g_Backend.RHI_DeleteMesh(newId);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: out of memory");
		return HRL_INVALID_ID;
	}
	mesh->scene_ = _sceneid;
	mesh->type_ = HRL_3D_SKELETAL_MESH;
	bool hasGeometryToWorld = false;
	for (int i = 0; i < 16; ++i)
		if (std::isfinite(_data->geometryToWorldMatrix[i]) && std::fabs(_data->geometryToWorldMatrix[i]) > 0.f) { hasGeometryToWorld = true; break; }
	mesh->fbx_geometry_to_world_ = hasGeometryToWorld
		? HRLSkeletal_ArrayToGLM(_data->geometryToWorldMatrix)
		: glm::mat4(1.f);
	mesh->triangle_count_ = (_data->indexCount > 0 ? _data->indexCount : generatedIndices.size()) / 3u;
	mesh->vertices_.assign(_data->vertices, _data->vertices + _data->vertexCount);
	mesh->indices_.assign(indices, indices + (_data->indexCount > 0 ? _data->indexCount : generatedIndices.size()));
	mesh->bones_.resize(_data->boneCount);
	for (size_t i = 0; i < _data->boneCount; ++i)
	{
		mesh->bones_[i].public_ = _data->bones[i];
		mesh->bones_[i].name_storage_ = _data->bones[i].name ? _data->bones[i].name : "Bone";
		mesh->bones_[i].public_.parentIndex = _data->bones[i].parentIndex < _data->boneCount ? _data->bones[i].parentIndex : HRL_INVALID_ID;
		for (int j = 0; j < 16; ++j)
			mesh->bones_[i].public_.inverseBindMatrix[j] = _data->bones[i].inverseBindMatrix[j];
		mesh->bones_[i].bind_local_ = _data->bones[i].bindTransform;
		glm::mat4 inverseBind(1.0f);
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
				inverseBind[col][row] = mesh->bones_[i].public_.inverseBindMatrix[col * 4 + row];
		mesh->bones_[i].inverse_bind_ = inverseBind;
		mesh->bones_[i].bind_world_ = HRLSkeletal_ArrayToGLM(_data->bones[i].bindWorldMatrix);
	}
	// Bind world matrices from the importer are exact; reconstruct only for legacy manually-created data.
	{
		const size_t boneCount = mesh->bones_.size();
		std::vector<unsigned char> built(boneCount, 0);
		std::function<glm::mat4(size_t)> buildBone = [&](size_t boneIndex) -> glm::mat4
		{
			if (built[boneIndex])
				return mesh->bones_[boneIndex].bind_world_;
			glm::mat4 parentWorld(1.f);
			const size_t parent = mesh->bones_[boneIndex].public_.parentIndex;
			if (parent < boneCount && parent != boneIndex)
				parentWorld = buildBone(parent);
			mesh->bones_[boneIndex].bind_world_ = parentWorld * HRLSkeletal_TransformToMatrix(mesh->bones_[boneIndex].bind_local_);
			built[boneIndex] = 1;
			return mesh->bones_[boneIndex].bind_world_;
		};
		for (size_t i = 0; i < boneCount; ++i)
		{
			bool hasBindWorld = false;
			for (int e = 0; e < 16; ++e)
				if (mesh->bones_[i].public_.bindWorldMatrix[e] != 0.f) { hasBindWorld = true; break; }
			if (!hasBindWorld)
				buildBone(i);
		}
	}

	// The baked animations are copied before public pointers are refreshed.
	mesh->animations_.resize(_data->animationCount);
	for (size_t i = 0; i < _data->animationCount; ++i)
	{
		mesh->animations_[i].public_ = _data->animations[i];
		mesh->animations_[i].name_storage_ = _data->animations[i].name ? _data->animations[i].name : "Animation";
		if (_data->boneCount != 0 && _data->animations[i].frameCount > std::numeric_limits<size_t>::max() / _data->boneCount)
		{
			g_Backend.RHI_DeleteMesh(newId);
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: animation frame data is too large");
			return HRL_INVALID_ID;
		}
		const size_t valueCount = _data->animations[i].frameCount * _data->boneCount;
		if (valueCount > 0 && !_data->animations[i].frames)
		{
			g_Backend.RHI_DeleteMesh(newId);
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: animation has no frame data");
			return HRL_INVALID_ID;
		}
		if (valueCount > 0)
			mesh->animations_[i].frames_.assign(_data->animations[i].frames, _data->animations[i].frames + valueCount);
		else
			mesh->animations_[i].frames_.clear();
		if (valueCount > 0 && _data->animations[i].poseMatrices)
			mesh->animations_[i].pose_matrices_storage_.assign(_data->animations[i].poseMatrices, _data->animations[i].poseMatrices + valueCount * 16u);
		else
			mesh->animations_[i].pose_matrices_storage_.clear();
	}

	for (auto& animation : mesh->animations_)
		HRLSkeletal_BuildPoseFramesForAnimation(mesh.get(), animation);

	mesh->RefreshPublicPointers();
	mesh->bone_matrices_.resize(mesh->bones_.size(), glm::mat4(1.f));
	HRLSkeletal_UpdatePose(mesh.get());

	glm::vec3 minPoint(std::numeric_limits<float>::max());
	glm::vec3 maxPoint(std::numeric_limits<float>::lowest());
	for (const auto& vertex : mesh->vertices_)
	{
		const glm::vec4 localPosition(vertex.vertex.position[0], vertex.vertex.position[1], vertex.vertex.position[2], 1.f);
		glm::vec4 skinnedPosition(0.f);
		float weightSum = 0.f;
		for (size_t influence = 0; influence < HRL_SKELETAL_MAX_INFLUENCES; ++influence)
		{
			const float weight = vertex.boneWeights[influence];
			if (!(weight > 0.f)) continue;
			const size_t boneIndex = (size_t)vertex.boneIndices[influence];
			if (boneIndex >= mesh->bone_matrices_.size()) continue;
			skinnedPosition += mesh->bone_matrices_[boneIndex] * localPosition * weight;
			weightSum += weight;
		}
		if (!(weightSum > 0.f))
			skinnedPosition = localPosition;
		else if (std::abs(weightSum - 1.f) > 1e-5f)
			skinnedPosition /= weightSum;
		const glm::vec3 p = glm::vec3(skinnedPosition);
		if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
		{
			minPoint = glm::min(minPoint, p);
			maxPoint = glm::max(maxPoint, p);
		}
	}
	if (!std::isfinite(minPoint.x) || !std::isfinite(maxPoint.x))
	{
		minPoint = glm::vec3(-1.f);
		maxPoint = glm::vec3(1.f);
	}
	mesh->bounds_center_ = (minPoint + maxPoint) * 0.5f;
	float radius2 = 0.f;
	for (const auto& vertex : mesh->vertices_)
	{
		const glm::vec4 localPosition(vertex.vertex.position[0], vertex.vertex.position[1], vertex.vertex.position[2], 1.f);
		glm::vec4 skinnedPosition(0.f);
		float weightSum = 0.f;
		for (size_t influence = 0; influence < HRL_SKELETAL_MAX_INFLUENCES; ++influence)
		{
			const float weight = vertex.boneWeights[influence];
			if (!(weight > 0.f)) continue;
			const size_t boneIndex = (size_t)vertex.boneIndices[influence];
			if (boneIndex >= mesh->bone_matrices_.size()) continue;
			skinnedPosition += mesh->bone_matrices_[boneIndex] * localPosition * weight;
			weightSum += weight;
		}
		if (!(weightSum > 0.f)) skinnedPosition = localPosition;
		else if (std::abs(weightSum - 1.f) > 1e-5f) skinnedPosition /= weightSum;
		const glm::vec3 p = glm::vec3(skinnedPosition);
		if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
			radius2 = std::max(radius2, glm::dot(p - mesh->bounds_center_, p - mesh->bounds_center_));
	}
	mesh->bounds_radius_ = std::sqrt(radius2);

	HRL_SkeletalMesh* rawMesh = mesh.release();
	sceneIt->second->meshes.emplace(newId, rawMesh);
	sceneIt->second->shadows_dirty = true;
	ctx_.meshes.emplace(newId, rawMesh);
	return newId;
}

HRL_uint HRL_GetSkeletalBoneCount(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalBoneCount: invalid skeletal mesh ID");
		return 0;
	}
	return (HRL_uint)static_cast<HRL_SkeletalMesh*>(it->second)->bones_.size();
}

const HRL_SkeletalBone* HRL_GetSkeletalBone(HRL_id _meshid, HRL_uint _index)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalBone: invalid skeletal mesh ID");
		return nullptr;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (_index >= mesh->bones_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalBone: invalid bone index");
		return nullptr;
	}
	return &mesh->bones_[_index].public_;
}

HRL_uint HRL_FindSkeletalBone(HRL_id _meshid, const char* _name)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH || !_name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_FindSkeletalBone: invalid mesh or bone name");
		return HRL_INVALID_ID;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	for (size_t i = 0; i < mesh->bones_.size(); ++i)
		if (mesh->bones_[i].name_storage_ == _name)
			return (HRL_uint)i;
	return HRL_INVALID_ID;
}

HRL_uint HRL_GetSkeletalAnimationCount(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimationCount: invalid skeletal mesh ID");
		return 0;
	}
	return (HRL_uint)static_cast<HRL_SkeletalMesh*>(it->second)->animations_.size();
}

const HRL_SkeletalAnimation* HRL_GetSkeletalAnimation(HRL_id _meshid, HRL_uint _index)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimation: invalid skeletal mesh ID");
		return nullptr;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (_index >= mesh->animations_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimation: invalid animation index");
		return nullptr;
	}
	return &mesh->animations_[_index].public_;
}

HRL_uint HRL_FindSkeletalAnimation(HRL_id _meshid, const char* _name)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH || !_name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_FindSkeletalAnimation: invalid mesh or animation name");
		return HRL_INVALID_ID;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	for (size_t i = 0; i < mesh->animations_.size(); ++i)
		if (mesh->animations_[i].name_storage_ == _name)
			return (HRL_uint)i;
	return HRL_INVALID_ID;
}

void HRL_PlaySkeletalAnimation(HRL_id _meshid, HRL_uint _animation)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_PlaySkeletalAnimation: invalid skeletal mesh ID");
		return;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (_animation >= mesh->animations_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_PlaySkeletalAnimation: invalid animation index");
		return;
	}
	mesh->current_animation_ = (int)_animation;
	mesh->animation_time_ = 0.f;
	mesh->animation_playing_ = true;
	HRLSkeletal_UpdatePose(mesh);
}

void HRL_StopSkeletalAnimation(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_StopSkeletalAnimation: invalid skeletal mesh ID");
		return;
	}
	static_cast<HRL_SkeletalMesh*>(it->second)->animation_playing_ = false;
}

void HRL_SetSkeletalAnimationTime(HRL_id _meshid, float _time)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationTime: invalid skeletal mesh ID");
		return;
	}
	if (!std::isfinite(_time))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationTime: time must be finite");
		return;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (mesh->current_animation_ < 0 || (size_t)mesh->current_animation_ >= mesh->animations_.size())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationTime: no animation is selected");
		return;
	}
	const float duration = std::max(mesh->animations_[(size_t)mesh->current_animation_].public_.duration, 0.f);
	if (mesh->animation_loop_ && duration > 0.f)
	{
		float wrapped = std::fmod(_time, duration);
		if (wrapped < 0.f) wrapped += duration;
		mesh->animation_time_ = wrapped;
	}
	else
		mesh->animation_time_ = std::clamp(_time, 0.f, duration);
	HRLSkeletal_UpdatePose(mesh);
}

void HRL_SetSkeletalAnimationSpeed(HRL_id _meshid, float _speed)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationSpeed: invalid skeletal mesh ID");
		return;
	}
	if (!std::isfinite(_speed))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationSpeed: speed must be finite");
		return;
	}
	static_cast<HRL_SkeletalMesh*>(it->second)->animation_speed_ = _speed;
}

void HRL_SetSkeletalAnimationLoop(HRL_id _meshid, int _loop)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationLoop: invalid skeletal mesh ID");
		return;
	}
	static_cast<HRL_SkeletalMesh*>(it->second)->animation_loop_ = _loop != HRL_FALSE;
}

int HRL_GetCurrentSkeletalAnimation(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetCurrentSkeletalAnimation: invalid skeletal mesh ID");
		return -1;
	}
	return static_cast<HRL_SkeletalMesh*>(it->second)->current_animation_;
}

float HRL_GetSkeletalAnimationTime(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimationTime: invalid skeletal mesh ID");
		return 0.f;
	}
	return static_cast<HRL_SkeletalMesh*>(it->second)->animation_time_;
}

int HRL_IsSkeletalAnimationPlaying(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_IsSkeletalAnimationPlaying: invalid skeletal mesh ID");
		return HRL_FALSE;
	}
	return static_cast<HRL_SkeletalMesh*>(it->second)->animation_playing_ ? HRL_TRUE : HRL_FALSE;
}

void HRL_UpdateSkeletalAnimations(float _deltaSeconds)
{
	if (!std::isfinite(_deltaSeconds) || _deltaSeconds < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_UpdateSkeletalAnimations: delta seconds must be finite and non-negative");
		return;
	}
	for (const auto& [id, baseMesh] : ctx_.meshes)
	{
		(void)id;
		if (!baseMesh || baseMesh->type_ != HRL_3D_SKELETAL_MESH)
			continue;
		auto* mesh = static_cast<HRL_SkeletalMesh*>(baseMesh);
		if (!mesh->animation_playing_ || mesh->current_animation_ < 0 || (size_t)mesh->current_animation_ >= mesh->animations_.size())
			continue;
		const float duration = std::max(mesh->animations_[(size_t)mesh->current_animation_].public_.duration, 0.f);
		if (duration <= 0.f)
		{
			mesh->animation_time_ = 0.f;
			mesh->animation_playing_ = false;
			HRLSkeletal_UpdatePose(mesh);
			continue;
		}
		mesh->animation_time_ += _deltaSeconds * mesh->animation_speed_;
		if (mesh->animation_loop_)
		{
			mesh->animation_time_ = std::fmod(mesh->animation_time_, duration);
			if (mesh->animation_time_ < 0.f) mesh->animation_time_ += duration;
		}
		else if (mesh->animation_time_ >= duration)
		{
			mesh->animation_time_ = duration;
			mesh->animation_playing_ = false;
		}
		else if (mesh->animation_time_ <= 0.f)
		{
			mesh->animation_time_ = 0.f;
			if (mesh->animation_speed_ < 0.f)
				mesh->animation_playing_ = false;
		}
		const bool poseChanged = HRLSkeletal_UpdatePose(mesh);
		if (poseChanged)
		{
			auto sceneIt = ctx_.scenes.find(mesh->scene_);
			if (sceneIt != ctx_.scenes.end() && sceneIt->second)
				sceneIt->second->shadows_dirty = true;
		}
	}
}

HRL_id HRL_CreateMesh(HRL_id _sceneid, HRL_EMeshType _type, const float *_vertices)
{
	(void)_sceneid;
	(void)_type;
	(void)_vertices;
	SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateMesh: reserved legacy entry point; use HRL_CreateMesh3D");
	return HRL_INVALID_ID;
}

HRL_id HRL_CreateMeshFromFile(HRL_id _sceneid, HRL_EMeshType _type, const char *_data, size_t _bufferSize)
{
	(void)_sceneid;
	(void)_type;
	(void)_data;
	(void)_bufferSize;
	SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateMeshFromFile: generic file-based mesh creation is reserved; use HRL_GetVertex3DFromFBX for FBX conversion");
	return HRL_INVALID_ID;
}

void HRL_DrawSceneAsDebugMode(HRL_id _sceneid, HRL_EDebugView mode)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawSceneAsDebugMode: invalid scene ID");
		return;
	}

	switch (mode)
	{
	case HRL_DEBUG_VIEW_NONE:
	case HRL_DEBUG_VIEW_UNLIT:
	case HRL_DEBUG_VIEW_NORMAL:
	case HRL_DEBUG_VIEW_LIGHTS:
	case HRL_DEBUG_VIEW_WIREFRAME:
	case HRL_DEBUG_VIEW_LOD:
	case HRL_DEBUG_VIEW_MESH_INFO:
		it->second->debug_view = mode;
		break;
	default:
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_DrawSceneAsDebugMode: invalid debug view mode");
		break;
	}
}



void HRL_SetDebugMeshInfoFont(HRL_id _sceneid, HRL_id _fontid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoFont: invalid scene ID");
		return;
	}
	if (_fontid != HRL_INVALID_ID && ctx_.fonts.find(_fontid) == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoFont: invalid font ID");
		return;
	}
	it->second->debug_mesh_info_font = _fontid;
}

void HRL_SetDebugMeshInfoTextSize(HRL_id _sceneid, float _size)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoTextSize: invalid scene ID");
		return;
	}
	if (!std::isfinite(_size) || _size <= 0.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoTextSize: size must be finite and positive");
		return;
	}
	// Debug mesh info text size is expressed in screen pixels.
	// Keep the value as-is so the OpenGL debug pass can render it at the
	// requested pixel height independently of the viewport resolution.
	it->second->debug_mesh_info_text_size = std::clamp(_size, 1.0f, 256.0f);
}

void HRL_SetDebugMeshInfoTextColor(HRL_id _sceneid, float r, float g, float b, float a)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoTextColor: invalid scene ID");
		return;
	}
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDebugMeshInfoTextColor: color values must be finite");
		return;
	}
	it->second->debug_mesh_info_text_color = glm::clamp(glm::vec4(r, g, b, a), glm::vec4(0.0f), glm::vec4(1.0f));
}

HRL_id HRL_AddScreenMessage(HRL_id _sceneid, float _duration_seconds, const char* _format, ...)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_AddScreenMessage: invalid scene ID");
		return HRL_INVALID_ID;
	}
	if (!_format || !_format[0])
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_AddScreenMessage: format must not be empty");
		return HRL_INVALID_ID;
	}
	if (!std::isfinite(_duration_seconds) || _duration_seconds <= 0.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_AddScreenMessage: duration must be finite and positive");
		return HRL_INVALID_ID;
	}

	va_list args;
	va_start(args, _format);
	va_list args_copy;
	va_copy(args_copy, args);
	const int required = std::vsnprintf(nullptr, 0, _format, args_copy);
	va_end(args_copy);

	if (required < 0)
	{
		va_end(args);
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_AddScreenMessage: invalid format string");
		return HRL_INVALID_ID;
	}

	std::string formatted(static_cast<size_t>(required) + 1, '\0');
	std::vsnprintf(formatted.data(), formatted.size(), _format, args);
	formatted.resize(static_cast<size_t>(required));
	va_end(args);

	HRL_ScreenMessage message;
	message.id = it->second->next_screen_message_id++;
	if (message.id == HRL_INVALID_ID)
		message.id = it->second->next_screen_message_id++;
	message.text = std::move(formatted);
	message.remaining_seconds = _duration_seconds;
	message.size = it->second->screen_message_text_size;
	message.color = it->second->screen_message_text_color;
	message.font = it->second->screen_message_font;
	if (message.font == HRL_INVALID_ID)
		message.font = it->second->debug_mesh_info_font;

	it->second->screen_messages.push_back(std::move(message));
	return it->second->screen_messages.back().id;
}

void HRL_SetScreenMessageTextSize(HRL_id _sceneid, float _size)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageTextSize: invalid scene ID");
		return;
	}
	if (!std::isfinite(_size) || _size <= 0.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageTextSize: size must be finite and positive");
		return;
	}
	it->second->screen_message_text_size = std::clamp(_size, 1.0f, 256.0f);
}

void HRL_SetScreenMessageTextColor(HRL_id _sceneid, float r, float g, float b, float a)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageTextColor: invalid scene ID");
		return;
	}
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageTextColor: color values must be finite");
		return;
	}
	it->second->screen_message_text_color = glm::clamp(glm::vec4(r, g, b, a), glm::vec4(0.0f), glm::vec4(1.0f));
}

void HRL_SetScreenMessageFont(HRL_id _sceneid, HRL_id _fontid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageFont: invalid scene ID");
		return;
	}
	if (_fontid != HRL_INVALID_ID && ctx_.fonts.find(_fontid) == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenMessageFont: invalid font ID");
		return;
	}
	it->second->screen_message_font = _fontid;
}

static HRL_Widget* FindWidget(HRL_id widget)
{
	auto it = ctx_.widgets.find(widget);
	return it == ctx_.widgets.end() ? nullptr : it->second;
}

static const HRL_Widget* FindWidgetConst(HRL_id widget)
{
	auto it = ctx_.widgets.find(widget);
	return it == ctx_.widgets.end() ? nullptr : it->second;
}

static int WidgetStateIndex(HRL_EWidgetState state)
{
	switch (state)
	{
	case HRL_WIDGET_STATE_IDLE: return 0;
	case HRL_WIDGET_STATE_HOVERED: return 1;
	case HRL_WIDGET_STATE_PRESSED: return 2;
	default: return -1;
	}
}

static void SetWidgetTypeError(const char* function, HRL_EWidgetType expected, HRL_id widget)
{
	(void)expected;
	(void)widget;
	SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, std::string(function) + ": invalid widget type");
}

template <typename T>
static T* FindTypedWidget(HRL_id widget, const char* function)
{
	HRL_Widget* base = FindWidget(widget);
	if (!base)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(function) + ": invalid widget ID");
		return nullptr;
	}
	T* typed = dynamic_cast<T*>(base);
	if (!typed)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, std::string(function) + ": invalid widget type");
		return nullptr;
	}
	return typed;
}

void HRL_MouseMovedCallback(float x, float y)
{
	ctx_.mouseX = x;
	ctx_.mouseY = y;
}

void HRL_MouseButtonCallback(int button, int pressed)
{
	if (button != HRL_MOUSE_BUTTON_LEFT)
		return;

	if (pressed == HRL_MOUSE_PRESS)
	{
		ctx_.mouseLeftDown = true;
		ctx_.mouseLeftPressed = true;
		ctx_.mouseCaptureWidget = HRL_INVALID_ID;
		ctx_.mouseCaptureGizmo = HRL_INVALID_ID;
		ctx_.mouseCaptureGizmoPart = HRL_GIZMO_PART_NONE;
	}
	else if (pressed == HRL_MOUSE_RELEASE)
	{
		ctx_.mouseLeftDown = false;
		ctx_.mouseLeftReleased = true;
	}
	else
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_MouseButtonCallback: pressed must be HRL_MOUSE_PRESS or HRL_MOUSE_RELEASE");
	}
}

HRL_id HRL_CreateWidget(HRL_id viewport, HRL_EWidgetType type)
{
	auto it = ctx_.viewports.find(viewport);
	if (it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateWidget: invalid viewport ID");
		return HRL_INVALID_ID;
	}

	HRL_id newId = GenerateHRL_ID();
	HRL_Widget* widget = nullptr;
	switch (type)
	{
	case HRL_WIDGET_BUTTON:      widget = new HRL_WidgetButton{}; break;
	case HRL_WIDGET_LABEL:       widget = new HRL_WidgetLabel{}; break;
	case HRL_WIDGET_IMAGE:       widget = new HRL_WidgetImage{}; break;
	case HRL_WIDGET_SLIDER:      widget = new HRL_WidgetSlider{}; break;
	case HRL_WIDGET_CHECKBOX:    widget = new HRL_WidgetCheckbox{}; break;
	case HRL_WIDGET_PROGRESSBAR: widget = new HRL_WidgetProgressBar{}; break;
	default:
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_CreateWidget: unsupported widget type");
		return HRL_INVALID_ID;
	}

	widget->SetId(newId);
	widget->SetViewport(viewport);
	ctx_.widgets.emplace(newId, widget);
	it->second->widgets.emplace(newId, widget);
	return newId;
}

void HRL_DeleteWidget(HRL_id widget)
{
	auto it = ctx_.widgets.find(widget);
	if (it == ctx_.widgets.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteWidget: invalid widget ID");
		return;
	}

	for (auto& [viewportId, viewport] : ctx_.viewports)
	{
		(void)viewportId;
		if (viewport) viewport->widgets.erase(widget);
	}

	delete it->second;
	ctx_.widgets.erase(it);
	if (ctx_.mouseCaptureWidget == widget)
		ctx_.mouseCaptureWidget = HRL_INVALID_ID;
}

int HRL_IsValidWidget(HRL_id widget)
{
	return FindWidgetConst(widget) ? HRL_TRUE : HRL_FALSE;
}

void HRL_SetWidgetPosition(HRL_id widget, float x, float y)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetPosition: invalid widget ID");
		return;
	}
	w->SetPosition(x, y);
}

void HRL_SetWidgetWorldPosition(HRL_id widget, float x, float y, float z)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetWorldPosition: invalid widget ID");
		return;
	}
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetWidgetWorldPosition: coordinates must be finite");
		return;
	}
	w->SetWorldPosition(x, y, z);
}

void HRL_SetWidgetWorldPositionEnabled(HRL_id widget, int enabled)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetWorldPositionEnabled: invalid widget ID");
		return;
	}
	w->SetWorldPositionEnabled(enabled != HRL_FALSE);
}

int HRL_IsWidgetWorldPositionEnabled(HRL_id widget)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_IsWidgetWorldPositionEnabled: invalid widget ID");
		return HRL_FALSE;
	}
	return w->IsWorldPositionEnabled() ? HRL_TRUE : HRL_FALSE;
}

void HRL_SetWidgetSize(HRL_id widget, float width, float height)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetSize: invalid widget ID");
		return;
	}
	w->SetScale(width, height);
}

void HRL_SetWidgetAlpha(HRL_id widget, float a)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetAlpha: invalid widget ID");
		return;
	}
	w->SetAlpha(a);
}

void HRL_SetWidgetAnchor(HRL_id widget, float ax, float ay)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetAnchor: invalid widget ID");
		return;
	}
	w->SetAnchor(ax, ay);
}

void HRL_SetWidgetVisible(HRL_id widget, int visible)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetVisible: invalid widget ID");
		return;
	}
	w->SetVisible(visible != HRL_FALSE);
}

void HRL_SetWidgetEnabled(HRL_id widget, int enabled)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetEnabled: invalid widget ID");
		return;
	}
	w->SetEnabled(enabled != HRL_FALSE);
}

void HRL_SetWidgetZIndex(HRL_id widget, int z_index)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetZIndex: invalid widget ID");
		return;
	}
	w->SetZIndex(z_index);
}

int HRL_IsWidgetHovered(HRL_id widget)
{
	HRL_Widget* w = FindWidget(widget);
	if (!w)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_IsWidgetHovered: invalid widget ID");
		return HRL_FALSE;
	}

	auto viewportIt = ctx_.viewports.find(w->GetViewport());
	if (viewportIt == ctx_.viewports.end() || !viewportIt->second)
		return HRL_FALSE;

	HRL_Viewport* viewport = viewportIt->second;
	w->UpdateInput(ctx_.mouseX, ctx_.mouseY,
		ctx_.mouseLeftDown, ctx_.mouseLeftPressed, ctx_.mouseLeftReleased,
		viewport->x_ * static_cast<float>(ctx_.window_width),
		viewport->y_ * static_cast<float>(ctx_.window_height),
		viewport->width_ * static_cast<float>(ctx_.window_width),
		viewport->height_ * static_cast<float>(ctx_.window_height));
	return w->IsHovered() ? HRL_TRUE : HRL_FALSE;
}

// BUTTON WIDGET
void HRL_SetButtonClickable(HRL_id widget, int clickable)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonClickable"))
		w->SetClickable(clickable != HRL_FALSE);
}

void HRL_SetButtonText(HRL_id widget, const char* text)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonText"))
		w->SetText(text);
}

void HRL_SetButtonTextSize(HRL_id widget, float size)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonTextSize"))
		w->SetTextSize(size);
}

void HRL_SetButtonTextTintColor(HRL_id widget, HRL_EWidgetState state, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonTextTintColor"))
	{
		const int index = WidgetStateIndex(state);
		if (index < 0)
		{
			SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetButtonTextTintColor: invalid widget state");
			return;
		}
		w->SetTextTintColor(state, {r, g, b, a});
	}
}

void HRL_SetButtonTextFont(HRL_id widget, HRL_id font)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonTextFont"))
	{
		if (!HRL_IsValidFont(font))
		{
			SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetButtonTextFont: invalid font ID");
			return;
		}
		w->SetFont(font);
	}
}

void HRL_SetButtonBackgroundTexture(HRL_id widget, HRL_EWidgetState state, HRL_id texture)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonBackgroundTexture"))
	{
		const int index = WidgetStateIndex(state);
		if (index < 0)
		{
			SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTexture: invalid widget state");
			return;
		}
		// Keep the historical API contract: texture 0 means the default white texture.
		if (texture == 0 || texture == HRL_INVALID_ID)
			texture = HRL_INVALID_ID;
		else if (!HRL_IsValidTexture(texture))
		{
			SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTexture: invalid texture ID");
			return;
		}
		w->SetBackgroundTexture(state, texture);
	}
}

void HRL_SetButtonBackgroundTintColor(HRL_id widget, HRL_EWidgetState state, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonBackgroundTintColor"))
	{
		if (WidgetStateIndex(state) < 0)
		{
			SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTintColor: invalid widget state");
			return;
		}
		w->SetBackgroundTintColor(state, {r, g, b, a});
	}
}

void HRL_SetButtonPressedCallback(HRL_id widget, HRL_CButtonPressed callback, void* user_data)
{
	if (auto* w = FindTypedWidget<HRL_WidgetButton>(widget, "HRL_SetButtonPressedCallback"))
	{
		w->pressed_callback = callback;
		w->pressed_user_data = user_data;
	}
}

// LABEL WIDGET
void HRL_SetLabelText(HRL_id widget, const char* text)
{
	if (auto* w = FindTypedWidget<HRL_WidgetLabel>(widget, "HRL_SetLabelText"))
		w->SetText(text);
}

void HRL_SetLabelTextSize(HRL_id widget, float size)
{
	if (auto* w = FindTypedWidget<HRL_WidgetLabel>(widget, "HRL_SetLabelTextSize"))
		w->SetTextSize(size);
}

void HRL_SetLabelFont(HRL_id widget, HRL_id font)
{
	if (auto* w = FindTypedWidget<HRL_WidgetLabel>(widget, "HRL_SetLabelFont"))
	{
		if (!HRL_IsValidFont(font))
		{
			SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLabelFont: invalid font ID");
			return;
		}
		w->SetFont(font);
	}
}

void HRL_SetLabelTintColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetLabel>(widget, "HRL_SetLabelTintColor"))
		w->SetTintColor({r, g, b, a});
}

// IMAGE WIDGET
void HRL_SetImageTexture(HRL_id widget, HRL_id texture)
{
	if (auto* w = FindTypedWidget<HRL_WidgetImage>(widget, "HRL_SetImageTexture"))
	{
		if (texture != HRL_INVALID_ID && !HRL_IsValidTexture(texture))
		{
			SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetImageTexture: invalid texture ID");
			return;
		}
		w->texture_ = texture;
	}
}

void HRL_SetImageTintColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetImage>(widget, "HRL_SetImageTintColor"))
		w->tint_color_ = {r, g, b, a};
}

// SLIDER WIDGET
void HRL_SetSliderRange(HRL_id widget, float minimum, float maximum)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderRange"))
	{
		if (maximum < minimum)
			std::swap(minimum, maximum);
		w->minimum_ = minimum;
		w->maximum_ = maximum;
		w->SetValue(w->value_);
	}
}

void HRL_SetSliderValue(HRL_id widget, float value)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderValue"))
		w->SetValue(value);
}

float HRL_GetSliderValue(HRL_id widget)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_GetSliderValue"))
		return w->value_;
	return 0.0f;
}

void HRL_SetSliderOrientation(HRL_id widget, HRL_ESliderOrientation orientation)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderOrientation"))
	{
		if (orientation != HRL_SLIDER_HORIZONTAL && orientation != HRL_SLIDER_VERTICAL)
		{
			SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetSliderOrientation: invalid orientation");
			return;
		}
		w->orientation_ = orientation;
	}
}

void HRL_SetSliderClickable(HRL_id widget, int clickable)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderClickable"))
		w->clickable_ = clickable != HRL_FALSE;
}

void HRL_SetSliderBackgroundColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderBackgroundColor"))
		w->background_color_ = {r, g, b, a};
}

void HRL_SetSliderFillColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderFillColor"))
		w->fill_color_ = {r, g, b, a};
}

void HRL_SetSliderHandleColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderHandleColor"))
		w->handle_color_ = {r, g, b, a};
}

void HRL_SetSliderChangedCallback(HRL_id widget, HRL_CSliderChanged callback, void* user_data)
{
	if (auto* w = FindTypedWidget<HRL_WidgetSlider>(widget, "HRL_SetSliderChangedCallback"))
	{
		w->changed_callback = callback;
		w->changed_user_data = user_data;
	}
}

// CHECKBOX WIDGET
void HRL_SetCheckboxChecked(HRL_id widget, int checked)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_SetCheckboxChecked"))
		w->SetChecked(checked != HRL_FALSE);
}

int HRL_IsCheckboxChecked(HRL_id widget)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_IsCheckboxChecked"))
		return w->checked_ ? HRL_TRUE : HRL_FALSE;
	return HRL_FALSE;
}

void HRL_SetCheckboxClickable(HRL_id widget, int clickable)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_SetCheckboxClickable"))
		w->clickable_ = clickable != HRL_FALSE;
}

void HRL_SetCheckboxBackgroundColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_SetCheckboxBackgroundColor"))
		w->background_color_ = {r, g, b, a};
}

void HRL_SetCheckboxCheckedColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_SetCheckboxCheckedColor"))
		w->checked_color_ = {r, g, b, a};
}

void HRL_SetCheckboxChangedCallback(HRL_id widget, HRL_CCheckboxChanged callback, void* user_data)
{
	if (auto* w = FindTypedWidget<HRL_WidgetCheckbox>(widget, "HRL_SetCheckboxChangedCallback"))
	{
		w->changed_callback = callback;
		w->changed_user_data = user_data;
	}
}

// PROGRESS BAR WIDGET
void HRL_SetProgressBarValue(HRL_id widget, float value)
{
	if (auto* w = FindTypedWidget<HRL_WidgetProgressBar>(widget, "HRL_SetProgressBarValue"))
		w->SetValue(value);
}

float HRL_GetProgressBarValue(HRL_id widget)
{
	if (auto* w = FindTypedWidget<HRL_WidgetProgressBar>(widget, "HRL_GetProgressBarValue"))
		return w->value_;
	return 0.0f;
}

void HRL_SetProgressBarBackgroundColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetProgressBar>(widget, "HRL_SetProgressBarBackgroundColor"))
		w->background_color_ = {r, g, b, a};
}

void HRL_SetProgressBarFillColor(HRL_id widget, float r, float g, float b, float a)
{
	if (auto* w = FindTypedWidget<HRL_WidgetProgressBar>(widget, "HRL_SetProgressBarFillColor"))
		w->fill_color_ = {r, g, b, a};
}

