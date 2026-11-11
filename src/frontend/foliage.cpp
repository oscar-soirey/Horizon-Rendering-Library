// HRL frontend - foliage (instanced plants / rocks) and procedural grass.
//
// The frontend keeps the instances in cells of kFoliageCellSize x
// kFoliageCellSize world units (X / Z) and places them (scattering, painting,
// erasing). Rendering - culling, LOD, impostors, wind - is done by the
// backend, which uploads only the cells whose revision changed.
#include "frontend_internal.h"
#include "../core/render_common.h"

#include <glm/gtc/quaternion.hpp>

namespace
{
// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
HRL_Foliage* GetFoliage(HRL_id id, const char* fn)
{
	auto it = ctx_.foliage.find(id);
	if (it == ctx_.foliage.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(fn) + ": invalid foliage ID");
		return nullptr;
	}
	return it->second;
}

HRL_Grass* GetGrass(HRL_id id, const char* fn)
{
	auto it = ctx_.grass.find(id);
	if (it == ctx_.grass.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(fn) + ": invalid grass ID");
		return nullptr;
	}
	return it->second;
}

// Small deterministic generator (PCG32).
struct Random
{
	uint64_t state;
	explicit Random(uint64_t seed) : state(seed * 6364136223846793005ull + 1442695040888963407ull) { Next(); }
	uint32_t Next()
	{
		const uint64_t old = state;
		state = old * 6364136223846793005ull + 1442695040888963407ull;
		const uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
		const uint32_t rot = (uint32_t)(old >> 59u);
		return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
	}
	float Float() { return (float)(Next() >> 8) * (1.f / 16777216.f); }
	float Range(float a, float b) { return a + (b - a) * Float(); }
};

uint64_t CellKey(int x, int z) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(z); }
glm::ivec2 CellOf(float x, float z)
{
	return glm::ivec2((int)std::floor(x / kFoliageCellSize), (int)std::floor(z / kFoliageCellSize));
}

// Shortest rotation taking unit vector `a` to unit vector `b`.
glm::quat RotationBetween(const glm::vec3& a, const glm::vec3& b)
{
	const float d = glm::dot(a, b);
	if (d > 0.99999f) return glm::quat(1.f, 0.f, 0.f, 0.f);
	if (d < -0.99999f) return glm::angleAxis(glm::pi<float>(), glm::vec3(1.f, 0.f, 0.f));
	const glm::vec3 c = glm::cross(a, b);
	return glm::normalize(glm::quat(1.f + d, c.x, c.y, c.z));
}

// Same rotation as a mesh: rotate X (pitch), then Y (yaw), then Z (roll).
glm::quat EulerQuat(const glm::vec3& degrees)
{
	return glm::angleAxis(glm::radians(degrees.x), glm::vec3(1, 0, 0)) *
		glm::angleAxis(glm::radians(degrees.y), glm::vec3(0, 1, 0)) *
		glm::angleAxis(glm::radians(degrees.z), glm::vec3(0, 0, 1));
}

void GrowBounds(HRL_FoliageCell& cell, const HRL_Foliage* f, const HRL_FoliageInstanceData& d, bool first)
{
	const glm::quat q(d.rotation.w, d.rotation.x, d.rotation.y, d.rotation.z);
	const glm::vec3 center = glm::vec3(d.position_scale) + q * (f->bounds_center * d.position_scale.w);
	const glm::vec3 r(std::max(f->bounds_radius, 0.01f) * d.position_scale.w);
	if (first) { cell.bounds_min = center - r; cell.bounds_max = center + r; }
	else
	{
		cell.bounds_min = glm::min(cell.bounds_min, center - r);
		cell.bounds_max = glm::max(cell.bounds_max, center + r);
	}
}

void RecomputeBounds(HRL_FoliageCell& cell, const HRL_Foliage* f)
{
	for (size_t i = 0; i < cell.instances.size(); ++i) GrowBounds(cell, f, cell.instances[i], i == 0);
	if (cell.instances.empty()) cell.bounds_min = cell.bounds_max = glm::vec3(0.f);
}

