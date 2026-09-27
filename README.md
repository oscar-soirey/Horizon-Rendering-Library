# Horizon Rendering Library

<img src="./docs/assets/icon.png" alt="C++Extended logo" width="128">

> \\\*\\\*Version\\\*\\\* `26.6` — A lightweight, explicit rendering abstraction layer.

HRL is a C/C++ rendering library designed to sit on top of multiple graphics backends (OpenGL, Vulkan, D3D11/12, Metal, and more) behind a unified, stable API. It is built around a simple principle: **nothing exists until you create it, and everything you create must be explicitly destroyed.**

\---

## Showcase

A few screenshots showcasing HRL's rendering and debug capabilities:

|Debug shape|Density|Gizmo|
|-|-|-|
|<img src="./docs/assets/screenshots/debug\\\_shape.png" alt="Debug shape" width="300">|<img src="./docs/assets/screenshots/density.png" alt="Density" width="300">|<img src="./docs/assets/screenshots/gizmo.png" alt="Gizmo" width="300">|

|Lightning|Lit|Normal|
|-|-|-|
|<img src="./docs/assets/screenshots/lightning.png" alt="Lightning" width="300">|<img src="./docs/assets/screenshots/lit.png" alt="Lit" width="300">|<img src="./docs/assets/screenshots/normal.png" alt="Normal" width="300">|

|Wireframe|Unlit|VFX|
|-|-|-|
|<img src="./docs/assets/screenshots/wireframe.png" alt="Screenshot" width="300">|<img src="./docs/assets/screenshots/unlit.png" alt="Unlit" width="300">|<img src="./docs/assets/screenshots/vfx.png" alt="VFX" width="300">|

# Full Documentation

