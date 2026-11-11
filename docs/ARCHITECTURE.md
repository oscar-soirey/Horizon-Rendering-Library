# HRL architecture

## Layers

```
application ── hrl.h (C API, opaque ids) ── hrl_vulkan.h / hrl_gl.h (backend specific)
                    │
          src/frontend/*.cpp     implementation of the public API: validates calls,
                                 owns the scene description (one file per domain)
          src/core/*             CPU systems shared with the backends (LOD, culling
                                 helpers, VFX simulation...), object types, id_map.h
                    │  HRL_vtable (core/backend_vtable.h) + HRL_SceneFrame (core/backend_api.h)
        ┌───────────┴────────────┐
 backend/vulkan (primary)   backend/opengl33, backend/gles3 (deprecated, frozen)
```

### Frontend

`src/frontend/` holds the public API, split by domain (`context`, `scene`,
`camera`, `mesh`, `skeletal`, `landscape`, `light`, `material`, `texture`,
`fbx`, `effects`, `vfx`, `voxel`, `gizmo`, `widget`, `debug`).
`frontend_internal.h` is their shared private header: the frontend state
(`ctx_`), the selected backend (`g_Backend`) and the few helpers used by
several files. Nothing outside `src/frontend` includes it.

Objects are stored in `HRL_IdMap` (`core/id_map.h`): a flat array of
(id, object) sorted by id. Ids only grow, so creation appends; iteration is
contiguous and follows the creation order, which makes light order, viewport
order and draw order deterministic.

### Frontend / backend boundary

The frontend never calls a graphics API. A backend receives resource calls
through `HRL_vtable` (meshes, textures, scenes) and, once per frame and per
scene, one `HRL_SceneFrame`: the scene description, the material table, the
debug primitives of the frame and the optional screenshot request. The
interface is deliberately high level, so a backend chooses the structure that
suits its API.

The Vulkan backend is isolated from the frontend: from the core it only
includes `backend_api.h`, `render_common.h` and `object_types.h`, and never
reads the frontend state. The `backend_isolation` test enforces it. The
deprecated OpenGL backends keep their historical access to the frontend
context (`utils_functions.h`) and the legacy entry points
(`RHI_RenderScene`, `RHI_DrawDebug`, `RHI_TakeScreenshot`).

Change tracking is explicit where the frontend is the only writer:
`HRL_Material::revision_` is incremented by every material setter that really
changes a value.

## Vulkan backend

### Device layer (`vk_device`)

* **Frames in flight**: 2. Each has a command pool, a descriptor pool reset
  every frame and a linear host-visible buffer for per-frame data (view
  constants, instances, materials, lights, indirect commands).
* **Uploads never block**: mesh and texture data are copied to staging buffers
  and recorded in an upload command buffer that is submitted in front of the
  next frame. Above 256 MB of pending staging data (loading screen) it is
  flushed synchronously.
* **Deferred destruction**: `Device::Defer` tags a destructor with the last
  frame that may use the resource; it runs once that frame has completed.
* **Memory**: Vulkan Memory Allocator.
* **Synchronisation**: `synchronization2` barriers derived from tracked image
  layouts. The backend is validated with the Khronos validation layer,
  including synchronisation validation, by `tests/vulkan_smoke.cpp`.

### Files

| File | Role |
|-|-|
| `vk_device` | Device layer (above) |
| `vk_jobs` | Worker threads for the CPU side of a frame (LOD selection, culling) |
| `vk_frame_graph` | Pass declaration, automatic barriers and rendering scopes, transient image pool |
| `vk_pipelines` | Descriptor set layouts and pipelines |
| `vk_resources` | Bindless textures, geometry pool, bottom level acceleration structures |
| `vk_scene_data` | Cached instances / materials, draw records, lights, cameras, shadow cascades |
| `vk_culling` | GPU culling and LOD selection of meshes, Hi-Z pyramids, statistics read back |
| `vk_landscape` | CDLOD landscapes: height textures, quadtree, node selection, ray traced geometry |
| `vk_foliage` | Foliage cells, GPU culling / LOD / cross-fades, draw commands, ray traced instances |
| `vk_impostor` | Baking and drawing of octahedral impostors |
| `vk_grass` | Procedural grass tiles around the camera |
| `vk_environment` | Image based lighting |
| `vk_renderer` | Render targets, frame of a scene, picking, screenshots, anti-aliasing |
| `vk_overlay` | Gizmos, widgets, texts and messages over the viewports |
| `vk_custom` | Custom shaders: run time GLSL compiler (glslang), surface functions, OpenGL post-process shaders |
| `vk_effects` | Screen space effects and custom post-processes |
| `vk_vfx` | Particles |
| `vk_voxel` | 2D voxel worlds, light of the emissive voxels |
| `vk_gi` | Global illumination probes |
| `vulkan_backend` | `HRL_vtable` and the `hrl_vulkan.h` functions |

