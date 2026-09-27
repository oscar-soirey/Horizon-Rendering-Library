#include "gl33_renderer.h"

#include "../../hrl_gl.h"

#include "gl33_definitions.h"
#include "gl33_shader.h"
#include "gl33_texture.h"
#include "gi.h"
#include "../../ressources/ressources.h"
#include "../../core/utils_functions.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <vector>
#include <unordered_map>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <cstring>
#include <chrono>
#include <sstream>
#include <iomanip>

#include "core/widgets.h"

//HRL PRIVATE
extern HRL_Context* GetPrivateContext();


//UTILS
struct FrustumPlaneSet;

static void InitTextureAndBindToFBO(GLuint _texture, GLuint _fbo, int width, int height);

static bool BindMaterial(HRL_Material* mat, HRL_id object_id, const HRL_Mesh* mesh, const glm::mat4& model);
static bool MaterialIsTwoSided(const HRL_Material* material)
{
	if (!material) return false;
	auto it = material->intParams_.find(HRL_MATERIAL_PARAM_TWO_SIDED);
	return it != material->intParams_.end() && it->second != 0;
}

static bool MaterialUsesScreenSpaceDisplacement(const HRL_Material* material);
static bool CaptureSceneColorForDisplacement(const GL_Scene* scene, GLuint destinationTexture);
static void DrawScreenSpaceDisplacementMeshes(HRL_id scene_id, const hrl_scene_t* scene, const FrustumPlaneSet& frustum);

static glm::mat4 CalculateModelMatrix(const HRL_Mesh* mesh);
static glm::mat4 CalculateProjectionMatrix();
static glm::mat4 CalculateViewMatrix();

static void DrawSkySphere(const hrl_scene_t* scene);
static void DrawOpaqueMeshes(HRL_id scene_id, const std::unordered_map<HRL_id, HRL_Mesh*>& meshes, HRL_EDebugView debug_view, const FrustumPlaneSet& frustum, bool skip_screen_space_displacement = false);
static void DrawSprites(const std::unordered_map<HRL_id, HRL_Mesh*>& meshes, const FrustumPlaneSet& frustum);
static void CreateSpriteGeometry();
static void InitVFXRenderer();
static void DrawVFX(const hrl_scene_t* scene);

void DrawWidgets(const std::unordered_map<HRL_id, HRL_Widget*>& widgets, const HRL_Viewport* viewport);
static void DrawMeshDebugInfoTexts(const hrl_scene_t* scene, const HRL_Viewport* viewport);
static void DrawGizmos(const hrl_scene_t* scene, const HRL_Viewport* viewport, HRL_id viewport_id);
static void GL33_DrawGizmoOverlay(const DebugRenderer& renderer, float line_thickness);
static void DrawPostProcessQuad(GLuint src_texture, GLuint bright_texture, HRL_PostProcess* pp);
static bool HasActiveVolumetricFog(const hrl_scene_t* scene)
{
    if (!scene)
        return false;

    const auto& globalFog = scene->global_volumetric_fog;
    if (globalFog.enabled && globalFog.density > 0.0f)
        return true;

    for (const auto& [fogId, fog] : scene->volumetric_fogs)
    {
        (void)fogId;
        if (fog && fog->enabled && fog->density > 0.0f && fog->radius > 0.0f)
            return true;
    }
    return false;
}

static void ApplyAmbientOcclusion(GL_Scene* scene, const hrl_scene_t* hrlScene, const glm::mat4& view, const glm::mat4& projection, int viewportX, int viewportY, int viewportWidth, int viewportHeight, GLuint& srcIndex);
static void ApplyScreenSpaceReflections(GL_Scene* scene, const hrl_scene_t* hrlScene, const glm::mat4& view, const glm::mat4& projection, int viewportX, int viewportY, int viewportWidth, int viewportHeight, GLuint& srcIndex);
static void ApplyDecals(GL_Scene* scene, const hrl_scene_t* hrlScene, const glm::mat4& view, const glm::mat4& projection, int viewportX, int viewportY, int viewportWidth, int viewportHeight, GLuint& srcIndex);
static void ApplyVolumetricCloudsPass(GL_Scene* scene, const hrl_scene_t* hrlScene, const glm::mat4& view, const glm::mat4& projection, int viewportX, int viewportY, int viewportWidth, int viewportHeight, GLuint& srcIndex);

static void ApplySceneEffects(GL_Scene* scene, const hrl_scene_t* hrlScene, const glm::mat4& view, const glm::mat4& projection, int viewportX, int viewportY, int viewportWidth, int viewportHeight, GLuint& srcIndex);
static void PrepareSceneShadows(hrl_scene_t* scene, HRL_id scene_id);
static void UploadSceneLights(const hrl_scene_t* scene, HRL_id scene_id);
static void DestroyLightShadowResources(HRL_id light_id);
static void DestroyMSAAResources(GL_Scene* scene);
static bool CreateMSAAResources(GL_Scene* scene, int samples);
static void ResolveSceneMSAA(GL_Scene* scene);
static glm::vec3 GetLightDirection(const HRL_Light* light);
static bool EnsureShadowResource(HRL_Light* light);
static void RenderShadowCasters(hrl_scene_t* scene, const glm::mat4& lightViewProjection, const glm::mat4& lightView, const glm::mat4& lightProjection, int shadowResolution, GL33_Shader* shader, bool pointLight, const glm::vec3& lightPosition, float farPlane, int face);
static void DrawLandscapes(HRL_id scene_id, const hrl_scene_t* scene, const FrustumPlaneSet& frustum);
static void DrawLandscapeShadowCasters(hrl_scene_t* scene, const glm::mat4& lightViewProjection, const glm::mat4& lightView, const glm::mat4& lightProjection, int shadowResolution, bool pointLight, const glm::vec3& lightPosition, float farPlane, int face);



//GL33 BACKEND IMPLEMENTATION
#define BUFFER_QUAD			0
#define BUFFER_DEBUG		1
#define BUFFER_UI				2
#define BUFFER_COUNT		3

#define UBO_LIGHTS			0
#define UBO_COUNT				1

#define SHADOW_2D_TEXTURE_UNIT_BASE 6
#define SHADOW_CUBE_TEXTURE_UNIT_BASE 10
#define ENVIRONMENT_TEXTURE_UNIT 14
#define MAX_SHADOW_SLOTS 4
#define MAX_SPRITE_BATCH_INSTANCES 16384

struct GL33_Backend {
	GLuint vao[BUFFER_COUNT];
	GLuint vbo[BUFFER_COUNT];
	GLuint ebo[BUFFER_COUNT];
	GLuint ubo[UBO_COUNT];

	//contains the render technique of the scene
	std::unordered_map<HRL_id, GL_Scene*> gpu_scenes;

	//backend ressources
	std::unordered_map<HRL_id, GL33_Shader*> shaders;
	std::unordered_map<HRL_id, GL33_Texture*> textures;

	struct MeshLOD_GPU {
		GLuint vao = 0;
		GLuint vbo = 0;
		GLuint ebo = 0;
		GLsizei vertex_count = 0;
		GLsizei index_count = 0;
		bool indexed = false;
	};
	struct MeshGPU {
		std::vector<MeshLOD_GPU> levels;
	};
	std::unordered_map<HRL_id, MeshGPU> meshes;

	struct SkeletalMeshGPU {
		GLuint vao = 0;
		GLuint vbo = 0;
		GLuint ebo = 0;
		GLuint bone_ubo = 0;
		GLsizei vertex_count = 0;
		GLsizei index_count = 0;
		HRL_uint bone_count = 0;
		bool indexed = false;
		uint64_t uploaded_pose_serial = 0;
	};
	std::unordered_map<HRL_id, SkeletalMeshGPU> skeletal_meshes;

	struct LandscapeGPU {
		MeshLOD_GPU geometry;
		uint64_t revision = 0;
		HRL_id heightmap = HRL_INVALID_ID;
		glm::vec3 bounds_center{0.f};
		float bounds_radius = 0.f;
	};
	std::unordered_map<HRL_id, LandscapeGPU> landscapes;

	// Shared procedural sky sphere geometry. Scene-specific state stays in hrl_scene_t.
	GLuint sky_vao = 0;
	GLuint sky_vbo = 0;
	GLuint sky_ebo = 0;
	GLsizei sky_index_count = 0;
	GL33_Shader* sky_shader = nullptr;

	// Shared sprite plane + per-instance stream. All normal sprites use this
	// geometry so compatible sprites can be submitted with instancing.
	GLuint sprite_vao = 0;
	GLuint sprite_instanced_vao = 0;
	GLuint sprite_vbo = 0;
	GLuint sprite_ebo = 0;
	GLuint sprite_instance_vbo = 0;
	size_t sprite_instance_capacity = 0;

	// Niagara-like VFX billboards. Particles remain CPU-side simulation data;
	// this stream only contains the current frame render instances.
	GLuint vfx_vao = 0;
	GLuint vfx_vbo = 0;
	GLuint vfx_ebo = 0;
	GLuint vfx_instance_vbo = 0;
	size_t vfx_instance_capacity = 0;
	GL33_Shader* vfx_shader = nullptr;

	GL33_Shader* ss_displacement_static_shader = nullptr;
	GL33_Shader* ss_displacement_skinned_shader = nullptr;

	GL33_Shader* shadow_2d_shader = nullptr;
	GL33_Shader* shadow_point_shader = nullptr;
	GL33_Shader* shadow_skeletal_2d_shader = nullptr;
	GL33_Shader* shadow_skeletal_point_shader = nullptr;

	struct ShadowGPU {
		GLuint fbo = 0;
		GLuint depth_2d = 0;
		GLuint depth_cube = 0;
		int resolution = 0;
		bool is_point = false;
	};
	std::unordered_map<HRL_id, ShadowGPU> shadow_maps;
	std::unordered_map<HRL_id, std::unordered_map<HRL_id, int>> active_shadow_slots_by_scene;

	int antialiasing_samples = 1;

	std::unordered_map<int, HRL_id> fallback_textures;

	//post process pass (ping pong method)
	GLuint post_fbo[2];
	GLuint post_textures[2];

	GL33_Shader* scene_effect_shader = nullptr;
	GL33_Shader* volumetric_cloud_shader = nullptr;
	GL33_Shader* ambient_occlusion_shader = nullptr;
	GL33_Shader* ssr_shader = nullptr;
	GL33_Shader* decal_shader = nullptr;

	//Widgets
	GL33_Shader* ui_shader=nullptr;

	// Cached SDF text textures used by the mesh-info debug overlay.
	struct DebugMeshInfoTextGPU {
		HRL_id texture = HRL_INVALID_ID;
		HRL_id font = HRL_INVALID_ID;
		float size = 0.0f;
		std::string text;
		int width = 0;
		int height = 0;
	};
	std::unordered_map<HRL_id, DebugMeshInfoTextGPU> debug_mesh_info_textures;
};
static GL33_Backend* bck_;
static GL_33_GI* g_gl33_gi = nullptr;



//Render context, used and updated every frame
typedef struct {
	//render context
	HRL_Viewport* viewport;
	GL33_Shader* shader;

	//cached matrices
	glm::mat4 proj_mat;
	glm::mat4 view_mat;

	//fog
	hrl_fog_t* current_fog;

	//current scene for scene-scoped environment/shadow resources
	hrl_scene_t* current_scene = nullptr;

	//debug
	size_t current_debug_buffer_size=0;

	// Material/shader cache used by renderer-side state batching.
	HRL_Material* bound_material = nullptr;
	GL33_Shader* bound_shader = nullptr;
} GL33_State;
static GL33_State* ctx_;

static void ApplyVolumetricCloudsPass(GL_Scene* scene, const hrl_scene_t* hrlScene,
    const glm::mat4& view, const glm::mat4& projection,
    int viewportX, int viewportY, int viewportWidth, int viewportHeight, GLuint& srcIndex)
{
    if (!scene || !hrlScene || !hrlScene->volumetric_cloud_enabled || !bck_->volumetric_cloud_shader || !scene->depth_texture)
        return;

    const int src = (srcIndex == bck_->post_textures[0]) ? 0 : 1;
    const int dst = 1 - src;

    glBindFramebuffer(GL_FRAMEBUFFER, bck_->post_fbo[dst]);
    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    bck_->volumetric_cloud_shader->Use();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bck_->post_textures[src]);
    bck_->volumetric_cloud_shader->SetInt("uScene", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scene->depth_texture);
    bck_->volumetric_cloud_shader->SetInt("uDepth", 1);

    bck_->volumetric_cloud_shader->SetMat4("uInvViewProjection", glm::inverse(projection * view));
    bck_->volumetric_cloud_shader->SetVec3("uCameraPos", ctx_->viewport->camera_->position_);
    bck_->volumetric_cloud_shader->SetVec2("uViewportOrigin", glm::vec2(
        (float)viewportX / std::max(1, scene->width),
        (float)viewportY / std::max(1, scene->height)));
    bck_->volumetric_cloud_shader->SetVec2("uViewportSize", glm::vec2(
        (float)viewportWidth / std::max(1, scene->width),
        (float)viewportHeight / std::max(1, scene->height)));
    bck_->volumetric_cloud_shader->SetVec2("uScreenSize", glm::vec2((float)scene->width, (float)scene->height));

    bck_->volumetric_cloud_shader->SetInt("uEnabled", 1);
    bck_->volumetric_cloud_shader->SetFloat("uCoverage", hrlScene->volumetric_cloud_coverage);
    bck_->volumetric_cloud_shader->SetFloat("uDensity", hrlScene->volumetric_cloud_density);
    bck_->volumetric_cloud_shader->SetFloat("uHeightMin", hrlScene->volumetric_cloud_height_min);
    bck_->volumetric_cloud_shader->SetFloat("uHeightMax", hrlScene->volumetric_cloud_height_max);
    bck_->volumetric_cloud_shader->SetFloat("uScale", hrlScene->volumetric_cloud_scale);
    bck_->volumetric_cloud_shader->SetFloat("uDetail", hrlScene->volumetric_cloud_detail);
    bck_->volumetric_cloud_shader->SetVec2("uWind", hrlScene->volumetric_cloud_wind);
    bck_->volumetric_cloud_shader->SetFloat("uWindSpeed", hrlScene->volumetric_cloud_wind_speed);
    bck_->volumetric_cloud_shader->SetVec3("uCloudColor", hrlScene->volumetric_cloud_color);
    bck_->volumetric_cloud_shader->SetVec3("uLightColor", hrlScene->volumetric_cloud_light_color);
    bck_->volumetric_cloud_shader->SetFloat("uLightAbsorption", hrlScene->volumetric_cloud_light_absorption);
    bck_->volumetric_cloud_shader->SetFloat("uLightIntensity", hrlScene->volumetric_cloud_light_intensity);
    bck_->volumetric_cloud_shader->SetInt("uSteps", (int)hrlScene->volumetric_cloud_steps);
    bck_->volumetric_cloud_shader->SetFloat("uMaxDistance", hrlScene->volumetric_cloud_max_distance);

    glm::vec3 sunDirection = glm::normalize(glm::vec3(0.35f, -0.85f, 0.2f));
    glm::vec3 lightColor = hrlScene->volumetric_cloud_light_color;
    float lightIntensity = hrlScene->volumetric_cloud_light_intensity;
    for (const auto& [lightId, light] : hrlScene->lights)
    {
        (void)lightId;
        if (!light || light->type_ != HRL_DIRECTIONAL_LIGHT || light->intensity_ <= 0.0f)
            continue;
        sunDirection = GetLightDirection(light);
        lightColor = light->color_;
        lightIntensity *= std::max(0.0f, light->intensity_);
        break;
    }
    bck_->volumetric_cloud_shader->SetVec3("uSunDirection", sunDirection);
    bck_->volumetric_cloud_shader->SetVec3("uLightColor", lightColor);
    bck_->volumetric_cloud_shader->SetFloat("uLightIntensity", lightIntensity);

    static const auto cloudStartTime = std::chrono::steady_clock::now();
    const float time = static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now() - cloudStartTime).count());
    bck_->volumetric_cloud_shader->SetFloat("uTime", time);

    glBindVertexArray(bck_->vao[BUFFER_QUAD]);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    srcIndex = bck_->post_textures[dst];
}


static const char* kVFXVertexShader = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in mat4 aInstanceModel;
layout(location = 7) in vec4 aInstanceColor;

uniform mat4 projection;
uniform mat4 view;
uniform mat4 model;
uniform int uInstanced;
uniform vec4 uTintColor;

out vec2 uv;
out vec4 tint;

void main()
{
    uv = aTexCoord;
    tint = (uInstanced != 0) ? aInstanceColor : uTintColor;
    mat4 m = (uInstanced != 0) ? aInstanceModel : model;
    gl_Position = projection * view * m * vec4(aPosition, 1.0);
}
)GLSL";

static const char* kVFXFragmentShader = R"GLSL(
#version 330 core
in vec2 uv;
in vec4 tint;

uniform sampler2D uTexture;
uniform int uUseTexture;

out vec4 FragColor;

void main()
{
    vec4 texel = (uUseTexture != 0) ? texture(uTexture, uv) : vec4(1.0);
    vec4 result = texel * tint;
    if (result.a <= 0.001) discard;
    FragColor = result;
}
 )GLSL";

static const char* kSSDisplacementFragmentShader = R"GLSL(
#version 330 core
layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 BrightColor;
layout(location = 2) out vec4 ColorPickingBuffer;
layout(location = 3) out vec4 GIAlbedoBuffer;
layout(location = 4) out vec4 GINormalBuffer;

in vec2 uv;
flat in uint sprite_id;

uniform sampler2D uScene;
uniform sampler2D uDisplacementMap;
uniform vec2 uScreenSize;
uniform float uStrength;
uniform float uScale;
uniform float uOpacity;
uniform int ss_displacement_enabled;

void main()
{
    vec2 screenSize = max(uScreenSize, vec2(1.0));
    if (ss_displacement_enabled == 0)
    {
        vec4 baseScene = texture(uScene, gl_FragCoord.xy / screenSize);
        FragColor = baseScene;
        BrightColor = vec4(0.0);
        GIAlbedoBuffer = vec4(0.0);
        GINormalBuffer = vec4(0.0);
        ColorPickingBuffer = vec4(0.0);
        return;
    }
    vec2 baseUV = gl_FragCoord.xy / screenSize;
    vec2 mapUV = uv * max(abs(uScale), 0.0001);
    vec2 displacement = texture(uDisplacementMap, mapUV).rg * 2.0 - 1.0;
    vec2 displacedUV = clamp(baseUV + displacement * uStrength / screenSize, vec2(0.0), vec2(1.0));
    vec4 baseScene = texture(uScene, baseUV);
    vec4 displacedScene = texture(uScene, displacedUV);
    float opacity = clamp(uOpacity, 0.0, 1.0);
    FragColor = vec4(mix(baseScene.rgb, displacedScene.rgb, opacity), baseScene.a);
    BrightColor = vec4(0.0);
    GIAlbedoBuffer = vec4(0.0);
    GINormalBuffer = vec4(0.0);

    uint id = sprite_id;
    ColorPickingBuffer = vec4(
        float((id >> 16u) & 255u) / 255.0,
        float((id >> 8u) & 255u) / 255.0,
        float(id & 255u) / 255.0,
        1.0
    );
}
)GLSL";

static const char* kAmbientOcclusionFragmentShader = R"GLSL(
#version 330 core
in vec2 uv;
out vec4 frag_color;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
uniform mat4 uInvViewProjection;
uniform vec3 uCameraPos;
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uScreenSize;
uniform float uRadius;
uniform float uBias;
uniform float uStrength;
uniform float uPower;
vec2 GlobalUV(vec2 localUV) { return uViewportOrigin + localUV * uViewportSize; }
vec3 ReconstructWorld(vec2 globalUV, float depth)
{
    vec2 localUV = (globalUV - uViewportOrigin) / max(uViewportSize, vec2(1e-6));
    vec4 clip = vec4(localUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProjection * clip;
    return abs(world.w) < 1e-6 ? vec3(0.0) : world.xyz / world.w;
}
void main()
{
    vec2 globalUV = GlobalUV(uv);
    vec4 base = texture(uScene, globalUV);
    float depth = texture(uDepth, globalUV).r;
    vec3 normal = texture(uNormal, globalUV).xyz;
    if (depth >= 0.99999 || dot(normal, normal) < 0.01 || uStrength <= 0.0 || uRadius <= 0.0) { frag_color = base; return; }
    normal = normalize(normal);
    vec3 worldPos = ReconstructWorld(globalUV, depth);
    const vec2 kernel[16] = vec2[](
        vec2(1.0,0.0), vec2(-1.0,0.0), vec2(0.0,1.0), vec2(0.0,-1.0),
        vec2(0.707,0.707), vec2(-0.707,0.707), vec2(0.707,-0.707), vec2(-0.707,-0.707),
        vec2(0.382,0.924), vec2(-0.382,0.924), vec2(0.382,-0.924), vec2(-0.382,-0.924),
        vec2(0.924,0.382), vec2(-0.924,0.382), vec2(0.924,-0.382), vec2(-0.924,-0.382));
    float distanceToCamera = max(length(worldPos - uCameraPos), 1.0);
    float radiusPixels = clamp(uRadius * 0.5 * min(uScreenSize.x, uScreenSize.y) / distanceToCamera, 1.0, 32.0);
    float occlusion = 0.0;
    for (int i=0; i<16; ++i)
    {
        float scale = mix(0.35, 1.0, float(i)/15.0);
        vec2 suv = globalUV + kernel[i] * (radiusPixels*scale) / uScreenSize;
        if (any(lessThan(suv,uViewportOrigin)) || any(greaterThan(suv,uViewportOrigin+uViewportSize))) continue;
        float sd = texture(uDepth,suv).r;
        if (sd >= 0.99999) continue;
        vec3 sp = ReconstructWorld(suv,sd);
        vec3 d = sp-worldPos;
        float dist = length(d);
        if (dist <= 1e-5 || dist > uRadius) continue;
        float farther = smoothstep(uBias, uBias + max(0.01,uRadius*0.08), length(sp - uCameraPos)-length(worldPos - uCameraPos));
        float facing = max(dot(normal, normalize(-d)),0.0);
        float rangeWeight = 1.0-smoothstep(0.0,uRadius,dist);
        occlusion += farther*facing*rangeWeight;
    }
    float ao = 1.0-uStrength*pow(clamp(occlusion/16.0,0.0,1.0),uPower);
    frag_color=vec4(base.rgb*ao,base.a);
}
)GLSL";

static const char* kVolumetricCloudFragmentShader = R"GLSL(
#version 330 core
in vec2 uv;
out vec4 frag_color;

uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform mat4 uInvViewProjection;
uniform vec3 uCameraPos;
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uScreenSize;

uniform int uEnabled;
uniform float uCoverage;
uniform float uDensity;
uniform float uHeightMin;
uniform float uHeightMax;
uniform float uScale;
uniform float uDetail;
uniform vec2 uWind;
uniform float uWindSpeed;
uniform vec3 uCloudColor;
uniform vec3 uLightColor;
uniform float uLightAbsorption;
uniform float uLightIntensity;
uniform int uSteps;
uniform float uTime;
uniform float uMaxDistance;
uniform vec3 uSunDirection;

vec2 ToGlobalUV(vec2 localUV)
{
    return uViewportOrigin + localUV * uViewportSize;
}

vec3 Unproject(vec2 globalUV, float depth)
{
    vec2 localUV = (globalUV - uViewportOrigin) / max(uViewportSize, vec2(1e-6));
    vec4 clip = vec4(localUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProjection * clip;
    return abs(world.w) < 1e-6 ? uCameraPos : world.xyz / world.w;
}

float Hash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float Noise3D(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f*f*(3.0-2.0*f);
    float n000 = Hash(i + vec3(0,0,0));
    float n100 = Hash(i + vec3(1,0,0));
    float n010 = Hash(i + vec3(0,1,0));
    float n110 = Hash(i + vec3(1,1,0));
    float n001 = Hash(i + vec3(0,0,1));
    float n101 = Hash(i + vec3(1,0,1));
    float n011 = Hash(i + vec3(0,1,1));
    float n111 = Hash(i + vec3(1,1,1));
    float nx00 = mix(n000,n100,f.x);
    float nx10 = mix(n010,n110,f.x);
    float nx01 = mix(n001,n101,f.x);
    float nx11 = mix(n011,n111,f.x);
    return mix(mix(nx00,nx10,f.y),mix(nx01,nx11,f.y),f.z);
}

float FBM3(vec3 p)
{
    float value = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i=0; i<3; ++i)
    {
        value += Noise3D(p * freq) * amp;
        freq *= 2.03;
        amp *= 0.5;
        p += vec3(19.1, 7.7, 31.3);
    }
    return value;
}

float FBM2(vec3 p)
{
    float value = 0.0;
    float amp = 0.6;
    float freq = 1.0;
    for (int i=0; i<2; ++i)
    {
        value += Noise3D(p * freq) * amp;
        freq *= 2.03;
        amp *= 0.5;
        p += vec3(19.1, 7.7, 31.3);
    }
    return value;
}

float CloudDensity(vec3 p)
{
    float range = max(uHeightMax - uHeightMin, 0.001);
    float h = clamp((p.y - uHeightMin) / range, 0.0, 1.0);
    if (h <= 0.0 || h >= 1.0) return 0.0;

    float vertical = smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(0.78, 1.0, h));
    vec3 wind = vec3(uWind.x, 0.0, uWind.y) * (uWindSpeed * uTime);
    vec3 q = p * max(uScale, 1e-5) + wind;

    float base = FBM3(q);
    float detail = Noise3D(q * 3.1 + vec3(17.0, 5.0, 29.0));
    float shape = mix(base, base * 0.82 + detail * 0.30, clamp(uDetail, 0.0, 1.0));

    // Coverage is deliberately centered around the useful visible range so
    // the documented default (0.58) produces obvious clouds.
    float threshold = mix(0.36, 0.72, clamp(uCoverage, 0.0, 1.0));
    float d = smoothstep(threshold - 0.08, threshold + 0.12, shape);
    return d * vertical * max(uDensity, 0.0);
}

bool IntersectLayer(vec3 ro, vec3 rd, out float t0, out float t1)
{
    float minH = min(uHeightMin, uHeightMax);
    float maxH = max(uHeightMin, uHeightMax);
    if (abs(rd.y) < 1e-5)
        return (ro.y > minH && ro.y < maxH);

    float a = (minH - ro.y) / rd.y;
    float b = (maxH - ro.y) / rd.y;
    t0 = max(min(a,b), 0.0);
    t1 = max(a,b);
    return t1 > t0 + 1e-4;
}

float LightVisibility(vec3 p, vec3 lightDir, float sampleStep)
{
    float opticalDepth = 0.0;
    float stepLen = max(sampleStep * 2.0, (uHeightMax-uHeightMin)*0.025);
    vec3 q = p;
    for (int i=0; i<3; ++i)
    {
        q += lightDir * stepLen;
        opticalDepth += CloudDensity(q) * stepLen;
        if (opticalDepth > 3.0) break;
    }
    return exp(-opticalDepth * max(uLightAbsorption, 0.0));
}

void main()
{
    vec2 globalUV = ToGlobalUV(uv);
    vec4 base = texture(uScene, globalUV);
    if (uEnabled == 0 || uDensity <= 0.0 || uHeightMax <= uHeightMin)
    {
        frag_color = base;
        return;
    }

    vec3 nearWorld = Unproject(globalUV, 0.0);
    vec3 farWorld  = Unproject(globalUV, 1.0);
    vec3 rayDir = normalize(farWorld - nearWorld);
    vec3 rayOrigin = nearWorld;
    float raySpan = length(farWorld - nearWorld);
    if (raySpan <= 1e-4)
    {
        frag_color = base;
        return;
    }

    float tSurface = raySpan;
    float depth = texture(uDepth, globalUV).r;
    if (depth < 0.99999)
    {
        vec3 surfaceWorld = Unproject(globalUV, depth);
        tSurface = clamp(dot(surfaceWorld - rayOrigin, rayDir), 0.0, raySpan);
    }
    tSurface = min(tSurface, max(uMaxDistance, 1.0));

    float tEnter, tExit;
    if (!IntersectLayer(rayOrigin, rayDir, tEnter, tExit))
    {
        frag_color = base;
        return;
    }
    tExit = min(tExit, tSurface);
    if (tExit <= tEnter + 1e-4)
    {
        frag_color = base;
        return;
    }

    float cloudDistance = tExit - tEnter;
    int requestedSteps = clamp(uSteps, 12, 96);
    float stepFactor = clamp(cloudDistance / 220.0, 0.0, 1.0);
    int steps = clamp(int(mix(12.0, float(requestedSteps), stepFactor)), 12, requestedSteps);
    float stepLen = cloudDistance / float(steps);
    float jitter = Hash(vec3(globalUV * uScreenSize, uTime)) - 0.5;
    float t = tEnter + stepLen * (0.5 + jitter * 0.65);

    vec3 lightDir = normalize(-uSunDirection);
    vec3 accum = vec3(0.0);
    float trans = 1.0;

    for (int i=0; i<96; ++i)
    {
        if (i >= steps || trans < 0.02) break;
        vec3 p = rayOrigin + rayDir * t;
        float density = CloudDensity(p);
        if (density > 0.001)
        {
            float lightT = (density > 0.015 && uLightIntensity > 0.001)
                ? LightVisibility(p, lightDir, stepLen)
                : 1.0;
            float phase = 0.55 + 0.45 * max(dot(-rayDir, lightDir), 0.0);
            float height = clamp((p.y-uHeightMin)/max(uHeightMax-uHeightMin,1e-4),0.0,1.0);
            float silver = 0.75 + 0.25 * pow(max(height, 1e-3), 0.2);
            float alpha = 1.0 - exp(-density * stepLen);
            vec3 lit = uCloudColor * uLightColor;
            lit *= 0.35 + lightT * phase * silver * max(uLightIntensity,0.0);
            accum += trans * alpha * lit;
            trans *= 1.0 - alpha;
        }
        t += stepLen;
    }

    frag_color = vec4(base.rgb * trans + accum, base.a);
}
)GLSL";

static const char* kSceneEffectsFragmentShader = R"GLSL(
#version 330 core
in vec2 uv;
out vec4 frag_color;
uniform sampler2D uScene;
uniform sampler2D uBrightScene;
uniform sampler2D uDepth;
uniform mat4 uInvViewProjection;
uniform vec3 uCameraPos;
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uScreenSize;

#define HRL_MAX_VOLUMETRIC_FOGS 64
uniform int uVolumetricFogCount;
uniform vec3 uFogPositions[HRL_MAX_VOLUMETRIC_FOGS];
uniform float uFogRadii[HRL_MAX_VOLUMETRIC_FOGS];
uniform vec3 uFogColors[HRL_MAX_VOLUMETRIC_FOGS];
uniform float uFogDensities[HRL_MAX_VOLUMETRIC_FOGS];
uniform int uVolumetricFogSteps;

uniform int uGlobalVolumetricFogEnabled;
uniform vec3 uGlobalFogColor;
uniform float uGlobalFogDensity;
uniform int uGlobalFogSteps;

uniform int uGodRaysEnabled;
uniform vec2 uGodRaysLightUV;
uniform vec3 uGodRaysColor;
uniform float uGodRaysDensity;
uniform float uGodRaysDecay;
uniform float uGodRaysWeight;
uniform int uGodRaysSamples;

// Volumetric cloud layer. The implementation is fully procedural so it does not
// require a 3D texture and remains compatible with OpenGL 3.3.
uniform int uVolumetricCloudEnabled;
uniform float uCloudCoverage;
uniform float uCloudDensity;
uniform float uCloudHeightMin;
uniform float uCloudHeightMax;
uniform float uCloudScale;
uniform float uCloudDetail;
uniform vec2 uCloudWind;
uniform float uCloudWindSpeed;
uniform vec3 uCloudColor;
uniform vec3 uCloudLightColor;
uniform float uCloudLightAbsorption;
uniform float uCloudLightIntensity;
uniform int uCloudSteps;
uniform float uCloudTime;
uniform float uCloudMaxDistance;
uniform vec3 uCloudSunDirection;