HRL_FoliageCell& CellFor(HRL_Foliage* f, const glm::ivec2& c)
{
	const uint64_t key = CellKey(c.x, c.y);
	auto it = f->cell_of_key.find(key);
	if (it != f->cell_of_key.end()) return f->cells[it->second];
	f->cell_of_key.emplace(key, (uint32_t)f->cells.size());
	f->cells.emplace_back();
	f->cells.back().key = c;
	++f->cells_revision;
	return f->cells.back();
}

void AddInstance(HRL_Foliage* f, const glm::vec3& position, const glm::quat& rotation, float scale)
{
	HRL_FoliageInstanceData d;
	d.position_scale = glm::vec4(position, scale);
	d.rotation = glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w);
	HRL_FoliageCell& cell = CellFor(f, CellOf(position.x, position.z));
	GrowBounds(cell, f, d, cell.instances.empty());
	cell.instances.push_back(d);
	++cell.revision;
	++f->instance_count;
	++f->instances_revision;
}

// ---------------------------------------------------------------------------
// Ground sampling: a landscape or the triangles of a 3D mesh.
// ---------------------------------------------------------------------------
struct Ground
{
	const HRL_Landscape* landscape = nullptr;
	const HRL_Mesh* mesh = nullptr;
	glm::mat4 model{1.f};
	std::vector<glm::vec3> triangles; // world space, 3 per triangle (meshes)
	glm::vec3 lo{0.f}, hi{0.f};       // world bounds
};

bool MakeGround(HRL_id target, Ground& g, const char* fn)
{
	if (auto l = ctx_.landscapes.find(target); l != ctx_.landscapes.end() && l->second)
	{
		g.landscape = l->second;
		if (g.landscape->heights_w_ < 2)
		{
			SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, std::string(fn) + ": the landscape has no heights");
			return false;
		}
		g.model = CalculateLandscapeModelMatrix(g.landscape);
		g.lo = glm::vec3(std::numeric_limits<float>::max());
		g.hi = -g.lo;
		for (int i = 0; i < 4; ++i)
		{
			const glm::vec3 p = glm::vec3(g.model * glm::vec4((i & 1 ? 0.5f : -0.5f) * g.landscape->size_.x, 0.f,
				(i & 2 ? 0.5f : -0.5f) * g.landscape->size_.y, 1.f));
			g.lo = glm::min(g.lo, p);
			g.hi = glm::max(g.hi, p);
		}
		return true;
	}
	if (auto m = ctx_.meshes.find(target); m != ctx_.meshes.end() && m->second && m->second->type_ == HRL_3D_MESH &&
		!m->second->lods_.empty())
	{
		g.mesh = m->second;
		g.model = CalculateModelMatrix(g.mesh);
		const HRL_MeshLODData& lod = g.mesh->lods_[0];
		g.lo = glm::vec3(std::numeric_limits<float>::max());
		g.hi = -g.lo;
		for (size_t i = 0; i + 2 < lod.indices.size(); i += 3)
			for (int k = 0; k < 3; ++k)
			{
				const HRL_Vertex3D& v = lod.vertices[lod.indices[i + k]];
				const glm::vec3 p = glm::vec3(g.model * glm::vec4(v.position[0], v.position[1], v.position[2], 1.f));
				g.triangles.push_back(p);
				g.lo = glm::min(g.lo, p);
				g.hi = glm::max(g.hi, p);
			}
		return !g.triangles.empty();
	}
	SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(fn) + ": the target must be a landscape or a 3D mesh");
	return false;
}

// Highest point of the ground under (x, z), with its normal.
bool SampleGround(const Ground& g, float x, float z, glm::vec3& position, glm::vec3& normal)
{
	if (g.landscape) return SampleLandscape(g.landscape, x, z, &position, &normal);
	bool hit = false;
	for (size_t i = 0; i + 2 < g.triangles.size(); i += 3)
	{
		const glm::vec3 &a = g.triangles[i], &b = g.triangles[i + 1], &c = g.triangles[i + 2];
		// Vertical ray: barycentric coordinates in the XZ plane.
		const float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
		if (std::abs(d) < 1e-12f) continue;
		const float u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
		const float v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
		if (u < 0.f || v < 0.f || u + v > 1.f) continue;
		const glm::vec3 p = a * u + b * v + c * (1.f - u - v);
		if (hit && p.y <= position.y) continue;
		glm::vec3 n = glm::cross(b - a, c - a);
		if (n.y < 0.f) n = -n;
		if (glm::dot(n, n) < 1e-20f) continue;
		position = p;
		normal = glm::normalize(n);
		hit = true;
	}
	return hit;
}

