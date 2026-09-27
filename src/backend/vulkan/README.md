# HRL Vulkan backend

This backend provides Vulkan rendering while keeping the original `hrl.h` API intact.
Vulkan-specific functionality is exposed separately by `hrl_vulkan.h`.

## Presentation / window integration

HRL does not depend on GLFW, SDL or another window library. The application gives HRL a
surface-creation callback and the required instance extensions before `HRL_InitContext()`:

```cpp
#include "hrl_vulkan.h"
#include <vulkan/vulkan.h> // application/backend side only

int CreateMyVkSurface(void* instance_ptr, void* surface_out_ptr, void* user_data)
{
    VkInstance instance = reinterpret_cast<VkInstance>(instance_ptr);
    VkSurfaceKHR* surface = reinterpret_cast<VkSurfaceKHR*>(surface_out_ptr);
    // Create the surface with your window system here.
    // Return the VkResult value as an int.
    return static_cast<int>(MyCreateSurface(instance, surface, user_data));
}

HRL_Vulkan_SetInstanceExtensions(required_extensions, required_extension_count);
HRL_Vulkan_SetSurfaceCallback(CreateMyVkSurface, my_window_pointer);
HRL_Init(HRL_VULKAN);
HRL_InitContext(width, height, nullptr);
```

The public `hrl_vulkan.h` contains no Vulkan SDK header and no Vulkan SDK types.
`CreateMyVkSurface` is responsible only for creating the `VkSurfaceKHR` for the host window.
HRL creates the Vulkan instance, physical/logical device, queues and swapchain itself.
The Vulkan library is still linked by the application.

## Built-in shaders

The Vulkan GLSL sources live under `shaders/vulkan/` and are explicitly prefixed with
`vulkan_` so their generated resource symbols cannot collide with the OpenGL shader resources.
CMake embeds these sources into `ressources/ressources.h`; the Vulkan backend uses those
embedded symbols directly. No Vulkan shader source is opened from disk at runtime.
