#include "gi.h"
#include "gl33_texture.h"
#include "gl33_renderer.h"
#include "../../core/utils_functions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <glm/gtc/matrix_transform.hpp>

extern HRL_Context* GetPrivateContext();

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInvPi = 1.0f / kPi;
constexpr float kRayEpsilon = 0.003f;
constexpr float kProbeMargin = 0.75f;
constexpr int kProbeNX = 8;
constexpr int kProbeNY = 6;
constexpr int kProbeNZ = 8;
constexpr int kRaysPerProbe = 12;
constexpr int kProbeBudgetPerFrame = 8;
constexpr int kBVHLeafSize = 4;
constexpr float kIndirectBounceWeight = 0.28f;
constexpr float kGIShaderStrength = 1.5f;
constexpr int kMaxTriangleCount = 200000;

const glm::vec3 kProbeAxes[6] = {
    glm::vec3( 1.f, 0.f, 0.f),
    glm::vec3(-1.f, 0.f, 0.f),
    glm::vec3( 0.f, 1.f, 0.f),
    glm::vec3( 0.f,-1.f, 0.f),
    glm::vec3( 0.f, 0.f, 1.f),
    glm::vec3( 0.f, 0.f,-1.f),
};

static glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback)
{
    const float l2 = glm::dot(v, v);
    return std::isfinite(l2) && l2 > 1e-10f ? v * (1.0f / std::sqrt(l2)) : fallback;
}

static glm::vec3 FibonacciSphereDirection(int i, int count)
{
    const float goldenAngle = 2.39996322972865332f;
    const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(count);
    const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    const float angle = goldenAngle * float(i);
    return SafeNormalize(
        glm::vec3(std::cos(angle) * r, y, std::sin(angle) * r),
        glm::vec3(0.f, 1.f, 0.f));
}

static glm::vec3 RotateAroundY(const glm::vec3& v, float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return glm::vec3(c * v.x - s * v.z, v.y, s * v.x + c * v.z);
}

static bool RayAabb(
    const glm::vec3& origin,
    const glm::vec3& direction,
    const glm::vec3& bmin,
    const glm::vec3& bmax,
    float maxDistance)
{
    float tmin = 0.f;
    float tmax = maxDistance;

    for (int axis = 0; axis < 3; ++axis)
    {
        const float o = origin[axis];
        const float d = direction[axis];
        if (std::abs(d) < 1e-8f)
        {
            if (o < bmin[axis] || o > bmax[axis])
                return false;
            continue;
        }

        const float invD = 1.f / d;
        float t0 = (bmin[axis] - o) * invD;
        float t1 = (bmax[axis] - o) * invD;
        if (t0 > t1)
            std::swap(t0, t1);
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmax < tmin)
            return false;
    }
    return tmax >= 0.f && tmin <= maxDistance;
}

static bool RayTriangle(
    const glm::vec3& origin,
    const glm::vec3& direction,
    const GL_33_GI::Triangle& tri,
    float maxDistance,
    float& outT,
    float& outU,
    float& outV)
{
    const glm::vec3 edge1 = tri.p1 - tri.p0;
    const glm::vec3 edge2 = tri.p2 - tri.p0;
    const glm::vec3 pvec = glm::cross(direction, edge2);
    const float det = glm::dot(edge1, pvec);
    if (std::abs(det) < 1e-8f)
        return false;

    const float invDet = 1.f / det;
    const glm::vec3 tvec = origin - tri.p0;
    const float u = glm::dot(tvec, pvec) * invDet;
    if (u < 0.f || u > 1.f)
        return false;

    const glm::vec3 qvec = glm::cross(tvec, edge1);
    const float v = glm::dot(direction, qvec) * invDet;
    if (v < 0.f || u + v > 1.f)
        return false;

    const float t = glm::dot(edge2, qvec) * invDet;
    if (t <= kRayEpsilon || t > maxDistance)
        return false;
    outT = t;
    outU = u;
    outV = v;
    return true;
}

static size_t ProbeLinearIndex(int x, int y, int z, int nx, int ny)
{
    return (size_t(z) * size_t(ny) + size_t(y)) * size_t(nx) + size_t(x);
}


} // namespace

GL_33_GI::~GL_33_GI()
{
    Shutdown();
}

void GL_33_GI::Initialize(GLuint fullscreenVao)
{
    fullscreenVao_ = fullscreenVao;
}

void GL_33_GI::Shutdown()
{
    for (auto& [sceneId, resources] : scenes_)
    {
        (void)sceneId;
        glDeleteBuffers(3, resources.probeUbo);
        resources.probeUbo[0] = resources.probeUbo[1] = resources.probeUbo[2] = 0;
    }
    scenes_.clear();
    fullscreenVao_ = 0;
    lastShaderProgram_ = 0;
    lastSceneId_ = HRL_INVALID_ID;
    lastUploadSerial_ = 0;
    lastEnabled_ = false;
}

bool GL_33_GI::Supports(HRL_EGlobalIlluminationMethod method) const
{
    return method == HRL_GI_DDGI;
}

uint32_t GL_33_GI::GetSupportedMethods() const
{
    return 1u << (unsigned)HRL_GI_DDGI;
}