// Spatial hash of the instances near the placed ones (minimal spacing).
struct SpacingGrid
{
	float cell = 1.f;
	std::unordered_map<uint64_t, std::vector<glm::vec2>> points;
	uint64_t Key(const glm::vec2& p) const
	{
		return CellKey((int)std::floor(p.x / cell), (int)std::floor(p.y / cell));
	}
	bool Free(const glm::vec2& p, float spacing) const
	{
		const int cx = (int)std::floor(p.x / cell), cz = (int)std::floor(p.y / cell);
		for (int z = cz - 1; z <= cz + 1; ++z)
			for (int x = cx - 1; x <= cx + 1; ++x)
			{
				auto it = points.find(CellKey(x, z));
				if (it == points.end()) continue;
				for (const glm::vec2& q : it->second)
					if (glm::dot(q - p, q - p) < spacing * spacing) return false;
			}
		return true;
	}
	void Add(const glm::vec2& p) { points[Key(p)].push_back(p); }
};

bool ValidScatter(const HRL_FoliageScatter* s, const char* fn)
{
	if (!s || !std::isfinite(s->density) || s->density <= 0.f || !std::isfinite(s->min_scale) || !std::isfinite(s->max_scale) ||
		s->min_scale <= 0.f || s->max_scale < s->min_scale || !std::isfinite(s->min_spacing) || s->min_spacing < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, std::string(fn) + ": invalid scatter settings (density > 0, 0 < min_scale <= max_scale)");
		return false;
	}
	return true;
}

// Places instances on the ground inside [lo, hi] (X / Z), optionally inside a circle.
size_t Scatter(HRL_Foliage* f, const Ground& ground, const HRL_FoliageScatter& s, glm::vec2 lo, glm::vec2 hi,
	const glm::vec3* circle)
{
	lo = glm::max(lo, glm::vec2(ground.lo.x, ground.lo.z));
	hi = glm::min(hi, glm::vec2(ground.hi.x, ground.hi.z));
	if (lo.x >= hi.x || lo.y >= hi.y) return 0;
	// Jittered grid: one candidate per cell of 1 / density square units.
	const float step = 1.f / std::sqrt(s.density);
	const double candidates = double((hi.x - lo.x) / step) * double((hi.y - lo.y) / step);
	if (candidates > 5.0e7)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_ScatterFoliage: too many instances (density x area > 50 million)");
		return 0;
	}
	SpacingGrid grid;
	if (s.min_spacing > 0.f)
	{
		grid.cell = s.min_spacing;
		// Existing instances near the area count too.
		const glm::ivec2 c0 = CellOf(lo.x - s.min_spacing, lo.y - s.min_spacing), c1 = CellOf(hi.x + s.min_spacing, hi.y + s.min_spacing);
		for (int z = c0.y; z <= c1.y; ++z)
			for (int x = c0.x; x <= c1.x; ++x)
			{
				auto it = f->cell_of_key.find(CellKey(x, z));
				if (it == f->cell_of_key.end()) continue;
				for (const HRL_FoliageInstanceData& d : f->cells[it->second].instances)
					grid.Add(glm::vec2(d.position_scale.x, d.position_scale.z));
			}
	}
	// The seed mixes in the area so repeated brush strokes are not identical.
	Random rng(uint64_t(s.seed) ^ (uint64_t(std::hash<float>{}(lo.x)) << 1) ^ uint64_t(std::hash<float>{}(lo.y)) ^ f->instance_count);
	const float minCos = std::cos(glm::radians(glm::clamp(s.max_slope, 0.f, 90.f)));
	const float maxCos = std::cos(glm::radians(glm::clamp(s.min_slope, 0.f, 90.f)));
	size_t added = 0;
	for (float z = lo.y; z < hi.y; z += step)
		for (float x = lo.x; x < hi.x; x += step)
		{
			const glm::vec2 p(x + rng.Float() * step, z + rng.Float() * step);
			const float scale = rng.Range(s.min_scale, s.max_scale);
			const float yaw = rng.Range(0.f, 360.f);
			const float tilt = rng.Range(-s.random_tilt, s.random_tilt), tiltDir = rng.Range(0.f, 360.f);
			if (p.x >= hi.x || p.y >= hi.y) continue;
			if (circle && (p.x - circle->x) * (p.x - circle->x) + (p.y - circle->z) * (p.y - circle->z) > circle->y * circle->y) continue;
			glm::vec3 position, normal;
			if (!SampleGround(ground, p.x, p.y, position, normal)) continue;
			if (normal.y < minCos - 1e-6f || normal.y > maxCos + 1e-6f) continue;
			if (position.y < s.min_height || position.y > s.max_height) continue;
			if (s.min_spacing > 0.f)
			{
				if (!grid.Free(p, s.min_spacing)) continue;
				grid.Add(p);
			}
			// Up axis: between world up and the ground normal, then a random lean.
			glm::vec3 up = glm::normalize(glm::mix(glm::vec3(0.f, 1.f, 0.f), normal, glm::clamp(s.align_to_normal, 0.f, 1.f)));
			glm::quat q = RotationBetween(glm::vec3(0.f, 1.f, 0.f), up);
			if (tilt != 0.f)
				q = glm::angleAxis(glm::radians(tilt), glm::vec3(std::cos(glm::radians(tiltDir)), 0.f, std::sin(glm::radians(tiltDir)))) * q;
			q = q * glm::angleAxis(glm::radians(yaw), glm::vec3(0.f, 1.f, 0.f));
			AddInstance(f, position - up * (s.sink * scale), glm::normalize(q), scale);
			++added;
		}
	return added;
}

