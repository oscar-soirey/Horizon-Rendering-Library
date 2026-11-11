# HRL native Vulkan backend

Vulkan 1.3 renderer selected with `HRL_Init(HRL_VULKAN)`. Design and frame
structure: `docs/ARCHITECTURE.md`. Planned work: `docs/ROADMAP.md`.

| File | Role |
|-|-|
| `vk_device.*` | Instance, GPU selection, device features, VMA allocator, swapchain, frames in flight, upload command buffer, deferred destruction, layout transitions |
| `vk_frame_graph.*` | Pass declaration, automatic barriers and rendering scopes, transient image pool |
| `vk_pipelines.cpp` | Descriptor set layouts and pipelines |
| `vk_resources.cpp` | Bindless textures, geometry pool, bottom level acceleration structures |
| `vk_scene_data.cpp` | Cached instances / materials, draw records, lights, cameras, shadow cascades |
| `vk_culling.cpp` | GPU culling / LOD selection of meshes, two-phase Hi-Z occlusion, statistics read back |
| `vk_landscape.cpp` | CDLOD landscapes (float height textures, quadtree, ray traced geometry) |
| `vk_foliage.cpp` | Foliage cells, culling / LOD / cross-fade compute passes, ray traced instances |
| `vk_impostor.cpp` | Octahedral impostors: baking and drawing |
| `vk_grass.cpp` | Procedural grass |
| `vk_environment.cpp` | Image based lighting |
| `vk_renderer.*` | Render targets, frame of a scene, picking, screenshots |
| `vk_renderer_internal.h` | Types shared by the renderer files |
| `vulkan_backend.*` | `HRL_vtable` of the backend and the `hrl_vulkan.h` functions |
| `vk_mem_alloc.cpp` | Implementation unit of Vulkan Memory Allocator (`third_party/vma`) |
| `src/shaders/vulkan/` | GLSL 4.60 sources, compiled to SPIR-V by CMake and embedded |

From the HRL core this backend only includes `backend_api.h`,
`render_common.h` and `object_types.h` (enforced by the `backend_isolation` test).

Requirements: Vulkan 1.3 with dynamic rendering, synchronization2, descriptor
indexing (update-after-bind sampled images), multi-draw-indirect, draw indirect count. Optional:
`VK_KHR_acceleration_structure` + `VK_KHR_ray_query` (ray traced shadows).

Environment variables: `HRL_VULKAN_VALIDATION=1`, `HRL_VULKAN_NO_RAYTRACING=1`.
