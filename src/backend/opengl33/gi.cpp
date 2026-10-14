#include "gi.h"

GL_33_GI::~GL_33_GI()
{
    Shutdown();
}

void GL_33_GI::Initialize(GLuint fullscreenVao)
{
    fullscreenVao_ = fullscreenVao;
}

void GL_33_GI::OnShutdownGPU()
{
    for (auto& [sceneId, ubos] : ubos_)
    {
        (void)sceneId;
        glDeleteBuffers(3, ubos.ubo);
    }
    ubos_.clear();
    fullscreenVao_ = 0;
    lastShaderProgram_ = 0;
    lastSceneId_ = HRL_INVALID_ID;
    lastUploadSerial_ = 0;
    lastEnabled_ = false;
}

void GL_33_GI::OnReleaseSceneGPU(HRL_id sceneId)
{
    auto it = ubos_.find(sceneId);
    if (it != ubos_.end())
    {
        glDeleteBuffers(3, it->second.ubo);
        ubos_.erase(it);
    }
    if (lastSceneId_ == sceneId)
    {
        lastShaderProgram_ = 0;
        lastSceneId_ = HRL_INVALID_ID;
        lastUploadSerial_ = 0;
        lastEnabled_ = false;
    }
}

void GL_33_GI::OnUploadProbeBlocks(HRL_id sceneId, const ProbeBlocks& blocks)
{
    SceneUBOs& ubos = ubos_[sceneId];
    if (ubos.ubo[0] == 0 || ubos.ubo[1] == 0 || ubos.ubo[2] == 0)
        glGenBuffers(3, ubos.ubo);
    for (int blockIndex = 0; blockIndex < 3; ++blockIndex)
    {
        const auto& block = blocks[blockIndex];
        glBindBuffer(GL_UNIFORM_BUFFER, ubos.ubo[blockIndex]);
        glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)(block.size() * sizeof(glm::vec4)), block.data(), GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2 + blockIndex, ubos.ubo[blockIndex]);
    }
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void GL_33_GI::ApplyToShader(HRL_id sceneId, GL33_Shader* shader)
{
    if (!shader)
        return;

    const GLuint program = (GLuint)shader->GetId();
    if (program == 0)
        return;

    auto uboIt = ubos_.find(sceneId);
    const bool enabled = IsActive(sceneId) && uboIt != ubos_.end() && uboIt->second.ubo[0] != 0;
    const uint64_t serial = GetUploadSerial(sceneId);
    if (program == lastShaderProgram_ &&
        sceneId == lastSceneId_ &&
        serial == lastUploadSerial_ &&
        enabled == lastEnabled_)
    {
        if (enabled)
        {
            glBindBufferBase(GL_UNIFORM_BUFFER, 2, uboIt->second.ubo[0]);
            glBindBufferBase(GL_UNIFORM_BUFFER, 3, uboIt->second.ubo[1]);
            glBindBufferBase(GL_UNIFORM_BUFFER, 4, uboIt->second.ubo[2]);
        }
        return;
    }

    shader->Use();
    shader->SetInt("DDGIEnabled", enabled ? 1 : 0);

    if (enabled)
    {
        const ShaderParams params = GetShaderParams(sceneId);
        shader->SetVec3("DDGIProbeOrigin", params.origin);
        shader->SetVec3("DDGIProbeSpacing", params.spacing);
        shader->SetVec3("DDGIProbeResolution", params.resolution);
        shader->SetFloat("DDGIStrength", params.strength);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, uboIt->second.ubo[0]);
        glBindBufferBase(GL_UNIFORM_BUFFER, 3, uboIt->second.ubo[1]);
        glBindBufferBase(GL_UNIFORM_BUFFER, 4, uboIt->second.ubo[2]);
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
