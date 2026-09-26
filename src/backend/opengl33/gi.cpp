#include "gi.h"
#include "../../core/utils_functions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

static const char* kFullscreenVertexShader = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main()
{
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

//
// This is deliberately a screen-space radiance gather rather than a fragile
// view-space ray tracer. It gathers nearby visible surfaces from Scene Color,
// weights them by receiver/source orientation and 3D proximity, and leaves the
// receiver albedo multiplication to the composite stage.
//
static const char* kRawSSGIFragmentShader = R"GLSL(
#version 330 core

in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneColor;
uniform sampler2D uNormal;
uniform sampler2D uDepth;
uniform mat4 uInvProjection;
uniform mat4 uView;
uniform vec2 uViewportSize;
uniform vec4 uViewportRect;
uniform float uRadiusPixels;
uniform float uGIIntensity;
uniform float uMaxRadiance;
uniform float uFrameIndex;

const float PI = 3.14159265358979323846;
const int SAMPLE_COUNT = 24;

vec2 GlobalUV(vec2 localUV)
{
    return mix(uViewportRect.xy, uViewportRect.zw, localUV);
}

float SampleDepth(vec2 localUV)
{
    return texture(uDepth, GlobalUV(clamp(localUV, vec2(0.0), vec2(1.0)))).r;
}

vec3 ViewPosition(vec2 localUV, float depth)
{
    vec2 ndc = localUV * 2.0 - 1.0;
    float ndcZ = depth * 2.0 - 1.0;
    vec4 p = uInvProjection * vec4(ndc, ndcZ, 1.0);
    return p.xyz / max(abs(p.w), 1e-6);
}

vec3 SampleNormalVS(vec2 localUV)
{
    vec3 nWorld = texture(uNormal, GlobalUV(clamp(localUV, vec2(0.0), vec2(1.0)))).xyz;
    float n2 = dot(nWorld, nWorld);
    if (n2 <= 1e-8)
        return vec3(0.0, 0.0, 1.0);
    return normalize(mat3(uView) * nWorld);
}

float Hash12(vec2 p)
{
    p = fract(p * vec2(0.1031, 0.11369));
    p += dot(p, p.yx + 33.33);
    return fract((p.x + p.y) * p.x);
}

void main()
{
    float receiverDepth = SampleDepth(vUV);
    if (receiverDepth >= 0.99999)
    {
        FragColor = vec4(0.0);
        return;
    }

    vec3 receiverPosition = ViewPosition(vUV, receiverDepth);
    vec3 receiverNormal = SampleNormalVS(vUV);

    float pixelNoise = Hash12(gl_FragCoord.xy + vec2(uFrameIndex * 0.37, uFrameIndex * 1.13));
    float goldenAngle = 2.399963229728653;
    vec2 invViewport = 1.0 / max(uViewportSize, vec2(1.0));

    vec3 accumulated = vec3(0.0);
    float accumulatedWeight = 0.0;

    for (int i = 0; i < SAMPLE_COUNT; ++i)
    {
        float fi = float(i) + 0.5;
        float normalized = fi / float(SAMPLE_COUNT);
        float radius = sqrt(normalized) * uRadiusPixels;
        float angle = goldenAngle * float(i) + pixelNoise * 2.0 * PI;

        vec2 dir = vec2(cos(angle), sin(angle));
        vec2 sampleUV = vUV + dir * radius * invViewport;

        if (sampleUV.x <= 0.002 || sampleUV.y <= 0.002 ||
            sampleUV.x >= 0.998 || sampleUV.y >= 0.998)
            continue;

        float sampleDepth = SampleDepth(sampleUV);
        if (sampleDepth >= 0.99999)
            continue;

        vec3 samplePosition = ViewPosition(sampleUV, sampleDepth);
        vec3 toSource = samplePosition - receiverPosition;
        float distanceToSource = length(toSource);
        if (distanceToSource < 0.025)
            continue;

        vec3 lightDirection = toSource / distanceToSource;
        vec3 sourceNormal = SampleNormalVS(sampleUV);

        float receiverCos = max(dot(receiverNormal, lightDirection), 0.0);
        // Keep a small floor so nearby visible radiance can still bleed between
        // surfaces whose exact projected direction is imperfect in screen space.
        receiverCos = max(receiverCos, 0.10);

        float sourceCos = max(dot(sourceNormal, -lightDirection), 0.0);
        float sourceWeight = mix(0.30, 1.0, sourceCos);

        // Depth-aware proximity. The tolerance scales with view distance, making
        // the gather work with both small and large scenes.
        float depthScale = max(0.04, abs(receiverPosition.z) * 0.12);
        float depthDifference = abs(samplePosition.z - receiverPosition.z);
        float depthWeight = exp(-depthDifference / max(depthScale, 1e-4));

        float radiusWeight = 1.0 - smoothstep(0.05 * uRadiusPixels, uRadiusPixels, radius);
        radiusWeight = max(radiusWeight, 0.06);

        vec3 sourceRadiance = max(texture(uSceneColor, GlobalUV(sampleUV)).rgb, vec3(0.0));
        sourceRadiance = min(sourceRadiance, vec3(4.0));
        float sourceLuma = dot(sourceRadiance, vec3(0.2126, 0.7152, 0.0722));
        if (sourceLuma <= 1e-4)
            continue;

        // Mild world-distance attenuation. It prevents distant screen samples from
        // becoming an unbounded ambient term without making the effect scale-away.
        float distanceWeight = 1.0 / (1.0 + distanceToSource * distanceToSource * 0.01);
        float weight = receiverCos * sourceWeight * depthWeight * radiusWeight * distanceWeight;

        accumulated += sourceRadiance * weight;
        accumulatedWeight += weight;
    }

    // Normalize by the valid sample weight instead of the fixed sample count.
    // This keeps the bounce visible even when only a small part of the neighborhood
    // contains useful radiance, while the radiance clamp below limits outliers.
    vec3 gi = accumulated / max(accumulatedWeight, 0.75);
    gi *= uGIIntensity;

    float luminance = dot(gi, vec3(0.2126, 0.7152, 0.0722));
    if (luminance > uMaxRadiance)
        gi *= uMaxRadiance / max(luminance, 1e-5);

    FragColor = vec4(max(gi, vec3(0.0)), 1.0);
}
)GLSL";

