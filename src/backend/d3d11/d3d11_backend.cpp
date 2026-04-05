#ifndef HRL_D3D11_BACKEND
#define HRL_D3D11_BACKEND

#include "../../core/backend_vtable.h"

HRL_vtable GetDirect3D11Backend() {
    HRL_vtable vtable;
    vtable.RHI_Init = D3D11_Init;
    vtable.RHI_InitContext = D3D11_InitContext;
    vtable.RHI_Shutdown = D3D11_Shutdown;
    vtable.RHI_BeginFrame = D3D11_BeginFrame;
    vtable.RHI_EndFrame = D3D11_EndFrame;
    vtable.RHI_DrawMesh = D3D11_DrawMesh;
    vtable.RHI_BindMaterial = D3D11_BindMaterial;
    vtable.RHI_BindViewport = D3D11_BindViewport;
    vtable.RHI_BindCamera = D3D11_BindCamera;
    vtable.RHI_BindLight = D3D11_BindLight;
    vtable.RHI_BindTexture = D3D11_BindTexture;
    vtable.RHI_BindShader = D3D11_BindShader;
    vtable.RHI_BindPostProcess = D3D11_BindPostProcess;
    vtable.RHI_BindPostProcess = D3D11_BindPostProcess;
// mapping de ttes les fonctions de la vtable
    return vtable;
}
