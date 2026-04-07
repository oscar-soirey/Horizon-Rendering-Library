#ifndef HRL_GL33_RENDERER
#define HRL_GL33_RENDERER

#include "../../hrl.h"
#include "../../core/backend_vtable.h"

#include <vector>

//Control//
void GL33_Init();
void GL33_InitContext(HRL_uint _width, HRL_uint _height, void* loader);
void GL33_Shutdown();

void GL33_ResetFramebuffer();

//Scene & Viewport//
void GL33_ClearScene();
void GL33_BindScene(HRL_id _sceneid);
void GL33_BindViewport(HRL_Viewport* viewport);
void GL33_ComputeFrameMatrices();
void GL33_BindMaterial(HRL_Material* mat);
void GL33_DrawMesh(HRL_Mesh* mesh);

//Lights//
void GL33_UpdateLights(const std::vector<HRL_Light*>& lights);

//Texture//
HRL_id GL33_CreateTexture(const char* _imageContent, size_t _imageSize);
HRL_id GL33_CreateTextureFromBitmap(BitmapResult bitmapResult);
void GL33_DeleteTexture(HRL_id _id);
void GL33_GetTextureSize(HRL_id id, int* width, int* height);
void GL33_SetTextureMinFilter(HRL_id id, HRL_uint _filter);
void GL33_SetTextureMaxFilter(HRL_id id, HRL_uint _filter);

//Scene//
void GL33_CreateScene(HRL_id _newSceneid, int _renderOnScreen);
void GL33_DeleteScene(HRL_id _sceneid);
void GL33_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height);

//Shader//
HRL_id GL33_CreateShader(const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize);
void GL33_DeleteShader(HRL_id _id);

//Matrices
void GL33_GetProjectionMatrix(float* aa);
void GL33_GetViewMatrix(float* aa);
void GL33_GetModelMatrix(HRL_Mesh* mesh, float* aa);

//Debug//
void GL33_DrawDebug(const DebugRenderer& _renderer, float line_thickness);

//Requests//
int GL33_IsValidTexture(HRL_id tex);
int GL33_IsValidShader(HRL_id shader);

#endif