static const char* kDenoiseFragmentShader = R"GLSL(
#version 330 core

in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputGI;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
uniform mat4 uInvProjection;
uniform mat4 uView;
uniform vec4 uViewportRect;
uniform vec2 uViewportSize;
uniform vec2 uDirection;
uniform float uSigma;
uniform float uDepthSigma;
uniform float uNormalPower;

vec2 GlobalUV(vec2 localUV)
{
    return mix(uViewportRect.xy, uViewportRect.zw, localUV);
}

float SampleDepth(vec2 localUV)
{
    return texture(uDepth, GlobalUV(clamp(localUV, vec2(0.0), vec2(1.0)))).r;
}

vec3 ViewPosition(vec2 localUV, float depth)
{
    vec2 ndc = localUV * 2.0 - 1.0;
    float ndcZ = depth * 2.0 - 1.0;
    vec4 p = uInvProjection * vec4(ndc, ndcZ, 1.0);
    return p.xyz / max(abs(p.w), 1e-6);
}

vec3 SampleNormalVS(vec2 localUV)
{
    vec3 nWorld = texture(uNormal, GlobalUV(clamp(localUV, vec2(0.0), vec2(1.0)))).xyz;
    float n2 = dot(nWorld, nWorld);
    if (n2 <= 1e-8)
        return vec3(0.0, 0.0, 1.0);
    return normalize(mat3(uView) * nWorld);
}