void GL_33_GI::ReleaseScene(HRL_id sceneId)
{
    auto it = scenes_.find(sceneId);
    if (it == scenes_.end())
        return;
    glDeleteBuffers(3, it->second.probeUbo);
    scenes_.erase(it);
    if (lastSceneId_ == sceneId)
    {
        lastShaderProgram_ = 0;
        lastSceneId_ = HRL_INVALID_ID;
        lastUploadSerial_ = 0;
        lastEnabled_ = false;
    }
}

glm::mat4 GL_33_GI::CalculateModelMatrix(const HRL_Mesh* mesh) const
{
    glm::mat4 model(1.f);
    if (!mesh)
        return model;
    model = glm::translate(model, mesh->position_);
    model = glm::translate(model, mesh->pivot_point_);
    model = glm::rotate(model, glm::radians(mesh->rotation_.x), glm::vec3(1.f, 0.f, 0.f));
    model = glm::rotate(model, glm::radians(mesh->rotation_.y), glm::vec3(0.f, 1.f, 0.f));
    model = glm::rotate(model, glm::radians(mesh->rotation_.z), glm::vec3(0.f, 0.f, 1.f));
    model = glm::translate(model, -mesh->pivot_point_);
    model = glm::scale(model, mesh->scale_);
    return model;
}

glm::vec3 GL_33_GI::GetMaterialTint(HRL_id materialId) const
{
    HRL_Context* context = GetPrivateContext();
    if (!context)
        return glm::vec3(1.f);
    auto it = context->materials.find(materialId);
    if (it == context->materials.end() || !it->second)
        return glm::vec3(1.f);
    const auto* material = it->second;
    auto tintIt = material->vec3Params_.find("TintColor");
    if (tintIt != material->vec3Params_.end())
        return glm::clamp(tintIt->second, glm::vec3(0.f), glm::vec3(4.f));
    return glm::vec3(1.f);
}

float GL_33_GI::SampleMaterialScalar(
    HRL_id materialId,
    const char* useValueName,
    const char* valueName,
    const char* textureName,
    const glm::vec2& uv,
    float fallback) const
{
    HRL_Context* context = GetPrivateContext();
    if (!context || !useValueName || !valueName || !textureName)
        return fallback;
    auto matIt = context->materials.find(materialId);
    if (matIt == context->materials.end() || !matIt->second)
        return fallback;
    const HRL_Material* material = matIt->second;

    bool useValue = false;
    auto useIt = material->intParams_.find(useValueName);
    if (useIt != material->intParams_.end())
        useValue = useIt->second != 0;

    auto valueIt = material->floatParams_.find(valueName);
    const float scalarValue = valueIt != material->floatParams_.end() ? valueIt->second : fallback;
    if (useValue)
        return std::clamp(scalarValue, 0.f, 1.f);

    auto texIt = material->textureParams_.find(textureName);
    if (texIt == material->textureParams_.end())
        return std::clamp(scalarValue, 0.f, 1.f);

    const GL33_Texture* texture = GL33_FindTexture(texIt->second);
    if (!texture || texture->GetWidth() == 0 || texture->GetHeight() == 0)
        return std::clamp(scalarValue, 0.f, 1.f);

    const auto& rgba = texture->GetCpuRGBA();
    const int width = static_cast<int>(texture->GetWidth());
    const int height = static_cast<int>(texture->GetHeight());
    if (rgba.size() != size_t(width) * size_t(height) * 4u || width <= 0 || height <= 0)
        return std::clamp(scalarValue, 0.f, 1.f);

    const float fx = uv.x - std::floor(uv.x);
    const float fy = uv.y - std::floor(uv.y);
    const float px = fx * float(width) - 0.5f;
    const float py = fy * float(height) - 0.5f;
    const int x0 = static_cast<int>(std::floor(px));
    const int y0 = static_cast<int>(std::floor(py));
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const float tx = px - float(x0);
    const float ty = py - float(y0);

    auto sample = [&](int x, int y) -> float
    {
        x = (x % width + width) % width;
        y = (y % height + height) % height;
        const size_t idx = (size_t(y) * size_t(width) + size_t(x)) * 4u;
        return float(rgba[idx]) / 255.f;
    };

    const float v00 = sample(x0, y0);
    const float v10 = sample(x1, y0);
    const float v01 = sample(x0, y1);
    const float v11 = sample(x1, y1);
    const float a = v00 + (v10 - v00) * tx;
    const float b = v01 + (v11 - v01) * tx;
    return std::clamp(a + (b - a) * ty, 0.f, 1.f);
}

