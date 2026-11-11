# Changelog

## Vulkan: feature parity with OpenGL (API 0.9)

### Added (Vulkan)

- **2D and UI**: sprites (UV regions, draw order, picking as `HRL_SPRITE`, nearest texels), widgets (buttons, labels, images, sliders, checkboxes, progress bars, mouse input and callbacks), SDF texts, on-screen messages, mesh information texts (`HRL_DEBUG_VIEW_MESH_INFO`) and gizmos. The frontend runs the widget logic and gives the backend textured quads per viewport (`HRL_SceneFrame::overlays`).
- **Anti-aliasing**: FXAA (default) and temporal anti-aliasing (`HRL_Vulkan_SetAntiAliasing`); `HRL_SetAntialiasingMode` with more than one sample selects TAA.
- **VSync**: `HRL_Vulkan_SetVSync` (MAILBOX, else IMMEDIATE, when off).
- **Point light shadow maps** without ray tracing (4 lights, 6 faces each, at most 1024 px).
- **Custom shaders** (`HRL_CreateShader`), compiled at run time with glslang: surface / vertex functions for meshes (lighting, shadows, ray tracing and picking kept), OpenGL 3.30 post-process shaders as they are. Material parameters come from the `uniform` declarations. New CMake option `HRL_VULKAN_RUNTIME_SHADERS` (ON; needs the glslang component of the Vulkan SDK).
- **Built-in post-process controls**: brightness, contrast, saturation, hue shift, tint, invert, vignette, film grain, fade, sharpen, chromatic aberration, `toneMappingEnabled` (0: colours shown as they are).
- **Screen space effects**: decals, screen space reflections, ambient occlusion, local and global volumetric fog, god rays, volumetric clouds. They run the shaders of the OpenGL backend, compiled at run time, on depth and normals given in the OpenGL conventions, so both backends give the same image.
- **Screen space displacement materials**.
- **VFX particles**: billboards, stretched billboards, mesh particles, alpha / additive / multiply blending.
- **2D voxel worlds**: the chunk geometry of the core, type textures (bindless), emissive voxels and the light they give (`HRL_SetVoxelEmissiveLighting`), edits and streaming.
- **Global illumination**: `HRL_GI_DDGI`, the irradiance probe volume of `core/gi_probes.cpp` (CPU traced, uploaded and sampled by every lit shader). SSGI, VCT and LPV stay OpenGL only.
- Tests `vulkan_2d`, `vulkan_custom`, `vulkan_effects` (screenshots checked pixel by pixel), anti-aliasing and point light shadows in `vulkan_smoke`.

### Fixed (Vulkan)

- Textures created from bitmaps (`HRL_CreateTextureAsync`, texts) were upside down compared with `HRL_CreateTexture` and the OpenGL backend.

## Native Vulkan backend, OpenGL deprecation (API 0.9)

### Changed (breaking for `HRL_VULKAN`)

- `HRL_VULKAN` is now a **native Vulkan 1.3 renderer** (`src/backend/vulkan/`). The OpenGL-on-Vulkan layer (`vkgl`) and the copies of the OpenGL renderer compiled over it are removed.
- The native backend covers 3D only for now: static meshes, LOD, PBR materials, lights, shadows, image based lighting, sky, fog, tone mapping, picking, screenshots, debug primitives. Sprites, widgets, VFX, voxels, skeletal meshes, landscapes, post-process materials, the OpenGL GI methods and `HRL_CreateShader` report `HRL_INVALID_BACKEND_OPERATION` (warning, once per feature) on Vulkan. Projects that need them stay on `HRL_OPENGL_33` for the moment. See the feature table in the README (skeletal meshes and landscapes were added since, see below).
- Vulkan requires a Vulkan 1.3 driver. Shaders are compiled to SPIR-V when HRL is built; nothing is compiled at run time and `glslangValidator` is no longer needed on the user's machine.
- CMake: `HRL_ENABLE_VULKAN` (ON by default) replaces `COMPILE_VULKAN` (still accepted). CMake 3.20 is enough. The hard coded Vulkan SDK path is gone (`VULKAN_SDK` environment variable or `-DHRL_VULKAN_SDK=`). New options `HRL_BUILD_STATIC`, `HRL_BUILD_EXAMPLES`, `HRL_BUILD_TESTS`.