vec2 GlobalUV(vec2 localUV) { return uViewportOrigin + localUV * uViewportSize; }
vec3 ReconstructWorld(vec2 localUV, float depth)
{
    vec4 clip = vec4(localUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProjection * clip;
    if (abs(world.w) < 1e-6) return uCameraPos;
    return world.xyz / world.w;
}

vec4 ApplyVolumetricFog(vec2 globalUV, vec3 worldEnd)
{
    vec4 base = texture(uScene, globalUV);

    bool hasGlobalFog = uGlobalVolumetricFogEnabled != 0 && uGlobalFogDensity > 0.0;
    bool hasLocalFog = uVolumetricFogCount > 0;
    if (!hasGlobalFog && !hasLocalFog)
        return base;

    vec3 ray = worldEnd - uCameraPos;
    float rayLength = length(ray);
    if (rayLength <= 1e-4)
        return base;

    vec3 direction = ray / rayLength;
    int steps = clamp(max(uGlobalFogSteps, uVolumetricFogSteps), 4, 64);
    float stepLength = rayLength / float(steps);
    float transmittance = 1.0;
    vec3 scattering = vec3(0.0);

    for (int i = 0; i < 64; ++i)
    {
        if (i >= steps) break;

        float t = (float(i) + 0.5) / float(steps);
        vec3 samplePos = uCameraPos + direction * (rayLength * t);

        float totalDensity = 0.0;
        vec3 weightedColor = vec3(0.0);

        if (hasGlobalFog)
        {
            totalDensity += uGlobalFogDensity;
            weightedColor += uGlobalFogDensity * uGlobalFogColor;
        }

        for (int fogIndex = 0; fogIndex < HRL_MAX_VOLUMETRIC_FOGS; ++fogIndex)
        {
            if (fogIndex >= uVolumetricFogCount) break;

            float radius = uFogRadii[fogIndex];
            float density = uFogDensities[fogIndex];
            if (radius <= 0.0 || density <= 0.0) continue;

            float normalized = 1.0 - length(samplePos - uFogPositions[fogIndex]) / radius;
            if (normalized <= 0.0) continue;

            float localDensity = normalized * normalized * density;
            totalDensity += localDensity;
            weightedColor += localDensity * uFogColors[fogIndex];
        }

        if (totalDensity <= 0.0)
            continue;

        float alpha = 1.0 - exp(-totalDensity * stepLength);
        vec3 fogColor = weightedColor / totalDensity;
        scattering += transmittance * alpha * fogColor;
        transmittance *= 1.0 - alpha;
        if (transmittance < 0.01) break;
    }

    return vec4(base.rgb * transmittance + scattering, base.a);
}

float CloudHash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float CloudNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = CloudHash(i + vec3(0,0,0));
    float n100 = CloudHash(i + vec3(1,0,0));
    float n010 = CloudHash(i + vec3(0,1,0));
    float n110 = CloudHash(i + vec3(1,1,0));
    float n001 = CloudHash(i + vec3(0,0,1));
    float n101 = CloudHash(i + vec3(1,0,1));
    float n011 = CloudHash(i + vec3(0,1,1));
    float n111 = CloudHash(i + vec3(1,1,1));

    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);
    return mix(nxy0, nxy1, f.z);
}

float CloudFBM3(vec3 p)
{
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    for (int i = 0; i < 3; ++i)
    {
        value += CloudNoise(p * frequency) * amplitude;
        frequency *= 2.03;
        amplitude *= 0.5;
        p += vec3(19.1, 7.7, 31.3);
    }
    return value;
}

float CloudFBM2(vec3 p)
{
    float value = 0.0;
    float amplitude = 0.65;
    float frequency = 1.0;
    for (int i = 0; i < 2; ++i)
    {
        value += CloudNoise(p * frequency) * amplitude;
        frequency *= 2.03;
        amplitude *= 0.5;
        p += vec3(19.1, 7.7, 31.3);
    }
    return value / 0.975;
}

float CloudHeightShape(vec3 p)
{
    float range = max(uCloudHeightMax - uCloudHeightMin, 0.001);
    float h = clamp((p.y - uCloudHeightMin) / range, 0.0, 1.0);
    if (h <= 0.0 || h >= 1.0) return 0.0;
    return smoothstep(0.0, 0.12, h) * (1.0 - smoothstep(0.76, 1.0, h));
}

vec3 CloudWindPosition(vec3 p)
{
    vec3 windOffset = vec3(uCloudWind.x, 0.0, uCloudWind.y) * (uCloudWindSpeed * uCloudTime);
    return p * max(uCloudScale, 0.00001) + windOffset;
}

float CloudDensity(vec3 worldPos)
{
    float heightShape = CloudHeightShape(worldPos);
    if (heightShape <= 0.0) return 0.0;

    vec3 q = CloudWindPosition(worldPos);
    float base = CloudFBM3(q);
    // One extra noise lookup for detail instead of a second multi-octave FBM.
    float detail = CloudNoise(q * 3.1 + vec3(5.7, 13.1, 2.9));
    float detailAmount = clamp(uCloudDetail, 0.0, 1.0);
    float shape = mix(base, base * 0.82 + detail * 0.30, detailAmount);

    float threshold = clamp(uCloudCoverage, 0.0, 1.0);
    float d = smoothstep(threshold - 0.055, min(0.999, threshold + 0.16), shape);
    return d * heightShape * max(uCloudDensity, 0.0);
}

float CloudLightDensity(vec3 worldPos)
{
    float heightShape = CloudHeightShape(worldPos);
    if (heightShape <= 0.0) return 0.0;

    vec3 q = CloudWindPosition(worldPos);
    float shape = CloudFBM2(q);
    float threshold = clamp(uCloudCoverage, 0.0, 1.0);
    float d = smoothstep(threshold - 0.08, min(0.999, threshold + 0.20), shape);
    return d * heightShape * max(uCloudDensity, 0.0);
}

float CloudLightTransmittance(vec3 startPos, vec3 towardLight, float sampleStep)
{
    float opticalDepth = 0.0;
    float layerHeight = max(uCloudHeightMax - uCloudHeightMin, 1.0);
    float lightStep = max(sampleStep * 3.0, layerHeight * 0.065);
    vec3 p = startPos;
    for (int i = 0; i < 2; ++i)
    {
        p += towardLight * lightStep;
        opticalDepth += CloudLightDensity(p) * lightStep;
    }
    return exp(-opticalDepth * max(uCloudLightAbsorption, 0.0));
}

bool CloudIntersectLayer(vec3 rayOrigin, vec3 rayDirection, out float tEnter, out float tExit)
{
    if (abs(rayDirection.y) < 1e-5)
    {
        tEnter = 0.0;
        tExit = 0.0;
        return rayOrigin.y > uCloudHeightMin && rayOrigin.y < uCloudHeightMax;
    }

    float a = (uCloudHeightMin - rayOrigin.y) / rayDirection.y;
    float b = (uCloudHeightMax - rayOrigin.y) / rayDirection.y;
    tEnter = max(min(a, b), 0.0);
    tExit = max(a, b);
    return tExit > tEnter + 1e-4;
}

vec4 ApplyVolumetricClouds(vec2 globalUV, vec2 localUV, float sceneDepth, vec4 base)
{
    if (uVolumetricCloudEnabled == 0 || uCloudDensity <= 0.0 || uCloudHeightMax <= uCloudHeightMin)
        return base;

    vec3 nearWorld = ReconstructWorld(localUV, 0.0);
    vec3 farWorld = ReconstructWorld(localUV, 1.0);
    vec3 rayVector = farWorld - nearWorld;
    float raySpan = length(rayVector);
    if (raySpan <= 1e-4)
        return base;

    vec3 rayDirection = rayVector / raySpan;
    vec3 rayOrigin = nearWorld;
    float sceneDistance = raySpan;

    if (sceneDepth < 0.99999)
    {
        vec3 worldSurface = ReconstructWorld(localUV, sceneDepth);
        sceneDistance = clamp(dot(worldSurface - rayOrigin, rayDirection), 0.0, raySpan);
    }
    sceneDistance = min(sceneDistance, max(uCloudMaxDistance, 1.0));

    float tEnter, tExit;
    if (!CloudIntersectLayer(rayOrigin, rayDirection, tEnter, tExit))
        return base;

    tExit = min(tExit, sceneDistance);
    if (tExit <= tEnter + 1e-4)
        return base;

    float cloudDistance = tExit - tEnter;
    int requestedSteps = clamp(uCloudSteps, 8, 96);
    float stepFactor = clamp(cloudDistance / 220.0, 0.0, 1.0);
    int steps = clamp(int(mix(8.0, float(requestedSteps), stepFactor)), 8, requestedSteps);
    float stepLength = cloudDistance / float(steps);

    float jitter = Hash(vec3(globalUV * uScreenSize, uCloudTime)) - 0.5;
    float t = tEnter + stepLength * (0.5 + jitter * 0.65);
    vec3 lightDir = normalize(-uCloudSunDirection);

    vec3 cloudScattering = vec3(0.0);
    float transmittance = 1.0;

    for (int i = 0; i < 96; ++i)
    {
        if (i >= steps || transmittance < 0.025)
            break;

        vec3 samplePos = rayOrigin + rayDirection * t;
        float density = CloudDensity(samplePos);
        if (density > 0.001)
        {
            float lightT = 1.0;
            if (density > 0.015 && uCloudLightIntensity > 0.001)
                lightT = CloudLightTransmittance(samplePos, lightDir, stepLength);

            float phase = 0.55 + 0.45 * max(dot(-rayDirection, lightDir), 0.0);
            float height01 = clamp((samplePos.y - uCloudHeightMin) / max(uCloudHeightMax - uCloudHeightMin, 1e-4), 0.0, 1.0);
            float silver = 0.65 + 0.35 * pow(max(height01, 1e-3), 0.2);
            float alpha = 1.0 - exp(-density * stepLength);
            vec3 lit = uCloudColor * (0.55 + lightT * phase * silver * max(uCloudLightIntensity, 0.0));
            lit *= uCloudLightColor;

            cloudScattering += transmittance * alpha * lit;
            transmittance *= 1.0 - alpha;
        }
        t += stepLength;
    }

    return vec4(base.rgb * transmittance + cloudScattering, base.a);
}

void main()
{
    vec2 globalUV = GlobalUV(uv);
    vec4 color = texture(uScene, globalUV);
    float depth = texture(uDepth, globalUV).r;
    color = ApplyVolumetricFog(globalUV, ReconstructWorld(uv, depth));
    color = ApplyGodRays(globalUV, color);
    frag_color = color;
}
)GLSL";

static const char* kSSRFragmentShader = R"GLSL(
#version 330 core
in vec2 uv;
out vec4 frag_color;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
uniform mat4 uViewProjection;
uniform mat4 uInvViewProjection;
uniform vec3 uCameraPos;
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uScreenSize;
uniform float uStrength;
uniform float uMaxDistance;
uniform float uThickness;
uniform float uFadeStart;
uniform float uFadeEnd;
uniform int uSteps;

vec2 GlobalUV(vec2 localUV) { return uViewportOrigin + localUV * uViewportSize; }
vec3 ReconstructWorld(vec2 globalUV, float depth)
{
    vec4 clip = vec4(globalUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProjection * clip;
    if (abs(world.w) < 1e-6) return uCameraPos;
    return world.xyz / world.w;
}
vec2 ProjectUV(vec3 world)
{
    vec4 clip = uViewProjection * vec4(world,1.0);
    if (clip.w <= 1e-5) return vec2(-1.0);
    return clip.xy / clip.w * 0.5 + 0.5;
}
void main()
{
    vec2 globalUV = GlobalUV(uv);
    vec4 base = texture(uScene, globalUV);
    float depth = texture(uDepth, globalUV).r;
    if (depth >= 0.99999 || uStrength <= 0.0 || uMaxDistance <= 0.0)
    {
        frag_color = base;
        return;
    }

    vec4 normalSample = texture(uNormal, globalUV);
    if (normalSample.a <= 0.001)
    {
        frag_color = base;
        return;
    }
    vec3 N = normalize(normalSample.rgb);
    vec3 worldPos = ReconstructWorld(globalUV, depth);
    vec3 V = normalize(uCameraPos - worldPos);
    vec3 R = normalize(reflect(-V, N));
    if (dot(R, R) < 1e-6)
    {
        frag_color = base;
        return;
    }

    float cosTheta = max(dot(N, V), 0.0);
    float fresnel = pow(1.0 - cosTheta, 5.0);
    fresnel = mix(0.04, 1.0, fresnel);
    float stepLength = uMaxDistance / float(max(uSteps, 8));
    vec3 rayPos = worldPos + N * max(uThickness * 0.5, 0.001);
    vec3 hitColor = vec3(0.0);
    float confidence = 0.0;

    for (int i = 0; i < 96; ++i)
    {
        if (i >= uSteps) break;
        rayPos += R * stepLength;
        vec2 hitUV = ProjectUV(rayPos);
        if (any(lessThan(hitUV, vec2(0.001))) || any(greaterThan(hitUV, vec2(0.999))))
            break;
        float hitDepth = texture(uDepth, hitUV).r;
        if (hitDepth >= 0.99999)
            continue;
        vec3 hitWorld = ReconstructWorld(hitUV, hitDepth);
        float sceneDistance = length(hitWorld - uCameraPos);
        float rayDistance = length(rayPos - uCameraPos);
        float thickness = max(uThickness, stepLength * 1.5);
        if (sceneDistance <= rayDistance + thickness && sceneDistance >= length(worldPos-uCameraPos) + stepLength * 0.25)
        {
            hitColor = texture(uScene, hitUV).rgb;
            float distanceFade = 1.0 - smoothstep(uMaxDistance * uFadeStart, uMaxDistance * uFadeEnd, rayDistance);
            float edgeFade = 1.0 - smoothstep(0.0, 0.15, max(abs(hitUV.x-0.5), abs(hitUV.y-0.5)) * 2.0);
            confidence = clamp(uStrength * fresnel * distanceFade * edgeFade, 0.0, 1.0);
            break;
        }
    }

    frag_color = vec4(mix(base.rgb, hitColor, confidence), base.a);
}
)GLSL";

static const char* kDecalFragmentShader = R"GLSL(
#version 330 core
in vec2 uv;
out vec4 frag_color;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
uniform sampler2D uDecal;
uniform mat4 uInvViewProjection;
uniform mat4 uDecalInvModel;
uniform vec3 uDecalNormal;
uniform vec4 uColor;
uniform float uOpacity;
uniform float uNormalFadeMin;
uniform float uNormalFadeMax;
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uScreenSize;
vec2 GlobalUV(vec2 localUV) { return uViewportOrigin + localUV * uViewportSize; }
vec3 ReconstructWorld(vec2 globalUV, float depth)
{
    vec4 clip = vec4(globalUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProjection * clip;
    if (abs(world.w) < 1e-6) return vec3(0.0);
    return world.xyz / world.w;
}
void main()
{
    vec2 globalUV = GlobalUV(uv);
    vec4 base = texture(uScene, globalUV);
    float depth = texture(uDepth, globalUV).r;
    vec4 n = texture(uNormal, globalUV);
    if (depth >= 0.99999 || n.a <= 0.001 || uOpacity <= 0.0)
    { frag_color = base; return; }
    vec3 worldPos = ReconstructWorld(globalUV, depth);
    vec3 local = (uDecalInvModel * vec4(worldPos,1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5))))
    { frag_color = base; return; }
    vec3 surfaceNormal = normalize(n.rgb);
    float facing = abs(dot(surfaceNormal, normalize(uDecalNormal)));
    float facingFade = smoothstep(uNormalFadeMin, max(uNormalFadeMin + 0.001, uNormalFadeMax), facing);
    if (facingFade <= 0.001)
    { frag_color = base; return; }
    vec2 decalUV = local.xz + 0.5;
    vec4 decal = texture(uDecal, decalUV) * uColor;
    float alpha = clamp(decal.a * uOpacity * facingFade, 0.0, 1.0);
    frag_color = vec4(mix(base.rgb, decal.rgb, alpha), base.a);
}
)GLSL";

static void UploadSkeletalBones(HRL_SkeletalMesh* mesh, GL33_Backend::SkeletalMeshGPU& gpu)
{
	if (!mesh || gpu.bone_ubo == 0)
		return;
	if (gpu.uploaded_pose_serial == mesh->pose_serial_)
		return;

	const size_t boneCount = std::min(mesh->bone_matrices_.size(), (size_t)HRL_MAX_SKELETAL_BONES);
	glBindBuffer(GL_UNIFORM_BUFFER, gpu.bone_ubo);
	if (boneCount > 0)
	{
		glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)(boneCount * sizeof(glm::mat4)), mesh->bone_matrices_.data());
	}
	glBindBufferBase(GL_UNIFORM_BUFFER, 1, gpu.bone_ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	gpu.uploaded_pose_serial = mesh->pose_serial_;
}

static const GL33_Backend::MeshLOD_GPU* GetMeshLOD_GPU(const GL33_Backend::MeshGPU& gpu, int level)
{
	if (level < 0 || gpu.levels.empty())
		return nullptr;
	const size_t index = (size_t)level;
	if (index >= gpu.levels.size())
		return &gpu.levels.front();
	const auto& lod = gpu.levels[index];
	if (lod.vao == 0 || lod.vertex_count <= 0)
		return nullptr;
	return &lod;
}

static int SelectMeshLOD(const HRL_Mesh* mesh, const glm::mat4& model,
    const glm::mat4& view, const glm::mat4& projection, float viewportHeight)
{
	if (!mesh || !mesh->lod_automatic_ || mesh->lods_.size() <= 1)
		return 0;

	const int maxLevel = (int)mesh->lods_.size() - 1;
	if (mesh->lod_override_ >= 0)
		return std::clamp(mesh->lod_override_, 0, maxLevel);

	float metric = 0.0f;
	float transition0 = 0.0f;

	if (mesh->lod_mode_ == HRL_LOD_DISTANCE)
	{
		const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.0f));
		glm::vec3 cameraPos(0.0f);
		if (ctx_->viewport && ctx_->viewport->camera_)
			cameraPos = ctx_->viewport->camera_->position_;
		const float distance = glm::length(center - cameraPos);
		metric = distance;
		transition0 = mesh->lod_base_distance_;
	}
	else
	{
		const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.0f));
		const glm::vec4 viewCenter = view * glm::vec4(center, 1.0f);
		const float z = std::max(std::abs(viewCenter.z), 1e-4f);
		const float objectScale = std::max({
			std::abs(mesh->scale_.x),
			std::abs(mesh->scale_.y),
			std::abs(mesh->scale_.z)
		});
		const float radius = mesh->bounds_radius_ * std::max(objectScale, 1e-6f);
		const bool perspective = ctx_->viewport && ctx_->viewport->camera_ &&
			ctx_->viewport->camera_->type_ == HRL_PERSPECTIVE;
		metric = perspective
			? std::abs(projection[1][1]) * radius / z
			: std::abs(projection[1][1]) * radius;
		(void)viewportHeight;
		transition0 = mesh->lod_screen_threshold_;
	}

	int desired = maxLevel;
	float threshold = transition0;
	if (mesh->lod_mode_ == HRL_LOD_DISTANCE)
	{
		if (metric <= mesh->lod_min_distance_)
			desired = 0;
		else if (metric >= mesh->lod_max_distance_)
			desired = maxLevel;
		else
		{
			for (int level = 1; level <= maxLevel; ++level)
			{
				if (metric < threshold)
				{
					desired = level - 1;
					break;
				}
				threshold *= mesh->lod_distance_scale_;
			}
		}
	}
	else
	{
		for (int level = 1; level <= maxLevel; ++level)
		{
			if (metric >= threshold)
			{
				desired = level - 1;
				break;
			}
			threshold *= mesh->lod_screen_scale_;
		}
	}

	// Hysteresis keeps the selected LOD from oscillating when the camera hovers
	// around a transition boundary.
	const int current = std::clamp(mesh->last_lod_level_, 0, maxLevel);
	if (mesh->lod_hysteresis_ <= 0.0f || desired == current)
		return desired;

	if (desired > current)
	{
		// Moving toward a cheaper LOD.
		float boundary;
		if (mesh->lod_mode_ == HRL_LOD_DISTANCE)
			boundary = mesh->lod_base_distance_ * std::pow(mesh->lod_distance_scale_, (float)current);
		else
			boundary = mesh->lod_screen_threshold_ * std::pow(mesh->lod_screen_scale_, (float)current);

		const float switchPoint = mesh->lod_mode_ == HRL_LOD_DISTANCE
			? boundary * (1.0f + mesh->lod_hysteresis_)
			: boundary * (1.0f - mesh->lod_hysteresis_);
		const bool crossed = mesh->lod_mode_ == HRL_LOD_DISTANCE
			? metric >= switchPoint
			: metric <= switchPoint;
		return crossed ? desired : current;
	}

	// Moving toward a more detailed LOD.
	float boundary;
	if (mesh->lod_mode_ == HRL_LOD_DISTANCE)
		boundary = mesh->lod_base_distance_ * std::pow(mesh->lod_distance_scale_, (float)(desired));
	else
		boundary = mesh->lod_screen_threshold_ * std::pow(mesh->lod_screen_scale_, (float)(desired));
	const float switchPoint = mesh->lod_mode_ == HRL_LOD_DISTANCE
		? boundary * (1.0f - mesh->lod_hysteresis_)
		: boundary * (1.0f + mesh->lod_hysteresis_);
	const bool crossed = mesh->lod_mode_ == HRL_LOD_DISTANCE
		? metric <= switchPoint
		: metric >= switchPoint;
	return crossed ? desired : current;
}


struct SpriteBatchInstance
{
	glm::mat4 model;
	glm::vec4 uvRegion;
	uint32_t spriteId = 0;
	uint32_t padding[3] = {0u, 0u, 0u};
};

struct FrustumPlaneSet
{
	glm::vec4 planes[6];
};

static glm::vec4 NormalizePlane(const glm::vec4& p)
{
	const float len = glm::length(glm::vec3(p));
	return (len > 1e-6f) ? (p / len) : p;
}

static FrustumPlaneSet BuildFrustum(const glm::mat4& clip)
{
	// GLM matrices are column-major. Extract the rows explicitly from the
	// combined clip matrix so the plane equations are independent of layout.
	const glm::vec4 r0(clip[0][0], clip[1][0], clip[2][0], clip[3][0]);
	const glm::vec4 r1(clip[0][1], clip[1][1], clip[2][1], clip[3][1]);
	const glm::vec4 r2(clip[0][2], clip[1][2], clip[2][2], clip[3][2]);
	const glm::vec4 r3(clip[0][3], clip[1][3], clip[2][3], clip[3][3]);

	FrustumPlaneSet f{};
	f.planes[0] = NormalizePlane(r3 + r0); // left
	f.planes[1] = NormalizePlane(r3 - r0); // right
	f.planes[2] = NormalizePlane(r3 + r1); // bottom
	f.planes[3] = NormalizePlane(r3 - r1); // top
	f.planes[4] = NormalizePlane(r3 + r2); // near
	f.planes[5] = NormalizePlane(r3 - r2); // far
	return f;
}

static bool SphereInsideFrustum(const FrustumPlaneSet& frustum, const glm::vec3& center, float radius)
{
	for (const glm::vec4& p : frustum.planes)
	{
		if (glm::dot(glm::vec3(p), center) + p.w < -radius)
			return false;
	}
	return true;
}

static bool IsMeshVisible(const HRL_Mesh* mesh, const glm::mat4& model, const FrustumPlaneSet& frustum)
{
	if (!mesh || mesh->bounds_radius_ <= 0.f)
		return true;
	const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.f));
	const float maxScale = std::max({std::abs(mesh->scale_.x), std::abs(mesh->scale_.y), std::abs(mesh->scale_.z)});
	const float radius = mesh->bounds_radius_ * std::max(maxScale, 1e-6f);
	return SphereInsideFrustum(frustum, center, radius);
}

static bool IsFiniteBounds(const HRL_Mesh* mesh)
{
	return mesh && std::isfinite(mesh->bounds_center_.x) && std::isfinite(mesh->bounds_center_.y) &&
		std::isfinite(mesh->bounds_center_.z) && std::isfinite(mesh->bounds_radius_);
}

static void CreateSpriteGeometry()
{
	const HRL_Vertex3D vertices[4] = {
		{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{ 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}
	};
	const HRL_uint indices[6] = {0, 1, 2, 2, 3, 0};

	glGenVertexArrays(1, &bck_->sprite_vao);
	glGenVertexArrays(1, &bck_->sprite_instanced_vao);
	glGenBuffers(1, &bck_->sprite_vbo);
	glGenBuffers(1, &bck_->sprite_ebo);
	glGenBuffers(1, &bck_->sprite_instance_vbo);

	glBindVertexArray(bck_->sprite_vao);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->sprite_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	const GLsizei vertexStride = (GLsizei)sizeof(HRL_Vertex3D);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vertexStride, (const void*)offsetof(HRL_Vertex3D, position));
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, vertexStride, (const void*)offsetof(HRL_Vertex3D, uv));
	glEnableVertexAttribArray(2);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bck_->sprite_ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	glBindVertexArray(bck_->sprite_instanced_vao);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->sprite_vbo);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vertexStride, (const void*)offsetof(HRL_Vertex3D, position));
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, vertexStride, (const void*)offsetof(HRL_Vertex3D, uv));
	glEnableVertexAttribArray(2);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bck_->sprite_ebo);

	glBindBuffer(GL_ARRAY_BUFFER, bck_->sprite_instance_vbo);
	bck_->sprite_instance_capacity = sizeof(SpriteBatchInstance) * 256u;
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bck_->sprite_instance_capacity, nullptr, GL_STREAM_DRAW);
	const GLsizei instanceStride = (GLsizei)sizeof(SpriteBatchInstance);
	for (int column = 0; column < 4; ++column)
	{
		const GLuint location = (GLuint)(5 + column);
		glEnableVertexAttribArray(location);
		glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, instanceStride,
			(const void*)(offsetof(SpriteBatchInstance, model) + sizeof(glm::vec4) * (size_t)column));
		glVertexAttribDivisor(location, 1);
	}
	glEnableVertexAttribArray(9);
	glVertexAttribPointer(9, 4, GL_FLOAT, GL_FALSE, instanceStride, (const void*)offsetof(SpriteBatchInstance, uvRegion));
	glVertexAttribDivisor(9, 1);
	glEnableVertexAttribArray(10);
	glVertexAttribIPointer(10, 1, GL_UNSIGNED_INT, instanceStride, (const void*)offsetof(SpriteBatchInstance, spriteId));
	glVertexAttribDivisor(10, 1);

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GL33_Init()
{
	//empty with opengl
}

static void CreateSkySphereGeometry()
{
	constexpr int slices = 64;
	constexpr int stacks = 32;
	constexpr float pi = 3.14159265358979323846f;
	constexpr float twoPi = 6.28318530717958647692f;

	std::vector<glm::vec3> positions;
	positions.reserve((size_t)(slices + 1) * (size_t)(stacks + 1));

	for (int y = 0; y <= stacks; ++y)
	{
		const float v = (float)y / (float)stacks;
		const float phi = pi * v;
		const float sinPhi = std::sin(phi);
		const float cosPhi = std::cos(phi);

		for (int x = 0; x <= slices; ++x)
		{
			const float u = (float)x / (float)slices;
			const float theta = twoPi * u;
			positions.emplace_back(
				sinPhi * std::cos(theta),
				cosPhi,
				sinPhi * std::sin(theta));
		}
	}

	std::vector<HRL_uint> indices;
	indices.reserve((size_t)slices * (size_t)stacks * 6u);
	for (int y = 0; y < stacks; ++y)
	{
		for (int x = 0; x < slices; ++x)
		{
			const HRL_uint a = (HRL_uint)(y * (slices + 1) + x);
			const HRL_uint b = a + 1u;
			const HRL_uint c = a + (HRL_uint)(slices + 1);
			const HRL_uint d = c + 1u;

			// Winding is chosen for the outside of the sphere; the renderer culls
			// front faces so only the camera-facing inside is drawn.
			indices.push_back(a);
			indices.push_back(b);
			indices.push_back(c);
			indices.push_back(b);
			indices.push_back(d);
			indices.push_back(c);
		}
	}

	glGenVertexArrays(1, &bck_->sky_vao);
	glGenBuffers(1, &bck_->sky_vbo);
	glGenBuffers(1, &bck_->sky_ebo);
	glBindVertexArray(bck_->sky_vao);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->sky_vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(positions.size() * sizeof(glm::vec3)), positions.data(), GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(glm::vec3), (void*)0);
	glEnableVertexAttribArray(0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bck_->sky_ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indices.size() * sizeof(HRL_uint)), indices.data(), GL_STATIC_DRAW);
	glBindVertexArray(0);

	bck_->sky_index_count = (GLsizei)indices.size();
}

