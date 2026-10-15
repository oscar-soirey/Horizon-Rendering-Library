#include "gi_scene.h"

#include "render_common.h"
#include "utils_functions.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

extern HRL_Context* GetPrivateContext();

HRL_GIAtlasLayout HRL_GIAtlasLayout::For(int resolution)
{
	HRL_GIAtlasLayout l;
	l.resolution = std::max(1, resolution);
	l.tilesPerRow = std::max(1, (int)std::ceil(std::sqrt((double)l.resolution)));
	const int rows = (l.resolution + l.tilesPerRow - 1) / l.tilesPerRow;
	l.width = l.resolution * l.tilesPerRow;
	l.height = l.resolution * rows;
	return l;
}

namespace
{
constexpr int kMaxTriangles = 300000;
constexpr int kLeafSize = 4;
constexpr int kMaxDepth = 40;     // shader traversal stack is 48 entries
constexpr int kSAHBins = 12;
constexpr int kLandscapeMaxCells = 96;

struct BuildTriangle
{
	glm::vec3 p0, p1, p2;
	glm::vec3 albedo;
	glm::vec3 bmin, bmax, centroid;
};

// --- Materials --------------------------------------------------------------
const HRL_Material* FindMaterial(HRL_id id)
{
	HRL_Context* ctx = GetPrivateContext();
	if (!ctx) return nullptr;
	auto it = ctx->materials.find(id);
	return it == ctx->materials.end() ? nullptr : it->second;
}

struct MaterialSampler
{
	glm::vec3 tint{1.f};
	bool hasAlbedoParam = false;
	float metallic = 0.f;
	bool metallicFromTexture = false;
	const std::vector<unsigned char>* albedo = nullptr;
	int aw = 0, ah = 0;
	const std::vector<unsigned char>* metal = nullptr;
	int mw = 0, mh = 0;

	explicit MaterialSampler(HRL_id materialId)
	{
		const HRL_Material* m = FindMaterial(materialId);
		if (!m) return;
		auto tintIt = m->vec3Params_.find("TintColor");
		if (tintIt != m->vec3Params_.end()) tint = glm::max(tintIt->second, glm::vec3(0.f));
		auto texIt = m->textureParams_.find("T_Albedo");
		if (texIt != m->textureParams_.end() && texIt->second != HRL_INVALID_ID)
		{
			hasAlbedoParam = true;
			albedo = HRL_InternalGetTextureCPUData(texIt->second, &aw, &ah);
			if (albedo && albedo->size() != (size_t)aw * (size_t)ah * 4u) albedo = nullptr;
		}
		bool useValue = false;
		auto useIt = m->intParams_.find("MetallicUseValue");
		if (useIt != m->intParams_.end()) useValue = useIt->second != 0;
		auto valIt = m->floatParams_.find("MetallicValue");
		if (valIt != m->floatParams_.end()) metallic = std::clamp(valIt->second, 0.f, 1.f);
		if (!useValue)
		{
			auto mt = m->textureParams_.find("T_Metallic");
			if (mt != m->textureParams_.end() && mt->second != HRL_INVALID_ID)
			{
				metal = HRL_InternalGetTextureCPUData(mt->second, &mw, &mh);
				if (metal && metal->size() != (size_t)mw * (size_t)mh * 4u) metal = nullptr;
				metallicFromTexture = metal != nullptr;
			}
			if (!metallicFromTexture) metallic = 0.f; // fallback texture of the renderer is black
		}
	}

	static glm::vec4 Fetch(const std::vector<unsigned char>& px, int w, int h, glm::vec2 uv)
	{
		const float u = uv.x - std::floor(uv.x);
		const float v = uv.y - std::floor(uv.y);
		const int x = std::clamp((int)(u * (float)w), 0, w - 1);
		const int y = std::clamp((int)(v * (float)h), 0, h - 1);
		const size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4u;
		return glm::vec4(px[i], px[i + 1], px[i + 2], px[i + 3]) / 255.f;
	}

