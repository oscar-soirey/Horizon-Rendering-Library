# HRL roadmap

State after the 0.9 Vulkan rewrite, then the planned order. Each step builds
on the previous ones; nothing is listed as done unless it runs in the tests.

## Audit summary (0.8)

| Area | Finding | Action |
|-|-|-|
| Public C API | Coherent, opaque ids, backend independent | Kept |
| Vulkan backend | OpenGL renderer compiled over an OpenGL-on-Vulkan layer (Vulkan 1.1, one command buffer, shaders compiled by spawning `glslangValidator` at run time) | Replaced by a native renderer |
| OpenGL 3.3 | Complete forward renderer, single shadow map per light, no IBL, GI by software BVH in fragment shaders | Deprecated, frozen |
| Frontend | `hrl.cpp` was one 10 000 line file with a global context of `unordered_map`s of raw pointers; backends read it directly | Split into `src/frontend/`, flat id storage, Vulkan isolated behind `HRL_SceneFrame` |
| D3D11 / Metal | Empty files | Removed |
| Build | Hard coded SDK path, CMake 3.31 required, no tests | Fixed, tests added |
| `HRL_MaterialSetEmissiveColor` | Declared, empty body | Implemented (Vulkan) |

## Done in 0.9 (Vulkan)

Device layer, geometry pool, bindless textures, multi-draw-indirect, PBR,
cascaded and spot shadow maps, ray traced shadows (ray query), image based
lighting, sky, fog, ACES tone mapping, LOD, picking, screenshots, debug
primitives, off-screen rendering, tests, example. Architecture: frontend split,
frame graph, scene data cache, backend isolation (see `ARCHITECTURE.md`).

Then: skeletal meshes, translucency, alpha tested shadows, bloom, pipeline
cache; GPU driven culling and LOD with two-phase Hi-Z occlusion; CDLOD
landscapes; foliage (GPU culled instances, LOD cross-fades, impostors, wind,
interaction, scatter / paint / erase, ray traced alpha tested shadows);
procedural grass.

## Next steps

1. **3D parity** (done: skeletal meshes, translucency, alpha tested
   shadows, landscapes). Remaining: point light shadow maps for GPUs without
   ray tracing, acceleration structures of skinned meshes (refit), landscape
   layers (splat maps) and holes.
2. **Custom materials**: surface functions (see `ARCHITECTURE.md`), which needs
   a run time GLSL → SPIR-V compiler linked into HRL (glslang) and a pipeline cache.
3. **Temporal foundation**: thin G-buffer (normal, roughness, motion vectors),
   depth pre-pass, TAA, then the custom post-process material chain (bloom is done).
   MSAA is not planned on Vulkan.
4. **Lighting scale**: clustered light lists (forward+), point light shadow
   maps for GPUs without ray tracing, SSAO, SSR.
5. **Ray tracing**: reflections, then diffuse GI with probes (DDGI) traced with
   hardware rays, with a screen space fallback; denoising relies on step 3.
   BLAS compaction and refit for skinned meshes.
6. **Frontend** (done): `hrl.cpp` split by domain, flat id storage, scene data
   cache with change tracking, Vulkan backend isolated from the frontend state.
   Remaining: objects are still owned through raw pointers created and
   deleted by the frontend; the OpenGL backends still read the frontend context.
7. **GPU driven** (culling, LOD selection, Hi-Z occlusion, draw-indirect-count:
   done). Remaining: packed vertex format; texture streaming driven by
   `HRL_Vulkan_GetMemoryUsage`; dedicated transfer queue for uploads; foliage
   in the ray traced scene beyond 100 m (impostor proxies); foliage and grass
   in point / spot light shadow maps; grass ray traced shadows.
8. **Frame graph** (first version done): passes, automatic barriers,
   transient pool. To add with the passes of steps 3 to 5: pass culling and
   reordering, transient aliasing inside a frame, compute passes as first
   class citizens.
9. **2D and tools**: sprites, text, widgets, gizmos, VFX, voxel worlds on Vulkan.

Rust: no component currently justifies a second language and an FFI boundary.
The asset pipeline (texture compression, mesh processing) would be the first candidate.

Not planned: virtualized geometry and virtual texturing, until content at that
scale exists to measure them against.
