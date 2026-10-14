# HRL Vulkan backend

This backend renders with Vulkan while keeping the `hrl.h` API intact.
Vulkan-specific functionality is exposed separately by `hrl_vulkan.h`.

It is compiled when CMake is configured with `-DCOMPILE_VULKAN=ON` (this defines
`HRL_ENABLE_VULKAN`). Without it, `HRL_Init(HRL_VULKAN)` reports
`HRL_INVALID_BACKEND_OPERATION` and the `hrl_vulkan.h` functions do nothing.

## Same renderer as OpenGL 3.3

The Vulkan backend does not duplicate the renderer. It compiles the OpenGL 3.3
renderer sources a second time (`vk_gl33_*.cpp`, every symbol prefixed with
`VKGL_`) on top of `vkgl/`, a layer that implements the OpenGL calls used by the
renderer with Vulkan:

| OpenGL concept | Vulkan implementation |
|-|-|
| Textures, renderbuffers | `VkImage` + views, layouts tracked automatically |
| Framebuffers, draw buffers | Render passes / framebuffers created and cached on demand |
| Programs (GLSL 3.30) | Translated to Vulkan GLSL 4.50, compiled to SPIR-V, reflected |
| Uniforms (`glUniform*`) | Per program std140 block, uploaded when it changes |
| Uniform buffers | Fixed bindings (LightBlock 0, BoneBlock 1, DDGIBlock0..2 2..4) |
| Buffers (`glBufferData`) | Versioned: a draw always sees the data current when it was recorded |
| Fixed function state | Cached `VkPipeline` |
| Blits, `glReadPixels`, `glCopyTexSubImage2D` | Copy / blit / resolve commands |
| Default framebuffer | Off-screen RGBA8 image copied to the swapchain in `HRL_EndFrame` |

Render targets keep the OpenGL row order and depth range (the vertex shaders
remap `gl_Position.z`), so projection matrices, `gl_FragCoord`, texture
coordinates and depth values are identical to the OpenGL backend. The image is
flipped only when it is presented. Every feature of the OpenGL 3.3 backend
(shadows, post-process, fog, clouds, GI/DDGI, voxels, VFX, picking, MSAA,
debug views, widgets, gizmos, screenshots...) therefore works the same way.

Files:

* `vk_core.*` — instance, device, memory, swapchain, frames, deferred deletion.
* `vk_context.*` — render passes, pipelines, descriptor sets, blits, read-backs.
* `vulkan_shader.*`, `vk_compile.*` — GLSL translation, SPIR-V compilation and cache, reflection.
* `vkgl/` — the OpenGL 3.3 entry points over Vulkan.
* `vulkan_backend.*` — backend table and `hrl_vulkan.h` accessors.

## Presentation / window integration

HRL does not depend on GLFW, SDL or another window library. The application gives HRL a
surface-creation callback and the required instance extensions before `HRL_InitContext()`:

```cpp
#include "hrl_vulkan.h"
#include <vulkan/vulkan.h> // application side only

int CreateMyVkSurface(void* instance_ptr, void* surface_out_ptr, void* user_data)
{
    VkInstance instance = reinterpret_cast<VkInstance>(instance_ptr);
    VkSurfaceKHR* surface = reinterpret_cast<VkSurfaceKHR*>(surface_out_ptr);
    // Create the surface with your window system here (e.g. glfwCreateWindowSurface).
    // Return the VkResult value as an int.
    return static_cast<int>(MyCreateSurface(instance, surface, user_data));
}

HRL_Vulkan_SetInstanceExtensions(required_extensions, required_extension_count);
HRL_Vulkan_SetSurfaceCallback(CreateMyVkSurface, my_window_pointer);
HRL_Init(HRL_VULKAN);
HRL_InitContext(width, height, nullptr);

while (running)
{
    HRL_BeginFrame();
    // ...
    HRL_EndFrame(); // records, submits and presents the frame
}
```

The public `hrl_vulkan.h` contains no Vulkan SDK header and no Vulkan SDK types.
HRL creates the Vulkan instance, physical/logical device, queues and swapchain itself.
Call `HRL_WindowResizeCallback()` when the window size changes: the swapchain is
recreated. Without a surface callback HRL renders off-screen (screenshots and
picking still work).

## Shaders

Built-in and user shaders are the OpenGL GLSL 3.30 sources; there is no
separate Vulkan shader set. They are compiled to SPIR-V by the Vulkan SDK
compiler (`glslangValidator`), found through `HRL_GLSLANG_VALIDATOR`,
`$VULKAN_SDK/Bin` or the `PATH`. Compiled shaders are cached on disk
(`HRL_VULKAN_SHADER_CACHE`, default `<temp>/hrl_vulkan_shader_cache`), so the
compiler only runs the first time a shader is seen.

## Debugging

Set `HRL_VULKAN_VALIDATION=1` to enable `VK_LAYER_KHRONOS_validation` (when it
is installed); validation messages are printed and reported as HRL warnings.
