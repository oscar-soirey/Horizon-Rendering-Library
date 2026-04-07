#include "gl33_backend.h"

#include "gl33_renderer.h"
#include "gl33_texture.h"

HRL_vtable GetOpenGL33Backend()
{
	HRL_vtable vtable;

	vtable.RHI_Init = GL33_Init;
	vtable.RHI_InitContext = GL33_InitContext;
	vtable.RHI_Shutdown = GL33_Shutdown;

	vtable.RHI_ResetFramebuffer = GL33_ResetFramebuffer;

	//vtable.RHI_BeginFrame = GL33_BeginFrame;
	vtable.RHI_ClearScene = GL33_ClearScene;
	vtable.RHI_BindScene = GL33_BindScene;
	vtable.RHI_BindViewport = GL33_BindViewport;
	vtable.RHI_ComputeFrameMatrices = GL33_ComputeFrameMatrices;
	vtable.RHI_BindMaterial = GL33_BindMaterial;
	vtable.RHI_DrawMesh = GL33_DrawMesh;

	vtable.RHI_UpdateLights = GL33_UpdateLights;

	vtable.RHI_CreateTexture = GL33_CreateTexture;
	vtable.RHI_CreateTextureFromBitmap = GL33_CreateTextureFromBitmap;
	vtable.RHI_DeleteTexture = GL33_DeleteTexture;
	vtable.RHI_GetTextureSize = GL33_GetTextureSize;
	vtable.RHI_SetTextureMinFilter = GL33_SetTextureMinFilter;
	vtable.RHI_SetTextureMaxFilter = GL33_SetTextureMaxFilter;

	vtable.RHI_CreateScene = GL33_CreateScene;
	vtable.RHI_DeleteScene = GL33_DeleteScene;
	vtable.RHI_ResizeSceneTexture = GL33_ResizeSceneTexture;

	vtable.RHI_CreateShader = GL33_CreateShader;
	vtable.RHI_DeleteShader = GL33_DeleteShader;

	vtable.RHI_GetProjectionMatrix = GL33_GetProjectionMatrix;
	vtable.RHI_GetViewMatrix = GL33_GetViewMatrix;
	vtable.RHI_GetModelMatrix = GL33_GetModelMatrix;

	vtable.RHI_DrawDebug = GL33_DrawDebug;

	vtable.RHI_IsValidTexture = GL33_IsValidTexture;
	vtable.RHI_IsValidShader = GL33_IsValidShader;

	return vtable;
}