`vk_renderer_internal.h` is the private header shared by these files.

### Data model

* **Geometry pool**: every mesh LOD is a range of one vertex buffer and one
  index buffer (sub-allocated with a VMA virtual block, grown on demand).
* **Bindless textures**: one descriptor array; each texture occupies two
  entries (UNORM view and sRGB view of the same image), so the material decides
  whether a map is colour data. Materials reference textures by index.
* **Scene data** (`SceneData`, one per scene): instances (transform, material
  index, object id), packed materials and the draw list, kept between frames.
  Each frame it is compared with its sources and only what changed is
  recomputed: the list of meshes (a mismatch rebuilds everything), each mesh
  transform (compared by value), `HRL_Material::revision_`, and revision
  counters of the renderer mesh and texture tables. The arrays are copied to
  the per-frame buffer; shaders index them with `gl_InstanceIndex`.
  Measured with `hrl_bench_scene` (20 000 meshes, 500 materials, CPU time of
  `HRL_EndFrame`, Mesa lavapipe, x86-64): 4.2 ms before the cache and the flat
  object storage, 1.5 ms after.
* **Event driven updates**: the frontend increments `mesh_revision` when the
  list of meshes changes and records moved meshes (`moved_meshes`), so a
  frame with nothing changed does no per-mesh CPU work at all.
* **Draw records**: one record per mesh part and LOD (index range, material,
  bucket: opaque / alpha tested / translucent × one / two sided). An instance
  points to its LOD range of records.

### GPU driven culling (`vk_culling`)

The CPU no longer chooses what is drawn. For each view a compute pass reads
every instance, selects its LOD (screen size with hysteresis, the rule of the
OpenGL backend), tests it against the frustum and the Hi-Z pyramid, then
appends `VkDrawIndexedIndirectCommand`s per bucket; the draw passes issue one
`vkCmdDrawIndexedIndirectCount` per bucket.

Occlusion culling is done in two phases, so it never shows a hole:

1. instances visible last frame are drawn (state buffer, bit per instance);
2. a Hi-Z pyramid (reversed-Z: each level keeps the farthest depth) is built
   from that depth;
3. the other instances are tested against it and the newly visible ones drawn.

Shadow views use frustum culling only. Statistics (instances drawn, LOD of
each mesh for `HRL_GetMeshLODLevel`) are read back two frames later, without
waiting. `HRL_Vulkan_SetOcclusionCulling(0)` keeps frustum culling only.
`hrl_bench_scene` (20 000 meshes, 500 materials, CPU time of `HRL_EndFrame`,
lavapipe): 4.2 ms with the 0.8 storage, 1.5 ms with the scene cache and CPU
culling, 0.09 ms GPU driven.

### Landscapes (`vk_landscape`)

Heights are a 32-bit float texture (bindless) displaced in the vertex shader.
The landscape is a quadtree of 32 × 32 quad patches (CDLOD): nodes are chosen
per view on the CPU from their min / max heights (a few hundred nodes for a
1 km terrain), the vertices morph between levels so there are no cracks or
pops. Normals are computed per pixel from the height texture. Edits
(`HRL_UpdateLandscapeHeights`) update the rectangle in the texture and the
min / max tree. With ray tracing, the landscape is a full resolution BLAS; the
shadow ray starts from the position of the real triangle to avoid acne.

### Foliage (`vk_foliage`, `vk_impostor`)

A foliage kind is a set of parts (mesh + material), with automatic LODs. Its
instances (32 bytes: position, scale, quaternion) live in cells of 32 m,
allocated in one GPU pool with a VMA virtual block; painting or erasing
re-uploads only the touched cells, and the table of cells is cached on the GPU
(`instances_revision`).

Per view: a compute pass (one workgroup per cell, cell bounds tested first)
culls each instance (frustum, Hi-Z in phase 2), picks LOD 0 / 1 / 2 / impostor
by distance and writes it to one of four visible lists. Around a LOD
boundary an instance is written in both lists with complementary dithered
coverage, giving a cross-fade without blending. A second small pass writes
the indirect commands. Wind (direction, strength, gusts) and up to 16
interactors (spheres that push plants away) are applied in the vertex shader
from the view uniform, so they cost nothing on the CPU.

Impostors are baked once per foliage kind: 8 × 8 hemi-octahedral views of
128 px (albedo with coverage, normal) with mipmaps. The impostor quad blends
the 4 nearest views and is lit as the real mesh.

Ray traced shadows: foliage within the impostor distance (100 m at most,
65 536 instances at most, nearest first) is added to the TLAS with one BLAS
per part and LOD, built from the rest pose (no wind), as non-opaque
instances. The shadow ray query alpha tests them against their material
(geometry table and vertex / index pools bound as storage buffers).

### Grass (`vk_grass`)

