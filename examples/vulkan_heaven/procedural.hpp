// HRL example "Heaven": procedural geometry. Everything is smooth: tubes
// follow curves with parallel transported frames, ends are rounded, normals
// are those of the analytic surfaces.
#pragma once

#include <hrl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace geo
{
constexpr float kPi = 3.14159265358979f;

struct V3
{
	float x = 0.f, y = 0.f, z = 0.f;
	V3() = default;
	V3(float a, float b, float c) : x(a), y(b), z(c) {}
	V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
	V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
	V3 operator*(float s) const { return {x * s, y * s, z * s}; }
	V3 operator-() const { return {-x, -y, -z}; }
	V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};
inline float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(const V3& a) { return std::sqrt(Dot(a, a)); }
inline V3 Normalize(const V3& a) { const float l = Length(a); return l > 1e-8f ? a * (1.f / l) : V3(0.f, 1.f, 0.f); }
inline V3 Lerp(const V3& a, const V3& b, float t) { return a + (b - a) * t; }

// Small deterministic generator (the scene is the same on every run).
struct Rng
{
	uint32_t s;
	explicit Rng(uint32_t seed) : s(seed * 2654435761u + 1u) {}
	uint32_t Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
	float Uniform() { return (float)(Next() & 0xFFFFFF) / 16777215.f; }
	float Range(float a, float b) { return a + (b - a) * Uniform(); }
	V3 Direction()
	{
		const float z = Range(-1.f, 1.f), a = Range(0.f, 2.f * kPi), r = std::sqrt(1.f - z * z);
		return {r * std::cos(a), z, r * std::sin(a)};
	}
};

// Smooth value noise for the shapes (not the shading).
inline float Hash3(int x, int y, int z)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.f;
}
inline float Noise3(float x, float y, float z)
{
	const int ix = (int)std::floor(x), iy = (int)std::floor(y), iz = (int)std::floor(z);
	float fx = x - ix, fy = y - iy, fz = z - iz;
	fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy); fz = fz * fz * (3 - 2 * fz);
	auto l = [](float a, float b, float t) { return a + (b - a) * t; };
	return l(l(l(Hash3(ix, iy, iz), Hash3(ix + 1, iy, iz), fx), l(Hash3(ix, iy + 1, iz), Hash3(ix + 1, iy + 1, iz), fx), fy),
		l(l(Hash3(ix, iy, iz + 1), Hash3(ix + 1, iy, iz + 1), fx), l(Hash3(ix, iy + 1, iz + 1), Hash3(ix + 1, iy + 1, iz + 1), fx), fy), fz);
}

struct Mesh
{
	std::vector<HRL_Vertex3D> vertices;
	std::vector<HRL_uint> indices;

	HRL_uint Add(const V3& p, const V3& n, float u, float v, const V3& t)
	{
		HRL_Vertex3D r{};
		r.position[0] = p.x; r.position[1] = p.y; r.position[2] = p.z;
		r.normal[0] = n.x; r.normal[1] = n.y; r.normal[2] = n.z;
		r.uv[0] = u; r.uv[1] = v;
		r.tangent[0] = t.x; r.tangent[1] = t.y; r.tangent[2] = t.z;
		const V3 b = Cross(n, t);
		r.bitangent[0] = b.x; r.bitangent[1] = b.y; r.bitangent[2] = b.z;
		vertices.push_back(r);
		return (HRL_uint)vertices.size() - 1;
	}
	void Tri(HRL_uint a, HRL_uint b, HRL_uint c) { indices.insert(indices.end(), {a, b, c}); }
	void Append(const Mesh& o)
	{
		const HRL_uint base = (HRL_uint)vertices.size();
		vertices.insert(vertices.end(), o.vertices.begin(), o.vertices.end());
		for (HRL_uint i : o.indices) indices.push_back(base + i);
	}
	// Every vertex gets this u (birds: 0.5 on the body, the wing flap leaves it still).
	void SetU(float u) { for (HRL_Vertex3D& v : vertices) v.uv[0] = u; }
};