void GL33_InitContext(HRL_uint _width, HRL_uint _height, void *loader)
{
	if (!gladLoadGLLoader((GLADloadproc)loader))
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL, "Failed to init GLAD, (loader error)");
		return;
	}

	//create backend
	bck_ = new GL33_Backend();
	ctx_ = new GL33_State();

	//Gen VAO, VBO and EBO
	glGenVertexArrays(BUFFER_COUNT, bck_->vao);
	glGenBuffers(BUFFER_COUNT, bck_->vbo);
	glGenBuffers(BUFFER_COUNT, bck_->ebo);
	glGenBuffers(UBO_COUNT, bck_->ubo);

	//INIT QUAD BUFFER (static draw)
	glBindVertexArray(bck_->vao[BUFFER_QUAD]);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_QUAD]);
	//alloca buffer data with
	glBufferData(GL_ARRAY_BUFFER, 16*sizeof(float), fullscreen_quad_verts, GL_STATIC_DRAW);
	//layout(location=0) in vec2 apos
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	//layout(location=1) in vec2 auv
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
	glEnableVertexAttribArray(1);
	//EBO
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bck_->ebo[BUFFER_QUAD]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STATIC_DRAW);

	// Global illumination backend is initialized lazily. The scene remains
	// completely unchanged until HRL_SetGlobalIlluminationEnabled() is used.
	delete g_gl33_gi;
	g_gl33_gi = new GL_33_GI();
	g_gl33_gi->Initialize(bck_->vao[BUFFER_QUAD]);

	//GEN FBO & TEXTURES (post processing)
	glGenFramebuffers(2, bck_->post_fbo);
	glGenTextures(2, bck_->post_textures);
	InitTextureAndBindToFBO(bck_->post_textures[0], bck_->post_fbo[0], (int)_width, (int)_height);
	InitTextureAndBindToFBO(bck_->post_textures[1], bck_->post_fbo[1], (int)_width, (int)_height);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);  //reset fbo binding


	//DEBUG
	glBindVertexArray(bck_->vao[BUFFER_DEBUG]);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_DEBUG]);
	//alloca buffer data (with no value)
	glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
	//layout(location = 0) in vec3 apos
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	//layout(location = 1) in vec3 acolor
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
	glEnableVertexAttribArray(1);

	//UI
	glBindVertexArray(bck_->vao[BUFFER_UI]);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_UI]);
	//alloca buffer data (without values)
	glBufferData(GL_ARRAY_BUFFER, 16*sizeof(float), nullptr, GL_DYNAMIC_DRAW);
	//layout(location = 0) in vec2
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	//layout(location = 1) in vec2
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
	glEnableVertexAttribArray(1);
	//ebo
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bck_->ebo[BUFFER_UI]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STATIC_DRAW);


	//RESET VAO BINDING
	glBindVertexArray(0);


	//SHADERS

	//DEFAULT POST PROCESS
	auto* default_post_process_shader = new GL33_Shader();
	default_post_process_shader->GL33_Create(
		(const char*)res_post_vert_glsl,
		res_post_vert_glsl_len,
		(const char*)res_post_frag_glsl,
		res_post_frag_glsl_len
	);
	bck_->shaders.emplace(HRL_DEFAULT_POST_PROCESS_SHADER, default_post_process_shader);

	bck_->scene_effect_shader = new GL33_Shader();
	if (bck_->scene_effect_shader->GL33_Create(
		(const char*)res_post_vert_glsl, res_post_vert_glsl_len,
		kSceneEffectsFragmentShader, std::strlen(kSceneEffectsFragmentShader)) != 0)
	{
		delete bck_->scene_effect_shader;
		bck_->scene_effect_shader = nullptr;
	}

	bck_->volumetric_cloud_shader = new GL33_Shader();
	if (bck_->volumetric_cloud_shader->GL33_Create(
		(const char*)res_post_vert_glsl, res_post_vert_glsl_len,
		kVolumetricCloudFragmentShader, std::strlen(kVolumetricCloudFragmentShader)) != 0)
	{
		delete bck_->volumetric_cloud_shader;
		bck_->volumetric_cloud_shader = nullptr;
	}

	bck_->ambient_occlusion_shader = new GL33_Shader();
	if (bck_->ambient_occlusion_shader->GL33_Create(
		(const char*)res_post_vert_glsl, res_post_vert_glsl_len,
		kAmbientOcclusionFragmentShader, std::strlen(kAmbientOcclusionFragmentShader)) != 0)
	{
		delete bck_->ambient_occlusion_shader;
		bck_->ambient_occlusion_shader = nullptr;
	}

	bck_->ssr_shader = new GL33_Shader();
	if (bck_->ssr_shader->GL33_Create(
		(const char*)res_post_vert_glsl, res_post_vert_glsl_len,
		kSSRFragmentShader, std::strlen(kSSRFragmentShader)) != 0)
	{
		delete bck_->ssr_shader;
		bck_->ssr_shader = nullptr;
	}

	bck_->decal_shader = new GL33_Shader();
	if (bck_->decal_shader->GL33_Create(
		(const char*)res_post_vert_glsl, res_post_vert_glsl_len,
		kDecalFragmentShader, std::strlen(kDecalFragmentShader)) != 0)
	{
		delete bck_->decal_shader;
		bck_->decal_shader = nullptr;
	}

	//SPRITE SHADER
	auto* sprite_shader = new GL33_Shader();
	sprite_shader->GL33_Create(
		(const char*)res_sprite_vert_glsl,
		res_sprite_vert_glsl_len,
		(const char*)res_sprite_frag_glsl,
		res_sprite_frag_glsl_len);
	bck_->shaders.emplace(HRL_SPRITE_SHADER, sprite_shader);
	sprite_shader->SetFloat("BrightThreshold", 0.75f);

	//STATIC 3D MESH SHADER
	auto* mesh3d_shader = new GL33_Shader();
	mesh3d_shader->GL33_Create(
		(const char*)res_static_3dmesh_vert_glsl,
		res_static_3dmesh_vert_glsl_len,
		(const char*)res_static_3dmesh_frag_glsl,
		res_static_3dmesh_frag_glsl_len);
	bck_->shaders.emplace(HRL_MESH_3D_SHADER, mesh3d_shader);
	mesh3d_shader->SetFloat("BrightThreshold", 0.75f);
	mesh3d_shader->SetFloat("EnvironmentStrength", 0.0f);

	//SKINNED 3D MESH SHADER
	auto* skinned_mesh_shader = new GL33_Shader();
	if (skinned_mesh_shader->GL33_Create(
		(const char*)res_skinned_3dmesh_vert_glsl, res_skinned_3dmesh_vert_glsl_len,
		(const char*)res_static_3dmesh_frag_glsl, res_static_3dmesh_frag_glsl_len) == 0)
	{
		bck_->shaders.emplace(HRL_SKINNED_3D_MESH_SHADER, skinned_mesh_shader);
		skinned_mesh_shader->SetFloat("BrightThreshold", 0.75f);
		skinned_mesh_shader->SetFloat("EnvironmentStrength", 0.0f);
	}
	else
	{
		delete skinned_mesh_shader;
	}

	// SCREEN-SPACE DISPLACEMENT
	bck_->ss_displacement_static_shader = new GL33_Shader();
	if (bck_->ss_displacement_static_shader->GL33_Create(
		(const char*)res_static_3dmesh_vert_glsl, res_static_3dmesh_vert_glsl_len,
		kSSDisplacementFragmentShader, std::strlen(kSSDisplacementFragmentShader)) != 0)
	{
		delete bck_->ss_displacement_static_shader;
		bck_->ss_displacement_static_shader = nullptr;
	}

	bck_->ss_displacement_skinned_shader = new GL33_Shader();
	if (bck_->ss_displacement_skinned_shader->GL33_Create(
		(const char*)res_skinned_3dmesh_vert_glsl, res_skinned_3dmesh_vert_glsl_len,
		kSSDisplacementFragmentShader, std::strlen(kSSDisplacementFragmentShader)) != 0)
	{
		delete bck_->ss_displacement_skinned_shader;
		bck_->ss_displacement_skinned_shader = nullptr;
	}

	//DEBUG SHADER
	auto* debug_shader = new GL33_Shader();
	debug_shader->GL33_Create(
		(const char*)res_debug_vert_glsl,
		res_debug_vert_glsl_len,
		(const char*)res_debug_frag_glsl,
		res_debug_frag_glsl_len
	);
	bck_->shaders.emplace(HRL_DEBUG_SHADER, debug_shader);

	//SKY SPHERE SHADER
	bck_->sky_shader = new GL33_Shader();
	if (bck_->sky_shader->GL33_Create(
		(const char*)res_sky_sphere_vert_glsl,
		res_sky_sphere_vert_glsl_len,
		(const char*)res_sky_sphere_frag_glsl,
		res_sky_sphere_frag_glsl_len) != 0)
	{
		delete bck_->sky_shader;
		bck_->sky_shader = nullptr;
	}
	CreateSkySphereGeometry();

	//SHADOW SHADERS
	bck_->shadow_2d_shader = new GL33_Shader();
	if (bck_->shadow_2d_shader->GL33_Create(
		(const char*)res_shadow_2d_vert_glsl, res_shadow_2d_vert_glsl_len,
		(const char*)res_shadow_2d_frag_glsl, res_shadow_2d_frag_glsl_len) != 0)
	{
		delete bck_->shadow_2d_shader;
		bck_->shadow_2d_shader = nullptr;
	}

	bck_->shadow_point_shader = new GL33_Shader();
	if (bck_->shadow_point_shader->GL33_Create(
		(const char*)res_shadow_point_vert_glsl, res_shadow_point_vert_glsl_len,
		(const char*)res_shadow_point_frag_glsl, res_shadow_point_frag_glsl_len) != 0)
	{
		delete bck_->shadow_point_shader;
		bck_->shadow_point_shader = nullptr;
	}

	bck_->shadow_skeletal_2d_shader = new GL33_Shader();
	if (bck_->shadow_skeletal_2d_shader->GL33_Create(
		(const char*)res_shadow_skeletal_2d_vert_glsl, res_shadow_skeletal_2d_vert_glsl_len,
		(const char*)res_shadow_2d_frag_glsl, res_shadow_2d_frag_glsl_len) != 0)
	{
		delete bck_->shadow_skeletal_2d_shader;
		bck_->shadow_skeletal_2d_shader = nullptr;
	}

	bck_->shadow_skeletal_point_shader = new GL33_Shader();
	if (bck_->shadow_skeletal_point_shader->GL33_Create(
		(const char*)res_shadow_skeletal_point_vert_glsl, res_shadow_skeletal_point_vert_glsl_len,
		(const char*)res_shadow_point_frag_glsl, res_shadow_point_frag_glsl_len) != 0)
	{
		delete bck_->shadow_skeletal_point_shader;
		bck_->shadow_skeletal_point_shader = nullptr;
	}

	//UI SHADER
	// The fragment shader is kept local here as well as in shaders/opengl/ui.frag.glsl
	// so an out-of-date generated resource bundle cannot silently disable SDF text.
	static const char* kUISDFFragmentShader = R"GLSL(#version 330 core

in vec2 uv;

uniform vec4 uTintColor;
uniform sampler2D uTexture;
uniform bool uSDFText;

out vec4 FragColor;

void main()
{
    vec4 texel = texture(uTexture, uv);
    if (uSDFText)
    {
        float distance = texel.r;
        float smoothing = max(fwidth(distance), 0.001);
        float alpha = smoothstep(0.5 - smoothing, 0.5 + smoothing, distance);
        FragColor = vec4(uTintColor.rgb, uTintColor.a * alpha);
    }
    else
    {
        FragColor = texel * uTintColor;
    }
}
)GLSL";
	bck_->ui_shader = new GL33_Shader();
	bck_->ui_shader->GL33_Create(
		(const char*)res_ui_vert_glsl,
		res_ui_vert_glsl_len,
		kUISDFFragmentShader,
		std::strlen(kUISDFFragmentShader)
	);

	InitVFXRenderer();

	//FALLBACK TEXTURES
	bck_->fallback_textures[ALBEDO_INT] = GL33_CreateTexture((const char*)res_default_albedo_png, res_default_albedo_png_len);
	bck_->fallback_textures[NORMAL_INT] = GL33_CreateTexture((const char*)res_default_normal_png, res_default_normal_png_len);
	bck_->fallback_textures[SPECULAR_INT] = GL33_CreateTexture((const char*)res_default_specular_png, res_default_specular_png_len);
	bck_->fallback_textures[ROUGHNESS_INT] = GL33_CreateTexture((const char*)res_default_roughness_png, res_default_roughness_png_len);
	bck_->fallback_textures[METALLIC_INT] = GL33_CreateTexture((const char*)res_default_metallic_png, res_default_metallic_png_len);
	bck_->fallback_textures[ALPHA_INT] = GL33_CreateTexture((const char*)res_default_alpha_png, res_default_alpha_png_len);
	// SS displacement mapping defaults to neutral/no-displacement (albedo fallback).
	bck_->fallback_textures[SS_DISPLACEMENT_MAPPING_INT] = bck_->fallback_textures[ALBEDO_INT];

	assert(bck_->fallback_textures[ALBEDO_INT] != HRL_INVALID_ID && "Failed to load fallback albedo");
	assert(bck_->fallback_textures[NORMAL_INT] != HRL_INVALID_ID && "Failed to load fallback normal");
	assert(bck_->fallback_textures[SPECULAR_INT] != HRL_INVALID_ID && "Failed to load fallback specular");
	assert(bck_->fallback_textures[ROUGHNESS_INT] != HRL_INVALID_ID && "Failed to load fallback roughness");
	assert(bck_->fallback_textures[METALLIC_INT] != HRL_INVALID_ID && "Failed to load fallback metallic");
	assert(bck_->fallback_textures[ALPHA_INT] != HRL_INVALID_ID && "Failed to load fallback alpha");


	//UBO
	//LIGHTS
	glBindBuffer(GL_UNIFORM_BUFFER, bck_->ubo[UBO_LIGHTS]);
	glBufferData(GL_UNIFORM_BUFFER, MAX_LIGHTS * sizeof(GL_Light), nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, bck_->ubo[UBO_LIGHTS]);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void GL33_Shutdown()
{
	for (auto& [id, mesh] : bck_->meshes)
	{
		for (auto& lod : mesh.levels)
		{
			if (lod.vao) glDeleteVertexArrays(1, &lod.vao);
			if (lod.vbo) glDeleteBuffers(1, &lod.vbo);
			if (lod.ebo) glDeleteBuffers(1, &lod.ebo);
		}
	}
	bck_->meshes.clear();

	for (auto& [id, mesh] : bck_->skeletal_meshes)
	{
		(void)id;
		if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
		if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
		if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
		if (mesh.bone_ubo) glDeleteBuffers(1, &mesh.bone_ubo);
	}
	bck_->skeletal_meshes.clear();

	for (auto& [id, landscape] : bck_->landscapes)
	{
		(void)id;
		if (landscape.geometry.vao) glDeleteVertexArrays(1, &landscape.geometry.vao);
		if (landscape.geometry.vbo) glDeleteBuffers(1, &landscape.geometry.vbo);
		if (landscape.geometry.ebo) glDeleteBuffers(1, &landscape.geometry.ebo);
	}
	bck_->landscapes.clear();

	for (auto& [scene_id, scene] : bck_->gpu_scenes)
		DestroyMSAAResources(scene);

	for (const auto s : bck_->shaders)
	{
		delete s.second;
	}
	bck_->debug_mesh_info_textures.clear();
	for (const auto t : bck_->textures)
	{
		delete t.second;
	}

	delete bck_->ui_shader;
	delete bck_->scene_effect_shader;
	delete bck_->volumetric_cloud_shader;
	delete bck_->ambient_occlusion_shader;
	delete bck_->ssr_shader;
	delete bck_->decal_shader;
	delete bck_->vfx_shader;
	delete bck_->ss_displacement_static_shader;
	delete bck_->ss_displacement_skinned_shader;
	if (bck_->vfx_vao) glDeleteVertexArrays(1, &bck_->vfx_vao);
	if (bck_->vfx_vbo) glDeleteBuffers(1, &bck_->vfx_vbo);
	if (bck_->vfx_ebo) glDeleteBuffers(1, &bck_->vfx_ebo);
	if (bck_->vfx_instance_vbo) glDeleteBuffers(1, &bck_->vfx_instance_vbo);
	delete bck_->sky_shader;
	delete bck_->shadow_2d_shader;
	delete bck_->shadow_point_shader;
	delete bck_->shadow_skeletal_2d_shader;
	delete bck_->shadow_skeletal_point_shader;

	for (auto& [id, shadow] : bck_->shadow_maps)
	{
		if (shadow.depth_2d) glDeleteTextures(1, &shadow.depth_2d);
		if (shadow.depth_cube) glDeleteTextures(1, &shadow.depth_cube);
		if (shadow.fbo) glDeleteFramebuffers(1, &shadow.fbo);
	}
	bck_->shadow_maps.clear();

	//DELETE POST PROCESS OBJECTS
	glDeleteFramebuffers(2, bck_->post_fbo);
	glDeleteTextures(2, bck_->post_textures);

	//DELETE SHARED SPRITE GEOMETRY
	if (bck_->sprite_vao) glDeleteVertexArrays(1, &bck_->sprite_vao);
	if (bck_->sprite_instanced_vao) glDeleteVertexArrays(1, &bck_->sprite_instanced_vao);
	if (bck_->sprite_vbo) glDeleteBuffers(1, &bck_->sprite_vbo);
	if (bck_->sprite_ebo) glDeleteBuffers(1, &bck_->sprite_ebo);
	if (bck_->sprite_instance_vbo) glDeleteBuffers(1, &bck_->sprite_instance_vbo);

	//DELETE SKY SPHERE BUFFERS
	if (bck_->sky_vao) glDeleteVertexArrays(1, &bck_->sky_vao);
	if (bck_->sky_vbo) glDeleteBuffers(1, &bck_->sky_vbo);
	if (bck_->sky_ebo) glDeleteBuffers(1, &bck_->sky_ebo);

	if (g_gl33_gi)
	{
		g_gl33_gi->Shutdown();
		delete g_gl33_gi;
		g_gl33_gi = nullptr;
	}

	//DELETE BUFFERS
	glDeleteVertexArrays(BUFFER_COUNT, bck_->vao);
	glDeleteBuffers(BUFFER_COUNT, bck_->vbo);
	glDeleteBuffers(BUFFER_COUNT, bck_->ebo);
	glDeleteBuffers(UBO_COUNT, bck_->ubo);

	delete bck_;
	delete ctx_;
}


void GL33_WindowResizeCallback(int width, int height)
{
	if (!bck_)
		return;

	// These are framebuffer/drawable dimensions in pixels. HRL viewports are
	// normalized and converted to pixels every frame, so keep the GL viewport
	// itself synchronized with the new drawable size as well.
	if (width <= 0 || height <= 0)
	{
		glViewport(0, 0, 0, 0);
		return;
	}
	glViewport(0, 0, width, height);

	// Resize post-process ping-pong targets.
	for (int i = 0; i < 2; ++i)
	{
		glBindTexture(GL_TEXTURE_2D, bck_->post_textures[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
	}

	// Resize every scene attachment. Attachments 3 and 4 are the DDGI albedo
	// and world-normal targets and must track the window size too.
	for (const auto& s : bck_->gpu_scenes)
	{
		GL_Scene* scene = s.second;
		if (!scene)
			continue;

		glBindTexture(GL_TEXTURE_2D, scene->textures[0]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
		glBindTexture(GL_TEXTURE_2D, scene->textures[1]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glBindTexture(GL_TEXTURE_2D, scene->textures[2]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindTexture(GL_TEXTURE_2D, scene->textures[3]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glBindTexture(GL_TEXTURE_2D, scene->textures[4]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);

		if (scene->depth_texture)
		{
			glBindTexture(GL_TEXTURE_2D, scene->depth_texture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
		}

		scene->width = width;
		scene->height = height;

		// Recreate multisample resources at the new resolution.
		if (bck_->antialiasing_samples > 1)
			CreateMSAAResources(scene, bck_->antialiasing_samples);
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}


void GL33_DrawScene(hrl_scene_t *scene, HRL_id scene_id)
{
	auto scene_it = bck_->gpu_scenes.find(scene_id);
	if (scene_it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GL33_DrawScene: tried to draw scene with invalid gpu ID");
		return;
	}

	GL_Scene* gpu_scene = scene_it->second;
	GLuint scene_fbo = gpu_scene->fbo;
	ctx_->current_scene = scene;
	ctx_->current_fog = &scene->fog;

	// Clear the actual render target once. With MSAA enabled, the multisample
	// framebuffer is the render target and the regular scene FBO is only the
	// resolve target used for post-processing/presentation/picking.
	GLuint initial_render_fbo = (gpu_scene->msaa_samples > 1 && gpu_scene->msaa_fbo != 0)
		? gpu_scene->msaa_fbo : scene_fbo;
	glBindFramebuffer(GL_FRAMEBUFFER, initial_render_fbo);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	const GLfloat zero[4] = {0.f, 0.f, 0.f, 0.f};
	glClearBufferfv(GL_COLOR, 2, zero);

	// Shadow maps and the scene light UBO do not depend on the active camera.
	// Prepare them once per scene, not once per viewport.
	PrepareSceneShadows(scene, scene_id);
	UploadSceneLights(scene, scene_id);

	// Scene-scoped shadow textures are identical for all viewports. Bind once.
	for (int i = 0; i < MAX_SHADOW_SLOTS; ++i)
	{
		glActiveTexture(GL_TEXTURE0 + SHADOW_2D_TEXTURE_UNIT_BASE + i);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0 + SHADOW_CUBE_TEXTURE_UNIT_BASE + i);
		glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
	}
	const auto shadowSlotsIt = bck_->active_shadow_slots_by_scene.find(scene_id);
	if (shadowSlotsIt != bck_->active_shadow_slots_by_scene.end())
	for (const auto& [lightId, slot] : shadowSlotsIt->second)
	{
		auto shadowIt = bck_->shadow_maps.find(lightId);
		if (shadowIt == bck_->shadow_maps.end()) continue;
		if (shadowIt->second.is_point)
		{
			glActiveTexture(GL_TEXTURE0 + SHADOW_CUBE_TEXTURE_UNIT_BASE + slot);
			glBindTexture(GL_TEXTURE_CUBE_MAP, shadowIt->second.depth_cube);
		}
		else
		{
			glActiveTexture(GL_TEXTURE0 + SHADOW_2D_TEXTURE_UNIT_BASE + slot);
			glBindTexture(GL_TEXTURE_2D, shadowIt->second.depth_2d);
		}
	}

	// DDGI is world-space and camera-independent: update the probe volume once
	// per scene frame, before any mesh is shaded. The mesh shader consumes the
	// resulting UBO directly.
	if (scene->global_illumination_enabled &&
		scene->global_illumination_method == HRL_GI_DDGI && g_gl33_gi)
	{
		g_gl33_gi->UpdateDDGI(scene_id);
	}

	for (const auto& v : scene->viewports)
	{
		if (!v.second || !v.second->camera_)
			continue;
		ctx_->viewport = v.second;
		ctx_->bound_material = nullptr;
		ctx_->bound_shader = nullptr;
		float winW = (float)GetWindowWidth();
		float winH = (float)GetWindowHeight();

		glViewport(
		 (GLsizei)(v.second->x_ * winW),
		 (GLsizei)(v.second->y_ * winH),
		 (GLsizei)(v.second->width_ * winW),
		 (GLsizei)(v.second->height_ * winH)
		);

		// Render every viewport into the scene framebuffer. Re-bind it before the
		// depth clear because DrawWidgets() leaves the default framebuffer bound.
		GLuint render_fbo = (gpu_scene->msaa_samples > 1 && gpu_scene->msaa_fbo != 0)
			? gpu_scene->msaa_fbo : scene_fbo;

		glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
		glClear(GL_DEPTH_BUFFER_BIT);

		ctx_->proj_mat = CalculateProjectionMatrix();
		ctx_->view_mat = CalculateViewMatrix();
		const FrustumPlaneSet cameraFrustum = BuildFrustum(ctx_->proj_mat * ctx_->view_mat);

		// PrepareSceneShadows()/UploadSceneLights() have already run once for this scene.
		// ---- STEP 1 : sky sphere, géométrie 3D puis sprites ----
		// The wireframe debug view is intentionally a pure geometry view. Do not
		// draw the filled sky or feed the result through scene/post-processing
		// passes, otherwise the debug image can be obscured by a later fullscreen
		// pass.
		const bool wireframeDebug = scene->debug_view == HRL_DEBUG_VIEW_WIREFRAME;
		if (!wireframeDebug)
			DrawSkySphere(scene);

		// ---- STEP 1b : rendu de la géométrie 3D puis des sprites ----
		glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
		glEnable(GL_DEPTH_TEST);
		glDepthMask(GL_TRUE);
		glDepthFunc(GL_LESS);
		glDisable(GL_BLEND);
		// Render the full opaque scene normally first. Screen-space displacement
		// is a second pass that re-samples this already-rendered image, so the
		// displaced mesh must be present in the source image.
		DrawLandscapes(scene_id, scene, cameraFrustum);
		DrawOpaqueMeshes(scene_id, scene->meshes, scene->debug_view, cameraFrustum, false);
		if (!wireframeDebug)
		{
			DrawSprites(scene->meshes, cameraFrustum);
			DrawVFX(scene);
		}

		// Resolve once so displacement samples a stable scene-color snapshot.
		ResolveSceneMSAA(gpu_scene);

		bool hasScreenSpaceDisplacement = false;
		for (const auto& [meshId, mesh] : scene->meshes)
		{
			(void)meshId;
			if (!mesh || mesh->material_ == HRL_INVALID_ID) continue;
			auto matIt = GetPrivateContext()->materials.find(mesh->material_);
			if (matIt != GetPrivateContext()->materials.end() && MaterialUsesScreenSpaceDisplacement(matIt->second))
			{
				hasScreenSpaceDisplacement = true;
				break;
			}
		}

		bool displacementRendered = false;
		if (!wireframeDebug && hasScreenSpaceDisplacement && scene->debug_view == HRL_DEBUG_VIEW_NONE &&
			CaptureSceneColorForDisplacement(gpu_scene, bck_->post_textures[0]))
		{
			// ResolveSceneMSAA() leaves the regular scene FBO bound. Re-bind the
			// active render target here so the displacement pass works both with
			// and without MSAA. It samples from post_textures[0], never from the
			// framebuffer it is currently writing to, so there is no feedback loop.
			glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
			glViewport(
				(GLsizei)(v.second->x_ * winW),
				(GLsizei)(v.second->y_ * winH),
				(GLsizei)(v.second->width_ * winW),
				(GLsizei)(v.second->height_ * winH)
			);
			DrawScreenSpaceDisplacementMeshes(scene_id, scene, cameraFrustum);
			displacementRendered = true;
		}

		// Debug primitives stay crisp and are rendered over the distortion.
		auto debugIt = GetPrivateContext()->debug_renderers.find(scene_id);
		if (debugIt != GetPrivateContext()->debug_renderers.end())
			GL33_DrawDebug(debugIt->second, GetPrivateContext()->debug_line_thickness);

		// If displacement was rendered into the MSAA target, resolve it now so the
		// post-process/fog stages see the final displaced scene. With no displacement
		// pass, the first resolve is already the final scene resolve; doing another
		// one here would overwrite debug primitives rendered after that resolve.
		if (displacementRendered)
			ResolveSceneMSAA(gpu_scene);

		bool has_post_process = !wireframeDebug && !v.second->post_processes.empty();
		bool has_scene_effects = !wireframeDebug && (HasActiveVolumetricFog(scene) || scene->god_rays.enabled);
		bool has_volumetric_clouds = !wireframeDebug && scene->volumetric_cloud_enabled;
		bool has_ambient_occlusion = !wireframeDebug && scene->ambient_occlusion_enabled;
		bool has_ssr = !wireframeDebug && scene->screen_space_reflections_enabled;
		bool has_decals = !wireframeDebug && !scene->decals.empty();

		if (has_post_process || has_scene_effects || has_volumetric_clouds || has_ambient_occlusion || has_ssr || has_decals)
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
			glReadBuffer(GL_COLOR_ATTACHMENT0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bck_->post_fbo[0]);
			glBlitFramebuffer(0, 0, (int)winW, (int)winH, 0, 0, (int)winW, (int)winH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

			GLuint currentTexture = bck_->post_textures[0];
			int src = 0;
			const int vx = (int)(v.second->x_ * winW);
			const int vy = (int)(v.second->y_ * winH);
			const int vw = std::max(1, (int)(v.second->width_ * winW));
			const int vh = std::max(1, (int)(v.second->height_ * winH));
			if (has_decals && bck_->decal_shader)
				ApplyDecals(gpu_scene, scene, ctx_->view_mat, ctx_->proj_mat, vx, vy, vw, vh, currentTexture);
			src = (currentTexture == bck_->post_textures[0]) ? 0 : 1;
			if (has_ssr && bck_->ssr_shader)
				ApplyScreenSpaceReflections(gpu_scene, scene, ctx_->view_mat, ctx_->proj_mat, vx, vy, vw, vh, currentTexture);
			src = (currentTexture == bck_->post_textures[0]) ? 0 : 1;
			if (has_ambient_occlusion && bck_->ambient_occlusion_shader)
			{
				ApplyAmbientOcclusion(gpu_scene, scene, ctx_->view_mat, ctx_->proj_mat, vx, vy, vw, vh, currentTexture);
				src = (currentTexture == bck_->post_textures[0]) ? 0 : 1;
			}
			if (has_scene_effects && bck_->scene_effect_shader)
			{
				ApplySceneEffects(gpu_scene, scene, ctx_->view_mat, ctx_->proj_mat, vx, vy, vw, vh, currentTexture);
				src = (currentTexture == bck_->post_textures[0]) ? 0 : 1;
			}
			if (has_volumetric_clouds && bck_->volumetric_cloud_shader)
			{
				ApplyVolumetricCloudsPass(gpu_scene, scene, ctx_->view_mat, ctx_->proj_mat, vx, vy, vw, vh, currentTexture);
				src = (currentTexture == bck_->post_textures[0]) ? 0 : 1;
			}

			// User post-processes keep the historic full-frame behavior.
			glViewport(0, 0, (int)winW, (int)winH);

			std::vector<HRL_PostProcess*> sortedPostProcesses;
			sortedPostProcesses.reserve(v.second->post_processes.size());
			for (const auto& [postId, pp] : v.second->post_processes)
			{
				(void)postId;
				if (pp) sortedPostProcesses.push_back(pp);
			}
			std::sort(sortedPostProcesses.begin(), sortedPostProcesses.end(), [](const HRL_PostProcess* a, const HRL_PostProcess* b) {
				if (a->priority_ != b->priority_) return a->priority_ < b->priority_;
				return a->id_ < b->id_;
			});
			for (HRL_PostProcess* pp : sortedPostProcesses)
			{
				int dst = 1 - src;
				glBindFramebuffer(GL_FRAMEBUFFER, bck_->post_fbo[dst]);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
				DrawPostProcessQuad(bck_->post_textures[src], scene_it->second->textures[1], pp);
				src = dst;
			}

			glBindFramebuffer(GL_READ_FRAMEBUFFER, bck_->post_fbo[src]);
			glReadBuffer(GL_COLOR_ATTACHMENT0);
			if (scene->draw_on_screen)
			{
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
				glBlitFramebuffer(0, 0, (int)winW, (int)winH, 0, 0, (int)winW, (int)winH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
			else
			{
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene_fbo);
				glDrawBuffer(GL_COLOR_ATTACHMENT0);
				glBlitFramebuffer(0, 0, (int)winW, (int)winH, 0, 0, (int)winW, (int)winH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
		}
		else if (scene->draw_on_screen)
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
			glReadBuffer(GL_COLOR_ATTACHMENT0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			glBlitFramebuffer(0, 0, (int)winW, (int)winH, 0, 0, (int)winW, (int)winH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		DrawGizmos(scene, v.second, [&](){ for (const auto& [vid, vp] : scene->viewports) if (vp == v.second) return vid; return (HRL_id)HRL_INVALID_ID; }());
		if (scene->debug_view == HRL_DEBUG_VIEW_MESH_INFO)
			DrawMeshDebugInfoTexts(scene, v.second);
		DrawWidgets(v.second->widgets, v.second);
	}
}






//UTILS IMPLEMENTATION
static void InitTextureAndBindToFBO(GLuint _texture, GLuint _fbo, int width, int height)
{
	//on initialise avec les bonnes valeurs la texture
	glBindTexture(GL_TEXTURE_2D, _texture);

	//HDR Texture (RGBA16F)
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	//pour eviter les artefacts sur les bords
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	//on unbind la texture du container texture openGL (on va la bind une seule fois au FBO correspondant)
	glBindTexture(GL_TEXTURE_2D, 0);

	//on attache la texture au framebuffer
	glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _texture, 0);
}



//MATERIALS
static void ApplyFallbackToUnit(int textureUnit, int fallbackIndex)
{
	if (!bck_ || fallbackIndex < 0 || fallbackIndex >= 6) return;
	glActiveTexture(GL_TEXTURE0 + textureUnit);
	HRL_id fallback_hrl_id = bck_->fallback_textures[fallbackIndex];
	auto fallback = bck_->textures.find(fallback_hrl_id);
	if (fallback != bck_->textures.end() && fallback->second)
		glBindTexture(GL_TEXTURE_2D, fallback->second->GetGL_ID());
}

static void ApplyFallback(int index)
{
	ApplyFallbackToUnit(index, index);
}

static bool BindMaterial(HRL_Material* mat, HRL_id object_id, const HRL_Mesh* mesh, const glm::mat4& model)
{
	auto it = bck_->shaders.find(mat->shader_);
	if (it == bck_->shaders.end())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "Bind material error: shader doesn't exist");
		return false;
	}
	auto* s = it->second;
	ctx_->shader = s;
	const bool shaderChanged = ctx_->bound_shader != s;
	const bool materialChanged = shaderChanged || ctx_->bound_material != mat;
	if (shaderChanged)
	{
		s->Use();
		ctx_->bound_shader = s;
	}

	// Per-object state.
	s->SetMat4("projection", ctx_->proj_mat);
	s->SetMat4("view", ctx_->view_mat);
	s->SetMat4("model", model);
	if (mat->shader_ == HRL_MESH_3D_SHADER)
	{
		const glm::mat3 model3(model);
		glm::mat3 normalMatrix(1.0f);
		const float det = glm::determinant(model3);
		if (std::isfinite(det) && std::abs(det) > 1e-8f)
			normalMatrix = glm::transpose(glm::inverse(model3));
		s->SetMat3("normalMatrix", normalMatrix);
	}
	s->SetUint("uSpriteID", object_id);
	s->SetVec4("UVRegion", {mesh->region_[0], mesh->region_[1], mesh->region_[2], mesh->region_[3]});
	s->SetVec3("CamPos", ctx_->viewport->camera_->position_);

	if (shaderChanged)
	{
		if (mat->shader_ == HRL_SPRITE_SHADER)
			s->SetInt("uInstanced", 0);
		s->SetInt("FogEnabled", ctx_->current_fog->enabled);
		s->SetInt("FogMode", ctx_->current_fog->mode);
		if (mat->shader_ == HRL_MESH_3D_SHADER || mat->shader_ == HRL_SKINNED_3D_MESH_SHADER)
			s->SetInt("DebugView", (int)(ctx_->current_scene ? ctx_->current_scene->debug_view : HRL_DEBUG_VIEW_NONE));
		s->SetVec4("FogColor", {ctx_->current_fog->r, ctx_->current_fog->g, ctx_->current_fog->b, 1.f});
		s->SetFloat("FogStart", ctx_->current_fog->range_start);
		s->SetFloat("FogEnd", ctx_->current_fog->range_end);
		s->SetFloat("FogDensity", ctx_->current_fog->density);
		s->SetVec3("TintColor", glm::vec3(1.f));
	}

	if ((mat->shader_ == HRL_MESH_3D_SHADER || mat->shader_ == HRL_SKINNED_3D_MESH_SHADER) && shaderChanged)
	{
		// Defaults for the built-in FBX material reconstruction uniforms. Existing
		// manually-created materials keep their previous texture-driven behavior.
		s->SetFloat("BaseColorAlpha", 1.f);
		s->SetFloat("RoughnessValue", 1.f);
		s->SetFloat("MetallicValue", 0.f);
		s->SetFloat("SpecularValue", 1.f);
		s->SetFloat("OpacityValue", 1.f);
		s->SetInt("RoughnessUseValue", 0);
		s->SetInt("MetallicUseValue", 0);
		s->SetInt("SpecularUseValue", 0);
		s->SetInt("OpacityUseValue", 0);
		s->SetInt("RoughnessInvert", 0);
		s->SetInt("AlphaInvert", 0);
		s->SetInt("ShadowMap2D_0", SHADOW_2D_TEXTURE_UNIT_BASE + 0);
		s->SetInt("ShadowMap2D_1", SHADOW_2D_TEXTURE_UNIT_BASE + 1);
		s->SetInt("ShadowMap2D_2", SHADOW_2D_TEXTURE_UNIT_BASE + 2);
		s->SetInt("ShadowMap2D_3", SHADOW_2D_TEXTURE_UNIT_BASE + 3);
		s->SetInt("ShadowMapCube_0", SHADOW_CUBE_TEXTURE_UNIT_BASE + 0);
		s->SetInt("ShadowMapCube_1", SHADOW_CUBE_TEXTURE_UNIT_BASE + 1);
		s->SetInt("ShadowMapCube_2", SHADOW_CUBE_TEXTURE_UNIT_BASE + 2);
		s->SetInt("ShadowMapCube_3", SHADOW_CUBE_TEXTURE_UNIT_BASE + 3);
		s->SetInt("EnvironmentMap", ENVIRONMENT_TEXTURE_UNIT);
		bool environmentEnabled = false;
		if (ctx_->current_scene && ctx_->current_scene->environment_mapping_enabled)
		{
			HRL_id envId = ctx_->current_scene->environment_texture;
			if (envId == HRL_INVALID_ID) envId = ctx_->current_scene->sky_texture;
			auto envIt = bck_->textures.find(envId);
			if (envIt != bck_->textures.end())
			{
				environmentEnabled = true;
				glActiveTexture(GL_TEXTURE0 + ENVIRONMENT_TEXTURE_UNIT);
				glBindTexture(GL_TEXTURE_2D, envIt->second->GetGL_ID());
			}
		}
		s->SetInt("EnvironmentEnabled", environmentEnabled ? 1 : 0);
	}

	if (materialChanged)
	{
		if (mat->shader_ == HRL_MESH_3D_SHADER || mat->shader_ == HRL_SKINNED_3D_MESH_SHADER)
			s->SetInt("ss_displacement_enabled", 0);
		s->SetInt(HRL_MATERIAL_PARAM_TWO_SIDED, 0);

		for (const auto& [name, value] : mat->intParams_) s->SetInt(name, value);
		for (int i = 0; i < 6; ++i)
		{
			s->SetInt(tex_uniform_name[i], i);
			auto itParam = mat->textureParams_.find(std::string(tex_uniform_name[i]));
			if (itParam == mat->textureParams_.end()) { ApplyFallback(i); continue; }
			auto itTexture = bck_->textures.find(itParam->second);
			if (itTexture == bck_->textures.end()) { ApplyFallback(i); continue; }
			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(GL_TEXTURE_2D, itTexture->second->GetGL_ID());
		}
		s->SetInt(HRL_T_AMBIENT_OCCLUSION, AO_TEXTURE_UNIT);
		glActiveTexture(GL_TEXTURE0 + AO_TEXTURE_UNIT);
		auto aoParam = mat->textureParams_.find(std::string(HRL_T_AMBIENT_OCCLUSION));
		if (aoParam != mat->textureParams_.end())
		{
			auto aoTexture = bck_->textures.find(aoParam->second);
			if (aoTexture != bck_->textures.end() && aoTexture->second)
				glBindTexture(GL_TEXTURE_2D, aoTexture->second->GetGL_ID());
			else
				ApplyFallbackToUnit(AO_TEXTURE_UNIT, ROUGHNESS_INT);
		}
		else
			ApplyFallbackToUnit(AO_TEXTURE_UNIT, ROUGHNESS_INT);
		for (const auto& [name, value] : mat->floatParams_) s->SetFloat(name, value);
		for (const auto& [name, value] : mat->vec2Params_) s->SetVec2(name, value);
		for (const auto& [name, value] : mat->vec3Params_) s->SetVec3(name, value);
		for (const auto& [name, value] : mat->vec4Params_) s->SetVec4(name, value);
		ctx_->bound_material = mat;
	}
	return true;
}

static glm::mat4 CalculateModelMatrix(const HRL_Mesh* mesh)
{
	glm::mat4 model(1.f);
	model = glm::translate(model, mesh->position_);
	model = glm::translate(model, mesh->pivot_point_);
	model = glm::rotate(model, glm::radians(mesh->rotation_.x), glm::vec3(1.f, 0.f, 0.f));
	model = glm::rotate(model, glm::radians(mesh->rotation_.y), glm::vec3(0.f, 1.f, 0.f));
	model = glm::rotate(model, glm::radians(mesh->rotation_.z), glm::vec3(0.f, 0.f, 1.f));
	model = glm::translate(model, -mesh->pivot_point_);
	model = glm::scale(model, mesh->scale_);
	// Skeletal skinning matrices are already expressed in FBX world space.
	// Do not append the FBX geometry_to_world transform here: that would apply
	// the import transform a second time. The model matrix therefore contains
	// only HRL's user-controlled object transform.
	return model;
}

static void DrawSkySphere(const hrl_scene_t* scene)
{
	if (!scene || !scene->sky_sphere_enabled || !bck_->sky_shader || bck_->sky_index_count <= 0)
		return;

	// The sphere is centered in view space: removing camera translation makes it
	// effectively infinitely far away while remaining a real sphere geometry.
	const glm::mat4 skyView = glm::mat4(glm::mat3(ctx_->view_mat));
	glm::mat4 rotation(1.f);
	rotation = glm::rotate(rotation, glm::radians(scene->sky_rotation.x), glm::vec3(1.f, 0.f, 0.f));
	rotation = glm::rotate(rotation, glm::radians(scene->sky_rotation.y), glm::vec3(0.f, 1.f, 0.f));
	rotation = glm::rotate(rotation, glm::radians(scene->sky_rotation.z), glm::vec3(0.f, 0.f, 1.f));

	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT);

	GLint previousPolygonMode[2] = {GL_FILL, GL_FILL};
	glGetIntegerv(GL_POLYGON_MODE, previousPolygonMode);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	bck_->sky_shader->Use();
	bck_->sky_shader->SetMat4("projection", ctx_->proj_mat);
	bck_->sky_shader->SetMat4("view", skyView);
	bck_->sky_shader->SetMat4("rotation", rotation);
	bck_->sky_shader->SetVec3("SkyTopColor", scene->sky_top_color);
	bck_->sky_shader->SetVec3("SkyHorizonColor", scene->sky_horizon_color);
	bck_->sky_shader->SetVec3("SkyBottomColor", scene->sky_bottom_color);

	bool hasSkyTexture = false;
	if (scene->sky_texture != HRL_INVALID_ID)
	{
		auto sky_it = bck_->textures.find(scene->sky_texture);
		if (sky_it != bck_->textures.end())
		{
			hasSkyTexture = true;
			glActiveTexture(GL_TEXTURE0 + ENVIRONMENT_TEXTURE_UNIT);
			glBindTexture(GL_TEXTURE_2D, sky_it->second->GetGL_ID());
		}
	}
	bck_->sky_shader->SetInt("SkyTexture", ENVIRONMENT_TEXTURE_UNIT);
	bck_->sky_shader->SetInt("SkyTextureEnabled", hasSkyTexture ? 1 : 0);

	glBindVertexArray(bck_->sky_vao);
	glDrawElements(GL_TRIANGLES, bck_->sky_index_count, GL_UNSIGNED_INT, nullptr);
	glBindVertexArray(0);

	glPolygonMode(GL_FRONT, previousPolygonMode[0]);
	glPolygonMode(GL_BACK, previousPolygonMode[1]);
	glCullFace(GL_BACK);
	glDisable(GL_CULL_FACE);
	glDepthMask(GL_TRUE);
}

static bool MaterialUsesScreenSpaceDisplacement(const HRL_Material* material)
{
    if (!material || !bck_) return false;
    if (material->shader_ != HRL_MESH_3D_SHADER && material->shader_ != HRL_SKINNED_3D_MESH_SHADER)
        return false;
    // The feature is opt-in. Merely assigning a displacement texture must not
    // change the rendering path; the material must explicitly enable it.
    auto enabledParam = material->intParams_.find(HRL_MATERIAL_PARAM_SS_DISPLACEMENT_ENABLED);
    if (enabledParam == material->intParams_.end() || enabledParam->second == 0)
        return false;

    auto texParam = material->textureParams_.find(HRL_MATERIAL_TEXTURE_SS_DISPLACEMENT_MAPPING);
    if (texParam == material->textureParams_.end() || texParam->second == HRL_INVALID_ID)
        return false;
    auto texIt = bck_->textures.find(texParam->second);
    return texIt != bck_->textures.end() && texIt->second && texIt->second->GetGL_ID() != 0;
}

static bool CaptureSceneColorForDisplacement(const GL_Scene* scene, GLuint destinationTexture)
{
    if (!scene || scene->fbo == 0 || destinationTexture == 0 || scene->width <= 0 || scene->height <= 0)
        return false;
    GLint previousReadFbo = 0;
    GLint previousTexture = 0;
    GLint previousReadBuffer = 0;
    GLint previousActiveTexture = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, destinationTexture);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, scene->width, scene->height);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previousTexture);
    glActiveTexture((GLenum)previousActiveTexture);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previousReadFbo);
    if (previousReadFbo != 0) glReadBuffer((GLenum)previousReadBuffer);
    return true;
}

static void DrawScreenSpaceDisplacementMeshes(HRL_id scene_id, const hrl_scene_t* scene, const FrustumPlaneSet& frustum)
{
    (void)scene_id;
    if (!scene || !ctx_ || !ctx_->viewport || !ctx_->viewport->camera_ || !bck_ ||
        (!bck_->ss_displacement_static_shader && !bck_->ss_displacement_skinned_shader) ||
        !bck_->post_textures[0])
        return;

    auto gpuSceneIt = bck_->gpu_scenes.find(scene_id);
    if (gpuSceneIt == bck_->gpu_scenes.end() || !gpuSceneIt->second)
        return;
    const GL_Scene* gpuScene = gpuSceneIt->second;

    struct DrawItem
    {
        HRL_id id;
        HRL_Mesh* mesh;
        HRL_Material* material;
        const GL33_Backend::MeshLOD_GPU* staticGpu;
        GL33_Backend::SkeletalMeshGPU* skeletalGpu;
        glm::mat4 model;
        float distance2;
    };

    std::vector<DrawItem> items;
    items.reserve(scene->meshes.size());
    const glm::vec3 cameraPos = ctx_->viewport->camera_->position_;

    for (const auto& [id, mesh] : scene->meshes)
    {
        if (!mesh || (mesh->type_ != HRL_3D_MESH && mesh->type_ != HRL_3D_SKELETAL_MESH) || !IsFiniteBounds(mesh))
            continue;
        if (mesh->material_ == HRL_INVALID_ID)
            continue;
        auto matIt = GetPrivateContext()->materials.find(mesh->material_);
        if (matIt == GetPrivateContext()->materials.end() || !MaterialUsesScreenSpaceDisplacement(matIt->second))
            continue;
        const glm::mat4 model = CalculateModelMatrix(mesh);
        if (mesh->type_ != HRL_3D_SKELETAL_MESH && !IsMeshVisible(mesh, model, frustum))
            continue;

        GL33_Backend::SkeletalMeshGPU* skeletalGpu = nullptr;
        const GL33_Backend::MeshLOD_GPU* staticGpu = nullptr;
        if (mesh->type_ == HRL_3D_SKELETAL_MESH)
        {
            auto gpuIt = bck_->skeletal_meshes.find(id);
            if (gpuIt == bck_->skeletal_meshes.end() || gpuIt->second.vao == 0)
                continue;
            skeletalGpu = &gpuIt->second;
            UploadSkeletalBones(static_cast<HRL_SkeletalMesh*>(mesh), *skeletalGpu);
        }
        else
        {
            auto gpuIt = bck_->meshes.find(id);
            if (gpuIt == bck_->meshes.end())
                continue;
            const int lod = SelectMeshLOD(mesh, model, ctx_->view_mat, ctx_->proj_mat, static_cast<float>(GetWindowHeight()));
            staticGpu = GetMeshLOD_GPU(gpuIt->second, lod);
            if (!staticGpu || staticGpu->vao == 0)
                continue;
        }

        const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.0f));
        items.push_back({id, mesh, matIt->second, staticGpu, skeletalGpu, model, glm::dot(center - cameraPos, center - cameraPos)});
    }

    std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) { return a.distance2 > b.distance2; });

    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);

    for (const DrawItem& item : items)
    {
        GL33_Shader* shader = item.skeletalGpu ? bck_->ss_displacement_skinned_shader : bck_->ss_displacement_static_shader;
        if (!shader)
            continue;
        shader->Use();
        ctx_->shader = shader;
        ctx_->bound_shader = shader;
        ctx_->bound_material = nullptr;
        shader->SetMat4("projection", ctx_->proj_mat);
        shader->SetMat4("view", ctx_->view_mat);
        shader->SetMat4("model", item.model);
        shader->SetVec4("UVRegion", {item.mesh->region_[0], item.mesh->region_[1], item.mesh->region_[2], item.mesh->region_[3]});
        shader->SetUint("uSpriteID", item.id);
        shader->SetVec2("uScreenSize", {static_cast<float>(gpuScene->width), static_cast<float>(gpuScene->height)});

        float strength = 12.0f;
        float scale = 1.0f;
        float opacity = 1.0f;
        if (auto it = item.material->floatParams_.find(HRL_MATERIAL_PARAM_SS_DISPLACEMENT_STRENGTH); it != item.material->floatParams_.end()) strength = it->second;
        if (auto it = item.material->floatParams_.find(HRL_MATERIAL_PARAM_SS_DISPLACEMENT_SCALE); it != item.material->floatParams_.end()) scale = it->second;
        if (auto it = item.material->floatParams_.find(HRL_MATERIAL_PARAM_SS_DISPLACEMENT_OPACITY); it != item.material->floatParams_.end()) opacity = it->second;
        shader->SetFloat("uStrength", strength);
        shader->SetFloat("uScale", scale);
        shader->SetFloat("uOpacity", opacity);
        int displacementEnabled = 0;
        if (auto it = item.material->intParams_.find(HRL_MATERIAL_PARAM_SS_DISPLACEMENT_ENABLED); it != item.material->intParams_.end())
            displacementEnabled = it->second != 0 ? 1 : 0;
        shader->SetInt("ss_displacement_enabled", displacementEnabled);

        auto texParam = item.material->textureParams_.find(HRL_MATERIAL_TEXTURE_SS_DISPLACEMENT_MAPPING);
        if (texParam == item.material->textureParams_.end())
            continue;
        auto texIt = bck_->textures.find(texParam->second);
        if (texIt == bck_->textures.end() || !texIt->second || texIt->second->GetGL_ID() == 0)
            continue;

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, bck_->post_textures[0]);
        shader->SetInt("uScene", 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texIt->second->GetGL_ID());
        shader->SetInt("uDisplacementMap", 1);

        if (item.skeletalGpu)
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, item.skeletalGpu->bone_ubo);
        glBindVertexArray(item.skeletalGpu ? item.skeletalGpu->vao : item.staticGpu->vao);
        if (item.skeletalGpu)
        {
            if (item.skeletalGpu->indexed) glDrawElements(GL_TRIANGLES, item.skeletalGpu->index_count, GL_UNSIGNED_INT, nullptr);
            else glDrawArrays(GL_TRIANGLES, 0, item.skeletalGpu->vertex_count);
        }
        else
        {
            if (item.staticGpu->indexed) glDrawElements(GL_TRIANGLES, item.staticGpu->index_count, GL_UNSIGNED_INT, nullptr);
            else glDrawArrays(GL_TRIANGLES, 0, item.staticGpu->vertex_count);
        }
    }

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
}