glm::vec3 GL_33_GI::SampleMaterialAlbedo(HRL_id materialId, const glm::vec2& uv, const glm::vec3& fallback) const
{
    HRL_Context* context = GetPrivateContext();
    if (!context)
        return fallback;
    auto matIt = context->materials.find(materialId);
    if (matIt == context->materials.end() || !matIt->second)
        return fallback;

    const HRL_Material* material = matIt->second;
    const glm::vec3 tint = GetMaterialTint(materialId);
    auto texIt = material->textureParams_.find("T_Albedo");
    if (texIt == material->textureParams_.end() || texIt->second == HRL_INVALID_ID)
        return fallback * tint;

    const GL33_Texture* texture = GL33_FindTexture(texIt->second);
    if (!texture || texture->GetWidth() == 0 || texture->GetHeight() == 0)
        return fallback * tint;
    const auto& pixels = texture->GetCpuRGBA();
    if (pixels.size() != size_t(texture->GetWidth()) * size_t(texture->GetHeight()) * 4u)
        return fallback * tint;

    const float u = uv.x - std::floor(uv.x);
    const float v = uv.y - std::floor(uv.y);
    const float fx = u * float(texture->GetWidth() - 1);
    const float fy = v * float(texture->GetHeight() - 1);
    const int x0 = std::clamp((int)std::floor(fx), 0, (int)texture->GetWidth() - 1);
    const int y0 = std::clamp((int)std::floor(fy), 0, (int)texture->GetHeight() - 1);
    const int x1 = std::min(x0 + 1, (int)texture->GetWidth() - 1);
    const int y1 = std::min(y0 + 1, (int)texture->GetHeight() - 1);
    const float tx = fx - float(x0);
    const float ty = fy - float(y0);

    auto sample = [&](int x, int y) {
        const size_t base = (size_t(y) * size_t(texture->GetWidth()) + size_t(x)) * 4u;
        return glm::vec3(
            pixels[base + 0], pixels[base + 1], pixels[base + 2]) / 255.f;
    };

    const glm::vec3 c00 = sample(x0, y0);
    const glm::vec3 c10 = sample(x1, y0);
    const glm::vec3 c01 = sample(x0, y1);
    const glm::vec3 c11 = sample(x1, y1);
    const glm::vec3 row0 = glm::mix(c00, c10, tx);
    const glm::vec3 row1 = glm::mix(c01, c11, tx);
    return glm::clamp(glm::mix(row0, row1, ty) * tint, glm::vec3(0.f), glm::vec3(1.f));
}

bool GL_33_GI::EnsureGeometryCache(HRL_id sceneId, GeometryCache& geometry)
{
    HRL_Context* context = GetPrivateContext();
    if (!context)
        return false;
    auto sceneIt = context->scenes.find(sceneId);
    if (sceneIt == context->scenes.end() || !sceneIt->second)
        return false;
    const hrl_scene_t* scene = sceneIt->second;

    const uint64_t revision = scene->gi_geometry_revision;
    if (geometry.valid && geometry.revision == revision)
        return true;

    geometry.triangles.clear();
    geometry.indices.clear();
    geometry.nodes.clear();
    geometry.valid = false;

    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
    bool hasGeometry = false;
    geometry.triangles.reserve(8192);

    for (const auto& [meshId, mesh] : scene->meshes)
    {
        (void)meshId;
        if (!mesh || mesh->type_ != HRL_3D_MESH || mesh->lods_.empty())
            continue;
        const auto& lod = mesh->lods_[0];
        if (lod.vertices.empty())
            continue;

        const glm::mat4 model = CalculateModelMatrix(mesh);

        auto emitTriangle = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
            if (i0 >= lod.vertices.size() || i1 >= lod.vertices.size() || i2 >= lod.vertices.size())
                return;
            if ((int)geometry.triangles.size() >= kMaxTriangleCount)
                return;

            const glm::vec3 p0 = glm::vec3(model * glm::vec4(
                lod.vertices[i0].position[0], lod.vertices[i0].position[1], lod.vertices[i0].position[2], 1.f));
            const glm::vec3 p1 = glm::vec3(model * glm::vec4(
                lod.vertices[i1].position[0], lod.vertices[i1].position[1], lod.vertices[i1].position[2], 1.f));
            const glm::vec3 p2 = glm::vec3(model * glm::vec4(
                lod.vertices[i2].position[0], lod.vertices[i2].position[1], lod.vertices[i2].position[2], 1.f));

            glm::vec3 normal = glm::cross(p1 - p0, p2 - p0);
            const float area2 = glm::dot(normal, normal);
            if (area2 < 1e-10f)
                return;
            normal *= 1.f / std::sqrt(area2);

            Triangle tri;
            tri.p0 = p0;
            tri.p1 = p1;
            tri.p2 = p2;
            tri.normal = normal;
            tri.centroid = (p0 + p1 + p2) / 3.f;
            tri.material = mesh->material_;
            tri.uv0 = glm::vec2(lod.vertices[i0].uv[0], lod.vertices[i0].uv[1]);
            tri.uv1 = glm::vec2(lod.vertices[i1].uv[0], lod.vertices[i1].uv[1]);
            tri.uv2 = glm::vec2(lod.vertices[i2].uv[0], lod.vertices[i2].uv[1]);
            geometry.triangles.push_back(tri);

            minBounds = glm::min(minBounds, glm::min(p0, glm::min(p1, p2)));
            maxBounds = glm::max(maxBounds, glm::max(p0, glm::max(p1, p2)));
            hasGeometry = true;
        };

        if (!lod.indices.empty())
        {
            for (size_t i = 0; i + 2 < lod.indices.size() && (int)geometry.triangles.size() < kMaxTriangleCount; i += 3)
                emitTriangle(lod.indices[i], lod.indices[i + 1], lod.indices[i + 2]);
        }
        else
        {
            for (size_t i = 0; i + 2 < lod.vertices.size() && (int)geometry.triangles.size() < kMaxTriangleCount; i += 3)
                emitTriangle((uint32_t)i, (uint32_t)i + 1u, (uint32_t)i + 2u);
        }
    }

    if (!hasGeometry || geometry.triangles.empty())
        return false;

    geometry.boundsMin = minBounds;
    geometry.boundsMax = maxBounds;
    geometry.indices.resize(geometry.triangles.size());
    for (uint32_t i = 0; i < geometry.indices.size(); ++i)
        geometry.indices[i] = i;

    BuildBVH(geometry);
    geometry.revision = revision;
    geometry.valid = !geometry.nodes.empty();

    std::printf("[HRL][DDGI] geometry cache: %zu triangles, BVH nodes=%zu\n",
        geometry.triangles.size(), geometry.nodes.size());
    return geometry.valid;
}

