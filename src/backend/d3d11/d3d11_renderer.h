#ifndef HRL_D3D11_RENDERER
#define HRL_D3D11_RENDERER

#include "../../hrl.h"
#include "../../core/backend_vtable.h"

#include <vector>

void D3D11_Init();
void D3D11_InitContext(HRL_uint _width, HRL_uint _height, void* loader);
void D3D11_Shutdown();

void D3D11_BeginFrame();
void D3D11_BindViewport(HRL_Viewport* viewport);
void D3D11_BindMaterial(HRL_Material* mat);
void D3D11_DrawMesh(HRL_Mesh* mesh);

void D3D11_UpdateLights(const std::vector<HRL_Light*>& lights);

HRL_id D3D11_CreateTexture(const char* _imageContent, const size_t _imageSize);
void D3D11_DeleteTexture(HRL_id _id);

HRL_id D3D11_CreateShader(const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize);
void D3D11_DeleteShader(HRL_id _id);

#endif
