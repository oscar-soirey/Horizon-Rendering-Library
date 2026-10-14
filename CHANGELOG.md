# Changelog

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