void GL_33_GI::BuildBVH(GeometryCache& geometry)
{
    geometry.nodes.clear();
    if (geometry.indices.empty())
        return;
    geometry.nodes.reserve(geometry.indices.size() * 2u);
    BuildBVHNode(geometry, 0u, (uint32_t)geometry.indices.size());
}

uint32_t GL_33_GI::BuildBVHNode(GeometryCache& geometry, uint32_t first, uint32_t count)
{
    BVHNode node;
    node.first = first;
    node.count = count;

    glm::vec3 bmin(std::numeric_limits<float>::max());
    glm::vec3 bmax(std::numeric_limits<float>::lowest());
    glm::vec3 cmin(std::numeric_limits<float>::max());
    glm::vec3 cmax(std::numeric_limits<float>::lowest());

    for (uint32_t i = 0; i < count; ++i)
    {
        const Triangle& tri = geometry.triangles[geometry.indices[first + i]];
        const glm::vec3 triMin = glm::min(tri.p0, glm::min(tri.p1, tri.p2));
        const glm::vec3 triMax = glm::max(tri.p0, glm::max(tri.p1, tri.p2));
        const glm::vec3 centroid = tri.centroid;
        bmin = glm::min(bmin, triMin);
        bmax = glm::max(bmax, triMax);
        cmin = glm::min(cmin, centroid);
        cmax = glm::max(cmax, centroid);
    }

    const uint32_t nodeIndex = (uint32_t)geometry.nodes.size();
    geometry.nodes.push_back(node);
    geometry.nodes[nodeIndex].bmin = bmin;
    geometry.nodes[nodeIndex].bmax = bmax;

    if (count <= kBVHLeafSize)
        return nodeIndex;

    const glm::vec3 extent = cmax - cmin;
    int axis = 0;
    if (extent.y > extent.x) axis = 1;
    if (extent.z > extent[axis]) axis = 2;
    if (extent[axis] < 1e-6f)
        return nodeIndex;

    const uint32_t mid = first + count / 2u;
    std::nth_element(
        geometry.indices.begin() + first,
        geometry.indices.begin() + mid,
        geometry.indices.begin() + first + count,
        [&](uint32_t a, uint32_t b) {
            const Triangle& ta = geometry.triangles[a];
            const Triangle& tb = geometry.triangles[b];
            return ta.centroid[axis] < tb.centroid[axis];
        });

    const uint32_t left = BuildBVHNode(geometry, first, mid - first);
    const uint32_t right = BuildBVHNode(geometry, mid, first + count - mid);
    geometry.nodes[nodeIndex].left = left;
    geometry.nodes[nodeIndex].right = right;
    geometry.nodes[nodeIndex].first = 0;
    geometry.nodes[nodeIndex].count = 0;
    return nodeIndex;
}

bool GL_33_GI::TraceNearest(
    const GeometryCache& geometry,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float maxDistance,
    Hit& hit,
    int ignoredTriangle) const
{
    if (!geometry.valid || geometry.nodes.empty())
        return false;

    float closest = maxDistance;
    float closestU = 0.f;
    float closestV = 0.f;
    int triangleIndex = -1;
    uint32_t stack[96];
    int stackSize = 0;
    stack[stackSize++] = 0u;

    while (stackSize > 0)
    {
        const uint32_t nodeIndex = stack[--stackSize];
        const BVHNode& node = geometry.nodes[nodeIndex];
        if (!RayAabb(origin, direction, node.bmin, node.bmax, closest))
            continue;

        if (node.IsLeaf())
        {
            for (uint32_t i = 0; i < node.count; ++i)
            {
                const int triIndex = (int)geometry.indices[node.first + i];
                if (triIndex == ignoredTriangle)
                    continue;
                float t = 0.f, u = 0.f, v = 0.f;
                if (RayTriangle(origin, direction, geometry.triangles[triIndex], closest, t, u, v))
                {
                    closest = t;
                    closestU = u;
                    closestV = v;
                    triangleIndex = triIndex;
                }
            }
            continue;
        }

        if (node.left != UINT32_MAX && stackSize < 96)
            stack[stackSize++] = node.left;
        if (node.right != UINT32_MAX && stackSize < 96)
            stack[stackSize++] = node.right;
    }

    if (triangleIndex < 0)
        return false;

    const Triangle& tri = geometry.triangles[triangleIndex];
    hit.t = closest;
    hit.position = origin + direction * closest;
    hit.normal = tri.normal;
    hit.uv = tri.uv0 * (1.f - closestU - closestV) + tri.uv1 * closestU + tri.uv2 * closestV;
    hit.material = tri.material;
    hit.triangle = triangleIndex;
    return true;
}