void main()
{
    float centerDepth = SampleDepth(vUV);
    if (centerDepth >= 0.99999)
    {
        FragColor = vec4(0.0);
        return;
    }

    vec2 texel = 1.0 / max(uViewportSize, vec2(1.0));
    vec3 centerGI = texture(uInputGI, GlobalUV(vUV)).rgb;
    vec3 centerP = ViewPosition(vUV, centerDepth);
    vec3 centerN = SampleNormalVS(vUV);

    vec3 sum = centerGI;
    float weightSum = 1.0;

    const int RADIUS = 2;
    for (int i = 1; i <= RADIUS; ++i)
    {
        float fi = float(i);
        float spatialWeight = exp(-(fi * fi) / (2.0 * uSigma * uSigma));

        for (int sign = -1; sign <= 1; sign += 2)
        {
            vec2 offsetUV = clamp(vUV + uDirection * texel * fi * float(sign), 0.0, 1.0);
            float d = SampleDepth(offsetUV);
            if (d >= 0.99999)
                continue;

            vec3 P = ViewPosition(offsetUV, d);
            vec3 N = SampleNormalVS(offsetUV);

            float depthScale = max(0.05, abs(centerP.z) * 0.10);
            float depthWeight = exp(-abs(P.z - centerP.z) /
                max(uDepthSigma * depthScale, 1e-4));
            float normalWeight = pow(max(dot(centerN, N), 0.0), uNormalPower);
            float w = spatialWeight * depthWeight * normalWeight;

            sum += texture(uInputGI, GlobalUV(offsetUV)).rgb * w;
            weightSum += w;
        }
    }

    FragColor = vec4(sum / max(weightSum, 1e-5), 1.0);
}
)GLSL";

static const char* kCompositeFragmentShader = R"GLSL(
#version 330 core

in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneColor;
uniform sampler2D uGI;
uniform sampler2D uAlbedo;
uniform vec4 uViewportRect;
uniform float uStrength;

vec2 GlobalUV(vec2 localUV)
{
    return mix(uViewportRect.xy, uViewportRect.zw, localUV);
}

void main()
{
    vec2 uv = GlobalUV(vUV);
    vec4 scene = texture(uSceneColor, uv);
    vec3 gi = max(texture(uGI, uv).rgb, vec3(0.0));
    vec3 albedo = max(texture(uAlbedo, uv).rgb, vec3(0.0));

    // Indirect diffuse is reconstructed separately from the receiver material.
    // Scene Color is already lit, while albedo is the local diffuse response.
    vec3 indirect = gi * albedo * uStrength;
    FragColor = vec4(max(scene.rgb + indirect, vec3(0.0)), scene.a);
}
)GLSL";

