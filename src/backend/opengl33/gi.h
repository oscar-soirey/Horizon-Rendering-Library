#ifndef HRL_GL33_GI_H
#define HRL_GL33_GI_H

#include "../../core/gi_probes.h"
#include "gl33_definitions.h"
#include "gl33_shader.h"

#include <glad/glad.h>

/**
 * OpenGL 3.3 front-end of the shared DDGI-style probe volume (core/gi_probes).
 * The CPU probe update is backend-agnostic; this class only owns the three
 * std140 UBOs (bindings 2..4) and feeds the built-in mesh shader.
 */
class GL_33_GI final : public HRL_GIProbes {
public:
    GL_33_GI() = default;
    ~GL_33_GI() override;

    void Initialize(GLuint fullscreenVao);

    // Applies the current scene's DDGI UBO + uniforms to a built-in mesh shader.
    void ApplyToShader(HRL_id sceneId, GL33_Shader* shader);

protected:
    void OnUploadProbeBlocks(HRL_id sceneId, const ProbeBlocks& blocks) override;
    void OnReleaseSceneGPU(HRL_id sceneId) override;
    void OnShutdownGPU() override;

private:
    struct SceneUBOs { GLuint ubo[3] = {0, 0, 0}; };
    std::unordered_map<HRL_id, SceneUBOs> ubos_;
    GLuint fullscreenVao_ = 0;

    // Last shader state we pushed, to avoid repeatedly writing the same GI
    // uniforms for every mesh using the built-in shader.
    GLuint lastShaderProgram_ = 0;
    HRL_id lastSceneId_ = HRL_INVALID_ID;
    uint64_t lastUploadSerial_ = 0;
    bool lastEnabled_ = false;
};

#endif
