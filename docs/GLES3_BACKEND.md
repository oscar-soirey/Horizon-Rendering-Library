# OpenGL ES 3.0 backend (`HRL_OPENGL_ES_30`)

A small renderer for mobile (Android): 2D and 3D, without the advanced effects of
the desktop OpenGL 3.3 backend. Sources: `src/backend/gles3/`.

```c
HRL_Init(HRL_OPENGL_ES_30);
HRL_InitContext(width, height, (void*)loader);   // an OpenGL ES 3 context must be current
```

`loader` is a `void* (*)(const char* name)` returning GL entry points:
`eglGetProcAddress` / `dlsym` on Android, `glfwGetProcAddress` with GLFW. The backend
only calls functions that are part of OpenGL ES 3.0 (the list is in `GLES3_LoadGL`).

## Building

| Define | Effect |
|---|---|
| `HRL_ENABLE_GLES3` | compiles the backend in and makes `HRL_OPENGL_ES_30` selectable |
| `HRL_DISABLE_OPENGL33` | leaves the desktop OpenGL 3.3 backend out (mobile builds) |

With HRL's own `CMakeLists.txt`: `-DHRL_ENABLE_GLES3=ON` adds the four `gles3_*.cpp`
files to the DLL. For a mobile build compile `glad.c`, `ufbx.c`, `hrl.cpp`,
`core/*.cpp`, `backend/gles3/*.cpp` and the embedded resources, with both defines,
and do not compile `backend/opengl33/*`.

Both backends can live in the same binary (useful to compare them on a PC). The
`HRL_GL_*` functions of `hrl_gl.h` are implemented by the OpenGL 3.3 backend; the ES
backend provides them only when `HRL_DISABLE_OPENGL33` is defined.

## What is supported

| Feature | Notes |
|---|---|
| Sprites | instanced, NEAREST filtering, same sorting as desktop (distance, then draw order) |
| Widgets, labels, SDF text, screen messages | same input handling and layout |
| Textures | PNG/JPG..., from bitmap, async; min/mag filters, mipmaps |
| Materials and custom shaders | same uniform names and texture units 0..5 |
| Static 3D meshes | LODs, frustum culling, two-sided materials, FBX material values |
| Skeletal meshes | 128 bones, same `BoneBlock` UBO |
| Lights | point, directional, spot, sky: 32 per scene, same `LightBlock` layout |
| VFX | billboards, stretched billboards, mesh particles; alpha / additive / multiply |
| Distance fog (`HRL_SetFog*`) | linear, exp, exp2 |
| Sky sphere | gradient or equirectangular texture |
| Debug segments / polygons | line width is limited by the driver |
| Off-screen scenes | RGBA8 color + 24-bit depth |
| Screenshots | `HRL_TakeScreenshot` |

## What is not

Accepted by the API but without effect: shadows, post-processes (bloom, tone
mapping...), global illumination, ambient occlusion, screen-space reflections,
decals, volumetric fog and clouds, god rays, environment reflections, screen-space
displacement, landscapes, voxel worlds, gizmos, color picking, MSAA
(`HRL_SetAntialiasingMode`: choose a multisampled EGL config instead), the debug views.

## Differences with the desktop backend

- A scene created with `HRL_CreateScene(HRL_TRUE)` is drawn straight into the default
  framebuffer: no intermediate HDR target and no blit, which is the cheapest path on
  tile-based mobile GPUs. Consequence: `HRL_GL_GetSceneTextureGL_ID` returns 0 for it.
  Off-screen scenes render into an RGBA8 texture (colors are clamped to [0, 1]).
- Without a post-process the desktop backend also ends up clamping its HDR image to
  the screen, so both produce the same picture for the supported features.
- Textures keep no CPU copy after the upload, and the single-color default maps are
  stored as 1x1 textures.
- Sprites lit only by sky lights, with a material that has none of the normal /
  specular / roughness / metallic maps, take a fast path (two texture fetches instead
  of six). The result is identical.

## Custom shaders

Write them in GLSL ES 3.00 (`#version 300 es`). A source starting with a desktop
`#version` line (for example `#version 330 core`) is rewritten to
`#version 300 es` + `precision highp float; precision highp int;`, which is enough
for simple shaders. GLSL ES is stricter, the usual things to fix are:

- no implicit conversions: `float x = 1;` must be `1.0`, unsigned literals need `u`;
- no initializers on uniforms (`uniform float gamma = 2.2;`);
- only write to the color outputs that exist: one, `layout(location = 0)`.

The built-in shaders also receive `uniform int LightCount` (number of used entries of
`lights[]`); unused entries are zeroed, so looping over all 32 still works.
