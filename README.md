# Note
This project was developed by a single passionate developer. I’ve tried to make everything work smoothly, but there may still be bugs.
If you encounter any issues or have suggestions, please feel free to contact me at: oscarsoirey.contact@gmail.com
Thank you for your support and understanding!

---

# Credits
- Structure, OpenGL & Vulkan backend : Oscar Soirey (oscarsoirey.contact@gmail.com)
- DirectX 11 backend : [@CodeBYMehdi](https://github.com/CodeBYMehdi) (mehdibjjj@gmail.com)

---

# HRL Engine

HRL is a high quality, multi-API rendering engine made for professional use. It supports OpenGL, Vulkan, DirectX, Metal, and modern consoles (NVN, GNM).
It provides a simple C/C++ API to manage meshes, lights, textures, shaders, materials, viewports, and cameras.

---

# Version

HRL is currently in pre-alpha version 0.1 and only has the OpenGL backend available at the moment.

---

## Table of Contents

- [Installation](#installation)  
- [Initialization](#initialization)  
- [Frame](#frame)  
- [Window](#window)  
- [Error Handling](#error-handling)  
- [Meshes & Sprites](#meshes--sprites)  
- [Lights](#lights)  
- [Textures](#textures)  
- [Shaders & Materials](#shaders--materials)  
- [Post-process](#post-process)  
- [Viewports](#viewports)  
- [Cameras](#cameras)  
- [Constants & IDs](#constants--ids)  

---

## Installation

Include `hrl.h` in your file:

```c
#include "hrl.h"
```

## Initialization

```c
void HRL_Init(HRL_uint api);
```
apis: HRL_OpenGL33, HRL_OpenGL45, HRL_Vulkan, HRL_DX11, HRL_DX12, HRL_Metal, HRL_NVN, HRL_GNM

```c
void HRL_InitContext(HRL_uint width, HRL_uint height, void* loader);
```
width / height: window or rendering context size
loader: function to retrieve API function pointers (e.g., wglGetProcAddress, vkGetInstanceProcAddr)

```c
void HRL_Shutdown();
```

## Frame

```c
void HRL_BeginFrame();
```
Refresh and clear the viewport (needs to be called before EndFrame();

```c
void HRL_EndFrame();
```
Render the scene.


## Window

HRL doesn't manage any window. You need to inform HRL of what is happening with the window

```c
void HRL_WindowResizeCallback(int width, int height);
```
Call when the window is resized to update viewport and projection matrices.


## Error Handling

```c
const char* HRL_GetLastError();
```
Returns the last error message as a string.


## Meshes & Sprites

# Meshes:
```c
HRL_id HRL_CreateMesh(HRL_uint type);
void HRL_DeleteMesh(HRL_id meshid);
void HRL_SetMeshMaterial(HRL_id meshid, HRL_id matid);
void HRL_SetMeshLocation(HRL_id meshid, float x, float y, float z);
void HRL_SetMeshRotation(HRL_id meshid, float yaw, float pitch, float roll);
void HRL_SetMeshScale(HRL_id meshid, float x, float y, float z);
```

Supported mesh types:
- `HRL_Sprite`
- `HRL_2D_Mesh`
- `HRL_3D_Mesh`


# Lights:
```c
HRL_id HRL_CreateLight(HRL_uint type);
void HRL_DeleteLight(HRL_id lightid);
void HRL_SetLightColor(HRL_id lightid, float r, float g, float b);
void HRL_SetLightIntensity(HRL_id lightid, float intensity);
void HRL_SetLightAttenuation(HRL_id lightid, float attenuation);
void HRL_SetLightLocation(HRL_id lightid, float x, float y, float z);
void HRL_SetLightRotation(HRL_id lightid, float yaw, float pitch, float roll);
```

Supported light types:
- `HRL_PointLight`
- `HRL_DirectionalLight`
- `HRL_SpotLight`


## Textures

```c
HRL_id HRL_CreateTexture(HRL_uint type, const char* fileContent, size_t bufferSize);
void HRL_DeleteTexture(HRL_id textureid);
```

Supported texture types:
| Texture Type           | Description                     |
|------------------------|---------------------------------|
| `HRL_Tex_Albedo`       | Diffuse / albedo map             |
| `HRL_Tex_Normal`       | Normal map                      |
| `HRL_Tex_Specular`     | Specular map                    |
| `HRL_Tex_Roughness`    | Roughness map                   |
| `HRL_Tex_Metalic`      | Metallic map                    |
| `HRL_Tex_Alpha`        | Alpha / transparency map        |
| `HRL_Tex_ShadowMap`    | Shadow map                      |
| `HRL_Tex_CubeMap`      | Cubemap / environment map       |

Supported formats: PNG, JPEG, BMP, TGA, GIF, HDR, PSD. (more information at the `stb` library documentation)


## Shaders & Materials

```c
HRL_id HRL_CreateShader(const char* vertContent, size_t vertSize, const char* fragContent, size_t fragSize);
void HRL_DeleteShader(HRL_id shaderid);

HRL_id HRL_CreateMaterial(HRL_id shaderid);
void HRL_DeleteMaterial(HRL_id matid);

void HRL_MaterialSetInt(HRL_id matid, const char* uniformName, int value);
void HRL_MaterialSetBool(HRL_id matid, const char* uniformName, int value);
void HRL_MaterialSetFloat(HRL_id matid, const char* uniformName, float value);
void HRL_MaterialSetVec2(HRL_id matid, const char* uniformName, float x, float y);
void HRL_MaterialSetVec3(HRL_id matid, const char* uniformName, float x, float y, float z);
void HRL_MaterialSetVec4(HRL_id matid, const char* uniformName, float x, float y, float z, float w);
void HRL_MaterialSetTexture(HRL_id matid, const char* uniformName, HRL_id textureid);
```

Reserved default shader IDs:

- `HRL_SpriteShader = UINT32_MAX`
- `HRL_Mesh2DShader = UINT32_MAX - 1`
- `HRL_Mesh3DShader = UINT32_MAX - 2`


## Post-process

```c
HRL_id HRL_CreatePostProcess(HRL_id matid);
void HRL_DeletePostProcess(HRL_id postid);
```

Apply post-processing effects using a material.


## Viewports

```c
HRL_id HRL_CreateWiewport(float x, float y, float width, float height);
void HRL_DeleteViewport(HRL_id viewportid);
void HRL_SetViewportRect(HRL_id viewportid, float x, float y, float width, float height);
```

Normalized coordinates [0,1]. Useful for split-screen or mini-maps.


## Cameras

```c
void HRL_SetCameraView(HRL_id viewportid, HRL_uint type);
void HRL_SetCameraOrthoVertical(HRL_id viewportid, float height);
void HRL_SetCameraPerspectiveFov(HRL_id viewportid, float fov);
void HRL_SetCameraPosition(HRL_id viewportid, float x, float y, float z);
void HRL_SetCameraRotation(HRL_id viewportid, float yaw, float pitch, float roll);
```

Camera types:

- `HRL_Ortho`
- `HRL_Perspective`

Each viewport contains one camera.


## Constants & IDs

| Category        | Constants / IDs                                                                 |
|-----------------|----------------------------------------------------------------------------------|
| Boolean         | `HRL_True`, `HRL_False`                                                          |
| Error           | `HRL_InvalidID`                                                                  |
| API             | `HRL_OpenGL33`, `HRL_OpenGL45`, `HRL_Vulkan`, `HRL_DX11`, `HRL_DX12`, `HRL_Metal`, `HRL_NVN`, `HRL_GNM` |
| Lights          | `HRL_PointLight`, `HRL_DirectionalLight`, `HRL_SpotLight`                        |
| Meshes          | `HRL_Sprite`, `HRL_2D_Mesh`, `HRL_3D_Mesh`                                       |
| Debug Geometry  | `HRL_DebugLine`, `HRL_DebugBox`, `HRL_DebugSphere`                               |
| Textures        | `HRL_Tex_Albedo`, `HRL_Tex_Normal`, `HRL_Tex_Specular`, `HRL_Tex_Roughness`, `HRL_Tex_Metalic`, `HRL_Tex_Alpha`, `HRL_Tex_ShadowMap`, `HRL_Tex_CubeMap` |
| Camera          | `HRL_Ortho`, `HRL_Perspective`                                                   |


## Notes

- `Every object is identified by a unique HRL_id.`

- `Default shaders and materials have reserved IDs.`

- `HRL is minimal, flexible, and easy to integrate into C/C++ projects.`