No per-blade data: a grid of 4 m tiles around the camera is culled in compute
(frustum, Hi-Z, landscape slope / mask), then each visible tile is one instanced
draw where the vertex shader generates the blades from a hash of their
position (15 vertices near, 3 far, density decreasing with distance). Grass
casts shadows in the two nearest cascades.

### Frame graph

The frame of a scene is declared as passes (`vk_frame_graph`). A pass names
the images it samples, copies or renders to; the graph inserts the layout
transitions, opens the dynamic rendering scope of raster passes, labels the
pass for RenderDoc and hands out transient images from a pool (the HDR colour
target is one: every scene of the same size shares it). CPU work (culling,
descriptor sets) is done while the passes are declared; GPU commands are
recorded by `FrameGraph::Execute`.

```
[brdf lut] [environment] [skinning] [impostor bake] [tlas build]   when needed
per viewport:  cull phase 1 → shadow map × N → forward phase 1 (meshes, landscapes)
               → Hi-Z → cull phase 2 (meshes, foliage, grass) → forward phase 2
               (meshes, foliage, impostors, grass, sky, translucent)
               → [screen space displacement]
[TAA resolve] → [screen space effects, custom post-processes] → [bloom downsample × N → upsample × N]
→ tone mapping → [FXAA] → [debug] → [overlays: gizmos, widgets, texts] → [screenshot] → [present copy]
```

Passes run in declaration order and none is culled: no pass is optional yet.
Reordering and culling belong with the first optional passes (SSAO, SSR...).

* **Shadows**: 4 stable cascades for the first shadow casting directional
  light (bounding sphere per slice, texel snapping, blended splits, 3×3 PCF)
  and up to 4 spot light maps, in one depth array. Skipped with ray traced
  shadows, where every shadow casting light (point lights included) traces a
  ray query against the scene.
* **Forward pass**: HDR colour (RGBA16F) + object id (R32UI), reversed-Z
  depth (D32F). Metallic-roughness BRDF (GGX, height correlated Smith,
  Schlick), image based lighting with multiple scattering compensation, fog,
  then the sky where the depth buffer is still empty.
* **Skinning**: a compute pass writes the skinned vertices of each skeletal
  mesh whose pose changed into its range of the vertex pool. Every later pass
  then draws it as a static mesh (they are not occlusion culled, their bounds
  change). Skinned meshes have no acceleration structure yet.
* **Translucency**: translucent buckets are drawn after the sky, back to front,
  with alpha blending and no depth write; they cast no shadow.
* **Bloom**: when the scene has a post-process material, the HDR image is
  downsampled level by level (first level weighted against fireflies) then
  upsampled and accumulated; tone mapping mixes it with the image.
* **Tone mapping** (ACES) into the LDR scene image, then debug primitives, the
  screenshot copy and the blit to the swapchain for on-screen scenes.

Forward shading was chosen over a full G-buffer: materials are evaluated once,
alpha tested and (later) transparent surfaces follow the same path, and
user surface functions plug into one place. Screen space and ray traced
effects will read a thin G-buffer (normal, roughness, motion vectors) written
by the same pass.

`HRL_Vulkan_GetSceneStats` reports, per scene, the instance and material
counts, the instances drawn, the passes recorded and the work done by the
scene data cache.

### Conventions

* World space and the public matrices (`HRL_GetProjectionMatrix`) keep the
  OpenGL conventions on every backend. Internally Vulkan uses reversed-Z with a
  0..1 depth range and a flipped viewport, so winding and UVs are unchanged.
* Light units, attenuation and material parameters are the ones of the OpenGL
  backend, so a scene keeps its look when the backend changes.
* Texture rows: `HRL_CreateTexture` stores the bottom row first (v = 0), as OpenGL does.

### Custom shaders (`vk_custom`)

`HRL_CreateShader` sources are compiled at run time by glslang (linked when
HRL is built, `HRL_VULKAN_RUNTIME_SHADERS`). A fragment source that defines
`Surface(HRL_SurfaceInput, inout HRL_Surface)` is a *surface shader*: it is
included into the built-in mesh shaders (`mesh.frag`, `shadow.frag`, and
`mesh.vert` / `shadow.vert` for a `Vertex(inout HRL_VertexData)` function),
whose GLSL sources are embedded in the library, so custom materials keep the
lighting, shadows, fog, ray traced shadows and picking. Their meshes are drawn
from the CPU (frustum culled, one pipeline per shader and bucket). Any other
fragment source is an OpenGL 3.30 post-process shader, compiled as it is.
The `uniform` declarations are removed from the source and replaced by reads
of a parameter table (`vec4` per value) filled from the material.

### OpenGL effects (`vk_effects`)

Decals, screen space reflections, ambient occlusion, volumetric fog, god rays
and clouds run the shaders of the OpenGL backend (`backend/common/hrl_builtin_glsl.h`)
through the same compiler. They read the HDR image as a vertically flipped
copy (OpenGL row order), the depth in the OpenGL convention and world normals
rebuilt from the depth (`glcompat.frag`), and write their viewport back.
