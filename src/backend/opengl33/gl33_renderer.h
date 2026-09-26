#ifndef HRL_GL33_RENDERER
#define HRL_GL33_RENDERER

#include "../../hrl.h"
#include "../../core/backend_vtable.h"

#include <vector>

class GL33_Texture;

//Control//
void GL33_Init();
void GL33_InitContext(HRL_uint _width, HRL_uint _height, void* loader);
void GL33_Shutdown();

void GL33_WindowResizeCallback(int width, int height);
void GL33_TakeScreenshot(HRL_id scene, const char* target_path);

void GL33_ResetFramebuffer();

void GL33_DrawScene(hrl_scene_t* scene, HRL_id scene_id);

//Meshes//
int GL33_CreateMesh(HRL_id id, const HRL_Vertex3D* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count);
int GL33_CreateSkeletalMesh(HRL_id id, const HRL_SkeletalVertex* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count, HRL_uint bone_count);
int GL33_CreateMeshLOD(HRL_id id, HRL_uint level, const HRL_Vertex3D* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count);
void GL33_DeleteMeshLODs(HRL_id id);
int GL33_CreateSpriteMesh(HRL_id id);
void GL33_DeleteMesh(HRL_id id);

//Lights//
void GL33_DeleteLight(HRL_id id);

//Scene & Viewport//
//void GL33_ClearScene();
//void GL33_BindScene(HRL_id _sceneid);
//void GL33_BindViewport(HRL_Viewport* viewport);
//void GL33_ComputeFrameMatrices();
//void GL33_BindMaterial(HRL_Material* mat);
//void GL33_DrawMesh(HRL_Mesh* mesh);

//Lights//
void GL33_UpdateLights(const std::vector<HRL_Light*>& lights);

//Texture//
HRL_id GL33_CreateTexture(const char* _imageContent, size_t _imageSize);
HRL_id GL33_CreateTextureFromBitmap(BitmapResult bitmapResult);
HRL_id GL33_CreateTextureFromBitmapWithId(HRL_id id, BitmapResult bitmapResult);
void GL33_DeleteTexture(HRL_id _id);
void GL33_GetTextureSize(HRL_id id, int* width, int* height);
void GL33_SetTextureMinFilter(HRL_id id, HRL_EFilterType _filter);
void GL33_SetTextureMaxFilter(HRL_id id, HRL_EFilterType _filter);
const GL33_Texture* GL33_FindTexture(HRL_id id);

//Scene//
void GL33_CreateScene(HRL_id _newSceneid, int _renderOnScreen);
void GL33_DeleteScene(HRL_id _sceneid);
void GL33_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height);

//Shader//
HRL_id GL33_CreateShader(const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize);
HRL_id GL33_CreateShaderWithId(HRL_id id, const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize);
void GL33_DeleteShader(HRL_id _id);

//Post Process//
void GL33_CreatePostProcess(HRL_id mat, int priority);
void GL33_DeletePostProcess(HRL_id post);

//Effects
void GL33_FogPropertyChanged(HRL_id scene, hrl_fog_t* fog_ptr);

//Matrices
void GL33_GetProjectionMatrix(float* aa);
void GL33_GetViewMatrix(float* aa);
void GL33_GetModelMatrix(HRL_Mesh* mesh, float* aa);

//Debug//
void GL33_DrawDebug(const DebugRenderer& _renderer, float line_thickness);

//Requests//
int GL33_IsGlobalIlluminationMethodSupported(int method);
uint32_t GL33_GetGlobalIlluminationSupportedMethods();

int GL33_IsValidTexture(HRL_id tex);
int GL33_IsValidShader(HRL_id shader);

//Color Picking
void GL33_EnableColorPickingBuffer(HRL_id scene, int _enable);
void GL33_SetAntialiasingMode(int samples);

#endif