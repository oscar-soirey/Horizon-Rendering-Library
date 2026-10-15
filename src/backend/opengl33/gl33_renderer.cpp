#include "gl33_renderer.h"

#include "../../hrl_gl.h"

#include "gl33_definitions.h"
#include "gl33_shader.h"
#include "gl33_texture.h"
#include "gi.h"
#include "../common/hrl_builtin_glsl.h"
#include "../../core/voxel_render.h"
#include "../../core/render_common.h"
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
#include <unordered_set>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <iterator>
#include <cstring>
#include <chrono>
#include <sstream>
#include <iomanip>

#include "core/widgets.h"

//HRL PRIVATE
extern HRL_Context* GetPrivateContext();


//UTILS

static void InitTextureAndBindToFBO(GLuint _texture, GLuint _fbo, int width, int height);
static const std::vector<unsigned char>* GL33_TextureCPUData(HRL_id id, int* width, int* height);

static bool BindMaterial(HRL_Material* mat, HRL_id object_id, const HRL_Mesh* mesh, const glm::mat4& model);
static bool MaterialIsTwoSided(const HRL_Material* material)
{
	// Flag maintenu par HRL_MaterialSetInt/Bool : pas de hash de chaine par draw.
	return material && material->two_sided_;
}

static bool MaterialUsesScreenSpaceDisplacement(const HRL_Material* material);
static bool CaptureSceneColorForDisplacement(const GL_Scene* scene, GLuint destinationTexture);
static void DrawScreenSpaceDisplacementMeshes(HRL_id scene_id, const hrl_scene_t* scene, const FrustumPlaneSet& frustum);

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
static void DrawScreenMessages(const hrl_scene_t* scene, const HRL_Viewport* viewport);
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
static void ResolveSceneMSAA(GL_Scene* scene, int x, int y, int width, int height);
static bool EnsureShadowResource(HRL_Light* light);
static void DrawLandscapes(HRL_id scene_id, const hrl_scene_t* scene, const FrustumPlaneSet& frustum);
static void DrawLandscapeShadowCasters(hrl_scene_t* scene, const glm::mat4& lightViewProjection, const glm::mat4& lightView, const glm::mat4& lightProjection, int shadowResolution, bool pointLight, const glm::vec3& lightPosition, float farPlane, int face);

static void DrawVoxelWorld(HRL_id scene_id, const hrl_scene_t* scene, const FrustumPlaneSet& frustum);
static void SyncVoxelChunks(HRL_id scene_id, const hrl_scene_t* scene);
static void DestroyAllVoxelChunks(HRL_id scene_id);






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
// Voxel light field (sprites, meshes, voxels). Needs 17 texture units : the
// field is disabled on a GPU that has only the 16 required by OpenGL 3.3.
#define VOXEL_LIGHT_FIELD_TEXTURE_UNIT 16
// Texture array of the voxel types (voxel shader only, no material there).
#define VOXEL_TEXTURE_ARRAY_UNIT 0
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

	// Sampler object forcing NEAREST min/mag filtering. It is bound over the
	// texture units while sprites are drawn, so sprites are always pixel-perfect
	// whatever filter the underlying texture was created/configured with.
	GLuint nearest_sampler = 0;

	struct MeshLOD_GPU {
		GLuint vao = 0;
		GLuint vbo = 0;
		GLuint ebo = 0;
		GLsizei vertex_count = 0;
		GLsizei index_count = 0;
		bool indexed = false;
		// Numero unique attribue a chaque upload (les noms GL peuvent etre
		// recycles apres glDelete*). Sert a detecter un changement de geometrie.
		uint64_t upload_serial = 0;
	};
	struct MeshGPU {
		std::vector<MeshLOD_GPU> levels;
	};
	std::unordered_map<HRL_id, MeshGPU> meshes;

	struct VoxelChunkGPU {
		GLuint vao = 0;
		GLuint vbo = 0;
		GLuint ebo = 0;
		GLsizei vertex_count = 0;
		GLsizei index_count = 0;

		// Per-chunk light UBO. Its contents are uploaded only when the scene/world
		// light set changes, instead of rewriting the UBO every frame.
		GLuint light_ubo = 0;
		uint64_t light_revision = 0;
		int light_count = 0;
		int shadow_light_count = 0;
		std::vector<GL_Light> lights;

		// Emissive geometry is kept in a second sparse mesh. Non-emissive voxels
		// therefore do not carry an extra color attribute or GPU vertex storage.
		GLuint emissive_vao = 0;
		GLuint emissive_vbo = 0;
		GLuint emissive_ebo = 0;
		GLsizei emissive_vertex_count = 0;
		GLsizei emissive_index_count = 0;

		uint64_t geometry_revision = 0;
		uint64_t color_revision = 0;
	};
	std::unordered_map<HRL_id, std::unordered_map<uint64_t, VoxelChunkGPU>> voxel_chunks;

	// Voxel emitters are derived from the CPU voxel world, but they do not need
	// to be rediscovered every frame. Rebuild this cache only when voxel data or
	// voxel lighting configuration changes. This is critical for large worlds:
	// a 4096x4096 empty world must not mean scanning 16.7M voxels every frame.
	struct VoxelEmitterCandidate
	{
		float energy = 0.f;
		glm::vec3 position{0.f};
		glm::vec3 color{0.f};
	};
	struct VoxelLightCache
	{
		// Emetteurs candidats par chunk (liste vide = chunk scanne sans emetteur).
		// Une edition ne rescanne que le chunk touche, plus le monde entier.
		std::unordered_map<uint64_t, std::vector<VoxelEmitterCandidate>> chunk_emitters;
		const HRL_VoxelWorld* world = nullptr;
		uint64_t geometry_revision = 0;
		uint64_t color_revision = 0;
		uint64_t revision = 0;
		uint64_t signature = 0;
		bool valid = false;
		int scene_light_count = 0;
		int scene_shadow_light_count = 0;
		std::array<GL_Light, 32> scene_lights{};
		std::vector<GL_Light> lights;
	};
	std::unordered_map<HRL_id, VoxelLightCache> voxel_light_cache;

	// Contenu actuel de ubo[UBO_LIGHTS] (scene + signature du VoxelLightCache).
	// UploadSceneLights ne reecrit les ~40 Ko de l'UBO que s'ils changent.
	// Framebuffer cible des passes d'overlay (gizmos, textes, messages, widgets) :
	// le FBO de la scene en cours, pour qu'elles fassent partie de l'image finale
	// de la scene, qu'elle soit affichee a l'ecran ou non.
	GLuint overlay_fbo = 0;

	HRL_id light_ubo_scene = HRL_INVALID_ID;
	uint64_t light_ubo_signature = 0;
	bool light_ubo_valid = false;

	GL33_Shader* voxel_shader = nullptr;

	// Light of the emissive voxels : a GPU light field rebuilt for each
	// viewport (emission -> downsample pyramid -> weighted upsample), read by
	// the voxel, sprite and mesh shaders. Its cost does not depend on the
	// number of emitters. See BuildVoxelLightField().
	struct VoxelLightField
	{
		GL33_Shader* emit_shader = nullptr;
		GL33_Shader* down_shader = nullptr;
		GL33_Shader* up_shader = nullptr;
		GLuint fbo = 0;
		std::vector<GLuint> down;       // level 0 : emission, then box-filtered halves
		std::vector<GLuint> up;         // accumulated light, level 0 is the result
		std::vector<glm::ivec2> sizes;
		bool supported = false;
		// Valid for the viewport being drawn.
		bool active = false;
		glm::vec4 region{0.f, 0.f, 1.f, 1.f}; // world min xy, 1 / world size
		float falloff_world = 1.f;
	} voxel_light_field;

	// Texture array of the textured voxel types, per scene.
	struct VoxelTextureArray
	{
		GLuint texture = 0;
		uint64_t revision = 0;
		int width = 0;
		int height = 0;
		int layers = 0;
		bool complete = false;   // false : a texture was missing, retried later
		uint32_t retry_counter = 0;
	};
	std::unordered_map<HRL_id, VoxelTextureArray> voxel_texture_arrays;

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
		uint64_t upload_serial = 0;
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
		// Empreinte de tout ce qui a ete rendu dans cette shadow map (parametres
		// de la lumiere + casters dans sa zone d'influence). Si elle n'a pas
		// change, la shadow map est reutilisee telle quelle.
		uint64_t content_signature = 0;
		bool content_valid = false;
	};
	std::unordered_map<HRL_id, ShadowGPU> shadow_maps;
	uint64_t gpu_upload_serial = 0;
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
	GLuint ui_white_texture = 0; // widgets without texture (created on first use)

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

	// Cached SDF textures for immutable screen messages. Each message owns its
	// font/size/color values, so later configuration changes never affect it.
	struct ScreenMessageTextGPU {
		HRL_id texture = HRL_INVALID_ID;
		HRL_id font = HRL_INVALID_ID;
		float size = 0.0f;
		std::string text;
		int width = 0;
		int height = 0;
	};
	std::unordered_map<HRL_id, ScreenMessageTextGPU> screen_message_textures;
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

    const float time = HRL_InternalRenderTimeSeconds();
    bck_->volumetric_cloud_shader->SetFloat("uTime", time);

    glBindVertexArray(bck_->vao[BUFFER_QUAD]);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    srcIndex = bck_->post_textures[dst];
}










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




struct SpriteBatchInstance
{
	glm::mat4 model;
	glm::vec4 uvRegion;
	uint32_t spriteId = 0;
	uint32_t padding[3] = {0u, 0u, 0u};
};











// Etat laisse par les passes opaques (landscapes, meshes). Remplace l'ancienne
// "restauration" basee sur glGet* : en profil core, glPolygonMode(GL_FRONT/
// GL_BACK, ...) est de toute facon invalide (GL_INVALID_ENUM) et ne restaurait
// rien. On laisse un etat connu : remplissage plein (les passes 2D suivantes
// en ont besoin) et culling arriere sauf en vue fil de fer.
static void RestoreOpaqueStageState(bool wireframe)
{
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	if (wireframe)
		glDisable(GL_CULL_FACE);
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
	}
}



using GL33_VoxelVertex = HRL_VoxelMeshVertex;

static void DestroyVoxelSubmeshGPU(GLuint& vao, GLuint& vbo, GLuint& ebo,
    GLsizei& vertexCount, GLsizei& indexCount)
{
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ebo) glDeleteBuffers(1, &ebo);
    vao = 0;
    vbo = 0;
    ebo = 0;
    vertexCount = 0;
    indexCount = 0;
}

static bool UploadVoxelSubmeshGPU(GLuint& vao, GLuint& vbo, GLuint& ebo,
    GLsizei& vertexCount, GLsizei& indexCount,
    const std::vector<GL33_VoxelVertex>& vertices, const std::vector<HRL_uint>& indices)
{
    if (vertices.empty() || indices.empty())
    {
        // Les objets GL existants sont conserves pour une prochaine edition ;
        // un compteur nul suffit pour que le chunk ne soit pas dessine.
        vertexCount = 0;
        indexCount = 0;
        return true;
    }
    if (indices.size() > static_cast<size_t>(std::numeric_limits<GLsizei>::max()) ||
        vertices.size() > static_cast<size_t>(std::numeric_limits<GLsizei>::max()))
        return false;

    // Reutilise le VAO/VBO/EBO du chunk : une edition ne fait plus
    // glDelete* + glGen* + reconfiguration complete des attributs.
    const bool created = vao == 0;
    if (created)
    {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
        if (!vao || !vbo || !ebo)
        {
            DestroyVoxelSubmeshGPU(vao, vbo, ebo, vertexCount, indexCount);
            return false;
        }
    }

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GL33_VoxelVertex)), vertices.data(), GL_STATIC_DRAW);
    if (created)
    {
        const GLsizei stride = static_cast<GLsizei>(sizeof(GL33_VoxelVertex));
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(GL33_VoxelVertex, position)));
        glEnableVertexAttribArray(0);
        // Attribut 1 (normale) : pas de tableau, valeur generique fixee au dessin.
        glDisableVertexAttribArray(1);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(GL33_VoxelVertex, color)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(GL33_VoxelVertex, layer)));
        glEnableVertexAttribArray(3);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(HRL_uint)), indices.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);

    vertexCount = static_cast<GLsizei>(vertices.size());
    indexCount = static_cast<GLsizei>(indices.size());
    return true;
}

