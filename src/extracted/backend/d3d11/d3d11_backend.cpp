#include "d3d11_backend.h"

#include "d3d11_renderer.h"
#include "d3d11_texture.h"

HRL_vtable GetDirect3D11Backend()
{
  HRL_vtable vtable;

  /**

  vtable.RHI_Init = ;
  vtable.RHI_InitContext = ;
  vtable.RHI_Shutdown = ;

  vtable.RHI_BeginFrame = ;
  vtable.RHI_BindViewport = ;
  vtable.RHI_BindMaterial = ;
  vtable.RHI_DrawMesh = ;

  vtable.RHI_CreateTexture = ;
  vtable.RHI_DeleteTexture = ;

  vtable.RHI_CreateShader = ;
  vtable.RHI_DeleteShader = ;

  */

  return vtable;
}