// Foliage and grass are drawn by the Vulkan renderer only.
void WarnIfNotRendered(const char* fn)
{
	static bool warned = false;
	if (g_Backend.RHI_RenderSceneFrame || warned) return;
	warned = true;
	SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING,
		std::string(fn) + ": foliage and grass are drawn by the Vulkan backend only");
}

void RemovePart(HRL_FoliagePart& part)
{
	if (part.mesh_id != HRL_INVALID_ID && g_Backend.RHI_DeleteMesh) g_Backend.RHI_DeleteMesh(part.mesh_id);
	part.mesh_id = HRL_INVALID_ID;
}
} // namespace

// ---------------------------------------------------------------------------
// Foliage
// ---------------------------------------------------------------------------
HRL_FoliageScatter HRL_DefaultFoliageScatter(void)
{
	HRL_FoliageScatter s{};
	s.density = 0.05f;
	s.min_scale = 0.8f;
	s.max_scale = 1.2f;
	s.min_slope = 0.f;
	s.max_slope = 35.f;
	s.min_height = -1e30f;
	s.max_height = 1e30f;
	s.align_to_normal = 0.f;
	s.min_spacing = 0.f;
	s.sink = 0.f;
	s.random_tilt = 0.f;
	s.seed = 1;
	return s;
}

HRL_id HRL_CreateFoliage(HRL_id _sceneid)
{
	auto sceneIt = ctx_.scenes.find(_sceneid);
	if (sceneIt == ctx_.scenes.end() || !sceneIt->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateFoliage: invalid scene ID");
		return HRL_INVALID_ID;
	}
	auto* f = new (std::nothrow) HRL_Foliage();
	if (!f)
	{
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateFoliage: allocation failed");
		return HRL_INVALID_ID;
	}
	WarnIfNotRendered("HRL_CreateFoliage");
	const HRL_id id = GenerateHRL_ID();
	f->id_ = id;
	f->scene_ = _sceneid;
	sceneIt->second->foliage.emplace(id, f);
	ctx_.foliage.emplace(id, f);
	return id;
}

void HRL_DeleteFoliage(HRL_id _foliage)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_DeleteFoliage");
	if (!f) return;
	for (auto& part : f->parts) RemovePart(*part);
	if (auto it = ctx_.scenes.find(f->scene_); it != ctx_.scenes.end() && it->second) it->second->foliage.erase(_foliage);
	ctx_.foliage.erase(_foliage);
	delete f;
}

int HRL_IsValidFoliage(HRL_id _foliage)
{
	return ctx_.foliage.find(_foliage) != ctx_.foliage.end() ? HRL_TRUE : HRL_FALSE;
}