### Deprecated

- **OpenGL 3.3 backend** (`HRL_OPENGL_33`, `hrl_gl.h`): frozen, bug fixes only. `HRL_Init(HRL_OPENGL_33)` reports a weak warning. Its behaviour is unchanged.

### Added

- Vulkan: cascaded shadow maps (4 cascades), spot light shadow maps, hardware ray traced shadows for every light type (`HRL_Vulkan_SetRayTracedShadows`, `HRL_Vulkan_IsRayTracingSupported`), image based lighting from the sky texture or gradient, emissive materials, off-screen rendering without a window, GPU name (`HRL_Vulkan_GetDeviceName`), `HRL_Vulkan_SetShadowDistance`.
- `HRL_MaterialSetEmissiveColor` is implemented (it was an empty function).
- `examples/vulkan_pbr`, `tests/vulkan_smoke.cpp` (run with `ctest`), `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`.
- New example `hrl_editor`: scene editor with a Raven interface, HRL rendering with Vulkan in a native viewport of Raven. No change to the library.
- The examples are part of the main CMake project (`-DHRL_BUILD_EXAMPLES=ON`): they link the `hrldll` target and include `src/` directly. The library and the executables are written to `build/bin`. The former `example/` folder (copied headers, copied `libhrldll`, prebuilt GLFW, copy of GLM) is replaced by `examples/opengl/` (targets `hrl_gl_showcase`, `hrl_gl_minimal`, `hrl_gl_vfx`, `hrl_gl_voxel`, `hrl_gl_widgets`); GLFW is downloaded by CMake when it is not installed. Their resources live in `examples/assets` (not versioned).
- Third party: Vulkan Memory Allocator 3.1.0 (`third_party/vma`).

### Added (Vulkan rendering)

- Translucent materials: `HRL_MATERIAL_PARAM_TRANSLUCENT`, alpha blended, drawn back to front after the opaque scene and the sky.
- Alpha tested shadow casters (the holes of a masked material no longer cast a shadow).
- Bloom: enabled by a post-process material on a viewport (`bloomStrength`, default 1), computed on a chain of pooled half resolution images.
- Skeletal meshes: skinned by a compute pass into the shared vertex buffer, only when their pose changes; lighting and shadow maps then treat them as any mesh. They are not part of the ray traced scene yet (they cast no ray traced shadow).
- Pipeline cache on disk (`HRL_Vulkan_SetPipelineCachePath` or `HRL_VULKAN_PIPELINE_CACHE`).
- Frame preparation (LOD selection, culling for the camera and each shadow view) on worker threads (`HRL_VULKAN_THREADS`).
- `HRL_Vulkan_GetMemoryUsage` (GPU memory used / budget, `VK_EXT_memory_budget` when available).
- The device now requires the `independentBlend` feature.

### Added (landscapes, foliage, GPU driven rendering)

- Vulkan landscapes: CDLOD (quadtree, morphing, no cracks), 32-bit float heights, per pixel normals, ray traced shadows. New functions `HRL_CreateLandscapeFromHeights`, `HRL_SetLandscapeHeights`, `HRL_UpdateLandscapeHeights` (rectangle), `HRL_GetLandscapeHeightsSize`, `HRL_GetLandscapeHeightAt`, `HRL_GetLandscapeNormalAt`, `HRL_SetLandscapeLODDistance`. Landscapes can be picked (`HRL_LANDSCAPE`).
- Foliage (Vulkan): `HRL_CreateFoliage`, `HRL_AddFoliageMesh` (several parts, one material each), `HRL_AddFoliageInstances`, `HRL_ScatterFoliage` (on a landscape or a 3D mesh: density, scale, slope, height, alignment, spacing), `HRL_PaintFoliage`, `HRL_EraseFoliage`, `HRL_ClearFoliage`, `HRL_GetFoliageInstances`, draw / LOD / impostor distances, shadows, wind response. Culled and LOD-selected on the GPU with dithered cross-fades, octahedral impostors baked automatically, wind (`HRL_SetWind`) and interaction (`HRL_SetFoliageInteractor`), ray traced shadows alpha tested up to the impostor distance (100 m at most). Picking returns `HRL_FOLIAGE`. `HRL_Vulkan_GetFoliageStats`.
- Procedural grass (Vulkan): `HRL_CreateGrass` on a landscape, density, blade size, colours, distance, maximum slope, mask texture, shadows. Generated on the GPU, no per-blade memory.
- `HRL_CreateTextureFromPixels` (RGBA8 from memory).
- `HRL_Vulkan_SetOcclusionCulling` / `HRL_Vulkan_GetOcclusionCulling`.
- Example `hrl_vulkan_nature`, test `vulkan_nature` (with and without ray tracing).

