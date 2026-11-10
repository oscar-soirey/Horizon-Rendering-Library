# HRL architecture

## Layers

```
application ── hrl.h (C API, opaque ids) ── hrl_vulkan.h / hrl_gl.h (backend specific)
                    │
              src/hrl.cpp            frontend: validates calls, owns the scene description
              src/core/*             CPU systems shared by backends (LOD, culling helpers, FBX, VFX simulation...)
                    │  HRL_vtable (src/core/backend_vtable.h)
        ┌───────────┴────────────┐
 backend/vulkan (primary)   backend/opengl33, backend/gles3 (deprecated, frozen)
```

The frontend never calls a graphics API. A backend receives resource
creation calls (meshes, textures, scenes) and, once per frame and per scene,
`RHI_RenderScene(scene)`: it reads the scene description (`hrl_scene_t`) and
decides how to draw it. The interface is deliberately high level, so a backend
is free to use the structure that suits its API; it is not a thin wrapper
around OpenGL concepts.

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

### Data model (`vk_renderer`)

* **Geometry pool**: every mesh LOD is a range of one vertex buffer and one
  index buffer (sub-allocated with a VMA virtual block, grown on demand).
* **Bindless textures**: one descriptor array; each texture occupies two
  entries (UNORM view and sRGB view of the same image), so the material decides
  whether a map is colour data. Materials reference textures by index.
* **Scene buffers**, rebuilt each frame: instances (transform, material
  index, object id), materials, lights. Shaders index them with
  `gl_InstanceIndex`.
* **Draw submission**: each pass builds a list of
  `VkDrawIndexedIndirectCommand` and issues one `vkCmdDrawIndexedIndirect` per
  pipeline bucket (opaque / alpha tested × one / two sided). Culling and LOD
  selection run on the CPU today; the data layout is the one a GPU culling
  pass needs.

### Frame

Per scene: environment update when the sky changed (compute), top level
acceleration structure when ray traced shadows are on. Per viewport:

1. **Shadows**: 4 stable cascades for the first shadow casting directional
   light (bounding sphere per slice, texel snapping, blended splits, 3×3 PCF)
   and up to 4 spot light maps, in one depth array. Skipped with ray traced
   shadows, where every shadow casting light (point lights included) traces a
   ray query against the scene.
2. **Forward pass**: HDR colour (RGBA16F) + object id (R32UI), reversed-Z
   depth (D32F). Metallic-roughness BRDF (GGX, height correlated Smith,
   Schlick), image based lighting with multiple scattering compensation, fog.
3. **Sky** where the depth buffer is still empty.

Then, per scene: ACES tone mapping into the LDR scene image, debug primitives,
and a blit to the swapchain for on-screen scenes.

Forward shading was chosen over a full G-buffer: materials are evaluated once,
alpha tested and (later) transparent surfaces follow the same path, and
user surface functions plug into one place. Screen space and ray traced
effects will read a thin G-buffer (normal, roughness, motion vectors) written
by the same pass.

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