// Ellipsoid, counter-clockwise from outside.
inline Mesh Ellipsoid(const V3& c, const V3& r, int rings = 16, int sectors = 24)
{
	Mesh m;
	for (int i = 0; i <= rings; ++i)
	{
		const float v = (float)i / rings, th = v * kPi;
		for (int j = 0; j <= sectors; ++j)
		{
			const float u = (float)j / sectors, ph = u * 2.f * kPi;
			const V3 s(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
			const V3 n = Normalize(V3(s.x / r.x, s.y / r.y, s.z / r.z));
			m.Add(c + V3(s.x * r.x, s.y * r.y, s.z * r.z), n, u, v, V3(-std::sin(ph), 0.f, std::cos(ph)));
		}
	}
	for (int i = 0; i < rings; ++i)
		for (int j = 0; j < sectors; ++j)
		{
			const HRL_uint a = (HRL_uint)(i * (sectors + 1) + j), b = a + (HRL_uint)sectors + 1;
			m.Tri(a, a + 1, b);
			m.Tri(a + 1, b + 1, b);
		}
	return m;
}

// Tube along a polyline, radius per point, rounded ends (hemispheres).
inline Mesh Tube(const std::vector<V3>& pts, const std::vector<float>& radii, int sides, bool roundStart = true, bool roundEnd = true)
{
	Mesh m;
	const size_t n = pts.size();
	if (n < 2) return m;
	std::vector<V3> tangents(n);
	for (size_t i = 0; i < n; ++i)
		tangents[i] = Normalize(pts[std::min(i + 1, n - 1)] - pts[i > 0 ? i - 1 : 0]);
	// Parallel transport: no twist along the curve.
	V3 normal = Normalize(Cross(tangents[0], std::fabs(tangents[0].y) < 0.9f ? V3(0, 1, 0) : V3(1, 0, 0)));
	std::vector<V3> normals(n);
	normals[0] = normal;
	for (size_t i = 1; i < n; ++i)
	{
		const V3 t = tangents[i];
		normal = Normalize(normal - t * Dot(normal, t));
		normals[i] = normal;
	}
	float length = 0.f;
	for (size_t i = 0; i < n; ++i)
	{
		if (i) length += Length(pts[i] - pts[i - 1]);
		const V3 t = tangents[i], a = normals[i], b = Cross(t, a);
		for (int s = 0; s <= sides; ++s)
		{
			const float ang = (float)s / sides * 2.f * kPi;
			const V3 dir = a * std::cos(ang) + b * std::sin(ang);
			m.Add(pts[i] + dir * radii[i], dir, (float)s / sides, length, t);
		}
	}
	for (size_t i = 0; i + 1 < n; ++i)
		for (int s = 0; s < sides; ++s)
		{
			const HRL_uint a = (HRL_uint)(i * (sides + 1) + s), b = a + (HRL_uint)sides + 1;
			m.Tri(a, a + 1, b);
			m.Tri(a + 1, b + 1, b);
		}
	auto cap = [&](size_t i, float sign)
	{
		const int rings = std::max(3, sides / 3);
		const V3 t = tangents[i] * sign, a = normals[i], b = Cross(tangents[i], a);
		const HRL_uint base = (HRL_uint)m.vertices.size();
		for (int r = 1; r <= rings; ++r)
		{
			const float phi = (float)r / rings * kPi * 0.5f;
			for (int s = 0; s <= sides; ++s)
			{
				const float ang = (float)s / sides * 2.f * kPi;
				const V3 dir = Normalize((a * std::cos(ang) + b * std::sin(ang)) * std::cos(phi) + t * std::sin(phi));
				m.Add(pts[i] + dir * radii[i], dir, (float)s / sides, length, tangents[i]);
			}
		}
		const HRL_uint ring0 = (HRL_uint)(i * (sides + 1));
		for (int r = 0; r < rings; ++r)
			for (int s = 0; s < sides; ++s)
			{
				const HRL_uint a0 = r == 0 ? ring0 + (HRL_uint)s : base + (HRL_uint)((r - 1) * (sides + 1) + s);
				const HRL_uint b0 = base + (HRL_uint)(r * (sides + 1) + s);
				if (sign > 0.f) { m.Tri(a0, a0 + 1, b0); m.Tri(a0 + 1, b0 + 1, b0); }
				else { m.Tri(a0, b0, a0 + 1); m.Tri(a0 + 1, b0, b0 + 1); }
			}
	};
	if (roundStart) cap(0, -1.f);
	if (roundEnd) cap(n - 1, 1.f);
	return m;
}

// Height field of `size` metres, `res` x `res` quads, centred on the origin.
inline Mesh Terrain(float size, int res, const std::function<float(float, float)>& height)
{
	Mesh m;
	const float step = size / res, e = step * 0.5f;
	for (int j = 0; j <= res; ++j)
		for (int i = 0; i <= res; ++i)
		{
			const float x = -size * 0.5f + i * step, z = -size * 0.5f + j * step;
			const float hx = height(x + e, z) - height(x - e, z), hz = height(x, z + e) - height(x, z - e);
			const V3 n = Normalize(V3(-hx, 2.f * e, -hz));
			m.Add(V3(x, height(x, z), z), n, (float)i / res * 16.f, (float)j / res * 16.f, V3(1, 0, 0));
		}
	for (int j = 0; j < res; ++j)
		for (int i = 0; i < res; ++i)
		{
			const HRL_uint a = (HRL_uint)(j * (res + 1) + i), b = a + (HRL_uint)res + 1;
			m.Tri(a, b, a + 1);
			m.Tri(a + 1, b, b + 1);
		}
	return m;
}

// A crooked branch: points bent by noise, then children (recursive).
inline void Branch(Mesh& out, V3 start, V3 dir, float length, float radius, int depth, int maxDepth, Rng& rng,
	std::vector<std::pair<V3, V3>>* tips = nullptr)
{
	const int segments = std::max(4, 12 - depth * 2);
	std::vector<V3> pts;
	std::vector<float> radii;
	V3 p = start, d = Normalize(dir);
	const float seed = rng.Range(0.f, 100.f);
	for (int i = 0; i <= segments; ++i)
	{
		const float t = (float)i / segments;
		pts.push_back(p);
		radii.push_back(radius * (1.f - 0.62f * t) * (1.f + 0.12f * (Noise3(seed, t * 6.f, 0.f) - 0.5f)));
		// Gnarled: the direction wanders, branches slowly reach up then droop.
		const V3 wander(Noise3(seed + t * 3.f, 0.f, 1.f) - 0.5f, Noise3(seed, t * 3.f, 2.f) - 0.5f, Noise3(1.f, seed, t * 3.f) - 0.5f);
		d = Normalize(d + wander * 0.55f + V3(0.f, depth == 0 ? 0.04f : (depth > 1 ? -0.05f : 0.03f), 0.f));
		p += d * (length / segments);
	}
	out.Append(Tube(pts, radii, std::max(6, 16 - depth * 3), depth == 0, true));
	if (depth >= maxDepth)
	{
		if (tips) tips->push_back({pts.back(), d});
		return;
	}
	const int children = depth == 0 ? 3 : 2 + (int)(rng.Uniform() * 1.5f);
	for (int c = 0; c < children; ++c)
	{
		const float at = rng.Range(depth == 0 ? 0.45f : 0.35f, 0.92f);
		const size_t idx = std::min(pts.size() - 1, (size_t)(at * segments));
		const V3 side = Normalize(Cross(d, rng.Direction()));
		const V3 cd = Normalize(d * rng.Range(0.35f, 0.75f) + side * rng.Range(0.6f, 1.1f) + V3(0.f, rng.Range(-0.1f, 0.35f), 0.f));
		Branch(out, pts[idx], cd, length * rng.Range(0.55f, 0.72f), radii[idx] * rng.Range(0.6f, 0.75f), depth + 1, maxDepth, rng, tips);
	}
}

// A dead, twisted tree with root flares; `tips` receives the ends of the smallest branches.
inline Mesh Tree(Rng& rng, float height, std::vector<std::pair<V3, V3>>* tips = nullptr)
{
	Mesh m;
	const float radius = height * rng.Range(0.035f, 0.05f);
	Branch(m, V3(0.f, -0.4f, 0.f), V3(rng.Range(-0.15f, 0.15f), 1.f, rng.Range(-0.15f, 0.15f)), height * 0.55f, radius, 0, 3, rng, tips);
	const int roots = 4 + (int)(rng.Uniform() * 3.f);
	for (int r = 0; r < roots; ++r)
	{
		const float a = (float)r / roots * 2.f * kPi + rng.Range(-0.3f, 0.3f);
		std::vector<V3> pts;
		std::vector<float> radii;
		for (int i = 0; i <= 6; ++i)
		{
			const float t = (float)i / 6.f;
			pts.push_back(V3(std::cos(a) * t * radius * 5.f, 0.5f * radius * (1.f - t) * 2.f - t * 0.35f, std::sin(a) * t * radius * 5.f));
			radii.push_back(radius * (0.75f - 0.6f * t));
		}
		m.Append(Tube(pts, radii, 10));
	}
	return m;
}

// Vines hanging from a branch tip: a slack curve swaying under its own weight.
inline Mesh Vine(const V3& top, float length, Rng& rng)
{
	std::vector<V3> pts;
	std::vector<float> radii;
	const V3 drift(rng.Range(-0.3f, 0.3f), 0.f, rng.Range(-0.3f, 0.3f));
	for (int i = 0; i <= 14; ++i)
	{
		const float t = (float)i / 14.f;
		pts.push_back(top + V3(0.f, -length * t, 0.f) + drift * std::sin(t * kPi));
		radii.push_back(0.012f + 0.01f * std::sin(t * 23.f + rng.Range(0.f, 6.f)) * std::sin(t * kPi));
	}
	Mesh m = Tube(pts, radii, 5);
	// Knots along the vine.
	for (int k = 0; k < 4; ++k)
	{
		const float t = rng.Range(0.2f, 0.95f);
		m.Append(Ellipsoid(Lerp(pts.front(), pts.back(), t) + drift * std::sin(t * kPi), V3(0.035f, 0.05f, 0.035f), 5, 8));
	}
	return m;
}

// Tall, thin watcher: big oval head, long limbs. Faces +Z.
inline Mesh Figure(float height, Rng& rng, bool spear = false)
{
	Mesh m;
	const float s = height / 2.6f;
	auto limb = [&](std::vector<V3> pts, float r0, float r1)
	{
		std::vector<float> radii;
		for (size_t i = 0; i < pts.size(); ++i) radii.push_back((r0 + (r1 - r0) * (float)i / (pts.size() - 1)) * s);
		for (V3& p : pts) p = p * s;
		m.Append(Tube(pts, radii, 12));
	};
	const float lean = rng.Range(-0.08f, 0.08f);
	// Head, neck, torso.
	m.Append(Ellipsoid(V3(lean, 2.36f, 0.f) * s, V3(0.17f, 0.24f, 0.19f) * s, 18, 24));
	limb({V3(lean * 0.9f, 2.17f, 0.f), V3(lean * 0.7f, 2.0f, 0.01f), V3(lean * 0.5f, 1.88f, 0.f)}, 0.045f, 0.06f);
	limb({V3(lean * 0.5f, 1.88f, 0.f), V3(lean * 0.3f, 1.55f, 0.02f), V3(lean * 0.1f, 1.22f, 0.f), V3(0.f, 1.05f, 0.f)}, 0.15f, 0.1f);
	// Arms: hanging, long, slightly bent; one raised when it holds a spear.
	for (int side = -1; side <= 1; side += 2)
	{
		const float sx = (float)side;
		if (spear && side > 0)
		{
			limb({V3(0.16f, 1.82f, 0.f), V3(0.3f, 1.55f, 0.08f), V3(0.36f, 1.42f, 0.22f)}, 0.04f, 0.03f);
			m.Append(Ellipsoid(V3(0.36f, 1.4f, 0.24f) * s, V3(0.04f, 0.05f, 0.04f) * s, 6, 8));
			std::vector<V3> shaft = {V3(0.37f, 0.2f, 0.26f) * s, V3(0.37f, 2.9f, 0.2f) * s};
			m.Append(Tube(shaft, {0.016f * s, 0.012f * s}, 6));
			m.Append(Ellipsoid(V3(0.37f, 2.98f, 0.2f) * s, V3(0.025f, 0.11f, 0.012f) * s, 6, 8));
			continue;
		}
		limb({V3(0.15f * sx, 1.83f, 0.f), V3(0.22f * sx, 1.5f, 0.02f), V3(0.25f * sx, 1.15f, 0.05f), V3(0.24f * sx, 0.85f, 0.04f)}, 0.042f, 0.03f);
		m.Append(Ellipsoid(V3(0.24f * sx, 0.8f, 0.04f) * s, V3(0.03f, 0.07f, 0.03f) * s, 6, 8));
	}
	// Legs.
	for (int side = -1; side <= 1; side += 2)
	{
		const float sx = (float)side * 0.08f;
		limb({V3(sx, 1.1f, 0.f), V3(sx * 1.1f, 0.6f, 0.03f), V3(sx * 1.2f, 0.05f, 0.f)}, 0.065f, 0.04f);
	}
	return m;
}

// A crow: body, head, beak, tail; wings carry their span in u (0.5 at the body).
inline Mesh Bird(float span = 0.9f)
{
	Mesh body = Ellipsoid(V3(0.f, 0.f, 0.f), V3(0.07f, 0.06f, 0.18f), 8, 12);
	body.Append(Ellipsoid(V3(0.f, 0.03f, 0.17f), V3(0.045f, 0.045f, 0.05f), 6, 10));
	body.Append(Ellipsoid(V3(0.f, 0.025f, 0.23f), V3(0.012f, 0.01f, 0.04f), 4, 6));
	body.Append(Ellipsoid(V3(0.f, 0.0f, -0.24f), V3(0.07f, 0.01f, 0.09f), 4, 8));
	body.SetU(0.5f);
	Mesh m = body;
	// Wings: thin, swept, with ragged feather tips.
	for (int side = -1; side <= 1; side += 2)
	{
		const float sx = (float)side;
		const int segments = 8;
		const HRL_uint base = (HRL_uint)m.vertices.size();
		for (int i = 0; i <= segments; ++i)
		{
			const float t = (float)i / segments;
			const float x = sx * (0.05f + t * span * 0.5f);
			const float chord = 0.16f * (1.f - t * 0.55f) * (1.f + 0.25f * std::sin(t * 37.f));
			const float sweep = -t * t * 0.12f;
			const float u = 0.5f + sx * t * 0.5f;
			m.Add(V3(x, t * 0.03f, 0.06f + sweep), V3(0, 1, 0), u, 0.f, V3(1, 0, 0));
			m.Add(V3(x, t * 0.03f, 0.06f + sweep - chord), V3(0, 1, 0), u, 1.f, V3(1, 0, 0));
		}
		for (int i = 0; i < segments; ++i)
		{
			const HRL_uint a = base + (HRL_uint)i * 2;
			if (side > 0) { m.Tri(a, a + 2, a + 1); m.Tri(a + 1, a + 2, a + 3); }
			else { m.Tri(a, a + 1, a + 2); m.Tri(a + 1, a + 3, a + 2); }
		}
	}
	return m;
}

// A tuft of tall, thin, curved blades (two sided material).
inline Mesh GrassTuft(Rng& rng, int blades, float radius, float height)
{
	Mesh m;
	for (int b = 0; b < blades; ++b)
	{
		const float a = rng.Range(0.f, 2.f * kPi), r = std::sqrt(rng.Uniform()) * radius;
		const V3 root(std::cos(a) * r, -0.05f, std::sin(a) * r);
		const V3 lean = Normalize(V3(rng.Range(-1.f, 1.f), 0.f, rng.Range(-1.f, 1.f))) * rng.Range(0.1f, 0.45f);
		const V3 side = Normalize(Cross(V3(0, 1, 0), lean + V3(0.001f, 0, 0)));
		const float h = height * rng.Range(0.5f, 1.2f), w = rng.Range(0.012f, 0.03f);
		const int seg = 5;
		const HRL_uint base = (HRL_uint)m.vertices.size();
		for (int i = 0; i <= seg; ++i)
		{
			const float t = (float)i / seg;
			const V3 c = root + V3(0.f, h * t, 0.f) + lean * (t * t * h);
			const V3 n = Normalize(Cross(side, V3(0.f, 1.f, 0.f) + lean * (2.f * t)));
			m.Add(c - side * (w * (1.f - t)), n, 0.f, t, side);
			m.Add(c + side * (w * (1.f - t)), n, 1.f, t, side);
		}
		for (int i = 0; i < seg; ++i)
		{
			const HRL_uint a = base + (HRL_uint)i * 2;
			m.Tri(a, a + 1, a + 2);
			m.Tri(a + 1, a + 3, a + 2);
		}
	}
	return m;
}

// Dream catcher: ring, spiral web, beads and hanging feathers. Hangs from (0, 0, 0).
inline Mesh DreamCatcher(float radius)
{
	Mesh m;
	std::vector<V3> ring;
	std::vector<float> rr;
	for (int i = 0; i <= 48; ++i)
	{
		const float a = (float)i / 48.f * 2.f * kPi;
		ring.push_back(V3(std::cos(a) * radius, -0.25f - radius + std::sin(a) * radius, 0.f));
		rr.push_back(radius * 0.05f);
	}
	m.Append(Tube(ring, rr, 6, false, false));
	std::vector<V3> spiral;
	std::vector<float> sr;
	for (int i = 0; i <= 160; ++i)
	{
		const float t = (float)i / 160.f, a = t * 2.f * kPi * 4.5f, r = radius * (0.95f - 0.85f * t);
		spiral.push_back(V3(std::cos(a) * r, -0.25f - radius + std::sin(a) * r, 0.f));
		sr.push_back(radius * 0.012f);
	}
	m.Append(Tube(spiral, sr, 4));
	m.Append(Tube({V3(0, 0, 0), V3(0, -0.25f, 0)}, {0.006f, 0.006f}, 4));
	for (int f = -1; f <= 1; ++f)
	{
		const V3 top(f * radius * 0.6f, -0.25f - radius - std::sqrt(std::max(0.f, radius * radius - (f * radius * 0.6f) * (f * radius * 0.6f))), 0.f);
		const float len = 0.18f + 0.06f * (f == 0);
		m.Append(Tube({top, top + V3(0, -len, 0)}, {0.004f, 0.004f}, 4));
		m.Append(Ellipsoid(top + V3(0, -len * 0.45f, 0), V3(0.012f, 0.012f, 0.012f), 5, 6));
		m.Append(Ellipsoid(top + V3(0, -len - 0.08f, 0), V3(0.022f, 0.09f, 0.006f), 6, 8));
	}
	return m;
}
} // namespace geo
