// HRL extras - tileable 3D noise for volumes, baked on the CPU into a 2D atlas.
//
// 64 x 64 x 64 texels, period 1 in every direction (the noise tiles), stored
// as 8 x 8 slices of 66 x 66 texels (one texel of wrapped border around each
// slice, so bilinear filtering inside a slice wraps exactly). GLSL side:
// hv_Noise() in heaven_shaders.hpp.
//   R : Perlin-Worley (billowy base shape of smoke and clouds)
//   G : Worley fbm, frequency 2x    \  erosion details
//   B : Worley fbm, frequency 4x    /
//   A : Perlin fbm (soft fog)
// Header-only, depends on nothing but the C++ standard library.
#ifndef HRL_EXTRAS_HEAVEN_NOISE_HPP
#define HRL_EXTRAS_HEAVEN_NOISE_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hrl::heaven
{
constexpr int kNoiseSize = 64;               // texels per axis
constexpr int kNoiseTile = kNoiseSize + 2;   // with the wrapped border
constexpr int kNoiseGrid = 8;                // slices per row of the atlas
constexpr int kNoiseAtlas = kNoiseTile * kNoiseGrid; // 528

namespace detail
{
inline uint32_t Hash(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}
inline uint32_t Hash3(int x, int y, int z, uint32_t seed)
{
	return Hash((uint32_t)x * 73856093u ^ Hash((uint32_t)y * 19349663u ^ Hash((uint32_t)z * 83492791u ^ seed)));
}
inline float Unit(uint32_t h) { return (float)(h & 0xFFFFFFu) / 16777215.f; }
inline int Wrap(int v, int period) { return ((v % period) + period) % period; }
inline float Fade(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

// Gradient noise of period `period` cells, about -1..1.
inline float Perlin(float x, float y, float z, int period, uint32_t seed)
{
	const int x0 = (int)std::floor(x), y0 = (int)std::floor(y), z0 = (int)std::floor(z);
	const float fx = x - (float)x0, fy = y - (float)y0, fz = z - (float)z0;
	auto grad = [&](int ix, int iy, int iz, float dx, float dy, float dz)
	{
		const uint32_t h = Hash3(Wrap(ix, period), Wrap(iy, period), Wrap(iz, period), seed);
		// 12 edge directions of a cube.
		static const float g[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
			{1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
		const float* d = g[h % 12u];
		return d[0] * dx + d[1] * dy + d[2] * dz;
	};
	const float u = Fade(fx), v = Fade(fy), w = Fade(fz);
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	const float x00 = lerp(grad(x0, y0, z0, fx, fy, fz), grad(x0 + 1, y0, z0, fx - 1, fy, fz), u);
	const float x10 = lerp(grad(x0, y0 + 1, z0, fx, fy - 1, fz), grad(x0 + 1, y0 + 1, z0, fx - 1, fy - 1, fz), u);
	const float x01 = lerp(grad(x0, y0, z0 + 1, fx, fy, fz - 1), grad(x0 + 1, y0, z0 + 1, fx - 1, fy, fz - 1), u);
	const float x11 = lerp(grad(x0, y0 + 1, z0 + 1, fx, fy - 1, fz - 1), grad(x0 + 1, y0 + 1, z0 + 1, fx - 1, fy - 1, fz - 1), u);
	return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

// Distance to the nearest feature point (one per cell, `period` cells), 0..~1.
inline float Worley(float x, float y, float z, int period, uint32_t seed)
{
	const int cx = (int)std::floor(x), cy = (int)std::floor(y), cz = (int)std::floor(z);
	float best = 8.f;
	for (int k = -1; k <= 1; ++k)
		for (int j = -1; j <= 1; ++j)
			for (int i = -1; i <= 1; ++i)
			{
				const int ix = cx + i, iy = cy + j, iz = cz + k;
				const uint32_t h = Hash3(Wrap(ix, period), Wrap(iy, period), Wrap(iz, period), seed);
				const float px = (float)ix + Unit(h), py = (float)iy + Unit(Hash(h)), pz = (float)iz + Unit(Hash(h ^ 0x9e3779b9u));
				const float dx = px - x, dy = py - y, dz = pz - z;
				best = std::min(best, dx * dx + dy * dy + dz * dz);
			}
	return std::min(std::sqrt(best), 1.f);
}

// Fractal sums, `p` in 0..1 (one period).
inline float PerlinFbm(float x, float y, float z, int frequency, int octaves, uint32_t seed)
{
	float sum = 0.f, amp = 1.f, norm = 0.f;
	for (int o = 0; o < octaves; ++o)
	{
		const int f = frequency << o;
		sum += amp * Perlin(x * (float)f, y * (float)f, z * (float)f, f, seed + (uint32_t)o * 131u);
		norm += amp;
		amp *= 0.5f;
	}
	return sum / norm; // about -1..1
}
inline float WorleyFbm(float x, float y, float z, int frequency, uint32_t seed)
{
	// Inverted Worley, 3 octaves (0.625, 0.25, 0.125): round billows.
	float sum = 0.f;
	const float weights[3] = {0.625f, 0.25f, 0.125f};
	for (int o = 0; o < 3; ++o)
	{
		const int f = frequency << o;
		sum += weights[o] * (1.f - Worley(x * (float)f, y * (float)f, z * (float)f, f, seed + (uint32_t)o * 977u));
	}
	return sum;
}
inline float Remap(float v, float lo, float hi, float a, float b)
{
	return a + (v - lo) / std::max(hi - lo, 1e-6f) * (b - a);
}
inline unsigned char Byte(float v) { return (unsigned char)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); }
} // namespace detail

// RGBA8 atlas of kNoiseAtlas x kNoiseAtlas texels (see the top of the file).
inline std::vector<unsigned char> BakeNoiseAtlas(uint32_t seed = 1337u)
{
	using namespace detail;
	const int n = kNoiseSize;
	std::vector<float> values((size_t)n * n * n * 4);
	for (int z = 0; z < n; ++z)
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				const float px = ((float)x + 0.5f) / (float)n, py = ((float)y + 0.5f) / (float)n, pz = ((float)z + 0.5f) / (float)n;
				const float perlin = PerlinFbm(px, py, pz, 4, 4, seed) * 0.5f + 0.5f;
				const float worley = WorleyFbm(px, py, pz, 4, seed + 7u);
				// Perlin-Worley: Perlin dilated by the Worley billows (Schneider, Horizon Zero Dawn).
				const float pw = std::clamp(Remap(perlin, worley - 1.f, 1.f, 0.f, 1.f), 0.f, 1.f);
				const float w2 = WorleyFbm(px, py, pz, 8, seed + 31u);
				const float w4 = WorleyFbm(px, py, pz, 16, seed + 63u);
				const float fog = PerlinFbm(px, py, pz, 2, 5, seed + 101u) * 0.5f + 0.5f;
				float* t = &values[(((size_t)z * n + y) * n + x) * 4];
				t[0] = pw;
				t[1] = w2;
				t[2] = w4;
				t[3] = fog;
			}
	// Every channel stretched over 0..1: the shapes erode the whole range.
	float lo[4] = {1e9f, 1e9f, 1e9f, 1e9f}, hi[4] = {-1e9f, -1e9f, -1e9f, -1e9f};
	for (size_t i = 0; i < values.size(); ++i)
	{
		lo[i & 3] = std::min(lo[i & 3], values[i]);
		hi[i & 3] = std::max(hi[i & 3], values[i]);
	}
	std::vector<unsigned char> volume(values.size());
	for (size_t i = 0; i < values.size(); ++i)
		volume[i] = Byte((values[i] - lo[i & 3]) / std::max(hi[i & 3] - lo[i & 3], 1e-6f));
	std::vector<unsigned char> atlas((size_t)kNoiseAtlas * kNoiseAtlas * 4);
	for (int z = 0; z < n; ++z)
	{
		const int ox = (z % kNoiseGrid) * kNoiseTile, oy = (z / kNoiseGrid) * kNoiseTile;
		for (int y = 0; y < kNoiseTile; ++y)
			for (int x = 0; x < kNoiseTile; ++x)
			{
				const int sx = Wrap(x - 1, n), sy = Wrap(y - 1, n);
				const unsigned char* s = &volume[(((size_t)z * n + sy) * n + sx) * 4];
				unsigned char* d = &atlas[((size_t)(oy + y) * kNoiseAtlas + (ox + x)) * 4];
				d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
			}
	}
	return atlas;
}
} // namespace hrl::heaven

#endif