static void DrawOpaqueMeshes(HRL_id scene_id, const std::unordered_map<HRL_id, HRL_Mesh*>& meshes, HRL_EDebugView debug_view, const FrustumPlaneSet& frustum, bool skip_screen_space_displacement)
{
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);	glDepthFunc(GL_LESS);
	glDisable(GL_BLEND);

	GLboolean previousCullEnabled = glIsEnabled(GL_CULL_FACE);
	GLint previousCullFace = GL_BACK;
	glGetIntegerv(GL_CULL_FACE_MODE, &previousCullFace);
	GLint previousPolygonMode[2] = {GL_FILL, GL_FILL};
	glGetIntegerv(GL_POLYGON_MODE, previousPolygonMode);

	const bool wireframe = debug_view == HRL_DEBUG_VIEW_WIREFRAME;
	if (wireframe)
	{
		// Wireframe must show all triangle edges, regardless of imported winding.
		glDisable(GL_CULL_FACE);
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		glLineWidth(1.0f);
	}
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
	}

	struct DrawItem {
		HRL_id id;
		HRL_Mesh* mesh;
		HRL_Material* material;
		const GL33_Backend::MeshLOD_GPU* staticGpu;
		GL33_Backend::SkeletalMeshGPU* skeletalGpu;
		glm::mat4 model;
		float distance2;
		int lodLevel;
	};

	std::vector<DrawItem> visible;
	visible.reserve(meshes.size());
	const glm::vec3 cameraPos = ctx_->viewport && ctx_->viewport->camera_
		? ctx_->viewport->camera_->position_ : glm::vec3(0.f);

	for (const auto& [id, mesh] : meshes)
	{
		if (!mesh || mesh->type_ == HRL_SPRITE || !IsFiniteBounds(mesh))
			continue;

		const glm::mat4 model = CalculateModelMatrix(mesh);
		if (mesh->type_ != HRL_3D_SKELETAL_MESH && !IsMeshVisible(mesh, model, frustum))
			continue;

		auto materialIt = GetPrivateContext()->materials.find(mesh->material_);
		if (materialIt == GetPrivateContext()->materials.end())
			continue;
		if (skip_screen_space_displacement && debug_view == HRL_DEBUG_VIEW_NONE &&
			MaterialUsesScreenSpaceDisplacement(materialIt->second))
			continue;

		GL33_Backend::SkeletalMeshGPU* skeletalGpu = nullptr;
		const GL33_Backend::MeshLOD_GPU* staticGpu = nullptr;
		int lodLevel = 0;

		if (mesh->type_ == HRL_3D_SKELETAL_MESH)
		{
			auto gpuIt = bck_->skeletal_meshes.find(id);
			if (gpuIt == bck_->skeletal_meshes.end() || gpuIt->second.vao == 0)
				continue;
			skeletalGpu = &gpuIt->second;
			UploadSkeletalBones(static_cast<HRL_SkeletalMesh*>(mesh), *skeletalGpu);
		}
		else
		{
			auto gpuIt = bck_->meshes.find(id);
			if (gpuIt == bck_->meshes.end())
				continue;
			lodLevel = SelectMeshLOD(mesh, model, ctx_->view_mat, ctx_->proj_mat, (float)GetWindowHeight());
			staticGpu = GetMeshLOD_GPU(gpuIt->second, lodLevel);
			if (!staticGpu)
				continue;
			mesh->last_lod_level_ = lodLevel;
		}

		const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.f));
		const float distance2 = glm::dot(center - cameraPos, center - cameraPos);
		visible.push_back({id, mesh, materialIt->second, staticGpu, skeletalGpu, model, distance2, lodLevel});
	}

	std::sort(visible.begin(), visible.end(), [](const DrawItem& a, const DrawItem& b)
	{
		if (a.material->shader_ != b.material->shader_)
			return a.material->shader_ < b.material->shader_;
		if (a.mesh->material_ != b.mesh->material_)
			return a.mesh->material_ < b.mesh->material_;
		const GLuint vaoA = a.staticGpu ? a.staticGpu->vao : (a.skeletalGpu ? a.skeletalGpu->vao : 0);
		const GLuint vaoB = b.staticGpu ? b.staticGpu->vao : (b.skeletalGpu ? b.skeletalGpu->vao : 0);
		if (vaoA != vaoB)
			return vaoA < vaoB;
		return a.distance2 < b.distance2;
	});

	ctx_->bound_material = nullptr;
	ctx_->bound_shader = nullptr;

	for (const DrawItem& item : visible)
	{
		if (!wireframe)
		{
			if (MaterialIsTwoSided(item.material))
				glDisable(GL_CULL_FACE);
			else
			{
				glEnable(GL_CULL_FACE);
				glCullFace(GL_BACK);
			}
		}
		else
		{
			glDisable(GL_CULL_FACE);
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		}
		if (!BindMaterial(item.material, item.id, item.mesh, item.model))
			continue;

		if ((item.material->shader_ == HRL_MESH_3D_SHADER || item.material->shader_ == HRL_SKINNED_3D_MESH_SHADER) && g_gl33_gi)
			g_gl33_gi->ApplyToShader(scene_id, ctx_->shader);

		if ((item.material->shader_ == HRL_MESH_3D_SHADER || item.material->shader_ == HRL_SKINNED_3D_MESH_SHADER) &&
			debug_view == HRL_DEBUG_VIEW_LOD)
		{
			size_t triangleCount = item.mesh->triangle_count_;
			if (item.staticGpu && !item.mesh->lods_.empty() && (size_t)item.lodLevel < item.mesh->lods_.size())
				triangleCount = item.mesh->lods_[(size_t)item.lodLevel].indices.size() / 3u;
			constexpr size_t whiteThreshold = 250000;
			const float normalized = triangleCount >= whiteThreshold
				? 1.f
				: (float)(std::log((double)triangleCount + 1.0) / std::log((double)whiteThreshold + 1.0));
			ctx_->shader->SetVec3("DebugLODColor",
				triangleCount >= whiteThreshold ? glm::vec3(1.f) : glm::vec3(normalized, 1.f - normalized, 0.f));
		}

		if (item.skeletalGpu)
		{
			glBindBufferBase(GL_UNIFORM_BUFFER, 1, item.skeletalGpu->bone_ubo);
			glBindVertexArray(item.skeletalGpu->vao);
			if (item.skeletalGpu->indexed)
				glDrawElements(GL_TRIANGLES, item.skeletalGpu->index_count, GL_UNSIGNED_INT, nullptr);
			else
				glDrawArrays(GL_TRIANGLES, 0, item.skeletalGpu->vertex_count);
		}
		else if (item.staticGpu)
		{
			glBindVertexArray(item.staticGpu->vao);
			if (item.staticGpu->indexed)
				glDrawElements(GL_TRIANGLES, item.staticGpu->index_count, GL_UNSIGNED_INT, nullptr);
			else
				glDrawArrays(GL_TRIANGLES, 0, item.staticGpu->vertex_count);
		}
	}

	glBindVertexArray(0);
	glPolygonMode(GL_FRONT, previousPolygonMode[0]);
	glPolygonMode(GL_BACK, previousPolygonMode[1]);
	if (previousCullEnabled)
	{
		glEnable(GL_CULL_FACE);
		glCullFace(previousCullFace);
	}
	else
	{
		glDisable(GL_CULL_FACE);
	}
}



static void UploadSpriteBatch(const std::vector<SpriteBatchInstance>& instances)
{
	if (instances.empty()) return;
	const size_t required = instances.size() * sizeof(SpriteBatchInstance);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->sprite_instance_vbo);
	if (required > bck_->sprite_instance_capacity)
	{
		bck_->sprite_instance_capacity = std::max(required, sizeof(SpriteBatchInstance) * 256u);
	}
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bck_->sprite_instance_capacity, nullptr, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)required, instances.data());
}

static void DrawSprites(const std::unordered_map<HRL_id, HRL_Mesh*>& meshes, const FrustumPlaneSet& frustum)
{
	struct SpriteItem { HRL_id id; HRL_Mesh* mesh; HRL_Material* material; glm::mat4 model; float distance2; };
	std::vector<SpriteItem> sprites;
	sprites.reserve(meshes.size());
	const glm::vec3 cameraPos = ctx_->viewport->camera_->position_;
	for (const auto& [id, mesh] : meshes)
	{
		if (!mesh || mesh->type_ != HRL_SPRITE || mesh->material_ == HRL_INVALID_ID) continue;
		const glm::mat4 model = CalculateModelMatrix(mesh);
		if (!IsMeshVisible(mesh, model, frustum)) continue;
		auto matIt = GetPrivateContext()->materials.find(mesh->material_);
		if (matIt == GetPrivateContext()->materials.end()) continue;
		const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.f));
		const float distance2 = glm::dot(center - cameraPos, center - cameraPos);
		sprites.push_back({id, mesh, matIt->second, model, distance2});
	}
	std::stable_sort(sprites.begin(), sprites.end(), [](const SpriteItem& a, const SpriteItem& b)
	{
		if (std::abs(a.distance2 - b.distance2) > 1e-6f) return a.distance2 > b.distance2;
		return a.mesh->draw_order_ < b.mesh->draw_order_;
	});

	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	ctx_->bound_material = nullptr;
	ctx_->bound_shader = nullptr;
	std::vector<SpriteBatchInstance> instances;
	instances.reserve(std::min<size_t>(sprites.size(), MAX_SPRITE_BATCH_INSTANCES));

	size_t i = 0;
	while (i < sprites.size())
	{
		const SpriteItem& first = sprites[i];
		if (first.material->shader_ != HRL_SPRITE_SHADER)
		{
			if (BindMaterial(first.material, first.id, first.mesh, first.model))
			{
				glBindVertexArray(bck_->sprite_vao);
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
			}
			++i;
			continue;
		}

		size_t end = i + 1;
		while (end < sprites.size() && sprites[end].material == first.material && (end-i) < MAX_SPRITE_BATCH_INSTANCES) ++end;
		instances.clear();
		instances.reserve(end-i);
		for (size_t j=i; j<end; ++j)
		{
			SpriteBatchInstance instance;
			instance.model = sprites[j].model;
			instance.uvRegion = glm::vec4(sprites[j].mesh->region_[0], sprites[j].mesh->region_[1], sprites[j].mesh->region_[2], sprites[j].mesh->region_[3]);
			instance.spriteId = sprites[j].id;
			instances.push_back(instance);
		}
		if (BindMaterial(first.material, first.id, first.mesh, instances.front().model))
		{
			ctx_->shader->SetInt("uInstanced", 1);
			UploadSpriteBatch(instances);
			glBindVertexArray(bck_->sprite_instanced_vao);
			glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, (GLsizei)instances.size());
		}
		i = end;
	}
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}


struct VFXRenderInstance
{
	glm::mat4 model{1.f};
	glm::vec4 color{1.f};
};

static glm::mat4 MakeVFXSystemMatrix(const HRL_VFXSystem* system)
{
	glm::mat4 m(1.f);
	m = glm::translate(m, system->position_);
	m = glm::rotate(m, glm::radians(system->rotation_.x), glm::vec3(1.f,0.f,0.f));
	m = glm::rotate(m, glm::radians(system->rotation_.y), glm::vec3(0.f,1.f,0.f));
	m = glm::rotate(m, glm::radians(system->rotation_.z), glm::vec3(0.f,0.f,1.f));
	m = glm::scale(m, system->scale_);
	return m;
}

static glm::mat4 MakeVFXParticleModel(const HRL_VFXParticle& p, const glm::mat4& systemMatrix,
	const glm::mat4& billboardBasis, bool stretched, float stretch)
{
	glm::vec3 worldPos = p.position;
	if (systemMatrix != glm::mat4(1.f))
		worldPos = glm::vec3(systemMatrix * glm::vec4(worldPos, 1.f));

	glm::mat4 model = glm::translate(glm::mat4(1.f), worldPos);
	glm::mat4 orient = billboardBasis;
	if (stretched)
	{
		const glm::vec3 viewRight = glm::normalize(glm::vec3(billboardBasis[0]));
		const glm::vec3 viewUp = glm::normalize(glm::vec3(billboardBasis[1]));
		const glm::vec3 v = p.velocity;
		const float projectedLength = glm::length(v - glm::vec3(billboardBasis[2]) * glm::dot(v, glm::vec3(billboardBasis[2])));
		if (projectedLength > 1e-4f)
		{
			const glm::vec3 pv = glm::normalize(v - glm::vec3(billboardBasis[2]) * glm::dot(v, glm::vec3(billboardBasis[2])));
			const float angle = std::atan2(glm::dot(pv, viewUp), glm::dot(pv, viewRight));
			orient = glm::rotate(orient, angle, glm::vec3(billboardBasis[2]));
		}
	}
	else
	{
		orient = glm::rotate(orient, glm::radians(p.rotation.z), glm::vec3(billboardBasis[2]));
	}
	model *= orient;
	float sx = std::max(0.f, p.size.x);
	float sy = std::max(0.f, p.size.y);
	if (stretched) sx += glm::length(p.velocity) * std::max(0.f, stretch);
	model = glm::scale(model, glm::vec3(sx, sy, 1.f));
	return model;
}

static GLuint ResolveVFXTexture(const HRL_VFXEmitter* emitter)
{
	if (!emitter) return 0;
	HRL_id textureId = emitter->texture_;
	if (textureId == HRL_INVALID_ID && emitter->material_ != HRL_INVALID_ID)
	{
		auto matIt = GetPrivateContext()->materials.find(emitter->material_);
		if (matIt != GetPrivateContext()->materials.end() && matIt->second && !matIt->second->textureParams_.empty())
			textureId = matIt->second->textureParams_.begin()->second;
	}
	if (textureId != HRL_INVALID_ID)
	{
		auto texIt = bck_->textures.find(textureId);
		if (texIt != bck_->textures.end() && texIt->second)
			return texIt->second->GetGL_ID();
	}
	const auto fallbackIdIt = bck_->fallback_textures.find(ALBEDO_INT);
	if (fallbackIdIt != bck_->fallback_textures.end())
	{
		auto fallback = bck_->textures.find(fallbackIdIt->second);
		if (fallback != bck_->textures.end() && fallback->second) return fallback->second->GetGL_ID();
	}
	return 0;
}

static void InitVFXRenderer()
{
	if (!bck_) return;
	const HRL_Vertex3D vertices[4] = {
		{{-0.5f,-0.5f,0.f},{0.f,0.f,1.f},{0.f,0.f},{1.f,0.f,0.f},{0.f,1.f,0.f}},
		{{ 0.5f,-0.5f,0.f},{0.f,0.f,1.f},{1.f,0.f},{1.f,0.f,0.f},{0.f,1.f,0.f}},
		{{ 0.5f, 0.5f,0.f},{0.f,0.f,1.f},{1.f,1.f},{1.f,0.f,0.f},{0.f,1.f,0.f}},
		{{-0.5f, 0.5f,0.f},{0.f,0.f,1.f},{0.f,1.f},{1.f,0.f,0.f},{0.f,1.f,0.f}}
	};
	const GLuint indices[6] = {0,1,2,2,3,0};
	glGenVertexArrays(1, &bck_->vfx_vao);
	glGenBuffers(1, &bck_->vfx_vbo);
	glGenBuffers(1, &bck_->vfx_ebo);
	glGenBuffers(1, &bck_->vfx_instance_vbo);
	glBindVertexArray(bck_->vfx_vao);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->vfx_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	const GLsizei stride = static_cast<GLsizei>(sizeof(HRL_Vertex3D));
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(HRL_Vertex3D, position));
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(HRL_Vertex3D, uv));
	glEnableVertexAttribArray(2);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bck_->vfx_ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, bck_->vfx_instance_vbo);
	bck_->vfx_instance_capacity = sizeof(VFXRenderInstance) * 256u;
	glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bck_->vfx_instance_capacity), nullptr, GL_STREAM_DRAW);
	const GLsizei instanceStride = static_cast<GLsizei>(sizeof(VFXRenderInstance));
	for (int column = 0; column < 4; ++column)
	{
		const GLuint location = 3u + static_cast<GLuint>(column);
		glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, instanceStride,
			(const void*)(offsetof(VFXRenderInstance, model) + sizeof(glm::vec4) * column));
		glEnableVertexAttribArray(location);
		glVertexAttribDivisor(location, 1);
	}
	glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, instanceStride, (const void*)offsetof(VFXRenderInstance, color));
	glEnableVertexAttribArray(7);
	glVertexAttribDivisor(7, 1);
	glBindVertexArray(0);

	bck_->vfx_shader = new GL33_Shader();
	if (bck_->vfx_shader->GL33_Create(kVFXVertexShader, std::strlen(kVFXVertexShader), kVFXFragmentShader, std::strlen(kVFXFragmentShader)) != 0)
	{
		delete bck_->vfx_shader;
		bck_->vfx_shader = nullptr;
	}
}

