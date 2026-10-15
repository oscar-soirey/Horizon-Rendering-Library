# Changelog

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