### Changed (performance)

- Vulkan meshes are culled and LOD-selected on the GPU (compute), with two-phase Hi-Z occlusion culling, and drawn with `vkCmdDrawIndexedIndirectCount`. The device now requires the `drawIndirectCount` feature (all Vulkan 1.3 desktop GPUs of the supported range have it).
- The scene data cache is event driven (mesh list revision, list of moved meshes) instead of comparing every mesh each frame. `hrl_bench_scene`, 20 000 meshes: `HRL_EndFrame` CPU time 1.5 ms → 0.09 ms; 1 000 000 foliage instances: 0.17 ms per frame, 0.31 ms while painting every frame (lavapipe, x86-64).
- `HRL_GetMeshLODLevel` on Vulkan reports the LOD chosen by the GPU two frames earlier.

### Limitations

- The OpenGL 3.3 backend does not render foliage and grass, and only uses 8-bit heightmap landscapes (float heights are ignored there).
- `HRL_GetLandscapeHeightAt` ignores the pitch and roll of the landscape.
- Foliage beyond 100 m and grass are not in the ray traced scene (grass shadows come from the cascades).

### Architecture

- `src/hrl.cpp` (10 600 lines) is split into `src/frontend/*.cpp`, one file per domain, with a private shared header. No behaviour change: the OpenGL regression test renders the same image before and after.
- Objects are stored in `HRL_IdMap` (flat array sorted by id) instead of hash maps: contiguous iteration in creation order.
- New backend entry point `RHI_RenderSceneFrame(const HRL_SceneFrame&)`: the frame of a scene (scene, materials, debug primitives, screenshot request) arrives in one structure. The Vulkan backend no longer reads the frontend state (`backend_isolation` test); the OpenGL backends keep the legacy entry points.
- Vulkan renderer split into modules (`vk_frame_graph`, `vk_pipelines`, `vk_resources`, `vk_scene_data`, `vk_environment`, `vk_renderer`).
- Vulkan frame graph: passes declare their images; barriers, rendering scopes and debug labels are generated; the HDR target is a pooled transient image.
- Vulkan scene data cache: instances and materials are kept between frames and only what changed is recomputed (`HRL_Material::revision_`, transforms compared by value). 20 000 meshes: 4.2 ms → 1.5 ms of CPU per frame (`hrl_bench_scene`, software GPU).
- `HRL_Vulkan_GetSceneStats` (instances, materials, drawn instances, passes, cache work).
- Tests: `opengl_smoke` (regression of the deprecated backend, needs a display), `backend_isolation`, `hrl_bench_scene`.

### Removed

- `src/hrl.cpp` (replaced by `src/frontend/`).
- `src/backend/vulkan/vkgl/`, `vk_gl33_*.cpp`, `vk_core.*`, `vk_context.*`, `vk_compile.*`, `vulkan_shader.*`, `vulkan_texture.*`, `vulkan_builtin_shaders.h`, the old `src/shaders/vulkan/*.glsl`.
- Empty placeholders: `src/backend/d3d11/`, `src/shaders/d3d/`, `src/shaders/metal/`.

## Language bindings

### Added

- `bindings/generate.py` : generator of language bindings from the C headers (Python standard library only). It writes `bindings/hrl_api.json` (description of the whole API) and bindings for Python (ctypes package), C# (P/Invoke, .NET 6+), Rust (crate with raw FFI and safe wrappers), Go (cgo) and LuaJIT (FFI), plus a C program checking the generated description against the compiler.
- Strings, byte buffers, arrays with their count, out parameters, matrices and callbacks are converted automatically in every language. Examples in each folder.
- `HRL_AddScreenMessageText` : non-variadic version of `HRL_AddScreenMessage`, for bindings.

