#ifndef HRL_GL33_GI_H
#define HRL_GL33_GI_H

#include "../../core/gi_scene.h"
#include "gl33_definitions.h"
#include "gl33_shader.h"

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <unordered_map>

/**
 * Global illumination of the OpenGL 3.3 renderer (also used by the Vulkan
 * backend, which runs the same renderer).
 *
 * Methods (HRL_EGlobalIlluminationMethod):
 *  - HRL_GI_SSGI        screen space rays against the depth buffer, radiance of
 *                       the lit image, temporal accumulation + bilateral filter
 *  - HRL_GI_VCT         voxel cone tracing in a 64^3 voxel volume lit with
 *                       ray traced shadows, 6 cones per pixel
 *  - HRL_GI_LPV         light propagation volume (L1 SH, 32^3) injected from
 *                       the lit voxels and propagated with occlusion
 *  - HRL_GI_DDGI        dynamic diffuse GI : probe grid updated every frame by
 *                       GPU ray tracing, irradiance + depth moments (octahedral)
 *                       with Chebyshev visibility, infinite bounces
 *  - HRL_GI_RAY_TRACING one ray traced bounce per pixel and per frame,
 *                       temporal accumulation + denoising (real time)
 *  - HRL_GI_PATH_TRACING multi bounce path tracing accumulated while the view
 *                       is still (reference quality)
 *
 * Ray tracing runs in fragment shaders on a BVH stored in textures (OpenGL 3.3
 * has no compute shaders and no ray tracing hardware access). The final
 * indirect light is added to the scene color in a screen pass that reads the
 * GI G-buffer (diffuse albedo + normal) and the depth buffer.
 */
class GL_33_GI final {
public:
    GL_33_GI() = default;
    ~GL_33_GI();

    void Initialize(GLuint fullscreenVao);
    void Shutdown();

    static bool Supports(int method);
    static uint32_t SupportedMethods();

    // World space work (BVH upload, probes, voxels, propagation). Once per
    // scene and per frame, after the light uniform buffer was uploaded.
    void UpdateScene(HRL_id sceneId, const hrl_scene_t* scene);

    struct View {
        HRL_id sceneId = HRL_INVALID_ID;
        const hrl_scene_t* scene = nullptr;
        HRL_id viewportId = HRL_INVALID_ID;
        int sceneWidth = 1;
        int sceneHeight = 1;
        GLuint depth = 0;
        GLuint normal = 0;
        GLuint albedo = 0;
        GLuint sceneColor = 0;   // input color (full scene texture)
        GLuint outputFbo = 0;    // receives the viewport rectangle with GI
        glm::mat4 view{1.f};
        glm::mat4 projection{1.f};
        glm::vec3 cameraPosition{0.f};
        int x = 0, y = 0, width = 1, height = 1;
    };
    // Screen space part : writes sceneColor + indirect light into outputFbo.
    // Returns false when GI was not applied (nothing was written).
    bool Apply(const View& view);

    void ReleaseScene(HRL_id sceneId);
    void ReleaseViewport(HRL_id viewportId);

private:
    struct Target {
        GLuint texture = 0;
        GLuint fbo = 0;
    };
    struct SceneGPU {
        HRL_GISceneData data;
        uint64_t uploadedRevision = 0;
        GLuint nodes = 0;
        GLuint triangles = 0;
        uint64_t frame = 0;

        // DDGI
        bool ddgiReady = false;
        Target ddgiRays;
        Target ddgiIrradiance[2];
        Target ddgiDistance[2];
        int ddgiCurrent = 0;
        int ddgiTilesPerRow = 1;
        uint64_t ddgiFrames = 0;
        uint64_t ddgiLightingRevision = 0;

        // Voxels (VCT, LPV)
        bool voxelsReady = false;
        GLuint voxelAlbedo = 0;
        GLuint voxelNormal = 0;
        Target voxelLevels[6];
        Target voxelScratch;
        uint64_t voxelLightingRevision = 0;
        bool voxelsLit = false;

        // LPV
        bool lpvReady = false;
        GLuint lpvTex[3][3] = {}; // [current, next, accumulated][r, g, b]
        GLuint lpvFbo[3] = {};
        uint64_t lpvVoxelSerial = 0;
        uint64_t voxelSerial = 0;
    };
    struct ViewGPU {
        HRL_id sceneId = HRL_INVALID_ID;
        int method = 0;
        int width = 0, height = 0;
        float scale = 1.f;
        Target target;
        Target history[2];
        int current = 0;
        bool historyValid = false;
        glm::mat4 previousViewProjection{1.f};
        glm::vec3 previousCamera{0.f};
        glm::vec4 previousRect{0.f};
        uint64_t geometryRevision = 0;
        uint64_t lightingRevision = 0;
        float sampleCount = 0.f;
    };

    GL33_Shader* Program(const char* key);
    void SetCommon(GL33_Shader* s, SceneGPU& g, const hrl_scene_t* scene, int& unit);
    void SetDDGI(GL33_Shader* s, SceneGPU& g, int& unit);
    void SetVCT(GL33_Shader* s, SceneGPU& g, int& unit);
    void SetLPV(GL33_Shader* s, SceneGPU& g, int& unit);
    void SetScreen(GL33_Shader* s, const View& v, float scale, int& unit);
    void Bind(GL33_Shader* s, const char* name, GLuint texture, int& unit);
    void Quad();

    bool UploadScene(SceneGPU& g);
    void UpdateDDGI(SceneGPU& g, const hrl_scene_t* scene);
    void UpdateVoxels(SceneGPU& g, const hrl_scene_t* scene);
    void UpdateLPV(SceneGPU& g, const hrl_scene_t* scene);
    void ReleaseSceneGPU(SceneGPU& g);
    void ReleaseView(ViewGPU& v);

    GLuint fullscreenVao_ = 0;
    std::unordered_map<std::string, std::unique_ptr<GL33_Shader>> programs_;
    std::unordered_map<HRL_id, std::unique_ptr<SceneGPU>> scenes_;
    std::unordered_map<HRL_id, std::unique_ptr<ViewGPU>> views_;
};

#endif
