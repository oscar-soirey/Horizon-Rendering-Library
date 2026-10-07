#include "gl33_backend.h"

#include "gl33_renderer.h"
#include "gl33_texture.h"

HRL_vtable GetOpenGL33Backend()
{
	HRL_vtable vtable{};

	vtable.RHI_Init = GL33_Init;
	vtable.RHI_InitContext = GL33_InitContext;
	vtable.RHI_Shutdown = GL33_Shutdown;

	vtable.RHI_WindowResizeCallback = GL33_WindowResizeCallback;
	vtable.RHI_TakeScreenshot = GL33_TakeScreenshot;

	vtable.RHI_ResetFramebuffer = GL33_ResetFramebuffer;

	vtable.RHI_RenderScene = GL33_DrawScene;

	vtable.RHI_CreateMesh = GL33_CreateMesh;
	vtable.RHI_CreateSkeletalMesh = GL33_CreateSkeletalMesh;
	vtable.RHI_CreateMeshLOD = GL33_CreateMeshLOD;
	vtable.RHI_DeleteMeshLODs = GL33_DeleteMeshLODs;
	vtable.RHI_CreateSpriteMesh = GL33_CreateSpriteMesh;
	vtable.RHI_DeleteMesh = GL33_DeleteMesh;

	//vtable.RHI_BeginFrame = GL33_BeginFrame;
	//vtable.RHI_ClearScene = GL33_ClearScene;
	//vtable.RHI_BindScene = GL33_BindScene;
	//vtable.RHI_BindViewport = GL33_BindViewport;
	//vtable.RHI_ComputeFrameMatrices = GL33_ComputeFrameMatrices;
	//vtable.RHI_BindMaterial = GL33_BindMaterial;
	//vtable.RHI_DrawMesh = GL33_DrawMesh;

	vtable.RHI_UpdateLights = GL33_UpdateLights;
	vtable.RHI_DeleteLight = GL33_DeleteLight;

	vtable.RHI_CreateTexture = GL33_CreateTexture;
	vtable.RHI_CreateTextureFromBitmap = GL33_CreateTextureFromBitmap;
	vtable.RHI_CreateTextureFromBitmapWithId = GL33_CreateTextureFromBitmapWithId;
	vtable.RHI_DeleteTexture = GL33_DeleteTexture;
	vtable.RHI_GetTextureSize = GL33_GetTextureSize;
	vtable.RHI_SetTextureMinFilter = GL33_SetTextureMinFilter;
	vtable.RHI_SetTextureMaxFilter = GL33_SetTextureMaxFilter;

	vtable.RHI_CreateScene = GL33_CreateScene;
	vtable.RHI_DeleteScene = GL33_DeleteScene;
	vtable.RHI_ResizeSceneTexture = GL33_ResizeSceneTexture;

	vtable.RHI_CreateShader = GL33_CreateShader;
	vtable.RHI_CreateShaderWithId = GL33_CreateShaderWithId;
	vtable.RHI_DeleteShader = GL33_DeleteShader;

	vtable.RHI_CreatePostProcess = GL33_CreatePostProcess;
	vtable.RHI_DeletePostProcess = GL33_DeletePostProcess;

	vtable.RHI_IsGlobalIlluminationMethodSupported = GL33_IsGlobalIlluminationMethodSupported;
	vtable.RHI_GetGlobalIlluminationSupportedMethods = GL33_GetGlobalIlluminationSupportedMethods;

	vtable.RHI_FogPropertyChanged = GL33_FogPropertyChanged;

	vtable.RHI_GetProjectionMatrix = GL33_GetProjectionMatrix;
	vtable.RHI_GetViewMatrix = GL33_GetViewMatrix;
	vtable.RHI_GetModelMatrix = GL33_GetModelMatrix;

	vtable.RHI_DrawDebug = GL33_DrawDebugAfterScene;

	vtable.RHI_IsValidTexture = GL33_IsValidTexture;
	vtable.RHI_IsValidShader = GL33_IsValidShader;

	vtable.RHI_EnableColorPickingBuffer = GL33_EnableColorPickingBuffer;
	vtable.RHI_SetAntialiasingMode = GL33_SetAntialiasingMode;

	return vtable;
}