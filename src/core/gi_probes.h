#ifndef HRL_GI_PROBES_H
#define HRL_GI_PROBES_H

#include "../hrl.h"
#include "object_types.h"

#include <array>
#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

/**
 * World-space diffuse global illumination (CPU part, shared by every backend).
 *
 * This is an irradiance-probe volume inspired by DDGI. The probes are updated
 * on the CPU from the scene's static triangle geometry, accelerated by a BVH,
 * and the resulting volume is consumed directly by the forward mesh shader.
 *
 * Keeping the final GI contribution inside the normal material shader avoids
 * any dependency on screen-space depth, resolve targets, or post-processing
 * framebuffers. The probe volume is uploaded through a small std140 UBO.
 */
class HRL_GIProbes {
public:
    HRL_GIProbes() = default;
    virtual ~HRL_GIProbes() = default;

    HRL_GIProbes(const HRL_GIProbes&) = delete;
    HRL_GIProbes& operator=(const HRL_GIProbes&) = delete;

    void Shutdown();

    bool Supports(HRL_EGlobalIlluminationMethod method) const;
    uint32_t GetSupportedMethods() const;

    void ReleaseScene(HRL_id sceneId);

    // Updates the world-space probe volume. This is deliberately separate from
    // the camera/view rendering path: once a static volume is converged there
    // is no per-frame CPU ray tracing cost until geometry or lights change.
    bool UpdateDDGI(HRL_id sceneId);

    // Probe UBO layout: three std140 blocks (DDGIBlock0..2), each holding two
    // directional lobes (vec4) per probe.
    static constexpr int kMaxProbeCount = 384;
    static constexpr int kDirectionalLobes = 6;
    static constexpr int kProbeLobesPerBlock = 2;
    static constexpr int kProbeVec4CountPerBlock = kMaxProbeCount * kProbeLobesPerBlock;
    using ProbeBlocks = std::array<std::vector<glm::vec4>, 3>;

    // True when the scene uses DDGI and its probe volume has been uploaded.
    bool IsActive(HRL_id sceneId) const;
    // Shader parameters of the current volume (valid when IsActive()).
    struct ShaderParams { glm::vec3 origin{0.f}; glm::vec3 spacing{1.f}; glm::vec3 resolution{1.f}; float strength = 0.f; };
    ShaderParams GetShaderParams(HRL_id sceneId) const;
    uint64_t GetUploadSerial(HRL_id sceneId) const;

protected:
    // Called when the probe radiance changed and must reach the GPU.
    virtual void OnUploadProbeBlocks(HRL_id sceneId, const ProbeBlocks& blocks) = 0;
    // Called when the GPU data of a scene must be released.
    virtual void OnReleaseSceneGPU(HRL_id sceneId) = 0;
    virtual void OnShutdownGPU() {}

public:
    struct Triangle {
        glm::vec3 p0{};
        glm::vec3 p1{};
        glm::vec3 p2{};
        glm::vec3 normal{0.f, 1.f, 0.f};
        glm::vec3 centroid{0.f};
        glm::vec2 uv0{0.f};
        glm::vec2 uv1{0.f};
        glm::vec2 uv2{0.f};
        HRL_id material = HRL_INVALID_ID;
    };

    struct ProbeVolume {
        glm::vec3 origin{0.f};
        glm::vec3 extent{1.f};
        glm::vec3 spacing{1.f};
        int nx = 0;
        int ny = 0;
        int nz = 0;

        // Six directional diffuse radiance lobes per probe (+/- X/Y/Z).
        std::vector<glm::vec3> radiance[6];
        std::vector<uint8_t> valid;
        std::vector<uint8_t> historyValid;
        std::vector<uint32_t> updateOrder;
        size_t updateCursor = 0;

        uint64_t geometryRevision = 0;
        uint64_t lightingRevision = 0;
        bool initialized = false;
        bool dirty = true;
        bool converged = false;
    };

private:
    struct Hit {
        float t = 0.f;
        glm::vec3 position{0.f};
        glm::vec3 normal{0.f, 1.f, 0.f};
        glm::vec2 uv{0.f};
        HRL_id material = HRL_INVALID_ID;
        int triangle = -1;
    };

    struct BVHNode {
        glm::vec3 bmin{0.f};
        glm::vec3 bmax{0.f};
        uint32_t left = UINT32_MAX;
        uint32_t right = UINT32_MAX;
        uint32_t first = 0;
        uint32_t count = 0;
        bool IsLeaf() const { return left == UINT32_MAX; }
    };

    struct GeometryCache {
        std::vector<Triangle> triangles;
        std::vector<uint32_t> indices;
        std::vector<BVHNode> nodes;
        glm::vec3 boundsMin{0.f};
        glm::vec3 boundsMax{0.f};
        uint64_t revision = 0;
        bool valid = false;
    };

    struct SceneResources {
        ProbeVolume probes;
        GeometryCache geometry;
        bool uploaded = false;
        uint64_t frameIndex = 0;
        uint64_t uploadSerial = 0;
    };

    bool EnsureGeometryCache(HRL_id sceneId, GeometryCache& geometry);
    void BuildBVH(GeometryCache& geometry);
    uint32_t BuildBVHNode(GeometryCache& geometry, uint32_t first, uint32_t count);

    bool BuildOrUpdateProbeScene(ProbeVolume& probes, const GeometryCache& geometry);
    bool UpdateProbeBatch(HRL_id sceneId, SceneResources& resources, int budget);
    void UploadProbeVolume(HRL_id sceneId, SceneResources& resources);

    bool TraceNearest(
        const GeometryCache& geometry,
        const glm::vec3& origin,
        const glm::vec3& direction,
        float maxDistance,
        Hit& hit,
        int ignoredTriangle = -1) const;

    glm::vec3 SampleCpuIrradiance(
        const ProbeVolume& probes,
        const glm::vec3& position,
        const glm::vec3& normal) const;

    glm::vec3 EvaluateHitRadiance(
        HRL_id sceneId,
        const SceneResources& resources,
        const Hit& hit) const;

    glm::vec3 SampleSky(const hrl_scene_t* scene, const glm::vec3& direction) const;
    glm::vec3 GetMaterialTint(HRL_id materialId) const;
    glm::vec3 SampleMaterialAlbedo(HRL_id materialId, const glm::vec2& uv, const glm::vec3& fallback) const;
    float SampleMaterialScalar(HRL_id materialId, const char* useValueName, const char* valueName, const char* textureName, const glm::vec2& uv, float fallback) const;
    glm::mat4 CalculateModelMatrix(const HRL_Mesh* mesh) const;

    std::unordered_map<HRL_id, SceneResources> scenes_;
};

#endif
