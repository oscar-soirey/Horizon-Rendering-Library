# HRL roadmap

State after the 0.9 Vulkan rewrite, then the planned order. Each step builds
on the previous ones; nothing is listed as done unless it runs in the tests.

## Audit summary (0.8)

| Area | Finding | Action |
|-|-|-|
| Public C API | Coherent, opaque ids, backend independent | Kept |
| Vulkan backend | OpenGL renderer compiled over an OpenGL-on-Vulkan layer (Vulkan 1.1, one command buffer, shaders compiled by spawning `glslangValidator` at run time) | Replaced by a native renderer |
| OpenGL 3.3 | Complete forward renderer, single shadow map per light, no IBL, GI by software BVH in fragment shaders | Deprecated, frozen |
| Frontend | `hrl.cpp` is one 10 000 line file with a global context of `unordered_map`s of raw pointers; backends read it directly | To split (step 6) |
| D3D11 / Metal | Empty files | Removed |
| Build | Hard coded SDK path, CMake 3.31 required, no tests | Fixed, tests added |
| `HRL_MaterialSetEmissiveColor` | Declared, empty body | Implemented (Vulkan) |

## Done in 0.9 (Vulkan)

Device layer, geometry pool, bindless textures, multi-draw-indirect, PBR,
cascaded and spot shadow maps, ray traced shadows (ray query), image based
lighting, sky, fog, ACES tone mapping, LOD, picking, screenshots, debug
primitives, off-screen rendering, tests, example.

## Next steps

1. **3D parity**: skeletal meshes (compute skinning into the geometry pool, so
   shadows and ray tracing see the skinned mesh), landscapes, alpha blended
   materials, shadows of alpha tested geometry.
2. **Custom materials**: surface functions (see `ARCHITECTURE.md`), which needs
   a run time GLSL → SPIR-V compiler linked into HRL (glslang) and a pipeline cache.
3. **Temporal foundation**: thin G-buffer (normal, roughness, motion vectors),
   depth pre-pass, TAA, then bloom and the post-process material chain.
   MSAA is not planned on Vulkan.
4. **Lighting scale**: clustered light lists (forward+), point light shadow
   maps for GPUs without ray tracing, SSAO, SSR.
5. **Ray tracing**: reflections, then diffuse GI with probes (DDGI) traced with
   hardware rays, with a screen space fallback; denoising relies on step 3.
   BLAS compaction and refit for skinned meshes.
6. **Frontend**: split `hrl.cpp` by domain, replace the per-frame material
   and instance rebuild with dirty tracking, handle-based storage instead of
   hash maps of pointers.
7. **GPU driven**: culling and LOD selection in compute, Hi-Z occlusion,
   draw-indirect-count; packed vertex format; texture streaming with a VRAM budget.
8. **Render graph**: once steps 3 to 5 add enough passes to justify it, replace
   the explicit pass sequence by a graph that owns transient images and barriers.
9. **2D and tools**: sprites, text, widgets, gizmos, VFX, voxel worlds on Vulkan.

Rust: no component currently justifies a second language and an FFI boundary.
The asset pipeline (texture compression, mesh processing) would be the first candidate.

Not planned: virtualized geometry and virtual texturing, until content at that
scale exists to measure them against.