static void UploadVFXInstances(const std::vector<VFXRenderInstance>& instances)
{
	const size_t required = std::max<size_t>(sizeof(VFXRenderInstance), instances.size() * sizeof(VFXRenderInstance));
	if (required > bck_->vfx_instance_capacity)
	{
		bck_->vfx_instance_capacity = std::max(required, bck_->vfx_instance_capacity * 2u);
		glBindBuffer(GL_ARRAY_BUFFER, bck_->vfx_instance_vbo);
		glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bck_->vfx_instance_capacity), nullptr, GL_STREAM_DRAW);
	}
	glBindBuffer(GL_ARRAY_BUFFER, bck_->vfx_instance_vbo);
	if (!instances.empty())
		glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(instances.size()*sizeof(VFXRenderInstance)), instances.data());
}

static void DrawVFX(const hrl_scene_t* scene)
{
	if (!scene || scene->vfx_systems.empty() || !bck_ || !bck_->vfx_shader || !ctx_ || !ctx_->viewport || !ctx_->viewport->camera_)
		return;

	struct Batch
	{
		HRL_EVFXBlendMode blend = HRL_VFX_BLEND_ALPHA;
		GLuint texture = 0;
		std::vector<VFXRenderInstance> instances;
	};

	std::vector<Batch> batches;
	batches.reserve(16);
	std::vector<std::pair<float, VFXRenderInstance>> sorted;
	const glm::mat4 inverseView = glm::inverse(ctx_->view_mat);
	const glm::mat4 billboardBasis = glm::mat4(glm::mat3(inverseView));
	const glm::vec3 cameraPos = ctx_->viewport->camera_->position_;

	for (const auto& [sid, system] : scene->vfx_systems)
	{
		(void)sid;
		if (!system || !system->enabled_) continue;
		const glm::mat4 systemMatrix = MakeVFXSystemMatrix(system);
		for (const auto& [eid, emitter] : system->emitters_)
		{
			(void)eid;
			if (!emitter || !emitter->enabled_ || emitter->render_mode_ == HRL_VFX_RENDER_MESH || emitter->particles_.empty()) continue;
			sorted.clear();
			sorted.reserve(emitter->particles_.size());
			for (const auto& particle : emitter->particles_)
			{
				VFXRenderInstance inst;
				inst.model = MakeVFXParticleModel(
					particle,
					emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL ? systemMatrix : glm::mat4(1.f),
					billboardBasis,
					emitter->render_mode_ == HRL_VFX_RENDER_STRETCHED_BILLBOARD,
					emitter->stretch_);
				inst.color = particle.color;
				const glm::vec3 pos = glm::vec3(inst.model[3]);
				sorted.emplace_back(glm::dot(pos - cameraPos, pos - cameraPos), inst);
			}
			if (emitter->blend_mode_ == HRL_VFX_BLEND_ALPHA)
				std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			Batch batch;
			batch.blend = emitter->blend_mode_;
			batch.texture = ResolveVFXTexture(emitter);
			batch.instances.reserve(sorted.size());
			for (const auto& pair : sorted) batch.instances.push_back(pair.second);
			if (!batch.instances.empty()) batches.push_back(std::move(batch));
		}
	}

	bck_->vfx_shader->Use();
	bck_->vfx_shader->SetMat4("projection", ctx_->proj_mat);
	bck_->vfx_shader->SetMat4("view", ctx_->view_mat);
	bck_->vfx_shader->SetInt("uInstanced", 1);
	bck_->vfx_shader->SetInt("uTexture", 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(bck_->vfx_vao);
	glDisable(GL_CULL_FACE);

	// Automatic VFX depth policy: depth-test against opaque scene geometry,
	// but never write depth. The application never configures depth for VFX.
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_FALSE);

	for (auto& batch : batches)
	{
		if (batch.instances.empty()) continue;
		glEnable(GL_BLEND);
		switch (batch.blend)
		{
		case HRL_VFX_BLEND_ADDITIVE: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
		case HRL_VFX_BLEND_MULTIPLY: glBlendFunc(GL_DST_COLOR, GL_ZERO); break;
		case HRL_VFX_BLEND_ALPHA:
		default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
		}
		glBindTexture(GL_TEXTURE_2D, batch.texture);
		bck_->vfx_shader->SetInt("uUseTexture", batch.texture != 0 ? 1 : 0);
		UploadVFXInstances(batch.instances);
		glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(batch.instances.size()));
	}

	// Mesh particles use their HRL static mesh as a template. The template's
	// scene transform is intentionally ignored; each particle gets its own
	// transform while reusing the mesh's material (or a compatible emitter
	// material override).
	ctx_->bound_material = nullptr;
	ctx_->bound_shader = nullptr;

	for (const auto& [sid, system] : scene->vfx_systems)
	{
		(void)sid;
		if (!system || !system->enabled_) continue;
		const glm::mat4 systemMatrix = MakeVFXSystemMatrix(system);

		for (const auto& [eid, emitter] : system->emitters_)
		{
			(void)eid;
			if (!emitter || !emitter->enabled_ ||
				emitter->render_mode_ != HRL_VFX_RENDER_MESH ||
				emitter->mesh_ == HRL_INVALID_ID ||
				emitter->particles_.empty())
				continue;

			auto meshIt = GetPrivateContext()->meshes.find(emitter->mesh_);
			if (meshIt == GetPrivateContext()->meshes.end() || !meshIt->second ||
				meshIt->second->type_ != HRL_3D_MESH)
				continue;

			auto gpuIt = bck_->meshes.find(emitter->mesh_);
			if (gpuIt == bck_->meshes.end()) continue;
			const HRL_Mesh& mesh = *meshIt->second;
			const int lodLevel = SelectMeshLOD(meshIt->second, CalculateModelMatrix(&mesh),
				ctx_->view_mat, ctx_->proj_mat, (float)GetWindowHeight());
			const GL33_Backend::MeshLOD_GPU* gpu = GetMeshLOD_GPU(gpuIt->second, lodLevel);
			if (!gpu || gpu->vao == 0 || gpu->vertex_count <= 0) continue;

			HRL_Material* material = nullptr;
			if (emitter->material_ != HRL_INVALID_ID)
			{
				auto matIt = GetPrivateContext()->materials.find(emitter->material_);
				if (matIt != GetPrivateContext()->materials.end() && matIt->second &&
					(matIt->second->shader_ == HRL_MESH_3D_SHADER))
					material = matIt->second;
			}
			if (!material && mesh.material_ != HRL_INVALID_ID)
			{
				auto matIt = GetPrivateContext()->materials.find(mesh.material_);
				if (matIt != GetPrivateContext()->materials.end()) material = matIt->second;
			}
			if (!material || (material->shader_ != HRL_MESH_3D_SHADER))
				continue;

			std::vector<std::pair<float, const HRL_VFXParticle*>> ordered;
			ordered.reserve(emitter->particles_.size());
			for (const auto& particle : emitter->particles_)
			{
				glm::vec3 worldPos = particle.position;
				if (emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL)
					worldPos = glm::vec3(systemMatrix * glm::vec4(worldPos, 1.f));
				const glm::vec3 d = worldPos - cameraPos;
				ordered.emplace_back(glm::dot(d, d), &particle);
			}
			if (emitter->blend_mode_ == HRL_VFX_BLEND_ALPHA)
				std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

			if (emitter->blend_mode_ == HRL_VFX_BLEND_ADDITIVE) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			else if (emitter->blend_mode_ == HRL_VFX_BLEND_MULTIPLY) glBlendFunc(GL_DST_COLOR, GL_ZERO);
			else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

			for (const auto& entry : ordered)
			{
				const HRL_VFXParticle& particle = *entry.second;
				glm::vec3 worldPos = particle.position;
				if (emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL)
					worldPos = glm::vec3(systemMatrix * glm::vec4(worldPos, 1.f));

				glm::mat4 model(1.f);
				model = glm::translate(model, worldPos);
				model = glm::translate(model, mesh.pivot_point_);
				model = glm::rotate(model, glm::radians(particle.rotation.x + emitter->mesh_rotation_offset_.x), glm::vec3(1.f, 0.f, 0.f));
				model = glm::rotate(model, glm::radians(particle.rotation.y + emitter->mesh_rotation_offset_.y), glm::vec3(0.f, 1.f, 0.f));
				model = glm::rotate(model, glm::radians(particle.rotation.z + emitter->mesh_rotation_offset_.z), glm::vec3(0.f, 0.f, 1.f));
				model = glm::translate(model, -mesh.pivot_point_);
				model = glm::scale(model, emitter->mesh_scale_ * std::max(0.f, particle.size.x));

				if (!BindMaterial(material, emitter->id_, &mesh, model)) continue;
				if (ctx_->shader) ctx_->shader->SetVec3("TintColor", glm::vec3(particle.color));

				if (emitter->texture_ != HRL_INVALID_ID && material->shader_ == HRL_MESH_3D_SHADER)
				{
					auto texIt = bck_->textures.find(emitter->texture_);
					if (texIt != bck_->textures.end() && texIt->second)
					{
						glActiveTexture(GL_TEXTURE0);
						glBindTexture(GL_TEXTURE_2D, texIt->second->GetGL_ID());
					}
				}


				glBindVertexArray(gpu->vao);
				if (gpu->indexed && gpu->index_count > 0) glDrawElements(GL_TRIANGLES, gpu->index_count, GL_UNSIGNED_INT, nullptr);
				else glDrawArrays(GL_TRIANGLES, 0, gpu->vertex_count);
			}
		}
	}

	bck_->vfx_shader->Use();
	bck_->vfx_shader->SetInt("uInstanced", 1);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDisable(GL_BLEND);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
}

static glm::vec3 GizmoAxisRenderer(const HRL_Gizmo* g, int axis)
{
    glm::vec3 v(0.f); v[axis] = 1.f;
    if (g->space_ == HRL_GIZMO_SPACE_WORLD) return v;
    glm::mat4 r(1.f);
    r = glm::rotate(r, glm::radians(g->rotation_.x), glm::vec3(1,0,0));
    r = glm::rotate(r, glm::radians(g->rotation_.y), glm::vec3(0,1,0));
    r = glm::rotate(r, glm::radians(g->rotation_.z), glm::vec3(0,0,1));
    return glm::normalize(glm::vec3(r * glm::vec4(v,0.f)));
}

static float GizmoSizeRenderer(const HRL_Gizmo* g, const HRL_Viewport* viewport)
{
    if (!g || !viewport || !viewport->camera_) return 1.f;
    if (!g->use_screen_size_) return std::max(0.0001f, g->world_size_);
    const float winH = std::max(1.f, static_cast<float>(GetWindowHeight()) * viewport->height_);
    const float depth = std::max(0.001f, std::abs(glm::vec3(ctx_->view_mat * glm::vec4(g->position_,1.f)).z));
    if (viewport->camera_->type_ == HRL_PERSPECTIVE) {
        const float worldHeight = 2.f * depth * std::tan(glm::radians(viewport->camera_->value_) * 0.5f);
        return std::max(0.0001f, g->screen_size_pixels_ * worldHeight / winH);
    }
    return std::max(0.0001f, g->screen_size_pixels_ * viewport->camera_->value_ / winH);
}

static void GizmoPushLine(DebugRenderer& d, const glm::vec3& a, const glm::vec3& b, const glm::vec4& c)
{
    d.lines.emplace_back(a.x,a.y,a.z,c.x,c.y,c.z);
    d.lines.emplace_back(b.x,b.y,b.z,c.x,c.y,c.z);
}

static void GizmoPushArc(DebugRenderer& d, const glm::vec3& center, const glm::vec3& normal,
    const glm::vec3& u, float radius, float start, float sweep, int segments, const glm::vec4& color)
{
    glm::vec3 v = glm::normalize(glm::cross(normal, u));
    sweep = glm::clamp(sweep, 0.17453292519943295769f, 2.9670597283903604f);
    segments = std::max(6, segments);
    glm::vec3 prev = center + (std::cos(start)*u + std::sin(start)*v) * radius;
    for (int i=1; i<=segments; ++i) {
        const float a = start + sweep * float(i) / float(segments);
        glm::vec3 cur = center + (std::cos(a)*u + std::sin(a)*v) * radius;
        GizmoPushLine(d, prev, cur, color);
        prev = cur;
    }
}

static void DrawGizmos(const hrl_scene_t* scene, const HRL_Viewport* viewport, HRL_id viewport_id)
{
    if (!scene || !viewport || !viewport->camera_ || viewport_id == HRL_INVALID_ID || scene->gizmos.empty()) return;
    DebugRenderer overlay;
    const float winW = static_cast<float>(GetWindowWidth());
    const float winH = static_cast<float>(GetWindowHeight());
    glViewport(
        static_cast<GLint>(viewport->x_ * winW),
        static_cast<GLint>(viewport->y_ * winH),
        std::max(1, static_cast<int>(viewport->width_ * winW)),
        std::max(1, static_cast<int>(viewport->height_ * winH)));

    for (const auto& [id, g] : scene->gizmos) {
        (void)id;
        if (!g || !g->visible_ || g->viewport_ != viewport_id) continue;
        const float size = GizmoSizeRenderer(g, viewport);
        const glm::vec3 center = g->position_;

        if ((static_cast<int>(g->mode_) & static_cast<int>(HRL_GIZMO_MODE_TRANSLATE)) && g->show_translate_) {
            for (int axis=0; axis<3; ++axis) {
                if (!(g->translate_axes_ & (1<<axis))) continue;
                const glm::vec3 dir = GizmoAxisRenderer(g, axis);
                const glm::vec3 end = center + dir * size;
                const glm::vec4 col = (g->hovered_operation_ == HRL_GIZMO_OPERATION_TRANSLATE && g->hovered_part_ == static_cast<HRL_EGizmoPart>(axis+1)) ? g->hover_color_ : g->axis_colors_[axis];
                GizmoPushLine(overlay, center, end, col);
                glm::vec3 side = glm::cross(dir, glm::normalize(viewport->camera_->position_ - center));
                if (glm::length(side) < 1e-4f) side = glm::cross(dir, glm::vec3(0,1,0));
                side = glm::normalize(side);
                const float head = size * 0.14f;
                GizmoPushLine(overlay, end, end - dir*head + side*head*0.55f, col);
                GizmoPushLine(overlay, end, end - dir*head - side*head*0.55f, col);
            }
            if (g->hovered_operation_ == HRL_GIZMO_OPERATION_TRANSLATE && g->hovered_part_ == HRL_GIZMO_PART_CENTER) {
                const glm::vec3 forward = GetForwardVector(viewport->camera_->rotation_);
                glm::vec3 right = glm::cross(forward, glm::vec3(0,1,0));
                if (glm::length(right) < 1e-4f) right = glm::cross(forward, glm::vec3(1,0,0));
                right = glm::normalize(right);
                const glm::vec3 up = glm::normalize(glm::cross(right, forward));
                const float box=size*0.16f;
                const glm::vec4 col=g->hover_color_;
                GizmoPushLine(overlay, center-right*box-up*box, center+right*box-up*box, col);
                GizmoPushLine(overlay, center+right*box-up*box, center+right*box+up*box, col);
                GizmoPushLine(overlay, center+right*box+up*box, center-right*box+up*box, col);
                GizmoPushLine(overlay, center-right*box+up*box, center-right*box-up*box, col);
            }
        }

        // Scale intentionally mirrors translation geometry: one axis per handle, with a square endpoint.
        if ((static_cast<int>(g->mode_) & static_cast<int>(HRL_GIZMO_MODE_SCALE)) && g->show_scale_) {
            for (int axis=0; axis<3; ++axis) {
                if (!(g->scale_axes_ & (1<<axis))) continue;
                const glm::vec3 dir=GizmoAxisRenderer(g,axis);
                glm::vec3 side=(std::abs(dir.y)<0.9f)?glm::normalize(glm::cross(dir,glm::vec3(0,1,0))):glm::normalize(glm::cross(dir,glm::vec3(1,0,0)));
                const glm::vec3 up=glm::normalize(glm::cross(side,dir));
                const glm::vec3 end=center+dir*size;
                const float h=size*0.10f;
                const glm::vec4 col=(g->hovered_operation_==HRL_GIZMO_OPERATION_SCALE&&g->hovered_part_==static_cast<HRL_EGizmoPart>(axis+1))?g->hover_color_:g->axis_colors_[axis];
                GizmoPushLine(overlay,center,end,col);
                GizmoPushLine(overlay,end-side*h-up*h,end+side*h-up*h,col);
                GizmoPushLine(overlay,end+side*h-up*h,end+side*h+up*h,col);
                GizmoPushLine(overlay,end+side*h+up*h,end-side*h+up*h,col);
                GizmoPushLine(overlay,end-side*h+up*h,end-side*h-up*h,col);
            }
        }

        // Rotation uses three separate quarter wheels. They remain independently pickable.
        if ((static_cast<int>(g->mode_) & static_cast<int>(HRL_GIZMO_MODE_ROTATE)) && g->show_rotate_) {
            const float sweep=glm::radians(glm::clamp(g->rotate_arc_degrees_,15.f,170.f));
            for (int axis=0; axis<3; ++axis) {
                if (!(g->rotate_axes_ & (1<<axis))) continue;
                const glm::vec3 n=GizmoAxisRenderer(g,axis);
                // Keep every quarter-wheel aligned to the next positive axis:
                // X: +Y -> +Z, Y: +Z -> +X, Z: +X -> +Y.
                const glm::vec3 u=GizmoAxisRenderer(g,(axis+1)%3);
                const float start=0.f;
                const glm::vec4 col=(g->hovered_operation_==HRL_GIZMO_OPERATION_ROTATE&&g->hovered_part_==static_cast<HRL_EGizmoPart>(axis+1))?g->hover_color_:g->axis_colors_[axis];
                GizmoPushArc(overlay,center,n,u,size,start,sweep,20,col);
            }
        }

        if ((static_cast<int>(g->mode_) & (static_cast<int>(HRL_GIZMO_MODE_ROTATE) | static_cast<int>(HRL_GIZMO_MODE_SCALE))) != 0) {
            const float s=size*0.06f;
            const glm::vec4 col=g->center_color_;
            GizmoPushLine(overlay,center-glm::vec3(s,0,0),center+glm::vec3(s,0,0),col);
            GizmoPushLine(overlay,center-glm::vec3(0,s,0),center+glm::vec3(0,s,0),col);
            GizmoPushLine(overlay,center-glm::vec3(0,0,s),center+glm::vec3(0,0,s),col);
        }
    }
    if (!overlay.lines.empty())
        GL33_DrawGizmoOverlay(overlay, std::max(1.5f, GetPrivateContext()->debug_line_thickness + 0.5f));
}

static void GL33_DrawGizmoOverlay(const DebugRenderer& renderer, float line_thickness)
{
    if (renderer.lines.empty()) return;
    auto shaderIt=bck_->shaders.find(HRL_DEBUG_SHADER);
    if(shaderIt==bck_->shaders.end()) return;
    GL33_Shader* shader=shaderIt->second;
    shader->Use();
    shader->SetMat4("projection",ctx_->proj_mat);
    shader->SetMat4("view",ctx_->view_mat);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(std::max(1.0f,line_thickness));
    glBindVertexArray(bck_->vao[BUFFER_DEBUG]);
    glBindBuffer(GL_ARRAY_BUFFER,bck_->vbo[BUFFER_DEBUG]);
    const size_t bytes=renderer.lines.size()*sizeof(DebugVertex);
    if(bytes>ctx_->current_debug_buffer_size){
        glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)bytes,renderer.lines.data(),GL_STREAM_DRAW);
        ctx_->current_debug_buffer_size=bytes;
    }else{
        glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)bytes,renderer.lines.data());
    }
    glDrawArrays(GL_LINES,0,(GLsizei)renderer.lines.size());
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

static const char* MeshDebugTypeName(const HRL_Mesh* mesh)
{
	if (!mesh) return "Unknown";
	switch (mesh->type_)
	{
	case HRL_3D_MESH: return "Mesh3D";
	case HRL_3D_SKELETAL_MESH: return "SkeletalMesh";
	case HRL_2D_MESH: return "Mesh2D";
	case HRL_SPRITE: return "Sprite";
	default: return "Unknown";
	}
}

static glm::vec3 ComputeMeshAverageNormal(const HRL_Mesh* mesh, int lodLevel)
{
	glm::vec3 sum(0.0f);
	size_t count = 0;
	if (!mesh) return glm::vec3(0.0f, 1.0f, 0.0f);
	if (mesh->type_ == HRL_3D_SKELETAL_MESH)
	{
		const auto* skinned = static_cast<const HRL_SkeletalMesh*>(mesh);
		for (const auto& v : skinned->vertices_)
		{
			const glm::vec3 n(v.vertex.normal[0], v.vertex.normal[1], v.vertex.normal[2]);
			if (glm::dot(n, n) > 1e-10f && std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z))
			{
				sum += glm::normalize(n);
				++count;
			}
		}
	}
	else if (!mesh->lods_.empty())
	{
		const int level = std::clamp(lodLevel, 0, static_cast<int>(mesh->lods_.size()) - 1);
		for (const auto& v : mesh->lods_[(size_t)level].vertices)
		{
			const glm::vec3 n(v.normal[0], v.normal[1], v.normal[2]);
			if (glm::dot(n, n) > 1e-10f && std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z))
			{
				sum += glm::normalize(n);
				++count;
			}
		}
	}
	if (count == 0 || glm::dot(sum, sum) <= 1e-12f)
		return glm::vec3(0.0f, 1.0f, 0.0f);
	return glm::normalize(sum / static_cast<float>(count));
}

static void BuildMeshDebugInfoText(const HRL_Mesh* mesh, std::string& out)
{
	out.clear();
	if (!mesh) return;
	const int lodLevel = std::max(0, mesh->last_lod_level_);
	size_t vertexCount = 0;
	size_t triangleCount = mesh->triangle_count_;
	if (mesh->type_ == HRL_3D_SKELETAL_MESH)
	{
		const auto* skinned = static_cast<const HRL_SkeletalMesh*>(mesh);
		vertexCount = skinned->vertices_.size();
		triangleCount = !skinned->indices_.empty() ? skinned->indices_.size() / 3u : skinned->vertices_.size() / 3u;
	}
	else if (!mesh->lods_.empty())
	{
		const size_t level = std::min<size_t>((size_t)lodLevel, mesh->lods_.size() - 1);
		const auto& lod = mesh->lods_[level];
		vertexCount = lod.vertices.size();
		triangleCount = !lod.indices.empty() ? lod.indices.size() / 3u : lod.vertices.size() / 3u;
	}
	const glm::vec3 n = ComputeMeshAverageNormal(mesh, lodLevel);
	std::ostringstream ss;
	ss << MeshDebugTypeName(mesh) << "\n"
	   << "Triangles: " << triangleCount << "\n"
	   << "Vertices: " << vertexCount << "\n"
	   << "LOD: " << lodLevel << "\n"
	   << std::fixed << std::setprecision(2)
	   << "Normal: (" << n.x << ", " << n.y << ", " << n.z << ")\n"
	   << "Position: (" << mesh->position_.x << ", " << mesh->position_.y << ", " << mesh->position_.z << ")\n"
	   << "Scale: (" << mesh->scale_.x << ", " << mesh->scale_.y << ", " << mesh->scale_.z << ")\n"
	   << "Material: " << static_cast<unsigned long long>(mesh->material_);
	if (mesh->type_ == HRL_3D_SKELETAL_MESH)
	{
		const auto* skinned = static_cast<const HRL_SkeletalMesh*>(mesh);
		ss << "\nBones: " << skinned->bones_.size()
		   << "\nAnimation: " << skinned->current_animation_;
	}
	out = ss.str();
}

static void DrawMeshDebugInfoTexts(const hrl_scene_t* scene, const HRL_Viewport* viewport)
{
	if (!scene || !viewport || !bck_ || !bck_->ui_shader || !ctx_)
		return;
	HRL_Context* privateContext = GetPrivateContext();
	if (!privateContext || privateContext->fonts.empty())
		return;

	// Debug mesh information is a screen-space UI pass.
	// Do not inherit the viewport/FBO state left by the 3D/gizmo passes.
	const float winW = static_cast<float>(GetWindowWidth());
	const float winH = static_cast<float>(GetWindowHeight());
	const int viewportX = static_cast<int>(viewport->x_ * winW);
	const int viewportY = static_cast<int>(viewport->y_ * winH);
	const int viewportW = std::max(1, static_cast<int>(viewport->width_ * winW));
	const int viewportH = std::max(1, static_cast<int>(viewport->height_ * winH));

	// The scene has already been composited to the screen immediately before
	// this function is called. Mesh info must therefore be drawn over the
	// default framebuffer, using the owning viewport.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(viewportX, viewportY, viewportW, viewportH);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	HRL_id fontId = scene->debug_mesh_info_font;
	if (fontId == HRL_INVALID_ID || privateContext->fonts.find(fontId) == privateContext->fonts.end())
		fontId = privateContext->fonts.begin()->first;

	const float textSize = scene->debug_mesh_info_text_size;
	for (const auto& [meshId, mesh] : scene->meshes)
	{
		if (!mesh || (mesh->type_ != HRL_3D_MESH && mesh->type_ != HRL_3D_SKELETAL_MESH))
			continue;
		const glm::vec4 clip = ctx_->proj_mat * ctx_->view_mat * glm::vec4(glm::vec3(CalculateModelMatrix(mesh) * glm::vec4(mesh->bounds_center_, 1.0f)), 1.0f);
		if (!std::isfinite(clip.w) || clip.w <= 1e-6f)
			continue;
		const glm::vec2 ndc = glm::vec2(clip) / clip.w;
		if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || ndc.x < -1.15f || ndc.x > 1.15f || ndc.y < -1.15f || ndc.y > 1.15f)
			continue;

		std::string info;
		BuildMeshDebugInfoText(mesh, info);
		std::ostringstream withId;
		withId << "ID: " << static_cast<unsigned long long>(meshId) << "\n" << info;
		info = withId.str();

		auto& cache = bck_->debug_mesh_info_textures[meshId];
		if (cache.texture == HRL_INVALID_ID || cache.font != fontId || cache.size != textSize || cache.text != info || !HRL_IsValidTexture(cache.texture))
		{
			if (cache.texture != HRL_INVALID_ID && HRL_IsValidTexture(cache.texture))
				HRL_DeleteTexture(cache.texture);
			cache = {};
			cache.font = fontId;
			cache.size = textSize;
			cache.text = info;
			cache.texture = HRL_InternalCreateSDFTextTexture(info.c_str(), fontId, textSize);
			if (cache.texture == HRL_INVALID_ID)
				continue;
			HRL_SetTextureMinFilter(cache.texture, HRL_FILTER_LINEAR);
			HRL_SetTextureMagFilter(cache.texture, HRL_FILTER_LINEAR);
			HRL_GetTextureSize(cache.texture, &cache.width, &cache.height);
		}
		if (cache.texture == HRL_INVALID_ID || cache.width <= 0 || cache.height <= 0)
			continue;

		const float vpW = static_cast<float>(viewportW);
		const float vpH = static_cast<float>(viewportH);

		// HRL_SetDebugMeshInfoTextSize() is a screen-space size, in pixels,
		// and refers to ONE LINE of text, not the complete generated bitmap.
		// The SDF texture contains all lines, so scale its total height by
		// the number of lines while preserving the bitmap aspect ratio.
		const float requestedLineHeight = std::max(1.0f, textSize);
		const float lineCount = 1.0f +
			static_cast<float>(std::count(info.begin(), info.end(), '\n'));
		const float requestedPixelHeight =
			requestedLineHeight * lineCount;
		const float bitmapAspect = static_cast<float>(cache.width) /
			static_cast<float>(cache.height);
		const float textW =
			(requestedPixelHeight * bitmapAspect) / vpW;
		const float textH = requestedPixelHeight / vpH;
		const float anchorX = 0.5f + 0.5f * ndc.x;
		const float anchorY = 0.5f - 0.5f * ndc.y;
		const float px = anchorX - textW * 0.5f;
		const float py = anchorY - textH - 4.0f / vpH;

		bck_->ui_shader->Use();
		bck_->ui_shader->SetMat4("projection", glm::ortho(0.f, 1.f, 1.f, 0.f, -1.f, 1.f));
		bck_->ui_shader->SetVec4("uTintColor", scene->debug_mesh_info_text_color);
		bck_->ui_shader->SetInt("uSDFText", 1);
		glActiveTexture(GL_TEXTURE0);
		auto texIt = bck_->textures.find(cache.texture);
		if (texIt == bck_->textures.end() || !texIt->second)
			continue;
		glBindTexture(GL_TEXTURE_2D, texIt->second->GetGL_ID());
		bck_->ui_shader->SetInt("uTexture", 0);
		const float vertices[16] = {
			px,        py,        0.0f, 1.0f,
			px+textW,  py,        1.0f, 1.0f,
			px+textW,  py+textH,  1.0f, 0.0f,
			px,        py+textH,  0.0f, 0.0f,
		};
		glBindVertexArray(bck_->vao[BUFFER_UI]);
		glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_UI]);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
	}

	for (auto it = bck_->debug_mesh_info_textures.begin(); it != bck_->debug_mesh_info_textures.end(); )
	{
		if (privateContext->meshes.find(it->first) == privateContext->meshes.end())
		{
			if (it->second.texture != HRL_INVALID_ID && HRL_IsValidTexture(it->second.texture))
				HRL_DeleteTexture(it->second.texture);
			it = bck_->debug_mesh_info_textures.erase(it);
		}
		else ++it;
	}
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glViewport(0, 0, static_cast<GLsizei>(winW), static_cast<GLsizei>(winH));
}

