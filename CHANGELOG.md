# Changelog

## Native Vulkan backend, OpenGL deprecation (API 0.9)

### Changed (breaking for `HRL_VULKAN`)

- `HRL_VULKAN` is now a **native Vulkan 1.3 renderer** (`src/backend/vulkan/`). The OpenGL-on-Vulkan layer (`vkgl`) and the copies of the OpenGL renderer compiled over it are removed.
- The native backend covers 3D only for now: static meshes, LOD, PBR materials, lights, shadows, image based lighting, sky, fog, tone mapping, picking, screenshots, debug primitives. Sprites, widgets, VFX, voxels, skeletal meshes, landscapes, post-process materials, the OpenGL GI methods and `HRL_CreateShader` report `HRL_INVALID_BACKEND_OPERATION` (warning, once per feature) on Vulkan. Projects that need them stay on `HRL_OPENGL_33` for the moment. See the feature table in the README.
- Vulkan requires a Vulkan 1.3 driver. Shaders are compiled to SPIR-V when HRL is built; nothing is compiled at run time and `glslangValidator` is no longer needed on the user's machine.
- CMake: `HRL_ENABLE_VULKAN` (ON by default) replaces `COMPILE_VULKAN` (still accepted). CMake 3.20 is enough. The hard coded Vulkan SDK path is gone (`VULKAN_SDK` environment variable or `-DHRL_VULKAN_SDK=`). New options `HRL_BUILD_STATIC`, `HRL_BUILD_EXAMPLES`, `HRL_BUILD_TESTS`.

### Deprecated

- **OpenGL 3.3 backend** (`HRL_OPENGL_33`, `hrl_gl.h`): frozen, bug fixes only. `HRL_Init(HRL_OPENGL_33)` reports a weak warning. Its behaviour is unchanged.

### Added

- Vulkan: cascaded shadow maps (4 cascades), spot light shadow maps, hardware ray traced shadows for every light type (`HRL_Vulkan_SetRayTracedShadows`, `HRL_Vulkan_IsRayTracingSupported`), image based lighting from the sky texture or gradient, emissive materials, off-screen rendering without a window, GPU name (`HRL_Vulkan_GetDeviceName`), `HRL_Vulkan_SetShadowDistance`.
- `HRL_MaterialSetEmissiveColor` is implemented (it was an empty function).
- `examples/vulkan_pbr`, `tests/vulkan_smoke.cpp` (run with `ctest`), `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`.
- The examples are part of the main CMake project (`-DHRL_BUILD_EXAMPLES=ON`): they link the `hrldll` target and include `src/` directly. The library and the executables are written to `build/bin`. The former `example/` folder (copied headers, copied `libhrldll`, prebuilt GLFW, copy of GLM) is replaced by `examples/opengl/` (targets `hrl_gl_showcase`, `hrl_gl_minimal`, `hrl_gl_vfx`, `hrl_gl_voxel`, `hrl_gl_widgets`); GLFW is downloaded by CMake when it is not installed. Their resources live in `examples/assets` (not versioned).
- Third party: Vulkan Memory Allocator 3.1.0 (`third_party/vma`).

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
