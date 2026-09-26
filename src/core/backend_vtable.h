#ifndef HRL_BACKEND_VTABLE
#define HRL_BACKEND_VTABLE

//on peut inclure hrl.h car hrl.h n'inclura jamais ce fichier
#include "../hrl.h"
#include "object_types.h"

#include <vector>

typedef struct {
	//Init-Shutdown//
	void(*RHI_Init)();
	void(*RHI_InitContext)(HRL_uint width, HRL_uint height, void* loader);
	void(*RHI_Shutdown)();

	void(*RHI_WindowResizeCallback)(int width, int height);

	//Draw & batching//
	void(*RHI_BeginFrame)();

	//If draw on screen is 0, it will render on an intermediate framebuffer
	void(*RHI_RenderScene)(hrl_scene_t* scene, HRL_id scene_id);

	//Call at the end of function end frame
	void(*RHI_ResetFramebuffer)();

	//Bind Viewport appeller avant les autres binds et draw car il d�finit la camera!
	void(*RHI_BindViewport)(HRL_Viewport* viewport);
	void(*RHI_ComputeFrameMatrices)();
	void(*RHI_BindMaterial)(HRL_Material* mat);

	//Draw
	void(*RHI_DrawMesh)(HRL_Mesh* mesh);

	//Mesh GPU resources
	int(*RHI_CreateMesh)(HRL_id id, const HRL_Vertex3D* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count);
	int(*RHI_CreateSkeletalMesh)(HRL_id id, const HRL_SkeletalVertex* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count, HRL_uint bone_count);
	int(*RHI_CreateMeshLOD)(HRL_id id, HRL_uint level, const HRL_Vertex3D* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count);
	void(*RHI_DeleteMeshLODs)(HRL_id id);
	int(*RHI_CreateSpriteMesh)(HRL_id id);
	void(*RHI_DeleteMesh)(HRL_id id);

	//Lights//
	void(*RHI_UpdateLights)(const std::vector<HRL_Light*>& lights);
	void(*RHI_DeleteLight)(HRL_id id);

	//Textures//
	HRL_id(*RHI_CreateTexture)(const char* imageContent, const size_t imageSize);
	HRL_id(*RHI_CreateTextureFromBitmap)(BitmapResult bitmapResult);
	void(*RHI_DeleteTexture)(HRL_id id);
	void(*RHI_GetTextureSize)(HRL_id id, int *width, int *height);
	void(*RHI_SetTextureMinFilter)(HRL_id textureid, HRL_EFilterType filter);
	void(*RHI_SetTextureMaxFilter)(HRL_id textureid, HRL_EFilterType filter);

	//Scenes//
	void(*RHI_CreateScene)(HRL_id newSceneId, int renderOnScreen);
	void(*RHI_DeleteScene)(HRL_id sceneid);
	void(*RHI_BindScene)(HRL_id sceneid);
	void(*RHI_ClearScene)();
	void(*RHI_ResizeSceneTexture)(HRL_id sceneid, int width, int height);
	void(*RHI_EnableColorPickingBuffer)(HRL_id sceneid, int enable);
	void(*RHI_SetAntialiasingMode)(int samples);

	//Shaders//
	HRL_id(*RHI_CreateShader)(const char* vertContent, size_t vertSize, const char* fragContent, size_t fragSize);
	void(*RHI_DeleteShader)(HRL_id id);

	//Matrices//
	void(*RHI_GetProjectionMatrix)(float* aa);
	void(*RHI_GetViewMatrix)(float* aa);
	void(*RHI_GetModelMatrix)(HRL_Mesh* mesh, float* aa);

	//Post Process//
	void(*RHI_CreatePostProcess)(HRL_id material, int priority);
	void(*RHI_DeletePostProcess)(HRL_id post);

	//Global illumination support queries//
	int(*RHI_IsGlobalIlluminationMethodSupported)(int method);
	uint32_t(*RHI_GetGlobalIlluminationSupportedMethods)();

	//Effects//
	void(*RHI_FogPropertyChanged)(HRL_id scene, hrl_fog_t* fog);

	//Debug//
	void(*RHI_DrawDebug)(const DebugRenderer&, float line_thickness);

	//Requests//
	int(*RHI_IsValidTexture)(HRL_id tex);
	int(*RHI_IsValidShader)(HRL_id shader);
}HRL_vtable;

#endif