void DrawWidgets(const std::unordered_map<HRL_id, HRL_Widget*>& widgets, const HRL_Viewport* viewport)
{
	if (!viewport || widgets.empty())
		return;

	bck_->ui_shader->Use();
	const float winW = static_cast<float>(GetWindowWidth());
	const float winH = static_cast<float>(GetWindowHeight());
	const int viewportX = static_cast<int>(viewport->x_ * winW);
	const int viewportY = static_cast<int>(viewport->y_ * winH);
	const int viewportW = std::max(1, static_cast<int>(viewport->width_ * winW));
	const int viewportH = std::max(1, static_cast<int>(viewport->height_ * winH));

	// Widget positions/sizes are normalized to the owning viewport.
	const glm::mat4 ui_proj = glm::ortho(0.f, 1.f, 1.f, 0.f, -1.f, 1.f);
	glViewport(viewportX, viewportY, viewportW, viewportH);

	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	std::vector<HRL_Widget*> sortedWidgets;
	sortedWidgets.reserve(widgets.size());
	for (const auto& [id, widget] : widgets)
	{
		(void)id;
		if (widget)
			sortedWidgets.push_back(widget);
	}
	std::sort(sortedWidgets.begin(), sortedWidgets.end(), [](const HRL_Widget* a, const HRL_Widget* b) {
		if (a->GetZIndex() != b->GetZIndex())
			return a->GetZIndex() < b->GetZIndex();
		return a->GetId() < b->GetId();
	});

	HRL_Context* privateContext = GetPrivateContext();
	const float mouseX = privateContext->mouseX;
	const float mouseY = privateContext->mouseY;
	const float vpX = static_cast<float>(viewportX);
	const float vpY = static_cast<float>(viewportY);
	const float vpW = static_cast<float>(viewportW);
	const float vpH = static_cast<float>(viewportH);

	// First update hover state for every widget without dispatching the mouse button.
	for (HRL_Widget* w : sortedWidgets)
	{
		w->UpdateInput(mouseX, mouseY, false, false, false, vpX, vpY, vpW, vpH);
	}

	// A press is captured by the highest-z interactive widget under the cursor.
	if (privateContext->mouseLeftPressed && privateContext->mouseCaptureWidget == HRL_INVALID_ID && privateContext->mouseCaptureGizmo == HRL_INVALID_ID)
	{
		for (auto it = sortedWidgets.rbegin(); it != sortedWidgets.rend(); ++it)
		{
			HRL_Widget* w = *it;
			if (w->IsPointerInteractive() && w->IsVisible() && w->IsEnabled() && w->IsHovered())
			{
				privateContext->mouseCaptureWidget = w->GetId();
				break;
			}
		}
	}

	for (HRL_Widget* w : sortedWidgets)
	{
		const bool isCapture = (w->GetId() == privateContext->mouseCaptureWidget);
		w->UpdateInput(
			mouseX, mouseY,
			isCapture ? privateContext->mouseLeftDown : false,
			isCapture ? privateContext->mouseLeftPressed : false,
			isCapture ? privateContext->mouseLeftReleased : false,
			vpX, vpY, vpW, vpH);
		w->Logic();

		std::vector<HRL_Widget::WidgetDrawInfos> geometries;
		w->GetDrawInfos(geometries);

		// A world-projected widget keeps the regular 2D geometry/size of the
		// widget, but its anchor point is replaced by the 3D point projected
		// through the current viewport camera.
		if (w->IsWorldPositionEnabled())
		{
			if (!ctx_ || !viewport->camera_)
				continue;
			const glm::vec4 clip = ctx_->proj_mat * ctx_->view_mat * glm::vec4(w->GetWorldPosition(), 1.0f);
			if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.w) || clip.w <= 1e-6f)
				continue;
			const glm::vec2 ndc = glm::vec2(clip) / clip.w;
			if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y))
				continue;
			const glm::vec2 projected(0.5f + 0.5f * ndc.x, 0.5f - 0.5f * ndc.y);
			const glm::vec2 screenAnchor = w->GetPosition();
			const glm::vec2 delta = projected - screenAnchor;
			for (auto& g : geometries)
			{
				g.px += delta.x;
				g.py += delta.y;
			}
		}

		for (const auto& g : geometries)
		{
			if (g.sx <= 0.0f || g.sy <= 0.0f || g.a <= 0.0f)
				continue;

			auto texture_it = bck_->textures.find(g.texture);
			if (texture_it == bck_->textures.end())
			{
				auto fallback_it = bck_->textures.find(bck_->fallback_textures[ALBEDO_INT]);
				if (fallback_it == bck_->textures.end())
					continue;
				texture_it = fallback_it;
			}

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, texture_it->second->GetGL_ID());
			bck_->ui_shader->SetInt("uTexture", 0);

			const float vertices[16] = {
				g.px,        g.py,        0.0f, 1.0f,
				g.px+g.sx,   g.py,        1.0f, 1.0f,
				g.px+g.sx,   g.py+g.sy,   1.0f, 0.0f,
				g.px,        g.py+g.sy,   0.0f, 0.0f,
			};

			bck_->ui_shader->SetMat4("projection", ui_proj);
			bck_->ui_shader->SetVec4("uTintColor", {g.r, g.g, g.b, g.a});
			bck_->ui_shader->SetInt("uSDFText", g.sdf ? 1 : 0);

			glBindVertexArray(bck_->vao[BUFFER_UI]);
			glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_UI]);
			glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
			glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
		}
	}

	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glViewport(0, 0, static_cast<GLsizei>(winW), static_cast<GLsizei>(winH));

	if (privateContext->mouseLeftReleased)
		privateContext->mouseCaptureWidget = HRL_INVALID_ID;
}


static uint32_t PNG_CRC32(const unsigned char* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static uint32_t PNG_Adler32(const std::vector<unsigned char>& data)
{
    uint32_t a = 1u, b = 0u;
    for (unsigned char c : data)
    {
        a += c;
        if (a >= 65521u) a -= 65521u;
        b += a;
        if (b >= 65521u) b -= 65521u;
    }
    return (b << 16) | a;
}

static void PNG_AppendU32BE(std::vector<unsigned char>& out, uint32_t value)
{
    out.push_back((unsigned char)((value >> 24) & 0xFFu));
    out.push_back((unsigned char)((value >> 16) & 0xFFu));
    out.push_back((unsigned char)((value >> 8) & 0xFFu));
    out.push_back((unsigned char)(value & 0xFFu));
}

static void PNG_AppendChunk(std::vector<unsigned char>& png, const char type[4], const std::vector<unsigned char>& payload)
{
    PNG_AppendU32BE(png, (uint32_t)payload.size());
    size_t typeOffset = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), payload.begin(), payload.end());
    uint32_t crc = PNG_CRC32(png.data() + typeOffset, 4 + payload.size());
    PNG_AppendU32BE(png, crc);
}

static bool WritePNG_RGBA8(const char* path, int width, int height, const std::vector<unsigned char>& rgbaTopDown)
{
    if (!path || width <= 0 || height <= 0 || rgbaTopDown.size() != (size_t)width * (size_t)height * 4u)
        return false;

    std::vector<unsigned char> raw;
    raw.reserve((size_t)height * ((size_t)width * 4u + 1u));
    for (int y = 0; y < height; ++y)
    {
        raw.push_back(0);
        const unsigned char* row = rgbaTopDown.data() + (size_t)y * (size_t)width * 4u;
        raw.insert(raw.end(), row, row + (size_t)width * 4u);
    }

    std::vector<unsigned char> zlib;
    zlib.push_back(0x78); zlib.push_back(0x01);
    size_t offset = 0;
    while (offset < raw.size())
    {
        const size_t remaining = raw.size() - offset;
        const uint16_t blockLen = (uint16_t)std::min<size_t>(remaining, 65535u);
        const bool finalBlock = (offset + blockLen == raw.size());
        zlib.push_back(finalBlock ? 0x01 : 0x00);
        zlib.push_back((unsigned char)(blockLen & 0xFFu));
        zlib.push_back((unsigned char)((blockLen >> 8) & 0xFFu));
        const uint16_t nlen = (uint16_t)~blockLen;
        zlib.push_back((unsigned char)(nlen & 0xFFu));
        zlib.push_back((unsigned char)((nlen >> 8) & 0xFFu));
        zlib.insert(zlib.end(), raw.begin() + (ptrdiff_t)offset, raw.begin() + (ptrdiff_t)(offset + blockLen));
        offset += blockLen;
    }
    PNG_AppendU32BE(zlib, PNG_Adler32(raw));

    std::vector<unsigned char> png = {0x89, 'P','N','G',0x0D,0x0A,0x1A,0x0A};
    std::vector<unsigned char> ihdr;
    PNG_AppendU32BE(ihdr, (uint32_t)width);
    PNG_AppendU32BE(ihdr, (uint32_t)height);
    ihdr.push_back(8);
    ihdr.push_back(6);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    PNG_AppendChunk(png, "IHDR", ihdr);
    PNG_AppendChunk(png, "IDAT", zlib);
    PNG_AppendChunk(png, "IEND", {});

    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write((const char*)png.data(), (std::streamsize)png.size());
    return (bool)file;
}

static void ApplyScreenSpaceReflections(GL_Scene* scene, const hrl_scene_t* hrlScene, const glm::mat4& view,
    const glm::mat4& projection, int viewportX, int viewportY, int viewportWidth, int viewportHeight, GLuint& srcIndex)
{
    if (!scene || !hrlScene || !hrlScene->screen_space_reflections_enabled || !bck_->ssr_shader || !scene->depth_texture)
        return;
    const int src = (srcIndex == bck_->post_textures[0]) ? 0 : 1;
    const int dst = 1 - src;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, bck_->post_fbo[src]);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bck_->post_fbo[dst]);
    glBlitFramebuffer(0,0,scene->width,scene->height,0,0,scene->width,scene->height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, bck_->post_fbo[dst]);
    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    bck_->ssr_shader->Use();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, bck_->post_textures[src]); bck_->ssr_shader->SetInt("uScene",0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, scene->depth_texture); bck_->ssr_shader->SetInt("uDepth",1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, scene->textures[4]); bck_->ssr_shader->SetInt("uNormal",2);
    bck_->ssr_shader->SetMat4("uViewProjection", projection * view);
    bck_->ssr_shader->SetMat4("uInvViewProjection", glm::inverse(projection * view));
    bck_->ssr_shader->SetVec3("uCameraPos", ctx_->viewport->camera_->position_);
    bck_->ssr_shader->SetVec2("uViewportOrigin", glm::vec2((float)viewportX/scene->width,(float)viewportY/scene->height));
    bck_->ssr_shader->SetVec2("uViewportSize", glm::vec2((float)viewportWidth/scene->width,(float)viewportHeight/scene->height));
    bck_->ssr_shader->SetVec2("uScreenSize", glm::vec2((float)scene->width,(float)scene->height));
    bck_->ssr_shader->SetFloat("uStrength", hrlScene->screen_space_reflections_strength);
    bck_->ssr_shader->SetFloat("uMaxDistance", hrlScene->screen_space_reflections_max_distance);
    bck_->ssr_shader->SetFloat("uThickness", hrlScene->screen_space_reflections_thickness);
    bck_->ssr_shader->SetFloat("uFadeStart", hrlScene->screen_space_reflections_fade_start);
    bck_->ssr_shader->SetFloat("uFadeEnd", hrlScene->screen_space_reflections_fade_end);
    bck_->ssr_shader->SetInt("uSteps", (int)hrlScene->screen_space_reflections_steps);
    glBindVertexArray(bck_->vao[BUFFER_QUAD]); glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,nullptr); glBindVertexArray(0);
    srcIndex = bck_->post_textures[dst];
}

static void ApplyDecals(GL_Scene* scene, const hrl_scene_t* hrlScene, const glm::mat4& view,
    const glm::mat4& projection, int viewportX, int viewportY, int viewportWidth, int viewportHeight, GLuint& srcIndex)
{
    (void)view; (void)projection;
    if (!scene || !hrlScene || !bck_->decal_shader || hrlScene->decals.empty() || !scene->depth_texture)
        return;
    std::vector<HRL_Decal*> decals;
    decals.reserve(hrlScene->decals.size());
    for (const auto& [id,d] : hrlScene->decals) { (void)id; if (d && d->enabled && d->texture != HRL_INVALID_ID) decals.push_back(d); }
    if (decals.empty()) return;

    std::sort(decals.begin(), decals.end(), [](const HRL_Decal* a, const HRL_Decal* b){ return a->id_ < b->id_; });
    for (HRL_Decal* decal : decals)
    {
        auto texIt = bck_->textures.find(decal->texture);
        if (texIt == bck_->textures.end() || !texIt->second || texIt->second->GetGL_ID()==0) continue;
        const int src = (srcIndex == bck_->post_textures[0]) ? 0 : 1;
        const int dst = 1 - src;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, bck_->post_fbo[src]);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bck_->post_fbo[dst]);
        glBlitFramebuffer(0,0,scene->width,scene->height,0,0,scene->width,scene->height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER,bck_->post_fbo[dst]);
        glViewport(viewportX,viewportY,viewportWidth,viewportHeight);
        glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
        bck_->decal_shader->Use();
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,bck_->post_textures[src]); bck_->decal_shader->SetInt("uScene",0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,scene->depth_texture); bck_->decal_shader->SetInt("uDepth",1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D,scene->textures[4]); bck_->decal_shader->SetInt("uNormal",2);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D,texIt->second->GetGL_ID()); bck_->decal_shader->SetInt("uDecal",3);
        glm::mat4 model(1.0f);
        model = glm::translate(model,decal->position);
        model = glm::rotate(model,glm::radians(decal->rotation.x),glm::vec3(1,0,0));
        model = glm::rotate(model,glm::radians(decal->rotation.y),glm::vec3(0,1,0));
        model = glm::rotate(model,glm::radians(decal->rotation.z),glm::vec3(0,0,1));
        model = glm::scale(model,decal->size);
        bck_->decal_shader->SetMat4("uInvViewProjection", glm::inverse(projection*view));
        bck_->decal_shader->SetMat4("uDecalInvModel", glm::inverse(model));
        bck_->decal_shader->SetVec3("uDecalNormal", glm::normalize(glm::vec3(model * glm::vec4(0,1,0,0))));
        bck_->decal_shader->SetVec4("uColor", glm::vec4(decal->color,1.0f));
        bck_->decal_shader->SetFloat("uOpacity",decal->opacity);
        bck_->decal_shader->SetFloat("uNormalFadeMin",decal->normal_fade_min);
        bck_->decal_shader->SetFloat("uNormalFadeMax",decal->normal_fade_max);
        bck_->decal_shader->SetVec2("uViewportOrigin",glm::vec2((float)viewportX/scene->width,(float)viewportY/scene->height));
        bck_->decal_shader->SetVec2("uViewportSize",glm::vec2((float)viewportWidth/scene->width,(float)viewportHeight/scene->height));
        bck_->decal_shader->SetVec2("uScreenSize",glm::vec2((float)scene->width,(float)scene->height));
        glBindVertexArray(bck_->vao[BUFFER_QUAD]); glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,nullptr); glBindVertexArray(0);
        srcIndex=bck_->post_textures[dst];
    }
}

static void ApplyAmbientOcclusion(GL_Scene* scene, const hrl_scene_t* hrlScene, const glm::mat4& view,
    const glm::mat4& projection, int viewportX, int viewportY, int viewportWidth, int viewportHeight, GLuint& srcIndex)
{
    if (!scene || !hrlScene || !hrlScene->ambient_occlusion_enabled || !bck_->ambient_occlusion_shader || !scene->depth_texture)
        return;
    const int src = (srcIndex == bck_->post_textures[0]) ? 0 : 1;
    const int dst = 1 - src;
    glBindFramebuffer(GL_FRAMEBUFFER, bck_->post_fbo[dst]);
    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    bck_->ambient_occlusion_shader->Use();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, bck_->post_textures[src]);
    bck_->ambient_occlusion_shader->SetInt("uScene", 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, scene->depth_texture);
    bck_->ambient_occlusion_shader->SetInt("uDepth", 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, scene->textures[4]);
    bck_->ambient_occlusion_shader->SetInt("uNormal", 2);
    bck_->ambient_occlusion_shader->SetMat4("uInvViewProjection", glm::inverse(projection * view));
    bck_->ambient_occlusion_shader->SetVec3("uCameraPos", ctx_->viewport->camera_->position_);
    bck_->ambient_occlusion_shader->SetVec2("uViewportOrigin", glm::vec2((float)viewportX/(float)scene->width,(float)viewportY/(float)scene->height));
    bck_->ambient_occlusion_shader->SetVec2("uViewportSize", glm::vec2((float)viewportWidth/(float)scene->width,(float)viewportHeight/(float)scene->height));
    bck_->ambient_occlusion_shader->SetVec2("uScreenSize", glm::vec2((float)scene->width,(float)scene->height));
    bck_->ambient_occlusion_shader->SetFloat("uRadius", hrlScene->ambient_occlusion_radius);
    bck_->ambient_occlusion_shader->SetFloat("uBias", hrlScene->ambient_occlusion_bias);
    bck_->ambient_occlusion_shader->SetFloat("uStrength", hrlScene->ambient_occlusion_strength);
    bck_->ambient_occlusion_shader->SetFloat("uPower", hrlScene->ambient_occlusion_power);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    glBindVertexArray(bck_->vao[BUFFER_QUAD]);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    srcIndex = bck_->post_textures[dst];
}

static void ApplySceneEffects(GL_Scene* scene, const hrl_scene_t* hrlScene, const glm::mat4& view,
    const glm::mat4& projection, int viewportX, int viewportY, int viewportWidth, int viewportHeight, GLuint& srcIndex)
{
    if (!scene || !hrlScene || !bck_->scene_effect_shader || !scene->depth_texture)
        return;
    const bool hasVolumetricFog = HasActiveVolumetricFog(hrlScene);
    if (!hasVolumetricFog && !hrlScene->god_rays.enabled)
        return;

    const int src = (srcIndex == bck_->post_textures[0]) ? 0 : 1;
    const int dst = 1 - src;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, bck_->post_fbo[src]);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bck_->post_fbo[dst]);
    glBlitFramebuffer(viewportX, viewportY, viewportX + viewportWidth, viewportY + viewportHeight,
        viewportX, viewportY, viewportX + viewportWidth, viewportY + viewportHeight,
        GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, bck_->post_fbo[dst]);
    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    bck_->scene_effect_shader->Use();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bck_->post_textures[src]);
    bck_->scene_effect_shader->SetInt("uScene", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scene->textures[1]);
    bck_->scene_effect_shader->SetInt("uBrightScene", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, scene->depth_texture);
    bck_->scene_effect_shader->SetInt("uDepth", 2);

    bck_->scene_effect_shader->SetMat4("uInvViewProjection", glm::inverse(projection * view));
    bck_->scene_effect_shader->SetVec3("uCameraPos", ctx_->viewport->camera_->position_);
    bck_->scene_effect_shader->SetVec2("uViewportOrigin", glm::vec2((float)viewportX / (float)scene->width, (float)viewportY / (float)scene->height));
    bck_->scene_effect_shader->SetVec2("uViewportSize", glm::vec2((float)viewportWidth / (float)scene->width, (float)viewportHeight / (float)scene->height));
    bck_->scene_effect_shader->SetVec2("uScreenSize", glm::vec2((float)scene->width, (float)scene->height));

    const auto& globalFog = hrlScene->global_volumetric_fog;
    bck_->scene_effect_shader->SetInt("uGlobalVolumetricFogEnabled", globalFog.enabled ? 1 : 0);
    bck_->scene_effect_shader->SetVec3("uGlobalFogColor", globalFog.color);
    bck_->scene_effect_shader->SetFloat("uGlobalFogDensity", globalFog.density);
    bck_->scene_effect_shader->SetInt("uGlobalFogSteps", (int)globalFog.steps);

    int fogCount = 0;
    HRL_uint volumetricFogSteps = 4;
    for (const auto& [fogId, fog] : hrlScene->volumetric_fogs)
    {
        (void)fogId;
        if (!fog || !fog->enabled || fog->density <= 0.0f || fog->radius <= 0.0f)
            continue;
        if (fogCount >= HRL_MAX_VOLUMETRIC_FOGS)
            break;

        bck_->scene_effect_shader->SetVec3("uFogPositions[" + std::to_string(fogCount) + "]", fog->position);
        bck_->scene_effect_shader->SetFloat("uFogRadii[" + std::to_string(fogCount) + "]", fog->radius);
        bck_->scene_effect_shader->SetVec3("uFogColors[" + std::to_string(fogCount) + "]", fog->color);
        bck_->scene_effect_shader->SetFloat("uFogDensities[" + std::to_string(fogCount) + "]", fog->density);
        volumetricFogSteps = std::max(volumetricFogSteps, fog->steps);
        ++fogCount;
    }
    bck_->scene_effect_shader->SetInt("uVolumetricFogCount", fogCount);
    bck_->scene_effect_shader->SetInt("uVolumetricFogSteps", (int)volumetricFogSteps);

    // Volumetric clouds are a scene-wide atmospheric layer. Use the first
    // directional light as the sun when available, otherwise a neutral
    // downward light keeps the effect usable without any light objects.
    glm::vec3 cloudSunDirection = glm::normalize(glm::vec3(0.35f, -0.85f, 0.2f));
    glm::vec3 cloudLightColor = hrlScene->volumetric_cloud_light_color;
    float cloudLightIntensity = hrlScene->volumetric_cloud_light_intensity;
    for (const auto& [cloudLightId, cloudLight] : hrlScene->lights)
    {
        (void)cloudLightId;
        if (!cloudLight || cloudLight->type_ != HRL_DIRECTIONAL_LIGHT || cloudLight->intensity_ <= 0.0f)
            continue;
        cloudSunDirection = GetLightDirection(cloudLight);
        cloudLightColor = cloudLight->color_;
        cloudLightIntensity *= std::max(0.0f, cloudLight->intensity_);
        break;
    }
    const auto& clouds = *hrlScene;
    // Clouds are rendered by the dedicated volumetric_cloud_shader pass below.
    bck_->scene_effect_shader->SetInt("uVolumetricCloudEnabled", 0);
    bck_->scene_effect_shader->SetFloat("uCloudCoverage", clouds.volumetric_cloud_coverage);
    bck_->scene_effect_shader->SetFloat("uCloudDensity", clouds.volumetric_cloud_density);
    bck_->scene_effect_shader->SetFloat("uCloudHeightMin", clouds.volumetric_cloud_height_min);
    bck_->scene_effect_shader->SetFloat("uCloudHeightMax", clouds.volumetric_cloud_height_max);
    bck_->scene_effect_shader->SetFloat("uCloudScale", clouds.volumetric_cloud_scale);
    bck_->scene_effect_shader->SetFloat("uCloudDetail", clouds.volumetric_cloud_detail);
    bck_->scene_effect_shader->SetVec2("uCloudWind", clouds.volumetric_cloud_wind);
    bck_->scene_effect_shader->SetFloat("uCloudWindSpeed", clouds.volumetric_cloud_wind_speed);
    bck_->scene_effect_shader->SetVec3("uCloudColor", clouds.volumetric_cloud_color);
    bck_->scene_effect_shader->SetVec3("uCloudLightColor", cloudLightColor);
    bck_->scene_effect_shader->SetFloat("uCloudLightAbsorption", clouds.volumetric_cloud_light_absorption);
    bck_->scene_effect_shader->SetFloat("uCloudLightIntensity", cloudLightIntensity);
    bck_->scene_effect_shader->SetInt("uCloudSteps", (int)clouds.volumetric_cloud_steps);
    bck_->scene_effect_shader->SetFloat("uCloudMaxDistance", clouds.volumetric_cloud_max_distance);
    bck_->scene_effect_shader->SetVec3("uCloudSunDirection", cloudSunDirection);
    static const auto cloudStartTime = std::chrono::steady_clock::now();
    const float cloudTime = static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now() - cloudStartTime).count());
    bck_->scene_effect_shader->SetFloat("uCloudTime", cloudTime);

    const auto& rays = hrlScene->god_rays;
    glm::vec2 lightUV(0.5f);
    glm::vec4 lightClip = projection * view * glm::vec4(rays.position, 1.0f);
    if (std::abs(lightClip.w) > 1e-5f)
        lightUV = glm::vec2(lightClip.x, lightClip.y) / lightClip.w * 0.5f + 0.5f;
    bck_->scene_effect_shader->SetInt("uGodRaysEnabled", rays.enabled ? 1 : 0);
    bck_->scene_effect_shader->SetVec2("uGodRaysLightUV", lightUV);
    bck_->scene_effect_shader->SetVec3("uGodRaysColor", rays.color);
    bck_->scene_effect_shader->SetFloat("uGodRaysDensity", rays.density);
    bck_->scene_effect_shader->SetFloat("uGodRaysDecay", rays.decay);
    bck_->scene_effect_shader->SetFloat("uGodRaysWeight", rays.weight);
    bck_->scene_effect_shader->SetInt("uGodRaysSamples", (int)rays.samples);

    glBindVertexArray(bck_->vao[BUFFER_QUAD]);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

    srcIndex = bck_->post_textures[dst];
}

static void DrawPostProcessQuad(GLuint src_texture, GLuint bright_texture, HRL_PostProcess* pp)
{
	auto mat_it = GetPrivateContext()->materials.find(pp->material_);
	if (mat_it == GetPrivateContext()->materials.end())
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "DrawPostProcessQuad: invalid material ID");
		return;
	}
	HRL_Material* mat = mat_it->second;

	auto shader_it = bck_->shaders.find(mat->shader_);
	if (shader_it == bck_->shaders.end())
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "DrawPostProcessQuad: invalid shader ID");
		return;
	}
	GL33_Shader* shader = shader_it->second;
	shader->Use();

	// HRL-owned post-process time for animated default-shader effects (e.g. film grain).
	static const auto postStartTime = std::chrono::steady_clock::now();
	const double postTimeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - postStartTime).count();
	shader->SetFloat("uTime", static_cast<float>(postTimeSeconds));

	//texture de la passe précédente (ou de la scène)
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src_texture);
	shader->SetInt("uScene", 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, bright_texture);
	shader->SetInt("uBrightScene", 1);

	shader->SetVec2("uScreenSize",{ctx_->viewport->width_ * (float)GetWindowWidth(), ctx_->viewport->height_ * (float)GetWindowHeight()});

	//uniforms utilisateur (ex: saturation, brightness...)
	for (const auto& [name, value] : mat->floatParams_)
		shader->SetFloat(name, value);
	for (const auto& [name, value] : mat->intParams_)
		shader->SetInt(name, value);
	for (const auto& [name, value] : mat->vec2Params_)
		shader->SetVec2(name, value);
	for (const auto& [name, value] : mat->vec3Params_)
		shader->SetVec3(name, value);
	for (const auto& [name, value] : mat->vec4Params_)
		shader->SetVec4(name, value);

	glBindVertexArray(bck_->vao[BUFFER_QUAD]);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
}


// SCREENSHOT
void GL33_TakeScreenshot(HRL_id scene, const char* target_path)
{
    if (!bck_ || !target_path || *target_path == '\0')
        return;

    auto gpuIt = bck_->gpu_scenes.find(scene);
    if (gpuIt == bck_->gpu_scenes.end() || !gpuIt->second)
    {
        SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_TakeScreenshot: invalid scene ID");
        return;
    }

    const GL_Scene* gpuScene = gpuIt->second;
    const auto ctxIt = GetPrivateContext()->scenes.find(scene);
    const bool sceneOnScreen = (ctxIt != GetPrivateContext()->scenes.end() && ctxIt->second && ctxIt->second->draw_on_screen != 0);
    const int width = std::max(1, gpuScene->width);
    const int height = std::max(1, gpuScene->height);
    std::vector<unsigned char> pixels((size_t)width * (size_t)height * 4u);

    GLint oldReadFbo = 0;
    GLint oldDrawFbo = 0;
    GLint oldPack = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &oldPack);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    if (sceneOnScreen)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
    }
    else
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gpuScene->fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)oldReadFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)oldDrawFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, oldPack);

    const size_t rowBytes = (size_t)width * 4u;
    for (int y = 0; y < height / 2; ++y)
    {
        unsigned char* top = pixels.data() + (size_t)y * rowBytes;
        unsigned char* bottom = pixels.data() + (size_t)(height - 1 - y) * rowBytes;
        for (size_t x = 0; x < rowBytes; ++x)
            std::swap(top[x], bottom[x]);
    }

    if (!WritePNG_RGBA8(target_path, width, height, pixels))
        SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "GL33_TakeScreenshot: failed to write PNG");
}

//EFFECTS
void GL33_FogPropertyChanged(HRL_id scene, hrl_fog_t* fog_ptr) {}



//MATRICES
static glm::mat4 CalculateProjectionMatrix()
{
	//taille absolue du viewport width et height (on prend en compte la taille de la fenetre et la taille relative du viewport HRL)
	float viewportWidth  = (float)GetWindowWidth() * ctx_->viewport->width_;
	float viewportHeight = (float)GetWindowHeight() * ctx_->viewport->height_;

	//on evite la division par 0
	if (viewportHeight < 1e-3f)
	{
		viewportHeight = 1.f;
	}

	//calcul du ratio largeur/hauteur du viewport
	float aspect = viewportWidth / viewportHeight;

	glm::mat4 proj;
	if (ctx_->viewport->camera_->type_ == HRL_PERSPECTIVE)
	{
		proj = glm::perspective(glm::radians(ctx_->viewport->camera_->value_), aspect, ctx_->viewport->camera_->near_plane_, ctx_->viewport->camera_->far_plane_);
	}
	else
	{
		//on calcule la taille en hauteur d'abord, puis on fait le calcul de la largeur en fonction de la hauteur et de l'aspect
		float halfHeight = ctx_->viewport->camera_->value_ * 0.5f;
		float halfWidth  = halfHeight * aspect;
		proj = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight,
											ctx_->viewport->camera_->near_plane_, ctx_->viewport->camera_->far_plane_);
	}
	return proj;
}
static glm::mat4 CalculateViewMatrix()
{
	//position et vue de la camera
	glm::mat4 view = glm::lookAt(
		ctx_->viewport->camera_->position_,
		ctx_->viewport->camera_->position_ + GetForwardVector(ctx_->viewport->camera_->rotation_),
		GetUpVector(ctx_->viewport->camera_->rotation_)
	);
	return view;
}



//LIGHTS
void GL33_UpdateLights(const std::vector<HRL_Light*>& _lights)
{
	//on commence par creer le tableau de GL_Lights
	GL_Light gpu_lights[MAX_LIGHTS]{};
	for (int i = 0; i < MAX_LIGHTS; ++i)
	{
		gpu_lights[i].shadowMatrix = glm::mat4(1.f);
		gpu_lights[i].shadowParams = glm::vec4(0.f, 0.f, -1.f, 0.f);
		gpu_lights[i].shadowStrength = 1.f;
	}
	size_t count = 0;

	//on rempli le tableau
	for (const auto& light: _lights)
	{
		if (count >= MAX_LIGHTS)
		{
			break;
		}

		gpu_lights[count].type = light->type_;
		gpu_lights[count].intensity = light->intensity_;
		gpu_lights[count].attenuation = light->attenuation_;

		gpu_lights[count].innerCutoff = std::cos(glm::radians(light->innerCutoff));

		gpu_lights[count].position = light->position_;
		gpu_lights[count].outerCutoff = std::cos(glm::radians(light->outerCutoff));

		// yaw = rotation.y, pitch = rotation.x
		glm::vec3 dir;
		dir.x = cos(glm::radians(light->rotation_.y)) * cos(glm::radians(light->rotation_.x));
		dir.y = sin(glm::radians(light->rotation_.x));
		dir.z = sin(glm::radians(light->rotation_.y)) * cos(glm::radians(light->rotation_.x));
		gpu_lights[count].rotation = glm::normalize(dir);

		gpu_lights[count].padding3 = 0.f;

		gpu_lights[count].color = light->color_;
		gpu_lights[count].shadowStrength = light->shadow_strength_;

		++count;
	}

	//on passe les données à opengl
	glBindBuffer(GL_UNIFORM_BUFFER, bck_->ubo[UBO_LIGHTS]);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)sizeof(gpu_lights), gpu_lights);
}







//SHADOWS / ANTI-ALIASING
static glm::vec3 GetLightDirection(const HRL_Light* light)
{
	const float pitch = glm::radians(light->rotation_.x);
	const float yaw = glm::radians(light->rotation_.y);
	glm::vec3 dir(
		std::cos(yaw) * std::cos(pitch),
		std::sin(pitch),
		std::sin(yaw) * std::cos(pitch));
	if (glm::length(dir) < 1e-5f)
		return glm::vec3(0.f, -1.f, 0.f);
	return glm::normalize(dir);
}

static void DestroyLightShadowResources(HRL_id light_id)
{
	auto it = bck_->shadow_maps.find(light_id);
	if (it == bck_->shadow_maps.end())
		return;
	if (it->second.depth_2d)
		glDeleteTextures(1, &it->second.depth_2d);
	if (it->second.depth_cube)
		glDeleteTextures(1, &it->second.depth_cube);
	if (it->second.fbo)
		glDeleteFramebuffers(1, &it->second.fbo);
	bck_->shadow_maps.erase(it);
}