static GLuint CompileShader(GLenum type, const char* source, const char* label)
{
    GLuint shader = glCreateShader(type);
    if (!shader)
        return 0;

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE)
    {
        char log[4096] = {};
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log), nullptr, log);
        SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_ERROR,
            "GL33 SSGI " + std::string(label) + " shader compilation failed: " + log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint LinkProgram(GLuint vs, GLuint fs, const char* label)
{
    GLuint program = glCreateProgram();
    if (!program)
        return 0;

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE)
    {
        char log[4096] = {};
        glGetProgramInfoLog(program, (GLsizei)sizeof(log), nullptr, log);
        SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_ERROR,
            std::string("GL33 SSGI ") + label + " program link failed: " + log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static void DrawFullscreen(GLuint vao)
{
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
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

GLuint GL_33_GI::CreateProgram(const char* vertexSource, const char* fragmentSource, const char* label)
{
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexSource, label);
    if (!vs)
        return 0;

    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentSource, label);
    if (!fs)
    {
        glDeleteShader(vs);
        return 0;
    }

    GLuint program = LinkProgram(vs, fs, label);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

bool GL_33_GI::EnsureProgram()
{
    if (rawProgram_ && denoiseProgram_ && compositeProgram_)
        return true;

    if (!rawProgram_)
        rawProgram_ = CreateProgram(kFullscreenVertexShader, kRawSSGIFragmentShader, "raw");
    if (!rawProgram_)
        return false;

    if (!denoiseProgram_)
        denoiseProgram_ = CreateProgram(kFullscreenVertexShader, kDenoiseFragmentShader, "denoise");
    if (!denoiseProgram_)
        return false;

    if (!compositeProgram_)
        compositeProgram_ = CreateProgram(kFullscreenVertexShader, kCompositeFragmentShader, "composite");
    if (!compositeProgram_)
        return false;

    rawLocSceneColor_ = glGetUniformLocation(rawProgram_, "uSceneColor");
    rawLocNormal_ = glGetUniformLocation(rawProgram_, "uNormal");
    rawLocDepth_ = glGetUniformLocation(rawProgram_, "uDepth");
    rawLocInvProjection_ = glGetUniformLocation(rawProgram_, "uInvProjection");
    rawLocView_ = glGetUniformLocation(rawProgram_, "uView");
    rawLocViewportRect_ = glGetUniformLocation(rawProgram_, "uViewportRect");
    rawLocViewportSize_ = glGetUniformLocation(rawProgram_, "uViewportSize");
    rawLocRadiusPixels_ = glGetUniformLocation(rawProgram_, "uRadiusPixels");
    rawLocGIIntensity_ = glGetUniformLocation(rawProgram_, "uGIIntensity");
    rawLocMaxRadiance_ = glGetUniformLocation(rawProgram_, "uMaxRadiance");
    rawLocFrameIndex_ = glGetUniformLocation(rawProgram_, "uFrameIndex");

    glUseProgram(rawProgram_);
    glUniform1i(rawLocSceneColor_, 0);
    glUniform1i(rawLocDepth_, 1);
    glUniform1i(rawLocNormal_, 2);

    denoiseLocInputGI_ = glGetUniformLocation(denoiseProgram_, "uInputGI");
    denoiseLocDepth_ = glGetUniformLocation(denoiseProgram_, "uDepth");
    denoiseLocNormal_ = glGetUniformLocation(denoiseProgram_, "uNormal");
    denoiseLocInvProjection_ = glGetUniformLocation(denoiseProgram_, "uInvProjection");
    denoiseLocView_ = glGetUniformLocation(denoiseProgram_, "uView");
    denoiseLocViewportRect_ = glGetUniformLocation(denoiseProgram_, "uViewportRect");
    denoiseLocViewportSize_ = glGetUniformLocation(denoiseProgram_, "uViewportSize");
    denoiseLocDirection_ = glGetUniformLocation(denoiseProgram_, "uDirection");
    denoiseLocSigma_ = glGetUniformLocation(denoiseProgram_, "uSigma");
    denoiseLocDepthSigma_ = glGetUniformLocation(denoiseProgram_, "uDepthSigma");
    denoiseLocNormalPower_ = glGetUniformLocation(denoiseProgram_, "uNormalPower");

    glUseProgram(denoiseProgram_);
    glUniform1i(denoiseLocInputGI_, 0);
    glUniform1i(denoiseLocDepth_, 1);
    glUniform1i(denoiseLocNormal_, 2);

    compositeLocSceneColor_ = glGetUniformLocation(compositeProgram_, "uSceneColor");
    compositeLocGI_ = glGetUniformLocation(compositeProgram_, "uGI");
    compositeLocAlbedo_ = glGetUniformLocation(compositeProgram_, "uAlbedo");
    compositeLocStrength_ = glGetUniformLocation(compositeProgram_, "uStrength");
    compositeLocViewportRect_ = glGetUniformLocation(compositeProgram_, "uViewportRect");

    glUseProgram(compositeProgram_);
    glUniform1i(compositeLocSceneColor_, 0);
    glUniform1i(compositeLocGI_, 1);
    glUniform1i(compositeLocAlbedo_, 2);

    glUseProgram(0);
    return true;
}

void GL_33_GI::Shutdown()
{
    for (auto& sceneEntry : scenes_)
        for (auto& viewportEntry : sceneEntry.second)
            DestroySceneResources(viewportEntry.second);
    scenes_.clear();

    if (rawProgram_) glDeleteProgram(rawProgram_);
    if (denoiseProgram_) glDeleteProgram(denoiseProgram_);
    if (compositeProgram_) glDeleteProgram(compositeProgram_);
    rawProgram_ = denoiseProgram_ = compositeProgram_ = 0;

    rawLocSceneColor_ = rawLocNormal_ = rawLocDepth_ = -1;
    rawLocInvProjection_ = rawLocView_ = -1;
    rawLocViewportRect_ = rawLocViewportSize_ = -1;
    rawLocRadiusPixels_ = rawLocGIIntensity_ = rawLocMaxRadiance_ = rawLocFrameIndex_ = -1;

    denoiseLocInputGI_ = denoiseLocDepth_ = denoiseLocNormal_ = -1;
    denoiseLocInvProjection_ = denoiseLocView_ = -1;
    denoiseLocViewportRect_ = denoiseLocViewportSize_ = denoiseLocDirection_ = -1;
    denoiseLocSigma_ = denoiseLocDepthSigma_ = denoiseLocNormalPower_ = -1;

    compositeLocSceneColor_ = compositeLocGI_ = compositeLocAlbedo_ = -1;
    compositeLocStrength_ = compositeLocViewportRect_ = -1;
    fullscreenVao_ = 0;
}

bool GL_33_GI::Supports(HRL_EGlobalIlluminationMethod method) const
{
    return method == HRL_GI_SSGI;
}

uint32_t GL_33_GI::GetSupportedMethods() const
{
    return 1u << (unsigned)HRL_GI_SSGI;
}

void GL_33_GI::DestroySceneResources(ViewportResources& resources)
{
    if (resources.depthTexture) glDeleteTextures(1, &resources.depthTexture);
    if (resources.depthFbo) glDeleteFramebuffers(1, &resources.depthFbo);

    if (resources.sceneColorTexture) glDeleteTextures(1, &resources.sceneColorTexture);
    if (resources.sceneColorFbo) glDeleteFramebuffers(1, &resources.sceneColorFbo);

    if (resources.rawTexture) glDeleteTextures(1, &resources.rawTexture);
    if (resources.rawFbo) glDeleteFramebuffers(1, &resources.rawFbo);

    if (resources.denoiseTexture) glDeleteTextures(1, &resources.denoiseTexture);
    if (resources.denoiseFbo) glDeleteFramebuffers(1, &resources.denoiseFbo);

    if (resources.denoisePingTexture) glDeleteTextures(1, &resources.denoisePingTexture);
    if (resources.denoisePingFbo) glDeleteFramebuffers(1, &resources.denoisePingFbo);

    resources = {};
}

static bool AttachColorTexture(GLuint fbo, GLuint texture, int width, int height)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
        GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static bool AttachDepthTexture(GLuint fbo, GLuint texture, int width, int height)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
        GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

bool GL_33_GI::EnsureSceneResources(HRL_id sceneId, HRL_id viewportId, int width, int height)
{
    auto& viewportMap = scenes_[sceneId];
    auto it = viewportMap.find(viewportId);
    if (it == viewportMap.end())
        it = viewportMap.emplace(viewportId, ViewportResources{}).first;

    ViewportResources& r = it->second;
    if (r.width == width && r.height == height &&
        r.depthFbo && r.sceneColorFbo && r.rawFbo && r.denoiseFbo && r.denoisePingFbo)
        return true;

    DestroySceneResources(r);
    r.width = width;
    r.height = height;

    glGenFramebuffers(1, &r.depthFbo);
    glGenTextures(1, &r.depthTexture);
    if (!AttachDepthTexture(r.depthFbo, r.depthTexture, width, height))
    {
        DestroySceneResources(r);
        SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
            "GL33 SSGI: failed to create depth texture framebuffer");
        return false;
    }

    glGenFramebuffers(1, &r.sceneColorFbo);
    glGenTextures(1, &r.sceneColorTexture);
    if (!AttachColorTexture(r.sceneColorFbo, r.sceneColorTexture, width, height))
    {
        DestroySceneResources(r);
        SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
            "GL33 SSGI: failed to create scene color copy framebuffer");
        return false;
    }

    glGenFramebuffers(1, &r.rawFbo);
    glGenTextures(1, &r.rawTexture);
    if (!AttachColorTexture(r.rawFbo, r.rawTexture, width, height))
    {
        DestroySceneResources(r);
        SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
            "GL33 SSGI: failed to create raw GI framebuffer");
        return false;
    }

    glGenFramebuffers(1, &r.denoiseFbo);
    glGenTextures(1, &r.denoiseTexture);
    if (!AttachColorTexture(r.denoiseFbo, r.denoiseTexture, width, height))
    {
        DestroySceneResources(r);
        SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
            "GL33 SSGI: failed to create GI denoise framebuffer");
        return false;
    }

    glGenFramebuffers(1, &r.denoisePingFbo);
    glGenTextures(1, &r.denoisePingTexture);
    if (!AttachColorTexture(r.denoisePingFbo, r.denoisePingTexture, width, height))
    {
        DestroySceneResources(r);
        SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
            "GL33 SSGI: failed to create GI denoise ping framebuffer");
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void GL_33_GI::ReleaseScene(HRL_id sceneId)
{
    auto it = scenes_.find(sceneId);
    if (it == scenes_.end())
        return;
    for (auto& viewportEntry : it->second)
        DestroySceneResources(viewportEntry.second);
    scenes_.erase(it);
}