[Official Site](https://oscar-soirey.github.io/Horizon-Rendering-Library)

\---

# Credits

### Structure, OpenGL \& Vulkan backend

* Oscar Soirey
* contact : oscarsoirey.contact@gmail.com

### DirectX 11 backend

* [@CodeBYMehdi](https://github.com/CodeBYMehdi)
* contact : mehdibjjj@gmail.com

\---

## Table of Contents

* [Supported Backends](#supported-backends)
* [Core Philosophy — Explicit Object Model](#core-philosophy--explicit-object-model)
* [Lifecycle](#lifecycle)
* [Object Overview](#object-overview)
* [Key Functions](#key-functions)

  * [Initialization](#initialization)
  * [Scenes](#scenes)
  * [Meshes \& Sprites](#meshes--sprites)
  * [Materials \& Shaders](#materials--shaders)
  * [Textures \& Fonts](#textures--fonts)
  * [Lights](#lights)
  * [Camera \& Viewports](#camera--viewports)
  * [Error Handling](#error-handling)
  * [Debug Utilities](#debug-utilities)
* [Minimal Example](#minimal-example)

\---

## Supported Backends

|Constant|Backend|Platform|
|-|-|-|
|`HRL\\\_OpenGL33`|OpenGL 3.3|Windows, Linux, MacOS|
|`HRL\\\_OpenGL45`|OpenGL 4.5|Windows, Linux, MacOS|
|`HRL\\\_Vulkan`|Vulkan|Windows, Linux|
|`HRL\\\_D3D11`|Direct3D 11|Windows, Xbox|
|`HRL\\\_D3D12`|Direct3D 12|Windows, Xbox|
|`HRL\\\_Metal`|Metal|Apple|
|`HRL\\\_NVN`|NVN|Nintedo|
|`HRL\\\_GNM`|GNM|Playstation|

\---

# Comparaison des backends graphiques

|Fonctionnalité|OpenGL 3.3|Vulkan|Direct3D 11|Direct3D 12|Metal|
|-|:-:|:-:|:-:|:-:|:-:|
|Initialisation / contexte|✅|✅|✅|✅|✅|
|Swapchain / présentation|—|✅|✅|✅|✅|
|Mesh 3D|✅|✅|✅|✅|✅|
|Mesh indexé|✅|✅|✅|✅|✅|
|Sprite|✅|✅|✅|✅|✅|
|Skeletal mesh|✅|✅|✅|✅|✅|
|LOD|✅|✅|✅|✅|✅|
|Textures|✅|✅|✅|✅|✅|
|Filtrage texture|✅|✅|✅|✅|✅|
|Matériaux PBR|✅|✅|✅|✅|✅|
|Albedo|✅|✅|✅|✅|✅|
|Normal map|✅|✅|✅|✅|✅|
|Metallic / Roughness|✅|✅|✅|✅|✅|
|Specular|✅|✅|✅|✅|✅|
|Alpha|✅|✅|✅|✅|✅|
|Lights point / directional / spot|✅|✅|✅|✅|✅|
|Sky light|✅|✅|✅|✅|✅|
|Fog classique|✅|✅|✅|✅|✅|
|MSAA / antialiasing|✅|✅|✅|✅|✅|
|Color picking|✅|✅|✅|✅|✅|
|Debug rendering|✅|✅|✅|✅|✅|
|Debug views|✅|⚠️ Partiel|⚠️ Partiel|⚠️ Partiel|⚠️ Partiel|
|Screenshot|✅|✅|✅|✅|✅|
|Post-processing|✅|❌|❌|❌|❌|
|Ambient Occlusion|✅|❌|❌|❌|❌|
|God Rays|✅|❌|❌|❌|❌|
|Sky Sphere|✅|❌|❌|❌|❌|
|Environment mapping|✅|❌|❌|❌|❌|
|Ombres des lights|✅|❌|❌|❌|❌|
|Volumetric fog|✅|❌|❌|❌|❌|
|Global volumetric fog|✅|❌|❌|❌|❌|
|GI / DDGI|✅|❌|❌|❌|❌|
|SSGI|API seulement|❌|❌|❌|❌|
|VCT|API seulement|❌|❌|❌|❌|
|LPV|API seulement|❌|❌|❌|❌|
|Path tracing|API seulement|❌|❌|❌|❌|
|Ray tracing|❌|✅|❌|✅|✅|
|VFX / particules|✅|✅/⚠️|✅/⚠️|✅/⚠️|✅/⚠️|
|VFX mesh particles|✅|✅|✅|✅|✅|
|Widgets|✅|✅|✅|✅|✅|
|Gizmos|✅|⚠️|⚠️|⚠️|⚠️|
|Post-process materials|✅|❌|❌|❌|❌|
|Async texture/shader|CPU/API|✅|⚠️|✅|✅|
|FBX|✅|✅|✅|✅|✅|
|Screen Space Displacement map|✅|✅|✅|✅|✅|
|Virtualized Geomtry|❌|✅|❌|✅|✅|

## Légende

* ✅ —
* ⚠️ —
* ❌ —



\---

## Core Philosophy — Explicit Object Model

HRL does **not** manage object lifetimes on your behalf. Every object — scene, mesh, texture, shader, material, light, camera, viewport, font — must be explicitly created before use and explicitly destroyed when no longer needed.

**If you did not call the `HRL\\\_Create\\\*` function for an object, that object does not exist.** There are no implicit defaults loaded in the background, no hidden allocations, and no garbage collection. This design gives you full, deterministic control over GPU memory and render state.

The consequences of this model are straightforward:

* A mesh without a material assigned will not render correctly.
* A viewport without a camera assigned will not render.
* A material without a shader is invalid.
* A scene with no camera produces no output.

Every `HRL\\\_Create\\\*` function returns an `HRL\\\_id`. Always check that the returned value is **not** `HRL\\\_INVALID\\\_ID` before using it.

\---

## Lifecycle

A standard HRL application follows this structure:

```c
HRL\\\_Init(backend)
HRL\\\_InitContext(width, height, loader)
│
├── Create scenes, cameras, viewports
├── Create shaders, materials, textures
├── Create meshes and assign materials
├── Create lights
│
└── Main loop:
    ├── HRL\\\_BeginFrame()
    ├── \\\[ update object transforms, uniforms, etc. ]
    ├── HRL\\\_EndFrame()
    └── \\\[ swap buffers via your windowing layer ]

HRL\\\_Shutdown() (clean every objects automatically)
```

\---

## Object Overview

|Object|Created by|Depends on|
|-|-|-|
|Scene|`HRL\\\_CreateScene`|—|
|Camera|`HRL\\\_CreateCamera`|Scene|
|Viewport|`HRL\\\_CreateViewport`|Scene, Camera|
|Shader|`HRL\\\_CreateShader`|—|
|Material|`HRL\\\_CreateMaterial`|Shader|
|Texture|`HRL\\\_CreateTexture`|—|
|Font|`HRL\\\_CreateFont`|—|
|Mesh|`HRL\\\_CreateMesh`|Scene, Material|
|Sprite|`HRL\\\_CreateMeshSprite`|Scene, Material|
|Light|`HRL\\\_CreateLight`|Scene|
|Post Process|`HRL\\\_CreatePostProcess`|Scene, Material|

\---

## Key Functions

### Initialization

```c
HRL\\\_Init(HRL\\\_uint api);
```

Selects the graphics backend. Must be called first, before any other function.

```c
HRL\\\_InitContext(HRL\\\_uint width, HRL\\\_uint height, void\\\* loader);
```

Creates the rendering context. `loader` is your platform's function loader (e.g. `glfwGetProcAddress` for OpenGL).

```c
HRL\\\_Shutdown();
```

Releases all internal resources. Call before closing the window.

\---

### Scenes

A **scene** is the top-level container for all renderable objects, lights, and cameras. You need at least one scene to render anything.

```c
HRL\\\_id HRL\\\_CreateScene(int renderOnScreen);
```

Pass `HRL\\\_True` to render directly to the screen, or `HRL\\\_False` to render into an off-screen texture (useful for render-to-texture, post-processing pipelines, etc.). Off-screen scenes default to 480×480; use `HRL\\\_ResizeSceneTexture` to change this.

```c
void HRL\\\_DeleteScene(HRL\\\_id sceneid);
```

\---

### Meshes \& Sprites

```c
HRL\\\_id HRL\\\_CreateMesh(HRL\\\_id sceneid, HRL\\\_uint type, float\\\* vertices);
HRL\\\_id HRL\\\_CreateMeshFromFile(HRL\\\_id sceneid, HRL\\\_uint type, const char\\\* data, size\\\_t size);
HRL\\\_id HRL\\\_CreateMeshSprite(HRL\\\_id sceneid);
```

`type` is one of `HRL\\\_2D\\\_Mesh`, `HRL\\\_3D\\\_Mesh`, or `HRL\\\_3D\\\_SkeletalMesh`. File loading supports `.fbx` and `.obj` formats.

Once created, a mesh must have a material assigned before it will render:

```c
void HRL\\\_SetMeshMaterial(HRL\\\_id meshid, HRL\\\_id matid);
```

Transform functions:

```c
void HRL\\\_SetMeshLocation(HRL\\\_id meshid, float x, float y, float z);
void HRL\\\_SetMeshRotation(HRL\\\_id meshid, float pitch, float yaw, float roll);
void HRL\\\_SetMeshScale(HRL\\\_id meshid, float x, float y, float z);
```

\---

### Materials \& Shaders

A **shader** is a compiled GPU program. A **material** is an instance of a shader with specific uniform values bound to it. The same shader can back many different materials.

```c
HRL\\\_id HRL\\\_CreateShader(const char\\\* vertSrc, size\\\_t vertSize, const char\\\* fragSrc, size\\\_t fragSize);
HRL\\\_id HRL\\\_CreateMaterial(HRL\\\_id shaderid);
```

Setting uniforms on a material:

```c
void HRL\\\_MaterialSetFloat(HRL\\\_id matid, const char\\\* name, float value);
void HRL\\\_MaterialSetVec3(HRL\\\_id matid, const char\\\* name, float x, float y, float z);
void HRL\\\_MaterialSetTexture(HRL\\\_id matid, const char\\\* name, HRL\\\_id textureid);
// Also available: SetInt, SetBool, SetVec2, SetVec4
```

HRL provides built-in default shaders for common cases: `HRL\\\_SpriteShader`, `HRL\\\_Mesh2DShader`, `HRL\\\_Mesh3DShader`, `HRL\\\_DebugShader`.

\---

### Textures \& Fonts

```c
HRL\\\_id HRL\\\_CreateTexture(const char\\\* data, size\\\_t size);
```

Accepted formats: `png`, `jpeg`, `bmp`, `tga`, `gif` (first frame), `hdr`, `psd` (partial). Data must be the raw file contents read in binary mode.

```c
HRL\\\_id HRL\\\_CreateTextureFromText(const char\\\* text, HRL\\\_id fontid,
    float fontSize, float wrapWidth,
    float r, float g, float b,
    float bg\\\_r, float bg\\\_g, float bg\\\_b, float bg\\\_a);
```

Rasterizes a UTF-8 string into a GPU texture. Pass `wrapWidth = 0` to disable line wrapping. Set `bg\\\_a = 0` for a transparent background.

```c
HRL\\\_id HRL\\\_CreateFont(const char\\\* data, size\\\_t size);
```

Loads a TrueType font (`.ttf`) from a memory buffer. Required before calling `HRL\\\_CreateTextureFromText`.

\---

### Lights

```c
HRL\\\_id HRL\\\_CreateLight(HRL\\\_id sceneid, HRL\\\_uint type);
```

`type` is one of `HRL\\\_PointLight`, `HRL\\\_DirectionalLight`, or `HRL\\\_SpotLight`.

```c
void HRL\\\_SetLightColor(HRL\\\_id lightid, float r, float g, float b);
void HRL\\\_SetLightIntensity(HRL\\\_id lightid, float intensity);
void HRL\\\_SetLightAttenuation(HRL\\\_id lightid, float attenuation);
void HRL\\\_SetLightLocation(HRL\\\_id lightid, float x, float y, float z);
void HRL\\\_SetLightRotation(HRL\\\_id lightid, float pitch, float yaw, float roll);
```

\---

### Camera \& Viewports

A **camera** defines the point of view. A **viewport** maps a camera's output to a rectangular region of the screen. You need both to see anything rendered.

```c
HRL\\\_id HRL\\\_CreateCamera(HRL\\\_id sceneid, HRL\\\_uint type);  // HRL\\\_Ortho or HRL\\\_Perspective
void HRL\\\_SetCameraPosition(HRL\\\_id camid, float x, float y, float z);
void HRL\\\_SetCameraRotation(HRL\\\_id camid, float pitch, float yaw, float roll);
void HRL\\\_SetCameraPerspectiveFov(HRL\\\_id camid, float fov);    // degrees
void HRL\\\_SetCameraOrthoVertical(HRL\\\_id camid, float height);  // world units
```

```c
// Coordinates are normalized \\\[0..1]. (0,0) = top-left, (1,1) = bottom-right.
HRL\\\_id HRL\\\_CreateViewport(HRL\\\_id sceneid, HRL\\\_id cameraid, float x, float y, float w, float h);
```

Multiple viewports can be created for the same scene, enabling split-screen or picture-in-picture setups.

\---

### Error Handling

HRL records the last error internally. Query it after any operation that returns an `HRL\\\_id` or may fail:

```c
HRL\\\_Error HRL\\\_GetLastError(const char\\\*\\\* detail, HRL\\\_Severity\\\* severity);
```

For continuous monitoring, register a callback that will be invoked every time an error occurs:

```c
void HRL\\\_RegisterErrorCallback(HRL\\\_ErrorCallback callback);
// Signature: void callback(HRL\\\_Error code, HRL\\\_Severity severity, const char\\\* detail)
```

The convenience macro `HRL\\\_CheckErrors()` prints any pending error to stdout, including the source file and line number. Suitable for debug builds.

Severity levels: `HRL\\\_SEVERITY\\\_WEAK\\\_WARNING` · `HRL\\\_SEVERITY\\\_WARNING` · `HRL\\\_SEVERITY\\\_ERROR` · `HRL\\\_SEVERITY\\\_FATAL`

\---

### Debug Utilities

**Debug views** override scene rendering with a diagnostic mode:

```c
void HRL\\\_DrawSceneAsDebugMode(HRL\\\_id sceneid, HRL\\\_uint mode);
// mode: HRL\\\_DebugViewNone | HRL\\\_DebugViewNormal | HRL\\\_DebugViewLights
```

**Immediate-mode debug geometry** (must be called every frame to persist):

```c
void HRL\\\_DrawDebugSegment(...);   // Line segment
void HRL\\\_DrawDebugCircle(...);    // Circle (hollow or solid)
void HRL\\\_DrawDebugCapsule(...);   // Capsule (hollow or solid)
void HRL\\\_DrawDebugPolygon(...);   // Arbitrary polygon
void HRL\\\_DrawDebugPoint(...);     // Screen-space point
```

**Screenshot:**

```c
void HRL\\\_TakeScreenshot(HRL\\\_id sceneid, const char\\\* path); // Saves as PNG
```

\---

## Minimal Example

```c
// 1. Initialize
HRL\\\_Init(HRL\\\_OpenGL45);
HRL\\\_InitContext(1280, 720, glfwGetProcAddress);

// 2. Scene, camera, viewport
HRL\\\_id scene    = HRL\\\_CreateScene(HRL\\\_True);
HRL\\\_id camera   = HRL\\\_CreateCamera(scene, HRL\\\_Perspective);
HRL\\\_id viewport = HRL\\\_CreateViewport(scene, camera, 0.f, 0.f, 1.f, 1.f);
HRL\\\_SetCameraPerspectiveFov(camera, 60.f);
HRL\\\_SetCameraPosition(camera, 0.f, 0.f, -5.f);

// 3. Shader, material, texture
HRL\\\_id shader   = HRL\\\_CreateShader(vert\\\_src, vert\\\_len, frag\\\_src, frag\\\_len);
HRL\\\_id material = HRL\\\_CreateMaterial(shader);
HRL\\\_id texture  = HRL\\\_CreateTexture(png\\\_data, png\\\_size);
HRL\\\_MaterialSetTexture(material, HRL\\\_T\\\_ALBEDO, texture);

// 4. Mesh
HRL\\\_id mesh = HRL\\\_CreateMeshFromFile(scene, HRL\\\_3D\\\_Mesh, obj\\\_data, obj\\\_size);
HRL\\\_SetMeshMaterial(mesh, material);

// 5. Render loop
while (!glfwWindowShouldClose(window)) {
    HRL\\\_BeginFrame();
    HRL\\\_EndFrame();
    glfwSwapBuffers(window);
    glfwPollEvents();
}

// 6. Cleanup
HRL\\\_Shutdown();
```

\---

*HRL is licensed under the Apache License 2.0. See* [*LICENSE*](http://www.apache.org/licenses/LICENSE-2.0) *for details.
Contact: oscarsoirey.contact@gmail.com*

