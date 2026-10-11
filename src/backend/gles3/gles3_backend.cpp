#include "gles3_backend.h"

#include "gles3_renderer.h"

HRL_vtable GetOpenGLES3Backend()
{
	HRL_vtable vtable{};

	vtable.RHI_Init = GLES3_Init;
	vtable.RHI_InitContext = GLES3_InitContext;
	vtable.RHI_Shutdown = GLES3_Shutdown;

	vtable.RHI_WindowResizeCallback = GLES3_WindowResizeCallback;
	vtable.RHI_TakeScreenshot = GLES3_TakeScreenshot;

	vtable.RHI_ResetFramebuffer = GLES3_ResetFramebuffer;

	vtable.RHI_RenderScene = GLES3_DrawScene;

	vtable.RHI_CreateMesh = GLES3_CreateMesh;
	vtable.RHI_CreateSkeletalMesh = GLES3_CreateSkeletalMesh;
	vtable.RHI_CreateMeshLOD = GLES3_CreateMeshLOD;
	vtable.RHI_DeleteMeshLODs = GLES3_DeleteMeshLODs;
	vtable.RHI_CreateSpriteMesh = GLES3_CreateSpriteMesh;
	vtable.RHI_DeleteMesh = GLES3_DeleteMesh;

	vtable.RHI_UpdateLights = GLES3_UpdateLights;
	vtable.RHI_DeleteLight = GLES3_DeleteLight;

	vtable.RHI_CreateTexture = GLES3_CreateTexture;
	vtable.RHI_CreateTextureFromBitmap = GLES3_CreateTextureFromBitmap;
	vtable.RHI_CreateTextureFromBitmapWithId = GLES3_CreateTextureFromBitmapWithId;
	vtable.RHI_DeleteTexture = GLES3_DeleteTexture;
	vtable.RHI_GetTextureSize = GLES3_GetTextureSize;
	vtable.RHI_SetTextureMinFilter = GLES3_SetTextureMinFilter;
	vtable.RHI_SetTextureMaxFilter = GLES3_SetTextureMaxFilter;

	vtable.RHI_CreateScene = GLES3_CreateScene;
	vtable.RHI_DeleteScene = GLES3_DeleteScene;
	vtable.RHI_ResizeSceneTexture = GLES3_ResizeSceneTexture;

	vtable.RHI_CreateShader = GLES3_CreateShader;
	vtable.RHI_CreateShaderWithId = GLES3_CreateShaderWithId;
	vtable.RHI_DeleteShader = GLES3_DeleteShader;

	vtable.RHI_CreatePostProcess = GLES3_CreatePostProcess;
	vtable.RHI_DeletePostProcess = GLES3_DeletePostProcess;

	vtable.RHI_IsGlobalIlluminationMethodSupported = GLES3_IsGlobalIlluminationMethodSupported;
	vtable.RHI_GetGlobalIlluminationSupportedMethods = GLES3_GetGlobalIlluminationSupportedMethods;

	vtable.RHI_FogPropertyChanged = GLES3_FogPropertyChanged;

	vtable.RHI_GetProjectionMatrix = GLES3_GetProjectionMatrix;
	vtable.RHI_GetViewMatrix = GLES3_GetViewMatrix;
	vtable.RHI_GetModelMatrix = GLES3_GetModelMatrix;

	vtable.RHI_DrawDebug = GLES3_DrawDebugAfterScene;

	vtable.RHI_IsValidTexture = GLES3_IsValidTexture;
	vtable.RHI_IsValidShader = GLES3_IsValidShader;

	vtable.RHI_EnableColorPickingBuffer = GLES3_EnableColorPickingBuffer;
	vtable.RHI_SetAntialiasingMode = GLES3_SetAntialiasingMode;

	return vtable;
}
