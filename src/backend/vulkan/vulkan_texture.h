#pragma once

#include <vulkan/vulkan.h>

#include <string>

#include "../../core/object_types.h"

class VulkanTexture
{
public:
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 1;
    HRL_EFilterType min_filter = HRL_FILTER_LINEAR;
    HRL_EFilterType mag_filter = HRL_FILTER_LINEAR;
    float max_anisotropy = 1.0f;

    bool Create(VkPhysicalDevice physical, VkDevice device, VkCommandPool pool, VkQueue queue,
                const BitmapResult& bitmap, float max_anisotropy, std::string& error);
    bool CreateFromEncoded(VkPhysicalDevice physical, VkDevice device, VkCommandPool pool, VkQueue queue,
                           const char* data, size_t size, float max_anisotropy, std::string& error);
    void Destroy(VkDevice device);
    bool SetMinFilter(VkPhysicalDevice physical, VkDevice device, HRL_EFilterType filter, std::string& error);
    bool SetMagFilter(VkPhysicalDevice physical, VkDevice device, HRL_EFilterType filter, std::string& error);
};