	// Diffuse albedo in linear space, as the mesh shader computes it.
	glm::vec3 Diffuse(glm::vec2 uv) const
	{
		// Without albedo texture the renderer samples its fallback checker,
		// whose average linear value is 0.117.
		glm::vec3 base(hasAlbedoParam ? 1.f : 0.117f);
		if (albedo) base = glm::pow(glm::max(glm::vec3(Fetch(*albedo, aw, ah, uv)), glm::vec3(0.f)), glm::vec3(2.2f));
		float m = metallic;
		if (metallicFromTexture && metal) m = Fetch(*metal, mw, mh, uv).r;
		return glm::clamp(base * tint * (1.f - m), glm::vec3(0.f), glm::vec3(1.f));
	}
};

glm::mat4 MeshModelMatrix(const HRL_Mesh* mesh)
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

void AddMesh(std::vector<BuildTriangle>& out, const std::vector<HRL_Vertex3D>& verts, const std::vector<HRL_uint>& indices,
	const glm::mat4& model, HRL_id material)
{
	if (verts.empty()) return;
	const MaterialSampler mat(material);
	auto emit = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
		if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size() || (int)out.size() >= kMaxTriangles) return;
		const HRL_Vertex3D* v[3] = {&verts[i0], &verts[i1], &verts[i2]};
		BuildTriangle t;
		glm::vec3* p[3] = {&t.p0, &t.p1, &t.p2};
		glm::vec2 uvSum(0.f);
		glm::vec3 albedoSum(0.f);
		for (int k = 0; k < 3; ++k)
		{
			*p[k] = glm::vec3(model * glm::vec4(v[k]->position[0], v[k]->position[1], v[k]->position[2], 1.f));
			const glm::vec2 uv(v[k]->uv[0], v[k]->uv[1]);
			uvSum += uv;
			albedoSum += mat.Diffuse(uv);
		}
		const glm::vec3 n = glm::cross(t.p1 - t.p0, t.p2 - t.p0);
		if (!(glm::dot(n, n) > 1e-14f)) return;
		albedoSum += mat.Diffuse(uvSum / 3.f) * 2.f;
		t.albedo = albedoSum / 5.f;
		t.bmin = glm::min(t.p0, glm::min(t.p1, t.p2));
		t.bmax = glm::max(t.p0, glm::max(t.p1, t.p2));
		t.centroid = (t.p0 + t.p1 + t.p2) / 3.f;
		out.push_back(t);
	};
	if (!indices.empty())
		for (size_t i = 0; i + 2 < indices.size(); i += 3) emit(indices[i], indices[i + 1], indices[i + 2]);
	else
		for (size_t i = 0; i + 2 < verts.size(); i += 3) emit((uint32_t)i, (uint32_t)i + 1, (uint32_t)i + 2);
}

// --- BVH (binned SAH) -------------------------------------------------------
struct BuildNode { glm::vec3 bmin, bmax; int left = -1, right = -1, first = 0, count = 0; };

float Area(const glm::vec3& mn, const glm::vec3& mx)
{
	const glm::vec3 e = glm::max(mx - mn, glm::vec3(0.f));
	return 2.f * (e.x * e.y + e.y * e.z + e.z * e.x);
}