int HRL_AddFoliageMesh(HRL_id _foliage, const HRL_Vertex3D* _vertices, size_t _vertexCount,
	const HRL_uint* _indices, size_t _indexCount, HRL_id _material)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_AddFoliageMesh");
	if (!f) return -1;
	if (f->parts.size() >= kFoliageMaxParts)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_AddFoliageMesh: a foliage has at most 4 parts");
		return -1;
	}
	if (!_vertices || _vertexCount == 0 || (_indexCount == 0 && _vertexCount % 3) || (_indexCount && (!_indices || _indexCount % 3)))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_AddFoliageMesh: invalid geometry (triangles expected)");
		return -1;
	}
	for (size_t i = 0; i < _indexCount; ++i)
		if (_indices[i] >= _vertexCount)
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_AddFoliageMesh: index outside the vertex array");
			return -1;
		}
	for (size_t i = 0; i < _vertexCount; ++i)
		for (float v : _vertices[i].position)
			if (!std::isfinite(v))
			{
				SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_AddFoliageMesh: vertex position contains NaN or infinity");
				return -1;
			}
	auto part = std::make_unique<HRL_FoliagePart>();
	part->material = _material;
	HRL_Mesh& mesh = part->mesh;
	mesh.scene_ = HRL_INVALID_ID; // not in a scene: drawn through its foliage only
	mesh.type_ = HRL_3D_MESH;
	HRL_MeshLODData base;
	base.vertices.assign(_vertices, _vertices + _vertexCount);
	if (_indexCount) base.indices.assign(_indices, _indices + _indexCount);
	else
	{
		base.indices.resize(_vertexCount);
		for (size_t i = 0; i < _vertexCount; ++i) base.indices[i] = (HRL_uint)i;
	}
	mesh.lods_.push_back(std::move(base));
	mesh.triangle_count_ = mesh.lods_[0].indices.size() / 3;
	// Three levels of detail: LOD 0 near, then about 1/2 and 1/4 of the triangles.
	GenerateMeshLODs(&mesh, 3);

	part->mesh_id = GenerateHRL_ID();
	const HRL_MeshLODData& lod0 = mesh.lods_[0];
	if (!g_Backend.RHI_CreateMesh || g_Backend.RHI_CreateMesh(part->mesh_id, lod0.vertices.data(), lod0.vertices.size(),
		lod0.indices.data(), lod0.indices.size()) != HRL_TRUE)
		return -1;
	for (size_t level = 1; level < mesh.lods_.size(); ++level)
	{
		const HRL_MeshLODData& lod = mesh.lods_[level];
		if (!g_Backend.RHI_CreateMeshLOD || g_Backend.RHI_CreateMeshLOD(part->mesh_id, (HRL_uint)level,
			lod.vertices.data(), lod.vertices.size(), lod.indices.data(), lod.indices.size()) != HRL_TRUE)
		{
			mesh.lods_.resize(level);
			break;
		}
	}

	// Bounds of every part together.
	glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
	if (!f->parts.empty()) { lo = f->bounds_min; hi = f->bounds_max; }
	for (const HRL_Vertex3D& v : lod0.vertices)
	{
		const glm::vec3 p(v.position[0], v.position[1], v.position[2]);
		lo = glm::min(lo, p);
		hi = glm::max(hi, p);
	}
	f->bounds_min = lo;
	f->bounds_max = hi;
	f->bounds_center = (lo + hi) * 0.5f;
	f->bounds_radius = glm::length(hi - lo) * 0.5f;
	f->parts.push_back(std::move(part));
	++f->geometry_revision;
	// The cell bounds depend on the size of the foliage.
	for (HRL_FoliageCell& cell : f->cells)
	{
		RecomputeBounds(cell, f);
		++cell.revision;
	}
	++f->instances_revision;
	return (int)f->parts.size() - 1;
}

void HRL_SetFoliageMaterial(HRL_id _foliage, int _part, HRL_id _material)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_SetFoliageMaterial");
	if (!f) return;
	if (_part < 0 || (size_t)_part >= f->parts.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetFoliageMaterial: invalid part index");
		return;
	}
	f->parts[(size_t)_part]->material = _material;
	++f->geometry_revision;
}

