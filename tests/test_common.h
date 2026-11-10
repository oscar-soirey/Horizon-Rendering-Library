// Helpers shared by the HRL tests: error counting, procedural meshes and
// in-memory PNG textures (no external resource is needed).
#ifndef HRL_TEST_COMMON_H
#define HRL_TEST_COMMON_H

#include <hrl.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
int g_errors = 0;
int g_failures = 0;

#define CHECK(cond) do { if (!(cond)) { ++g_failures; std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond); } } while (0)

// Errors, and Vulkan validation messages of any severity, fail the test.
void OnError(HRL_EError code, HRL_ESeverity severity, const char* detail)
{
	const std::string text = detail ? detail : "";
	if (severity == HRL_SEVERITY_ERROR || severity == HRL_SEVERITY_FATAL ||
		text.find("Vulkan validation") != std::string::npos)
	{
		++g_errors;
		std::fprintf(stderr, "[HRL %s] %s: %.600s\n", HRL_SeverityEnumToString(severity), HRL_ErrorEnumToString(code), text.c_str());
	}
}

// ---- Minimal PNG writer (stored deflate blocks) for in-memory test textures --
uint32_t Crc32(const unsigned char* data, size_t size, uint32_t crc = 0)
{
	crc = ~crc;
	for (size_t i = 0; i < size; ++i)
	{
		crc ^= data[i];
		for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

void PutU32(std::vector<unsigned char>& out, uint32_t v)
{
	for (int s = 24; s >= 0; s -= 8) out.push_back((unsigned char)(v >> s));
}

void PutChunk(std::vector<unsigned char>& out, const char* type, const std::vector<unsigned char>& data)
{
	PutU32(out, (uint32_t)data.size());
	std::vector<unsigned char> body(type, type + 4);
	body.insert(body.end(), data.begin(), data.end());
	out.insert(out.end(), body.begin(), body.end());
	PutU32(out, Crc32(body.data(), body.size()));
}

std::vector<unsigned char> EncodePNG(int width, int height, const std::vector<unsigned char>& rgba)
{
	std::vector<unsigned char> raw;
	for (int y = 0; y < height; ++y)
	{
		raw.push_back(0);
		raw.insert(raw.end(), rgba.begin() + (size_t)y * width * 4, rgba.begin() + (size_t)(y + 1) * width * 4);
	}
	std::vector<unsigned char> z = {0x78, 0x01};
	uint32_t a = 1, b = 0;
	for (size_t pos = 0; pos < raw.size();)
	{
		const size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
		z.push_back(pos + n == raw.size() ? 1 : 0);
		z.push_back((unsigned char)(n & 0xFF)); z.push_back((unsigned char)(n >> 8));
		z.push_back((unsigned char)(~n & 0xFF)); z.push_back((unsigned char)((~n >> 8) & 0xFF));
		for (size_t i = 0; i < n; ++i) { a = (a + raw[pos + i]) % 65521u; b = (b + a) % 65521u; }
		z.insert(z.end(), raw.begin() + (long)pos, raw.begin() + (long)(pos + n));
		pos += n;
	}
	PutU32(z, (b << 16) | a);

	std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	std::vector<unsigned char> ihdr;
	PutU32(ihdr, (uint32_t)width);
	PutU32(ihdr, (uint32_t)height);
	ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
	PutChunk(png, "IHDR", ihdr);
	PutChunk(png, "IDAT", z);
	PutChunk(png, "IEND", {});
	return png;
}

HRL_id MakeCheckerTexture(int size, unsigned char r, unsigned char g, unsigned char b)
{
	std::vector<unsigned char> px((size_t)size * size * 4);
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x)
		{
			const bool on = ((x / 8) + (y / 8)) % 2 == 0;
			unsigned char* p = &px[((size_t)y * size + x) * 4];
			p[0] = on ? r : 40; p[1] = on ? g : 40; p[2] = on ? b : 40; p[3] = 255;
		}
	const std::vector<unsigned char> png = EncodePNG(size, size, px);
	return HRL_CreateTexture(reinterpret_cast<const char*>(png.data()), png.size());
}

// ---- Geometry ---------------------------------------------------------------
struct MeshData { std::vector<HRL_Vertex3D> v; std::vector<HRL_uint> i; };

MeshData MakeSphere(float radius, int rings, int sectors)
{
	const float pi = 3.14159265f;
	MeshData m;
	for (int r = 0; r <= rings; ++r)
		for (int s = 0; s <= sectors; ++s)
		{
			const float theta = pi * (float)r / (float)rings, phi = 2.f * pi * (float)s / (float)sectors;
			HRL_Vertex3D v{};
			v.normal[0] = std::sin(theta) * std::cos(phi); v.normal[1] = std::cos(theta); v.normal[2] = std::sin(theta) * std::sin(phi);
			for (int k = 0; k < 3; ++k) v.position[k] = v.normal[k] * radius;
			v.uv[0] = (float)s / (float)sectors; v.uv[1] = 1.f - (float)r / (float)rings;
			v.tangent[0] = -std::sin(phi); v.tangent[2] = std::cos(phi);
			v.bitangent[0] = v.normal[1] * v.tangent[2] - v.normal[2] * v.tangent[1];
			v.bitangent[1] = v.normal[2] * v.tangent[0] - v.normal[0] * v.tangent[2];
			v.bitangent[2] = v.normal[0] * v.tangent[1] - v.normal[1] * v.tangent[0];
			m.v.push_back(v);
		}
	for (int r = 0; r < rings; ++r)
		for (int s = 0; s < sectors; ++s)
		{
			const HRL_uint a = (HRL_uint)(r * (sectors + 1) + s), b = a + (HRL_uint)sectors + 1;
			m.i.insert(m.i.end(), {a, a + 1, b, a + 1, b + 1, b});
		}
	return m;
}

MeshData MakeGround(float half)
{
	MeshData m;
	const float p[4][2] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
	for (int k = 0; k < 4; ++k)
	{
		HRL_Vertex3D v{};
		v.position[0] = p[k][0]; v.position[2] = p[k][1];
		v.normal[1] = 1.f; v.tangent[0] = 1.f; v.bitangent[2] = -1.f;
		v.uv[0] = p[k][0] / half; v.uv[1] = p[k][1] / half;
		m.v.push_back(v);
	}
	m.i = {0, 2, 1, 0, 3, 2}; // counter-clockwise seen from +Y
	return m;
}

HRL_id AddMesh(HRL_id scene, const MeshData& d, HRL_id material, float x, float y, float z)
{
	const HRL_id mesh = HRL_CreateMesh3D(scene, d.v.data(), d.v.size(), d.i.data(), d.i.size());
	HRL_SetMeshMaterial(mesh, material);
	HRL_SetMeshLocation(mesh, x, y, z);
	return mesh;
}

void Frames(int count)
{
	for (int i = 0; i < count; ++i) { HRL_BeginFrame(); HRL_EndFrame(); }
}
} // namespace

#endif
