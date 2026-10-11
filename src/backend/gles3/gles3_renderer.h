// HRL - OpenGL ES 3.0 backend (mobile).
//
// A deliberately small renderer: 2D (sprites, widgets, text), 3D (static and
// skinned meshes, lights), VFX particles, debug primitives and the sky sphere.
// No shadows, post-processing, global illumination or other advanced effects.
// See docs/GLES3_BACKEND.md for the exact feature list.
#ifndef HRL_GLES3_RENDERER
#define HRL_GLES3_RENDERER

#include "../../hrl.h"
#include "../../core/backend_vtable.h"

#include <vector>

//Control//
void GLES3_Init();
void GLES3_InitContext(HRL_uint _width, HRL_uint _height, void* loader);
void GLES3_Shutdown();

void GLES3_WindowResizeCallback(int width, int height);
void GLES3_TakeScreenshot(HRL_id scene, const char* target_path);

void GLES3_ResetFramebuffer();

void GLES3_DrawScene(hrl_scene_t* scene, HRL_id scene_id);

//Meshes//
int GLES3_CreateMesh(HRL_id id, const HRL_Vertex3D* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count);
int GLES3_CreateSkeletalMesh(HRL_id id, const HRL_SkeletalVertex* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count, HRL_uint bone_count);
int GLES3_CreateMeshLOD(HRL_id id, HRL_uint level, const HRL_Vertex3D* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count);
void GLES3_DeleteMeshLODs(HRL_id id);
int GLES3_CreateSpriteMesh(HRL_id id);
void GLES3_DeleteMesh(HRL_id id);

//Lights//
void GLES3_UpdateLights(const std::vector<HRL_Light*>& lights);
void GLES3_DeleteLight(HRL_id id);

//Texture//
HRL_id GLES3_CreateTexture(const char* _imageContent, size_t _imageSize);
HRL_id GLES3_CreateTextureFromBitmap(BitmapResult bitmapResult);
HRL_id GLES3_CreateTextureFromBitmapWithId(HRL_id id, BitmapResult bitmapResult);
void GLES3_DeleteTexture(HRL_id _id);
void GLES3_GetTextureSize(HRL_id id, int* width, int* height);
void GLES3_SetTextureMinFilter(HRL_id id, HRL_EFilterType _filter);
void GLES3_SetTextureMaxFilter(HRL_id id, HRL_EFilterType _filter);

//Scene//
void GLES3_CreateScene(HRL_id _newSceneid, int _renderOnScreen);
void GLES3_DeleteScene(HRL_id _sceneid);
void GLES3_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height);

//Shader//
HRL_id GLES3_CreateShader(const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize);
HRL_id GLES3_CreateShaderWithId(HRL_id id, const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize);
void GLES3_DeleteShader(HRL_id _id);

//Not supported by this backend (accepted and ignored)//
void GLES3_CreatePostProcess(HRL_id mat, int priority);
void GLES3_DeletePostProcess(HRL_id post);
void GLES3_FogPropertyChanged(HRL_id scene, hrl_fog_t* fog_ptr);
void GLES3_EnableColorPickingBuffer(HRL_id scene, int _enable);
void GLES3_SetAntialiasingMode(int samples);
int GLES3_IsGlobalIlluminationMethodSupported(int method);
uint32_t GLES3_GetGlobalIlluminationSupportedMethods();

//Matrices
void GLES3_GetProjectionMatrix(float* aa);
void GLES3_GetViewMatrix(float* aa);
void GLES3_GetModelMatrix(HRL_Mesh* mesh, float* aa);

//Debug//
void GLES3_DrawDebugAfterScene(const DebugRenderer& _renderer, float line_thickness);

//Requests//
int GLES3_IsValidTexture(HRL_id tex);
int GLES3_IsValidShader(HRL_id shader);

#endif