static bool EnsureShadowResource(HRL_Light* light)
{
	if (!light || !light->cast_shadows_)
		return false;

	const bool point = light->type_ == HRL_POINT_LIGHT;
	const int resolution = std::max(128, std::min(light->shadow_resolution_, 4096));
	auto it = bck_->shadow_maps.find(light->id_);
	if (it != bck_->shadow_maps.end() && it->second.resolution == resolution && it->second.is_point == point)
		return true;

	if (it != bck_->shadow_maps.end())
		DestroyLightShadowResources(light->id_);

	GL33_Backend::ShadowGPU shadow;
	shadow.resolution = resolution;
	shadow.is_point = point;
	glGenFramebuffers(1, &shadow.fbo);

	if (point)
	{
		glGenTextures(1, &shadow.depth_cube);
		glBindTexture(GL_TEXTURE_CUBE_MAP, shadow.depth_cube);
		for (int face = 0; face < 6; ++face)
		{
			glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_DEPTH_COMPONENT24,
				resolution, resolution, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
		}
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	}
	else
	{
		glGenTextures(1, &shadow.depth_2d);
		glBindTexture(GL_TEXTURE_2D, shadow.depth_2d);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
			resolution, resolution, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		const float border[] = {1.f, 1.f, 1.f, 1.f};
		glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, shadow.fbo);
	if (point)
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_CUBE_MAP_POSITIVE_X, shadow.depth_cube, 0);
	else
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow.depth_2d, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		if (shadow.depth_2d) glDeleteTextures(1, &shadow.depth_2d);
		if (shadow.depth_cube) glDeleteTextures(1, &shadow.depth_cube);
		if (shadow.fbo) glDeleteFramebuffers(1, &shadow.fbo);
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GL33: failed to create shadow framebuffer");
		return false;
	}

	bck_->shadow_maps.emplace(light->id_, shadow);
	return true;
}

static void RenderShadowCasters(hrl_scene_t* scene, const glm::mat4& lightViewProjection, const glm::mat4& lightView, const glm::mat4& lightProjection, int shadowResolution,
	GL33_Shader* shader, bool pointLight, const glm::vec3& lightPosition,
	float farPlane, int face)
{
	if (!scene || !shader)
		return;

	GL33_Shader* activeShader = nullptr;
	const FrustumPlaneSet frustum = BuildFrustum(lightViewProjection);
	for (const auto& [id, mesh] : scene->meshes)
	{
		if (!mesh || mesh->type_ == HRL_SPRITE)
			continue;

		const bool skeletal = mesh->type_ == HRL_3D_SKELETAL_MESH;
		GL33_Backend::SkeletalMeshGPU* skeletalGpu = nullptr;
		const GL33_Backend::MeshLOD_GPU* staticGpu = nullptr;
		if (skeletal)
		{
			auto it = bck_->skeletal_meshes.find(id);
			if (it == bck_->skeletal_meshes.end() || it->second.vao == 0)
				continue;
			skeletalGpu = &it->second;
			UploadSkeletalBones(static_cast<HRL_SkeletalMesh*>(mesh), *skeletalGpu);
		}
		else
		{
			auto it = bck_->meshes.find(id);
			if (it == bck_->meshes.end())
				continue;
			const glm::mat4 model = CalculateModelMatrix(mesh);
			if (pointLight)
			{
				const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.f));
				const float maxScale = std::max({std::abs(mesh->scale_.x), std::abs(mesh->scale_.y), std::abs(mesh->scale_.z)});
				const float radius = mesh->bounds_radius_ * std::max(maxScale, 1e-6f);
				const float limit = farPlane + radius;
				if (glm::dot(center - lightPosition, center - lightPosition) > limit * limit)
					continue;
			}
			else if (!IsMeshVisible(mesh, model, frustum))
				continue;
			const int lodLevel = SelectMeshLOD(mesh, model, lightView, lightProjection, (float)shadowResolution);
			staticGpu = GetMeshLOD_GPU(it->second, lodLevel);
			if (!staticGpu)
				continue;
		}

		const glm::mat4 model = CalculateModelMatrix(mesh);
		if (!skeletal && !pointLight && !IsMeshVisible(mesh, model, frustum))
			continue;

		GL33_Shader* meshShader = shader;
		if (skeletal)
			meshShader = pointLight ? bck_->shadow_skeletal_point_shader : bck_->shadow_skeletal_2d_shader;
		if (!meshShader)
			continue;
		if (meshShader != activeShader)
		{
			meshShader->Use();
			meshShader->SetMat4("lightSpaceMatrix", lightViewProjection);
			meshShader->SetMat4("lightViewProjection", lightViewProjection);
			meshShader->SetVec3("lightPosition", lightPosition);
			meshShader->SetFloat("farPlane", farPlane);
			activeShader = meshShader;
		}
		meshShader->SetMat4("model", model);
		const auto shadowMaterialIt = GetPrivateContext()->materials.find(mesh->material_);
		const bool shadowTwoSided = shadowMaterialIt != GetPrivateContext()->materials.end() && MaterialIsTwoSided(shadowMaterialIt->second);
		if (shadowTwoSided)
			glDisable(GL_CULL_FACE);
		else
		{
			glEnable(GL_CULL_FACE);
			glCullFace(GL_FRONT);
		}

		if (skeletal)
		{
			glBindBufferBase(GL_UNIFORM_BUFFER, 1, skeletalGpu->bone_ubo);
			glBindVertexArray(skeletalGpu->vao);
			if (skeletalGpu->indexed)
				glDrawElements(GL_TRIANGLES, skeletalGpu->index_count, GL_UNSIGNED_INT, nullptr);
			else
				glDrawArrays(GL_TRIANGLES, 0, skeletalGpu->vertex_count);
		}
		else
		{
			glBindVertexArray(staticGpu->vao);
			if (staticGpu->indexed)
				glDrawElements(GL_TRIANGLES, staticGpu->index_count, GL_UNSIGNED_INT, nullptr);
			else
				glDrawArrays(GL_TRIANGLES, 0, staticGpu->vertex_count);
		}
	}
	DrawLandscapeShadowCasters(scene, lightViewProjection, lightView, lightProjection, shadowResolution, pointLight, lightPosition, farPlane, face);
	glBindVertexArray(0);
	(void)face;
}



static void PrepareSceneShadows(hrl_scene_t* scene, HRL_id scene_id)
{
	if (!scene || !bck_->shadow_2d_shader || !bck_->shadow_point_shader)
		return;
	auto& activeSlots = bck_->active_shadow_slots_by_scene[scene_id];
	if (!scene->shadows_dirty)
		return;
	activeSlots.clear();

	int slot = 0;
	for (const auto& [id, light] : scene->lights)
	{
		if (!light || light->type_ == HRL_SKY_LIGHT || !light->cast_shadows_ || slot >= MAX_SHADOW_SLOTS)
			continue;
		if (!EnsureShadowResource(light))
			continue;

		auto shadowIt = bck_->shadow_maps.find(id);
		if (shadowIt == bck_->shadow_maps.end())
			continue;

		const int resolution = shadowIt->second.resolution;
		glBindFramebuffer(GL_FRAMEBUFFER, shadowIt->second.fbo);
		glViewport(0, 0, resolution, resolution);
		glEnable(GL_DEPTH_TEST);
		glDepthMask(GL_TRUE);
		glDepthFunc(GL_LESS);
		glDisable(GL_BLEND);
		glEnable(GL_CULL_FACE);
		glCullFace(GL_FRONT);
		// Point-light shadows use a manually computed linear radial depth in the
		// fragment shader. Large polygon offsets in this space can visibly detach
		// the shadow from the caster ("double"/ghosted silhouettes), especially
		// on animated meshes. Receiver-side bias is applied in the lighting shader.
		// Keep the existing polygon offset for projected 2D shadow maps.
		if (light->type_ == HRL_POINT_LIGHT)
			glDisable(GL_POLYGON_OFFSET_FILL);
		else
		{
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(2.0f, 4.0f);
		}

		if (light->type_ == HRL_POINT_LIGHT)
		{
			constexpr float nearPlane = 0.05f;
			constexpr float farPlane = 100.f;
			const glm::mat4 projection = glm::perspective(glm::radians(90.f), 1.f, nearPlane, farPlane);
			const glm::vec3 pos = light->position_;
			const std::array<glm::vec3, 6> directions = {
				glm::vec3( 1, 0, 0), glm::vec3(-1, 0, 0),
				glm::vec3( 0, 1, 0), glm::vec3( 0,-1, 0),
				glm::vec3( 0, 0, 1), glm::vec3( 0, 0,-1)
			};
			const std::array<glm::vec3, 6> ups = {
				glm::vec3(0,-1,0), glm::vec3(0,-1,0),
				glm::vec3(0,0,1), glm::vec3(0,0,-1),
				glm::vec3(0,-1,0), glm::vec3(0,-1,0)
			};
			for (int face = 0; face < 6; ++face)
			{
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
					GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, shadowIt->second.depth_cube, 0);
				glClear(GL_DEPTH_BUFFER_BIT);
				const glm::mat4 view = glm::lookAt(pos, pos + directions[face], ups[face]);
				RenderShadowCasters(scene, projection * view, view, projection, resolution, bck_->shadow_point_shader, true, pos, farPlane, face);
			}
		}
		else
		{
			const glm::vec3 lightDir = GetLightDirection(light);
			glm::vec3 center(0.f);
			if (light->type_ == HRL_SPOT_LIGHT)
				center = light->position_;
			else
				center = glm::vec3(0.f);

			glm::vec3 up = std::abs(glm::dot(lightDir, glm::vec3(0,1,0))) > 0.98f
				? glm::vec3(0,0,1) : glm::vec3(0,1,0);
			glm::mat4 lightView;
			glm::mat4 projection;
			if (light->type_ == HRL_SPOT_LIGHT)
			{
				const float outerAngle = glm::clamp(light->outerCutoff, 1.f, 89.0f);
				projection = glm::perspective(glm::radians(outerAngle * 2.f), 1.f, 0.1f, 100.f);
				lightView = glm::lookAt(light->position_, light->position_ + lightDir, up);
			}
			else
			{
				const glm::vec3 lightPosition = center - lightDir * 80.f;
				projection = glm::ortho(-60.f, 60.f, -60.f, 60.f, 0.1f, 200.f);
				lightView = glm::lookAt(lightPosition, center, up);
			}
			const glm::mat4 shadowClip = projection * lightView;
			// Shadow rasterization needs clip-space coordinates (-1..1). The 0..1
			// bias transform belongs only to the sampling matrix used by the lit shader.
			RenderShadowCasters(scene, shadowClip, lightView, projection, resolution, bck_->shadow_2d_shader, false, light->position_, 0.f, -1);
		}

		glDisable(GL_POLYGON_OFFSET_FILL);
		glCullFace(GL_BACK);
		++slot;
		activeSlots[id] = slot - 1;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDisable(GL_CULL_FACE);
	scene->shadows_dirty = false;
}

static void UploadSceneLights(const hrl_scene_t* scene, HRL_id scene_id)
{
	GL_Light gpu_lights[MAX_LIGHTS]{};
	for (int i = 0; i < MAX_LIGHTS; ++i)
	{
		gpu_lights[i].shadowMatrix = glm::mat4(1.f);
		gpu_lights[i].shadowParams = glm::vec4(0.f, 0.f, -1.f, 0.f);
		gpu_lights[i].shadowStrength = 1.f;
	}
	if (!scene)
		return;

	const auto slotsIt = bck_->active_shadow_slots_by_scene.find(scene_id);
	const auto* activeSlots = (slotsIt != bck_->active_shadow_slots_by_scene.end()) ? &slotsIt->second : nullptr;

	int count = 0;
	for (const auto& [id, light] : scene->lights)
	{
		if (count >= MAX_LIGHTS)
			break;
		GL_Light& dst = gpu_lights[count];
		dst.type = light->type_;
		dst.intensity = light->intensity_;
		dst.attenuation = light->attenuation_;
		dst.innerCutoff = std::cos(glm::radians(light->innerCutoff));
		dst.position = light->position_;
		dst.outerCutoff = std::cos(glm::radians(light->outerCutoff));
		dst.rotation = GetLightDirection(light);
		dst.color = light->color_;
		dst.shadowStrength = light->shadow_strength_;

		if (activeSlots)
		{
			auto slotIt = activeSlots->find(id);
			if (slotIt != activeSlots->end())
		{
			const int slot = slotIt->second;
			if (light->type_ == HRL_POINT_LIGHT)
			{
				constexpr float farPlane = 100.f;
				const glm::vec3 pos = light->position_;
				// The point-light shader only needs the far plane; cube sampling uses
				// the fragment-to-light vector as its lookup direction.
				dst.shadowParams = glm::vec4(light->shadow_bias_, farPlane, (float)slot, 2.f);
			}
			else
			{
				const glm::vec3 lightDir = GetLightDirection(light);
				const glm::vec3 center = (light->type_ == HRL_SPOT_LIGHT) ? light->position_ : glm::vec3(0.f);
				const glm::vec3 up = std::abs(glm::dot(lightDir, glm::vec3(0,1,0))) > 0.98f ? glm::vec3(0,0,1) : glm::vec3(0,1,0);
				glm::mat4 lightView;
				glm::mat4 projection;
				if (light->type_ == HRL_SPOT_LIGHT)
				{
					const float outerAngle = glm::clamp(light->outerCutoff, 1.f, 89.0f);
					projection = glm::perspective(glm::radians(outerAngle * 2.f), 1.f, 0.1f, 100.f);
					lightView = glm::lookAt(light->position_, light->position_ + lightDir, up);
				}
				else
				{
					const glm::vec3 lightPosition = center - lightDir * 80.f;
					projection = glm::ortho(-60.f, 60.f, -60.f, 60.f, 0.1f, 200.f);
					lightView = glm::lookAt(lightPosition, center, up);
				}
				const glm::mat4 clip = projection * lightView;
				const glm::mat4 matrix = glm::translate(glm::mat4(1.f), glm::vec3(0.5f)) *
					glm::scale(glm::mat4(1.f), glm::vec3(0.5f)) * clip;
				dst.shadowMatrix = matrix;
				dst.shadowParams = glm::vec4(light->shadow_bias_, 0.f, (float)slot, 1.f);
			}
		}
		}
		++count;
	}

	glBindBuffer(GL_UNIFORM_BUFFER, bck_->ubo[UBO_LIGHTS]);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)sizeof(gpu_lights), gpu_lights);
}

static void DestroyMSAAResources(GL_Scene* scene)
{
	if (!scene)
		return;
	bool hasMSAATextures = false;
	for (GLuint tex : scene->msaa_textures) hasMSAATextures |= (tex != 0);
	if (hasMSAATextures)
		glDeleteTextures(5, scene->msaa_textures);
	if (scene->msaa_depth_rbo)
		glDeleteRenderbuffers(1, &scene->msaa_depth_rbo);
	if (scene->msaa_fbo)
		glDeleteFramebuffers(1, &scene->msaa_fbo);
	for (GLuint& tex : scene->msaa_textures) tex = 0;
	scene->msaa_depth_rbo = 0;
	scene->msaa_fbo = 0;
	scene->msaa_samples = 1;
}

static bool CreateMSAAResources(GL_Scene* scene, int samples)
{
	if (!scene)
		return false;
	DestroyMSAAResources(scene);
	if (samples <= 1)
		return true;

	GLint maxSamples = 1;
	glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
	if (samples > maxSamples)
		samples = maxSamples;
	if (samples <= 1)
		return true;

	glGenFramebuffers(1, &scene->msaa_fbo);
	glGenTextures(5, scene->msaa_textures);
	glBindFramebuffer(GL_FRAMEBUFFER, scene->msaa_fbo);
	const GLenum internalFormats[5] = {GL_RGBA16F, GL_RGBA16F, GL_RGBA8, GL_RGBA16F, GL_RGBA16F};
	for (int i = 0; i < 5; ++i)
	{
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, scene->msaa_textures[i]);
		glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, internalFormats[i], scene->width, scene->height, GL_TRUE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D_MULTISAMPLE, scene->msaa_textures[i], 0);
	}
	glGenRenderbuffers(1, &scene->msaa_depth_rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, scene->msaa_depth_rbo);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, scene->width, scene->height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, scene->msaa_depth_rbo);
	GLenum attachments[5] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4};
	glDrawBuffers(5, attachments);
	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		DestroyMSAAResources(scene);
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GL33: failed to create MSAA framebuffer");
		return false;
	}
	scene->msaa_samples = samples;
	return true;
}

static void ResolveSceneMSAA(GL_Scene* scene)
{
	if (!scene || scene->msaa_samples <= 1 || !scene->msaa_fbo)
		return;
	for (int i = 0; i < 5; ++i)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->msaa_fbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->fbo);
		glDrawBuffer(GL_COLOR_ATTACHMENT0 + i);
		glBlitFramebuffer(0, 0, scene->width, scene->height,
			0, 0, scene->width, scene->height,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->msaa_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->fbo);
	glBlitFramebuffer(0, 0, scene->width, scene->height, 0, 0, scene->width, scene->height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

	GLenum attachments[5] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4};
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->fbo);
	glDrawBuffers(5, attachments);
	// Keep the resolved scene FBO bound. Subsequent scene passes (including
	// screen-space displacement) must continue rendering into the HRL scene
	// target instead of accidentally falling back to the default window FBO.
	glBindFramebuffer(GL_FRAMEBUFFER, scene->fbo);
}



//SCENES
void GL33_CreateScene(HRL_id _newSceneid, int _renderOnScreen)
{
	auto* scene = new GL_Scene();
	scene->width = (int)GetWindowWidth();
	scene->height = (int)GetWindowHeight();

	printf("scene size : %dx%d\n", scene->width, scene->height);


	// Gen scene textures: color, bloom, picking, GI albedo, GI normal.
	glGenFramebuffers(1, &scene->fbo);
	glGenTextures(5, scene->textures);

	glBindFramebuffer(GL_FRAMEBUFFER, scene->fbo);

	//Color buffer
	glBindTexture(GL_TEXTURE_2D, scene->textures[0]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, scene->width, scene->height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	//Bright color buffer
	glBindTexture(GL_TEXTURE_2D, scene->textures[1]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, scene->width, scene->height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	//Color picking buffer: normalized 8-bit RGBA is enough for object IDs encoded by the shader.
	glBindTexture(GL_TEXTURE_2D, scene->textures[2]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, scene->width, scene->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// GI G-buffer: diffuse albedo (linear) and world-space normal.
	glBindTexture(GL_TEXTURE_2D, scene->textures[3]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, scene->width, scene->height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glBindTexture(GL_TEXTURE_2D, scene->textures[4]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, scene->width, scene->height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene->textures[0], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, scene->textures[1], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, scene->textures[2], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, scene->textures[3], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4, GL_TEXTURE_2D, scene->textures[4], 0);

	// Sampleable depth texture used by localized volumetric fog and god rays.
	glGenTextures(1, &scene->depth_texture);
	glBindTexture(GL_TEXTURE_2D, scene->depth_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, scene->width, scene->height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, scene->depth_texture, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	GLenum attachments[5] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4 };
	glDrawBuffers(5, attachments);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		printf("FBO incomplete: 0x%x\n", status);
		assert(false);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// Create multisample targets when the global anti-aliasing mode is already enabled.
	if (bck_->antialiasing_samples > 1)
		CreateMSAAResources(scene, bck_->antialiasing_samples);

	//les HRL_id sont partagés entre le backend et l'api
	bck_->gpu_scenes.emplace(_newSceneid, scene);
}
void GL33_DeleteScene(HRL_id _sceneid)
{
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_DeleteScene error: invalid scene id");
		return;
	}

	if (g_gl33_gi)
		g_gl33_gi->ReleaseScene(_sceneid);

	//scene is not rendered at screen
	DestroyMSAAResources(it->second);
	glDeleteTextures(5, it->second->textures);
	if (it->second->depth_texture)
		glDeleteTextures(1, &it->second->depth_texture);
	if (it->second->depth_rbo)
		glDeleteRenderbuffers(1, &it->second->depth_rbo);
	glDeleteFramebuffers(1, &it->second->fbo);

	delete it->second;
	bck_->gpu_scenes.erase(it);
	bck_->active_shadow_slots_by_scene.erase(_sceneid);
}
void GL33_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height)
{
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_ResizeSceneTexture error: invalid scene id");
		return;
	}
	if (it->second->fbo == 0)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING, "GL33_ResizeSceneTexture error: scene is render on the screen");
		return;
	}

	glBindTexture(GL_TEXTURE_2D, it->second->textures[0]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, _width, _height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glBindTexture(GL_TEXTURE_2D, it->second->textures[1]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, _width, _height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glBindTexture(GL_TEXTURE_2D, it->second->textures[2]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _width, _height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindTexture(GL_TEXTURE_2D, it->second->textures[3]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, _width, _height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glBindTexture(GL_TEXTURE_2D, it->second->textures[4]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, _width, _height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindTexture(GL_TEXTURE_2D, it->second->depth_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, _width, _height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	glBindTexture(GL_TEXTURE_2D, 0);

	it->second->width = _width;
	it->second->height = _height;
	if (bck_->antialiasing_samples > 1)
		CreateMSAAResources(it->second, bck_->antialiasing_samples);
}






//MESH GPU RESOURCES
int GL33_CreateSpriteMesh(HRL_id id)
{
	if (!bck_ || id == HRL_INVALID_ID || bck_->sprite_vao == 0 || bck_->sprite_instanced_vao == 0)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GL33_CreateSpriteMesh: shared sprite geometry is not initialized");
		return HRL_FALSE;
	}
	// Sprites use the backend-owned shared plane, so no per-sprite GPU buffers are created.
	return HRL_TRUE;
}

static bool GL33_UploadMeshLOD(GL33_Backend::MeshLOD_GPU& gpu,
	const HRL_Vertex3D* vertices, size_t vertexCount,
	const HRL_uint* indices, size_t indexCount)
{
	if (!vertices || vertexCount == 0 ||
		vertexCount > (size_t)std::numeric_limits<GLsizei>::max() ||
		indexCount > (size_t)std::numeric_limits<GLsizei>::max() ||
		(indexCount != 0 && !indices))
		return false;

	glGenVertexArrays(1, &gpu.vao);
	glGenBuffers(1, &gpu.vbo);
	if (!gpu.vao || !gpu.vbo)
	{
		if (gpu.vao) glDeleteVertexArrays(1, &gpu.vao);
		if (gpu.vbo) glDeleteBuffers(1, &gpu.vbo);
		gpu = {};
		return false;
	}

	glBindVertexArray(gpu.vao);
	glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
	glBufferData(GL_ARRAY_BUFFER,
		(GLsizeiptr)(vertexCount * sizeof(HRL_Vertex3D)),
		vertices, GL_STATIC_DRAW);

	const GLsizei stride = (GLsizei)sizeof(HRL_Vertex3D);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
		(const void*)offsetof(HRL_Vertex3D, position));
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
		(const void*)offsetof(HRL_Vertex3D, normal));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
		(const void*)offsetof(HRL_Vertex3D, uv));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride,
		(const void*)offsetof(HRL_Vertex3D, tangent));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride,
		(const void*)offsetof(HRL_Vertex3D, bitangent));
	glEnableVertexAttribArray(4);

	if (indexCount)
	{
		glGenBuffers(1, &gpu.ebo);
		if (!gpu.ebo)
		{
			glBindVertexArray(0);
			glDeleteVertexArrays(1, &gpu.vao);
			glDeleteBuffers(1, &gpu.vbo);
			gpu = {};
			return false;
		}
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER,
			(GLsizeiptr)(indexCount * sizeof(HRL_uint)),
			indices, GL_STATIC_DRAW);
		gpu.indexed = true;
		gpu.index_count = (GLsizei)indexCount;
	}

	gpu.vertex_count = (GLsizei)vertexCount;
	glBindVertexArray(0);
	return true;
}

static glm::mat4 CalculateLandscapeModelMatrix(const HRL_Landscape* landscape)
{
	glm::mat4 model(1.f);
	model = glm::translate(model, landscape->position_);
	model = glm::rotate(model, glm::radians(landscape->rotation_.x), glm::vec3(1.f, 0.f, 0.f));
	model = glm::rotate(model, glm::radians(landscape->rotation_.y), glm::vec3(0.f, 1.f, 0.f));
	model = glm::rotate(model, glm::radians(landscape->rotation_.z), glm::vec3(0.f, 0.f, 1.f));
	model = glm::scale(model, landscape->scale_);
	return model;
}

static float SampleLandscapeHeight(const GL33_Texture* texture, float u, float v)
{
	if (!texture || texture->GetWidth() == 0 || texture->GetHeight() == 0)
		return 0.f;
	const auto& pixels = texture->GetCpuRGBA();
	const int width = static_cast<int>(texture->GetWidth());
	const int height = static_cast<int>(texture->GetHeight());
	if (pixels.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
		return 0.f;

	u = glm::clamp(u, 0.f, 1.f);
	v = glm::clamp(v, 0.f, 1.f);
	const float fx = u * static_cast<float>(std::max(1, width - 1));
	const float fy = v * static_cast<float>(std::max(1, height - 1));
	const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, width - 1);
	const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, height - 1);
	const int x1 = std::min(x0 + 1, width - 1);
	const int y1 = std::min(y0 + 1, height - 1);
	const float tx = fx - static_cast<float>(x0);
	const float ty = fy - static_cast<float>(y0);

	auto read = [&](int x, int y) -> float {
		const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
		return static_cast<float>(pixels[index]) / 255.f;
	};
	const float a = glm::mix(read(x0, y0), read(x1, y0), tx);
	const float b = glm::mix(read(x0, y1), read(x1, y1), tx);
	return glm::mix(a, b, ty);
}

static bool BuildLandscapeGeometry(const HRL_Landscape* landscape, const GL33_Texture* heightmap,
	std::vector<HRL_Vertex3D>& vertices, std::vector<HRL_uint>& indices,
	glm::vec3& boundsCenter, float& boundsRadius)
{
	if (!landscape || !heightmap)
		return false;

	const HRL_uint rx = std::clamp<HRL_uint>(landscape->resolution_x_, 2u, 512u);
	const HRL_uint rz = std::clamp<HRL_uint>(landscape->resolution_z_, 2u, 512u);
	const size_t vertexCount = static_cast<size_t>(rx + 1u) * static_cast<size_t>(rz + 1u);
	const size_t indexCount = static_cast<size_t>(rx) * static_cast<size_t>(rz) * 6u;
	if (vertexCount > static_cast<size_t>(std::numeric_limits<GLsizei>::max()) ||
		indexCount > static_cast<size_t>(std::numeric_limits<GLsizei>::max()))
		return false;

	vertices.resize(vertexCount);
	indices.resize(indexCount);

	const float cellX = landscape->size_.x / static_cast<float>(rx);
	const float cellZ = landscape->size_.y / static_cast<float>(rz);
	const float du = 1.f / static_cast<float>(rx);
	const float dv = 1.f / static_cast<float>(rz);

	float minHeight = std::numeric_limits<float>::max();
	float maxHeight = -std::numeric_limits<float>::max();

	for (HRL_uint z = 0; z <= rz; ++z)
	{
		const float v = static_cast<float>(z) / static_cast<float>(rz);
		for (HRL_uint x = 0; x <= rx; ++x)
		{
			const float u = static_cast<float>(x) / static_cast<float>(rx);
			const float h = SampleLandscapeHeight(heightmap, u, v) * landscape->height_scale_;
			minHeight = std::min(minHeight, h);
			maxHeight = std::max(maxHeight, h);

			HRL_Vertex3D& vertex = vertices[static_cast<size_t>(z) * static_cast<size_t>(rx + 1u) + x];
			const float hL = SampleLandscapeHeight(heightmap, std::max(0.f, u - du), v) * landscape->height_scale_;
			const float hR = SampleLandscapeHeight(heightmap, std::min(1.f, u + du), v) * landscape->height_scale_;
			const float hD = SampleLandscapeHeight(heightmap, u, std::max(0.f, v - dv)) * landscape->height_scale_;
			const float hU = SampleLandscapeHeight(heightmap, u, std::min(1.f, v + dv)) * landscape->height_scale_;

			const float dhx = (hR - hL) / std::max(2.f * cellX, 1e-6f);
			const float dhz = (hU - hD) / std::max(2.f * cellZ, 1e-6f);
			glm::vec3 normal = glm::normalize(glm::vec3(-dhx, 1.f, -dhz));
			glm::vec3 tangent = glm::normalize(glm::vec3(1.f, dhx, 0.f));
			tangent = glm::normalize(tangent - normal * glm::dot(normal, tangent));
			if (glm::dot(tangent, tangent) < 1e-8f)
				tangent = glm::vec3(1.f, 0.f, 0.f);
			glm::vec3 bitangent = glm::normalize(glm::cross(normal, tangent));
			if (glm::dot(bitangent, bitangent) < 1e-8f)
				bitangent = glm::vec3(0.f, 0.f, 1.f);

			const float localX = (u - 0.5f) * landscape->size_.x;
			const float localZ = (v - 0.5f) * landscape->size_.y;
			vertex.position[0] = localX;
			vertex.position[1] = h;
			vertex.position[2] = localZ;
			vertex.normal[0] = normal.x;
			vertex.normal[1] = normal.y;
			vertex.normal[2] = normal.z;
			vertex.uv[0] = u * landscape->uv_scale_.x;
			vertex.uv[1] = v * landscape->uv_scale_.y;
			vertex.tangent[0] = tangent.x;
			vertex.tangent[1] = tangent.y;
			vertex.tangent[2] = tangent.z;
			vertex.bitangent[0] = bitangent.x;
			vertex.bitangent[1] = bitangent.y;
			vertex.bitangent[2] = bitangent.z;
		}
	}

	size_t writeIndex = 0;
	for (HRL_uint z = 0; z < rz; ++z)
	{
		for (HRL_uint x = 0; x < rx; ++x)
		{
			const HRL_uint a = z * (rx + 1u) + x;
			const HRL_uint b = a + 1u;
			const HRL_uint c = a + (rx + 1u);
			const HRL_uint d = c + 1u;
			// a-c-b gives an upward-facing (+Y) normal in the HRL coordinate system.
			indices[writeIndex++] = a;
			indices[writeIndex++] = c;
			indices[writeIndex++] = b;
			indices[writeIndex++] = b;
			indices[writeIndex++] = c;
			indices[writeIndex++] = d;
		}
	}

	boundsCenter = glm::vec3(0.f, 0.5f * (minHeight + maxHeight), 0.f);
	const glm::vec3 halfExtents(
		0.5f * std::abs(landscape->size_.x),
		0.5f * std::abs(maxHeight - minHeight),
		0.5f * std::abs(landscape->size_.y));
	boundsRadius = glm::length(halfExtents);
	return true;
}

static void DestroyLandscapeGPU(GL33_Backend::LandscapeGPU& gpu)
{
	if (gpu.geometry.vao) glDeleteVertexArrays(1, &gpu.geometry.vao);
	if (gpu.geometry.vbo) glDeleteBuffers(1, &gpu.geometry.vbo);
	if (gpu.geometry.ebo) glDeleteBuffers(1, &gpu.geometry.ebo);
	gpu = {};
}

static bool EnsureLandscapeGPU(const HRL_Landscape* landscape, GL33_Backend::LandscapeGPU& gpu)
{
	if (!landscape)
		return false;
	if (gpu.geometry.vao != 0 && gpu.revision == landscape->revision_ && gpu.heightmap == landscape->heightmap_)
		return true;

	if (landscape->heightmap_ == HRL_INVALID_ID)
		return false;
	const auto texIt = bck_->textures.find(landscape->heightmap_);
	if (texIt == bck_->textures.end() || !texIt->second || texIt->second->GetGL_ID() == 0)
		return false;

	std::vector<HRL_Vertex3D> vertices;
	std::vector<HRL_uint> indices;
	glm::vec3 boundsCenter(0.f);
	float boundsRadius = 0.f;
	if (!BuildLandscapeGeometry(landscape, texIt->second, vertices, indices, boundsCenter, boundsRadius))
		return false;

	GL33_Backend::LandscapeGPU rebuilt;
	if (!GL33_UploadMeshLOD(rebuilt.geometry, vertices.data(), vertices.size(), indices.data(), indices.size()))
		return false;
	rebuilt.revision = landscape->revision_;
	rebuilt.heightmap = landscape->heightmap_;
	rebuilt.bounds_center = boundsCenter;
	rebuilt.bounds_radius = boundsRadius;

	DestroyLandscapeGPU(gpu);
	gpu = std::move(rebuilt);
	return true;
}