size_t HRL_AddFoliageInstances(HRL_id _foliage, const HRL_FoliageInstance* _instances, size_t _count)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_AddFoliageInstances");
	if (!f) return 0;
	if (!_instances && _count)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_AddFoliageInstances: instances must not be null");
		return 0;
	}
	size_t added = 0;
	for (size_t i = 0; i < _count; ++i)
	{
		const HRL_FoliageInstance& in = _instances[i];
		const glm::vec3 p(in.position[0], in.position[1], in.position[2]);
		const glm::vec3 r(in.rotation[0], in.rotation[1], in.rotation[2]);
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(r.x) || !std::isfinite(r.y) ||
			!std::isfinite(r.z) || !std::isfinite(in.scale) || in.scale <= 0.f)
			continue;
		AddInstance(f, p, EulerQuat(r), in.scale);
		++added;
	}
	if (added < _count)
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING, "HRL_AddFoliageInstances: instances with invalid values were skipped");
	return added;
}

size_t HRL_ScatterFoliage(HRL_id _foliage, HRL_id _target, const HRL_FoliageScatter* _scatter)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_ScatterFoliage");
	Ground ground;
	if (!f || !ValidScatter(_scatter, "HRL_ScatterFoliage") || !MakeGround(_target, ground, "HRL_ScatterFoliage")) return 0;
	return Scatter(f, ground, *_scatter, glm::vec2(ground.lo.x, ground.lo.z), glm::vec2(ground.hi.x, ground.hi.z), nullptr);
}

size_t HRL_PaintFoliage(HRL_id _foliage, HRL_id _target, float _x, float _z, float _radius, const HRL_FoliageScatter* _scatter)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_PaintFoliage");
	Ground ground;
	if (!f || !ValidScatter(_scatter, "HRL_PaintFoliage")) return 0;
	if (!std::isfinite(_x) || !std::isfinite(_z) || !std::isfinite(_radius) || _radius <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_PaintFoliage: invalid brush");
		return 0;
	}
	if (!MakeGround(_target, ground, "HRL_PaintFoliage")) return 0;
	const glm::vec3 circle(_x, _radius, _z);
	return Scatter(f, ground, *_scatter, glm::vec2(_x - _radius, _z - _radius), glm::vec2(_x + _radius, _z + _radius), &circle);
}

size_t HRL_EraseFoliage(HRL_id _foliage, float _x, float _z, float _radius)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_EraseFoliage");
	if (!f || !std::isfinite(_x) || !std::isfinite(_z) || !std::isfinite(_radius) || _radius <= 0.f) return 0;
	const glm::ivec2 c0 = CellOf(_x - _radius, _z - _radius), c1 = CellOf(_x + _radius, _z + _radius);
	size_t removed = 0;
	for (int z = c0.y; z <= c1.y; ++z)
		for (int x = c0.x; x <= c1.x; ++x)
		{
			auto it = f->cell_of_key.find(CellKey(x, z));
			if (it == f->cell_of_key.end()) continue;
			HRL_FoliageCell& cell = f->cells[it->second];
			const size_t before = cell.instances.size();
			cell.instances.erase(std::remove_if(cell.instances.begin(), cell.instances.end(), [&](const HRL_FoliageInstanceData& d)
			{
				const float dx = d.position_scale.x - _x, dz = d.position_scale.z - _z;
				return dx * dx + dz * dz <= _radius * _radius;
			}), cell.instances.end());
			if (cell.instances.size() == before) continue;
			removed += before - cell.instances.size();
			RecomputeBounds(cell, f);
			++cell.revision;
			++f->instances_revision;
		}
	f->instance_count -= removed;
	return removed;
}

void HRL_ClearFoliage(HRL_id _foliage)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_ClearFoliage");
	if (!f) return;
	f->cells.clear();
	f->cell_of_key.clear();
	f->instance_count = 0;
	++f->cells_revision;
	++f->instances_revision;
}

size_t HRL_GetFoliageInstanceCount(HRL_id _foliage)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_GetFoliageInstanceCount");
	return f ? f->instance_count : 0;
}