glm::vec3 GL_33_GI::SampleSky(const hrl_scene_t* scene, const glm::vec3& direction) const
{
    if (!scene || !scene->sky_sphere_enabled)
        return glm::vec3(0.f);
    const float t = std::clamp(direction.y * 0.5f + 0.5f, 0.f, 1.f);
    if (t > 0.5f)
        return glm::mix(scene->sky_horizon_color, scene->sky_top_color, (t - 0.5f) * 2.f);
    return glm::mix(scene->sky_bottom_color, scene->sky_horizon_color, t * 2.f);
}

glm::vec3 GL_33_GI::SampleCpuIrradiance(
    const ProbeVolume& probes,
    const glm::vec3& position,
    const glm::vec3& normal) const
{
    if (!probes.initialized || probes.nx <= 0 || probes.ny <= 0 || probes.nz <= 0)
        return glm::vec3(0.f);

    glm::vec3 grid = (position - probes.origin) / glm::max(probes.spacing, glm::vec3(1e-4f));
    if (grid.x < 0.f || grid.y < 0.f || grid.z < 0.f ||
        grid.x > float(probes.nx - 1) || grid.y > float(probes.ny - 1) || grid.z > float(probes.nz - 1))
        return glm::vec3(0.f);

    const glm::vec3 base = glm::floor(grid);
    const glm::vec3 f = grid - base;
    const int x0 = std::clamp((int)base.x, 0, probes.nx - 1);
    const int y0 = std::clamp((int)base.y, 0, probes.ny - 1);
    const int z0 = std::clamp((int)base.z, 0, probes.nz - 1);
    const int x1 = std::min(x0 + 1, probes.nx - 1);
    const int y1 = std::min(y0 + 1, probes.ny - 1);
    const int z1 = std::min(z0 + 1, probes.nz - 1);

    const int xs[2] = {x0, x1};
    const int ys[2] = {y0, y1};
    const int zs[2] = {z0, z1};
    const float wx[2] = {1.f - f.x, f.x};
    const float wy[2] = {1.f - f.y, f.y};
    const float wz[2] = {1.f - f.z, f.z};

    const glm::vec3 n = SafeNormalize(normal, glm::vec3(0.f, 1.f, 0.f));
    glm::vec3 result(0.f);
    float totalWeight = 0.f;

    for (int ix = 0; ix < 2; ++ix)
        for (int iy = 0; iy < 2; ++iy)
            for (int iz = 0; iz < 2; ++iz)
            {
                const size_t index = ProbeLinearIndex(xs[ix], ys[iy], zs[iz], probes.nx, probes.ny);
                if (!probes.valid[index])
                    continue;
                const float spatial = wx[ix] * wy[iy] * wz[iz];
                if (spatial <= 0.f)
                    continue;

                glm::vec3 local(0.f);
                float directionalWeight = 0.f;
                for (int d = 0; d < 6; ++d)
                {
                    const float w = std::max(glm::dot(n, kProbeAxes[d]), 0.f);
                    local += probes.radiance[d][index] * w;
                    directionalWeight += w;
                }
                if (directionalWeight <= 1e-5f)
                    continue;

                local /= directionalWeight;
                result += local * spatial;
                totalWeight += spatial;
            }

    return totalWeight > 1e-5f ? result / totalWeight : glm::vec3(0.f);
}