static bool BindLandscapeMaterial(HRL_id landscapeId, HRL_Material* material, const HRL_Landscape* landscape,
	const glm::mat4& model)
{
	if (!bck_ || !ctx_ || !landscape)
		return false;
	const auto shaderIt = bck_->shaders.find(HRL_MESH_3D_SHADER);
	if (shaderIt == bck_->shaders.end() || !shaderIt->second)
		return false;
	GL33_Shader* shader = shaderIt->second;

	shader->Use();
	ctx_->shader = shader;
	ctx_->bound_shader = shader;
	ctx_->bound_material = material;

	shader->SetMat4("projection", ctx_->proj_mat);
	shader->SetMat4("view", ctx_->view_mat);
	shader->SetMat4("model", model);
	glm::mat3 normalMatrix(1.f);
	const glm::mat3 model3(model);
	const float det = glm::determinant(model3);
	if (std::isfinite(det) && std::abs(det) > 1e-8f)
		normalMatrix = glm::transpose(glm::inverse(model3));
	shader->SetMat3("normalMatrix", normalMatrix);
	shader->SetUint("uSpriteID", 0u);
	shader->SetVec4("UVRegion", glm::vec4(0.f, 0.f, 1.f, 1.f));
	shader->SetVec3("CamPos", ctx_->viewport->camera_->position_);
	shader->SetVec3("TintColor", glm::vec3(1.f));
	shader->SetInt("TwoSided", 0);
	shader->SetInt("DebugView", static_cast<int>(ctx_->current_scene ? ctx_->current_scene->debug_view : HRL_DEBUG_VIEW_NONE));
	shader->SetInt("ss_displacement_enabled", 0);

	shader->SetInt("FogEnabled", ctx_->current_fog ? (ctx_->current_fog->enabled ? 1 : 0) : 0);
	shader->SetInt("FogMode", ctx_->current_fog ? static_cast<int>(ctx_->current_fog->mode) : 0);
	shader->SetVec4("FogColor", ctx_->current_fog ? glm::vec4(ctx_->current_fog->r, ctx_->current_fog->g, ctx_->current_fog->b, 1.f) : glm::vec4(0.f, 0.f, 0.f, 1.f));
	shader->SetFloat("FogStart", ctx_->current_fog ? ctx_->current_fog->range_start : 20.f);
	shader->SetFloat("FogEnd", ctx_->current_fog ? ctx_->current_fog->range_end : 100.f);
	shader->SetFloat("FogDensity", ctx_->current_fog ? ctx_->current_fog->density : 0.1f);

	shader->SetFloat("BaseColorAlpha", 1.f);
	shader->SetFloat("RoughnessValue", 1.f);
	shader->SetFloat("MetallicValue", 0.f);
	shader->SetFloat("SpecularValue", 1.f);
	shader->SetFloat("OpacityValue", 1.f);
	shader->SetInt("RoughnessUseValue", 0);
	shader->SetInt("MetallicUseValue", 0);
	shader->SetInt("SpecularUseValue", 0);
	shader->SetInt("OpacityUseValue", 0);
	shader->SetInt("RoughnessInvert", 0);
	shader->SetInt("AlphaInvert", 0);
	shader->SetInt("ShadowMap2D_0", SHADOW_2D_TEXTURE_UNIT_BASE + 0);
	shader->SetInt("ShadowMap2D_1", SHADOW_2D_TEXTURE_UNIT_BASE + 1);
	shader->SetInt("ShadowMap2D_2", SHADOW_2D_TEXTURE_UNIT_BASE + 2);
	shader->SetInt("ShadowMap2D_3", SHADOW_2D_TEXTURE_UNIT_BASE + 3);
	shader->SetInt("ShadowMapCube_0", SHADOW_CUBE_TEXTURE_UNIT_BASE + 0);
	shader->SetInt("ShadowMapCube_1", SHADOW_CUBE_TEXTURE_UNIT_BASE + 1);
	shader->SetInt("ShadowMapCube_2", SHADOW_CUBE_TEXTURE_UNIT_BASE + 2);
	shader->SetInt("ShadowMapCube_3", SHADOW_CUBE_TEXTURE_UNIT_BASE + 3);
	shader->SetInt("EnvironmentMap", ENVIRONMENT_TEXTURE_UNIT);

	bool environmentEnabled = false;
	if (ctx_->current_scene && ctx_->current_scene->environment_mapping_enabled)
	{
		HRL_id envId = ctx_->current_scene->environment_texture;
		if (envId == HRL_INVALID_ID) envId = ctx_->current_scene->sky_texture;
		auto envIt = bck_->textures.find(envId);
		if (envIt != bck_->textures.end() && envIt->second)
		{
			environmentEnabled = true;
			glActiveTexture(GL_TEXTURE0 + ENVIRONMENT_TEXTURE_UNIT);
			glBindTexture(GL_TEXTURE_2D, envIt->second->GetGL_ID());
		}
	}
	shader->SetInt("EnvironmentEnabled", environmentEnabled ? 1 : 0);
	shader->SetFloat("EnvironmentStrength", environmentEnabled ? 1.0f : 0.0f);

	for (int i = 0; i < 6; ++i)
	{
		shader->SetInt(tex_uniform_name[i], i);
		if (!material)
		{
			ApplyFallback(i);
			continue;
		}
		auto itParam = material->textureParams_.find(std::string(tex_uniform_name[i]));
		if (itParam == material->textureParams_.end())
		{
			ApplyFallback(i);
			continue;
		}
		auto itTexture = bck_->textures.find(itParam->second);
		if (itTexture == bck_->textures.end() || !itTexture->second)
		{
			ApplyFallback(i);
			continue;
		}
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, itTexture->second->GetGL_ID());
	}

	shader->SetInt(HRL_T_AMBIENT_OCCLUSION, AO_TEXTURE_UNIT);
	ApplyFallbackToUnit(AO_TEXTURE_UNIT, ROUGHNESS_INT);
	if (material)
	{
		if (auto aoParam = material->textureParams_.find(std::string(HRL_T_AMBIENT_OCCLUSION)); aoParam != material->textureParams_.end())
		{
			auto aoTexture = bck_->textures.find(aoParam->second);
			if (aoTexture != bck_->textures.end() && aoTexture->second)
			{
				glActiveTexture(GL_TEXTURE0 + AO_TEXTURE_UNIT);
				glBindTexture(GL_TEXTURE_2D, aoTexture->second->GetGL_ID());
			}
		}
		for (const auto& [name, value] : material->intParams_) shader->SetInt(name, value);
		for (const auto& [name, value] : material->floatParams_) shader->SetFloat(name, value);
		for (const auto& [name, value] : material->vec2Params_) shader->SetVec2(name, value);
		for (const auto& [name, value] : material->vec3Params_) shader->SetVec3(name, value);
		for (const auto& [name, value] : material->vec4Params_) shader->SetVec4(name, value);
	}
	return true;
}

static void DrawLandscapes(HRL_id scene_id, const hrl_scene_t* scene, const FrustumPlaneSet& frustum)
{
	if (!bck_ || !ctx_ || !ctx_->viewport || !ctx_->viewport->camera_ || !scene || scene->landscapes.empty())
		return;

	// Remove backend allocations for landscapes deleted from the public context.
	for (auto it = bck_->landscapes.begin(); it != bck_->landscapes.end(); )
	{
		if (GetPrivateContext()->landscapes.find(it->first) == GetPrivateContext()->landscapes.end())
		{
			DestroyLandscapeGPU(it->second);
			it = bck_->landscapes.erase(it);
		}
		else
			++it;
	}

	const bool wireframe = scene->debug_view == HRL_DEBUG_VIEW_WIREFRAME;
	GLint previousPolygonMode[2] = {GL_FILL, GL_FILL};
	glGetIntegerv(GL_POLYGON_MODE, previousPolygonMode);
	GLboolean previousCull = glIsEnabled(GL_CULL_FACE);
	GLint previousCullFace = GL_BACK;
	glGetIntegerv(GL_CULL_FACE_MODE, &previousCullFace);

	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glDisable(GL_BLEND);
	if (wireframe)
	{
		glDisable(GL_CULL_FACE);
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	}
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}

	for (const auto& [id, landscape] : scene->landscapes)
	{
		if (!landscape)
			continue;
		auto& gpu = bck_->landscapes[id];
		if (!EnsureLandscapeGPU(landscape, gpu) || gpu.geometry.vao == 0 || gpu.geometry.index_count <= 0)
			continue;

		const glm::mat4 model = CalculateLandscapeModelMatrix(landscape);
		const glm::vec3 center = glm::vec3(model * glm::vec4(gpu.bounds_center, 1.f));
		const float maxScale = std::max({std::abs(landscape->scale_.x), std::abs(landscape->scale_.y), std::abs(landscape->scale_.z), 1e-6f});
		if (!SphereInsideFrustum(frustum, center, gpu.bounds_radius * maxScale))
			continue;

		HRL_Material* material = nullptr;
		if (landscape->material_ != HRL_INVALID_ID)
		{
			auto matIt = GetPrivateContext()->materials.find(landscape->material_);
			if (matIt != GetPrivateContext()->materials.end()) material = matIt->second;
		}
		if (!BindLandscapeMaterial(id, material, landscape, model))
			continue;

		const bool twoSided = MaterialIsTwoSided(material);
		if (wireframe || twoSided) glDisable(GL_CULL_FACE);
		else { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
		glBindVertexArray(gpu.geometry.vao);
		glDrawElements(GL_TRIANGLES, gpu.geometry.index_count, GL_UNSIGNED_INT, nullptr);
	}

	glBindVertexArray(0);
	glPolygonMode(GL_FRONT, previousPolygonMode[0]);
	glPolygonMode(GL_BACK, previousPolygonMode[1]);
	glCullFace(previousCullFace);
	if (previousCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}

static void DrawLandscapeShadowCasters(hrl_scene_t* scene, const glm::mat4& lightViewProjection,
	const glm::mat4& lightView, const glm::mat4& lightProjection, int shadowResolution,
	bool pointLight, const glm::vec3& lightPosition, float farPlane, int face)
{
	(void)lightView;
	(void)lightProjection;
	(void)shadowResolution;
	(void)face;
	if (!scene || !bck_ || scene->landscapes.empty())
		return;

	const FrustumPlaneSet frustum = BuildFrustum(lightViewProjection);
	for (const auto& [id, landscape] : scene->landscapes)
	{
		if (!landscape)
			continue;
		auto& gpu = bck_->landscapes[id];
		if (!EnsureLandscapeGPU(landscape, gpu) || gpu.geometry.vao == 0 || gpu.geometry.index_count <= 0)
			continue;

		const glm::mat4 model = CalculateLandscapeModelMatrix(landscape);
		const glm::vec3 center = glm::vec3(model * glm::vec4(gpu.bounds_center, 1.f));
		const float maxScale = std::max({std::abs(landscape->scale_.x), std::abs(landscape->scale_.y), std::abs(landscape->scale_.z), 1e-6f});
		const float radius = gpu.bounds_radius * maxScale;
		if (pointLight)
		{
			const float limit = farPlane + radius;
			if (glm::dot(center - lightPosition, center - lightPosition) > limit * limit)
				continue;
		}
		else if (!SphereInsideFrustum(frustum, center, radius))
			continue;

		GL33_Shader* shader = pointLight ? bck_->shadow_point_shader : bck_->shadow_2d_shader;
		if (!shader)
			continue;
		shader->Use();
		shader->SetMat4("lightSpaceMatrix", lightViewProjection);
		shader->SetMat4("lightViewProjection", lightViewProjection);
		shader->SetVec3("lightPosition", lightPosition);
		shader->SetFloat("farPlane", farPlane);
		shader->SetMat4("model", model);

		HRL_Material* material = nullptr;
		if (landscape->material_ != HRL_INVALID_ID)
		{
			auto matIt = GetPrivateContext()->materials.find(landscape->material_);
			if (matIt != GetPrivateContext()->materials.end()) material = matIt->second;
		}
		if (MaterialIsTwoSided(material)) glDisable(GL_CULL_FACE);
		else { glEnable(GL_CULL_FACE); glCullFace(GL_FRONT); }

		glBindVertexArray(gpu.geometry.vao);
		glDrawElements(GL_TRIANGLES, gpu.geometry.index_count, GL_UNSIGNED_INT, nullptr);
	}
}

static bool GL33_UploadSkeletalMesh(GL33_Backend::SkeletalMeshGPU& gpu,
	const HRL_SkeletalVertex* vertices, size_t vertexCount,
	const HRL_uint* indices, size_t indexCount, HRL_uint boneCount)
{
	if (!vertices || vertexCount < 3 || boneCount == 0 || boneCount > HRL_MAX_SKELETAL_BONES)
		return false;
	if (indexCount > 0 && !indices)
		return false;
	if (vertexCount > (size_t)std::numeric_limits<GLsizei>::max() || indexCount > (size_t)std::numeric_limits<GLsizei>::max())
		return false;

	glGenVertexArrays(1, &gpu.vao);
	glGenBuffers(1, &gpu.vbo);
	glGenBuffers(1, &gpu.bone_ubo);
	if (!gpu.vao || !gpu.vbo || !gpu.bone_ubo)
	{
		if (gpu.vao) glDeleteVertexArrays(1, &gpu.vao);
		if (gpu.vbo) glDeleteBuffers(1, &gpu.vbo);
		if (gpu.bone_ubo) glDeleteBuffers(1, &gpu.bone_ubo);
		gpu = {};
		return false;
	}

	glBindVertexArray(gpu.vao);
	glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertexCount * sizeof(HRL_SkeletalVertex)), vertices, GL_STATIC_DRAW);
	const GLsizei stride = (GLsizei)sizeof(HRL_SkeletalVertex);
	const size_t vertexBaseOffset = offsetof(HRL_SkeletalVertex, vertex);
	const size_t positionOffset = vertexBaseOffset + offsetof(HRL_Vertex3D, position);
	const size_t normalOffset = vertexBaseOffset + offsetof(HRL_Vertex3D, normal);
	const size_t uvOffset = vertexBaseOffset + offsetof(HRL_Vertex3D, uv);
	const size_t tangentOffset = vertexBaseOffset + offsetof(HRL_Vertex3D, tangent);
	const size_t bitangentOffset = vertexBaseOffset + offsetof(HRL_Vertex3D, bitangent);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(positionOffset));
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(normalOffset));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(uvOffset));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(tangentOffset));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(bitangentOffset));
	glEnableVertexAttribArray(4);
	glVertexAttribIPointer(5, 4, GL_UNSIGNED_INT, stride, reinterpret_cast<const void*>(offsetof(HRL_SkeletalVertex, boneIndices)));
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(HRL_SkeletalVertex, boneWeights)));
	glEnableVertexAttribArray(6);

	if (indexCount > 0)
	{
		glGenBuffers(1, &gpu.ebo);
		if (!gpu.ebo)
		{
			glBindVertexArray(0);
			glDeleteVertexArrays(1, &gpu.vao);
			glDeleteBuffers(1, &gpu.vbo);
			glDeleteBuffers(1, &gpu.bone_ubo);
			gpu = {};
			return false;
		}
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indexCount * sizeof(HRL_uint)), indices, GL_STATIC_DRAW);
		gpu.indexed = true;
		gpu.index_count = (GLsizei)indexCount;
	}

	glBindBuffer(GL_UNIFORM_BUFFER, gpu.bone_ubo);
	glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)(HRL_MAX_SKELETAL_BONES * sizeof(glm::mat4)), nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, 1, gpu.bone_ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);

	gpu.vertex_count = (GLsizei)vertexCount;
	gpu.bone_count = boneCount;
	gpu.uploaded_pose_serial = 0;
	glBindVertexArray(0);
	return true;
}

int GL33_CreateSkeletalMesh(HRL_id id, const HRL_SkeletalVertex* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count, HRL_uint bone_count)
{
	if (bck_->skeletal_meshes.find(id) != bck_->skeletal_meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_CreateSkeletalMesh: skeletal mesh ID already exists");
		return HRL_FALSE;
	}
	GL33_Backend::SkeletalMeshGPU gpu;
	if (!GL33_UploadSkeletalMesh(gpu, vertices, vertex_count, indices, index_count, bone_count))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "GL33_CreateSkeletalMesh: invalid skeletal vertex/index data");
		return HRL_FALSE;
	}
	bck_->skeletal_meshes.emplace(id, std::move(gpu));
	return HRL_TRUE;
}

int GL33_CreateMesh(HRL_id id,const HRL_Vertex3D*v,size_t vc,const HRL_uint*i,size_t ic){if(bck_->meshes.find(id)!=bck_->meshes.end()){SetErrorCode(HRL_ERROR_INVALID_ID,HRL_SEVERITY_ERROR,"GL33_CreateMesh: mesh ID already exists");return HRL_FALSE;}GL33_Backend::MeshGPU g;g.levels.resize(1);if(!GL33_UploadMeshLOD(g.levels[0],v,vc,i,ic)){SetErrorCode(HRL_INVALID_VALUE,HRL_SEVERITY_ERROR,"GL33_CreateMesh: invalid vertex/index data");return HRL_FALSE;}bck_->meshes.emplace(id,std::move(g));return HRL_TRUE;}
int GL33_CreateMeshLOD(HRL_id id,HRL_uint level,const HRL_Vertex3D*v,size_t vc,const HRL_uint*i,size_t ic){auto it=bck_->meshes.find(id);if(it==bck_->meshes.end()||level==0)return HRL_FALSE;if(it->second.levels.size()<=level)it->second.levels.resize((size_t)level+1);auto&g=it->second.levels[level];if(g.vao)glDeleteVertexArrays(1,&g.vao);if(g.vbo)glDeleteBuffers(1,&g.vbo);if(g.ebo)glDeleteBuffers(1,&g.ebo);g={};return GL33_UploadMeshLOD(g,v,vc,i,ic)?HRL_TRUE:HRL_FALSE;}
void GL33_DeleteMeshLODs(HRL_id id){auto it=bck_->meshes.find(id);if(it==bck_->meshes.end())return;for(size_t k=1;k<it->second.levels.size();++k){auto&g=it->second.levels[k];if(g.vao)glDeleteVertexArrays(1,&g.vao);if(g.vbo)glDeleteBuffers(1,&g.vbo);if(g.ebo)glDeleteBuffers(1,&g.ebo);}it->second.levels.resize(1);}
void GL33_DeleteMesh(HRL_id id){auto it=bck_->meshes.find(id);if(it!=bck_->meshes.end()){for(auto&g:it->second.levels){if(g.vao)glDeleteVertexArrays(1,&g.vao);if(g.vbo)glDeleteBuffers(1,&g.vbo);if(g.ebo)glDeleteBuffers(1,&g.ebo);}bck_->meshes.erase(it);return;}auto sit=bck_->skeletal_meshes.find(id);if(sit!=bck_->skeletal_meshes.end()){auto&g=sit->second;if(g.vao)glDeleteVertexArrays(1,&g.vao);if(g.vbo)glDeleteBuffers(1,&g.vbo);if(g.ebo)glDeleteBuffers(1,&g.ebo);if(g.bone_ubo)glDeleteBuffers(1,&g.bone_ubo);bck_->skeletal_meshes.erase(sit);}}

//LIGHT RESOURCE LIFETIME
void GL33_DeleteLight(HRL_id id)
{
	DestroyLightShadowResources(id);
	for (auto& [sceneId, slots] : bck_->active_shadow_slots_by_scene)
		slots.erase(id);
}

void GL33_SetAntialiasingMode(int samples)
{
	if (!bck_)
		return;
	if (samples != 1 && samples != 2 && samples != 4 && samples != 8)
		return;
	if (bck_->antialiasing_samples == samples)
		return;
	bck_->antialiasing_samples = samples;
	glEnable(GL_MULTISAMPLE);
	if (samples == 1)
		glDisable(GL_MULTISAMPLE);
	for (auto& [id, scene] : bck_->gpu_scenes)
		CreateMSAAResources(scene, samples);
}


//CREATE & DELETE CUSTOM SHADERS
HRL_id GL33_CreateShader(const char *_vertContent, size_t _vertSize, const char *_fragContent, size_t _fragSize)
{
	//on crée le shader et on recupere le code d'erreur
	auto* s = new GL33_Shader();
	int error = s->GL33_Create(_vertContent, _vertSize, _fragContent, _fragSize);

	//si il n'y a pas d'erreur, on génere un ID et on push le shader dans la liste des shaders, sinon on retourne invalid
	//la classe shader s'occupe des codes d'erreurs HRL, pas besoin de le faire ici.
	if (error == 0)
	{
		HRL_id id = GenerateHRL_ID();
		bck_->shaders.emplace(id, s);
		return id;
	}
	return HRL_INVALID_ID;
}
HRL_id GL33_CreateShaderWithId(HRL_id id, const char *_vertContent, size_t _vertSize, const char *_fragContent, size_t _fragSize)
{
    if (id == HRL_INVALID_ID || bck_->shaders.find(id) != bck_->shaders.end())
    {
        SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_CreateShaderWithId: ID already exists or is invalid");
        return HRL_INVALID_ID;
    }
    auto* s = new GL33_Shader();
    if (s->GL33_Create(_vertContent, _vertSize, _fragContent, _fragSize) != 0)
    {
        delete s;
        return HRL_INVALID_ID;
    }
    bck_->shaders.emplace(id, s);
    return id;
}
void GL33_DeleteShader(HRL_id _id)
{
	auto it = bck_->shaders.find(_id);
	if (it == bck_->shaders.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "DeleteShader error: Shader ID doesn't exists");
		return;
	}
	delete it->second;
	bck_->shaders.erase(it);
}



//TEXTURES
HRL_id GL33_CreateTexture(const char* _imageContent, size_t _imageSize)
{
  //on crée la texture et on récupere le code d'erreur
  auto* t = new GL33_Texture();
  int error = t->GL33_Create(_imageContent, _imageSize);

  //si il n'y a pas d'erreur, on génere un ID et on push la texture dans la liste des textures, sinon on retourne invalid
  //la classe texture s'occupe des codes d'erreurs HRL, pas besoin de le faire ici.
  if (error == 0)
  {
    HRL_id id = GenerateHRL_ID();
    bck_->textures.emplace(id, t);
    return id;
  }
  return HRL_INVALID_ID;
}
HRL_id GL33_CreateTextureFromBitmap(BitmapResult bmp)
{
  //on crée la texture et on récupere le code d'erreur
  auto* t = new GL33_Texture();
  int error = t->GL33_CreateFromBitmap(&bmp);

  //si il n'y a pas d'erreur, on génere un ID et on push la texture dans la liste des textures, sinon on retourne invalid
  //la classe texture s'occupe des codes d'erreurs HRL, pas besoin de le faire ici.
  if (error == 0)
  {
    HRL_id id = GenerateHRL_ID();
    bck_->textures.emplace(id, t);
    return id;
  }
  return HRL_INVALID_ID;
}
HRL_id GL33_CreateTextureFromBitmapWithId(HRL_id id, BitmapResult bmp)
{
    if (id == HRL_INVALID_ID || bck_->textures.find(id) != bck_->textures.end())
    {
        SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_CreateTextureFromBitmapWithId: ID already exists or is invalid");
        return HRL_INVALID_ID;
    }
    auto* t = new GL33_Texture();
    if (t->GL33_CreateFromBitmap(&bmp) != 0)
    {
        delete t;
        return HRL_INVALID_ID;
    }
    bck_->textures.emplace(id, t);
    return id;
}
void GL33_DeleteTexture(HRL_id _id)
{
  auto it = bck_->textures.find(_id);
  if (it == bck_->textures.end())
  {
    SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "DeleteTexture error: Texture ID doesn't exists");
    return;
  }
  delete it->second;
  bck_->textures.erase(it);
}
void GL33_GetTextureSize(HRL_id id, int *width, int *height)
{
  auto it = bck_->textures.find(id);
  if (it == bck_->textures.end())
  {
    SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_GetTextureSize error: Texture ID doesn't exists");
    return;
  }
  *width = (int)it->second->GetWidth();
  *height = (int)it->second->GetHeight();
}
void GL33_SetTextureMinFilter(HRL_id id, HRL_EFilterType _filter)
{
  auto it = bck_->textures.find(id);
  if (it == bck_->textures.end())
  {
    SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_SetTextureMinFilter error: Texture ID doesn't exists");
    return;
  }
  it->second->SetMinFilter(_filter);
}
void GL33_SetTextureMaxFilter(HRL_id id, HRL_EFilterType _filter)
{
  auto it = bck_->textures.find(id);
  if (it == bck_->textures.end())
  {
    SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_SetTextureMaxFilter error: Texture ID doesn't exists");
    return;
  }
  it->second->SetMaxFilter(_filter);
}



//POST PROCESSING
void GL33_CreatePostProcess(HRL_id post, int priority)
{

}
void GL33_DeletePostProcess(HRL_id post)
{

}
void GL33_ResetFramebuffer()
{

}





//HRL UTILS
void GL33_GetProjectionMatrix(float *aa)
{
	glm::mat4 proj = ctx_->proj_mat;
	memcpy(aa, glm::value_ptr(proj), sizeof(float) * 16);
}
void GL33_GetViewMatrix(float *aa)
{
	glm::mat4 proj = ctx_->view_mat;
	memcpy(aa, glm::value_ptr(proj), sizeof(float) * 16);
}
void GL33_GetModelMatrix(HRL_Mesh *mesh, float *aa)
{
	if (!mesh || !aa)
		return;
	glm::mat4 model = CalculateModelMatrix(mesh);
	memcpy(aa, glm::value_ptr(model), sizeof(float) * 16);
}




//DEBUG
void GL33_DrawDebug(const DebugRenderer &_renderer, float line_thickness)
{
	glDisable(GL_DEPTH_TEST);

	auto it = bck_->shaders.find(HRL_DEBUG_SHADER);
	if (it == bck_->shaders.end())
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL, "GL33_DrawDebug error: debug shader doesn't exists");
		return;
	}
	auto* s = it->second;
	//on set CurrentShader pour spécifier que les prochains calls utiliseront ce shader
	ctx_->shader = s;
	s->Use();

	s->SetMat4("projection", ctx_->proj_mat);
	s->SetMat4("view", ctx_->view_mat);

	//bind vao and initialize opengl evironement
	glBindVertexArray(bck_->vao[BUFFER_DEBUG]);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_DEBUG]);

	glEnable(GL_DEPTH_TEST);

	//remplacer par une seule fonction dans la vtable pour eviter de le faire a chaque frames
	glLineWidth(line_thickness);

	// lignes
	if (!_renderer.lines.empty())
	{
		size_t size = _renderer.lines.size() * sizeof(DebugVertex);

		//réalloue si le buffer est trop petit
		if (size > ctx_->current_debug_buffer_size)
		{
			glBufferData(GL_ARRAY_BUFFER, (GLsizei)size, _renderer.lines.data(), GL_STREAM_DRAW);
			ctx_->current_debug_buffer_size = size;
		}
		else
		{
			glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizei)size, _renderer.lines.data());
		}

		glDrawArrays(GL_LINES, 0, (GLsizei)_renderer.lines.size());
	}

	// triangles
	if (!_renderer.triangles.empty())
	{
		size_t size = _renderer.triangles.size() * sizeof(DebugVertex);

		if (size > ctx_->current_debug_buffer_size)
		{
			glBufferData(GL_ARRAY_BUFFER, (GLsizei)size, _renderer.triangles.data(), GL_STREAM_DRAW);
			ctx_->current_debug_buffer_size = size;
		}
		else
		{
			glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizei)size, _renderer.triangles.data());
		}

		glDrawArrays(GL_TRIANGLES, 0, (GLsizei)_renderer.triangles.size());
	}
}




//HRL REQUESTS
int GL33_IsValidTexture(HRL_id tex)
{
	auto it = bck_->textures.find(tex);
	if (it == bck_->textures.end())
	{
		return 0;
	}
	return 1;
}
int GL33_IsValidShader(HRL_id shader)
{
	auto it = bck_->shaders.find(shader);
	if (it == bck_->shaders.end())
	{
		return 0;
	}
	return 1;
}



///////// HRL_GL (hrl_gl.h) /////////
unsigned int HRL_GL_GetTextureGL_ID(HRL_id _textureid)
{
	auto it = bck_->textures.find(_textureid);
	if (it == bck_->textures.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetTextureGL_ID error: Texture ID doesn't exists");
		return GL_INVALID_VALUE;
	}

	return it->second->GetGL_ID();
}
unsigned int HRL_GL_GetShaderGL_ID(HRL_id _shaderid)
{
	auto it = bck_->shaders.find(_shaderid);
	if (it == bck_->shaders.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetShaderGL_ID error: Shader ID doesn't exists");
		return GL_INVALID_VALUE;
	}

	return it->second->GetId();
}
unsigned int HRL_GL_GetSceneTextureGL_ID(HRL_id _sceneid)
{
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetSceneTextureGL_ID: scene ID is not valid");
		return GL_INVALID_VALUE;
	}

	return it->second->textures[0];
}

unsigned int HRL_GL_GetSceneColorBufferGL_ID(HRL_id _sceneid)
{
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetSceneColorBufferGL_ID: scene ID is not valid");
		return GL_INVALID_VALUE;
	}
	return it->second->textures[2];
}

unsigned int HRL_GL_GetSceneAlbedoBufferGL_ID(HRL_id _sceneid)
{
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetSceneAlbedoBufferGL_ID: scene ID is not valid");
		return GL_INVALID_VALUE;
	}
	return it->second->textures[3];
}

unsigned int HRL_GL_GetSceneNormalBufferGL_ID(HRL_id _sceneid)
{
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetSceneNormalBufferGL_ID: scene ID is not valid");
		return GL_INVALID_VALUE;
	}
	return it->second->textures[4];
}

HRL_id HRL_GL_GetHoveredObject(HRL_id _scene, int mouseX, int mouseY, HRL_EMeshType* mesh_type)
{
	auto it = bck_->gpu_scenes.find(_scene);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetHoveredObject: scene ID is not valid");
		return GL_INVALID_VALUE;
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, it->second->fbo);
	glReadBuffer(GL_COLOR_ATTACHMENT2);
	const GLboolean multisampleEnabled = glIsEnabled(GL_MULTISAMPLE);
	const GLboolean ditherEnabled = glIsEnabled(GL_DITHER);
	glDisable(GL_MULTISAMPLE);
	glDisable(GL_DITHER);

	unsigned char pixel[4] = {0, 0, 0, 0};
	glReadPixels(mouseX, mouseY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

	const uint32_t id =
		((uint32_t)pixel[0] << 16) |
		((uint32_t)pixel[1] << 8) |
		(uint32_t)pixel[2];

	HRL_id result = HRL_INVALID_ID;
	auto it_mesh = GetPrivateContext()->meshes.find(id);
	if (it_mesh != GetPrivateContext()->meshes.end())
	{
		result = id;
		if (mesh_type)
			*mesh_type = it_mesh->second->type_;
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	if (multisampleEnabled) glEnable(GL_MULTISAMPLE); else glDisable(GL_MULTISAMPLE);
	if (ditherEnabled) glEnable(GL_DITHER); else glDisable(GL_DITHER);
	return result;
}



//Global illumination support
int GL33_IsGlobalIlluminationMethodSupported(int method)
{
	if (!g_gl33_gi)
		return HRL_FALSE;
	if (method < HRL_GI_NONE || method > HRL_GI_RAY_TRACING)
		return HRL_FALSE;
	return g_gl33_gi->Supports((HRL_EGlobalIlluminationMethod)method) ? HRL_TRUE : HRL_FALSE;
}

uint32_t GL33_GetGlobalIlluminationSupportedMethods()
{
	return g_gl33_gi ? g_gl33_gi->GetSupportedMethods() : 0u;
}

//Color Picking
void GL33_EnableColorPickingBuffer(HRL_id _scene, int _enable)
{
	auto it = GetPrivateContext()->scenes.find(_scene);
	if (it == GetPrivateContext()->scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetHoveredObject: scene ID is not valid");
		return;
	}

	it->second->using_color_picking = _enable;
}


const GL33_Texture* GL33_FindTexture(HRL_id id)
{
    if (!bck_)
        return nullptr;
    auto it = bck_->textures.find(id);
    return it == bck_->textures.end() ? nullptr : it->second;
}