static void DestroyVoxelChunkGPU(GL33_Backend::VoxelChunkGPU& gpu)
{
    if (gpu.light_ubo)
        glDeleteBuffers(1, &gpu.light_ubo);
    gpu.light_ubo = 0;
    gpu.light_revision = 0;
    gpu.light_count = 0;

    DestroyVoxelSubmeshGPU(gpu.vao, gpu.vbo, gpu.ebo, gpu.vertex_count, gpu.index_count);
    DestroyVoxelSubmeshGPU(gpu.emissive_vao, gpu.emissive_vbo, gpu.emissive_ebo,
        gpu.emissive_vertex_count, gpu.emissive_index_count);
    gpu.geometry_revision = 0;
    gpu.color_revision = 0;
}

static bool RebuildVoxelChunk(const HRL_VoxelWorld* world, int chunkX, int chunkY, GL33_Backend::VoxelChunkGPU& gpu)
{
    // Tampons CPU reutilises d'une reconstruction a l'autre (thread GL unique).
    static std::vector<GL33_VoxelVertex> vertices;
    static std::vector<HRL_uint> indices;
    static std::vector<GL33_VoxelVertex> emissiveVertices;
    static std::vector<HRL_uint> emissiveIndices;
    if (!BuildVoxelChunkGeometry(world, chunkX, chunkY, vertices, indices, emissiveVertices, emissiveIndices))
        return false;

    // Les buffers GL sont mis a jour en place. L'UBO de lumieres du chunk est
    // conserve, mais sa liste est recalculee (les bornes du chunk peuvent avoir
    // change avec la taille des voxels/chunks).
    gpu.light_revision = 0;
    if (!UploadVoxelSubmeshGPU(gpu.vao, gpu.vbo, gpu.ebo, gpu.vertex_count, gpu.index_count, vertices, indices))
    {
        DestroyVoxelChunkGPU(gpu);
        return false;
    }
    if (!UploadVoxelSubmeshGPU(gpu.emissive_vao, gpu.emissive_vbo, gpu.emissive_ebo,
        gpu.emissive_vertex_count, gpu.emissive_index_count, emissiveVertices, emissiveIndices))
    {
        DestroyVoxelChunkGPU(gpu);
        return false;
    }
    return true;
}

static void DestroyAllVoxelChunks(HRL_id scene_id)
{
	if (!bck_)
		return;
	auto it = bck_->voxel_chunks.find(scene_id);
	if (it == bck_->voxel_chunks.end())
		return;
	for (auto& [key, gpu] : it->second)
	{
		(void)key;
		DestroyVoxelChunkGPU(gpu);
	}
	bck_->voxel_chunks.erase(it);
}

static void SyncVoxelChunks(HRL_id sceneId, const hrl_scene_t* scene)
{
	if (!bck_ || !scene || !scene->voxel_world)
	{
		return;
	}
	HRL_VoxelWorld* world = scene->voxel_world;
	if (world->width_ <= 0 || world->height_ <= 0)
	{
		DestroyAllVoxelChunks(sceneId);
		return;
	}

	auto& chunks = bck_->voxel_chunks[sceneId];
	std::unordered_set<uint64_t> desired;
	for (const auto& [viewportId, viewport] : scene->viewports)
	{
		(void)viewportId;
		CollectVoxelVisibleChunksForViewport(world, viewport, desired);
	}

	for (uint64_t key : desired)
	{
		const int chunkX = VoxelChunkXFromKey(key);
		const int chunkY = VoxelChunkYFromKey(key);
		auto it = chunks.find(key);

		// CPU-side voxel data is lazy after a disk/buffer load. Materialize only
		// chunks that are currently visible (plus the visibility margin selected
		// by CollectVoxelVisibleChunksForViewport).
		HRL_EnsureVoxelChunkLoaded(world, chunkX, chunkY);

		// SMOOTH/BLOCKY lisent les voxels des 8 chunks voisins : ils doivent etre
		// charges avant le maillage, sinon leur bordure serait vue comme vide.
		bool hasGeometrySource = world->FindVoxelChunk(chunkX, chunkY) != nullptr;
		if (world->render_mode_ != HRL_VOXEL_FLAT)
		{
			for (int dy = -1; dy <= 1; ++dy)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					if (dx == 0 && dy == 0)
						continue;
					const int nx = chunkX + dx;
					const int ny = chunkY + dy;
					if (nx < 0 || ny < 0 || nx * world->chunk_size_ >= world->width_ || ny * world->chunk_size_ >= world->height_)
						continue;
					HRL_EnsureVoxelChunkLoaded(world, nx, ny);
					// En SMOOTH, un chunk vide dessine quand meme la demi-bordure
					// des voxels de ses voisins (cellules a cheval sur la frontiere).
					if (world->render_mode_ == HRL_VOXEL_SMOOTH && world->FindVoxelChunk(nx, ny))
						hasGeometrySource = true;
				}
			}
		}

		// Empty chunks have no GPU representation.
		if (!hasGeometrySource)
		{
			if (it != chunks.end())
			{
				DestroyVoxelChunkGPU(it->second);
				chunks.erase(it);
			}
			continue;
		}
		if (it == chunks.end())
		{
			GL33_Backend::VoxelChunkGPU gpu{};
			if (!RebuildVoxelChunk(world, chunkX, chunkY, gpu))
				continue;
			gpu.geometry_revision = world->geometry_revision_;
			gpu.color_revision = world->color_revision_;
			chunks.emplace(key, gpu);
			world->render_dirty_chunks_.erase(key);
			continue;
		}

		const bool dirty = world->render_dirty_chunks_.find(key) != world->render_dirty_chunks_.end();
		const bool revisionChanged = it->second.geometry_revision != world->geometry_revision_ ||
			it->second.color_revision != world->color_revision_;
		if (dirty || revisionChanged)
		{
			if (RebuildVoxelChunk(world, chunkX, chunkY, it->second))
			{
				it->second.geometry_revision = world->geometry_revision_;
				it->second.color_revision = world->color_revision_;
				world->render_dirty_chunks_.erase(key);
			}
		}
	}

	for (auto it = chunks.begin(); it != chunks.end(); )
	{
		if (desired.find(it->first) == desired.end())
		{
			DestroyVoxelChunkGPU(it->second);
			it = chunks.erase(it);
		}
		else
			++it;
	}
}

// =============================================================================
// Voxel textures : one GL_TEXTURE_2D_ARRAY per scene, one layer per textured
// type (HRL_SetVoxelTypeTexture). Rebuilt only when the texture list changes.
// =============================================================================

static void DestroyVoxelTextureArray(HRL_id scene_id)
{
	if (!bck_)
		return;
	auto it = bck_->voxel_texture_arrays.find(scene_id);
	if (it == bck_->voxel_texture_arrays.end())
		return;
	if (it->second.texture)
		glDeleteTextures(1, &it->second.texture);
	bck_->voxel_texture_arrays.erase(it);
}

// Returns the texture array to use (0 : no textured type).
static GLuint EnsureVoxelTextureArray(HRL_id scene_id, const HRL_VoxelWorld* world)
{
	if (!world || world->textured_type_count_ == 0)
	{
		DestroyVoxelTextureArray(scene_id);
		return 0;
	}

	auto& ta = bck_->voxel_texture_arrays[scene_id];
	if (ta.texture && ta.revision == world->texture_revision_)
	{
		// A texture created asynchronously may arrive later : retry from time to time.
		if (ta.complete || (++ta.retry_counter % 30u) != 0u)
			return ta.texture;
	}

	struct Source { int layer; const GL33_Texture* texture; };
	std::vector<Source> sources;
	sources.reserve(world->textured_type_count_);
	int layers = 0;
	int width = 1;
	int height = 1;
	bool complete = true;
	for (size_t type = 1; type < world->type_textures_.size(); ++type)
	{
		const int layer = world->type_texture_layers_[type];
		if (layer < 0)
			continue;
		layers = std::max(layers, layer + 1);
		const GL33_Texture* texture = nullptr;
		auto texIt = bck_->textures.find(world->type_textures_[type]);
		if (texIt != bck_->textures.end() && texIt->second &&
			texIt->second->GetWidth() > 0 && texIt->second->GetHeight() > 0 &&
			!texIt->second->GetCpuRGBA().empty())
		{
			texture = texIt->second;
			width = std::max(width, static_cast<int>(texture->GetWidth()));
			height = std::max(height, static_cast<int>(texture->GetHeight()));
		}
		else
			complete = false;
		sources.push_back({ layer, texture });
	}
	if (layers <= 0)
	{
		DestroyVoxelTextureArray(scene_id);
		return 0;
	}
	width = std::min(width, 256);
	height = std::min(height, 256);

	if (!ta.texture)
		glGenTextures(1, &ta.texture);
	glActiveTexture(GL_TEXTURE0 + VOXEL_TEXTURE_ARRAY_UNIT);
	glBindTexture(GL_TEXTURE_2D_ARRAY, ta.texture);
	if (ta.width != width || ta.height != height || ta.layers != layers)
	{
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, width, height, layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		ta.width = width;
		ta.height = height;
		ta.layers = layers;
	}

	// Nearest resampling on the CPU (pixel art stays sharp), white if missing.
	static std::vector<unsigned char> pixels;
	pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
	for (const Source& src : sources)
	{
		if (!src.texture)
		{
			std::fill(pixels.begin(), pixels.end(), static_cast<unsigned char>(255));
		}
		else
		{
			const int sw = static_cast<int>(src.texture->GetWidth());
			const int sh = static_cast<int>(src.texture->GetHeight());
			const std::vector<unsigned char>& rgba = src.texture->GetCpuRGBA();
			for (int y = 0; y < height; ++y)
			{
				const int sy = std::min(sh - 1, (y * sh) / height);
				for (int x = 0; x < width; ++x)
				{
					const int sx = std::min(sw - 1, (x * sw) / width);
					const size_t from = (static_cast<size_t>(sy) * static_cast<size_t>(sw) + static_cast<size_t>(sx)) * 4u;
					const size_t to = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
					if (from + 3u < rgba.size())
						std::memcpy(&pixels[to], &rgba[from], 4u);
				}
			}
		}
		glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, src.layer, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	}

	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

	ta.revision = world->texture_revision_;
	ta.complete = complete;
	ta.retry_counter = 0;
	return ta.texture;
}


// =============================================================================
// Emissive voxel lighting : GPU light field
// -----------------------------------------------------------------------------
// The previous system turned the brightest emissive voxels into point lights
// (at most 256, every fragment looping over them). Now, for each viewport :
//
//   1. the emissive geometry of the loaded chunks is drawn into a small
//      texture covering the view + the light reach (1 texel = 1 voxel, or
//      more voxels when the view is very large) ;
//   2. a box-filtered pyramid of that emission is built (each level halves) ;
//   3. the levels are added back from the coarsest one, each with a weight
//      (and a tent upsample) : level l holds the energy of blocks of 2^l
//      texels, so the sum reproduces the falloff of a point light
//        f(d) = (0.5 / sqrt(d^2 + 0.25)) / (1 + (d^2 + 0.25) / F^2)
//      (F : HRL_SetVoxelEmissiveLighting falloff, the attenuation of the old
//      voxel point lights, Lambert term included).
//
// The shaders read the result with a single texture fetch. The cost depends
// only on the size of the view, never on the number of emissive voxels. The
// region is snapped to the coarsest texel so the light does not shimmer when
// the camera moves. Light is not occluded (like the old voxel lights).
// =============================================================================




