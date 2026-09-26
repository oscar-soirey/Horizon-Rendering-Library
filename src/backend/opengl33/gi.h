#ifndef HRL_GL33_GI_H
#define HRL_GL33_GI_H

#include "../../hrl.h"
#include "gl33_definitions.h"

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <unordered_map>
#include <cstdint>

/**
 * OpenGL 3.3 screen-space global illumination.
 *
 * The GI path deliberately keeps the frame graph small and explicit:
 *
 *   scene color (resolved) -> sceneColorCopy
 *                         -> raw screen-space radiance gather
 *                         -> bilateral denoise (horizontal + vertical)
 *                         -> composite back into scene.fbo attachment 0
 *
 * Depth is copied from the active render target into a single-sample depth
 * texture because the scene depth is otherwise stored in a renderbuffer.
 *
 * There is no temporal accumulation in this implementation. The first goal is
 * a deterministic, visible and debuggable GI signal; temporal reprojection can
 * be added later once the base transport is validated.
 */
class GL_33_GI final {
public:
    GL_33_GI() = default;
    ~GL_33_GI();

    GL_33_GI(const GL_33_GI&) = delete;
    GL_33_GI& operator=(const GL_33_GI&) = delete;

    void Initialize(GLuint fullscreenVao);
    void Shutdown();

    bool Supports(HRL_EGlobalIlluminationMethod method) const;
    uint32_t GetSupportedMethods() const;

    void ReleaseScene(HRL_id sceneId);

    bool RenderSSGI(
        HRL_id sceneId,
        HRL_id viewportId,
        const GL_Scene* scene,
        GLuint depthSourceFbo,
        const glm::mat4& projection,
        const glm::mat4& view,
        float farPlane,
        const glm::vec4& viewportRect,
        int viewportX,
        int viewportY,
        int viewportWidth,
        int viewportHeight);

private:
    struct ViewportResources {
        GLuint depthFbo = 0;
        GLuint depthTexture = 0;

        // Immutable copy of the resolved scene color used by the GI passes.
        GLuint sceneColorFbo = 0;
        GLuint sceneColorTexture = 0;

        GLuint rawFbo = 0;
        GLuint rawTexture = 0;

        GLuint denoiseFbo = 0;
        GLuint denoiseTexture = 0;

        GLuint denoisePingFbo = 0;
        GLuint denoisePingTexture = 0;

        int width = 0;
        int height = 0;
        uint64_t frameIndex = 0;
    };

    bool EnsureProgram();
    bool EnsureSceneResources(HRL_id sceneId, HRL_id viewportId, int width, int height);
    void DestroySceneResources(ViewportResources& resources);
    bool CopyDepth(ViewportResources& resources, GLuint sourceFbo, int width, int height);
    bool CopySceneColor(ViewportResources& resources, const GL_Scene* scene, int width, int height);

    GLuint CreateProgram(const char* vertexSource, const char* fragmentSource, const char* label);

    GLuint fullscreenVao_ = 0;

    GLuint rawProgram_ = 0;
    GLuint denoiseProgram_ = 0;
    GLuint compositeProgram_ = 0;

    // Raw gather.
    GLint rawLocSceneColor_ = -1;
    GLint rawLocNormal_ = -1;
    GLint rawLocDepth_ = -1;
    GLint rawLocInvProjection_ = -1;
    GLint rawLocView_ = -1;
    GLint rawLocViewportRect_ = -1;
    GLint rawLocViewportSize_ = -1;
    GLint rawLocRadiusPixels_ = -1;
    GLint rawLocGIIntensity_ = -1;
    GLint rawLocMaxRadiance_ = -1;
    GLint rawLocFrameIndex_ = -1;

    // Bilateral denoise.
    GLint denoiseLocInputGI_ = -1;
    GLint denoiseLocDepth_ = -1;
    GLint denoiseLocNormal_ = -1;
    GLint denoiseLocInvProjection_ = -1;
    GLint denoiseLocView_ = -1;
    GLint denoiseLocViewportRect_ = -1;
    GLint denoiseLocViewportSize_ = -1;
    GLint denoiseLocDirection_ = -1;
    GLint denoiseLocSigma_ = -1;
    GLint denoiseLocDepthSigma_ = -1;
    GLint denoiseLocNormalPower_ = -1;

    // Composite.
    GLint compositeLocSceneColor_ = -1;
    GLint compositeLocGI_ = -1;
    GLint compositeLocAlbedo_ = -1;
    GLint compositeLocStrength_ = -1;
    GLint compositeLocViewportRect_ = -1;

    std::unordered_map<HRL_id, std::unordered_map<HRL_id, ViewportResources>> scenes_;
};

#endif