int BuildRecursive(std::vector<BuildNode>& nodes, std::vector<BuildTriangle>& tris, int first, int count, int depth)
{
	BuildNode node;
	node.bmin = glm::vec3(std::numeric_limits<float>::max());
	node.bmax = glm::vec3(std::numeric_limits<float>::lowest());
	glm::vec3 cmin = node.bmin, cmax = node.bmax;
	for (int i = first; i < first + count; ++i)
	{
		node.bmin = glm::min(node.bmin, tris[i].bmin);
		node.bmax = glm::max(node.bmax, tris[i].bmax);
		cmin = glm::min(cmin, tris[i].centroid);
		cmax = glm::max(cmax, tris[i].centroid);
	}
	const int index = (int)nodes.size();
	nodes.push_back(node);
	if (count <= kLeafSize || depth >= kMaxDepth)
	{
		nodes[index].first = first;
		nodes[index].count = count;
		return index;
	}

	// Binned SAH over the three axes.
	int bestAxis = -1, bestSplit = -1;
	float bestCost = std::numeric_limits<float>::max();
	const glm::vec3 extent = cmax - cmin;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (extent[axis] < 1e-6f) continue;
		struct Bin { glm::vec3 mn{std::numeric_limits<float>::max()}, mx{std::numeric_limits<float>::lowest()}; int n = 0; } bins[kSAHBins];
		const float scale = (float)kSAHBins / extent[axis];
		for (int i = first; i < first + count; ++i)
		{
			int b = std::min(kSAHBins - 1, (int)((tris[i].centroid[axis] - cmin[axis]) * scale));
			bins[b].n++;
			bins[b].mn = glm::min(bins[b].mn, tris[i].bmin);
			bins[b].mx = glm::max(bins[b].mx, tris[i].bmax);
		}
		float leftArea[kSAHBins], rightArea[kSAHBins];
		int leftCount[kSAHBins], rightCount[kSAHBins];
		glm::vec3 mn(std::numeric_limits<float>::max()), mx(std::numeric_limits<float>::lowest());
		int n = 0;
		for (int b = 0; b < kSAHBins; ++b)
		{
			n += bins[b].n; mn = glm::min(mn, bins[b].mn); mx = glm::max(mx, bins[b].mx);
			leftCount[b] = n; leftArea[b] = n ? Area(mn, mx) : 0.f;
		}
		mn = glm::vec3(std::numeric_limits<float>::max()); mx = glm::vec3(std::numeric_limits<float>::lowest()); n = 0;
		for (int b = kSAHBins - 1; b >= 0; --b)
		{
			n += bins[b].n; mn = glm::min(mn, bins[b].mn); mx = glm::max(mx, bins[b].mx);
			rightCount[b] = n; rightArea[b] = n ? Area(mn, mx) : 0.f;
		}
		for (int s = 0; s < kSAHBins - 1; ++s)
		{
			if (leftCount[s] == 0 || rightCount[s + 1] == 0) continue;
			const float cost = leftArea[s] * (float)leftCount[s] + rightArea[s + 1] * (float)rightCount[s + 1];
			if (cost < bestCost) { bestCost = cost; bestAxis = axis; bestSplit = s; }
		}
	}

	int mid = first + count / 2;
	if (bestAxis >= 0)
	{
		const float scale = (float)kSAHBins / extent[bestAxis];
		auto it = std::partition(tris.begin() + first, tris.begin() + first + count, [&](const BuildTriangle& t) {
			return std::min(kSAHBins - 1, (int)((t.centroid[bestAxis] - cmin[bestAxis]) * scale)) <= bestSplit;
		});
		mid = (int)(it - tris.begin());
	}
	if (mid == first || mid == first + count)
	{
		// Degenerate split : median on the longest axis.
		int axis = 0;
		if (extent.y > extent[axis]) axis = 1;
		if (extent.z > extent[axis]) axis = 2;
		mid = first + count / 2;
		std::nth_element(tris.begin() + first, tris.begin() + mid, tris.begin() + first + count,
			[axis](const BuildTriangle& a, const BuildTriangle& b) { return a.centroid[axis] < b.centroid[axis]; });
	}
	const int left = BuildRecursive(nodes, tris, first, mid - first, depth + 1);
	const int right = BuildRecursive(nodes, tris, mid, first + count - mid, depth + 1);
	nodes[index].left = left;
	nodes[index].right = right;
	return index;
}

// --- Voxelization ------------------------------------------------------------
// Triangle / box overlap (Akenine-Moller).
bool AxisTest(const glm::vec3& axis, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& h)
{
	const float p0 = glm::dot(v0, axis), p1 = glm::dot(v1, axis), p2 = glm::dot(v2, axis);
	const float r = h.x * std::abs(axis.x) + h.y * std::abs(axis.y) + h.z * std::abs(axis.z);
	return !(std::max(p0, std::max(p1, p2)) < -r || std::min(p0, std::min(p1, p2)) > r);
}

bool TriangleBox(const glm::vec3& center, const glm::vec3& h, const BuildTriangle& t)
{
	const glm::vec3 v0 = t.p0 - center, v1 = t.p1 - center, v2 = t.p2 - center;
	const glm::vec3 e0 = v1 - v0, e1 = v2 - v1, e2 = v0 - v2;
	const glm::vec3 axes[3] = {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)};
	const glm::vec3 edges[3] = {e0, e1, e2};
	for (const auto& a : axes)
		for (const auto& e : edges)
		{
			const glm::vec3 axis = glm::cross(a, e);
			if (glm::dot(axis, axis) > 1e-12f && !AxisTest(axis, v0, v1, v2, h)) return false;
		}
	for (int i = 0; i < 3; ++i)
	{
		const float mn = std::min(v0[i], std::min(v1[i], v2[i]));
		const float mx = std::max(v0[i], std::max(v1[i], v2[i]));
		if (mn > h[i] || mx < -h[i]) return false;
	}
	const glm::vec3 n = glm::cross(e0, e1);
	return AxisTest(n, v0, v1, v2, h);
}