bool GL_33_GI::CopyDepth(ViewportResources& resources, GLuint sourceFbo, int width, int height)
{
    (void)glGetError();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources.depthFbo);
    glDrawBuffer(GL_NONE);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
        GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        printf("GL33 SSGI: depth copy OpenGL error: 0x%x\n", (unsigned)err);
    return err == GL_NO_ERROR;
}

bool GL_33_GI::CopySceneColor(ViewportResources& resources, const GL_Scene* scene, int width, int height)
{
    (void)glGetError();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources.sceneColorFbo);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        printf("GL33 SSGI: scene color copy OpenGL error: 0x%x\n", (unsigned)err);
    return err == GL_NO_ERROR;
}

bool GL_33_GI::RenderSSGI(
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
    int viewportHeight)
{
    (void)farPlane;

    if (!scene || !fullscreenVao_ || !depthSourceFbo || !scene->fbo)
        return false;
    if (!EnsureProgram())
        return false;
    if (!EnsureSceneResources(sceneId, viewportId, scene->width, scene->height))
        return false;

    auto sceneIt = scenes_.find(sceneId);
    if (sceneIt == scenes_.end())
        return false;
    auto viewportIt = sceneIt->second.find(viewportId);
    if (viewportIt == sceneIt->second.end())
        return false;
    ViewportResources& r = viewportIt->second;

    if (!CopySceneColor(r, scene, scene->width, scene->height))
    {
        printf("GL33 SSGI: scene color copy failed.\n");
        return false;
    }
    if (!CopyDepth(r, depthSourceFbo, scene->width, scene->height))
    {
        printf("GL33 SSGI: depth copy failed.\n");
        return false;
    }

    const glm::mat4 invProjection = glm::inverse(projection);

    GLint oldViewport[4] = {0, 0, 0, 0};
    GLint oldReadFramebuffer = 0;
    GLint oldDrawFramebuffer = 0;
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebuffer);

    const GLboolean oldDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean oldBlend = glIsEnabled(GL_BLEND);
    const GLboolean oldCull = glIsEnabled(GL_CULL_FACE);
    const GLboolean oldScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean oldDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    // 1. Raw screen-space radiance gather.
    glBindFramebuffer(GL_FRAMEBUFFER, r.rawFbo);
    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glUseProgram(rawProgram_);
    glUniformMatrix4fv(rawLocInvProjection_, 1, GL_FALSE, &invProjection[0][0]);
    glUniformMatrix4fv(rawLocView_, 1, GL_FALSE, &view[0][0]);
    glUniform4f(rawLocViewportRect_, viewportRect.x, viewportRect.y, viewportRect.z, viewportRect.w);
    glUniform2f(rawLocViewportSize_, (float)std::max(1, viewportWidth), (float)std::max(1, viewportHeight));
    glUniform1f(rawLocRadiusPixels_, 72.0f);
    glUniform1f(rawLocGIIntensity_, 0.85f);
    glUniform1f(rawLocMaxRadiance_, 2.0f);
    glUniform1f(rawLocFrameIndex_, (float)(r.frameIndex & 1048575ull));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r.sceneColorTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r.depthTexture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, scene->textures[4]);
    DrawFullscreen(fullscreenVao_);

    // 2. Horizontal bilateral denoise.
    glBindFramebuffer(GL_FRAMEBUFFER, r.denoiseFbo);
    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glUseProgram(denoiseProgram_);
    glUniformMatrix4fv(denoiseLocInvProjection_, 1, GL_FALSE, &invProjection[0][0]);
    glUniformMatrix4fv(denoiseLocView_, 1, GL_FALSE, &view[0][0]);
    glUniform4f(denoiseLocViewportRect_, viewportRect.x, viewportRect.y, viewportRect.z, viewportRect.w);
    glUniform2f(denoiseLocViewportSize_, (float)std::max(1, viewportWidth), (float)std::max(1, viewportHeight));
    glUniform2f(denoiseLocDirection_, 1.0f, 0.0f);
    glUniform1f(denoiseLocSigma_, 1.35f);
    glUniform1f(denoiseLocDepthSigma_, 1.35f);
    glUniform1f(denoiseLocNormalPower_, 8.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r.rawTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r.depthTexture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, scene->textures[4]);
    DrawFullscreen(fullscreenVao_);

    // 3. Vertical bilateral denoise.
    glBindFramebuffer(GL_FRAMEBUFFER, r.denoisePingFbo);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glUniform2f(denoiseLocDirection_, 0.0f, 1.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r.denoiseTexture);
    DrawFullscreen(fullscreenVao_);

    // 4. Composite directly into the real scene color attachment. The sampled
    // scene texture is the immutable sceneColorCopy, so there is no feedback loop.
    glBindFramebuffer(GL_FRAMEBUFFER, scene->fbo);
    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glUseProgram(compositeProgram_);
    glUniform4f(compositeLocViewportRect_, viewportRect.x, viewportRect.y, viewportRect.z, viewportRect.w);
    glUniform1f(compositeLocStrength_, 0.70f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r.sceneColorTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r.denoisePingTexture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, scene->textures[3]);
    DrawFullscreen(fullscreenVao_);

    GLenum attachments[5] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2,
        GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4
    };
    glDrawBuffers(5, attachments);

    // MSAA scenes are rendered into the MSAA target first. Publish the composited
    // attachment 0 back to that target so a later viewport resolve cannot erase GI.
    if (scene->msaa_samples > 1 && scene->msaa_fbo != 0 && depthSourceFbo == scene->msaa_fbo)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->msaa_fbo);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glBlitFramebuffer(
            viewportX, viewportY, viewportX + viewportWidth, viewportY + viewportHeight,
            viewportX, viewportY, viewportX + viewportWidth, viewportY + viewportHeight,
            GL_COLOR_BUFFER_BIT, GL_NEAREST);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->msaa_fbo);
        glDrawBuffers(5, attachments);
    }

    // Cleanup / restore the GL state used by the caller.
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    if (oldDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (oldBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (oldCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (oldScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glDepthMask(oldDepthMask ? GL_TRUE : GL_FALSE);
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)oldDrawFramebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)oldReadFramebuffer);

    GLenum finalError = glGetError();
    if (finalError != GL_NO_ERROR)
        printf("GL33 SSGI: OpenGL error after GI pass: 0x%x\n", (unsigned)finalError);

    ++r.frameIndex;
    return true;
}