// Half-size downsample : 4 bilinear taps one source texel around the
// destination center (a 4x4 tent). Smoother than a 2x2 box, so the light of an
// emitter barely depends on where it sits in the coarse texels.


static void DestroyVoxelLightFieldTextures()
{
	auto& lf = bck_->voxel_light_field;
	if (!lf.down.empty())
		glDeleteTextures(static_cast<GLsizei>(lf.down.size()), lf.down.data());
	if (!lf.up.empty())
		glDeleteTextures(static_cast<GLsizei>(lf.up.size()), lf.up.data());
	lf.down.clear();
	lf.up.clear();
	lf.sizes.clear();
}

static void InitVoxelLightField()
{
	auto& lf = bck_->voxel_light_field;
	GLint units = 0;
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
	if (units <= VOXEL_LIGHT_FIELD_TEXTURE_UNIT)
		return; // unsupported : emissive voxels glow but light nothing

	auto make = [](const char* vs, const char* fs) -> GL33_Shader*
	{
		auto* shader = new GL33_Shader();
		if (shader->GL33_Create(vs, std::strlen(vs), fs, std::strlen(fs)) != 0)
		{
			delete shader;
			return nullptr;
		}
		return shader;
	};
	lf.emit_shader = make(kVoxelLightEmitVS, kVoxelLightEmitFS);
	lf.down_shader = make(kVoxelLightFullscreenVS, kVoxelLightDownFS);
	lf.up_shader = make(kVoxelLightFullscreenVS, kVoxelLightUpFS);
	glGenFramebuffers(1, &lf.fbo);
	lf.supported = lf.emit_shader && lf.down_shader && lf.up_shader && lf.fbo;
}

static void ShutdownVoxelLightField()
{
	auto& lf = bck_->voxel_light_field;
	DestroyVoxelLightFieldTextures();
	delete lf.emit_shader;
	delete lf.down_shader;
	delete lf.up_shader;
	lf.emit_shader = lf.down_shader = lf.up_shader = nullptr;
	if (lf.fbo)
		glDeleteFramebuffers(1, &lf.fbo);
	lf.fbo = 0;
	lf.supported = false;
	lf.active = false;
}

static void EnsureVoxelLightFieldTextures(int width0, int height0, int levels)
{
	auto& lf = bck_->voxel_light_field;
	if (static_cast<int>(lf.sizes.size()) == levels && !lf.sizes.empty() &&
		lf.sizes[0].x == width0 && lf.sizes[0].y == height0)
		return;

	DestroyVoxelLightFieldTextures();
	lf.down.resize(static_cast<size_t>(levels));
	lf.up.resize(static_cast<size_t>(levels));
	lf.sizes.resize(static_cast<size_t>(levels));
	glGenTextures(levels, lf.down.data());
	glGenTextures(levels, lf.up.data());
	for (int l = 0; l < levels; ++l)
	{
		const glm::ivec2 size(std::max(1, width0 >> l), std::max(1, height0 >> l));
		lf.sizes[static_cast<size_t>(l)] = size;
		for (GLuint tex : { lf.down[static_cast<size_t>(l)], lf.up[static_cast<size_t>(l)] })
		{
			glBindTexture(GL_TEXTURE_2D, tex);
			// Packed float HDR : 4 bytes per texel, filterable and renderable.
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R11F_G11F_B10F, size.x, size.y, 0, GL_RGB, GL_FLOAT, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
	}
	glBindTexture(GL_TEXTURE_2D, 0);
}







// Builds the light field of the viewport about to be drawn. Called before
// the viewport binds its framebuffer : leaves the depth test on, blending off.
static void BuildVoxelLightField(HRL_id scene_id, const hrl_scene_t* scene, const HRL_Viewport* viewport)
{
	auto& lf = bck_->voxel_light_field;
	lf.active = false;
	if (!lf.supported || !scene || !scene->voxel_world || !viewport)
		return;
	const HRL_VoxelWorld* world = scene->voxel_world;
	const float reach = VoxelLightReachVoxels(world);
	if (reach <= 0.f || world->voxel_size_ <= 0.f || world->chunk_size_ <= 0 ||
		scene->debug_view == HRL_DEBUG_VIEW_WIREFRAME || scene->debug_view == HRL_DEBUG_VIEW_UNLIT)
		return;
	auto mapIt = bck_->voxel_chunks.find(scene_id);
	if (mapIt == bck_->voxel_chunks.end() || mapIt->second.empty())
		return;

	float minWX, minWY, maxWX, maxWY;
	if (!ComputeVoxelViewWorldRect(world, viewport, minWX, minWY, maxWX, maxWY))
		return;

	// Region in voxels : the view + the reach of the light.
	const float vs = world->voxel_size_;
	const float rx0 = minWX / vs - reach, ry0 = minWY / vs - reach;
	const float rx1 = maxWX / vs + reach, ry1 = maxWY / vs + reach;
	if (!(rx1 > rx0) || !(ry1 > ry0) || !std::isfinite(rx0 + ry0 + rx1 + ry1))
		return;

	// Voxels per texel : 1, or more for a very large view (zoomed out).
	constexpr float kMaxSide = 1024.f;
	int s = 1;
	while (std::max(rx1 - rx0, ry1 - ry0) / static_cast<float>(s) > kMaxSide && s < 64)
		s *= 2;

	// Levels until a texel of the coarsest one covers the reach.
	int levels = 1;
	while (levels < 10 && 0.75f * static_cast<float>(s << (levels - 1)) < reach)
		++levels;

	// Snap the region to the coarsest texels (no shimmering when the camera
	// moves), sizes rounded up so the textures are rarely reallocated.
	const int coarse = s << (levels - 1);
	const int sizeStep = std::max(coarse, s * 32);
	const int x0 = static_cast<int>(std::floor(rx0 / coarse)) * coarse;
	const int y0 = static_cast<int>(std::floor(ry0 / coarse)) * coarse;
	int spanX = static_cast<int>(std::ceil((rx1 - static_cast<float>(x0)) / sizeStep)) * sizeStep;
	int spanY = static_cast<int>(std::ceil((ry1 - static_cast<float>(y0)) / sizeStep)) * sizeStep;
	spanX = std::max(spanX, coarse);
	spanY = std::max(spanY, coarse);
	const int width0 = spanX / s;
	const int height0 = spanY / s;
	if (width0 <= 0 || height0 <= 0 || width0 > 4096 || height0 > 4096)
		return;

	EnsureVoxelLightFieldTextures(width0, height0, levels);

	// GL state : restored for the scene passes that follow.
	const GLboolean cullWasOn = glIsEnabled(GL_CULL_FACE);
	const GLboolean scissorWasOn = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glBindFramebuffer(GL_FRAMEBUFFER, lf.fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	// 1. Emission of the loaded chunks inside the region.
	const float regionMinX = static_cast<float>(x0) * vs;
	const float regionMinY = static_cast<float>(y0) * vs;
	const float regionW = static_cast<float>(spanX) * vs;
	const float regionH = static_cast<float>(spanY) * vs;
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lf.down[0], 0);
	glViewport(0, 0, width0, height0);
	glClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);
	lf.emit_shader->Use();
	lf.emit_shader->SetVec4("Region", glm::vec4(regionMinX, regionMinY, 1.f / regionW, 1.f / regionH));
	int emitterChunks = 0;
	const float chunkWorld = static_cast<float>(world->chunk_size_) * vs;
	for (const auto& [key, gpu] : mapIt->second)
	{
		if (!gpu.emissive_vao || gpu.emissive_index_count <= 0)
			continue;
		const float cx0 = static_cast<float>(VoxelChunkXFromKey(key)) * chunkWorld - vs;
		const float cy0 = static_cast<float>(VoxelChunkYFromKey(key)) * chunkWorld - vs;
		if (cx0 > regionMinX + regionW || cy0 > regionMinY + regionH ||
			cx0 + chunkWorld + 2.f * vs < regionMinX || cy0 + chunkWorld + 2.f * vs < regionMinY)
			continue;
		glBindVertexArray(gpu.emissive_vao);
		glDrawElements(GL_TRIANGLES, gpu.emissive_index_count, GL_UNSIGNED_INT, nullptr);
		++emitterChunks;
	}
	glBindVertexArray(0);

	if (emitterChunks > 0)
	{
		glBindVertexArray(bck_->vao[BUFFER_QUAD]); // attributes unused (gl_VertexID)

		// 2. Box-filtered pyramid.
		lf.down_shader->Use();
		lf.down_shader->SetInt("Src", 0);
		glActiveTexture(GL_TEXTURE0);
		for (int l = 1; l < levels; ++l)
		{
			const glm::ivec2 size = lf.sizes[static_cast<size_t>(l)];
			const glm::ivec2 src = lf.sizes[static_cast<size_t>(l - 1)];
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lf.down[static_cast<size_t>(l)], 0);
			glViewport(0, 0, size.x, size.y);
			glBindTexture(GL_TEXTURE_2D, lf.down[static_cast<size_t>(l - 1)]);
			lf.down_shader->SetVec2("SrcTexel", glm::vec2(1.f / static_cast<float>(src.x), 1.f / static_cast<float>(src.y)));
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}

		// 3. Weighted sum, coarsest level first. The pyramid conserves energy,
		//    so level l receives the energy of the falloff between the radii
		//    r_l and r_(l+1) (integral of f over that ring) : the sum then
		//    follows f (checked numerically against the point-light curve).
		const float falloff = world->emissive_light_falloff_;
		const float intensity = world->emissive_light_intensity_;
		const std::array<float, 10>& weights = VoxelLightLevelWeights(falloff, s, levels);
		lf.up_shader->Use();
		lf.up_shader->SetInt("Cur", 0);
		lf.up_shader->SetInt("Low", 1);
		for (int l = levels - 1; l >= 0; --l)
		{
			const glm::ivec2 size = lf.sizes[static_cast<size_t>(l)];
			const float weight = intensity * weights[static_cast<size_t>(l)];
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lf.up[static_cast<size_t>(l)], 0);
			glViewport(0, 0, size.x, size.y);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, lf.down[static_cast<size_t>(l)]);
			const bool hasLow = l + 1 < levels;
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, hasLow ? lf.up[static_cast<size_t>(l + 1)] : 0);
			lf.up_shader->SetInt("HasLow", hasLow ? 1 : 0);
			lf.up_shader->SetFloat("Weight", weight);
			if (hasLow)
			{
				const glm::ivec2 low = lf.sizes[static_cast<size_t>(l + 1)];
				lf.up_shader->SetVec2("LowTexel", glm::vec2(1.f / static_cast<float>(low.x), 1.f / static_cast<float>(low.y)));
			}
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindVertexArray(0);

		lf.region = glm::vec4(regionMinX, regionMinY, 1.f / regionW, 1.f / regionH);
		lf.falloff_world = falloff * vs;
		lf.active = true;

		glActiveTexture(GL_TEXTURE0 + VOXEL_LIGHT_FIELD_TEXTURE_UNIT);
		glBindTexture(GL_TEXTURE_2D, lf.up[0]);
		glActiveTexture(GL_TEXTURE0);
	}

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	if (cullWasOn) glEnable(GL_CULL_FACE);
	if (scissorWasOn) glEnable(GL_SCISSOR_TEST);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	ctx_->bound_shader = nullptr;
	ctx_->bound_material = nullptr;
}