void Voxelize(const std::vector<BuildTriangle>& tris, HRL_GISceneData& d)
{
	const int R = kHRLGIVoxelResolution;
	const glm::vec3 extent = d.boundsMax - d.boundsMin;
	const float size = std::max(extent.x, std::max(extent.y, extent.z));
	// One voxel of margin on each side.
	d.voxelResolution = R;
	d.voxelSize = std::max(size, 1e-3f) / (float)(R - 2);
	const glm::vec3 center = (d.boundsMin + d.boundsMax) * 0.5f;
	d.voxelOrigin = center - glm::vec3(d.voxelSize * (float)R * 0.5f);

	const size_t count = (size_t)R * R * R;
	std::vector<glm::vec4> albedo(count, glm::vec4(0.f));
	std::vector<glm::vec3> normal(count, glm::vec3(0.f));
	const glm::vec3 half(d.voxelSize * 0.5f);
	for (const auto& t : tris)
	{
		const glm::ivec3 lo = glm::clamp(glm::ivec3(glm::floor((t.bmin - d.voxelOrigin) / d.voxelSize)), glm::ivec3(0), glm::ivec3(R - 1));
		const glm::ivec3 hi = glm::clamp(glm::ivec3(glm::floor((t.bmax - d.voxelOrigin) / d.voxelSize)), glm::ivec3(0), glm::ivec3(R - 1));
		glm::vec3 n = glm::cross(t.p1 - t.p0, t.p2 - t.p0);
		const float area = 0.5f * glm::length(n);
		n = glm::normalize(n);
		for (int z = lo.z; z <= hi.z; ++z)
			for (int y = lo.y; y <= hi.y; ++y)
				for (int x = lo.x; x <= hi.x; ++x)
				{
					const glm::vec3 c = d.voxelOrigin + (glm::vec3(x, y, z) + 0.5f) * d.voxelSize;
					if (!TriangleBox(c, half, t)) continue;
					const size_t i = ((size_t)z * R + (size_t)y) * R + (size_t)x;
					const float w = std::max(area, 1e-6f);
					albedo[i] += glm::vec4(t.albedo * w, w);
					normal[i] += n * w;
				}
	}
	const HRL_GIAtlasLayout atlas = HRL_GIAtlasLayout::For(R);
	d.voxelAlbedo.assign((size_t)atlas.width * atlas.height * 4u, 0);
	d.voxelNormal.assign((size_t)atlas.width * atlas.height * 4u, 0);
	for (int z = 0; z < R; ++z)
		for (int y = 0; y < R; ++y)
			for (int x = 0; x < R; ++x)
			{
				const size_t i = ((size_t)z * R + (size_t)y) * R + (size_t)x;
				if (albedo[i].w <= 0.f) continue;
				const int tx = x + (z % atlas.tilesPerRow) * R;
				const int ty = y + (z / atlas.tilesPerRow) * R;
				const size_t o = ((size_t)ty * atlas.width + (size_t)tx) * 4u;
				const glm::vec3 a = glm::clamp(glm::vec3(albedo[i]) / albedo[i].w, glm::vec3(0.f), glm::vec3(1.f));
				glm::vec3 n = normal[i];
				const float l = glm::length(n);
				n = l > 1e-6f ? n / l : glm::vec3(0.f);
				for (int k = 0; k < 3; ++k)
				{
					d.voxelAlbedo[o + k] = (unsigned char)std::lround(a[k] * 255.f);
					d.voxelNormal[o + k] = (unsigned char)std::lround((n[k] * 0.5f + 0.5f) * 255.f);
				}
				d.voxelAlbedo[o + 3] = 255;
				d.voxelNormal[o + 3] = l > 1e-6f ? 255 : 0;
			}
}

