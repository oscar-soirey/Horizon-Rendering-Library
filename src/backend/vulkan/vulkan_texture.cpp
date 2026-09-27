#include "vulkan_texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>

#include <stb/stb_image.h>

namespace
{
static uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
    {
        if ((type_filter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

static bool CreateBuffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                         VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                         VkBuffer& buffer, VkDeviceMemory& memory)
{
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bi, nullptr, &buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    const uint32_t index = FindMemoryType(physical, req.memoryTypeBits, properties);
    if (index == UINT32_MAX)
    {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = index;
    if (vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS)
    {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(device, buffer, memory, 0);
    return true;
}

static bool CreateImage(VkPhysicalDevice physical, VkDevice device, uint32_t width, uint32_t height,
                        uint32_t mip_levels, VkFormat format, VkImageUsageFlags usage,
                        VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.extent = {width, height, 1};
    ii.mipLevels = mip_levels;
    ii.arrayLayers = 1;
    ii.format = format;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage = usage;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &ii, nullptr, &image) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, image, &req);
    const uint32_t index = FindMemoryType(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (index == UINT32_MAX)
    {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = index;
    if (vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS)
    {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS)
    {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        memory = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static void TransitionImage(VkCommandBuffer cmd, VkImage image,
                            VkImageLayout old_layout, VkImageLayout new_layout,
                            uint32_t mip_levels)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    else
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

static bool Immediate(VkDevice device, VkCommandPool pool, VkQueue queue, const std::function<void(VkCommandBuffer)>& fn)
{
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
    {
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        return false;
    }
    fn(cmd);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
    {
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        return false;
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const VkResult submit = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (submit == VK_SUCCESS) vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
    return submit == VK_SUCCESS;
}

static bool SupportsLinearBlit(VkPhysicalDevice physical, VkFormat format)
{
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0 &&
           (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0 &&
           (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
}
}

bool VulkanTexture::Create(VkPhysicalDevice physical, VkDevice device, VkCommandPool pool, VkQueue queue,
                           const BitmapResult& bitmap, float max_anisotropy, std::string& error)
{
    Destroy(device);
    if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.pixels.empty())
    {
        error = "Vulkan texture: invalid bitmap";
        return false;
    }

    width = static_cast<uint32_t>(bitmap.width);
    height = static_cast<uint32_t>(bitmap.height);
    format = VK_FORMAT_R8G8B8A8_UNORM;
    this->max_anisotropy = std::max(1.0f, max_anisotropy);
    mip_levels = SupportsLinearBlit(physical, format)
        ? static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(std::max(width, height))))) + 1u
        : 1u;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!CreateBuffer(physical, device, bitmap.pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      stagingBuffer, stagingMemory))
    {
        error = "Vulkan texture: failed to create staging buffer";
        return false;
    }

    void* mapped = nullptr;
    vkMapMemory(device, stagingMemory, 0, bitmap.pixels.size(), 0, &mapped);
    std::memcpy(mapped, bitmap.pixels.data(), bitmap.pixels.size());
    vkUnmapMemory(device, stagingMemory);

    if (!CreateImage(physical, device, width, height, mip_levels, format,
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     image, memory))
    {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        error = "Vulkan texture: failed to create image";
        return false;
    }

    const bool uploadOk = Immediate(device, pool, queue, [&](VkCommandBuffer cmd)
    {
        TransitionImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_levels);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        if (mip_levels > 1)
        {
            int32_t mipWidth = static_cast<int32_t>(width);
            int32_t mipHeight = static_cast<int32_t>(height);
            for (uint32_t level = 1; level < mip_levels; ++level)
            {
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.image = image;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;
                barrier.subresourceRange.baseMipLevel = level - 1;
                barrier.subresourceRange.levelCount = 1;
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

                VkImageBlit blit{};
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel = level - 1;
                blit.srcSubresource.layerCount = 1;
                blit.srcOffsets[0] = {0, 0, 0};
                blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel = level;
                blit.dstSubresource.layerCount = 1;
                blit.dstOffsets[0] = {0, 0, 0};
                blit.dstOffsets[1] = {std::max(1, mipWidth / 2), std::max(1, mipHeight / 2), 1};
                vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                mipWidth = std::max(1, mipWidth / 2);
                mipHeight = std::max(1, mipHeight / 2);
            }
        }
        VkImageMemoryBarrier lastBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        lastBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        lastBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        lastBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        lastBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        lastBarrier.image = image;
        lastBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        lastBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        lastBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        lastBarrier.subresourceRange.baseMipLevel = mip_levels - 1;
        lastBarrier.subresourceRange.levelCount = 1;
        lastBarrier.subresourceRange.baseArrayLayer = 0;
        lastBarrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &lastBarrier);
    });

    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    if (!uploadOk)
    {
        error = "Vulkan texture: image upload command failed";
        Destroy(device);
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = mip_levels;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS)
    {
        error = "Vulkan texture: failed to create image view";
        Destroy(device);
        return false;
    }

    if (!SetMinFilter(physical, device, HRL_FILTER_TRILINEAR, error))
        return false;
    return true;
}

bool VulkanTexture::CreateFromEncoded(VkPhysicalDevice physical, VkDevice device, VkCommandPool pool, VkQueue queue,
                                      const char* data, size_t size, float max_anisotropy, std::string& error)
{
    if (!data || size == 0 || size > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        error = "Vulkan texture: invalid encoded buffer";
        return false;
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data), static_cast<int>(size),
                                            &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels)
    {
        error = "Vulkan texture: stb_image decode failed";
        return false;
    }
    BitmapResult bitmap;
    bitmap.width = w;
    bitmap.height = h;
    bitmap.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    std::memcpy(bitmap.pixels.data(), pixels, bitmap.pixels.size());
    stbi_image_free(pixels);
    return Create(physical, device, pool, queue, bitmap, max_anisotropy, error);
}

void VulkanTexture::Destroy(VkDevice device)
{
    if (!device) return;
    if (sampler) vkDestroySampler(device, sampler, nullptr);
    if (view) vkDestroyImageView(device, view, nullptr);
    if (image) vkDestroyImage(device, image, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
    sampler = VK_NULL_HANDLE;
    view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    width = height = mip_levels = 0;
}

static VkFilter ToVkFilter(HRL_EFilterType filter)
{
    return filter == HRL_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

bool VulkanTexture::SetMinFilter(VkPhysicalDevice /*physical*/, VkDevice device, HRL_EFilterType filter, std::string& error)
{
    if (!device || !view) { error = "Vulkan texture: sampler is not initialized"; return false; }
    if (sampler) vkDestroySampler(device, sampler, nullptr);
    min_filter = filter;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = ToVkFilter(mag_filter);
    si.minFilter = ToVkFilter(filter);
    si.mipmapMode = (filter == HRL_FILTER_TRILINEAR || filter == HRL_FILTER_ANISOTROPIC || filter == HRL_FILTER_SUPERSAMPLING)
        ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.mipLodBias = 0.0f;
    si.minLod = 0.0f;
    si.maxLod = static_cast<float>(mip_levels > 0 ? mip_levels - 1 : 0);
    if (filter == HRL_FILTER_ANISOTROPIC && max_anisotropy > 1.0f)
    {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = max_anisotropy;
    }
    if (vkCreateSampler(device, &si, nullptr, &sampler) != VK_SUCCESS)
    {
        error = "Vulkan texture: failed to create sampler";
        return false;
    }
    return true;
}

bool VulkanTexture::SetMagFilter(VkPhysicalDevice physical, VkDevice device, HRL_EFilterType filter, std::string& error)
{
    mag_filter = filter;
    return SetMinFilter(physical, device, min_filter, error);
}