### Removed

- SWIG Python bindings (`hrl.i`, `hrl_python.i`, `generate-python.bat`).

## Global illumination

### Added

- Six working global illumination methods, in OpenGL 3.3 and Vulkan : `HRL_GI_SSGI`, `HRL_GI_VCT`, `HRL_GI_LPV`, `HRL_GI_DDGI`, `HRL_GI_RAY_TRACING` and `HRL_GI_PATH_TRACING` (see the README). Before, only DDGI was accepted and it barely changed the image (probes on the walls, light leaks, arbitrary strength).
- GPU ray tracing in shaders on a SAH BVH of the scene (static meshes LOD 0 + landscapes), shared by DDGI, voxel lighting, ray traced GI and path tracing.
- `HRL_SetGlobalIlluminationIntensity` / `HRL_GetGlobalIlluminationIntensity`.

### Changed

- The indirect light is added by a screen pass from the GI G-buffer (diffuse albedo, normal, depth) instead of the mesh shader, so it also applies to skinned meshes, landscapes and voxels.
- The GI albedo buffer (`HRL_GL_GetSceneAlbedoBufferGL_ID`) now holds the diffuse albedo (metallic removed), as documented.
- `core/gi_probes.*` (CPU probe tracing) replaced by `core/gi_scene.*` (BVH, voxelization, probe layout).

### Fixed

- Vulkan shader translation of functions with `out` parameters and of `layout(...)` uniform blocks.

## Vulkan backend (API 0.8)

### Added

- **Complete Vulkan backend** (`HRL_VULKAN`). It runs the same renderer as the OpenGL 3.3 backend over a new OpenGL-on-Vulkan layer (`src/backend/vulkan/vkgl/`), so every OpenGL 3.3 feature is available with Vulkan and both backends render the same images: PBR, lights and shadows, fog and volumetric fog, sky sphere, clouds, ambient occlusion, god rays, post-processing and post-process materials, GI/DDGI, MSAA, sprites, skeletal meshes, LOD, landscapes, 2D voxels, VFX, debug rendering and debug views, widgets, gizmos, screen messages, screenshots and color picking.
- The Vulkan backend presents the frame itself in `HRL_EndFrame` (swapchain created from the `hrl_vulkan.h` surface callback) and renders off-screen when no surface callback is given.
- `HRL_GetHoveredObject` is now implemented through the backend table (OpenGL 3.3 and Vulkan).
- `HRL_VULKAN_VALIDATION=1` enables the Khronos validation layer.
- `CHANGELOG.md`.

### Changed

- Shared CPU-side rendering code (voxel meshing, light/LOD/frustum helpers, GI probe placement, built-in GLSL sources) moved to `src/core/` and `src/backend/common/` so every backend takes the same decisions.
- The Vulkan backend uses the OpenGL GLSL shaders (built-in and `HRL_CreateShader` sources) directly; the separate `src/shaders/vulkan/` shader set is no longer needed.
- `bool` uniforms are stored as `uint` in the translated Vulkan shaders (portable across drivers).
- The Vulkan backend is only compiled with `-DCOMPILE_VULKAN=ON`; without it HRL builds and links without the Vulkan SDK (`HRL_Init(HRL_VULKAN)` then reports `HRL_INVALID_BACKEND_OPERATION`).
- README: backend table uses the real `HRL_*` constants and shows which backends are available, feature comparison in English for OpenGL 3.3 and Vulkan (with 2D voxels), Vulkan setup section, typo fixes.

### Fixed (previous Vulkan backend)

- Uniform buffer reallocated in the middle of a frame (use after free on the GPU).
- Vertex shader outputs matched to fragment inputs by declaration order instead of name.
- Built-in Vulkan shaders read from disk at runtime.
- Scene render targets stored in 8-bit (no HDR), wrong UV attribute format.
- Debug rendering, post-processing, fog, shadows, GI and many other passes missing.
- `hrl.cpp` required the Vulkan SDK even when the Vulkan backend was not compiled.
