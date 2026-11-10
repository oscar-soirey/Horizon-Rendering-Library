// HRL frontend - particle systems (simulation and API).
#include "frontend_internal.h"

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

	// Tout ce qui ne depend pas de la particule est calcule une seule fois par
	// emetteur (avant : recherches de courbes, matrice systeme + inverse et
	// matrices de tous les meshes de collision recalculees pour CHAQUE particule).
	const HRL_VFXCurve* colorCurve = FindVFXCurve(emitter->color_curve_);
	const HRL_VFXCurve* sizeCurve = FindVFXCurve(emitter->size_curve_);
	const HRL_VFXCurve* rotationCurve = FindVFXCurve(emitter->rotation_curve_);
	const glm::vec3 baseAccel = emitter->gravity_ + emitter->force_;
	const float noiseScale = emitter->noise_strength_ * emitter->noise_scroll_speed_;
	const float dragFactor = std::max(0.f, 1.f - emitter->drag_ * dt);
	const float restitution = glm::clamp(emitter->collision_restitution_, 0.f, 1.f);
	const float frictionFactor = std::max(0.f, 1.f - glm::clamp(emitter->collision_friction_, 0.f, 1.f) * dt * 8.f);

	struct CollisionSphere { glm::vec3 center; float radius; };
	static thread_local std::vector<CollisionSphere> collisionSpheres;
	collisionSpheres.clear();
	bool collideWithScene = false;
	glm::mat4 systemMatrix(1.f);
	glm::mat4 worldToLocal(1.f);
	if (emitter->collision_enabled_)
	{
		if (emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL)
		{
			systemMatrix = VFXSystemMatrix(system);
			worldToLocal = glm::inverse(systemMatrix);
		}
		if (emitter->collision_scene_ != HRL_INVALID_ID)
		{
			collideWithScene = true;
			auto collisionSceneIt = ctx_.scenes.find(emitter->collision_scene_);
			if (collisionSceneIt != ctx_.scenes.end() && collisionSceneIt->second)
			{
				// Meme ordre d'iteration que l'ancien code (le premier contact gagne).
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
					const float scale = std::max({std::abs(mesh->scale_.x), std::abs(mesh->scale_.y), std::abs(mesh->scale_.z)});
					collisionSpheres.push_back({
						glm::vec3(meshMatrix * glm::vec4(mesh->bounds_center_, 1.f)),
						std::max(0.001f, mesh->bounds_radius_ * scale) });
				}
			}
		}
	}

	for (auto& p : emitter->particles_)
	{
		if (p.lifetime <= 0.f) continue;
		p.age += dt;
		if (p.age >= p.lifetime) continue;
		const float normalizedLife = glm::clamp(p.age / p.lifetime, 0.f, 1.f);

		glm::vec3 accel = baseAccel;
		if (emitter->noise_strength_ > 0.f)
		{
			const float t = (system->time_ + p.age) * emitter->noise_frequency_ + static_cast<float>(p.seed % 97u);
			accel += glm::vec3(
				std::sin(t * 1.37f),
				std::cos(t * 1.91f),
				std::sin(t * 2.43f + 0.7f)) * noiseScale;
		}
		p.velocity += accel * dt;
		if (emitter->drag_ > 0.f)
			p.velocity *= dragFactor;
		p.position += p.velocity * dt;
		p.rotation = p.base_rotation + p.angular_velocity * p.age;

		if (emitter->collision_enabled_)
		{
			glm::vec3 worldPosition = glm::vec3(systemMatrix * glm::vec4(p.position, 1.f));
			glm::vec3 worldVelocity = VFXTransformDirection(systemMatrix, p.velocity) * glm::length(p.velocity);
			bool hit = false;

			if (collideWithScene)
			{
				for (const CollisionSphere& sphere : collisionSpheres)
				{
					const glm::vec3 delta = worldPosition - sphere.center;
					const float distance2 = glm::dot(delta, delta);
					if (distance2 < sphere.radius * sphere.radius)
					{
						const float distance = std::sqrt(distance2);
						const glm::vec3 normal = distance > 1e-5f ? delta / distance : glm::vec3(0.f,1.f,0.f);
						worldPosition = sphere.center + normal * sphere.radius;
						const float normalVelocity = glm::dot(worldVelocity, normal);
						if (normalVelocity < 0.f) worldVelocity -= (1.f + restitution) * normalVelocity * normal;
						const glm::vec3 tangent = worldVelocity - normal * glm::dot(worldVelocity, normal);
						worldVelocity = normal * glm::dot(worldVelocity, normal) + tangent * frictionFactor;
						hit = true;
						break;
					}
				}
			}
			else if (worldPosition.y < 0.f)
			{
				worldPosition.y = 0.f;
				if (worldVelocity.y < 0.f) worldVelocity.y = -worldVelocity.y * restitution;
				worldVelocity.x *= frictionFactor;
				worldVelocity.z *= frictionFactor;
				hit = true;
			}

			if (hit)
			{
				p.position = glm::vec3(worldToLocal * glm::vec4(worldPosition, 1.f));
				const float speed = glm::length(worldVelocity);
				p.velocity = speed > 1e-6f ? VFXTransformDirection(worldToLocal, worldVelocity) * speed : glm::vec3(0.f);
			}
		}

		if (colorCurve) p.color = EvalVFXColor(colorCurve, normalizedLife);
		if (sizeCurve)
		{
			const float scale = std::max(0.f, EvalVFXFloat(sizeCurve, normalizedLife, 1.f));
			p.size = emitter->particle_size_ * scale;
		}
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

void SimulateVFXSystem(HRL_VFXSystem* system, float dt)
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