glm::vec3 GL_33_GI::EvaluateHitRadiance(
    HRL_id sceneId,
    const SceneResources& resources,
    const Hit& hit) const
{
    HRL_Context* context = GetPrivateContext();
    if (!context)
        return glm::vec3(0.f);
    auto sceneIt = context->scenes.find(sceneId);
    if (sceneIt == context->scenes.end() || !sceneIt->second)
        return glm::vec3(0.f);
    const hrl_scene_t* scene = sceneIt->second;

    const glm::vec3 N = SafeNormalize(hit.normal, glm::vec3(0.f, 1.f, 0.f));
    const glm::vec3 albedo = SampleMaterialAlbedo(hit.material, hit.uv, glm::vec3(1.f));
    const float metallic = SampleMaterialScalar(
        hit.material, "MetallicUseValue", "MetallicValue", "T_Metallic", hit.uv, 0.f);
    const glm::vec3 diffuseAlbedo = albedo * (1.f - metallic);
    glm::vec3 directIrradiance(0.f);

    // Cast a secondary BVH ray for shadow-casting lights so direct probe
    // irradiance does not leak through occluders.
    for (const auto& [lightId, light] : scene->lights)
    {
        (void)lightId;
        if (!light || light->intensity_ <= 0.f)
            continue;

        glm::vec3 lightDir(0.f);
        float attenuation = 1.f;
        float lightDistance = std::numeric_limits<float>::infinity();

        if (light->type_ == HRL_POINT_LIGHT || light->type_ == HRL_SPOT_LIGHT)
        {
            const glm::vec3 toLight = light->position_ - hit.position;
            lightDistance = glm::length(toLight);
            if (lightDistance <= 1e-4f)
                continue;
            lightDir = toLight / lightDistance;
            attenuation = 1.f / (1.f + std::max(0.f, light->attenuation_) * lightDistance * lightDistance);

            if (light->type_ == HRL_SPOT_LIGHT)
            {
                const float pitch = glm::radians(light->rotation_.x);
                const float yaw = glm::radians(light->rotation_.y);
                const glm::vec3 spotForward = SafeNormalize(glm::vec3(
                    std::cos(yaw) * std::cos(pitch),
                    std::sin(pitch),
                    std::sin(yaw) * std::cos(pitch)),
                    glm::vec3(0.f, -1.f, 0.f));
                const float cosTheta = glm::dot(lightDir, -spotForward);
                const float inner = std::cos(glm::radians(light->innerCutoff));
                const float outer = std::cos(glm::radians(light->outerCutoff));
                attenuation *= std::clamp(
                    (cosTheta - outer) / std::max(inner - outer, 1e-4f), 0.f, 1.f);
            }
        }
        else if (light->type_ == HRL_DIRECTIONAL_LIGHT)
        {
            lightDir = SafeNormalize(-light->rotation_, glm::vec3(0.f, 1.f, 0.f));
        }
        else
        {
            continue;
        }

        const float nDotL = std::max(glm::dot(N, lightDir), 0.f);
        if (nDotL <= 0.f || attenuation <= 0.f)
            continue;

        if (light->cast_shadows_)
        {
            const float maxShadowDistance = (light->type_ == HRL_POINT_LIGHT || light->type_ == HRL_SPOT_LIGHT)
                ? std::max(lightDistance - std::max(kRayEpsilon, light->shadow_bias_), kRayEpsilon)
                : std::max(glm::length(resources.probes.extent) * 2.f, 10.f);
            Hit shadowHit;
            if (TraceNearest(resources.geometry,
                hit.position + N * std::max(kRayEpsilon, light->shadow_bias_),
                lightDir, maxShadowDistance, shadowHit, hit.triangle))
                continue;
        }

        directIrradiance += light->color_ * light->intensity_ * attenuation * nDotL;
    }

    const glm::vec3 directRadiance = directIrradiance * diffuseAlbedo * kInvPi;
    const glm::vec3 previousIndirect = SampleCpuIrradiance(
        resources.probes,
        hit.position + N * kRayEpsilon * 4.f,
        N);

    return glm::max(directRadiance + previousIndirect * kIndirectBounceWeight, glm::vec3(0.f));
}

bool GL_33_GI::BuildOrUpdateProbeScene(ProbeVolume& probes, const GeometryCache& geometry)
{
    if (probes.initialized && probes.geometryRevision == geometry.revision)
        return true;

    glm::vec3 extent = glm::max(geometry.boundsMax - geometry.boundsMin, glm::vec3(2.f));
    const float longest = std::max(extent.x, std::max(extent.y, extent.z));
    const float marginAmount = std::max(kProbeMargin, longest * 0.03f);
    const glm::vec3 margin(marginAmount);
    const glm::vec3 boundsMin = geometry.boundsMin - margin;
    const glm::vec3 boundsMax = geometry.boundsMax + margin;
    extent = glm::max(boundsMax - boundsMin, glm::vec3(2.f));

    probes = {};
    probes.nx = kProbeNX;
    probes.ny = kProbeNY;
    probes.nz = kProbeNZ;
    probes.origin = boundsMin;
    probes.extent = extent;
    probes.spacing = glm::vec3(
        extent.x / float(std::max(1, probes.nx - 1)),
        extent.y / float(std::max(1, probes.ny - 1)),
        extent.z / float(std::max(1, probes.nz - 1)));

    const size_t count = size_t(probes.nx) * size_t(probes.ny) * size_t(probes.nz);
    for (int d = 0; d < 6; ++d)
        probes.radiance[d].assign(count, glm::vec3(0.f));
    probes.valid.assign(count, 1u);
    probes.historyValid.assign(count, 0u);
    probes.updateOrder.resize(count);
    for (size_t i = 0; i < count; ++i)
        probes.updateOrder[i] = (uint32_t)i;
    probes.updateCursor = 0;
    probes.geometryRevision = geometry.revision;
    probes.lightingRevision = 0;
    probes.initialized = true;
    probes.dirty = true;
    probes.converged = false;

    std::printf("[HRL][DDGI] rebuilt probe volume: %dx%dx%d (%zu probes, %zu triangles)\n",
        probes.nx, probes.ny, probes.nz, count, geometry.triangles.size());
    return true;
}

