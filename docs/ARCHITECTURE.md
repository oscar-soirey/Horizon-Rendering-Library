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
| `vk_frame_graph` | Pass declaration, automatic barriers and rendering scopes, transient image pool |
| `vk_pipelines` | Descriptor set layouts and pipelines |
| `vk_resources` | Bindless textures, geometry pool, bottom level acceleration structures |
| `vk_scene_data` | Cached instances / materials, lights, cameras, shadow cascades |
| `vk_environment` | Image based lighting |
| `vk_renderer` | Render targets, frame of a scene, picking, screenshots |
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
* **Draw submission**: each pass builds a list of
  `VkDrawIndexedIndirectCommand` and issues one `vkCmdDrawIndexedIndirect` per
  pipeline bucket (opaque / alpha tested × one / two sided). Culling and LOD
  selection run on the CPU today; the data layout is the one a GPU culling
  pass needs.

### Frame graph

The frame of a scene is declared as passes (`vk_frame_graph`). A pass names
the images it samples, copies or renders to; the graph inserts the layout
transitions, opens the dynamic rendering scope of raster passes, labels the
pass for RenderDoc and hands out transient images from a pool (the HDR colour
target is one: every scene of the same size shares it). CPU work (culling,
descriptor sets) is done while the passes are declared; GPU commands are
recorded by `FrameGraph::Execute`.

```
[brdf lut] [environment] [tlas build]                 when needed
per viewport:  shadow map × N  →  forward (+ sky)
tone mapping → [debug] → [screenshot] → [present copy]
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

### Material contract (planned for custom shaders)

`HRL_CreateShader` is not implemented on Vulkan yet. It will accept a GLSL
*surface function* that fills base colour, normal, roughness, metallic,
emissive and opacity; HRL generates the rest, so custom materials work with
shadows, ray tracing and GI. Full vertex + fragment shaders stay an OpenGL feature.