size_t HRL_GetFoliageInstances(HRL_id _foliage, HRL_FoliageInstance* _out, size_t _max)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_GetFoliageInstances");
	if (!f || !_out) return 0;
	size_t n = 0;
	for (const HRL_FoliageCell& cell : f->cells)
		for (const HRL_FoliageInstanceData& d : cell.instances)
		{
			if (n >= _max) return n;
			HRL_FoliageInstance& o = _out[n++];
			o.position[0] = d.position_scale.x;
			o.position[1] = d.position_scale.y;
			o.position[2] = d.position_scale.z;
			o.scale = d.position_scale.w;
			// Back to pitch / yaw / roll (X, then Y, then Z).
			const glm::mat3 m = glm::mat3_cast(glm::quat(d.rotation.w, d.rotation.x, d.rotation.y, d.rotation.z));
			const float sy = glm::clamp(m[2][0], -1.f, 1.f);
			const float yaw = std::asin(sy);
			float pitch, roll;
			if (std::abs(sy) < 0.9999f)
			{
				pitch = std::atan2(-m[2][1], m[2][2]);
				roll = std::atan2(-m[1][0], m[0][0]);
			}
			else
			{
				pitch = std::atan2(m[1][2], m[1][1]);
				roll = 0.f;
			}
			o.rotation[0] = glm::degrees(pitch);
			o.rotation[1] = glm::degrees(yaw);
			o.rotation[2] = glm::degrees(roll);
		}
	return n;
}

void HRL_SetFoliageDrawDistance(HRL_id _foliage, float _distance)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_SetFoliageDrawDistance");
	if (!f) return;
	if (!std::isfinite(_distance) || _distance <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetFoliageDrawDistance: distance must be > 0");
		return;
	}
	f->draw_distance = _distance;
}

void HRL_SetFoliageLODDistance(HRL_id _foliage, float _distance)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_SetFoliageLODDistance");
	if (!f) return;
	if (!std::isfinite(_distance) || _distance < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetFoliageLODDistance: distance must be >= 0");
		return;
	}
	f->lod_distance = _distance;
}

void HRL_SetFoliageImpostorDistance(HRL_id _foliage, float _distance)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_SetFoliageImpostorDistance");
	if (!f) return;
	if (!std::isfinite(_distance) || _distance < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetFoliageImpostorDistance: distance must be >= 0");
		return;
	}
	f->impostor_distance = _distance;
}

void HRL_SetFoliageCastShadows(HRL_id _foliage, int _cast)
{
	if (HRL_Foliage* f = GetFoliage(_foliage, "HRL_SetFoliageCastShadows")) f->cast_shadows = _cast != 0;
}

void HRL_SetFoliageWindResponse(HRL_id _foliage, float _response)
{
	HRL_Foliage* f = GetFoliage(_foliage, "HRL_SetFoliageWindResponse");
	if (!f) return;
	if (!std::isfinite(_response) || _response < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetFoliageWindResponse: response must be >= 0");
		return;
	}
	f->wind_response = _response;
}

void HRL_SetWind(HRL_id _sceneid, float _dirX, float _dirZ, float _strength, float _gustiness)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWind: invalid scene ID");
		return;
	}
	const float len = std::sqrt(_dirX * _dirX + _dirZ * _dirZ);
	if (!std::isfinite(len) || !std::isfinite(_strength) || !std::isfinite(_gustiness) || _strength < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetWind: invalid values");
		return;
	}
	const glm::vec2 d = len > 1e-6f ? glm::vec2(_dirX, _dirZ) / len : glm::vec2(1.f, 0.f);
	it->second->wind = glm::vec4(d.x, d.y, _strength, glm::clamp(_gustiness, 0.f, 1.f));
}

void HRL_SetFoliageInteractor(HRL_id _sceneid, HRL_uint _slot, float _x, float _y, float _z, float _radius)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFoliageInteractor: invalid scene ID");
		return;
	}
	if (_slot >= kFoliageMaxInteractors || !std::isfinite(_x) || !std::isfinite(_y) || !std::isfinite(_z) ||
		!std::isfinite(_radius) || _radius < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetFoliageInteractor: slot must be < 16 and radius >= 0");
		return;
	}
	it->second->foliage_interactors[_slot] = HRL_FoliageInteractor{glm::vec3(_x, _y, _z), _radius};
}

