// HRL Vulkan backend - internal entry points (not part of the public API).
#pragma once

#include "../../core/backend_vtable.h"
#include "../../hrl_vulkan.h"

#include <vulkan/vulkan.h>

#include <cstdint>

void VulkanSetSurfaceCallback(HRL_VulkanCreateSurfaceCallback callback, void* user_data);
void VulkanSetInstanceExtensions(const char* const* extensions, HRL_uint extension_count);
VkInstance VulkanGetInstance();
VkPhysicalDevice VulkanGetPhysicalDevice();
VkDevice VulkanGetDevice();
VkQueue VulkanGetGraphicsQueue();
VkSurfaceKHR VulkanGetSurface();

// Internal backend factory used by hrl.cpp.
HRL_vtable GetVulkanBackend();
