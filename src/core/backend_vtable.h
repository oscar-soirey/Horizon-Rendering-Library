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

	//Draw & batching//
	void(*RHI_BeginFrame)();

	//Call at the end of function end frame
	void(*RHI_ResetFramebuffer)();

	//Bind Viewport appeller avant les autres binds et draw car il d�finit la camera!
	void(*RHI_BindViewport)(HRL_Viewport* viewport);
	void(*RHI_BindMaterial)(HRL_Material* mat);

	//Draw
	void(*RHI_DrawMesh)(HRL_Mesh* mesh);

	//Lights//
	void(*RHI_UpdateLights)(const std::vector<HRL_Light*>& lights);

	//Window//
	void(*RHI_WindowResizeCallback)(int width, int height);

	//Textures//
	HRL_id(*RHI_CreateTexture)(const char* imageContent, const size_t imageSize);
	void(*RHI_DeleteTexture)(HRL_id id);

	//Scenes//
	void(*RHI_CreateScene)(HRL_id newSceneId, int renderOnScreen);
	void(*RHI_DeleteScene)(HRL_id sceneid);
	void(*RHI_BindScene)(HRL_id sceneid);
	void(*RHI_ClearScene)();
	void(*RHI_ResizeSceneTexture)(HRL_id sceneid, int width, int height);
	void(*RHI_EnableColorPickingBuffer)(HRL_id sceneid, int enable);

	//Shaders//
	HRL_id(*RHI_CreateShader)(const char* vertContent, size_t vertSize, const char* fragContent, size_t fragSize);
	void(*RHI_DeleteShader)(HRL_id id);

	//Debug//
	void(*RHI_DrawDebug)(const DebugRenderer&, float line_thickness);
}HRL_vtable;

#endif