bool GL_33_GI::UpdateProbeBatch(HRL_id sceneId, SceneResources& resources, int budget)
{
    HRL_Context* context = GetPrivateContext();
    if (!context)
        return false;
    auto sceneIt = context->scenes.find(sceneId);
    if (sceneIt == context->scenes.end() || !sceneIt->second || !resources.geometry.valid)
        return false;
    const hrl_scene_t* scene = sceneIt->second;

    bool initialPass = resources.probes.dirty;
    if (resources.probes.lightingRevision != scene->gi_lighting_revision)
    {
        resources.probes.lightingRevision = scene->gi_lighting_revision;
        resources.probes.updateCursor = 0;
        resources.probes.dirty = true;
        resources.probes.converged = false;
        std::fill(resources.probes.historyValid.begin(), resources.probes.historyValid.end(), uint8_t(0));
        for (int d = 0; d < 6; ++d)
            std::fill(resources.probes.radiance[d].begin(), resources.probes.radiance[d].end(), glm::vec3(0.f));
        initialPass = true;
        std::printf("[HRL][DDGI] lighting revision changed; refreshing probe cache\n");
    }

    if (resources.probes.updateOrder.empty())
        return false;

    if (!resources.probes.dirty && resources.probes.converged && resources.probes.updateCursor >= resources.probes.updateOrder.size())
        resources.probes.updateCursor = 0;

    if (!resources.probes.dirty && resources.probes.converged)
        budget = std::min(budget, 4); // cheap temporal refinement once the volume is stable

    const float sceneDiagonal = glm::length(resources.probes.extent);
    const float rayLength = std::max(sceneDiagonal * 2.f, 10.f);
    int updated = 0;
    while (updated < budget && resources.probes.updateCursor < resources.probes.updateOrder.size())
    {
        const size_t probeIndex = resources.probes.updateOrder[resources.probes.updateCursor++];
        const int x = int(probeIndex % size_t(resources.probes.nx));
        const size_t yz = probeIndex / size_t(resources.probes.nx);
        const int y = int(yz % size_t(resources.probes.ny));
        const int z = int(yz / size_t(resources.probes.ny));

        const glm::vec3 position = resources.probes.origin +
            resources.probes.spacing * glm::vec3(float(x), float(y), float(z));

        glm::vec3 sums[6] = { glm::vec3(0.f), glm::vec3(0.f), glm::vec3(0.f),
                              glm::vec3(0.f), glm::vec3(0.f), glm::vec3(0.f) };
        float weights[6] = {0.f,0.f,0.f,0.f,0.f,0.f};
        int hitCount = 0;
        float closestSurface = std::numeric_limits<float>::max();

        for (int rayIndex = 0; rayIndex < kRaysPerProbe; ++rayIndex)
        {
            glm::vec3 direction = FibonacciSphereDirection(rayIndex, kRaysPerProbe);
            const float phase = 0.37f * float((probeIndex * 17u + resources.frameIndex * 13u) & 1023u) / 1024.f;
            direction = SafeNormalize(RotateAroundY(direction, phase), direction);

            Hit hit;
            glm::vec3 radiance(0.f);
            if (TraceNearest(resources.geometry,
                position + direction * kRayEpsilon * 2.f,
                direction, rayLength, hit))
            {
                ++hitCount;
                closestSurface = std::min(closestSurface, hit.t);
                radiance = EvaluateHitRadiance(sceneId, resources, hit);
            }
            else
            {
                radiance = SampleSky(scene, direction);
            }

            for (int d = 0; d < 6; ++d)
            {
                const float cosine = std::max(glm::dot(direction, kProbeAxes[d]), 0.f);
                const float w = cosine * cosine;
                sums[d] += radiance * w;
                weights[d] += w;
            }
        }

        const float minSpacing = std::min(resources.probes.spacing.x,
            std::min(resources.probes.spacing.y, resources.probes.spacing.z));
        const bool probablyInside =
            hitCount >= int(float(kRaysPerProbe) * 0.75f) &&
            closestSurface < minSpacing * 0.30f;
        resources.probes.valid[probeIndex] = probablyInside ? 0u : 1u;

        if (!resources.probes.valid[probeIndex])
        {
            resources.probes.historyValid[probeIndex] = 0u;
            for (int d = 0; d < 6; ++d)
                resources.probes.radiance[d][probeIndex] = glm::vec3(0.f);
        }
        else
        {
            const float blend = resources.probes.historyValid[probeIndex] ? 0.15f : 1.f;
            for (int d = 0; d < 6; ++d)
            {
                const glm::vec3 sample = (weights[d] > 1e-5f) ? sums[d] / weights[d] : glm::vec3(0.f);
                resources.probes.radiance[d][probeIndex] =
                    glm::mix(resources.probes.radiance[d][probeIndex], sample, blend);
            }
            resources.probes.historyValid[probeIndex] = 1u;
        }
        ++updated;
    }

    if (resources.probes.dirty && resources.probes.updateCursor >= resources.probes.updateOrder.size())
    {
        resources.probes.dirty = false;
        resources.probes.converged = true;
        resources.probes.updateCursor = 0;
        std::printf("[HRL][DDGI] initial probe volume converged (%zu probes)\n", resources.probes.updateOrder.size());
    }
    else if (!resources.probes.dirty && resources.probes.converged &&
             resources.probes.updateCursor >= resources.probes.updateOrder.size())
    {
        resources.probes.updateCursor = 0;
    }

    return updated > 0 || initialPass;
}

