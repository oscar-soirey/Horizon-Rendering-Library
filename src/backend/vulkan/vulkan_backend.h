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


// Internal backend factory used by hrl.cpp. Not part of the public HRL API.
HRL_vtable GetVulkanBackend();
