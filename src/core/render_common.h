// CPU-side rendering helpers shared by the OpenGL 3.3 and Vulkan backends.
#ifndef HRL_RENDER_COMMON_H
#define HRL_RENDER_COMMON_H

#include "../hrl.h"
#include "object_types.h"

#include <glm/glm.hpp>
#include <array>
#include <string>
#include <vector>

struct FrustumPlaneSet
{
	glm::vec4 planes[6];
};

glm::vec4 NormalizePlane(const glm::vec4& p);
FrustumPlaneSet BuildFrustum(const glm::mat4& clip);
bool SphereInsideFrustum(const FrustumPlaneSet& frustum, const glm::vec3& center, float radius);
bool IsMeshVisible(const HRL_Mesh* mesh, const glm::mat4& model, const FrustumPlaneSet& frustum);
bool IsFiniteBounds(const HRL_Mesh* mesh);
glm::mat4 CalculateModelMatrix(const HRL_Mesh* mesh);
// viewport: the camera used for distance-based LODs (may be null).
int SelectMeshLOD(const HRL_Mesh* mesh, const glm::mat4& model,
	const glm::mat4& view, const glm::mat4& projection, float viewportHeight, const HRL_Viewport* viewport);

glm::mat4 MakeVFXSystemMatrix(const HRL_VFXSystem* system);
glm::mat4 MakeVFXParticleModel(const HRL_VFXParticle& p, const glm::mat4* systemMatrix,
	const glm::mat4& billboardBasis, bool stretched, float stretch);

glm::vec3 GizmoAxisRenderer(const HRL_Gizmo* g, int axis);
float GizmoSizeRenderer(const HRL_Gizmo* g, const HRL_Viewport* viewport, const glm::mat4& view);
void GizmoPushLine(DebugRenderer& d, const glm::vec3& a, const glm::vec3& b, const glm::vec4& c);
void GizmoPushArc(DebugRenderer& d, const glm::vec3& center, const glm::vec3& normal,
	const glm::vec3& u, float radius, float start, float sweep, int segments, const glm::vec4& color);
// Builds the line list of every gizmo of a viewport. Returns false when empty.
bool BuildGizmoOverlay(const hrl_scene_t* scene, const HRL_Viewport* viewport, HRL_id viewport_id,
	const glm::mat4& view, DebugRenderer& overlay);

const char* MeshDebugTypeName(const HRL_Mesh* mesh);
glm::vec3 ComputeMeshAverageNormal(const HRL_Mesh* mesh, int lodLevel);
void BuildMeshDebugInfoText(const HRL_Mesh* mesh, std::string& out);

bool WritePNG_RGBA8(const char* path, int width, int height, const std::vector<unsigned char>& rgbaTopDown);

glm::vec3 GetLightDirection(const HRL_Light* light);
constexpr float HRL_POINT_SHADOW_NEAR = 0.05f;
constexpr float HRL_POINT_SHADOW_FAR = 100.f;
// View/projection used to render and sample the shadow map of a light.
void ComputeLightShadowMatrices(const HRL_Light* light, glm::mat4& lightView, glm::mat4& projection);

// CPU heightmap (RGBA8, GL row order) used to build landscape geometry.
struct HRL_LandscapeHeightmap
{
	int width = 0;
	int height = 0;
	const std::vector<unsigned char>* rgba = nullptr;
};
glm::mat4 CalculateLandscapeModelMatrix(const HRL_Landscape* landscape);
float SampleLandscapeHeight(const HRL_LandscapeHeightmap* texture, float u, float v);
bool BuildLandscapeGeometry(const HRL_Landscape* landscape, const HRL_LandscapeHeightmap* heightmap,
	std::vector<HRL_Vertex3D>& vertices, std::vector<HRL_uint>& indices,
	glm::vec3& boundsCenter, float& boundsRadius);

float VoxelEmitterFalloff(float distance, float falloff);
float VoxelEmitterRingEnergy(float a, float b, float falloff);
const std::array<float, 10>& VoxelLightLevelWeights(float falloff, int s, int levels);

// Seconds used by animated shader effects (HRL_DEBUG_FIXED_TIME freezes it).
float HRL_InternalRenderTimeSeconds();

#endif