// ---------------------------------------------------------------------------
// Grass
// ---------------------------------------------------------------------------
HRL_id HRL_CreateGrass(HRL_id _landscape)
{
	auto l = ctx_.landscapes.find(_landscape);
	if (l == ctx_.landscapes.end() || !l->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateGrass: invalid landscape ID");
		return HRL_INVALID_ID;
	}
	auto sceneIt = ctx_.scenes.find(l->second->scene_);
	if (sceneIt == ctx_.scenes.end() || !sceneIt->second) return HRL_INVALID_ID;
	auto* g = new (std::nothrow) HRL_Grass();
	if (!g)
	{
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateGrass: allocation failed");
		return HRL_INVALID_ID;
	}
	WarnIfNotRendered("HRL_CreateGrass");
	const HRL_id id = GenerateHRL_ID();
	g->id_ = id;
	g->scene_ = l->second->scene_;
	g->landscape_ = _landscape;
	sceneIt->second->grass.emplace(id, g);
	ctx_.grass.emplace(id, g);
	return id;
}

void HRL_DeleteGrass(HRL_id _grass)
{
	HRL_Grass* g = GetGrass(_grass, "HRL_DeleteGrass");
	if (!g) return;
	if (auto it = ctx_.scenes.find(g->scene_); it != ctx_.scenes.end() && it->second) it->second->grass.erase(_grass);
	ctx_.grass.erase(_grass);
	delete g;
}

void HRL_SetGrassDensity(HRL_id _grass, float _density)
{
	HRL_Grass* g = GetGrass(_grass, "HRL_SetGrassDensity");
	if (!g) return;
	if (!std::isfinite(_density) || _density <= 0.f || _density > 2000.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetGrassDensity: density must be in (0, 2000]");
		return;
	}
	g->density = _density;
}

void HRL_SetGrassBladeSize(HRL_id _grass, float _height, float _width, float _variation)
{
	HRL_Grass* g = GetGrass(_grass, "HRL_SetGrassBladeSize");
	if (!g) return;
	if (!std::isfinite(_height) || !std::isfinite(_width) || !std::isfinite(_variation) || _height <= 0.f || _width <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetGrassBladeSize: height and width must be > 0");
		return;
	}
	g->blade_height = _height;
	g->blade_width = _width;
	g->height_variation = glm::clamp(_variation, 0.f, 1.f);
}

void HRL_SetGrassColors(HRL_id _grass, float _baseR, float _baseG, float _baseB, float _tipR, float _tipG, float _tipB)
{
	HRL_Grass* g = GetGrass(_grass, "HRL_SetGrassColors");
	if (!g) return;
	g->base_color = glm::max(glm::vec3(_baseR, _baseG, _baseB), glm::vec3(0.f));
	g->tip_color = glm::max(glm::vec3(_tipR, _tipG, _tipB), glm::vec3(0.f));
}

void HRL_SetGrassDistance(HRL_id _grass, float _distance)
{
	HRL_Grass* g = GetGrass(_grass, "HRL_SetGrassDistance");
	if (!g) return;
	if (!std::isfinite(_distance) || _distance <= 0.f || _distance > 1000.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetGrassDistance: distance must be in (0, 1000]");
		return;
	}
	g->distance = _distance;
}

void HRL_SetGrassMaxSlope(HRL_id _grass, float _degrees)
{
	if (HRL_Grass* g = GetGrass(_grass, "HRL_SetGrassMaxSlope")) g->max_slope = glm::clamp(_degrees, 0.f, 90.f);
}

void HRL_SetGrassMask(HRL_id _grass, HRL_id _texture)
{
	HRL_Grass* g = GetGrass(_grass, "HRL_SetGrassMask");
	if (!g) return;
	if (_texture != HRL_INVALID_ID && (!g_Backend.RHI_IsValidTexture || !g_Backend.RHI_IsValidTexture(_texture)))
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGrassMask: invalid texture ID");
		return;
	}
	g->mask = _texture;
}

void HRL_SetGrassCastShadows(HRL_id _grass, int _cast)
{
	if (HRL_Grass* g = GetGrass(_grass, "HRL_SetGrassCastShadows")) g->cast_shadows = _cast != 0;
}