void LayoutProbes(HRL_GISceneData& d)
{
	// Probes sit at the centers of the cells of a grid covering the scene, so
	// that none of them lies exactly on the outer surfaces (walls, floors).
	const glm::vec3 extent = glm::max(d.boundsMax - d.boundsMin, glm::vec3(1e-3f));
	const float longest = std::max(extent.x, std::max(extent.y, extent.z));
	float spacing = longest / (float)kHRLGIMaxProbesPerAxis;
	glm::ivec3 counts;
	for (;;)
	{
		counts = glm::clamp(glm::ivec3(glm::ceil(extent / spacing - 1e-3f)), glm::ivec3(2), glm::ivec3(kHRLGIMaxProbesPerAxis));
		if (counts.x * counts.y * counts.z <= kHRLGIMaxProbeCount) break;
		spacing *= 1.1f;
	}
	d.probeCounts = counts;
	d.probeSpacing = extent / glm::vec3(counts);
	d.probeOrigin = d.boundsMin + d.probeSpacing * 0.5f;
}
} // namespace

bool HRL_UpdateGISceneData(HRL_id sceneId, HRL_GISceneData& data)
{
	HRL_Context* ctx = GetPrivateContext();
	if (!ctx) return false;
	auto sceneIt = ctx->scenes.find(sceneId);
	if (sceneIt == ctx->scenes.end() || !sceneIt->second) return false;
	const hrl_scene_t* scene = sceneIt->second;
	if (data.geometryRevision == scene->gi_geometry_revision && data.geometryRevision != 0)
		return data.valid;

	data = HRL_GISceneData{};
	data.geometryRevision = scene->gi_geometry_revision;

	std::vector<BuildTriangle> tris;
	tris.reserve(16384);
	for (const auto& [id, mesh] : scene->meshes)
	{
		(void)id;
		if (!mesh || mesh->type_ != HRL_3D_MESH || mesh->lods_.empty()) continue;
		AddMesh(tris, mesh->lods_[0].vertices, mesh->lods_[0].indices, MeshModelMatrix(mesh), mesh->material_);
	}
	for (const auto& [id, landscape] : scene->landscapes)
	{
		(void)id;
		if (!landscape) continue;
		HRL_LandscapeHeightmap heightmap;
		heightmap.rgba = HRL_InternalGetTextureCPUData(landscape->heightmap_, &heightmap.width, &heightmap.height);
		HRL_Landscape coarse = *landscape;
		coarse.resolution_x_ = std::min<HRL_uint>(coarse.resolution_x_, kLandscapeMaxCells);
		coarse.resolution_z_ = std::min<HRL_uint>(coarse.resolution_z_, kLandscapeMaxCells);
		std::vector<HRL_Vertex3D> verts;
		std::vector<HRL_uint> indices;
		glm::vec3 center;
		float radius = 0.f;
		if (BuildLandscapeGeometry(&coarse, heightmap.rgba ? &heightmap : nullptr, verts, indices, center, radius))
			AddMesh(tris, verts, indices, CalculateLandscapeModelMatrix(landscape), landscape->material_);
	}
	if (tris.empty())
		return false;

	data.boundsMin = glm::vec3(std::numeric_limits<float>::max());
	data.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
	for (const auto& t : tris)
	{
		data.boundsMin = glm::min(data.boundsMin, t.bmin);
		data.boundsMax = glm::max(data.boundsMax, t.bmax);
	}

	std::vector<BuildNode> nodes;
	nodes.reserve(tris.size() * 2 / kLeafSize + 1);
	BuildRecursive(nodes, tris, 0, (int)tris.size(), 0);

	data.nodes.resize(nodes.size() * 2);
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		const BuildNode& n = nodes[i];
		const bool leaf = n.left < 0;
		data.nodes[i * 2] = glm::vec4(n.bmin, leaf ? (float)n.first : (float)n.left);
		data.nodes[i * 2 + 1] = glm::vec4(n.bmax, leaf ? -(float)n.count : (float)n.right);
	}
	data.triangles.resize(tris.size() * 3);
	for (size_t i = 0; i < tris.size(); ++i)
	{
		data.triangles[i * 3 + 0] = glm::vec4(tris[i].p0, tris[i].albedo.r);
		data.triangles[i * 3 + 1] = glm::vec4(tris[i].p1, tris[i].albedo.g);
		data.triangles[i * 3 + 2] = glm::vec4(tris[i].p2, tris[i].albedo.b);
	}
	data.triangleCount = (uint32_t)tris.size();
	data.nodeCount = (uint32_t)nodes.size();

	Voxelize(tris, data);
	LayoutProbes(data);
	data.valid = true;
	return true;
}
