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

Then the parity with OpenGL: sprites, widgets, texts, gizmos (2D overlays
built by the frontend), VFX particles, 2D voxel worlds with the light of the
emissive voxels, custom shaders (surface / vertex functions, OpenGL
post-process shaders, compiled at run time with glslang), the screen space
effects of OpenGL (decals, SSR, SSAO, volumetric fog, god rays, clouds:
their shaders compiled at run time), screen space displacement, FXAA and TAA,
VSync control, point light shadow maps, DDGI probes.

## Next steps

1. **3D**: acceleration structures of skinned meshes (refit), landscape layers
   (splat maps) and holes; voxel worlds in the shadow maps.
2. **Temporal foundation**: motion vectors (moving objects in the TAA), thin
   G-buffer written by the forward pass (normals instead of the ones rebuilt
   from the depth for the effects).
3. **Lighting scale**: clustered light lists (forward+).
4. **Ray tracing**: reflections, then diffuse GI with probes traced with
   hardware rays (the DDGI probes are CPU traced today); SSGI / VCT / LPV are
   not planned on Vulkan. BLAS compaction and refit for skinned meshes.
5. **Frontend**: objects are still owned through raw pointers created and
   deleted by the frontend; the OpenGL backends still read the frontend context.
6. **GPU driven** (culling, LOD selection, Hi-Z occlusion, draw-indirect-count:
   done). Remaining: packed vertex format; texture streaming driven by
   `HRL_Vulkan_GetMemoryUsage`; dedicated transfer queue for uploads; foliage
   in the ray traced scene beyond 100 m (impostor proxies); foliage and grass
   in point / spot light shadow maps; grass ray traced shadows.
7. **Frame graph**: pass culling and reordering, transient aliasing inside a
   frame, compute passes as first class citizens; the screen space effects
   ping-pong through copies today.

Rust: no component currently justifies a second language and an FFI boundary.
The asset pipeline (texture compression, mesh processing) would be the first candidate.

Not planned: virtualized geometry and virtual texturing, until content at that
scale exists to measure them against.