// Uniforms of the light field for a shader that reads it (voxel, sprite,
// mesh). Cached by GL33_Shader : only changes reach OpenGL.
static void ApplyVoxelLightFieldUniforms(GL33_Shader* shader)
{
	if (!shader || !bck_)
		return;
	const auto& lf = bck_->voxel_light_field;
	// Always give the sampler its own unit, even unused : by default it would
	// share unit 0 with a sampler of another type (the voxel texture array),
	// and OpenGL then refuses the draw call (GL_INVALID_OPERATION).
	shader->SetInt("VoxelLightField", VOXEL_LIGHT_FIELD_TEXTURE_UNIT);
	shader->SetInt("VoxelLightEnabled", lf.active ? 1 : 0);
	if (!lf.active)
		return;
	shader->SetVec4("VoxelLightRegion", lf.region);
	shader->SetFloat("VoxelLightFalloff", lf.falloff_world);
}


static void DrawVoxelWorld(HRL_id scene_id, const hrl_scene_t* scene, const FrustumPlaneSet& frustum)
{
	if (!bck_ || !scene || !scene->voxel_world || !bck_->voxel_shader || !ctx_ || !ctx_->viewport || !ctx_->viewport->camera_)
		return;

	auto mapIt = bck_->voxel_chunks.find(scene_id);
	if (mapIt == bck_->voxel_chunks.end() || mapIt->second.empty())
		return;

	GL33_Shader* shader = bck_->voxel_shader;
	shader->Use();
	// Normale constante des voxels 2D (attribut 1 sans tableau, voir GL33_VoxelVertex).
	glVertexAttrib3f(1, 0.f, 0.f, 1.f);
	ctx_->shader = shader;
	ctx_->bound_shader = shader;
	ctx_->bound_material = nullptr;
	shader->SetMat4("projection", ctx_->proj_mat);
	shader->SetMat4("view", ctx_->view_mat);
	shader->SetMat4("model", glm::mat4(1.f));
	shader->SetVec3("CamPos", ctx_->viewport->camera_->position_);
	shader->SetFloat("BrightThreshold", 1.0f);
	shader->SetFloat("VoxelSize", scene->voxel_world->voxel_size_);
	// Display limiter: leave normal colors alone up to 75%, then softly
	// compress HDR peaks toward 1.0 while preserving the voxel hue.
	shader->SetInt("VoxelEmissionPass", 0);
	shader->SetInt("DebugView", static_cast<int>(scene->debug_view));
	shader->SetInt("FogEnabled", scene->fog.enabled ? 1 : 0);
	shader->SetInt("FogMode", static_cast<int>(scene->fog.mode));
	shader->SetVec4("FogColor", glm::vec4(scene->fog.r, scene->fog.g, scene->fog.b, 1.f));
	shader->SetFloat("FogStart", scene->fog.range_start);
	shader->SetFloat("FogEnd", scene->fog.range_end);
	shader->SetFloat("FogDensity", scene->fog.density);
	// The light list is selected per visible chunk below. Start from zero so a
	// frame with no usable chunk-light cache can never inherit a stale count.
	shader->SetInt("VoxelLightCount", 0);
	shader->SetInt("VoxelShadowLightCount", 0);

	shader->SetInt("ShadowMap2D_0", SHADOW_2D_TEXTURE_UNIT_BASE + 0);
	shader->SetInt("ShadowMap2D_1", SHADOW_2D_TEXTURE_UNIT_BASE + 1);
	shader->SetInt("ShadowMap2D_2", SHADOW_2D_TEXTURE_UNIT_BASE + 2);
	shader->SetInt("ShadowMap2D_3", SHADOW_2D_TEXTURE_UNIT_BASE + 3);
	shader->SetInt("ShadowMapCube_0", SHADOW_CUBE_TEXTURE_UNIT_BASE + 0);
	shader->SetInt("ShadowMapCube_1", SHADOW_CUBE_TEXTURE_UNIT_BASE + 1);
	shader->SetInt("ShadowMapCube_2", SHADOW_CUBE_TEXTURE_UNIT_BASE + 2);
	shader->SetInt("ShadowMapCube_3", SHADOW_CUBE_TEXTURE_UNIT_BASE + 3);

	const bool wireframe = scene->debug_view == HRL_DEBUG_VIEW_WIREFRAME;
	// DrawScene enters the opaque stage with blending disabled and writable depth.
	// Establish the complete voxel state directly instead of querying OpenGL.
	// glGet*/glIsEnabled can force a driver synchronization and are needlessly
	// expensive on a per-frame rendering path.
	glDisable(GL_CULL_FACE); // 2D voxel squares are intentionally double-sided.
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	if (wireframe)
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	else
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	// Build the visible chunk list once. The emissive pass reuses the same list,
	// avoiding a second pass over every chunk and duplicate frustum/bounds math.
	struct VoxelVisibleChunk
	{
		uint64_t key = 0;
		GL33_Backend::VoxelChunkGPU* gpu = nullptr;
		glm::vec3 center{ 0.f };
		float radius = 0.f;
	};
	std::vector<VoxelVisibleChunk> visibleChunks;
	visibleChunks.reserve(mapIt->second.size());

	for (auto& [key, gpu] : mapIt->second)
	{
		if (!gpu.vao || gpu.index_count <= 0)
			continue;
		const int chunkX = VoxelChunkXFromKey(key);
		const int chunkY = VoxelChunkYFromKey(key);
		const int startX = chunkX * scene->voxel_world->chunk_size_;
		const int startY = chunkY * scene->voxel_world->chunk_size_;
		const int endX = std::min(startX + scene->voxel_world->chunk_size_, scene->voxel_world->width_);
		const int endY = std::min(startY + scene->voxel_world->chunk_size_, scene->voxel_world->height_);
		if (startX >= endX || startY >= endY)
			continue;

		const glm::vec3 center(
			(0.5f * static_cast<float>(startX + endX)) * scene->voxel_world->voxel_size_,
			(0.5f * static_cast<float>(startY + endY)) * scene->voxel_world->voxel_size_,
			0.f);
		// En SMOOTH, la geometrie d'un chunk deborde d'un demi-voxel sur ses
		// voisins : marge d'un voxel sur les bornes (culling et lumieres).
		const float boundsMargin = scene->voxel_world->render_mode_ == HRL_VOXEL_FLAT ? 0.f : 1.f;
		const float dx = (0.5f * static_cast<float>(endX - startX) + boundsMargin) * scene->voxel_world->voxel_size_;
		const float dy = (0.5f * static_cast<float>(endY - startY) + boundsMargin) * scene->voxel_world->voxel_size_;
		const float radius = std::sqrt(dx * dx + dy * dy);
		if (!SphereInsideFrustum(frustum, center, radius))
			continue;

		visibleChunks.push_back({ key, &gpu, center, radius });
	}

	// Lights : the scene lights of the shared UBO (binding 0, at most 32). The
	// emissive voxels no longer are point lights : their light comes from the
	// light field (one texture read), whatever their number.
	{
		auto cacheIt = bck_->voxel_light_cache.find(scene_id);
		const int lightCount = cacheIt != bck_->voxel_light_cache.end() ? cacheIt->second.scene_light_count : 0;
		const int shadowCount = cacheIt != bck_->voxel_light_cache.end() ? cacheIt->second.scene_shadow_light_count : 0;
		shader->SetInt("VoxelLightCount", lightCount);
		shader->SetInt("VoxelShadowLightCount", shadowCount);
		glBindBufferBase(GL_UNIFORM_BUFFER, 0, bck_->ubo[UBO_LIGHTS]);
	}
	ApplyVoxelLightFieldUniforms(shader);

	// Textures of the voxel types.
	const GLuint textureArray = EnsureVoxelTextureArray(scene_id, scene->voxel_world);
	shader->SetInt("VoxelTexturesEnabled", textureArray ? 1 : 0);
	shader->SetInt("VoxelTextures", VOXEL_TEXTURE_ARRAY_UNIT);
	if (textureArray)
	{
		glActiveTexture(GL_TEXTURE0 + VOXEL_TEXTURE_ARRAY_UNIT);
		glBindTexture(GL_TEXTURE_2D_ARRAY, textureArray);

		// World size of one copy of each texture (4 layers per vec4).
		static std::array<glm::vec4, 64> tiles;
		tiles.fill(glm::vec4(1.f));
		const HRL_VoxelWorld* world = scene->voxel_world;
		int layerCount = 0;
		for (size_t type = 1; type < world->type_texture_layers_.size(); ++type)
		{
			const int layer = world->type_texture_layers_[type];
			if (layer < 0 || layer >= 256)
				continue;
			tiles[static_cast<size_t>(layer >> 2)][layer & 3] = world->type_texture_tiles_[type] * world->voxel_size_;
			layerCount = std::max(layerCount, layer + 1);
		}
		const GLint location = glGetUniformLocation(static_cast<GLuint>(shader->GetId()), "VoxelTexTiles");
		if (location >= 0 && layerCount > 0)
			glUniform4fv(location, (layerCount + 3) / 4, &tiles[0].x);
	}

	for (const VoxelVisibleChunk& visible : visibleChunks)
	{
		glBindVertexArray(visible.gpu->vao);
		glDrawElements(GL_TRIANGLES, visible.gpu->index_count, GL_UNSIGNED_INT, nullptr);
	}

	glBindVertexArray(0);

	// Emissive voxels are rendered additively in a second, sparse geometry pass.
	// Only the first two MRTs are touched here; GI and picking buffers keep the
	// values produced by the regular shaded pass.
	if (!wireframe)
	{
		glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glColorMaski(2, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glColorMaski(3, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glColorMaski(4, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

		glDepthFunc(GL_EQUAL);
	glDepthMask(GL_FALSE);
		glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		shader->SetInt("VoxelEmissionPass", 1);

		for (const VoxelVisibleChunk& visible : visibleChunks)
		{
			const auto& gpu = *visible.gpu;
			if (!gpu.emissive_vao || gpu.emissive_index_count <= 0)
				continue;
			glBindVertexArray(gpu.emissive_vao);
			glDrawElements(GL_TRIANGLES, gpu.emissive_index_count, GL_UNSIGNED_INT, nullptr);
		}

		shader->SetInt("VoxelEmissionPass", 0);
		glBindVertexArray(0);
		for (GLuint i = 0; i < 5; ++i)
			glColorMaski(i, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}

	// Leave the opaque-stage state in the known configuration expected by the
	// following mesh/sprite passes. No driver state queries are needed.
	glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
	if (wireframe) glDisable(GL_CULL_FACE); else { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);

	// The shared scene-light UBO stays bound at binding 0 for the rest of the renderer.
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, bck_->ubo[UBO_LIGHTS]);
	if (textureArray)
	{
		glActiveTexture(GL_TEXTURE0 + VOXEL_TEXTURE_ARRAY_UNIT);
		glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
		glActiveTexture(GL_TEXTURE0);
	}
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
	HRL_InternalSetTextureCPUDataLookup(GL33_TextureCPUData);

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

	//2D VOXEL SHADER
	bck_->voxel_shader = new GL33_Shader();
	if (bck_->voxel_shader->GL33_Create(
		kVoxel2DVertexShader, std::strlen(kVoxel2DVertexShader),
		kVoxel2DFragmentShader, std::strlen(kVoxel2DFragmentShader)) != 0)
	{
		delete bck_->voxel_shader;
		bck_->voxel_shader = nullptr;
	}
	else
	{
		// Voxel HDR emission starts above 1.0. A color of (5,5,5) therefore
		// becomes a bloom source with intensity 5 while colors <= 1 stay unlit.
		bck_->voxel_shader->SetFloat("BrightThreshold", 1.0f);
	}

	// Light of the emissive voxels (light field shaders + framebuffer).
	InitVoxelLightField();

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
	CreateSpriteGeometry();

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

	for (auto& [scene_id, chunks] : bck_->voxel_chunks)
	{
		(void)scene_id;
		for (auto& [key, gpu] : chunks)
		{
			(void)key;
			if (gpu.light_ubo) glDeleteBuffers(1, &gpu.light_ubo);
			if (gpu.vao) glDeleteVertexArrays(1, &gpu.vao);
			if (gpu.vbo) glDeleteBuffers(1, &gpu.vbo);
			if (gpu.ebo) glDeleteBuffers(1, &gpu.ebo);
		}
	}
	bck_->voxel_chunks.clear();
	bck_->voxel_light_cache.clear();
	for (auto& [scene_id, textureArray] : bck_->voxel_texture_arrays)
	{
		(void)scene_id;
		if (textureArray.texture) glDeleteTextures(1, &textureArray.texture);
	}
	bck_->voxel_texture_arrays.clear();
	ShutdownVoxelLightField();

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
	if (bck_->ui_white_texture != 0)
	{
		glDeleteTextures(1, &bck_->ui_white_texture);
		bck_->ui_white_texture = 0;
	}
	delete bck_->scene_effect_shader;
	delete bck_->volumetric_cloud_shader;
	delete bck_->ambient_occlusion_shader;
	delete bck_->ssr_shader;
	delete bck_->decal_shader;
	delete bck_->vfx_shader;
	delete bck_->ss_displacement_static_shader;
	delete bck_->ss_displacement_skinned_shader;
	delete bck_->voxel_shader;
	bck_->voxel_shader = nullptr;
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

	// Resize every scene attachment. Attachments 3 and 4 are the GI albedo
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
	// Les uniforms redondants sont filtres dans GL33_Shader. On repart d'un cache
	// vide a chaque scene pour rester robuste si l'application ecrit elle-meme
	// des uniforms via glUniform* sur un programme HRL.
	GL33_Shader::InvalidateAllValueCaches();
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

	// Global illumination, world space part (camera independent) : BVH,
	// probes, voxels. Done before the shadow maps are bound to their units.
	if (g_gl33_gi && scene->global_illumination_enabled && scene->debug_view == HRL_DEBUG_VIEW_NONE)
		g_gl33_gi->UpdateScene(scene_id, scene);

	// Voxel world geometry is streamed from the complete CPU-side voxel array.
	// Only chunks intersecting the scene cameras (plus a small margin) receive
	// OpenGL buffers.
	SyncVoxelChunks(scene_id, scene);

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

	// Independant de la camera : calcule une fois par scene, pas par viewport.
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

	// Viewports dessines dans leur ordre de creation (IDs croissants) : le
	// dernier cree passe au-dessus, ce qui rend les incrustations previsibles.
	// L'ordre d'une unordered_map est arbitraire : un petit viewport pouvait
	// etre entierement recouvert par un viewport plein ecran dessine apres lui.
	static std::vector<std::pair<HRL_id, HRL_Viewport*>> orderedViewports;
	orderedViewports.assign(scene->viewports.begin(), scene->viewports.end());
	std::sort(orderedViewports.begin(), orderedViewports.end(),
		[](const auto& a, const auto& b) { return a.first < b.first; });

	for (const auto& v : orderedViewports)
	{
		if (!v.second || !v.second->camera_)
			continue;
		ctx_->viewport = v.second;
		ctx_->bound_material = nullptr;
		ctx_->bound_shader = nullptr;

		// Light of the emissive voxels seen by this viewport (own framebuffer,
		// before the viewport binds the scene one).
		BuildVoxelLightField(scene_id, scene, v.second);

		float winW = (float)GetWindowWidth();
		float winH = (float)GetWindowHeight();
		// Rectangle du viewport en pixels (meme arrondi que le glViewport).
		const int rectX = (int)(v.second->x_ * winW);
		const int rectY = (int)(v.second->y_ * winH);
		const int rectW = (int)(v.second->width_ * winW);
		const int rectH = (int)(v.second->height_ * winH);

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
		DrawVoxelWorld(scene_id, scene, cameraFrustum);
		DrawOpaqueMeshes(scene_id, scene->meshes, scene->debug_view, cameraFrustum, false);
		if (!wireframeDebug)
		{
			DrawSprites(scene->meshes, cameraFrustum);
			DrawVFX(scene);
		}

		// Resolve once so displacement samples a stable scene-color snapshot.
		ResolveSceneMSAA(gpu_scene, rectX, rectY, rectW, rectH);

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

		// If displacement was rendered into the MSAA target, resolve it now so the
		// post-process/fog stages see the final displaced scene.
		if (displacementRendered)
			ResolveSceneMSAA(gpu_scene, rectX, rectY, rectW, rectH);

		// Debug primitives stay crisp and are rendered over the distortion.
		// Elles sont dessinees dans le FBO resolu de la scene (et non dans le
		// buffer MSAA) : avant, avec MSAA, elles etaient tracees apres la
		// resolution et n'apparaissaient jamais dans l'image de la scene (elles
		// n'etaient visibles que grace a un second dessin, direct a l'ecran,
		// depuis HRL_EndFrame). Seule la couleur (attachment 0) est ecrite.
		auto debugIt = GetPrivateContext()->debug_renderers.find(scene_id);
		if (debugIt != GetPrivateContext()->debug_renderers.end())
		{
			glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			glViewport(rectX, rectY, rectW, rectH);
			GL33_DrawDebug(debugIt->second, GetPrivateContext()->debug_line_thickness);
			const GLenum attachments[5] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4};
			glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
			glDrawBuffers(5, attachments);
		}

		bool has_post_process = !wireframeDebug && !v.second->post_processes.empty();
		bool has_scene_effects = !wireframeDebug && (HasActiveVolumetricFog(scene) || scene->god_rays.enabled);
		bool has_volumetric_clouds = !wireframeDebug && scene->volumetric_cloud_enabled;
		bool has_ambient_occlusion = !wireframeDebug && scene->ambient_occlusion_enabled;
		bool has_ssr = !wireframeDebug && scene->screen_space_reflections_enabled;
		bool has_decals = !wireframeDebug && !scene->decals.empty();
		bool has_gi = g_gl33_gi && scene->global_illumination_enabled && scene->debug_view == HRL_DEBUG_VIEW_NONE;

		if (has_post_process || has_scene_effects || has_volumetric_clouds || has_ambient_occlusion || has_ssr || has_decals || has_gi)
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
			if (has_gi && gpu_scene->depth_texture)
			{
				// Indirect diffuse light, added before the effects that darken or
				// fog the image (AO, volumetric fog, clouds).
				GL_33_GI::View giView;
				giView.sceneId = scene_id;
				giView.scene = scene;
				giView.viewportId = v.first;
				giView.sceneWidth = gpu_scene->width;
				giView.sceneHeight = gpu_scene->height;
				giView.depth = gpu_scene->depth_texture;
				giView.normal = gpu_scene->textures[4];
				giView.albedo = gpu_scene->textures[3];
				giView.sceneColor = bck_->post_textures[src];
				giView.outputFbo = bck_->post_fbo[1 - src];
				giView.view = ctx_->view_mat;
				giView.projection = ctx_->proj_mat;
				giView.cameraPosition = v.second->camera_->position_;
				giView.x = vx; giView.y = vy; giView.width = vw; giView.height = vh;
				if (g_gl33_gi->Apply(giView))
				{
					currentTexture = bck_->post_textures[1 - src];
					src = 1 - src;
				}
			}
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

			// Le resultat post-traite revient TOUJOURS dans le FBO de la scene
			// (couleur finale = attachment 0), qu'elle soit affichee ou non, et
			// seulement sur le rectangle de ce viewport pour ne pas ecraser les
			// viewports precedents.
			glBindFramebuffer(GL_READ_FRAMEBUFFER, bck_->post_fbo[src]);
			glReadBuffer(GL_COLOR_ATTACHMENT0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene_fbo);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			glBlitFramebuffer(rectX, rectY, rectX + rectW, rectY + rectH,
				rectX, rectY, rectX + rectW, rectY + rectH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}

		// Overlays (gizmos, infos de debug, messages, widgets) : dessines dans
		// l'image finale de la scene, uniquement dans sa couleur (attachment 0)
		// pour ne pas polluer les buffers de picking / albedo / normales.
		// Avant, ils allaient toujours sur l'ecran (FBO 0), meme pour une scene
		// creee avec HRL_CreateScene(false).
		glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
		glDrawBuffer(GL_COLOR_ATTACHMENT0);
		bck_->overlay_fbo = scene_fbo;
		DrawGizmos(scene, v.second, v.first);
		if (scene->debug_view == HRL_DEBUG_VIEW_MESH_INFO)
			DrawMeshDebugInfoTexts(scene, v.second);
		DrawScreenMessages(scene, v.second);
		glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
		DrawWidgets(v.second->widgets, v.second);

		// Retablit les 5 sorties du FBO de scene pour le rendu suivant. (Avant,
		// le glDrawBuffer(GL_COLOR_ATTACHMENT0) du post-process restait actif sur
		// ce FBO : sans MSAA, les frames suivantes n'ecrivaient plus le picking,
		// l'albedo ni les normales.)
		{
			const GLenum attachments[5] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4};
			glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
			glDrawBuffers(5, attachments);
		}
	}
	bck_->overlay_fbo = 0;

	// Image finale complete (tous les viewports + overlays) : copiee une seule
	// fois a l'ecran, et seulement si la scene est affichee.
	if (scene->draw_on_screen)
	{
		const int winW = (int)GetWindowWidth();
		const int winH = (int)GetWindowHeight();
		glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glBlitFramebuffer(0, 0, winW, winH, 0, 0, winW, winH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
	ApplyVoxelLightFieldUniforms(s);

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
		// Cles construites une seule fois : plus d'allocation de std::string par
		// texture et par changement de materiau.
		static const std::string kTextureKeys[6] = {
			tex_uniform_name[0], tex_uniform_name[1], tex_uniform_name[2],
			tex_uniform_name[3], tex_uniform_name[4], tex_uniform_name[5]
		};
		static const std::string kAmbientOcclusionKey = HRL_T_AMBIENT_OCCLUSION;
		for (int i = 0; i < 6; ++i)
		{
			s->SetInt(tex_uniform_name[i], i);
			auto itParam = mat->textureParams_.find(kTextureKeys[i]);
			if (itParam == mat->textureParams_.end()) { ApplyFallback(i); continue; }
			auto itTexture = bck_->textures.find(itParam->second);
			if (itTexture == bck_->textures.end()) { ApplyFallback(i); continue; }
			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(GL_TEXTURE_2D, itTexture->second->GetGL_ID());
		}
		s->SetInt(HRL_T_AMBIENT_OCCLUSION, AO_TEXTURE_UNIT);
		glActiveTexture(GL_TEXTURE0 + AO_TEXTURE_UNIT);
		auto aoParam = mat->textureParams_.find(kAmbientOcclusionKey);
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

	// Pas de glGet* ici : ces requetes peuvent forcer une synchronisation du
	// driver. Les passes 3D fixent explicitement l'etat dont elles ont besoin.
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
    if (!material->ss_displacement_enabled_)
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
            const int lod = SelectMeshLOD(mesh, model, ctx_->view_mat, ctx_->proj_mat, static_cast<float>(GetWindowHeight()), ctx_->viewport);
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

	// Etat fixe explicitement (plus de glIsEnabled/glGetIntegerv, qui peuvent
	// forcer une synchronisation CPU/GPU dans le driver).
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
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
	bool cullEnabled = !wireframe;

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
			lodLevel = SelectMeshLOD(mesh, model, ctx_->view_mat, ctx_->proj_mat, (float)GetWindowHeight(), ctx_->viewport);
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
		// N'emettre glEnable/glDisable que lorsque l'etat change reellement.
		const bool cull = !wireframe && !MaterialIsTwoSided(item.material);
		if (cull != cullEnabled)
		{
			if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
			cullEnabled = cull;
		}
		if (!BindMaterial(item.material, item.id, item.mesh, item.model))
			continue;

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
	RestoreOpaqueStageState(wireframe);
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

// Texture units a sprite material can sample from (see BindMaterial).
static void BindSpriteSamplers(GLuint sampler)
{
	for (int unit = 0; unit < 6; ++unit)
		glBindSampler((GLuint)unit, sampler);
	glBindSampler((GLuint)AO_TEXTURE_UNIT, sampler);
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

	// Sprites are transparent, so they must not write depth, but they MUST
	// test against the depth written by opaque geometry (including voxels).
	// Previously depth testing was disabled here, which made every sprite
	// appear in front of the voxel world regardless of its position.
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	ctx_->bound_material = nullptr;
	ctx_->bound_shader = nullptr;

	// All sprites use NEAREST filtering (sampler objects override the filter
	// stored in the texture without modifying textures shared with 3D meshes).
	if (bck_->nearest_sampler == 0)
	{
		glGenSamplers(1, &bck_->nearest_sampler);
		glSamplerParameteri(bck_->nearest_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glSamplerParameteri(bck_->nearest_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glSamplerParameteri(bck_->nearest_sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glSamplerParameteri(bck_->nearest_sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}
	BindSpriteSamplers(bck_->nearest_sampler);

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

	// Restore the textures' own filtering for everything drawn after sprites.
	BindSpriteSamplers(0);

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}


struct VFXRenderInstance
{
	glm::mat4 model{1.f};
	glm::vec4 color{1.f};
};





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

	// Tampons persistants : plus d'allocation par frame ni de double copie des
	// instances (on trie des indices, puis on remplit le lot dans l'ordre).
	static std::vector<Batch> batches;
	size_t batchCount = 0;
	static std::vector<VFXRenderInstance> unsorted;
	static std::vector<std::pair<float, uint32_t>> order;
	const glm::mat4 inverseView = glm::inverse(ctx_->view_mat);
	const glm::mat4 billboardBasis = glm::mat4(glm::mat3(inverseView));
	const glm::vec3 cameraPos = ctx_->viewport->camera_->position_;

	for (const auto& [sid, system] : scene->vfx_systems)
	{
		(void)sid;
		if (!system || !system->enabled_) continue;
		const glm::mat4 systemMatrix = MakeVFXSystemMatrix(system);
		const bool systemIsIdentity = systemMatrix == glm::mat4(1.f);
		for (const auto& [eid, emitter] : system->emitters_)
		{
			(void)eid;
			if (!emitter || !emitter->enabled_ || emitter->render_mode_ == HRL_VFX_RENDER_MESH || emitter->particles_.empty()) continue;
			const glm::mat4* particleSystemMatrix =
				(emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL && !systemIsIdentity) ? &systemMatrix : nullptr;
			const bool stretched = emitter->render_mode_ == HRL_VFX_RENDER_STRETCHED_BILLBOARD;

			if (batchCount == batches.size())
				batches.emplace_back();
			Batch& batch = batches[batchCount];
			batch.blend = emitter->blend_mode_;
			batch.texture = ResolveVFXTexture(emitter);
			batch.instances.clear();

			if (emitter->blend_mode_ == HRL_VFX_BLEND_ALPHA)
			{
				unsorted.clear();
				order.clear();
				unsorted.reserve(emitter->particles_.size());
				order.reserve(emitter->particles_.size());
				for (const auto& particle : emitter->particles_)
				{
					VFXRenderInstance inst;
					inst.model = MakeVFXParticleModel(particle, particleSystemMatrix, billboardBasis, stretched, emitter->stretch_);
					inst.color = particle.color;
					const glm::vec3 pos = glm::vec3(inst.model[3]);
					order.emplace_back(glm::dot(pos - cameraPos, pos - cameraPos), static_cast<uint32_t>(unsorted.size()));
					unsorted.push_back(inst);
				}
				std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
				batch.instances.reserve(order.size());
				for (const auto& entry : order) batch.instances.push_back(unsorted[entry.second]);
			}
			else
			{
				// Additif / multiplicatif : l'ordre n'a pas d'importance, pas de tri.
				batch.instances.reserve(emitter->particles_.size());
				for (const auto& particle : emitter->particles_)
				{
					VFXRenderInstance inst;
					inst.model = MakeVFXParticleModel(particle, particleSystemMatrix, billboardBasis, stretched, emitter->stretch_);
					inst.color = particle.color;
					batch.instances.push_back(inst);
				}
			}
			if (!batch.instances.empty()) ++batchCount;
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

	for (size_t batchIndex = 0; batchIndex < batchCount; ++batchIndex)
	{
		const Batch& batch = batches[batchIndex];
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
				ctx_->view_mat, ctx_->proj_mat, (float)GetWindowHeight(), ctx_->viewport);
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

			static std::vector<std::pair<float, const HRL_VFXParticle*>> ordered;
			ordered.clear();
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

			// Texture de surcharge resolue une fois par emetteur.
			GLuint overrideTexture = 0;
			if (emitter->texture_ != HRL_INVALID_ID && material->shader_ == HRL_MESH_3D_SHADER)
			{
				auto texIt = bck_->textures.find(emitter->texture_);
				if (texIt != bck_->textures.end() && texIt->second)
					overrideTexture = texIt->second->GetGL_ID();
			}
			// Le materiau est le meme pour toutes les particules : apres le premier
			// BindMaterial, seuls les uniforms par objet changent (les valeurs
			// identiques sont filtrees par GL33_Shader). Textures et VAO ne sont
			// lies qu'une fois.
			bool emitterStateBound = false;
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

				if (!emitterStateBound)
				{
					if (overrideTexture != 0)
					{
						glActiveTexture(GL_TEXTURE0);
						glBindTexture(GL_TEXTURE_2D, overrideTexture);
					}
					glBindVertexArray(gpu->vao);
					emitterStateBound = true;
				}
				if (gpu->indexed && gpu->index_count > 0) glDrawElements(GL_TRIANGLES, gpu->index_count, GL_UNSIGNED_INT, nullptr);
				else glDrawArrays(GL_TRIANGLES, 0, gpu->vertex_count);
			}
			// La texture de surcharge a remplace celle du materiau sur l'unite 0 :
			// forcer un rebind complet si un autre emetteur reutilise ce materiau.
			if (overrideTexture != 0)
				ctx_->bound_material = nullptr;
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











static void DrawGizmos(const hrl_scene_t* scene, const HRL_Viewport* viewport, HRL_id viewport_id)
{
	DebugRenderer overlay;
	if (!BuildGizmoOverlay(scene, viewport, viewport_id, ctx_->view_mat, overlay))
		return;
	const float winW = static_cast<float>(GetWindowWidth());
	const float winH = static_cast<float>(GetWindowHeight());
	glViewport(
		static_cast<GLint>(viewport->x_ * winW),
		static_cast<GLint>(viewport->y_ * winH),
		std::max(1, static_cast<int>(viewport->width_ * winW)),
		std::max(1, static_cast<int>(viewport->height_ * winH)));
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

	// Dessine dans l'image finale de la scene (et non sur l'ecran).
	glBindFramebuffer(GL_FRAMEBUFFER, bck_->overlay_fbo);
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

static void DrawScreenMessages(const hrl_scene_t* scene, const HRL_Viewport* viewport)
{
	if (!scene || !viewport || !bck_ || !bck_->ui_shader || !ctx_)
		return;

	HRL_Context* privateContext = GetPrivateContext();
	if (!privateContext || privateContext->fonts.empty())
		return;

	const float winW = static_cast<float>(GetWindowWidth());
	const float winH = static_cast<float>(GetWindowHeight());
	const int viewportX = static_cast<int>(viewport->x_ * winW);
	const int viewportY = static_cast<int>(viewport->y_ * winH);
	const int viewportW = std::max(1, static_cast<int>(viewport->width_ * winW));
	const int viewportH = std::max(1, static_cast<int>(viewport->height_ * winH));

	// Dessine dans l'image finale de la scene (et non sur l'ecran).
	glBindFramebuffer(GL_FRAMEBUFFER, bck_->overlay_fbo);
	glViewport(viewportX, viewportY, viewportW, viewportH);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	bck_->ui_shader->Use();
	bck_->ui_shader->SetMat4("projection", glm::ortho(0.f, 1.f, 1.f, 0.f, -1.f, 1.f));
	bck_->ui_shader->SetInt("uSDFText", 1);
	bck_->ui_shader->SetInt("uTexture", 0);

	// Unreal-style stack: newest messages are appended below older ones.
	// All positions are screen-pixel based and therefore independent of the
	// viewport's resolution.
	float cursorY = 16.0f;
	const float leftMargin = 16.0f;
	const float lineGap = 4.0f;

	std::unordered_set<HRL_id> activeIds;
	activeIds.reserve(scene->screen_messages.size());

	for (const HRL_ScreenMessage& message : scene->screen_messages)
	{
		activeIds.insert(message.id);

		HRL_id fontId = message.font;
		if (fontId == HRL_INVALID_ID || privateContext->fonts.find(fontId) == privateContext->fonts.end())
			fontId = scene->debug_mesh_info_font;
		if (fontId == HRL_INVALID_ID || privateContext->fonts.find(fontId) == privateContext->fonts.end())
			fontId = privateContext->fonts.begin()->first;

		auto& cache = bck_->screen_message_textures[message.id];
		if (cache.texture == HRL_INVALID_ID ||
			cache.font != fontId ||
			cache.size != message.size ||
			cache.text != message.text ||
			!HRL_IsValidTexture(cache.texture))
		{
			if (cache.texture != HRL_INVALID_ID && HRL_IsValidTexture(cache.texture))
				HRL_DeleteTexture(cache.texture);

			cache = {};
			cache.font = fontId;
			cache.size = message.size;
			cache.text = message.text;
			cache.texture = HRL_InternalCreateSDFTextTexture(
				message.text.c_str(), fontId, message.size);
			if (cache.texture == HRL_INVALID_ID)
				continue;

			HRL_SetTextureMinFilter(cache.texture, HRL_FILTER_LINEAR);
			HRL_SetTextureMagFilter(cache.texture, HRL_FILTER_LINEAR);
			HRL_GetTextureSize(cache.texture, &cache.width, &cache.height);
		}

		if (cache.texture == HRL_INVALID_ID || cache.width <= 0 || cache.height <= 0)
			continue;

		const float lineHeight = std::max(1.0f, message.size);
		const float lineCount = 1.0f + static_cast<float>(std::count(message.text.begin(), message.text.end(), '\n'));
		const float pixelHeight = lineHeight * lineCount;
		const float bitmapAspect = static_cast<float>(cache.width) / static_cast<float>(cache.height);
		const float pixelWidth = pixelHeight * bitmapAspect;

		const float x = leftMargin / static_cast<float>(viewportW);
		const float y = cursorY / static_cast<float>(viewportH);
		const float w = pixelWidth / static_cast<float>(viewportW);
		const float h = pixelHeight / static_cast<float>(viewportH);

		// A small dark translucent shadow makes messages readable over the scene,
		// while preserving the exact configured message color.
		bck_->ui_shader->SetVec4("uTintColor", glm::vec4(0.f, 0.f, 0.f, message.color.a * 0.75f));
		glActiveTexture(GL_TEXTURE0);
		auto texIt = bck_->textures.find(cache.texture);
		if (texIt == bck_->textures.end() || !texIt->second)
			continue;
		glBindTexture(GL_TEXTURE_2D, texIt->second->GetGL_ID());

		const float shadowOffsetX = 1.5f / static_cast<float>(viewportW);
		const float shadowOffsetY = 1.5f / static_cast<float>(viewportH);
		const float shadowVertices[16] = {
			x + shadowOffsetX, y + shadowOffsetY, 0.0f, 1.0f,
			x + w + shadowOffsetX, y + shadowOffsetY, 1.0f, 1.0f,
			x + w + shadowOffsetX, y + h + shadowOffsetY, 1.0f, 0.0f,
			x + shadowOffsetX, y + h + shadowOffsetY, 0.0f, 0.0f,
		};
		glBindVertexArray(bck_->vao[BUFFER_UI]);
		glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_UI]);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(shadowVertices), shadowVertices);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

		bck_->ui_shader->SetVec4("uTintColor", message.color);
		const float vertices[16] = {
			x, y, 0.0f, 1.0f,
			x + w, y, 1.0f, 1.0f,
			x + w, y + h, 1.0f, 0.0f,
			x, y + h, 0.0f, 0.0f,
		};
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

		cursorY += pixelHeight + lineGap;
	}

	for (auto it = bck_->screen_message_textures.begin(); it != bck_->screen_message_textures.end(); )
	{
		if (activeIds.find(it->first) == activeIds.end())
		{
			if (it->second.texture != HRL_INVALID_ID && HRL_IsValidTexture(it->second.texture))
				HRL_DeleteTexture(it->second.texture);
			it = bck_->screen_message_textures.erase(it);
		}
		else
			++it;
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

			// No texture (progress bar, slider, checkbox, plain colors) : a
			// white texel, so the color is drawn as is. (The albedo fallback
			// is a grey grid : it made these widgets look broken.)
			GLuint gl_texture = 0;
			auto texture_it = bck_->textures.find(g.texture);
			if (texture_it != bck_->textures.end())
				gl_texture = texture_it->second->GetGL_ID();
			else
			{
				if (bck_->ui_white_texture == 0)
				{
					const unsigned char white[4] = {255, 255, 255, 255};
					glGenTextures(1, &bck_->ui_white_texture);
					glBindTexture(GL_TEXTURE_2D, bck_->ui_white_texture);
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				}
				gl_texture = bck_->ui_white_texture;
			}

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, gl_texture);
			bck_->ui_shader->SetInt("uTexture", 0);

			// UV : top of the quad = v 1 (texture convention of the UI).
			// u0/t0/u1/t1 crop the image (text clipped by its widget).
			const float vt = 1.0f - g.t0;
			const float vb = 1.0f - g.t1;
			const float vertices[16] = {
				g.px,        g.py,        g.u0, vt,
				g.px+g.sx,   g.py,        g.u1, vt,
				g.px+g.sx,   g.py+g.sy,   g.u1, vb,
				g.px,        g.py+g.sy,   g.u0, vb,
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
    const float cloudTime = HRL_InternalRenderTimeSeconds();
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
	const double postTimeSeconds = HRL_InternalRenderTimeSeconds();
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
	// Volontairement vide pour OpenGL : GL33_DrawScene -> UploadSceneLights()
	// reconstruit et envoie l'UBO complet de la scene (lumieres + matrices
	// d'ombre + emetteurs voxel) juste avant le rendu. L'ancien envoi fait ici
	// (appele par HRL_EndFrame et par chaque setter de lumiere) etait
	// systematiquement ecrase : ~40 Ko envoyes pour rien a chaque appel.
	(void)_lights;
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

// Un caster d'ombre prepare une seule fois par scene (matrice modele, bornes,
// GPU, flag double face) au lieu d'etre recalcule pour chaque lumiere et
// chaque face de cube map.
struct ShadowCaster
{
	HRL_id id = HRL_INVALID_ID;
	HRL_Mesh* mesh = nullptr;
	glm::mat4 model{1.f};
	glm::vec3 center{0.f};
	float radius = 0.f;
	bool hasBounds = false;   // bounds_radius_ > 0 (sinon toujours considere visible)
	bool twoSided = false;
	const GL33_Backend::MeshGPU* staticGpu = nullptr;
	GL33_Backend::SkeletalMeshGPU* skeletalGpu = nullptr;
};

static void HashBytes(uint64_t& hash, const void* data, size_t size)
{
	const unsigned char* bytes = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < size; ++i)
	{
		hash ^= static_cast<uint64_t>(bytes[i]);
		hash *= 1099511628211ull;
	}
}

template <typename T>
static void HashValue(uint64_t& hash, const T& value)
{
	HashBytes(hash, &value, sizeof(T));
}

static void CollectShadowCasters(hrl_scene_t* scene, std::vector<ShadowCaster>& out)
{
	out.clear();
	HRL_Context* context = GetPrivateContext();
	for (const auto& [id, mesh] : scene->meshes)
	{
		if (!mesh || mesh->type_ == HRL_SPRITE)
			continue;

		ShadowCaster caster;
		caster.id = id;
		caster.mesh = mesh;
		if (mesh->type_ == HRL_3D_SKELETAL_MESH)
		{
			auto it = bck_->skeletal_meshes.find(id);
			if (it == bck_->skeletal_meshes.end() || it->second.vao == 0)
				continue;
			caster.skeletalGpu = &it->second;
			UploadSkeletalBones(static_cast<HRL_SkeletalMesh*>(mesh), *caster.skeletalGpu);
		}
		else
		{
			auto it = bck_->meshes.find(id);
			if (it == bck_->meshes.end())
				continue;
			caster.staticGpu = &it->second;
		}

		caster.model = CalculateModelMatrix(mesh);
		caster.hasBounds = mesh->bounds_radius_ > 0.f;
		caster.center = glm::vec3(caster.model * glm::vec4(mesh->bounds_center_, 1.f));
		const float maxScale = std::max({std::abs(mesh->scale_.x), std::abs(mesh->scale_.y), std::abs(mesh->scale_.z)});
		caster.radius = mesh->bounds_radius_ * std::max(maxScale, 1e-6f);
		const auto materialIt = context->materials.find(mesh->material_);
		caster.twoSided = materialIt != context->materials.end() && MaterialIsTwoSided(materialIt->second);
		out.push_back(caster);
	}
}

// Filtre les casters qui peuvent affecter une lumiere (meme regles que
// l'ancien code : portee pour une point light, frustum pour les autres ;
// les meshes skinnes ne sont jamais rejetes car leurs bornes ne suivent pas
// l'animation).
static void SelectLightCasters(const std::vector<ShadowCaster>& all, bool pointLight,
	const glm::vec3& lightPosition, float farPlane, const FrustumPlaneSet& frustum,
	std::vector<const ShadowCaster*>& out)
{
	out.clear();
	for (const ShadowCaster& caster : all)
	{
		if (caster.skeletalGpu)
		{
			out.push_back(&caster);
			continue;
		}
		if (pointLight)
		{
			const float limit = farPlane + caster.radius;
			const glm::vec3 d = caster.center - lightPosition;
			if (glm::dot(d, d) > limit * limit)
				continue;
		}
		else if (caster.hasBounds && !SphereInsideFrustum(frustum, caster.center, caster.radius))
			continue;
		out.push_back(&caster);
	}
}

static uint64_t ComputeShadowSignature(const hrl_scene_t* scene, const HRL_Light* light,
	const GL33_Backend::ShadowGPU& shadow, const std::vector<const ShadowCaster*>& casters)
{
	uint64_t hash = 1469598103934665603ull;
	HashValue(hash, light->type_);
	HashValue(hash, light->position_);
	HashValue(hash, light->rotation_);
	HashValue(hash, light->outerCutoff);
	HashValue(hash, shadow.resolution);
	HashValue(hash, shadow.fbo);
	HashValue(hash, casters.size());
	for (const ShadowCaster* caster : casters)
	{
		HashValue(hash, caster->id);
		HashValue(hash, caster->model);
		HashValue(hash, caster->twoSided);
		if (caster->skeletalGpu)
		{
			HashValue(hash, caster->skeletalGpu->upload_serial);
			HashValue(hash, static_cast<const HRL_SkeletalMesh*>(caster->mesh)->pose_serial_);
		}
		else if (caster->staticGpu)
		{
			HashValue(hash, caster->staticGpu->levels.size());
			for (const auto& level : caster->staticGpu->levels)
				HashValue(hash, level.upload_serial);
			// Reglages qui influencent le choix du LOD dans la shadow map.
			HashValue(hash, caster->mesh->lod_automatic_);
			HashValue(hash, caster->mesh->lod_override_);
			HashValue(hash, caster->mesh->lod_mode_);
			HashValue(hash, caster->mesh->lod_screen_threshold_);
			HashValue(hash, caster->mesh->lod_base_distance_);
		}
	}
	// Les landscapes sont peu nombreux : on les inclut tous.
	for (const auto& [id, landscape] : scene->landscapes)
	{
		if (!landscape)
			continue;
		HashValue(hash, id);
		HashValue(hash, landscape->revision_);
		HashValue(hash, landscape->position_);
		HashValue(hash, landscape->rotation_);
		HashValue(hash, landscape->scale_);
		HashValue(hash, landscape->material_);
		auto gpuIt = bck_->landscapes.find(id);
		if (gpuIt != bck_->landscapes.end())
			HashValue(hash, gpuIt->second.geometry.upload_serial);
		auto matIt = GetPrivateContext()->materials.find(landscape->material_);
		const bool twoSided = matIt != GetPrivateContext()->materials.end() && MaterialIsTwoSided(matIt->second);
		HashValue(hash, twoSided);
	}
	return hash;
}

static void RenderShadowCasters(hrl_scene_t* scene, const std::vector<const ShadowCaster*>& casters,
	const glm::mat4& lightViewProjection, const glm::mat4& lightView, const glm::mat4& lightProjection, int shadowResolution,
	GL33_Shader* shader, bool pointLight, const glm::vec3& lightPosition,
	float farPlane, int face)
{
	if (!scene || !shader)
		return;

	GL33_Shader* activeShader = nullptr;
	const FrustumPlaneSet frustum = BuildFrustum(lightViewProjection);
	// PrepareSceneShadows entre ici avec GL_CULL_FACE actif et glCullFace(GL_FRONT).
	bool cullEnabled = true;
	for (const ShadowCaster* caster : casters)
	{
		const bool skeletal = caster->skeletalGpu != nullptr;
		const GL33_Backend::MeshLOD_GPU* staticGpu = nullptr;
		if (!skeletal)
		{
			// Culling par face de cube map (ou frustum de la lumiere). Les
			// meshes sans bornes restent toujours dessines, comme avant.
			if (caster->hasBounds && !SphereInsideFrustum(frustum, caster->center, caster->radius))
				continue;
			const int lodLevel = SelectMeshLOD(caster->mesh, caster->model, lightView, lightProjection, (float)shadowResolution, ctx_->viewport);
			staticGpu = GetMeshLOD_GPU(*caster->staticGpu, lodLevel);
			if (!staticGpu)
				continue;
		}

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
		meshShader->SetMat4("model", caster->model);
		const bool cull = !caster->twoSided;
		if (cull != cullEnabled)
		{
			if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
			cullEnabled = cull;
		}

		if (skeletal)
		{
			glBindBufferBase(GL_UNIFORM_BUFFER, 1, caster->skeletalGpu->bone_ubo);
			glBindVertexArray(caster->skeletalGpu->vao);
			if (caster->skeletalGpu->indexed)
				glDrawElements(GL_TRIANGLES, caster->skeletalGpu->index_count, GL_UNSIGNED_INT, nullptr);
			else
				glDrawArrays(GL_TRIANGLES, 0, caster->skeletalGpu->vertex_count);
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
	// DrawLandscapeShadowCasters suppose le meme etat d'entree.
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT);
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

	// Casters prepares une fois pour toutes les lumieres de la scene.
	static std::vector<ShadowCaster> allCasters;
	static std::vector<const ShadowCaster*> lightCasters;
	CollectShadowCasters(scene, allCasters);

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
		const bool isPoint = light->type_ == HRL_POINT_LIGHT;

		// Matrices de la lumiere (identiques a l'ancien code).
		constexpr float pointNearPlane = 0.05f;
		constexpr float pointFarPlane = 100.f;
		glm::mat4 lightView(1.f);
		glm::mat4 projection(1.f);
		if (isPoint)
		{
			projection = glm::perspective(glm::radians(90.f), 1.f, pointNearPlane, pointFarPlane);
		}
		else
		{
			const glm::vec3 lightDir = GetLightDirection(light);
			const glm::vec3 center = (light->type_ == HRL_SPOT_LIGHT) ? light->position_ : glm::vec3(0.f);
			glm::vec3 up = std::abs(glm::dot(lightDir, glm::vec3(0,1,0))) > 0.98f
				? glm::vec3(0,0,1) : glm::vec3(0,1,0);
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
		}
		const glm::mat4 shadowClip = projection * lightView;

		SelectLightCasters(allCasters, isPoint, light->position_, pointFarPlane,
			BuildFrustum(shadowClip), lightCasters);

		// Le contenu de cette shadow map est-il deja a jour ? Deplacer un objet
		// hors de la zone d'influence d'une lumiere ne la re-rend plus.
		const uint64_t signature = ComputeShadowSignature(scene, light, shadowIt->second, lightCasters);
		const bool upToDate = shadowIt->second.content_valid && shadowIt->second.content_signature == signature;

		if (!upToDate)
		{
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
			if (isPoint)
				glDisable(GL_POLYGON_OFFSET_FILL);
			else
			{
				glEnable(GL_POLYGON_OFFSET_FILL);
				glPolygonOffset(2.0f, 4.0f);
			}

			if (isPoint)
			{
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
					RenderShadowCasters(scene, lightCasters, projection * view, view, projection, resolution,
						bck_->shadow_point_shader, true, pos, pointFarPlane, face);
				}
			}
			else
			{
				glClear(GL_DEPTH_BUFFER_BIT);
				// Shadow rasterization needs clip-space coordinates (-1..1). The 0..1
				// bias transform belongs only to the sampling matrix used by the lit shader.
				RenderShadowCasters(scene, lightCasters, shadowClip, lightView, projection, resolution,
					bck_->shadow_2d_shader, false, light->position_, 0.f, -1);
			}

			glDisable(GL_POLYGON_OFFSET_FILL);
			glCullFace(GL_BACK);
			shadowIt->second.content_signature = signature;
			shadowIt->second.content_valid = true;
		}
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
	auto& voxelCache = bck_->voxel_light_cache[scene_id];
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

	// Reserve the original HRL light budget and use the remaining slots for
	// HDR voxel emitters. Both use exactly the same global LightBlock.
	constexpr int MAX_HRL_SCENE_LIGHTS = 32;
	int count = 0;
	for (const auto& [id, light] : scene->lights)
	{
		if (!light || count >= MAX_HRL_SCENE_LIGHTS || count >= MAX_LIGHTS)
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
					dst.shadowParams = glm::vec4(light->shadow_bias_, farPlane, (float)slot, 2.f);
				}
				else
				{
					const glm::vec3 lightDir = GetLightDirection(light);
					const glm::vec3 center = (light->type_ == HRL_SPOT_LIGHT) ? light->position_ : glm::vec3(0.f);
					const glm::vec3 up = std::abs(glm::dot(lightDir, glm::vec3(0,1,0))) > 0.98f
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

	voxelCache.scene_light_count = count;
	for (int i = 0; i < count && i < 32; ++i)
		voxelCache.scene_lights[static_cast<size_t>(i)] = gpu_lights[i];

	voxelCache.scene_shadow_light_count = 0;
	for (int i = 0; i < count; ++i)
		if (gpu_lights[i].shadowParams.z >= -0.5f)
			++voxelCache.scene_shadow_light_count;

	// Emissive voxels are no longer promoted to point lights (that capped them
	// at 256 and made every voxel fragment loop over them) : they light the
	// scene through the per-viewport light field (BuildVoxelLightField).
	voxelCache.lights.clear();
	voxelCache.chunk_emitters.clear();
	if (HRL_VoxelWorld* world = scene->voxel_world)
		world->light_dirty_chunks_.clear(); // filled by the voxel edits, unused here
	voxelCache.world = scene->voxel_world;
	voxelCache.valid = true;

	// A stable signature lets each chunk keep its own UBO untouched across frames.
	// Hashing at most 32 scene lights + 256 voxel emitters is tiny compared with
	// rewriting an OpenGL buffer for every visible chunk.
	const auto hashLights = [](uint64_t seed, const GL_Light* data, size_t count)
	{
		const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
		const size_t byteCount = count * sizeof(GL_Light);
		uint64_t hash = seed;
		for (size_t i = 0; i < byteCount; ++i)
		{
			hash ^= static_cast<uint64_t>(bytes[i]);
			hash *= 1099511628211ull;
		}
		return hash;
	};

	uint64_t signature = 1469598103934665603ull;
	signature = hashLights(signature, voxelCache.scene_lights.data(), static_cast<size_t>(voxelCache.scene_light_count));
	if (!voxelCache.lights.empty())
		signature = hashLights(signature, voxelCache.lights.data(), voxelCache.lights.size());
	if (signature != voxelCache.signature)
	{
		voxelCache.signature = signature;
		++voxelCache.revision;
		if (voxelCache.revision == 0)
			voxelCache.revision = 1;
	}

	// La signature couvre exactement le contenu envoye (lumieres de la scene
	// avec leurs donnees d'ombre + emetteurs voxel). Si l'UBO contient deja ces
	// donnees pour cette scene, on evite de le reecrire.
	if (bck_->light_ubo_valid && bck_->light_ubo_scene == scene_id &&
		bck_->light_ubo_signature == voxelCache.signature)
		return;

	for (const GL_Light& light : voxelCache.lights)
	{
		if (count >= MAX_LIGHTS)
			break;
		gpu_lights[count++] = light;
	}

	glBindBuffer(GL_UNIFORM_BUFFER, bck_->ubo[UBO_LIGHTS]);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)sizeof(gpu_lights), gpu_lights);
	bck_->light_ubo_scene = scene_id;
	bck_->light_ubo_signature = voxelCache.signature;
	bck_->light_ubo_valid = true;
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

// Resout uniquement le rectangle du viewport : resoudre tout le buffer
// ecrasait l'image finale (post-process, overlays) des viewports precedents.
static void ResolveSceneMSAA(GL_Scene* scene, int x, int y, int width, int height)
{
	if (!scene || scene->msaa_samples <= 1 || !scene->msaa_fbo)
		return;
	const int x0 = std::clamp(x, 0, scene->width);
	const int y0 = std::clamp(y, 0, scene->height);
	const int x1 = std::clamp(x + width, 0, scene->width);
	const int y1 = std::clamp(y + height, 0, scene->height);
	if (x1 <= x0 || y1 <= y0)
		return;
	for (int i = 0; i < 5; ++i)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->msaa_fbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->fbo);
		glDrawBuffer(GL_COLOR_ATTACHMENT0 + i);
		glBlitFramebuffer(x0, y0, x1, y1, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->msaa_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->fbo);
	glBlitFramebuffer(x0, y0, x1, y1, x0, y0, x1, y1, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

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

	DestroyAllVoxelChunks(_sceneid);
	DestroyVoxelTextureArray(_sceneid);
	bck_->voxel_light_cache.erase(_sceneid);

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
	gpu.upload_serial = ++bck_->gpu_upload_serial;
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
	HRL_LandscapeHeightmap heightmap;
	heightmap.width = (int)texIt->second->GetWidth();
	heightmap.height = (int)texIt->second->GetHeight();
	heightmap.rgba = &texIt->second->GetCpuRGBA();
	if (!BuildLandscapeGeometry(landscape, &heightmap, vertices, indices, boundsCenter, boundsRadius))
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
	ApplyVoxelLightFieldUniforms(shader);
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
	// Etat de sortie deterministe (voir RestoreOpaqueStageState) au lieu de
	// glGet*/glIsEnabled, qui peuvent bloquer le pipeline du driver.

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
	bool cullEnabled = !wireframe;

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

		const bool cull = !wireframe && !MaterialIsTwoSided(material);
		if (cull != cullEnabled)
		{
			if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
			cullEnabled = cull;
		}
		glBindVertexArray(gpu.geometry.vao);
		glDrawElements(GL_TRIANGLES, gpu.geometry.index_count, GL_UNSIGNED_INT, nullptr);
	}

	glBindVertexArray(0);
	RestoreOpaqueStageState(wireframe);
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
	gpu.upload_serial = ++bck_->gpu_upload_serial;
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
// Point d'entree RHI_DrawDebug appele par HRL_EndFrame apres le rendu de la
// scene. Pour OpenGL, les primitives de debug sont deja dessinees DANS le
// framebuffer de la scene par GL33_DrawScene (pour chaque viewport). Les
// redessiner ici les envoyait une seconde fois sur l'ecran (FBO 0), meme
// pour une scene non affichee. Meme comportement que le backend Vulkan.
void GL33_DrawDebugAfterScene(const DebugRenderer& _renderer, float line_thickness)
{
	(void)_renderer;
	(void)line_thickness;
}

void GL33_DrawDebug(const DebugRenderer &_renderer, float line_thickness)
{
	if (_renderer.lines.empty() && _renderer.triangles.empty())
		return;

	auto it = bck_->shaders.find(HRL_DEBUG_SHADER);
	if (it == bck_->shaders.end())
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL, "GL33_DrawDebug error: debug shader doesn't exists");
		return;
	}
	auto* s = it->second;

	// Debug primitives are an overlay. They must not be hidden by the scene
	// geometry they are meant to inspect (voxel faces, sprites, etc.).
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	//on set CurrentShader pour spécifier que les prochains calls utiliseront ce shader
	ctx_->shader = s;
	s->Use();

	s->SetMat4("projection", ctx_->proj_mat);
	s->SetMat4("view", ctx_->view_mat);

	//bind vao and initialize opengl evironement
	glBindVertexArray(bck_->vao[BUFFER_DEBUG]);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->vbo[BUFFER_DEBUG]);

	// Keep depth testing disabled for the whole debug pass.
	glLineWidth(std::max(1.0f, line_thickness));

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

	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
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

unsigned int HRL_GL_GetSceneColorPickingBufferGL_ID(HRL_id _sceneid)
{
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetSceneColorPickingBufferGL_ID: scene ID is not valid");
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
	return GL_33_GI::Supports(method) ? HRL_TRUE : HRL_FALSE;
}

uint32_t GL33_GetGlobalIlluminationSupportedMethods()
{
	return GL_33_GI::SupportedMethods();
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


static const std::vector<unsigned char>* GL33_TextureCPUData(HRL_id id, int* width, int* height)
{
	const GL33_Texture* texture = GL33_FindTexture(id);
	if (!texture)
		return nullptr;
	if (width) *width = (int)texture->GetWidth();
	if (height) *height = (int)texture->GetHeight();
	return &texture->GetCpuRGBA();
}

const GL33_Texture* GL33_FindTexture(HRL_id id)
{
    if (!bck_)
        return nullptr;
    auto it = bck_->textures.find(id);
    return it == bck_->textures.end() ? nullptr : it->second;
}