void GL_33_GI::UploadProbeVolume(SceneResources& resources)
{
    if (resources.probeUbo[0] == 0 || resources.probeUbo[1] == 0 || resources.probeUbo[2] == 0)
        glGenBuffers(3, resources.probeUbo);

    const size_t probeCount = resources.probes.radiance[0].size();
    const size_t count = std::min(probeCount, size_t(kMaxProbeCount));

    // One vec4 = one RGB radiance lobe + padding alpha. Each UBO stores two
    // lobes per probe, so the block remains <= 16 KiB for the guaranteed 512 probes.
    std::array<glm::vec4, kProbeVec4CountPerBlock> block{};

    for (int blockIndex = 0; blockIndex < 3; ++blockIndex)
    {
        std::fill(block.begin(), block.end(), glm::vec4(0.f));
        const int lobe0 = blockIndex * kProbeLobesPerBlock;
        const int lobe1 = lobe0 + 1;

        for (size_t probe = 0; probe < count; ++probe)
        {
            const size_t base = probe * size_t(kProbeLobesPerBlock);
            const float valid = resources.probes.valid[probe] ? 1.f : 0.f;
            block[base + 0] = glm::vec4(resources.probes.radiance[lobe0][probe], valid);
            block[base + 1] = glm::vec4(resources.probes.radiance[lobe1][probe], valid);
        }

        glBindBuffer(GL_UNIFORM_BUFFER, resources.probeUbo[blockIndex]);
        glBufferData(
            GL_UNIFORM_BUFFER,
            (GLsizeiptr)(block.size() * sizeof(glm::vec4)),
            block.data(),
            GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2 + blockIndex, resources.probeUbo[blockIndex]);
    }

    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    ++resources.uploadSerial;
}

bool GL_33_GI::UpdateDDGI(HRL_id sceneId)
{
    HRL_Context* context = GetPrivateContext();
    if (!context)
        return false;
    auto sceneIt = context->scenes.find(sceneId);
    if (sceneIt == context->scenes.end() || !sceneIt->second)
        return false;

    SceneResources& resources = scenes_[sceneId];
    if (!EnsureGeometryCache(sceneId, resources.geometry))
        return false;
    if (!BuildOrUpdateProbeScene(resources.probes, resources.geometry))
        return false;

    ++resources.frameIndex;
    const int budget = resources.probes.dirty
        ? ((resources.probes.updateCursor == 0) ? 48 : kProbeBudgetPerFrame)
        : 4;
    const bool updated = UpdateProbeBatch(sceneId, resources, budget);

    if (updated || resources.probeUbo[0] == 0)
        UploadProbeVolume(resources);

    return true;
}

void GL_33_GI::ApplyToShader(HRL_id sceneId, GL33_Shader* shader)
{
    if (!shader)
        return;

    const GLuint program = (GLuint)shader->GetId();
    if (program == 0)
        return;

    auto sceneIt = scenes_.find(sceneId);
    HRL_Context* context = GetPrivateContext();
    const bool enabled = context &&
        context->scenes.find(sceneId) != context->scenes.end() &&
        context->scenes.at(sceneId) &&
        context->scenes.at(sceneId)->global_illumination_enabled &&
        context->scenes.at(sceneId)->global_illumination_method == HRL_GI_DDGI &&
        sceneIt != scenes_.end() && sceneIt->second.probeUbo[0] != 0 &&
        sceneIt->second.probes.initialized;

    const uint64_t serial = sceneIt != scenes_.end() ? sceneIt->second.uploadSerial : 0;
    if (program == lastShaderProgram_ &&
        sceneId == lastSceneId_ &&
        serial == lastUploadSerial_ &&
        enabled == lastEnabled_)
    {
        if (enabled && sceneIt != scenes_.end())
        {
            glBindBufferBase(GL_UNIFORM_BUFFER, 2, sceneIt->second.probeUbo[0]);
            glBindBufferBase(GL_UNIFORM_BUFFER, 3, sceneIt->second.probeUbo[1]);
            glBindBufferBase(GL_UNIFORM_BUFFER, 4, sceneIt->second.probeUbo[2]);
        }
        return;
    }

    shader->Use();
    shader->SetInt("DDGIEnabled", enabled ? 1 : 0);

    if (enabled)
    {
        const ProbeVolume& probes = sceneIt->second.probes;
        shader->SetVec3("DDGIProbeOrigin", probes.origin);
        shader->SetVec3("DDGIProbeSpacing", probes.spacing);
        shader->SetVec3("DDGIProbeResolution", glm::vec3(
            float(probes.nx), float(probes.ny), float(probes.nz)));
        shader->SetFloat("DDGIStrength", kGIShaderStrength);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, sceneIt->second.probeUbo[0]);
        glBindBufferBase(GL_UNIFORM_BUFFER, 3, sceneIt->second.probeUbo[1]);
        glBindBufferBase(GL_UNIFORM_BUFFER, 4, sceneIt->second.probeUbo[2]);
    }
    else
    {
        shader->SetVec3("DDGIProbeOrigin", glm::vec3(0.f));
        shader->SetVec3("DDGIProbeSpacing", glm::vec3(1.f));
        shader->SetVec3("DDGIProbeResolution", glm::vec3(1.f));
        shader->SetFloat("DDGIStrength", 0.f);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, 0);
        glBindBufferBase(GL_UNIFORM_BUFFER, 3, 0);
        glBindBufferBase(GL_UNIFORM_BUFFER, 4, 0);
    }

    lastShaderProgram_ = program;
    lastSceneId_ = sceneId;
    lastUploadSerial_ = serial;
    lastEnabled_ = enabled;
}
