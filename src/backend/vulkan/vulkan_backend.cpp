#include "vulkan_backend.h"

#include "vulkan_shader.h"
#include "vulkan_texture.h"

#include "../../core/object_types.h"
#include "../../core/widgets.h"
#include "../../core/utils_functions.h"
#include "../../ressources/ressources.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
constexpr uint32_t kMaxSamplers = 16;
constexpr uint32_t kMaxLights = 32;
constexpr uint32_t kLightBinding = 32;
constexpr uint32_t kUniformBinding = 31;
constexpr size_t kInitialUniformCapacity = 2u * 1024u * 1024u;
constexpr size_t kDebugBufferCapacity = 4u * 1024u * 1024u;

struct alignas(16) LightGPU
{
    // Exact std140 layout of the GLSL Light struct:
    //   0..15   : type, intensity, attenuation, innerCutoff
    //   16..31  : position.xyz, outerCutoff
    //   32..47  : rotation.xyz, padding
    //   48..63  : color.xyz, shadowStrength
    //   64..127 : shadowMatrix (mat4)
    //   128..143: shadowParams (vec4)
    uint32_t type = 0;
    float intensity = 0.0f;
    float attenuation = 0.0f;
    float innerCutoff = 0.0f;

    glm::vec4 position_outer{0.0f};
    glm::vec4 rotation_padding{0.0f};
    glm::vec4 color_shadow{1.0f, 1.0f, 1.0f, 1.0f};
    glm::mat4 shadowMatrix{1.0f};
    glm::vec4 shadowParams{0.0f, 0.0f, -1.0f, 0.0f};
};

static_assert(offsetof(LightGPU, type) == 0, "LightGPU::type offset mismatch");
static_assert(offsetof(LightGPU, position_outer) == 16, "LightGPU::position offset mismatch");
static_assert(offsetof(LightGPU, rotation_padding) == 32, "LightGPU::rotation offset mismatch");
static_assert(offsetof(LightGPU, color_shadow) == 48, "LightGPU::color offset mismatch");
static_assert(offsetof(LightGPU, shadowMatrix) == 64, "LightGPU::shadowMatrix offset mismatch");
static_assert(offsetof(LightGPU, shadowParams) == 128, "LightGPU::shadowParams offset mismatch");
static_assert(sizeof(LightGPU) == 144, "LightGPU must match std140 Light layout");

struct LightBlockGPU
{
    LightGPU lights[kMaxLights]{};
    uint32_t count = 0;
    uint32_t pad[3]{};
};

struct BufferResource
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct ImageResource
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0,0};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

struct MeshLevelGPU
{
    BufferResource vertex;
    BufferResource index;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
};

struct MeshGPU
{
    std::vector<MeshLevelGPU> levels;
    BufferResource skeletal_vertex;
    BufferResource skeletal_index;
    uint32_t skeletal_vertex_count = 0;
    uint32_t skeletal_index_count = 0;
    uint64_t uploaded_pose_serial = 0;
    bool skeletal = false;
};

struct SceneGPU
{
    int width = 1;
    int height = 1;
    int render_on_screen = 0;
    bool picking = true;
    VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat picking_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageLayout color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    ImageResource color;
    ImageResource bright;
    ImageResource picking_image;
    ImageResource depth;
    ImageResource color_msaa;
    ImageResource bright_msaa;
    ImageResource picking_msaa;
    ImageResource depth_msaa;

    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;

    ImageResource post_a;
    ImageResource post_b;
    ImageResource depth_resolve;
    VkRenderPass post_render_pass = VK_NULL_HANDLE;
    VkFramebuffer post_framebuffer_a = VK_NULL_HANDLE;
    VkFramebuffer post_framebuffer_b = VK_NULL_HANDLE;
    VkImageLayout post_a_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout post_b_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout depth_resolve_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct ShadowGPU
{
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> face_framebuffers;
    ImageResource depth;
    std::vector<VkImageView> face_views;
    bool cube = false;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    int resolution = 0;
};

struct PipelineKey
{
    HRL_id shader = HRL_INVALID_ID;
    uint32_t mode = 0; // 0 static, 1 skeletal, 2 sprite, 3 debug, 4 ui, 5 post, 6 sky
    uint32_t cull = 0;
    uint32_t blend = 0;
    VkRenderPass render_pass = VK_NULL_HANDLE;

    bool operator==(const PipelineKey& other) const
    {
        return shader == other.shader && mode == other.mode && cull == other.cull && blend == other.blend && render_pass == other.render_pass;
    }
};

struct PipelineKeyHash
{
    size_t operator()(const PipelineKey& k) const noexcept
    {
        size_t h = std::hash<uint32_t>{}(k.shader);
        h ^= std::hash<uint32_t>{}(k.mode + 0x9e3779b9u + static_cast<uint32_t>(h << 6) + static_cast<uint32_t>(h >> 2));
        h ^= std::hash<uint32_t>{}(k.cull + 0x9e3779b9u + static_cast<uint32_t>(h << 6) + static_cast<uint32_t>(h >> 2));
        h ^= std::hash<uint32_t>{}(k.blend + 0x9e3779b9u + static_cast<uint32_t>(h << 6) + static_cast<uint32_t>(h >> 2));
        h ^= std::hash<VkRenderPass>{}(k.render_pass);
        return h;
    }
};

struct SceneDrawState
{
    const hrl_scene_t* scene = nullptr;
    HRL_id scene_id = HRL_INVALID_ID;
    const HRL_Viewport* viewport = nullptr;
    glm::mat4 projection{1.0f};
    glm::mat4 view{1.0f};
    HRL_Material* material = nullptr;
};

struct UIVertex { float p[2]; float uv[2]; float c[4]; };
struct VFXRenderInstance { glm::mat4 model{1.0f}; glm::vec4 color{1.0f}; };

struct VulkanState
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    uint32_t graphics_family = VK_QUEUE_FAMILY_IGNORED;
    uint32_t present_family = VK_QUEUE_FAMILY_IGNORED;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    HRL_VulkanCreateSurfaceCallback create_surface_callback = nullptr;
    void* surface_user_data = nullptr;
    std::vector<std::string> instance_extensions;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapchain_extent{0,0};
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> swapchain_views;
    uint32_t swapchain_index = 0;
    VkImageLayout swapchain_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer frame_command = VK_NULL_HANDLE;
    VkCommandBuffer upload_command = VK_NULL_HANDLE;
    VkFence frame_fence = VK_NULL_HANDLE;
    VkFence upload_fence = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkSemaphore render_finished = VK_NULL_HANDLE;

    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

    BufferResource uniform_buffer;
    void* uniform_mapped = nullptr;
    size_t uniform_capacity = kInitialUniformCapacity;
    size_t uniform_cursor = 0;
    size_t uniform_alignment = 256;

    BufferResource light_buffer;
    void* light_mapped = nullptr;
    LightBlockGPU lights{};

    BufferResource debug_buffer;
    void* debug_mapped = nullptr;
    size_t debug_cursor = 0;

    BufferResource vfx_buffer;
    void* vfx_mapped = nullptr;
    size_t vfx_capacity = 8u * 1024u * 1024u;
    BufferResource vfx_quad_vertex;
    BufferResource vfx_quad_index;

    VkSampleCountFlagBits msaa = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlags supported_samples = VK_SAMPLE_COUNT_1_BIT;
    HRL_id fallback_texture = HRL_INVALID_ID;
    bool sampler_anisotropy = false;
    float max_anisotropy = 1.0f;
    bool linear_blit = false;

    std::unordered_map<HRL_id, VulkanTexture> textures;
    std::unordered_map<HRL_id, VulkanShader> shaders;
    std::unordered_map<HRL_id, MeshGPU> meshes;
    std::unordered_map<HRL_id, SceneGPU> scenes;
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;
    std::unordered_map<std::string, HRL_id> builtin_shader_ids;

    std::vector<HRL_id> pending_screenshot_scenes;
    std::unordered_map<HRL_id, std::string> screenshot_paths;
    std::unordered_map<HRL_id, ShadowGPU> shadow_maps;
    std::unordered_map<HRL_id, std::unordered_map<HRL_id, int>> active_shadow_slots_by_scene;
    bool frame_open = false;
    bool present_requested = false;
    bool surface_warning_emitted = false;
    bool initialized = false;
    bool instance_created = false;
} g;


static void VkError(const char* what, VkResult result, HRL_ESeverity severity = HRL_SEVERITY_ERROR)
{
    std::ostringstream ss;
    ss << what << " (VkResult=" << static_cast<int>(result) << ")";
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, severity, ss.str());
}

static uint32_t FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(g.physical, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((type_filter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    return UINT32_MAX;
}

static bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, BufferResource& out)
{
    out = {};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(g.device, &bi, nullptr, &out.buffer);
    if (r != VK_SUCCESS) { VkError("vkCreateBuffer", r); return false; }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g.device, out.buffer, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, properties);
    if (type == UINT32_MAX)
    {
        vkDestroyBuffer(g.device, out.buffer, nullptr);
        out = {};
        SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "Vulkan: no compatible buffer memory type");
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    r = vkAllocateMemory(g.device, &ai, nullptr, &out.memory);
    if (r != VK_SUCCESS)
    {
        vkDestroyBuffer(g.device, out.buffer, nullptr);
        out = {};
        VkError("vkAllocateMemory(buffer)", r);
        return false;
    }
    r = vkBindBufferMemory(g.device, out.buffer, out.memory, 0);
    if (r != VK_SUCCESS)
    {
        vkDestroyBuffer(g.device, out.buffer, nullptr);
        vkFreeMemory(g.device, out.memory, nullptr);
        out = {};
        VkError("vkBindBufferMemory", r);
        return false;
    }
    out.size = size;
    return true;
}

static void DestroyBuffer(BufferResource& buffer)
{
    if (buffer.buffer) vkDestroyBuffer(g.device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(g.device, buffer.memory, nullptr);
    buffer = {};
}

static bool CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                        VkSampleCountFlagBits samples, VkImageAspectFlags aspect, ImageResource& out)
{
    out = {};
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.extent = {width, height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.format = format;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage = usage;
    ii.samples = samples;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateImage(g.device, &ii, nullptr, &out.image);
    if (r != VK_SUCCESS) { VkError("vkCreateImage", r); return false; }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g.device, out.image, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        vkDestroyImage(g.device, out.image, nullptr);
        out = {};
        SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "Vulkan: no compatible image memory type");
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    r = vkAllocateMemory(g.device, &ai, nullptr, &out.memory);
    if (r != VK_SUCCESS)
    {
        vkDestroyImage(g.device, out.image, nullptr);
        out = {};
        VkError("vkAllocateMemory(image)", r);
        return false;
    }
    r = vkBindImageMemory(g.device, out.image, out.memory, 0);
    if (r != VK_SUCCESS)
    {
        vkDestroyImage(g.device, out.image, nullptr);
        vkFreeMemory(g.device, out.memory, nullptr);
        out = {};
        VkError("vkBindImageMemory", r);
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    r = vkCreateImageView(g.device, &vi, nullptr, &out.view);
    if (r != VK_SUCCESS)
    {
        vkDestroyImage(g.device, out.image, nullptr);
        vkFreeMemory(g.device, out.memory, nullptr);
        out = {};
        VkError("vkCreateImageView", r);
        return false;
    }
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT)
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        const bool nearest = (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        si.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        si.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.mipLodBias = 0.0f;
        si.anisotropyEnable = VK_FALSE;
        si.maxAnisotropy = 1.0f;
        si.compareEnable = VK_FALSE;
        si.minLod = 0.0f;
        si.maxLod = 0.0f;
        if (vkCreateSampler(g.device, &si, nullptr, &out.sampler) != VK_SUCCESS)
        {
            vkDestroyImageView(g.device, out.view, nullptr);
            vkDestroyImage(g.device, out.image, nullptr);
            vkFreeMemory(g.device, out.memory, nullptr);
            out = {};
            return false;
        }
    }
    out.format = format;
    out.extent = {width,height};
    out.samples = samples;
    return true;
}

static bool CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect, VkImageView& out)
{
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(g.device, &vi, nullptr, &out) == VK_SUCCESS;
}

static void DestroyImage(ImageResource& image)
{
    if (image.sampler) vkDestroySampler(g.device, image.sampler, nullptr);
    if (image.view) vkDestroyImageView(g.device, image.view, nullptr);
    if (image.image) vkDestroyImage(g.device, image.image, nullptr);
    if (image.memory) vkFreeMemory(g.device, image.memory, nullptr);
    image = {};
}

static void TransitionImage(VkCommandBuffer cmd, VkImage image,
                            VkImageLayout old_layout, VkImageLayout new_layout,
                            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                            uint32_t layerCount = 1)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = std::max(1u, layerCount);
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        b.srcAccessMask = 0;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    if (new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    else if (new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }
    else if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else if (new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
    {
        b.dstAccessMask = 0;
        dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn)
{
    vkWaitForFences(g.device, 1, &g.upload_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(g.device, 1, &g.upload_fence);
    vkResetCommandBuffer(g.upload_command, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(g.upload_command, &bi) != VK_SUCCESS) return false;
    fn(g.upload_command);
    if (vkEndCommandBuffer(g.upload_command) != VK_SUCCESS) return false;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.upload_command;
    if (vkQueueSubmit(g.graphics_queue, 1, &si, g.upload_fence) != VK_SUCCESS) return false;
    return vkWaitForFences(g.device, 1, &g.upload_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

static void CopyBuffer(const BufferResource& src, BufferResource& dst, VkDeviceSize size)
{
    ImmediateSubmit([&](VkCommandBuffer cmd)
    {
        VkBufferCopy copy{0,0,size};
        vkCmdCopyBuffer(cmd, src.buffer, dst.buffer, 1, &copy);
    });
}

static bool UploadBuffer(const void* data, size_t size, VkBufferUsageFlags usage, BufferResource& out)
{
    BufferResource staging;
    if (!CreateBuffer(static_cast<VkDeviceSize>(size), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging))
        return false;
    void* mapped = nullptr;
    if (vkMapMemory(g.device, staging.memory, 0, size, 0, &mapped) != VK_SUCCESS)
    {
        DestroyBuffer(staging);
        return false;
    }
    std::memcpy(mapped, data, size);
    vkUnmapMemory(g.device, staging.memory);
    if (!CreateBuffer(static_cast<VkDeviceSize>(size), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out))
    {
        DestroyBuffer(staging);
        return false;
    }
    CopyBuffer(staging, out, static_cast<VkDeviceSize>(size));
    DestroyBuffer(staging);
    return true;
}

static VkSampleCountFlagBits ChooseMSAA(int requested)
{
    VkSampleCountFlagBits desired = VK_SAMPLE_COUNT_1_BIT;
    if (requested >= 8) desired = VK_SAMPLE_COUNT_8_BIT;
    else if (requested >= 4) desired = VK_SAMPLE_COUNT_4_BIT;
    else if (requested >= 2) desired = VK_SAMPLE_COUNT_2_BIT;
    if ((g.supported_samples & desired) != 0) return desired;
    if (desired >= VK_SAMPLE_COUNT_8_BIT && (g.supported_samples & VK_SAMPLE_COUNT_4_BIT)) return VK_SAMPLE_COUNT_4_BIT;
    if (desired >= VK_SAMPLE_COUNT_4_BIT && (g.supported_samples & VK_SAMPLE_COUNT_2_BIT)) return VK_SAMPLE_COUNT_2_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
}

static VkFormat FindDepthFormat()
{
    const std::array<VkFormat,3> candidates{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (VkFormat f : candidates)
    {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(g.physical, f, &p);
        if ((p.optimalTilingFeatures & required) == required)
            return f;
    }
    return VK_FORMAT_D32_SFLOAT;
}

static std::pair<int,int> SceneDimensions(const hrl_scene_t* scene)
{
    if (!scene) return {static_cast<int>(GetWindowWidth()), static_cast<int>(GetWindowHeight())};
    return {std::max(1, static_cast<int>(GetWindowWidth())), std::max(1, static_cast<int>(GetWindowHeight()))};
}

static glm::mat4 ModelMatrix(const HRL_Mesh* mesh)
{
    if (!mesh) return glm::mat4(1.0f);
    glm::mat4 model(1.0f);
    model = glm::translate(model, mesh->position_);
    model = glm::translate(model, mesh->pivot_point_);
    model = glm::rotate(model, glm::radians(mesh->rotation_.x), glm::vec3(1,0,0));
    model = glm::rotate(model, glm::radians(mesh->rotation_.y), glm::vec3(0,1,0));
    model = glm::rotate(model, glm::radians(mesh->rotation_.z), glm::vec3(0,0,1));
    model = glm::translate(model, -mesh->pivot_point_);
    model = glm::scale(model, mesh->scale_);
    if (mesh->type_ == HRL_3D_SKELETAL_MESH)
    {
        if (const auto* skeletal = dynamic_cast<const HRL_SkeletalMesh*>(mesh))
            model *= skeletal->fbx_geometry_to_world_;
    }
    return model;
}

static glm::mat4 Projection(const HRL_Viewport* viewport)
{
    const float width = std::max(1.0f, static_cast<float>(GetWindowWidth()) * viewport->width_);
    const float height = std::max(1.0f, static_cast<float>(GetWindowHeight()) * viewport->height_);
    const float aspect = width / height;
    glm::mat4 proj(1.0f);
    if (viewport->camera_->type_ == HRL_PERSPECTIVE)
        proj = glm::perspective(glm::radians(viewport->camera_->value_), aspect,
                                viewport->camera_->near_plane_, viewport->camera_->far_plane_);
    else
    {
        const float hh = viewport->camera_->value_ * 0.5f;
        const float hw = hh * aspect;
        proj = glm::ortho(-hw, hw, -hh, hh,
                          viewport->camera_->near_plane_, viewport->camera_->far_plane_);
    }
    // OpenGL -> Vulkan clip-space conversion: Z [−1,1] -> [0,1], Y flip.
    glm::mat4 zclip(1.0f);
    zclip[2][2] = 0.5f;
    zclip[3][2] = 0.5f;
    proj = zclip * proj;
    proj[1][1] *= -1.0f;
    return proj;
}

static glm::mat4 View(const HRL_Viewport* viewport)
{
    return glm::lookAt(viewport->camera_->position_,
                       viewport->camera_->position_ + GetForwardVector(viewport->camera_->rotation_),
                       GetUpVector(viewport->camera_->rotation_));
}

static int SelectLOD(const HRL_Mesh* mesh, const glm::mat4& model, const glm::mat4& view, const glm::mat4& projection, const HRL_Viewport* viewport)
{
    if (!mesh || !mesh->lod_automatic_ || mesh->lods_.size() <= 1) return 0;
    const int maxLevel = static_cast<int>(mesh->lods_.size()) - 1;
    if (mesh->lod_override_ >= 0) return std::clamp(mesh->lod_override_, 0, maxLevel);

    const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.0f));
    float metric = 0.0f;
    float threshold = (mesh->lod_mode_ == HRL_LOD_DISTANCE) ? mesh->lod_base_distance_ : mesh->lod_screen_threshold_;
    if (mesh->lod_mode_ == HRL_LOD_DISTANCE)
    {
        metric = glm::length(center - viewport->camera_->position_);
        if (metric <= mesh->lod_min_distance_) return 0;
        if (metric >= mesh->lod_max_distance_) return maxLevel;
        for (int level = 1; level <= maxLevel; ++level)
        {
            if (metric < threshold) return level - 1;
            threshold *= std::max(1.01f, mesh->lod_distance_scale_);
        }
        return maxLevel;
    }
    const glm::vec4 vc = view * glm::vec4(center, 1.0f);
    const float z = std::max(std::abs(vc.z), 1e-4f);
    const float scale = std::max({std::abs(mesh->scale_.x), std::abs(mesh->scale_.y), std::abs(mesh->scale_.z)});
    const float radius = mesh->bounds_radius_ * std::max(scale, 1e-6f);
    metric = std::abs(projection[1][1]) * radius / z;
    for (int level = 1; level <= maxLevel; ++level)
    {
        if (metric >= threshold) return level - 1;
        threshold *= std::max(0.01f, mesh->lod_screen_scale_);
    }
    return maxLevel;
}

static const VulkanShader* GetShader(HRL_id id)
{
    auto it = g.shaders.find(id);
    return it == g.shaders.end() ? nullptr : &it->second;
}

static VulkanShader* GetShaderMutable(HRL_id id)
{
    auto it = g.shaders.find(id);
    return it == g.shaders.end() ? nullptr : &it->second;
}

static const VulkanTexture* GetTexture(HRL_id id)
{
    auto it = g.textures.find(id);
    return it == g.textures.end() ? nullptr : &it->second;
}

static bool EnsureFrameBufferRange(size_t required)
{
    if (required <= g.uniform_capacity) return true;
    const size_t new_capacity = std::max(required, g.uniform_capacity * 2);
    if (g.uniform_mapped) vkUnmapMemory(g.device, g.uniform_buffer.memory);
    DestroyBuffer(g.uniform_buffer);
    g.uniform_capacity = ((new_capacity + 65535) / 65536) * 65536;
    if (!CreateBuffer(g.uniform_capacity,
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      g.uniform_buffer))
        return false;
    if (vkMapMemory(g.device, g.uniform_buffer.memory, 0, VK_WHOLE_SIZE, 0, &g.uniform_mapped) != VK_SUCCESS)
    {
        SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "Vulkan: failed to map dynamic uniform buffer");
        return false;
    }
    return true;
}

static size_t AllocateUniform(size_t size, const void* data)
{
    const size_t aligned = (g.uniform_cursor + g.uniform_alignment - 1) / g.uniform_alignment * g.uniform_alignment;
    const size_t end = aligned + size;
    if (!EnsureFrameBufferRange(end)) return std::numeric_limits<size_t>::max();
    std::memset(static_cast<unsigned char*>(g.uniform_mapped) + aligned, 0, size);
    if (data) std::memcpy(static_cast<unsigned char*>(g.uniform_mapped) + aligned, data, size);
    g.uniform_cursor = end;
    return aligned;
}

static size_t Std140Size(VulkanUniformField::Type type, uint32_t count)
{
    size_t base = 4;
    switch (type)
    {
        case VulkanUniformField::Type::Float:
        case VulkanUniformField::Type::Int:
        case VulkanUniformField::Type::UInt:
        case VulkanUniformField::Type::Bool: base = 4; break;
        case VulkanUniformField::Type::Vec2: base = 8; break;
        case VulkanUniformField::Type::Vec3:
        case VulkanUniformField::Type::Vec4: base = 16; break;
        case VulkanUniformField::Type::Mat3: base = 48; break;
        case VulkanUniformField::Type::Mat4: base = 64; break;
        case VulkanUniformField::Type::Mat4Array: base = 64; break;
    }
    if (count > 1) base = ((base + 15) / 16) * 16 * count;
    return base;
}

static const VulkanUniformField* FindUniform(const VulkanShader* shader, const char* name)
{
    if (!shader || !name) return nullptr;
    for (const auto& field : shader->uniforms)
        if (field.name == name) return &field;
    return nullptr;
}

template<typename T>
static void SetUniformRaw(std::vector<unsigned char>& data, const VulkanShader* shader, const char* name, const T& value)
{
    const VulkanUniformField* f = FindUniform(shader, name);
    if (!f || f->offset + sizeof(T) > data.size()) return;
    std::memcpy(data.data() + f->offset, &value, sizeof(T));
}

static void SetUniformVec3(std::vector<unsigned char>& data, const VulkanShader* shader, const char* name, const glm::vec3& value)
{
    const VulkanUniformField* f = FindUniform(shader, name);
    if (!f) return;
    if (f->type == VulkanUniformField::Type::Vec4)
    {
        if (f->offset + 16 > data.size()) return;
        const glm::vec4 value4(value, 1.0f);
        std::memcpy(data.data() + f->offset, &value4, sizeof(value4));
        return;
    }
    if (f->offset + 12 > data.size()) return;
    std::memcpy(data.data() + f->offset, &value, sizeof(float) * 3);
}

static void SetUniformMat(std::vector<unsigned char>& data, const VulkanShader* shader, const char* name, const glm::mat4& value)
{
    const VulkanUniformField* f = FindUniform(shader, name);
    if (!f || f->offset + sizeof(glm::mat4) > data.size()) return;
    std::memcpy(data.data() + f->offset, &value, sizeof(glm::mat4));
}

static void SetUniformMat3(std::vector<unsigned char>& data, const VulkanShader* shader, const char* name, const glm::mat3& value)
{
    const VulkanUniformField* f = FindUniform(shader, name);
    if (!f || f->offset + 48 > data.size()) return;
    // std140 mat3 has three vec4 columns.
    for (int c = 0; c < 3; ++c)
        std::memcpy(data.data() + f->offset + static_cast<size_t>(c) * 16, &value[c], sizeof(float) * 3);
}

static void FillUniforms(const VulkanShader* shader, const SceneDrawState& state, const HRL_Mesh* mesh,
                         std::vector<unsigned char>& data, uint32_t colorPickingId, bool sprite = false,
                         const glm::mat4* modelOverride = nullptr, const glm::vec4* tintOverride = nullptr)
{
    if (!shader) return;
    if (data.empty()) data.resize(std::max<size_t>(16, shader->uniform_block_size));
    SetUniformMat(data, shader, "projection", state.projection);
    SetUniformMat(data, shader, "uProjection", state.projection);
    SetUniformMat(data, shader, "view", state.view);
    SetUniformMat(data, shader, "uView", state.view);

    const glm::mat4 model = modelOverride ? *modelOverride : (mesh ? ModelMatrix(mesh) : glm::mat4(1.0f));
    SetUniformMat(data, shader, "model", model);
    SetUniformMat(data, shader, "uModel", model);

    // Match the OpenGL renderer: normals/tangents use the inverse-transpose of
    // the object transform. A singular scale must not poison the whole draw
    // with NaNs, so fall back to an identity normal matrix in that case.
    glm::mat3 normalMatrix(1.0f);
    const glm::mat3 model3(model);
    const float determinant = glm::determinant(model3);
    if (std::isfinite(determinant) && std::abs(determinant) > 1e-8f)
        normalMatrix = glm::transpose(glm::inverse(model3));
    SetUniformMat3(data, shader, "normalMatrix", normalMatrix);
    SetUniformMat(data, shader, "uNormalMatrix", glm::mat4(normalMatrix));

    if (mesh)
    {
        glm::vec4 region(mesh->region_[0], mesh->region_[1], mesh->region_[2], mesh->region_[3]);
        SetUniformRaw(data, shader, "UVRegion", region);
        SetUniformRaw(data, shader, "uUVRegion", region);
    }

    if (state.viewport && state.viewport->camera_)
    {
        SetUniformVec3(data, shader, "CamPos", state.viewport->camera_->position_);
        SetUniformVec3(data, shader, "uCamPos", state.viewport->camera_->position_);
    }
    SetUniformRaw(data, shader, "ColorPickingID", colorPickingId);
    SetUniformRaw(data, shader, "uColorPickingID", colorPickingId);
    int spriteFlag = sprite ? 1 : 0;
    SetUniformRaw(data, shader, "uInstanced", spriteFlag);

    if (state.scene)
    {
        const hrl_fog_t& fog = state.scene->fog;
        SetUniformRaw(data, shader, "FogEnabled", static_cast<int>(fog.enabled));
        SetUniformRaw(data, shader, "uFogEnabled", static_cast<int>(fog.enabled));
        SetUniformRaw(data, shader, "FogMode", static_cast<int>(fog.mode));
        SetUniformRaw(data, shader, "uFogMode", static_cast<int>(fog.mode));
        glm::vec4 fc(fog.r, fog.g, fog.b, 1.0f);
        SetUniformRaw(data, shader, "FogColor", fc);
        SetUniformRaw(data, shader, "uFogColor", fc);
        SetUniformRaw(data, shader, "FogStart", fog.range_start);
        SetUniformRaw(data, shader, "FogEnd", fog.range_end);
        SetUniformRaw(data, shader, "FogDensity", fog.density);
        int dbg = static_cast<int>(state.scene->debug_view);
        SetUniformRaw(data, shader, "DebugView", dbg);
        SetUniformRaw(data, shader, "uDebugView", dbg);
        float threshold = 0.75f;
        SetUniformRaw(data, shader, "BrightThreshold", threshold);
        HRL_id envId = state.scene->environment_texture != HRL_INVALID_ID
            ? state.scene->environment_texture
            : state.scene->sky_texture;
        const int envEnabled = (state.scene->environment_mapping_enabled && envId != HRL_INVALID_ID && GetTexture(envId)) ? 1 : 0;
        SetUniformRaw(data, shader, "EnvironmentEnabled", envEnabled);
    }

    if (state.material)
    {
        glm::vec4 tint(1.0f);
        auto vit = state.material->vec4Params_.find("TintColor");
        if (vit != state.material->vec4Params_.end()) tint = vit->second;
        if (tintOverride) tint = *tintOverride;
        SetUniformRaw(data, shader, "TintColor", tint);
        SetUniformRaw(data, shader, "uTintColor", tint);
        float baseAlpha = 1.0f;
        auto bit = state.material->floatParams_.find("BaseColorAlpha");
        if (bit != state.material->floatParams_.end()) baseAlpha = bit->second;
        SetUniformRaw(data, shader, "BaseColorAlpha", baseAlpha);
        float roughness = 0.5f;
        auto rit = state.material->floatParams_.find("RoughnessValue");
        if (rit != state.material->floatParams_.end()) roughness = rit->second;
        float metallic = 0.0f;
        auto mit = state.material->floatParams_.find("MetallicValue");
        if (mit != state.material->floatParams_.end()) metallic = mit->second;
        float specular = 0.5f;
        auto sit = state.material->floatParams_.find("SpecularValue");
        if (sit != state.material->floatParams_.end()) specular = sit->second;
        float opacity = 1.0f;
        auto oit = state.material->floatParams_.find("OpacityValue");
        if (oit != state.material->floatParams_.end()) opacity = oit->second;
        SetUniformRaw(data, shader, "RoughnessValue", roughness);
        SetUniformRaw(data, shader, "MetallicValue", metallic);
        SetUniformRaw(data, shader, "SpecularValue", specular);
        SetUniformRaw(data, shader, "OpacityValue", opacity);
        const auto twoSidedIt = state.material->intParams_.find(HRL_MATERIAL_PARAM_TWO_SIDED);
        const int twoSided = (twoSidedIt != state.material->intParams_.end() && twoSidedIt->second != 0) ? 1 : 0;
        SetUniformRaw(data, shader, "TwoSided", twoSided);
        const int normalUseTexture = state.material->textureParams_.count("T_Normal") ? 1 : 0;
        SetUniformRaw(data, shader, "NormalUseTexture", normalUseTexture);
        SetUniformRaw(data, shader, "RoughnessUseValue", state.material->textureParams_.count("T_Roughness") ? 0 : 1);
        SetUniformRaw(data, shader, "MetallicUseValue", state.material->textureParams_.count("T_Metallic") ? 0 : 1);
        SetUniformRaw(data, shader, "SpecularUseValue", state.material->textureParams_.count("T_Specular") ? 0 : 1);
        SetUniformRaw(data, shader, "OpacityUseValue", state.material->textureParams_.count("T_Alpha") ? 0 : 1);
        float envStrength = 0.0f;
        auto esit = state.material->floatParams_.find("EnvironmentStrength");
        if (esit != state.material->floatParams_.end()) envStrength = esit->second;
        else if (state.scene && state.scene->environment_mapping_enabled) envStrength = 0.35f;
        SetUniformRaw(data, shader, "EnvironmentStrength", envStrength);

        for (const auto& [name, value] : state.material->intParams_) SetUniformRaw(data, shader, name.c_str(), value);
        for (const auto& [name, value] : state.material->floatParams_) SetUniformRaw(data, shader, name.c_str(), value);
        for (const auto& [name, value] : state.material->vec2Params_)
        {
            SetUniformRaw(data, shader, name.c_str(), value);
        }
        for (const auto& [name, value] : state.material->vec3Params_) SetUniformVec3(data, shader, name.c_str(), value);
        for (const auto& [name, value] : state.material->vec4Params_) SetUniformRaw(data, shader, name.c_str(), value);
    }

    if (mesh)
    {
        if (const HRL_SkeletalMesh* sk = dynamic_cast<const HRL_SkeletalMesh*>(mesh))
        {
            const VulkanUniformField* f = FindUniform(shader, "boneMatrices");
            if (f && f->type == VulkanUniformField::Type::Mat4Array)
            {
                const size_t count = std::min<size_t>(std::min<size_t>(sk->bone_matrices_.size(), f->array_count), HRL_MAX_SKELETAL_BONES);
                for (size_t i = 0; i < count; ++i)
                    std::memcpy(data.data() + f->offset + i * 64, &sk->bone_matrices_[i], sizeof(glm::mat4));
            }
        }
    }
}

static VkDescriptorSet AllocateDescriptorSet()
{
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g.descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g.descriptor_layout;
    if (vkAllocateDescriptorSets(g.device, &ai, &set) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return set;
}

static VkDescriptorSet BindResources(const HRL_Material* material, const VulkanShader* shader,
                                     const std::vector<HRL_id>* overrideTextures = nullptr, size_t uniformSize = 16)
{
    VkDescriptorSet set = AllocateDescriptorSet();
    if (!set) return VK_NULL_HANDLE;

    std::array<VkDescriptorImageInfo, kMaxSamplers> imageInfos{};
    std::array<VkWriteDescriptorSet, kMaxSamplers + 2> writes{};
    uint32_t writeCount = 0;

    std::array<HRL_id,kMaxSamplers> textureIds{};
    textureIds.fill(HRL_INVALID_ID);
    if (material)
    {
        for (const auto& [name, binding] : shader->samplers)
        {
            if (binding >= kMaxSamplers) continue;
            auto it = material->textureParams_.find(name);
            if (it != material->textureParams_.end()) textureIds[binding] = it->second;
        }
    }
    if (overrideTextures)
        for (size_t i = 0; i < std::min<size_t>(overrideTextures->size(), kMaxSamplers); ++i) textureIds[i] = (*overrideTextures)[i];

    // Vulkan descriptor sets are written for all fixed sampler slots; use the
    // dedicated 1x1 white texture for unused material samplers.
    const HRL_id fallback = g.fallback_texture;

    for (uint32_t binding = 0; binding < kMaxSamplers; ++binding)
    {
        HRL_id id = textureIds[binding] != HRL_INVALID_ID ? textureIds[binding] : fallback;
        const VulkanTexture* tex = GetTexture(id);
        if (!tex) continue;
        imageInfos[binding].sampler = tex->sampler;
        imageInfos[binding].imageView = tex->view;
        imageInfos[binding].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[writeCount] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[writeCount].dstSet = set;
        writes[writeCount].dstBinding = binding;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[writeCount].pImageInfo = &imageInfos[binding];
        ++writeCount;
    }

    VkDescriptorBufferInfo ub{};
    ub.buffer = g.uniform_buffer.buffer;
    ub.offset = 0;
    ub.range = std::max<VkDeviceSize>(16, static_cast<VkDeviceSize>(uniformSize));
    writes[writeCount] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[writeCount].dstSet = set;
    writes[writeCount].dstBinding = kUniformBinding;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[writeCount].pBufferInfo = &ub;
    ++writeCount;

    VkDescriptorBufferInfo lb{};
    lb.buffer = g.light_buffer.buffer;
    lb.offset = 0;
    lb.range = sizeof(LightBlockGPU);
    writes[writeCount] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[writeCount].dstSet = set;
    writes[writeCount].dstBinding = kLightBinding;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[writeCount].pBufferInfo = &lb;
    ++writeCount;

    vkUpdateDescriptorSets(g.device, writeCount, writes.data(), 0, nullptr);
    return set;
}

static VkPrimitiveTopology TopologyForMode(uint32_t mode)
{
    return mode == 3 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static bool CreatePipeline(const PipelineKey& key, VkRenderPass renderPass, VkPipeline& pipeline)
{
    pipeline = VK_NULL_HANDLE;
    const VulkanShader* shader = GetShader(key.shader);
    if (!shader || !shader->vertex || !shader->fragment) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shader->vertex;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shader->fragment;
    stages[1].pName = "main";

    std::array<VkVertexInputBindingDescription,2> bindings{};
    std::array<VkVertexInputAttributeDescription,7> attrs{};
    uint32_t bindingCount = 0;
    uint32_t attrCount = 0;
    if (key.mode == 9 || key.mode == 10)
    {
        if (key.mode == 10)
        {
            bindings[0] = {0, sizeof(HRL_SkeletalVertex), VK_VERTEX_INPUT_RATE_VERTEX};
            attrs[0] = {0,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, vertex) + offsetof(HRL_Vertex3D, position))};
            attrs[1] = {1,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, vertex) + offsetof(HRL_Vertex3D, normal))};
            attrs[2] = {2,0,VK_FORMAT_R32G32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, vertex) + offsetof(HRL_Vertex3D, uv))};
            attrs[3] = {3,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, vertex) + offsetof(HRL_Vertex3D, tangent))};
            attrs[4] = {4,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, vertex) + offsetof(HRL_Vertex3D, bitangent))};
            attrs[5] = {5,0,VK_FORMAT_R32G32B32A32_UINT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, boneIndices))};
            attrs[6] = {6,0,VK_FORMAT_R32G32B32A32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, boneWeights))};
            bindingCount = 1; attrCount = 7;
        }
        else
        {
            bindings[0] = {0, sizeof(HRL_Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX};
            attrs[0] = {0,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_Vertex3D, position))};
            attrs[1] = {1,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_Vertex3D, normal))};
            attrs[2] = {2,0,VK_FORMAT_R32G32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_Vertex3D, uv))};
            attrs[3] = {3,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_Vertex3D, tangent))};
            attrs[4] = {4,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_Vertex3D, bitangent))};
            bindingCount = 1; attrCount = 5;
        }
    }
    else if (key.mode == 3 || key.mode == 7)
    {
        bindings[0] = {0, sizeof(DebugVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        attrs[0] = {0,0,VK_FORMAT_R32G32B32_SFLOAT,0};
        attrs[1] = {1,0,VK_FORMAT_R32G32B32_SFLOAT,12};
        bindingCount = 1; attrCount = 2;
    }
    else if (key.mode == 8)
    {
        bindings[0] = {0, sizeof(HRL_Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX};
        attrs[0] = {0,0,VK_FORMAT_R32G32B32_SFLOAT,0};
        attrs[1] = {2,0,VK_FORMAT_R32G32_SFLOAT,24};
        bindings[1] = {1, sizeof(VFXRenderInstance), VK_VERTEX_INPUT_RATE_INSTANCE};
        attrs[2] = {3,1,VK_FORMAT_R32G32B32A32_SFLOAT,0};
        attrs[3] = {4,1,VK_FORMAT_R32G32B32A32_SFLOAT,16};
        attrs[4] = {5,1,VK_FORMAT_R32G32B32A32_SFLOAT,32};
        attrs[5] = {6,1,VK_FORMAT_R32G32B32A32_SFLOAT,48};
        attrs[6] = {7,1,VK_FORMAT_R32G32B32A32_SFLOAT,64};
        bindingCount = 2; attrCount = 7;
    }
    else if (key.mode == 4)
    {
        struct UIVertex { float p[2]; float uv[2]; float c[4]; };
        bindings[0] = {0, sizeof(UIVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        attrs[0] = {0,0,VK_FORMAT_R32G32_SFLOAT,0};
        attrs[1] = {1,0,VK_FORMAT_R32G32_SFLOAT,8};
        attrs[2] = {2,0,VK_FORMAT_R32G32B32A32_SFLOAT,16};
        bindingCount = 1; attrCount = 3;
    }
    else if (key.mode == 5 || key.mode == 6)
    {
        bindingCount = 0; attrCount = 0;
    }
    else if (key.mode == 1)
    {
        bindings[0] = {0, sizeof(HRL_SkeletalVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        constexpr uint32_t vb = 0;
        attrs[0] = {0,vb,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, vertex) + offsetof(HRL_Vertex3D, position))};
        attrs[1] = {1,vb,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, vertex) + offsetof(HRL_Vertex3D, normal))};
        attrs[2] = {2,vb,VK_FORMAT_R32G32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, vertex) + offsetof(HRL_Vertex3D, uv))};
        attrs[3] = {3,vb,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, vertex) + offsetof(HRL_Vertex3D, tangent))};
        attrs[4] = {4,vb,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, vertex) + offsetof(HRL_Vertex3D, bitangent))};
        attrs[5] = {5,vb,VK_FORMAT_R32G32B32A32_UINT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, boneIndices))};
        attrs[6] = {6,vb,VK_FORMAT_R32G32B32A32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_SkeletalVertex, boneWeights))};
        bindingCount = 1; attrCount = 7;
    }
    else
    {
        bindings[0] = {0, sizeof(HRL_Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX};
        attrs[0] = {0,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_Vertex3D, position))};
        attrs[1] = {1,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_Vertex3D, normal))};
        attrs[2] = {2,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_Vertex3D, uv))};
        attrs[3] = {3,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_Vertex3D, tangent))};
        attrs[4] = {4,0,VK_FORMAT_R32G32B32_SFLOAT,static_cast<uint32_t>(offsetof(HRL_Vertex3D, bitangent))};
        bindingCount = 1; attrCount = 5;
    }

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = bindingCount;
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = attrCount;
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = TopologyForMode(key.mode);
    ia.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.cullMode = key.cull == 2 ? VK_CULL_MODE_FRONT_BIT : (key.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    // Projection() flips NDC Y to match OpenGL's coordinate convention.
    // That reverses triangle winding in Vulkan framebuffer coordinates, so
    // the equivalent OpenGL GL_CCW front face is VK_FRONT_FACE_CLOCKWISE.
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = (key.mode == 5 || key.mode == 9 || key.mode == 10) ? VK_SAMPLE_COUNT_1_BIT : g.msaa;
    ms.sampleShadingEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = (key.mode == 0 || key.mode == 1 || key.mode == 8 || key.mode == 9 || key.mode == 10) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = ((key.mode == 0 || key.mode == 1 || key.mode == 9 || key.mode == 10) && key.blend == 0) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    std::array<VkPipelineColorBlendAttachmentState,3> cbAtt{};
    for (auto& a : cbAtt)
    {
        a.blendEnable = (key.blend != 0) ? VK_TRUE : VK_FALSE;
        a.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        a.colorBlendOp = VK_BLEND_OP_ADD;
        a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        a.alphaBlendOp = VK_BLEND_OP_ADD;
        if (key.blend == 2)
        {
            a.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        else if (key.blend == 3)
        {
            a.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            a.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        }
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = (key.mode == 5) ? 1u : ((key.mode == 9 || key.mode == 10) ? 0u : 3u);
    cb.pAttachments = cbAtt.data();

    std::array<VkDynamicState,2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynamicStates.data();

    VkPipelineRenderingCreateInfo dummyRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    (void)dummyRendering;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dyn;
    pi.layout = g.pipeline_layout;
    pi.renderPass = renderPass;
    pi.subpass = 0;

    VkResult r = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
    if (r != VK_SUCCESS) { VkError("vkCreateGraphicsPipelines", r); return false; }
    return true;
}

static VkPipeline GetPipeline(const PipelineKey& key, VkRenderPass renderPass)
{
    auto it = g.pipelines.find(key);
    if (it != g.pipelines.end()) return it->second;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!CreatePipeline(key, renderPass, pipeline)) return VK_NULL_HANDLE;
    g.pipelines.emplace(key, pipeline);
    return pipeline;
}

static bool ReadTextFile(const std::filesystem::path& path, std::string& contents)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream stream;
    stream << file.rdbuf();
    contents = stream.str();
    return !contents.empty();
}

static std::filesystem::path FindVulkanShaderDirectory()
{
    std::vector<std::filesystem::path> candidates;

    if (const char* explicitPath = std::getenv("HRL_VULKAN_SHADER_PATH"))
    {
        if (*explicitPath)
            candidates.emplace_back(explicitPath);
    }

    std::error_code ec;
    std::filesystem::path current = std::filesystem::current_path(ec);
    if (!ec)
    {
        for (int i = 0; i < 8 && !current.empty(); ++i)
        {
            candidates.push_back(current / "shaders" / "vulkan");
            candidates.push_back(current / "../shaders" / "vulkan");
            const auto parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
    }

    std::filesystem::path sourceDir = std::filesystem::path(__FILE__).parent_path();
    if (!sourceDir.empty())
    {
        candidates.push_back(sourceDir / "../../shaders/vulkan");
        candidates.push_back(sourceDir / "../../../shaders/vulkan");
    }

    for (const auto& candidate : candidates)
    {
        const auto normalized = candidate.lexically_normal();
        if (std::filesystem::exists(normalized / "vulkan_static_3dmesh.vert.glsl", ec) && !ec)
            return normalized;
        ec.clear();
    }

    return {};
}

static HRL_id CreateBuiltinShaderFromFiles(const char* name,
                                           const char* vertFile,
                                           const char* fragFile,
                                           HRL_id preferredId = HRL_INVALID_ID)
{
    const auto shaderDir = FindVulkanShaderDirectory();
    if (shaderDir.empty())
    {
        SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_ERROR,
                     std::string("Vulkan built-in shader ") + name + ": shaders/vulkan directory not found");
        return HRL_INVALID_ID;
    }

    std::string vert;
    std::string frag;
    if (!ReadTextFile(shaderDir / vertFile, vert) || !ReadTextFile(shaderDir / fragFile, frag))
    {
        SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_ERROR,
                     std::string("Vulkan built-in shader ") + name + ": failed to load " +
                     (shaderDir / vertFile).string() + " or " + (shaderDir / fragFile).string());
        return HRL_INVALID_ID;
    }

    const HRL_id id = preferredId != HRL_INVALID_ID ? preferredId : GenerateHRL_ID();
    VulkanShader shader;
    std::string error;
    if (!shader.Build(g.device, vert.data(), vert.size(), frag.data(), frag.size(), error))
    {
        SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_ERROR,
            std::string("Vulkan built-in shader ") + name + " [" + (shaderDir / vertFile).string() + ", " + (shaderDir / fragFile).string() + "]: " + error);
        return HRL_INVALID_ID;
    }
    g.shaders.emplace(id, std::move(shader));
    g.builtin_shader_ids.emplace(name, id);
    return id;
}

static bool CreateDescriptorInfrastructure()
{
    // Bindings are sparse: samplers 0..15, per-draw UBO at 31 and light UBO at 32.
    std::array<VkDescriptorSetLayoutBinding,18> bindings{};
    for (uint32_t i=0;i<kMaxSamplers;++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[16] = {kUniformBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[17] = {kLightBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(g.device, &li, nullptr, &g.descriptor_layout) != VK_SUCCESS) return false;

    std::array<VkDescriptorPoolSize,3> sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16u * 4096u},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096u},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096u}
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 4096;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(g.device, &pi, nullptr, &g.descriptor_pool) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g.descriptor_layout;
    if (vkCreatePipelineLayout(g.device, &pli, nullptr, &g.pipeline_layout) != VK_SUCCESS) return false;
    return true;
}

static bool CreateFrameBuffers()
{
    if (!CreateBuffer(g.uniform_capacity, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, g.uniform_buffer))
        return false;
    if (vkMapMemory(g.device, g.uniform_buffer.memory, 0, VK_WHOLE_SIZE, 0, &g.uniform_mapped) != VK_SUCCESS)
        return false;
    if (!CreateBuffer(sizeof(LightBlockGPU), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, g.light_buffer)) return false;
    if (vkMapMemory(g.device, g.light_buffer.memory, 0, sizeof(LightBlockGPU), 0, &g.light_mapped) != VK_SUCCESS) return false;
    if (!CreateBuffer(kDebugBufferCapacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, g.debug_buffer)) return false;
    if (vkMapMemory(g.device, g.debug_buffer.memory, 0, VK_WHOLE_SIZE, 0, &g.debug_mapped) != VK_SUCCESS) return false;
    if (!CreateBuffer(g.vfx_capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, g.vfx_buffer)) return false;
    if (vkMapMemory(g.device, g.vfx_buffer.memory, 0, VK_WHOLE_SIZE, 0, &g.vfx_mapped) != VK_SUCCESS) return false;
    const HRL_Vertex3D quadVertices[4] = {
        {{-0.5f,-0.5f,0.0f},{0,0,1},{0,0},{1,0,0},{0,1,0}},
        {{ 0.5f,-0.5f,0.0f},{0,0,1},{1,0},{1,0,0},{0,1,0}},
        {{ 0.5f, 0.5f,0.0f},{0,0,1},{1,1},{1,0,0},{0,1,0}},
        {{-0.5f, 0.5f,0.0f},{0,0,1},{0,1},{1,0,0},{0,1,0}}
    };
    const HRL_uint quadIndices[6] = {0,1,2,2,3,0};
    if (!UploadBuffer(quadVertices,sizeof(quadVertices),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,g.vfx_quad_vertex)) return false;
    if (!UploadBuffer(quadIndices,sizeof(quadIndices),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,g.vfx_quad_index)) return false;
    return true;
}

static bool ChoosePhysicalDevice()
{
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(g.instance, &count, nullptr) != VK_SUCCESS || count == 0)
    {
        SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "Vulkan: no physical device found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(g.instance, &count, devices.data());

    for (VkPhysicalDevice candidate : devices)
    {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        uint32_t gfx = VK_QUEUE_FAMILY_IGNORED;
        uint32_t present = VK_QUEUE_FAMILY_IGNORED;
        for (uint32_t i=0;i<familyCount;++i)
        {
            if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) gfx = i;
            if (g.surface)
            {
                VkBool32 supported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, g.surface, &supported);
                if (supported) present = i;
            }
        }
        if (gfx != VK_QUEUE_FAMILY_IGNORED && (!g.surface || present != VK_QUEUE_FAMILY_IGNORED))
        {
            g.physical = candidate;
            g.graphics_family = gfx;
            g.present_family = g.surface ? present : gfx;
            return true;
        }
    }
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "Vulkan: no suitable graphics/present queue family found");
    return false;
}

static bool CreateDevice()
{
    float priority = 1.0f;
    std::array<VkDeviceQueueCreateInfo,2> queueInfos{};
    uint32_t queueCount = 1;
    queueInfos[0] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfos[0].queueFamilyIndex = g.graphics_family;
    queueInfos[0].queueCount = 1;
    queueInfos[0].pQueuePriorities = &priority;
    if (g.present_family != g.graphics_family)
    {
        queueInfos[1] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfos[1].queueFamilyIndex = g.present_family;
        queueInfos[1].queueCount = 1;
        queueInfos[1].pQueuePriorities = &priority;
        queueCount = 2;
    }

    VkPhysicalDeviceFeatures features{};
    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(g.physical, &available);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(g.physical, &properties);
    g.sampler_anisotropy = available.samplerAnisotropy == VK_TRUE;
    g.max_anisotropy = g.sampler_anisotropy ? std::max(1.0f, properties.limits.maxSamplerAnisotropy) : 1.0f;
    if (g.sampler_anisotropy) features.samplerAnisotropy = VK_TRUE;

    std::vector<const char*> extensions;
    if (g.surface) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = queueCount;
    ci.pQueueCreateInfos = queueInfos.data();
    ci.pEnabledFeatures = &features;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    VkResult r = vkCreateDevice(g.physical, &ci, nullptr, &g.device);
    if (r != VK_SUCCESS) { VkError("vkCreateDevice", r); return false; }
    vkGetDeviceQueue(g.device, g.graphics_family, 0, &g.graphics_queue);
    if (g.present_family == g.graphics_family) g.present_queue = g.graphics_queue;
    else vkGetDeviceQueue(g.device, g.present_family, 0, &g.present_queue);

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(g.physical, &mem);
    (void)mem;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(g.physical, &p);
    g.uniform_alignment = static_cast<size_t>(std::max<VkDeviceSize>(1, p.limits.minUniformBufferOffsetAlignment));
    const VkSampleCountFlags sampleMask = p.limits.framebufferColorSampleCounts & p.limits.framebufferDepthSampleCounts;
    g.supported_samples = sampleMask | VK_SAMPLE_COUNT_1_BIT;
    return true;
}

static bool CreateCommandResources()
{
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = g.graphics_family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(g.device, &pi, nullptr, &g.command_pool) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g.command_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 2;
    std::array<VkCommandBuffer,2> cmds{};
    if (vkAllocateCommandBuffers(g.device, &ai, cmds.data()) != VK_SUCCESS) return false;
    g.frame_command = cmds[0];
    g.upload_command = cmds[1];

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(g.device, &fi, nullptr, &g.frame_fence) != VK_SUCCESS) return false;
    if (vkCreateFence(g.device, &fi, nullptr, &g.upload_fence) != VK_SUCCESS) return false;

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(g.device, &si, nullptr, &g.image_available) != VK_SUCCESS) return false;
    if (vkCreateSemaphore(g.device, &si, nullptr, &g.render_finished) != VK_SUCCESS) return false;
    return true;
}

static VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
{
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
    return formats.front();
}

static VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes)
{
    for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void DestroySwapchain();

static bool CreateSwapchain()
{
    if (!g.surface) return true;
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical, g.surface, &caps) != VK_SUCCESS) return false;
    uint32_t formatCount = 0, modeCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, g.surface, &formatCount, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.physical, g.surface, &modeCount, nullptr);
    if (formatCount == 0 || modeCount == 0) return false;
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, g.surface, &formatCount, formats.data());
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.physical, g.surface, &modeCount, modes.data());
    const VkSurfaceFormatKHR format = ChooseSurfaceFormat(formats);
    g.swapchain_format = format.format;
    if (caps.currentExtent.width != UINT32_MAX) g.swapchain_extent = caps.currentExtent;
    else
    {
        g.swapchain_extent.width = std::clamp<unsigned int>(GetWindowWidth(), caps.minImageExtent.width, caps.maxImageExtent.width);
        g.swapchain_extent.height = std::clamp<unsigned int>(GetWindowHeight(), caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g.surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = g.swapchain_format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = g.swapchain_extent;
    ci.imageArrayLayers = 1;
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
    {
        SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
            "Vulkan: swapchain does not support transfer-destination images");
        return false;
    }
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (g.graphics_family != g.present_family)
    {
        const uint32_t families[2] = {g.graphics_family, g.present_family};
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices = families;
    }
    else ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = ChoosePresentMode(modes);
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(g.device, &ci, nullptr, &g.swapchain) != VK_SUCCESS) return false;

    vkGetSwapchainImagesKHR(g.device, g.swapchain, &imageCount, nullptr);
    g.swapchain_images.resize(imageCount);
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &imageCount, g.swapchain_images.data());
    g.swapchain_views.resize(imageCount);
    for (uint32_t i=0;i<imageCount;++i)
    {
        if (!CreateImageView(g.swapchain_images[i], g.swapchain_format, VK_IMAGE_ASPECT_COLOR_BIT, g.swapchain_views[i]))
        {
            DestroySwapchain();
            return false;
        }
    }
    return true;
}

static bool CreateInstance()
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Horizon Rendering Library";
    app.applicationVersion = VK_MAKE_VERSION(0,6,0);
    app.pEngineName = "HRL";
    app.engineVersion = VK_MAKE_VERSION(0,6,0);
    app.apiVersion = VK_API_VERSION_1_0;

    std::vector<const char*> extensions;
    const bool wantsPresentation = g.create_surface_callback != nullptr;
    if (wantsPresentation)
    {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        for (const std::string& ext : g.instance_extensions)
            if (!ext.empty()) extensions.push_back(ext.c_str());
    }

    std::vector<const char*> unique;
    for (const char* ext : extensions)
    {
        if (!ext || !*ext) continue;
        if (std::find_if(unique.begin(), unique.end(), [&](const char* e){ return std::strcmp(e, ext) == 0; }) == unique.end())
            unique.push_back(ext);
    }

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(unique.size());
    ci.ppEnabledExtensionNames = unique.empty() ? nullptr : unique.data();
    VkResult r = vkCreateInstance(&ci, nullptr, &g.instance);
    if (r != VK_SUCCESS) { VkError("vkCreateInstance", r); return false; }
    g.instance_created = true;

    if (wantsPresentation)
    {
        r = static_cast<VkResult>(g.create_surface_callback(
            reinterpret_cast<void*>(g.instance),
            reinterpret_cast<void*>(&g.surface),
            g.surface_user_data));
        if (r != VK_SUCCESS || g.surface == VK_NULL_HANDLE)
        {
            VkError("Vulkan surface callback", r);
            return false;
        }
    }
    return true;
}

static void DestroySwapchain()
{
    if (!g.device) return;
    for (VkImageView view : g.swapchain_views) if (view) vkDestroyImageView(g.device, view, nullptr);
    g.swapchain_views.clear();
    g.swapchain_images.clear();
    if (g.swapchain) vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
    g.swapchain = VK_NULL_HANDLE;
}


static void UpdateDescriptorImage(VkDescriptorSet set, uint32_t binding, const ImageResource& image)
{
    if (!set || !image.view || !image.sampler) return;
    VkDescriptorImageInfo ii{};
    ii.sampler = image.sampler;
    ii.imageView = image.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g.device, 1, &w, 0, nullptr);
}

static uint32_t ShaderSamplerBinding(const VulkanShader* shader, const char* name, uint32_t fallback)
{
    if (!shader || !name) return fallback;
    auto it = shader->samplers.find(name);
    return it == shader->samplers.end() ? fallback : it->second;
}

static glm::vec3 GetLightDirection(const HRL_Light* light)
{
    if (!light) return glm::vec3(0,-1,0);
    const float pitch = glm::radians(light->rotation_.x);
    const float yaw = glm::radians(light->rotation_.y);
    glm::vec3 dir(std::cos(yaw)*std::cos(pitch), std::sin(pitch), std::sin(yaw)*std::cos(pitch));
    if (glm::length(dir) < 1e-5f) return glm::vec3(0,-1,0);
    return glm::normalize(dir);
}

static bool CreateDepthCubeImage(uint32_t width, uint32_t height, VkFormat format, ImageResource& out)
{
    out = {};
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.extent = {width, height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 6;
    ii.format = format;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(g.device, &ii, nullptr, &out.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g.device, out.image, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        DestroyImage(out);
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(g.device, &ai, nullptr, &out.memory) != VK_SUCCESS)
    {
        DestroyImage(out);
        return false;
    }
    if (vkBindImageMemory(g.device, out.image, out.memory, 0) != VK_SUCCESS)
    {
        DestroyImage(out);
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 6;
    if (vkCreateImageView(g.device, &vi, nullptr, &out.view) != VK_SUCCESS)
    {
        DestroyImage(out);
        return false;
    }

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(g.device, &si, nullptr, &out.sampler) != VK_SUCCESS)
    {
        DestroyImage(out);
        return false;
    }
    out.format = format;
    out.extent = {width, height};
    out.samples = VK_SAMPLE_COUNT_1_BIT;
    return true;
}

static bool CreateShadowRenderPass(VkFormat format, VkRenderPass& out)
{
    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &depthRef;

    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    return vkCreateRenderPass(g.device, &rp, nullptr, &out) == VK_SUCCESS;
}

static bool CreateShadowFaceView(VkImage image, VkFormat format, uint32_t layer, VkImageView& out)
{
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.baseArrayLayer = layer;
    vi.subresourceRange.layerCount = 1;
    vi.subresourceRange.levelCount = 1;
    return vkCreateImageView(g.device, &vi, nullptr, &out) == VK_SUCCESS;
}

static void DestroyShadowResource(HRL_id lightId)
{
    auto it = g.shadow_maps.find(lightId);
    if (it == g.shadow_maps.end()) return;
    for (VkFramebuffer fb : it->second.face_framebuffers)
        if (fb) vkDestroyFramebuffer(g.device, fb, nullptr);
    if (it->second.framebuffer)
        vkDestroyFramebuffer(g.device, it->second.framebuffer, nullptr);
    for (VkImageView view : it->second.face_views)
        if (view) vkDestroyImageView(g.device, view, nullptr);
    if (it->second.render_pass)
        vkDestroyRenderPass(g.device, it->second.render_pass, nullptr);
    DestroyImage(it->second.depth);
    g.shadow_maps.erase(it);
}

static bool EnsureShadowResource(HRL_Light* light)
{
    if (!light || !light->cast_shadows_) return false;
    if (light->type_ != HRL_POINT_LIGHT && light->type_ != HRL_DIRECTIONAL_LIGHT && light->type_ != HRL_SPOT_LIGHT)
        return false;

    const int resolution = std::clamp(light->shadow_resolution_, 128, 4096);
    const bool cube = light->type_ == HRL_POINT_LIGHT;
    auto it = g.shadow_maps.find(light->id_);
    if (it != g.shadow_maps.end() && it->second.resolution == resolution && it->second.cube == cube && it->second.depth.image)
        return true;
    if (it != g.shadow_maps.end()) DestroyShadowResource(light->id_);

    ShadowGPU shadow{};
    shadow.resolution = resolution;
    shadow.cube = cube;

    const VkFormat format = FindDepthFormat();
    const bool imageOk = cube
        ? CreateDepthCubeImage(resolution, resolution, format, shadow.depth)
        : CreateImage(resolution, resolution, format,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, shadow.depth);
    if (!imageOk) return false;
    if (!CreateShadowRenderPass(format, shadow.render_pass))
    {
        DestroyImage(shadow.depth);
        return false;
    }

    if (cube)
    {
        shadow.face_views.resize(6, VK_NULL_HANDLE);
        shadow.face_framebuffers.resize(6, VK_NULL_HANDLE);
        for (uint32_t face = 0; face < 6; ++face)
        {
            if (!CreateShadowFaceView(shadow.depth.image, format, face, shadow.face_views[face]))
            {
                for (VkFramebuffer fb : shadow.face_framebuffers) if (fb) vkDestroyFramebuffer(g.device, fb, nullptr);
                for (VkImageView view : shadow.face_views) if (view) vkDestroyImageView(g.device, view, nullptr);
                vkDestroyRenderPass(g.device, shadow.render_pass, nullptr);
                DestroyImage(shadow.depth);
                return false;
            }
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = shadow.render_pass;
            fb.attachmentCount = 1;
            fb.pAttachments = &shadow.face_views[face];
            fb.width = resolution;
            fb.height = resolution;
            fb.layers = 1;
            if (vkCreateFramebuffer(g.device, &fb, nullptr, &shadow.face_framebuffers[face]) != VK_SUCCESS)
            {
                for (VkFramebuffer existing : shadow.face_framebuffers) if (existing) vkDestroyFramebuffer(g.device, existing, nullptr);
                for (VkImageView view : shadow.face_views) if (view) vkDestroyImageView(g.device, view, nullptr);
                vkDestroyRenderPass(g.device, shadow.render_pass, nullptr);
                DestroyImage(shadow.depth);
                return false;
            }
        }
    }
    else
    {
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = shadow.render_pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &shadow.depth.view;
        fb.width = resolution;
        fb.height = resolution;
        fb.layers = 1;
        if (vkCreateFramebuffer(g.device, &fb, nullptr, &shadow.framebuffer) != VK_SUCCESS)
        {
            vkDestroyRenderPass(g.device, shadow.render_pass, nullptr);
            DestroyImage(shadow.depth);
            return false;
        }
    }

    shadow.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g.shadow_maps.emplace(light->id_, std::move(shadow));
    return true;
}

static glm::mat4 VulkanClipProjection(glm::mat4 proj)
{
    glm::mat4 zclip(1.0f);
    zclip[2][2] = 0.5f;
    zclip[3][2] = 0.5f;
    proj = zclip * proj;
    proj[1][1] *= -1.0f;
    return proj;
}

static glm::vec3 SceneShadowCenter(const hrl_scene_t* scene)
{
    if (!scene || scene->meshes.empty()) return glm::vec3(0.0f);

    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    bool found = false;

    for (const auto& [id, mesh] : scene->meshes)
    {
        if (!mesh) continue;
        const glm::mat4 model = ModelMatrix(mesh);
        const glm::vec3 localCenter = mesh->bounds_center_;
        const glm::vec3 localExtents(std::max(mesh->bounds_radius_, 0.5f));

        // Transform the eight corners so rotated/scaled meshes contribute
        // correctly to the directional light's orthographic shadow volume.
        for (int x = -1; x <= 1; x += 2)
        for (int y = -1; y <= 1; y += 2)
        for (int z = -1; z <= 1; z += 2)
        {
            const glm::vec3 corner = localCenter + glm::vec3(x, y, z) * localExtents;
            const glm::vec3 world = glm::vec3(model * glm::vec4(corner, 1.0f));
            boundsMin = glm::min(boundsMin, world);
            boundsMax = glm::max(boundsMax, world);
            found = true;
        }
    }

    return found ? (boundsMin + boundsMax) * 0.5f : glm::vec3(0.0f);
}

static glm::mat4 ShadowMatrixForLight(const HRL_Light* light, const glm::vec3& sceneCenter = glm::vec3(0.0f))
{
    if (!light) return glm::mat4(1.0f);
    const glm::vec3 dir = glm::normalize(GetLightDirection(light));
    const glm::vec3 up = std::abs(glm::dot(dir, glm::vec3(0,1,0))) > 0.98f ? glm::vec3(0,0,1) : glm::vec3(0,1,0);
    glm::mat4 view(1.0f);
    glm::mat4 proj(1.0f);
    if (light->type_ == HRL_SPOT_LIGHT)
    {
        const float outer = glm::clamp(light->outerCutoff, 1.0f, 89.0f);
        proj = glm::perspective(glm::radians(outer * 2.0f), 1.0f, 0.1f, 100.0f);
        view = glm::lookAt(light->position_, light->position_ + dir, up);
    }
    else if (light->type_ == HRL_DIRECTIONAL_LIGHT)
    {
        // Center the directional shadow volume around the actual scene instead
        // of assuming everything lives near the world origin.
        const glm::vec3 lp = sceneCenter - dir * 80.0f;
        proj = glm::ortho(-60.0f, 60.0f, -60.0f, 60.0f, 0.1f, 200.0f);
        view = glm::lookAt(lp, sceneCenter, up);
    }
    else
    {
        // Point-light shadow matrices are supplied per cubemap face elsewhere.
        return glm::mat4(1.0f);
    }

    proj = VulkanClipProjection(proj);
    const glm::mat4 bias = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f)) *
                           glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
    return bias * proj * view;
}

static void RenderShadowPassForFace(hrl_scene_t* scene, ShadowGPU& shadow, const HRL_Light* light,
                                    uint32_t face, const glm::mat4& lightMatrix)
{
    if (!scene || !light || !shadow.render_pass) return;
    const VkFramebuffer fb = shadow.cube ? shadow.face_framebuffers[face] : shadow.framebuffer;
    if (!fb) return;

    if (shadow.layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        TransitionImage(g.frame_command, shadow.depth.image, shadow.layout,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT,
                        shadow.cube ? 6u : 1u);
        shadow.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = shadow.render_pass;
    bi.framebuffer = fb;
    bi.renderArea.extent = {(uint32_t)shadow.resolution, (uint32_t)shadow.resolution};
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass(g.frame_command, &bi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0,0,(float)shadow.resolution,(float)shadow.resolution,0,1};
    VkRect2D sc{{0,0},{(uint32_t)shadow.resolution,(uint32_t)shadow.resolution}};
    vkCmdSetViewport(g.frame_command, 0, 1, &vp);
    vkCmdSetScissor(g.frame_command, 0, 1, &sc);

    for (const auto& [meshId, mesh] : scene->meshes)
    {
        if (!mesh || mesh->type_ == HRL_SPRITE) continue;
        const bool skinned = mesh->type_ == HRL_3D_SKELETAL_MESH;
        const HRL_id sid = g.builtin_shader_ids.at(skinned ? "shadow_skinned" : "shadow_static");
        const VulkanShader* shader = GetShader(sid);
        if (!shader) continue;
        PipelineKey key{sid, skinned ? 10u : 9u, 0u, 0u, shadow.render_pass};
        VkPipeline pipeline = GetPipeline(key, shadow.render_pass);
        if (!pipeline) continue;

        std::vector<unsigned char> uniform(shader->uniform_block_size, 0);
        SetUniformMat(uniform, shader, "lightSpaceMatrix", lightMatrix);
        SetUniformMat(uniform, shader, "model", ModelMatrix(mesh));
        if (skinned)
        {
            const auto* sm = dynamic_cast<const HRL_SkeletalMesh*>(mesh);
            const VulkanUniformField* f = FindUniform(shader, "boneMatrices");
            if (sm && f)
            {
                const size_t n = std::min<size_t>(std::min<size_t>(sm->bone_matrices_.size(), f->array_count), HRL_MAX_SKELETAL_BONES);
                for (size_t i=0; i<n; ++i)
                    std::memcpy(uniform.data() + f->offset + i*64, &sm->bone_matrices_[i], sizeof(glm::mat4));
            }
        }
        if (light->type_ == HRL_POINT_LIGHT)
        {
            SetUniformRaw(uniform, shader, "uPointShadow", 1);
            SetUniformVec3(uniform, shader, "uLightPosition", light->position_);
            SetUniformRaw(uniform, shader, "uFarPlane", 100.0f);
        }
        else
        {
            SetUniformRaw(uniform, shader, "uPointShadow", 0);
            SetUniformVec3(uniform, shader, "uLightPosition", light->position_);
            SetUniformRaw(uniform, shader, "uFarPlane", 100.0f);
        }
        const size_t off = AllocateUniform(uniform.size(), uniform.data());
        if (off == std::numeric_limits<size_t>::max()) continue;
        VkDescriptorSet ds = BindResources(nullptr, shader, nullptr, uniform.size());
        if (!ds) continue;

        vkCmdBindPipeline(g.frame_command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const uint32_t dyn = static_cast<uint32_t>(off);
        vkCmdBindDescriptorSets(g.frame_command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                g.pipeline_layout, 0, 1, &ds, 1, &dyn);

        auto gpuIt = g.meshes.find(meshId);
        if (gpuIt == g.meshes.end()) continue;
        MeshGPU& gpu = gpuIt->second;
        VkDeviceSize zero = 0;
        if (skinned)
        {
            if (!gpu.skeletal_vertex.buffer || gpu.skeletal_vertex_count == 0) continue;
            vkCmdBindVertexBuffers(g.frame_command, 0, 1, &gpu.skeletal_vertex.buffer, &zero);
            if (gpu.skeletal_index.buffer && gpu.skeletal_index_count)
            {
                vkCmdBindIndexBuffer(g.frame_command, gpu.skeletal_index.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(g.frame_command, gpu.skeletal_index_count, 1, 0, 0, 0);
            }
            else vkCmdDraw(g.frame_command, gpu.skeletal_vertex_count, 1, 0, 0);
        }
        else if (!skinned && !gpu.levels.empty())
        {
            const MeshLevelGPU& lvl = gpu.levels[0];
            if (!lvl.vertex.buffer || !lvl.vertex_count) continue;
            vkCmdBindVertexBuffers(g.frame_command, 0, 1, &lvl.vertex.buffer, &zero);
            if (lvl.index.buffer && lvl.index_count)
            {
                vkCmdBindIndexBuffer(g.frame_command, lvl.index.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(g.frame_command, lvl.index_count, 1, 0, 0, 0);
            }
            else vkCmdDraw(g.frame_command, lvl.vertex_count, 1, 0, 0);
        }
    }
    vkCmdEndRenderPass(g.frame_command);
    shadow.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

static void PrepareSceneShadows(hrl_scene_t* scene, HRL_id sceneId)
{
    auto& slots = g.active_shadow_slots_by_scene[sceneId];
    slots.clear();
    if (!scene) return;

    int slot = 0;
    for (const auto& [id, light] : scene->lights)
    {
        if (!light || !light->cast_shadows_ || slot >= 4) continue;
        if (!EnsureShadowResource(light)) continue;
        slots[id] = slot++;

        ShadowGPU& shadow = g.shadow_maps.at(id);
        if (light->type_ == HRL_POINT_LIGHT)
        {
            static const glm::vec3 directions[6] = {
                { 1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0,-1, 0}, {0,0, 1}, {0,0,-1}
            };
            static const glm::vec3 ups[6] = {
                {0,-1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}, {0,-1,0}, {0,-1,0}
            };
            const glm::mat4 proj = VulkanClipProjection(glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, 100.0f));
            for (uint32_t face=0; face<6; ++face)
            {
                const glm::mat4 view = glm::lookAt(light->position_, light->position_ + directions[face], ups[face]);
                RenderShadowPassForFace(scene, shadow, light, face, proj * view);
            }
        }
        else
        {
            RenderShadowPassForFace(scene, shadow, light, 0, ShadowMatrixForLight(light, SceneShadowCenter(scene)));
        }

        if (shadow.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            TransitionImage(g.frame_command, shadow.depth.image,
                            shadow.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT,
                            shadow.cube ? 6u : 1u);
            shadow.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }

    // Release resources that are no longer active for this scene (disabled, unsupported, or over the four public shadow slots).
    std::vector<HRL_id> stale;
    stale.reserve(scene->lights.size());
    for (const auto& [id, light] : scene->lights)
    {
        if (g.shadow_maps.find(id) != g.shadow_maps.end() && slots.find(id) == slots.end())
            stale.push_back(id);
    }
    for (HRL_id id : stale) DestroyShadowResource(id);
}

static bool EnsurePostResources(SceneGPU& scene)
{
    if (!scene.post_a.image)
    {
        if (!CreateImage(scene.width, scene.height, scene.color_format,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, scene.post_a)) return false;
    }
    if (!scene.post_b.image)
    {
        if (!CreateImage(scene.width, scene.height, scene.color_format,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, scene.post_b)) return false;
    }
    if (!scene.post_render_pass)
    {
        VkAttachmentDescription att{0,scene.color_format,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_STORE,
                                    VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rp.attachmentCount=1;rp.pAttachments=&att;rp.subpassCount=1;rp.pSubpasses=&sub;
        if(vkCreateRenderPass(g.device,&rp,nullptr,&scene.post_render_pass)!=VK_SUCCESS)return false;
    }
    if (!scene.post_framebuffer_a)
    {
        VkImageView v=scene.post_a.view; VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=scene.post_render_pass;fb.attachmentCount=1;fb.pAttachments=&v;fb.width=scene.width;fb.height=scene.height;fb.layers=1;
        if(vkCreateFramebuffer(g.device,&fb,nullptr,&scene.post_framebuffer_a)!=VK_SUCCESS)return false;
    }
    if (!scene.post_framebuffer_b)
    {
        VkImageView v=scene.post_b.view; VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=scene.post_render_pass;fb.attachmentCount=1;fb.pAttachments=&v;fb.width=scene.width;fb.height=scene.height;fb.layers=1;
        if(vkCreateFramebuffer(g.device,&fb,nullptr,&scene.post_framebuffer_b)!=VK_SUCCESS)return false;
    }
    return true;
}

static bool CreateSceneTargets(SceneGPU& scene)
{
    const VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags pickUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (scene.samples == VK_SAMPLE_COUNT_1_BIT)
    {
        if(!CreateImage(scene.width,scene.height,scene.color_format,colorUsage,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_COLOR_BIT,scene.color))return false;
        if(!CreateImage(scene.width,scene.height,scene.color_format,colorUsage,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_COLOR_BIT,scene.bright))return false;
        if(!CreateImage(scene.width,scene.height,scene.picking_format,pickUsage,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_COLOR_BIT,scene.picking_image))return false;
        if(!CreateImage(scene.width,scene.height,scene.depth_format,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,scene.depth))return false;
    }
    else
    {
        if(!CreateImage(scene.width,scene.height,scene.color_format,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,VK_SAMPLE_COUNT_1_BIT==VK_SAMPLE_COUNT_1_BIT?scene.samples:scene.samples,VK_IMAGE_ASPECT_COLOR_BIT,scene.color_msaa))return false;
        if(!CreateImage(scene.width,scene.height,scene.color_format,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,scene.samples,VK_IMAGE_ASPECT_COLOR_BIT,scene.bright_msaa))return false;
        if(!CreateImage(scene.width,scene.height,scene.picking_format,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,scene.samples,VK_IMAGE_ASPECT_COLOR_BIT,scene.picking_msaa))return false;
        if(!CreateImage(scene.width,scene.height,scene.depth_format,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,scene.samples,VK_IMAGE_ASPECT_DEPTH_BIT,scene.depth_msaa))return false;
        if(!CreateImage(scene.width,scene.height,scene.color_format,colorUsage,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_COLOR_BIT,scene.color))return false;
        if(!CreateImage(scene.width,scene.height,scene.color_format,colorUsage,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_COLOR_BIT,scene.bright))return false;
        if(!CreateImage(scene.width,scene.height,scene.picking_format,pickUsage,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_COLOR_BIT,scene.picking_image))return false;
        if(!CreateImage(scene.width,scene.height,scene.depth_format,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,scene.depth_resolve))return false;
    }
    std::array<VkAttachmentDescription,7> at{};uint32_t n=4;
    at[0]={0,scene.color_format,scene.samples,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    at[1]={0,scene.color_format,scene.samples,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    at[2]={0,scene.picking_format,scene.samples,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    at[3]={0,scene.depth_format,scene.samples,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference colors[3]={{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{2,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};VkAttachmentReference depth{3,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};VkAttachmentReference resolves[3]={{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    if(scene.samples>VK_SAMPLE_COUNT_1_BIT){at[4]={0,scene.color_format,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};at[5]={0,scene.color_format,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};at[6]={0,scene.picking_format,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};resolves[0]={4,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};resolves[1]={5,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};resolves[2]={6,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};n=7;}
    VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=3;sub.pColorAttachments=colors;sub.pResolveAttachments=scene.samples>VK_SAMPLE_COUNT_1_BIT?resolves:nullptr;sub.pDepthStencilAttachment=&depth;VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=n;rp.pAttachments=at.data();rp.subpassCount=1;rp.pSubpasses=&sub;if(vkCreateRenderPass(g.device,&rp,nullptr,&scene.render_pass)!=VK_SUCCESS)return false;
    std::array<VkImageView,7> views{};if(scene.samples>VK_SAMPLE_COUNT_1_BIT){views[0]=scene.color_msaa.view;views[1]=scene.bright_msaa.view;views[2]=scene.picking_msaa.view;views[3]=scene.depth_msaa.view;views[4]=scene.color.view;views[5]=scene.bright.view;views[6]=scene.picking_image.view;}else{views[0]=scene.color.view;views[1]=scene.bright.view;views[2]=scene.picking_image.view;views[3]=scene.depth.view;}
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=scene.render_pass;fb.attachmentCount=n;fb.pAttachments=views.data();fb.width=scene.width;fb.height=scene.height;fb.layers=1;if(vkCreateFramebuffer(g.device,&fb,nullptr,&scene.framebuffer)!=VK_SUCCESS)return false;
    scene.color_layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;scene.depth_resolve_layout=VK_IMAGE_LAYOUT_UNDEFINED;scene.post_a_layout=VK_IMAGE_LAYOUT_UNDEFINED;scene.post_b_layout=VK_IMAGE_LAYOUT_UNDEFINED;
    return EnsurePostResources(scene);
}

static void DestroySceneResources(SceneGPU& scene)
{
    if(scene.post_framebuffer_a)vkDestroyFramebuffer(g.device,scene.post_framebuffer_a,nullptr);
    if(scene.post_framebuffer_b)vkDestroyFramebuffer(g.device,scene.post_framebuffer_b,nullptr);
    if(scene.post_render_pass)vkDestroyRenderPass(g.device,scene.post_render_pass,nullptr);
    if(scene.framebuffer)vkDestroyFramebuffer(g.device,scene.framebuffer,nullptr);
    if(scene.render_pass)vkDestroyRenderPass(g.device,scene.render_pass,nullptr);
    scene.post_framebuffer_a=scene.post_framebuffer_b=VK_NULL_HANDLE;scene.post_render_pass=VK_NULL_HANDLE;scene.framebuffer=VK_NULL_HANDLE;scene.render_pass=VK_NULL_HANDLE;
    DestroyImage(scene.color);DestroyImage(scene.bright);DestroyImage(scene.picking_image);DestroyImage(scene.depth);DestroyImage(scene.color_msaa);DestroyImage(scene.bright_msaa);DestroyImage(scene.picking_msaa);DestroyImage(scene.depth_msaa);DestroyImage(scene.depth_resolve);DestroyImage(scene.post_a);DestroyImage(scene.post_b);
    scene.color_layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;scene.depth_resolve_layout=VK_IMAGE_LAYOUT_UNDEFINED;scene.post_a_layout=VK_IMAGE_LAYOUT_UNDEFINED;scene.post_b_layout=VK_IMAGE_LAYOUT_UNDEFINED;
}

static bool RecreateScene(SceneGPU& scene)
{
    if (g.device) vkDeviceWaitIdle(g.device);
    DestroySceneResources(scene);
    return CreateSceneTargets(scene);
}

static void SetViewportAndScissor(VkCommandBuffer cmd, const HRL_Viewport* viewport, int width, int height)
{
    VkViewport vp{};
    vp.x = viewport->x_ * width;
    vp.y = (1.0f - viewport->y_ - viewport->height_) * height;
    vp.width = std::max(1.0f, viewport->width_ * width);
    vp.height = std::max(1.0f, viewport->height_ * height);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{};
    sc.offset.x = std::max(0, static_cast<int32_t>(std::floor(vp.x)));
    sc.offset.y = std::max(0, static_cast<int32_t>(std::floor(vp.y)));
    sc.extent.width = std::max(1u, static_cast<uint32_t>(std::ceil(vp.width)));
    sc.extent.height = std::max(1u, static_cast<uint32_t>(std::ceil(vp.height)));
    if (sc.offset.x + static_cast<int32_t>(sc.extent.width) > width) sc.extent.width = width - sc.offset.x;
    if (sc.offset.y + static_cast<int32_t>(sc.extent.height) > height) sc.extent.height = height - sc.offset.y;
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

static void ClearViewport(VkCommandBuffer cmd, const HRL_Viewport* viewport, int width, int height)
{
    VkViewport vp{};
    vp.x = viewport->x_ * width;
    vp.y = (1.0f - viewport->y_ - viewport->height_) * height;
    vp.width = std::max(1.0f, viewport->width_ * width);
    vp.height = std::max(1.0f, viewport->height_ * height);
    VkClearAttachment clear[4]{};
    clear[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear[0].colorAttachment = 0;
    clear[0].clearValue.color = {{0.0f,0.0f,0.0f,1.0f}};
    clear[1].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear[1].colorAttachment = 1;
    clear[1].clearValue.color = {{0.0f,0.0f,0.0f,1.0f}};
    clear[2].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear[2].colorAttachment = 2;
    clear[2].clearValue.color = {{0.0f,0.0f,0.0f,1.0f}};
    clear[3].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    clear[3].clearValue.depthStencil = {1.0f,0};
    VkClearRect rect{};
    rect.rect.offset.x = std::max(0, static_cast<int32_t>(std::floor(vp.x)));
    rect.rect.offset.y = std::max(0, static_cast<int32_t>(std::floor(vp.y)));
    rect.rect.extent.width = std::max(1u, static_cast<uint32_t>(std::ceil(vp.width)));
    rect.rect.extent.height = std::max(1u, static_cast<uint32_t>(std::ceil(vp.height)));
    rect.baseArrayLayer = 0; rect.layerCount = 1;
    vkCmdClearAttachments(cmd, 4, clear, 1, &rect);
}

static void DrawBuiltinSky(VkCommandBuffer cmd, const SceneDrawState& state, const hrl_scene_t* scene)
{
    auto it = g.builtin_shader_ids.find("sky");
    if (it == g.builtin_shader_ids.end() || !scene->sky_sphere_enabled) return;
    const VulkanShader* shader = GetShader(it->second);
    PipelineKey key{it->second,6,0,0,g.scenes.at(state.scene_id).render_pass};
    VkPipeline pipeline = GetPipeline(key, g.scenes.at(state.scene_id).render_pass);
    if (!pipeline) return;

    std::vector<unsigned char> data(shader->uniform_block_size, 0);
    SetUniformMat(data, shader, "projection", state.projection);
    SetUniformMat(data, shader, "view", state.view);
    glm::mat4 rot(1.0f);
    rot = glm::rotate(rot, glm::radians(scene->sky_rotation.x), glm::vec3(1,0,0));
    rot = glm::rotate(rot, glm::radians(scene->sky_rotation.y), glm::vec3(0,1,0));
    rot = glm::rotate(rot, glm::radians(scene->sky_rotation.z), glm::vec3(0,0,1));
    SetUniformMat(data, shader, "skyRotation", rot);
    SetUniformVec3(data, shader, "SkyTopColor", scene->sky_top_color);
    SetUniformVec3(data, shader, "SkyHorizonColor", scene->sky_horizon_color);
    SetUniformVec3(data, shader, "SkyBottomColor", scene->sky_bottom_color);
    int tex = (scene->sky_texture != HRL_INVALID_ID && GetTexture(scene->sky_texture)) ? 1 : 0;
    SetUniformRaw(data, shader, "SkyUseTexture", tex);
    const size_t offset = AllocateUniform(data.size(), data.data());
    if (offset == std::numeric_limits<size_t>::max()) return;
    std::vector<HRL_id> skyTextures;
    if (scene->sky_texture != HRL_INVALID_ID && GetTexture(scene->sky_texture))
        skyTextures.push_back(scene->sky_texture);
    VkDescriptorSet ds = BindResources(nullptr, shader, skyTextures.empty() ? nullptr : &skyTextures, data.size());
    if (!ds) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const uint32_t dynamicOffset = static_cast<uint32_t>(offset);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline_layout, 0, 1, &ds, 1, &dynamicOffset);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

static void DrawMesh(VkCommandBuffer cmd, const SceneDrawState& state, HRL_Mesh* mesh, int lodLevel, uint32_t colorId)
{
    if (!mesh || mesh->material_ == HRL_INVALID_ID) return;
    auto mit = g.meshes.find(mesh->scene_ == HRL_INVALID_ID ? HRL_INVALID_ID : static_cast<HRL_id>(0));
    (void)mit;
}

static uint32_t IdToColor(HRL_id id) { return id; }

static HRL_id ResolveBuiltinMeshShader(const HRL_Mesh* mesh, HRL_id requestedShader)
{
    if (!mesh) return requestedShader;

    // The FBX automatic-material path can select the skeletal built-in shader
    // for a material when the source file contains both static and skinned
    // geometry. A static vertex buffer cannot be consumed by the skeletal
    // pipeline (locations 5/6 are absent), so force reserved built-ins to match
    // the actual mesh type. Custom shaders are never changed.
    if (mesh->type_ == HRL_3D_MESH && requestedShader == HRL_SKINNED_3D_MESH_SHADER)
        return HRL_MESH_3D_SHADER;
    if (mesh->type_ == HRL_3D_SKELETAL_MESH && requestedShader == HRL_MESH_3D_SHADER)
        return HRL_SKINNED_3D_MESH_SHADER;
    return requestedShader;
}

static void UpdateMeshShadowDescriptors(VkDescriptorSet ds, const VulkanShader* shader, HRL_id sceneId);

static void DrawOneMesh(VkCommandBuffer cmd, const SceneDrawState& state, HRL_id meshId, HRL_Mesh* mesh, int lodLevel)
{
    if (!mesh) return;
    auto matIt = GetPrivateContext()->materials.find(mesh->material_);
    if (matIt == GetPrivateContext()->materials.end() || !matIt->second) return;
    HRL_Material* material = matIt->second;

    const bool skeletal = mesh->type_ == HRL_3D_SKELETAL_MESH;
    const bool sprite = mesh->type_ == HRL_SPRITE;
    const HRL_id effectiveShaderId = ResolveBuiltinMeshShader(mesh, material->shader_);
    const VulkanShader* shader = GetShader(effectiveShaderId);
    auto gpuIt = g.meshes.find(meshId);
    if (!shader || gpuIt == g.meshes.end()) return;
    MeshGPU& gpu = gpuIt->second;
    if (skeletal != gpu.skeletal) return;

    const uint32_t mode = skeletal ? 1u : (sprite ? 2u : 0u);
    bool twoSided = false;
    auto ts = material->intParams_.find(HRL_MATERIAL_PARAM_TWO_SIDED);
    if (ts != material->intParams_.end()) twoSided = ts->second != 0;
    const bool blend = sprite;
    auto sceneIt = g.scenes.find(mesh->scene_);
    if (sceneIt == g.scenes.end() || !sceneIt->second.render_pass) return;

    // Built-in mesh pipelines intentionally disable culling. This removes the
    // dependency on an imported winding convention while the Vulkan clip-space
    // conversion differs from OpenGL. The fragment shader still honors
    // TwoSided when deciding whether to flip the lighting normal.
    const bool builtinMeshShader =
        effectiveShaderId == HRL_MESH_3D_SHADER ||
        effectiveShaderId == HRL_SKINNED_3D_MESH_SHADER;
    const uint32_t cullMode = (builtinMeshShader && !sprite)
        ? 0u
        : (twoSided ? 0u : 1u);

    PipelineKey key{effectiveShaderId, mode, cullMode, blend ? 1u : 0u, sceneIt->second.render_pass};
    const VkPipeline pipeline = GetPipeline(key, key.render_pass);
    if (!pipeline) return;

    std::vector<unsigned char> uniform(shader->uniform_block_size, 0);
    // DrawOneMesh resolves the concrete material locally, but SceneDrawState is
    // shared by the viewport pass. Populate a per-draw copy before filling the
    // material uniforms; otherwise TintColor/alpha/PBR values stay zero and the
    // built-in mesh fragment shader discards every fragment.
    SceneDrawState drawState = state;
    drawState.material = material;
    FillUniforms(shader, drawState, mesh, uniform, IdToColor(meshId), sprite);
    const size_t offset = AllocateUniform(uniform.size(), uniform.data());
    if (offset == std::numeric_limits<size_t>::max()) return;
    VkDescriptorSet ds = BindResources(material, shader, nullptr, uniform.size());
    if (!ds) return;
    if (state.scene)
    {
        HRL_id envId = state.scene->environment_texture != HRL_INVALID_ID
            ? state.scene->environment_texture
            : state.scene->sky_texture;
        const uint32_t envBinding = ShaderSamplerBinding(shader, "EnvironmentMap", 15);
        if (state.scene->environment_mapping_enabled && envId != HRL_INVALID_ID)
            if (const VulkanTexture* env = GetTexture(envId))
            {
                VkDescriptorImageInfo ii{}; ii.sampler=env->sampler; ii.imageView=env->view; ii.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet=ds; w.dstBinding=envBinding; w.descriptorCount=1;
                w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&ii;
                vkUpdateDescriptorSets(g.device,1,&w,0,nullptr);
            }
        UpdateMeshShadowDescriptors(ds, shader, state.scene_id);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const uint32_t dynamicOffset = static_cast<uint32_t>(offset);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline_layout, 0, 1, &ds, 1, &dynamicOffset);

    if (skeletal)
    {
        if (!gpu.skeletal_vertex.buffer || gpu.skeletal_vertex_count == 0) return;
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &gpu.skeletal_vertex.buffer, &zero);
        if (gpu.skeletal_index.buffer && gpu.skeletal_index_count > 0)
        {
            vkCmdBindIndexBuffer(cmd, gpu.skeletal_index.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, gpu.skeletal_index_count, 1, 0, 0, 0);
        }
        else
        {
            vkCmdDraw(cmd, gpu.skeletal_vertex_count, 1, 0, 0);
        }
    }
    else
    {
        if (gpu.levels.empty()) return;
        const size_t level = std::clamp<size_t>(static_cast<size_t>(std::max(0,lodLevel)), 0, gpu.levels.size()-1);
        const MeshLevelGPU& lvl = gpu.levels[level];
        if (!lvl.vertex.buffer || lvl.vertex_count == 0) return;
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &lvl.vertex.buffer, &zero);
        if (lvl.index.buffer && lvl.index_count > 0)
        {
            vkCmdBindIndexBuffer(cmd, lvl.index.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, lvl.index_count, 1, 0, 0, 0);
        }
        else
        {
            vkCmdDraw(cmd, lvl.vertex_count, 1, 0, 0);
        }
    }
}



static glm::mat4 MakeVFXSystemMatrix(const HRL_VFXSystem* system)
{
    glm::mat4 m(1.0f);
    m = glm::translate(m, system->position_);
    m = glm::rotate(m, glm::radians(system->rotation_.x), glm::vec3(1.0f,0.0f,0.0f));
    m = glm::rotate(m, glm::radians(system->rotation_.y), glm::vec3(0.0f,1.0f,0.0f));
    m = glm::rotate(m, glm::radians(system->rotation_.z), glm::vec3(0.0f,0.0f,1.0f));
    m = glm::scale(m, system->scale_);
    return m;
}

static glm::mat4 MakeVFXParticleModel(const HRL_VFXParticle& p, const glm::mat4& systemMatrix,
                                       const glm::mat4& billboardBasis, bool stretched, float stretch)
{
    glm::vec3 worldPos = p.position;
    if (systemMatrix != glm::mat4(1.0f)) worldPos = glm::vec3(systemMatrix * glm::vec4(worldPos,1.0f));
    glm::mat4 model = glm::translate(glm::mat4(1.0f), worldPos);
    glm::mat4 orient = billboardBasis;
    if (stretched)
    {
        const glm::vec3 viewRight = glm::normalize(glm::vec3(billboardBasis[0]));
        const glm::vec3 viewUp = glm::normalize(glm::vec3(billboardBasis[1]));
        const glm::vec3 normal = glm::normalize(glm::vec3(billboardBasis[2]));
        const glm::vec3 projected = p.velocity - normal * glm::dot(p.velocity, normal);
        const float projectedLength = glm::length(projected);
        if (projectedLength > 1e-4f)
        {
            const glm::vec3 pv = projected / projectedLength;
            const float angle = std::atan2(glm::dot(pv, viewUp), glm::dot(pv, viewRight));
            orient = glm::rotate(orient, angle, normal);
        }
    }
    else
    {
        orient = glm::rotate(orient, glm::radians(p.rotation.z), glm::vec3(billboardBasis[2]));
    }
    model *= orient;
    float sx = std::max(0.0f, p.size.x);
    float sy = std::max(0.0f, p.size.y);
    if (stretched) sx += glm::length(p.velocity) * std::max(0.0f, stretch);
    return glm::scale(model, glm::vec3(sx,sy,1.0f));
}

static HRL_id ResolveVFXTexture(const HRL_VFXEmitter* emitter)
{
    if (!emitter) return HRL_INVALID_ID;
    HRL_id textureId = emitter->texture_;
    if (textureId == HRL_INVALID_ID && emitter->material_ != HRL_INVALID_ID)
    {
        auto matIt = GetPrivateContext()->materials.find(emitter->material_);
        if (matIt != GetPrivateContext()->materials.end() && matIt->second && !matIt->second->textureParams_.empty())
            textureId = matIt->second->textureParams_.begin()->second;
    }
    if (textureId != HRL_INVALID_ID && GetTexture(textureId)) return textureId;
    return g.fallback_texture;
}

static glm::mat4 MakeVFXMeshParticleModel(const HRL_VFXParticle& p, const HRL_VFXEmitter* emitter,
                                           const HRL_Mesh* mesh, const glm::mat4& systemMatrix)
{
    glm::vec3 worldPos = p.position;
    if (emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL)
        worldPos = glm::vec3(systemMatrix * glm::vec4(worldPos,1.0f));

    glm::mat4 model = glm::translate(glm::mat4(1.0f), worldPos);
    if (mesh) model = glm::translate(model, mesh->pivot_point_);
    model = glm::rotate(model, glm::radians(p.rotation.x), glm::vec3(1,0,0));
    model = glm::rotate(model, glm::radians(p.rotation.y), glm::vec3(0,1,0));
    model = glm::rotate(model, glm::radians(p.rotation.z), glm::vec3(0,0,1));
    model = glm::rotate(model, glm::radians(emitter->mesh_rotation_offset_.x), glm::vec3(1,0,0));
    model = glm::rotate(model, glm::radians(emitter->mesh_rotation_offset_.y), glm::vec3(0,1,0));
    model = glm::rotate(model, glm::radians(emitter->mesh_rotation_offset_.z), glm::vec3(0,0,1));
    if (mesh) model = glm::translate(model, -mesh->pivot_point_);
    const glm::vec3 particleScale(std::max(0.0f,p.size.x), std::max(0.0f,p.size.y),
                                   std::max(0.0f,0.5f*(p.size.x+p.size.y)));
    const glm::vec3 baseScale = mesh ? mesh->scale_ : glm::vec3(1.0f);
    model = glm::scale(model, baseScale * emitter->mesh_scale_ * particleScale);
    return model;
}

static void DrawVFXMeshParticles(VkCommandBuffer cmd, const SceneDrawState& state, const hrl_scene_t* scene)
{
    if (!scene) return;
    const VkRenderPass renderPass = g.scenes.at(state.scene_id).render_pass;
    HRL_Context* context = GetPrivateContext();

    for (const auto& [sid, system] : scene->vfx_systems)
    {
        (void)sid;
        if (!system || !system->enabled_) continue;
        const glm::mat4 systemMatrix = MakeVFXSystemMatrix(system);
        for (const auto& [eid, emitter] : system->emitters_)
        {
            (void)eid;
            if (!emitter || !emitter->enabled_ || emitter->particles_.empty() || emitter->render_mode_ != HRL_VFX_RENDER_MESH) continue;
            auto meshIt = context->meshes.find(emitter->mesh_);
            if (meshIt == context->meshes.end() || !meshIt->second) continue;
            HRL_Mesh* mesh = meshIt->second;
            auto gpuIt = g.meshes.find(emitter->mesh_);
            if (gpuIt == g.meshes.end()) continue;
            MeshGPU& gpu = gpuIt->second;

            HRL_Material* material = nullptr;
            if (emitter->material_ != HRL_INVALID_ID)
            {
                auto it = context->materials.find(emitter->material_);
                if (it != context->materials.end()) material = it->second;
            }
            if (!material && mesh->material_ != HRL_INVALID_ID)
            {
                auto it = context->materials.find(mesh->material_);
                if (it != context->materials.end()) material = it->second;
            }
            if (!material) continue;
            const VulkanShader* shader = GetShader(material->shader_);
            if (!shader) continue;

            const bool skeletal = mesh->type_ == HRL_3D_SKELETAL_MESH;
            const bool sprite = mesh->type_ == HRL_SPRITE;
            const uint32_t mode = skeletal ? 1u : (sprite ? 2u : 0u);
            bool twoSided = false;
            auto ts = material->intParams_.find(HRL_MATERIAL_PARAM_TWO_SIDED);
            if (ts != material->intParams_.end()) twoSided = ts->second != 0;
            const uint32_t blend = static_cast<uint32_t>(emitter->blend_mode_) + 1u;
            const PipelineKey key{material->shader_, mode, twoSided ? 0u : 1u, blend, renderPass};
            const VkPipeline pipeline = GetPipeline(key, renderPass);
            if (!pipeline) continue;

            std::vector<const HRL_VFXParticle*> particles;
            particles.reserve(emitter->particles_.size());
            for (const HRL_VFXParticle& particle : emitter->particles_) particles.push_back(&particle);
            const glm::vec3 cameraPos = state.viewport && state.viewport->camera_ ? state.viewport->camera_->position_ : glm::vec3(0.0f);
            if (emitter->blend_mode_ == HRL_VFX_BLEND_ALPHA)
            {
                std::stable_sort(particles.begin(), particles.end(), [&](const HRL_VFXParticle* a, const HRL_VFXParticle* b){
                    const glm::vec3 pa = a->position + (emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL ? glm::vec3(systemMatrix * glm::vec4(0,0,0,1)) : glm::vec3(0.0f));
                    const glm::vec3 pb = b->position + (emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL ? glm::vec3(systemMatrix * glm::vec4(0,0,0,1)) : glm::vec3(0.0f));
                    return glm::length(pa-cameraPos) > glm::length(pb-cameraPos);
                });
            }

            std::vector<HRL_id> textureOverride;
            if (emitter->texture_ != HRL_INVALID_ID && GetTexture(emitter->texture_))
                textureOverride.push_back(emitter->texture_);
            VkDescriptorSet descriptorSet = BindResources(material, shader, textureOverride.empty() ? nullptr : &textureOverride, std::max<size_t>(16, shader->uniform_block_size));
            if (!descriptorSet) continue;

            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
            if (skeletal)
            {
                if (!gpu.skeletal_vertex.buffer || gpu.skeletal_vertex_count == 0) continue;
                VkDeviceSize zero=0;
                vkCmdBindVertexBuffers(cmd,0,1,&gpu.skeletal_vertex.buffer,&zero);
                if (gpu.skeletal_index.buffer && gpu.skeletal_index_count > 0) vkCmdBindIndexBuffer(cmd,gpu.skeletal_index.buffer,0,VK_INDEX_TYPE_UINT32);
            }
            else
            {
                if (gpu.levels.empty() || !gpu.levels[0].vertex.buffer || gpu.levels[0].vertex_count == 0) continue;
                const MeshLevelGPU& lvl = gpu.levels[0];
                VkDeviceSize zero=0;
                vkCmdBindVertexBuffers(cmd,0,1,&lvl.vertex.buffer,&zero);
                if (lvl.index.buffer && lvl.index_count > 0) vkCmdBindIndexBuffer(cmd,lvl.index.buffer,0,VK_INDEX_TYPE_UINT32);
            }

            for (const HRL_VFXParticle* particle : particles)
            {
                std::vector<unsigned char> uniform(std::max<size_t>(16,shader->uniform_block_size),0);
                const glm::mat4 model = MakeVFXMeshParticleModel(*particle,emitter,mesh,systemMatrix);
                FillUniforms(shader,state,mesh,uniform,0,false,&model,&particle->color);
                const size_t offset = AllocateUniform(uniform.size(),uniform.data());
                if (offset == std::numeric_limits<size_t>::max()) break;
                const uint32_t dynamicOffset=static_cast<uint32_t>(offset);
                vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.pipeline_layout,0,1,&descriptorSet,1,&dynamicOffset);
                if (skeletal)
                {
                    if (gpu.skeletal_index.buffer && gpu.skeletal_index_count > 0) vkCmdDrawIndexed(cmd,gpu.skeletal_index_count,1,0,0,0);
                    else vkCmdDraw(cmd,gpu.skeletal_vertex_count,1,0,0);
                }
                else
                {
                    const MeshLevelGPU& lvl=gpu.levels[0];
                    if (lvl.index.buffer && lvl.index_count > 0) vkCmdDrawIndexed(cmd,lvl.index_count,1,0,0,0);
                    else vkCmdDraw(cmd,lvl.vertex_count,1,0,0);
                }
            }
        }
    }
}

static void DrawVFX(VkCommandBuffer cmd, const SceneDrawState& state, const hrl_scene_t* scene)
{
    if (!scene || scene->vfx_systems.empty() || !state.viewport || !state.viewport->camera_) return;
    auto shaderIt = g.builtin_shader_ids.find("vfx");
    if (shaderIt == g.builtin_shader_ids.end()) return;
    const VulkanShader* shader = GetShader(shaderIt->second);
    if (!shader || !g.vfx_quad_vertex.buffer || !g.vfx_quad_index.buffer) return;

    struct Batch { HRL_EVFXBlendMode blend; HRL_id texture; std::vector<VFXRenderInstance> instances; };
    std::vector<Batch> batches;
    const glm::mat4 inverseView = glm::inverse(state.view);
    const glm::mat4 billboardBasis = glm::mat4(glm::mat3(inverseView));
    const glm::vec3 cameraPos = state.viewport->camera_->position_;

    for (const auto& [sid, system] : scene->vfx_systems)
    {
        (void)sid;
        if (!system || !system->enabled_) continue;
        const glm::mat4 systemMatrix = MakeVFXSystemMatrix(system);
        for (const auto& [eid, emitter] : system->emitters_)
        {
            (void)eid;
            if (!emitter || !emitter->enabled_ || emitter->particles_.empty() || emitter->render_mode_ == HRL_VFX_RENDER_MESH) continue;
            std::vector<std::pair<float,VFXRenderInstance>> sorted;
            sorted.reserve(emitter->particles_.size());
            for (const auto& particle : emitter->particles_)
            {
                VFXRenderInstance instance;
                instance.model = MakeVFXParticleModel(particle,
                    emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL ? systemMatrix : glm::mat4(1.0f),
                    billboardBasis,
                    emitter->render_mode_ == HRL_VFX_RENDER_STRETCHED_BILLBOARD,
                    emitter->stretch_);
                instance.color = particle.color;
                const glm::vec3 worldPos = glm::vec3(instance.model[3]);
                sorted.emplace_back(glm::dot(worldPos-cameraPos,worldPos-cameraPos),instance);
            }
            if (emitter->blend_mode_ == HRL_VFX_BLEND_ALPHA)
                std::stable_sort(sorted.begin(),sorted.end(),[](const auto&a,const auto&b){return a.first>b.first;});
            Batch batch{emitter->blend_mode_,ResolveVFXTexture(emitter),{}};
            batch.instances.reserve(sorted.size());
            for (auto& pair : sorted) batch.instances.push_back(pair.second);
            if (!batch.instances.empty()) batches.push_back(std::move(batch));
        }
    }

    const VkRenderPass renderPass = g.scenes.at(state.scene_id).render_pass;
    for (const auto& batch : batches)
    {
        const size_t bytes = batch.instances.size()*sizeof(VFXRenderInstance);
        if (bytes == 0 || bytes > g.vfx_capacity) continue;
        std::memcpy(g.vfx_mapped,batch.instances.data(),bytes);
        std::vector<unsigned char> uniform(std::max<size_t>(16,shader->uniform_block_size),0);
        SetUniformMat(uniform,shader,"projection",state.projection);
        SetUniformMat(uniform,shader,"view",state.view);
        const int useTexture = batch.texture != HRL_INVALID_ID && GetTexture(batch.texture) ? 1 : 0;
        SetUniformRaw(uniform,shader,"uUseTexture",useTexture);
        const size_t uoff = AllocateUniform(uniform.size(),uniform.data());
        if (uoff == std::numeric_limits<size_t>::max()) continue;
        std::vector<HRL_id> textureIds{batch.texture};
        VkDescriptorSet ds = BindResources(nullptr,shader,&textureIds,uniform.size());
        if (!ds) continue;
        PipelineKey key{shaderIt->second,8,0,static_cast<uint32_t>(batch.blend)+1u,renderPass};
        const VkPipeline pipeline = GetPipeline(key,renderPass);
        if (!pipeline) continue;
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        const uint32_t dynamicOffset = static_cast<uint32_t>(uoff);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.pipeline_layout,0,1,&ds,1,&dynamicOffset);
        VkBuffer vertexBuffers[2] = {g.vfx_quad_vertex.buffer,g.vfx_buffer.buffer};
        VkDeviceSize offsets[2] = {0,0};
        vkCmdBindVertexBuffers(cmd,0,2,vertexBuffers,offsets);
        vkCmdBindIndexBuffer(cmd,g.vfx_quad_index.buffer,0,VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd,6,static_cast<uint32_t>(batch.instances.size()),0,0,0);
    }
}

static void DrawDebug(VkCommandBuffer cmd, const SceneDrawState& state, const DebugRenderer& renderer, float lineThickness)
{
    if (renderer.lines.empty() && renderer.triangles.empty()) return;
    auto it = g.builtin_shader_ids.find("debug");
    if (it == g.builtin_shader_ids.end()) return;
    const VulkanShader* shader = GetShader(it->second);
    PipelineKey key{it->second,3,0,0,g.scenes.at(state.scene_id).render_pass};
    const VkPipeline pipeline = GetPipeline(key, g.scenes.at(state.scene_id).render_pass);
    if (!pipeline) return;
    if (g.debug_cursor + (renderer.lines.size()+renderer.triangles.size())*sizeof(DebugVertex) > kDebugBufferCapacity) return;
    DebugVertex* dst = reinterpret_cast<DebugVertex*>(static_cast<unsigned char*>(g.debug_mapped) + g.debug_cursor);
    const size_t lineOffset = g.debug_cursor;
    std::memcpy(dst, renderer.lines.data(), renderer.lines.size()*sizeof(DebugVertex));
    const size_t triOffset = lineOffset + renderer.lines.size()*sizeof(DebugVertex);
    std::memcpy(static_cast<unsigned char*>(g.debug_mapped) + triOffset, renderer.triangles.data(), renderer.triangles.size()*sizeof(DebugVertex));
    g.debug_cursor = triOffset + renderer.triangles.size()*sizeof(DebugVertex);

    std::vector<unsigned char> uniform(shader->uniform_block_size, 0);
    SetUniformMat(uniform, shader, "projection", state.projection);
    SetUniformMat(uniform, shader, "view", state.view);
    const size_t uoff = AllocateUniform(uniform.size(), uniform.data());
    if (uoff == std::numeric_limits<size_t>::max()) return;
    VkDescriptorSet ds = BindResources(nullptr, shader, nullptr, uniform.size());
    if (!ds) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const uint32_t dynamicOffset = static_cast<uint32_t>(uoff);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline_layout, 0, 1, &ds, 1, &dynamicOffset);
    VkDeviceSize off = lineOffset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g.debug_buffer.buffer, &off);
    if (!renderer.lines.empty()) vkCmdDraw(cmd, static_cast<uint32_t>(renderer.lines.size()), 1, 0, 0);
    if (!renderer.triangles.empty())
    {
        key.mode = 7;
        const VkPipeline trianglePipeline = GetPipeline(key, g.scenes.at(state.scene_id).render_pass);
        if (trianglePipeline)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, trianglePipeline);
            off = triOffset;
            vkCmdBindVertexBuffers(cmd, 0, 1, &g.debug_buffer.buffer, &off);
            vkCmdDraw(cmd, static_cast<uint32_t>(renderer.triangles.size()), 1, 0, 0);
        }
    }
    (void)lineThickness;
}

static void DrawWidgets(VkCommandBuffer cmd, const SceneDrawState& state, const HRL_Viewport* viewport)
{
    if (!viewport || viewport->widgets.empty()) return;
    auto it = g.builtin_shader_ids.find("ui");
    if (it == g.builtin_shader_ids.end()) return;
    const VulkanShader* shader = GetShader(it->second);
    if (!shader) return;
    const VkPipeline pipeline = GetPipeline(PipelineKey{it->second,4,0,1,g.scenes.at(state.scene_id).render_pass}, g.scenes.at(state.scene_id).render_pass);
    if (!pipeline) return;

    HRL_Context* privateContext = GetPrivateContext();
    std::vector<HRL_Widget*> widgets;
    widgets.reserve(viewport->widgets.size());
    for (auto& [id,w] : viewport->widgets) if (w) widgets.push_back(w);
    std::sort(widgets.begin(), widgets.end(), [](const HRL_Widget* a,const HRL_Widget* b){
        if (a->GetZIndex()!=b->GetZIndex()) return a->GetZIndex()<b->GetZIndex();
        return a->GetId()<b->GetId();
    });

    const float windowW = static_cast<float>(GetWindowWidth());
    const float windowH = static_cast<float>(GetWindowHeight());
    const float vpW = std::max(1.0f, viewport->width_ * windowW);
    const float vpH = std::max(1.0f, viewport->height_ * windowH);
    const float vpX = viewport->x_ * windowW;
    const float vpY = viewport->y_ * windowH;

    // Preserve the GL backend's widget interaction model: hover is evaluated for
    // every widget, then one top-most interactive widget captures the press.
    for (HRL_Widget* widget : widgets)
        widget->UpdateInput(static_cast<float>(privateContext->mouseX), static_cast<float>(privateContext->mouseY),
                            false, false, false, vpX, vpY, vpW, vpH);

    if (privateContext->mouseLeftPressed && privateContext->mouseCaptureWidget == HRL_INVALID_ID &&
        privateContext->mouseCaptureGizmo == HRL_INVALID_ID)
    {
        for (auto rit = widgets.rbegin(); rit != widgets.rend(); ++rit)
        {
            HRL_Widget* widget = *rit;
            if (widget && widget->IsPointerInteractive() && widget->IsVisible() && widget->IsEnabled() && widget->IsHovered())
            {
                privateContext->mouseCaptureWidget = widget->GetId();
                break;
            }
        }
    }

    for (HRL_Widget* widget : widgets)
    {
        const bool isCapture = widget->GetId() == privateContext->mouseCaptureWidget;
        widget->UpdateInput(static_cast<float>(privateContext->mouseX), static_cast<float>(privateContext->mouseY),
                            isCapture ? privateContext->mouseLeftDown : false,
                            isCapture ? privateContext->mouseLeftPressed : false,
                            isCapture ? privateContext->mouseLeftReleased : false,
                            vpX, vpY, vpW, vpH);
        widget->Logic();

        std::vector<HRL_Widget::WidgetDrawInfos> drawInfos;
        widget->GetDrawInfos(drawInfos);
        for (const auto& info : drawInfos)
        {
            if (info.sx <= 0.0f || info.sy <= 0.0f || info.a <= 0.0f) continue;
            UIVertex q[6]{
                {{info.px,info.py},{0,1},{info.r,info.g,info.b,info.a}},
                {{info.px+info.sx,info.py},{1,1},{info.r,info.g,info.b,info.a}},
                {{info.px+info.sx,info.py+info.sy},{1,0},{info.r,info.g,info.b,info.a}},
                {{info.px,info.py},{0,1},{info.r,info.g,info.b,info.a}},
                {{info.px+info.sx,info.py+info.sy},{1,0},{info.r,info.g,info.b,info.a}},
                {{info.px,info.py+info.sy},{0,0},{info.r,info.g,info.b,info.a}}
            };
            if (g.debug_cursor + sizeof(q) > kDebugBufferCapacity) return;
            const size_t vertexOffset = g.debug_cursor;
            std::memcpy(static_cast<unsigned char*>(g.debug_mapped)+vertexOffset, q, sizeof(q));
            g.debug_cursor += sizeof(q);

            std::vector<unsigned char> uniform(shader->uniform_block_size,0);
            const glm::mat4 ortho = glm::ortho(0.0f,1.0f,1.0f,0.0f,-1.0f,1.0f);
            SetUniformMat(uniform, shader, "uiProj", ortho);
            const int hasTexture = (info.texture != HRL_INVALID_ID && GetTexture(info.texture)) ? 1 : 0;
            SetUniformRaw(uniform, shader, "uHasTexture", hasTexture);
            SetUniformRaw(uniform, shader, "uSDF", info.sdf ? 1 : 0);
            const size_t uoff = AllocateUniform(uniform.size(), uniform.data());
            if (uoff == std::numeric_limits<size_t>::max()) return;

            std::vector<HRL_id> textures;
            if (hasTexture) textures.push_back(info.texture);
            VkDescriptorSet ds = BindResources(nullptr, shader, textures.empty() ? nullptr : &textures, uniform.size());
            if (!ds) return;

            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
            const uint32_t dynamicOffset = static_cast<uint32_t>(uoff);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.pipeline_layout,0,1,&ds,1,&dynamicOffset);
            VkDeviceSize vbOff = vertexOffset;
            vkCmdBindVertexBuffers(cmd,0,1,&g.debug_buffer.buffer,&vbOff);
            vkCmdDraw(cmd,6,1,0,0);
        }
    }
    if (privateContext->mouseLeftReleased)
        privateContext->mouseCaptureWidget = HRL_INVALID_ID;
}

static bool CreateFallbackTexture()
{
    const unsigned char white[] = {255,255,255,255};
    BitmapResult bmp{}; bmp.width=1; bmp.height=1; bmp.pixels.assign(white,white+4);
    const HRL_id id = GenerateHRL_ID();
    VulkanTexture tex;
    std::string error;
    if (!tex.Create(g.physical,g.device,g.command_pool,g.graphics_queue,bmp,g.max_anisotropy,error))
    {
        SetErrorCode(HRL_INVALID_BACKEND_OPERATION,HRL_SEVERITY_ERROR,"Vulkan fallback texture: "+error);
        return false;
    }
    g.textures.emplace(id,std::move(tex));
    g.fallback_texture = id;
    return true;
}

static bool ProcessScreenshot(HRL_id sceneId, const std::string& path)
{
    auto it = g.scenes.find(sceneId);
    if (it == g.scenes.end() || path.empty()) return false;
    SceneGPU& scene = it->second;
    if (!scene.color.image) return false;

    const VkDeviceSize size = static_cast<VkDeviceSize>(scene.width) * scene.height * 4u;
    BufferResource staging;
    if (!CreateBuffer(size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,staging)) return false;
    bool ok = ImmediateSubmit([&](VkCommandBuffer cmd)
    {
        TransitionImage(cmd,scene.color.image,scene.color_layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.bufferOffset=0;
        region.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount=1;
        region.imageExtent={static_cast<uint32_t>(scene.width),static_cast<uint32_t>(scene.height),1};
        vkCmdCopyImageToBuffer(cmd,scene.color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging.buffer,1,&region);
        TransitionImage(cmd,scene.color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    });
    if (!ok){DestroyBuffer(staging);return false;}
    scene.color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    void* mapped=nullptr;
    if (vkMapMemory(g.device,staging.memory,0,size,0,&mapped)!=VK_SUCCESS){DestroyBuffer(staging);return false;}
    std::vector<unsigned char> rgba(static_cast<size_t>(size));
    std::memcpy(rgba.data(),mapped,rgba.size());
    vkUnmapMemory(g.device,staging.memory);
    // Tiny self-contained PPM fallback for portability: if the requested path is
    // not .png we preserve the extension; .png receives a valid RGBA PNG writer.
    auto WriteU32 = [](std::vector<unsigned char>& out,uint32_t v){out.push_back((v>>24)&255);out.push_back((v>>16)&255);out.push_back((v>>8)&255);out.push_back(v&255);};
    auto Crc32 = [](const unsigned char* p,size_t n){uint32_t c=0xffffffffu;for(size_t i=0;i<n;++i){c^=p[i];for(int j=0;j<8;++j)c=(c>>1)^(0xedb88320u&-(int)(c&1));}return ~c;};
    auto Adler32=[](const unsigned char* p,size_t n){uint32_t a=1,b=0;for(size_t i=0;i<n;++i){a=(a+p[i])%65521u;b=(b+a)%65521u;}return (b<<16)|a;};
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<size_t>(scene.height)*(static_cast<size_t>(scene.width)*4+1));
    for(int y=0;y<scene.height;++y){raw.push_back(0);const unsigned char* row=rgba.data()+static_cast<size_t>(scene.height-1-y)*scene.width*4;raw.insert(raw.end(),row,row+scene.width*4);}    
    std::vector<unsigned char> z;z.push_back(0x78);z.push_back(0x01);size_t pos=0;while(pos<raw.size()){size_t rem=raw.size()-pos;uint16_t len=(uint16_t)std::min<size_t>(rem,65535);bool fin=pos+len==raw.size();z.push_back(fin?1:0);z.push_back(len&255);z.push_back((len>>8)&255);uint16_t nlen=~len;z.push_back(nlen&255);z.push_back((nlen>>8)&255);z.insert(z.end(),raw.begin()+pos,raw.begin()+pos+len);pos+=len;}uint32_t ad=Adler32(raw.data(),raw.size());WriteU32(z,ad);
    std::vector<unsigned char> png={137,80,78,71,13,10,26,10};
    auto chunk=[&](const char* type,const std::vector<unsigned char>& payload){WriteU32(png,(uint32_t)payload.size());size_t off=png.size();png.insert(png.end(),type,type+4);png.insert(png.end(),payload.begin(),payload.end());WriteU32(png,Crc32(png.data()+off,4+payload.size()));};
    std::vector<unsigned char> ihdr;WriteU32(ihdr,scene.width);WriteU32(ihdr,scene.height);ihdr.push_back(8);ihdr.push_back(6);ihdr.push_back(0);ihdr.push_back(0);ihdr.push_back(0);chunk("IHDR",ihdr);chunk("IDAT",z);chunk("IEND",{});
    std::ofstream file(path,std::ios::binary);if(file){file.write(reinterpret_cast<const char*>(png.data()),png.size());ok=file.good();}
    DestroyBuffer(staging);
    return ok;
}


static void UpdateDescriptorTexture(VkDescriptorSet set, uint32_t binding, const VulkanTexture& texture)
{
    if (!set || !texture.view || !texture.sampler || binding >= kMaxSamplers) return;
    VkDescriptorImageInfo ii{};
    ii.sampler = texture.sampler;
    ii.imageView = texture.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g.device, 1, &w, 0, nullptr);
}

static void UpdateMeshShadowDescriptors(VkDescriptorSet ds, const VulkanShader* shader, HRL_id sceneId)
{
    const auto sit = g.active_shadow_slots_by_scene.find(sceneId);
    if (sit == g.active_shadow_slots_by_scene.end()) return;

    const uint32_t tex2DBase = ShaderSamplerBinding(shader, "ShadowMap2D_0", 7);
    const uint32_t texCubeBase = ShaderSamplerBinding(shader, "ShadowMapCube_0", 11);
    for (const auto& [lightId, slot] : sit->second)
    {
        auto it = g.shadow_maps.find(lightId);
        if (it == g.shadow_maps.end()) continue;
        if (slot < 0 || slot >= 4) continue;
        if (it->second.cube)
        {
            if (texCubeBase + static_cast<uint32_t>(slot) < kMaxSamplers)
                UpdateDescriptorImage(ds, texCubeBase + static_cast<uint32_t>(slot), it->second.depth);
        }
        else
        {
            if (tex2DBase + static_cast<uint32_t>(slot) < kMaxSamplers)
                UpdateDescriptorImage(ds, tex2DBase + static_cast<uint32_t>(slot), it->second.depth);
        }
    }
}

static void SetUniformVec4ArrayElement(std::vector<unsigned char>& data, const VulkanShader* shader,
                                       const char* name, uint32_t index, const glm::vec4& value)
{
    const VulkanUniformField* f = FindUniform(shader, name);
    if (!f || index >= f->array_count) return;
    const size_t offset = f->offset + static_cast<size_t>(index) * 16u;
    if (offset + sizeof(value) > data.size()) return;
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

static void UpdateSceneLightBuffer(hrl_scene_t* scene, HRL_id sceneId)
{
    g.lights = LightBlockGPU{};
    if (!scene) return;

    const auto sit = g.active_shadow_slots_by_scene.find(sceneId);
    for (const auto& [id, light] : scene->lights)
    {
        if (!light || g.lights.count >= kMaxLights) break;
        LightGPU& d = g.lights.lights[g.lights.count++];
        d.type = light->type_;
        d.intensity = light->intensity_;
        d.attenuation = std::max(0.0f, light->attenuation_);
        d.innerCutoff = std::cos(glm::radians(glm::clamp(light->innerCutoff, 0.0f, 89.9f)));
        d.position_outer = glm::vec4(light->position_,
                                     std::cos(glm::radians(glm::clamp(light->outerCutoff, 0.0f, 89.9f))));
        d.rotation_padding = glm::vec4(GetLightDirection(light), 0.0f);
        d.color_shadow = glm::vec4(glm::max(light->color_, glm::vec3(0.0f)),
                                   glm::clamp(light->shadow_strength_, 0.0f, 1.0f));
        d.shadowMatrix = glm::mat4(1.0f);
        d.shadowParams = glm::vec4(light->shadow_bias_, 0.0f, -1.0f, 0.0f);

        if (sit != g.active_shadow_slots_by_scene.end())
        {
            const auto st = sit->second.find(id);
            if (st != sit->second.end())
            {
                const float farPlane = light->type_ == HRL_POINT_LIGHT ? 100.0f :
                                       (light->type_ == HRL_SPOT_LIGHT ? 100.0f : 200.0f);
                const float kind = light->type_ == HRL_POINT_LIGHT ? 2.0f : 1.0f;
                d.shadowParams = glm::vec4(light->shadow_bias_, farPlane, static_cast<float>(st->second), kind);
                if (light->type_ != HRL_POINT_LIGHT)
                    d.shadowMatrix = ShadowMatrixForLight(light, SceneShadowCenter(scene));
            }
        }
    }
    if (g.light_mapped)
        std::memcpy(g.light_mapped, &g.lights, sizeof(g.lights));
}

static void FillPostProcessUniforms(std::vector<unsigned char>& u, const VulkanShader* shader,
                                    const SceneGPU& scene, const HRL_Viewport* viewport,
                                    const glm::mat4& invPV)
{
    SetUniformMat(u, shader, "uInvViewProjection", invPV);
    static const auto postStartTime = std::chrono::steady_clock::now();
    const float postTimeSeconds = static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now() - postStartTime).count());
    SetUniformRaw(u, shader, "uTime", postTimeSeconds);

    // Match the defaults of the public OpenGL default post-process shader.
    SetUniformRaw(u, shader, "brightness", 1.0f);
    SetUniformRaw(u, shader, "contrast", 1.0f);
    SetUniformRaw(u, shader, "saturation", 1.0f);
    SetUniformRaw(u, shader, "gamma", 1.0f);
    SetUniformRaw(u, shader, "exposure", 0.0f);
    SetUniformRaw(u, shader, "hueShift", 0.0f);
    SetUniformVec3(u, shader, "tintColor", glm::vec3(1.0f));
    SetUniformRaw(u, shader, "invertColor", 0);
    SetUniformRaw(u, shader, "bloomStrength", 1.0f);
    SetUniformRaw(u, shader, "sharpenStrength", 0.0f);
    SetUniformRaw(u, shader, "chromaticAberration", 0.0f);
    SetUniformRaw(u, shader, "filmGrainStrength", 0.0f);
    SetUniformRaw(u, shader, "filmGrainScale", 1.0f);
    SetUniformRaw(u, shader, "vignetteStrength", 0.0f);
    SetUniformRaw(u, shader, "vignetteRadius", 0.75f);
    SetUniformRaw(u, shader, "vignetteSoftness", 0.25f);
    SetUniformVec3(u, shader, "vignetteColor", glm::vec3(0.0f));
    SetUniformRaw(u, shader, "fadeAmount", 0.0f);
    SetUniformVec3(u, shader, "fadeColor", glm::vec3(0.0f));

    if (viewport && viewport->camera_)
        SetUniformVec3(u, shader, "uCameraPos", viewport->camera_->position_);
    const float viewportPixelWidth = viewport ? std::max(1.0f, viewport->width_ * static_cast<float>(scene.width)) : static_cast<float>(scene.width);
    const float viewportPixelHeight = viewport ? std::max(1.0f, viewport->height_ * static_cast<float>(scene.height)) : static_cast<float>(scene.height);
    SetUniformRaw(u, shader, "uScreenSize", glm::vec2(viewportPixelWidth, viewportPixelHeight));
    if (viewport)
    {
        const glm::vec2 origin(viewport->x_, 1.0f - viewport->y_ - viewport->height_);
        const glm::vec2 size(viewport->width_, viewport->height_);
        SetUniformRaw(u, shader, "uViewportOrigin", origin);
        SetUniformRaw(u, shader, "uViewportSize", size);
    }
}

static bool ParseUniformArrayIndex(const std::string& name, std::string& baseName, uint32_t& index)
{
    const size_t open = name.find_last_of('[');
    if (open == std::string::npos || name.empty() || name.back() != ']') return false;
    baseName = name.substr(0, open);
    if (baseName.empty()) return false;
    const std::string number = name.substr(open + 1, name.size() - open - 2);
    if (number.empty()) return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(number.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed > UINT32_MAX) return false;
    index = static_cast<uint32_t>(parsed);
    return true;
}

static void SetUniformFloatArrayElement(std::vector<unsigned char>& data, const VulkanShader* shader,
                                        const std::string& name, float value)
{
    std::string base; uint32_t index = 0;
    if (!ParseUniformArrayIndex(name, base, index)) return;
    const VulkanUniformField* f = FindUniform(shader, base.c_str());
    if (!f || index >= f->array_count) return;
    const size_t offset = f->offset + static_cast<size_t>(index) * 16u;
    if (offset + sizeof(float) > data.size()) return;
    std::memcpy(data.data() + offset, &value, sizeof(float));
}

static void SetUniformVec3ArrayElement(std::vector<unsigned char>& data, const VulkanShader* shader,
                                       const std::string& name, const glm::vec3& value)
{
    std::string base; uint32_t index = 0;
    if (!ParseUniformArrayIndex(name, base, index)) return;
    const VulkanUniformField* f = FindUniform(shader, base.c_str());
    if (!f || index >= f->array_count) return;
    const size_t offset = f->offset + static_cast<size_t>(index) * 16u;
    if (offset + 12 > data.size()) return;
    std::memcpy(data.data() + offset, &value, 12);
}

static void SetMaterialUniforms(std::vector<unsigned char>& data, const VulkanShader* shader, const HRL_Material* material)
{
    if (!material) return;
    for (const auto& [n,v] : material->intParams_)
    {
        if (FindUniform(shader, n.c_str())) SetUniformRaw(data,shader,n.c_str(),v);
    }
    for (const auto& [n,v] : material->floatParams_)
    {
        if (FindUniform(shader, n.c_str())) SetUniformRaw(data,shader,n.c_str(),v);
        else SetUniformFloatArrayElement(data,shader,n,v);
    }
    for (const auto& [n,v] : material->vec2Params_)
    {
        if (FindUniform(shader, n.c_str())) SetUniformRaw(data,shader,n.c_str(),v);
    }
    for (const auto& [n,v] : material->vec3Params_)
    {
        if (FindUniform(shader, n.c_str())) SetUniformVec3(data,shader,n.c_str(),v);
        else SetUniformVec3ArrayElement(data,shader,n,v);
    }
    for (const auto& [n,v] : material->vec4Params_)
    {
        if (FindUniform(shader, n.c_str())) SetUniformRaw(data,shader,n.c_str(),v);
        else
        {
            std::string base; uint32_t index = 0;
            if (ParseUniformArrayIndex(n,base,index))
                SetUniformVec4ArrayElement(data,shader,base.c_str(),index,v);
        }
    }
}

static bool RunFullscreenPass(SceneGPU& scene, HRL_id shaderId,
                              const ImageResource& source, const ImageResource* brightSource,
                              const ImageResource* depthImage,
                              const HRL_Viewport* viewport, const glm::mat4& invPV,
                              HRL_Material* material, ImageResource& dst, VkImageLayout& dstLayout)
{
    const VulkanShader* shader = GetShader(shaderId);
    if (!shader || !EnsurePostResources(scene) || !source.image || !dst.image || source.image == dst.image)
        return false;

    if (dstLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        TransitionImage(g.frame_command, dst.image, dstLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    dstLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    TransitionImage(g.frame_command, source.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent = {static_cast<uint32_t>(scene.width), static_cast<uint32_t>(scene.height), 1};
    vkCmdCopyImage(g.frame_command, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    TransitionImage(g.frame_command, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionImage(g.frame_command, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    dstLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = scene.post_render_pass;
    bi.framebuffer = (dst.image == scene.post_a.image) ? scene.post_framebuffer_a : scene.post_framebuffer_b;
    bi.renderArea.extent = {(uint32_t)scene.width, (uint32_t)scene.height};
    vkCmdBeginRenderPass(g.frame_command, &bi, VK_SUBPASS_CONTENTS_INLINE);
    if (viewport)
        SetViewportAndScissor(g.frame_command, viewport, scene.width, scene.height);
    else
    {
        VkViewport vp{0,0,(float)scene.width,(float)scene.height,0,1};
        VkRect2D sc{{0,0},{(uint32_t)scene.width,(uint32_t)scene.height}};
        vkCmdSetViewport(g.frame_command,0,1,&vp);
        vkCmdSetScissor(g.frame_command,0,1,&sc);
    }

    std::vector<unsigned char> u(std::max<size_t>(16, shader->uniform_block_size), 0);
    FillPostProcessUniforms(u, shader, scene, viewport, invPV);
    SetMaterialUniforms(u, shader, material);

    const size_t off = AllocateUniform(u.size(), u.data());
    if (off == std::numeric_limits<size_t>::max())
    {
        vkCmdEndRenderPass(g.frame_command);
        return false;
    }
    VkDescriptorSet ds = BindResources(material, shader, nullptr, u.size());
    if (!ds)
    {
        vkCmdEndRenderPass(g.frame_command);
        return false;
    }
    const uint32_t sceneBinding = ShaderSamplerBinding(shader, "uScene", 0);
    UpdateDescriptorImage(ds, sceneBinding, source);
    if (brightSource)
        UpdateDescriptorImage(ds, ShaderSamplerBinding(shader, "uBrightScene", 1), *brightSource);
    if (depthImage && depthImage->image)
        UpdateDescriptorImage(ds, ShaderSamplerBinding(shader, "uDepth", 2), *depthImage);

    VkPipeline pipeline = GetPipeline(PipelineKey{shaderId,5,0,0,scene.post_render_pass}, scene.post_render_pass);
    if (!pipeline)
    {
        vkCmdEndRenderPass(g.frame_command);
        return false;
    }
    vkCmdBindPipeline(g.frame_command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const uint32_t dyn = static_cast<uint32_t>(off);
    vkCmdBindDescriptorSets(g.frame_command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g.pipeline_layout, 0, 1, &ds, 1, &dyn);
    vkCmdDraw(g.frame_command, 3, 1, 0, 0);
    vkCmdEndRenderPass(g.frame_command);
    dstLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

static bool RenderSceneEffectsForViewport(SceneGPU& scene, const hrl_scene_t* hrlScene,
                                          const HRL_Viewport* viewport, const glm::mat4& invPV,
                                          const ImageResource& source, const ImageResource& brightSource,
                                          const ImageResource& depth, ImageResource& dst, VkImageLayout& dstLayout)
{
    const auto it = g.builtin_shader_ids.find("scene_effects");
    if (it == g.builtin_shader_ids.end() || !hrlScene || !viewport) return false;
    const VulkanShader* shader = GetShader(it->second);
    if (!shader) return false;

    HRL_Material effect{};
    effect.shader_ = it->second;
    effect.intParams_["uAOEnabled"] = hrlScene->ambient_occlusion_enabled ? 1 : 0;
    effect.floatParams_["uAOStrength"] = hrlScene->ambient_occlusion_strength;
    effect.floatParams_["uAORadius"] = hrlScene->ambient_occlusion_radius;
    effect.floatParams_["uAOBias"] = hrlScene->ambient_occlusion_bias;
    effect.floatParams_["uAOPower"] = hrlScene->ambient_occlusion_power;

    const auto& globalFog = hrlScene->global_volumetric_fog;
    effect.intParams_["uGlobalVolumetricFogEnabled"] = globalFog.enabled ? 1 : 0;
    effect.vec3Params_["uGlobalFogColor"] = globalFog.color;
    effect.floatParams_["uGlobalFogDensity"] = globalFog.density;
    effect.intParams_["uGlobalFogSteps"] = static_cast<int>(globalFog.steps);

    int fogCount = 0;
    HRL_uint fogSteps = std::max<HRL_uint>(4u, globalFog.steps);
    for (const auto& [fogId, fog] : hrlScene->volumetric_fogs)
    {
        (void)fogId;
        if (!fog || !fog->enabled || fog->radius <= 0.0f || fog->density <= 0.0f || fogCount >= 64) continue;
        effect.vec4Params_["uFogPosRadius[" + std::to_string(fogCount) + "]"] = glm::vec4(fog->position, fog->radius);
        effect.vec4Params_["uFogColorDensity[" + std::to_string(fogCount) + "]"] = glm::vec4(fog->color, fog->density);
        fogSteps = std::max(fogSteps, fog->steps);
        ++fogCount;
    }
    effect.intParams_["uVolumetricFogCount"] = fogCount;
    effect.intParams_["uVolumetricFogSteps"] = static_cast<int>(fogSteps);

    const auto& rays = hrlScene->god_rays;
    effect.intParams_["uGodRaysEnabled"] = rays.enabled ? 1 : 0;
    effect.vec2Params_["uGodRaysLightUV"] = glm::vec2(0.5f);
    effect.vec3Params_["uGodRaysColor"] = rays.color;
    effect.floatParams_["uGodRaysDensity"] = rays.density;
    effect.floatParams_["uGodRaysDecay"] = rays.decay;
    effect.floatParams_["uGodRaysWeight"] = rays.weight;
    effect.intParams_["uGodRaysSamples"] = static_cast<int>(rays.samples);
    const glm::vec4 rayClip = Projection(viewport) * View(viewport) * glm::vec4(rays.position, 1.0f);
    if (std::abs(rayClip.w) > 1e-5f)
        effect.vec2Params_["uGodRaysLightUV"] = glm::vec2(rayClip.x, rayClip.y) / rayClip.w * 0.5f + 0.5f;

    return RunFullscreenPass(scene, it->second, source, &brightSource, &depth,
                             viewport, invPV, &effect, dst, dstLayout);
}



// --- backend entry points ---
void VK_Shutdown();

void VK_Init()
{
    // Intentionally empty; Vulkan instance/device are created in InitContext because
    // HRL_InitContext already carries the host/window loader pointer.
}

void VK_InitContext(HRL_uint width, HRL_uint height, void* loader)
{
    if (g.initialized) return;
    (void)width; (void)height;
    if (!CreateInstance()) { VK_Shutdown(); return; }
    if (!ChoosePhysicalDevice()) { VK_Shutdown(); return; }
    if (!CreateDevice()) { VK_Shutdown(); return; }
    if (!CreateCommandResources()) { VK_Shutdown(); return; }
    if (!CreateDescriptorInfrastructure()) { VK_Shutdown(); return; }
    if (!CreateFrameBuffers()) { VK_Shutdown(); return; }

    g.msaa = VK_SAMPLE_COUNT_1_BIT;
    g.initialized = true;

    // Built-in Vulkan shaders are kept as regular GLSL files in shaders/vulkan.
    // The public shader API remains source-based and is unchanged.
    if (CreateBuiltinShaderFromFiles("static", "vulkan_static_3dmesh.vert.glsl", "vulkan_static_3dmesh.frag.glsl", HRL_MESH_3D_SHADER) == HRL_INVALID_ID ||
        CreateBuiltinShaderFromFiles("skeletal", "vulkan_skinned_3dmesh.vert.glsl", "vulkan_skinned_3dmesh.frag.glsl", HRL_SKINNED_3D_MESH_SHADER) == HRL_INVALID_ID ||
        CreateBuiltinShaderFromFiles("sprite", "vulkan_sprite.vert.glsl", "vulkan_sprite.frag.glsl", HRL_SPRITE_SHADER) == HRL_INVALID_ID ||
        CreateBuiltinShaderFromFiles("debug", "vulkan_debug.vert.glsl", "vulkan_debug.frag.glsl", HRL_DEBUG_SHADER) == HRL_INVALID_ID ||
        CreateBuiltinShaderFromFiles("sky", "vulkan_sky_sphere.vert.glsl", "vulkan_sky_sphere.frag.glsl") == HRL_INVALID_ID ||
        CreateBuiltinShaderFromFiles("ui", "vulkan_ui.vert.glsl", "vulkan_ui.frag.glsl") == HRL_INVALID_ID ||
        CreateBuiltinShaderFromFiles("vfx", "vulkan_vfx.vert.glsl", "vulkan_vfx.frag.glsl") == HRL_INVALID_ID ||
        CreateBuiltinShaderFromFiles("default_post", "vulkan_post.vert.glsl", "vulkan_post.frag.glsl", HRL_DEFAULT_POST_PROCESS_SHADER) == HRL_INVALID_ID ||
        CreateBuiltinShaderFromFiles("scene_effects", "vulkan_post.vert.glsl", "vulkan_scene_effects.frag.glsl") == HRL_INVALID_ID ||
        CreateBuiltinShaderFromFiles("shadow_static", "vulkan_shadow_static.vert.glsl", "vulkan_shadow.frag.glsl") == HRL_INVALID_ID ||
        CreateBuiltinShaderFromFiles("shadow_skinned", "vulkan_shadow_skinned.vert.glsl", "vulkan_shadow.frag.glsl") == HRL_INVALID_ID ||
        !CreateFallbackTexture())
    {
        VK_Shutdown();
        return;
    }

    if (g.surface && !CreateSwapchain())
    {
        SetErrorCode(HRL_INVALID_BACKEND_OPERATION,HRL_SEVERITY_ERROR,"Vulkan: failed to create swapchain");
        VK_Shutdown();
        return;
    }
}

void VK_Shutdown()
{
    if (!g.device && !g.instance) return;
    if (g.device) vkDeviceWaitIdle(g.device);
    for (auto& [id, p] : g.pipelines) if (p) vkDestroyPipeline(g.device,p,nullptr);
    g.pipelines.clear();
    for (auto& [id, scene] : g.scenes)
    {
        DestroySceneResources(scene);
    }
    g.scenes.clear();
    while (!g.shadow_maps.empty())
        DestroyShadowResource(g.shadow_maps.begin()->first);
    g.active_shadow_slots_by_scene.clear();
    for (auto& [id, mesh] : g.meshes){for(auto& l:mesh.levels){DestroyBuffer(l.vertex);DestroyBuffer(l.index);}DestroyBuffer(mesh.skeletal_vertex);DestroyBuffer(mesh.skeletal_index);}g.meshes.clear();
    for (auto& [id, shader] : g.shaders) shader.Destroy(g.device); g.shaders.clear();
    for (auto& [id, texture] : g.textures) texture.Destroy(g.device); g.textures.clear();
    g.fallback_texture = HRL_INVALID_ID;
    DestroySwapchain();
    if (g.surface) vkDestroySurfaceKHR(g.instance,g.surface,nullptr);g.surface=VK_NULL_HANDLE;
    if (g.image_available) vkDestroySemaphore(g.device,g.image_available,nullptr);
    if (g.render_finished) vkDestroySemaphore(g.device,g.render_finished,nullptr);
    if (g.frame_fence) vkDestroyFence(g.device,g.frame_fence,nullptr);
    if (g.upload_fence) vkDestroyFence(g.device,g.upload_fence,nullptr);
    if (g.command_pool) vkDestroyCommandPool(g.device,g.command_pool,nullptr);
    if (g.pipeline_layout) vkDestroyPipelineLayout(g.device,g.pipeline_layout,nullptr);
    if (g.descriptor_pool) vkDestroyDescriptorPool(g.device,g.descriptor_pool,nullptr);
    if (g.descriptor_layout) vkDestroyDescriptorSetLayout(g.device,g.descriptor_layout,nullptr);
    if (g.uniform_mapped) vkUnmapMemory(g.device,g.uniform_buffer.memory); g.uniform_mapped=nullptr;
    if (g.light_mapped) vkUnmapMemory(g.device,g.light_buffer.memory); g.light_mapped=nullptr;
    if (g.debug_mapped) vkUnmapMemory(g.device,g.debug_buffer.memory); g.debug_mapped=nullptr;
    if (g.vfx_mapped) vkUnmapMemory(g.device,g.vfx_buffer.memory); g.vfx_mapped=nullptr;
    DestroyBuffer(g.uniform_buffer);DestroyBuffer(g.light_buffer);DestroyBuffer(g.debug_buffer);DestroyBuffer(g.vfx_buffer);DestroyBuffer(g.vfx_quad_vertex);DestroyBuffer(g.vfx_quad_index);
    if (g.device) vkDestroyDevice(g.device,nullptr);g.device=VK_NULL_HANDLE;
    if (g.instance) vkDestroyInstance(g.instance,nullptr);g.instance=VK_NULL_HANDLE;
    g.initialized=false;g.instance_created=false;g.physical=VK_NULL_HANDLE;
    g.graphics_queue=VK_NULL_HANDLE;g.present_queue=VK_NULL_HANDLE;
    g.graphics_family=VK_QUEUE_FAMILY_IGNORED;g.present_family=VK_QUEUE_FAMILY_IGNORED;
    g.fallback_texture=HRL_INVALID_ID;g.max_anisotropy=1.0f;g.sampler_anisotropy=false;
    g.frame_open=false;g.present_requested=false;
}

void VK_BeginFrame()
{
    if (!g.initialized || g.frame_open) return;
    if (vkWaitForFences(g.device,1,&g.frame_fence,VK_TRUE,UINT64_MAX) != VK_SUCCESS) return;
    vkResetDescriptorPool(g.device,g.descriptor_pool,0);
    g.uniform_cursor=0;
    g.debug_cursor=0;
    g.present_requested=false;

    if (g.swapchain)
    {
        VkResult r=vkAcquireNextImageKHR(g.device,g.swapchain,UINT64_MAX,g.image_available,VK_NULL_HANDLE,&g.swapchain_index);
        if (r==VK_ERROR_OUT_OF_DATE_KHR)
        {
            vkDeviceWaitIdle(g.device);
            DestroySwapchain();
            if (!CreateSwapchain()) return;
            r=vkAcquireNextImageKHR(g.device,g.swapchain,UINT64_MAX,g.image_available,VK_NULL_HANDLE,&g.swapchain_index);
        }
        if (r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR){VkError("vkAcquireNextImageKHR",r);return;}
        g.swapchain_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }

    if (vkResetCommandBuffer(g.frame_command,0)!=VK_SUCCESS) return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (vkBeginCommandBuffer(g.frame_command,&bi)!=VK_SUCCESS) return;
    g.frame_open=true;
}

void VK_ResetFramebuffer()
{
    if (!g.frame_open) return;
    if (g.swapchain && g.present_requested && g.swapchain_layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
    {
        TransitionImage(g.frame_command,g.swapchain_images[g.swapchain_index],g.swapchain_layout,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        g.swapchain_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    if (vkEndCommandBuffer(g.frame_command)!=VK_SUCCESS){g.frame_open=false;return;}
    VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (g.swapchain)
    {
        si.waitSemaphoreCount=1;si.pWaitSemaphores=&g.image_available;si.pWaitDstStageMask=&waitStage;
        if (g.present_requested) { si.signalSemaphoreCount=1; si.pSignalSemaphores=&g.render_finished; }
    }
    si.commandBufferCount=1;si.pCommandBuffers=&g.frame_command;
    vkResetFences(g.device,1,&g.frame_fence);
    VkResult r=vkQueueSubmit(g.graphics_queue,1,&si,g.frame_fence);
    if(r!=VK_SUCCESS)VkError("vkQueueSubmit",r);
    if(g.swapchain && g.present_requested)
    {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&g.render_finished;pi.swapchainCount=1;pi.pSwapchains=&g.swapchain;pi.pImageIndices=&g.swapchain_index;
        r=vkQueuePresentKHR(g.present_queue,&pi);if(r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR && r!=VK_ERROR_OUT_OF_DATE_KHR)VkError("vkQueuePresentKHR",r);
    }
    g.frame_open=false;
    for(auto& [scene,path]:g.screenshot_paths){ProcessScreenshot(scene,path);}g.screenshot_paths.clear();
}

void VK_WindowResizeCallback(int width,int height)
{
    if(!g.device||!g.swapchain)return;
    if(width<=0||height<=0)return;
    vkDeviceWaitIdle(g.device);DestroySwapchain();CreateSwapchain();
    for(auto& [id,scene]:g.scenes){if(scene.render_on_screen){scene.width=static_cast<int>(g.swapchain_extent.width);scene.height=static_cast<int>(g.swapchain_extent.height);scene.color_format=g.swapchain_format;RecreateScene(scene);}}
}

void VK_TakeScreenshot(HRL_id scene,const char* path)
{
    if(!path||!*path)return;
    g.screenshot_paths[scene]=path;
}

void VK_RenderScene(hrl_scene_t* scene, HRL_id sceneId)
{
    if (!g.initialized || !g.device || !g.frame_open || !scene) return;
    auto it = g.scenes.find(sceneId);
    if (it == g.scenes.end() || scene->viewports.empty()) return;
    SceneGPU& target = it->second;
    if (!target.framebuffer) RecreateScene(target);

    if (scene->shadows_dirty)
    {
        PrepareSceneShadows(scene, sceneId);
        scene->shadows_dirty = false;
    }
    UpdateSceneLightBuffer(scene, sceneId);

    VkClearValue clears[4]{};
    clears[0].color = {{0,0,0,1}};
    clears[1].color = {{0,0,0,1}};
    clears[2].color = {{0,0,0,1}};
    clears[3].depthStencil = {1.0f,0};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = target.render_pass;
    bi.framebuffer = target.framebuffer;
    bi.renderArea.extent = {(uint32_t)target.width,(uint32_t)target.height};
    bi.clearValueCount = 4;
    bi.pClearValues = clears;
    vkCmdBeginRenderPass(g.frame_command, &bi, VK_SUBPASS_CONTENTS_INLINE);

    for (const auto& [vpId, viewport] : scene->viewports)
    {
        (void)vpId;
        if (!viewport || !viewport->camera_) continue;
        SceneDrawState state;
        state.scene = scene;
        state.scene_id = sceneId;
        state.viewport = viewport;
        state.projection = Projection(viewport);
        state.view = View(viewport);

        SetViewportAndScissor(g.frame_command, viewport, target.width, target.height);
        ClearViewport(g.frame_command, viewport, target.width, target.height);
        DrawBuiltinSky(g.frame_command, state, scene);

        for (const auto& [meshId, mesh] : scene->meshes)
        {
            if (!mesh || mesh->type_ == HRL_SPRITE) continue;
            const int lod = SelectLOD(mesh, ModelMatrix(mesh), state.view, state.projection, viewport);
            mesh->last_lod_level_ = lod;
            DrawOneMesh(g.frame_command, state, meshId, mesh, lod);
        }

        std::vector<std::pair<HRL_id,HRL_Mesh*>> sprites;
        sprites.reserve(scene->meshes.size());
        for (const auto& [meshId, mesh] : scene->meshes)
            if (mesh && mesh->type_ == HRL_SPRITE) sprites.emplace_back(meshId,mesh);
        std::sort(sprites.begin(),sprites.end(),[](const auto& a,const auto& b){return a.second->draw_order_ < b.second->draw_order_;});
        for (const auto& [meshId, mesh] : sprites) DrawOneMesh(g.frame_command, state, meshId, mesh, 0);

        DrawVFX(g.frame_command, state, scene);
        DrawVFXMeshParticles(g.frame_command, state, scene);

        auto priv = GetPrivateContext();
        auto dit = priv->debug_renderers.find(sceneId);
        if (dit != priv->debug_renderers.end())
            DrawDebug(g.frame_command, state, dit->second, priv->debug_line_thickness);
        DrawWidgets(g.frame_command, state, viewport);
    }
    vkCmdEndRenderPass(g.frame_command);
    target.color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    bool hasPost = false;
    for (const auto& [vpId, viewport] : scene->viewports)
    {
        (void)vpId;
        if (viewport && !viewport->post_processes.empty()) { hasPost = true; break; }
    }
    const bool hasSceneEffects = scene->ambient_occlusion_enabled || scene->global_volumetric_fog.enabled ||
                                 !scene->volumetric_fogs.empty() || scene->god_rays.enabled;
    const bool canSampleDepth = target.samples == VK_SAMPLE_COUNT_1_BIT && target.depth.image;

    if (hasPost || (hasSceneEffects && canSampleDepth))
    {
        if (target.color_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            TransitionImage(g.frame_command, target.color.image, target.color_layout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            target.color_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        TransitionImage(g.frame_command, target.bright.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (canSampleDepth)
            TransitionImage(g.frame_command, target.depth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

        TransitionImage(g.frame_command, target.post_a.image, target.post_a_layout,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        target.post_a_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkImageCopy seed{};
        seed.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        seed.srcSubresource.layerCount = 1;
        seed.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        seed.dstSubresource.layerCount = 1;
        seed.extent = {(uint32_t)target.width,(uint32_t)target.height,1};
        vkCmdCopyImage(g.frame_command, target.color.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       target.post_a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &seed);
        TransitionImage(g.frame_command, target.post_a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        target.post_a_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        ImageResource* current = &target.post_a;
        VkImageLayout* currentLayout = &target.post_a_layout;
        ImageResource* other = &target.post_b;
        VkImageLayout* otherLayout = &target.post_b_layout;

        for (const auto& [vpId, viewport] : scene->viewports)
        {
            (void)vpId;
            if (!viewport || !viewport->camera_) continue;
            const glm::mat4 invPV = glm::inverse(Projection(viewport) * View(viewport));

            if (hasSceneEffects && canSampleDepth)
            {
                if (RenderSceneEffectsForViewport(target, scene, viewport, invPV, *current,
                                                  target.bright, target.depth, *other, *otherLayout))
                {
                    std::swap(current, other);
                    std::swap(currentLayout, otherLayout);
                }
            }

            std::vector<HRL_PostProcess*> postProcesses;
            postProcesses.reserve(viewport->post_processes.size());
            for (const auto& [postId, pp] : viewport->post_processes)
            {
                (void)postId;
                if (pp) postProcesses.push_back(pp);
            }
            std::stable_sort(postProcesses.begin(), postProcesses.end(), [](const HRL_PostProcess* a, const HRL_PostProcess* b)
            {
                if (a->priority_ != b->priority_) return a->priority_ < b->priority_;
                return a->id_ < b->id_;
            });

            for (const HRL_PostProcess* pp : postProcesses)
            {
                auto mit = GetPrivateContext()->materials.find(pp->material_);
                if (mit == GetPrivateContext()->materials.end() || !mit->second) continue;
                HRL_Material* material = mit->second;
                const ImageResource* depth = canSampleDepth ? &target.depth : nullptr;
                if (RunFullscreenPass(target, material->shader_, *current, &target.bright, depth,
                                       viewport, invPV, material, *other, *otherLayout))
                {
                    std::swap(current, other);
                    std::swap(currentLayout, otherLayout);
                }
            }
        }

        TransitionImage(g.frame_command, current->image, *currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        *currentLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        TransitionImage(g.frame_command, target.color.image, target.color_layout,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        target.color_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkImageCopy finalCopy{};
        finalCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        finalCopy.srcSubresource.layerCount = 1;
        finalCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        finalCopy.dstSubresource.layerCount = 1;
        finalCopy.extent = {(uint32_t)target.width,(uint32_t)target.height,1};
        vkCmdCopyImage(g.frame_command, current->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       target.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &finalCopy);
    }

    if (target.render_on_screen && g.swapchain)
    {
        TransitionImage(g.frame_command, target.color.image, target.color_layout,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        target.color_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (g.swapchain_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            TransitionImage(g.frame_command, g.swapchain_images[g.swapchain_index],
                            g.swapchain_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        g.swapchain_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkImageCopy copy{};
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource.layerCount = 1;
        copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.dstSubresource.layerCount = 1;
        copy.extent = {static_cast<uint32_t>(std::min(target.width,(int)g.swapchain_extent.width)),
                       static_cast<uint32_t>(std::min(target.height,(int)g.swapchain_extent.height)),1};
        vkCmdCopyImage(g.frame_command, target.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       g.swapchain_images[g.swapchain_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        TransitionImage(g.frame_command, g.swapchain_images[g.swapchain_index],
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        g.swapchain_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        g.present_requested = true;
    }
}

int VK_CreateMesh(HRL_id id,const HRL_Vertex3D* vertices,size_t vertexCount,const HRL_uint* indices,size_t indexCount)
{
    MeshGPU gpu;
    gpu.skeletal=false;
    gpu.levels.resize(1);
    gpu.levels[0].vertex_count=static_cast<uint32_t>(vertexCount);
    gpu.levels[0].index_count=static_cast<uint32_t>(indexCount);
    if(!UploadBuffer(vertices,vertexCount*sizeof(HRL_Vertex3D),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,gpu.levels[0].vertex))return HRL_FALSE;
    if(indexCount>0)
    {
        if(!UploadBuffer(indices,indexCount*sizeof(HRL_uint),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,gpu.levels[0].index))
        { DestroyBuffer(gpu.levels[0].vertex); return HRL_FALSE; }
    }
    g.meshes[id]=std::move(gpu);
    return HRL_TRUE;
}

int VK_CreateSkeletalMesh(HRL_id id,const HRL_SkeletalVertex* vertices,size_t vertexCount,const HRL_uint* indices,size_t indexCount,HRL_uint /*boneCount*/)
{
    MeshGPU gpu;
    gpu.skeletal=true;
    gpu.skeletal_vertex_count=static_cast<uint32_t>(vertexCount);
    gpu.skeletal_index_count=static_cast<uint32_t>(indexCount);
    if(!UploadBuffer(vertices,vertexCount*sizeof(HRL_SkeletalVertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,gpu.skeletal_vertex))return HRL_FALSE;
    if(indexCount>0)
    {
        if(!UploadBuffer(indices,indexCount*sizeof(HRL_uint),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,gpu.skeletal_index))
        { DestroyBuffer(gpu.skeletal_vertex); return HRL_FALSE; }
    }
    g.meshes[id]=std::move(gpu);
    return HRL_TRUE;
}

int VK_CreateMeshLOD(HRL_id id,HRL_uint level,const HRL_Vertex3D* vertices,size_t vertexCount,const HRL_uint* indices,size_t indexCount)
{
    auto it=g.meshes.find(id);
    if(it==g.meshes.end())return HRL_FALSE;
    MeshGPU& gpu=it->second;
    if(gpu.skeletal)return HRL_FALSE;
    if(level>=gpu.levels.size())gpu.levels.resize(level+1);
    MeshLevelGPU& dst=gpu.levels[level];
    DestroyBuffer(dst.vertex);
    DestroyBuffer(dst.index);
    dst.vertex_count=static_cast<uint32_t>(vertexCount);
    dst.index_count=static_cast<uint32_t>(indexCount);
    if(!UploadBuffer(vertices,vertexCount*sizeof(HRL_Vertex3D),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,dst.vertex))return HRL_FALSE;
    if(indexCount>0 && !UploadBuffer(indices,indexCount*sizeof(HRL_uint),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,dst.index))return HRL_FALSE;
    return HRL_TRUE;
}

void VK_DeleteMeshLODs(HRL_id id){auto it=g.meshes.find(id);if(it==g.meshes.end()||it->second.skeletal)return;for(size_t i=1;i<it->second.levels.size();++i){DestroyBuffer(it->second.levels[i].vertex);DestroyBuffer(it->second.levels[i].index);}if(it->second.levels.size()>1)it->second.levels.resize(1);}

int VK_CreateSpriteMesh(HRL_id id){const HRL_Vertex3D v[4]={{ {-0.5f,-0.5f,0},{0,0,1},{0,0},{1,0,0},{0,1,0}},{{0.5f,-0.5f,0},{0,0,1},{1,0},{1,0,0},{0,1,0}},{{0.5f,0.5f,0},{0,0,1},{1,1},{1,0,0},{0,1,0}},{{-0.5f,0.5f,0},{0,0,1},{0,1},{1,0,0},{0,1,0}}};const HRL_uint ind[6]={0,1,2,2,3,0};return VK_CreateMesh(id,v,4,ind,6);}
void VK_DeleteMesh(HRL_id id){auto it=g.meshes.find(id);if(it==g.meshes.end())return;if(g.device)vkDeviceWaitIdle(g.device);for(auto& l:it->second.levels){DestroyBuffer(l.vertex);DestroyBuffer(l.index);}DestroyBuffer(it->second.skeletal_vertex);DestroyBuffer(it->second.skeletal_index);g.meshes.erase(it);}

void VK_UpdateLights(const std::vector<HRL_Light*>& lights){
    g.lights = LightBlockGPU{};
    for (auto* l : lights) {
        if (!l || g.lights.count >= kMaxLights) break;
        LightGPU& d = g.lights.lights[g.lights.count++];
        d.type = l->type_;
        d.intensity = l->intensity_;
        d.attenuation = std::max(0.0f, l->attenuation_);
        d.innerCutoff = std::cos(glm::radians(glm::clamp(l->innerCutoff, 0.0f, 89.9f)));
        d.position_outer = glm::vec4(l->position_,
                                     std::cos(glm::radians(glm::clamp(l->outerCutoff, 0.0f, 89.9f))));
        d.rotation_padding = glm::vec4(GetLightDirection(l), 0.0f);
        d.color_shadow = glm::vec4(glm::max(l->color_, glm::vec3(0.0f)),
                                   glm::clamp(l->shadow_strength_, 0.0f, 1.0f));
        d.shadowMatrix = glm::mat4(1.0f);
        d.shadowParams = glm::vec4(l->shadow_bias_, 0.0f, -1.0f, 0.0f);
    }
    if (g.light_mapped)
        std::memcpy(g.light_mapped, &g.lights, sizeof(g.lights));
}
void VK_DeleteLight(HRL_id id){DestroyShadowResource(id);for(auto&[sid,slots]:g.active_shadow_slots_by_scene)slots.erase(id);}

HRL_id VK_CreateTexture(const char* data,size_t size){const HRL_id id=GenerateHRL_ID();VulkanTexture tex;std::string err;if(!tex.CreateFromEncoded(g.physical,g.device,g.command_pool,g.graphics_queue,data,size,g.max_anisotropy,err)){SetErrorCode(HRL_INVALID_FILE_FORMAT,HRL_SEVERITY_ERROR,"Vulkan texture: "+err);return HRL_INVALID_ID;}g.textures.emplace(id,std::move(tex));return id;}
HRL_id VK_CreateTextureFromBitmap(BitmapResult bmp){const HRL_id id=GenerateHRL_ID();VulkanTexture tex;std::string err;if(!tex.Create(g.physical,g.device,g.command_pool,g.graphics_queue,bmp,g.max_anisotropy,err)){SetErrorCode(HRL_OUT_OF_MEMORY,HRL_SEVERITY_ERROR,"Vulkan bitmap texture: "+err);return HRL_INVALID_ID;}g.textures.emplace(id,std::move(tex));return id;}
HRL_id VK_CreateTextureFromBitmapWithId(HRL_id id,BitmapResult bmp){VulkanTexture tex;std::string err;if(!tex.Create(g.physical,g.device,g.command_pool,g.graphics_queue,bmp,g.max_anisotropy,err)){SetErrorCode(HRL_OUT_OF_MEMORY,HRL_SEVERITY_ERROR,"Vulkan bitmap texture: "+err);return HRL_INVALID_ID;}auto it=g.textures.find(id);if(it!=g.textures.end())it->second.Destroy(g.device);g.textures[id]=std::move(tex);return id;}
void VK_DeleteTexture(HRL_id id){auto it=g.textures.find(id);if(it==g.textures.end())return;if(g.device)vkDeviceWaitIdle(g.device);it->second.Destroy(g.device);g.textures.erase(it);}void VK_GetTextureSize(HRL_id id,int*w,int*h){auto it=g.textures.find(id);if(it==g.textures.end()){if(w)*w=0;if(h)*h=0;return;}if(w)*w=(int)it->second.width;if(h)*h=(int)it->second.height;}
void VK_SetTextureMinFilter(HRL_id id,HRL_EFilterType f){auto it=g.textures.find(id);if(it==g.textures.end())return;if(g.device)vkDeviceWaitIdle(g.device);std::string e;if(!it->second.SetMinFilter(g.physical,g.device,f,e)&&!e.empty())SetErrorCode(HRL_INVALID_BACKEND_OPERATION,HRL_SEVERITY_ERROR,e);}
void VK_SetTextureMaxFilter(HRL_id id,HRL_EFilterType f){auto it=g.textures.find(id);if(it==g.textures.end())return;if(g.device)vkDeviceWaitIdle(g.device);std::string e;if(!it->second.SetMagFilter(g.physical,g.device,f,e)&&!e.empty())SetErrorCode(HRL_INVALID_BACKEND_OPERATION,HRL_SEVERITY_ERROR,e);}

void VK_CreateScene(HRL_id id,int renderOnScreen){if(!g.initialized||!g.device){SetErrorCode(HRL_INVALID_BACKEND_OPERATION,HRL_SEVERITY_ERROR,"Vulkan: backend is not initialized");return;}SceneGPU s;auto dims=SceneDimensions(nullptr);s.width=dims.first;s.height=dims.second;s.render_on_screen=renderOnScreen;s.color_format=(renderOnScreen&&g.swapchain)?g.swapchain_format:VK_FORMAT_B8G8R8A8_UNORM;s.picking_format=VK_FORMAT_R8G8B8A8_UNORM;s.depth_format=FindDepthFormat();s.samples=g.msaa;if(!CreateSceneTargets(s)){DestroySceneResources(s);SetErrorCode(HRL_INVALID_BACKEND_OPERATION,HRL_SEVERITY_ERROR,"Vulkan: failed to create scene targets");return;}g.scenes.emplace(id,std::move(s));}
void VK_DeleteScene(HRL_id id){auto it=g.scenes.find(id);if(it==g.scenes.end())return;if(g.device)vkDeviceWaitIdle(g.device);DestroySceneResources(it->second);g.scenes.erase(it);}
void VK_ResizeSceneTexture(HRL_id id,int w,int h){auto it=g.scenes.find(id);if(it==g.scenes.end())return;if(w<=0||h<=0)return;it->second.width=w;it->second.height=h;RecreateScene(it->second);}void VK_EnableColorPickingBuffer(HRL_id id,int enable){auto it=g.scenes.find(id);if(it!=g.scenes.end())it->second.picking=enable!=0;}
void VK_SetAntialiasingMode(int samples){g.msaa=ChooseMSAA(samples);for(auto&[id,s]:g.scenes){s.samples=g.msaa;RecreateScene(s);} }

HRL_id VK_CreateShaderWithId(HRL_id id,const char* v,size_t vs,const char* f,size_t fs);

HRL_id VK_CreateShader(const char* v,size_t vs,const char* f,size_t fs){HRL_id id=GenerateHRL_ID();return VK_CreateShaderWithId(id,v,vs,f,fs);}HRL_id VK_CreateShaderWithId(HRL_id id,const char* v,size_t vs,const char* f,size_t fs){VulkanShader shader;std::string err;if(!shader.Build(g.device,v,vs,f,fs,err)){SetErrorCode(HRL_SHADER_COMPILE_FAIL,HRL_SEVERITY_ERROR,"Vulkan shader: "+err);return HRL_INVALID_ID;}g.shaders[id]=std::move(shader);return id;}
void VK_DeleteShader(HRL_id id){auto it=g.shaders.find(id);if(it==g.shaders.end())return;if(g.device)vkDeviceWaitIdle(g.device);it->second.Destroy(g.device);g.shaders.erase(it);for(auto pit=g.pipelines.begin();pit!=g.pipelines.end();)if(pit->first.shader==id){vkDestroyPipeline(g.device,pit->second,nullptr);pit=g.pipelines.erase(pit);}else++pit;}

void VK_GetProjectionMatrix(float* out){if(!out)return;auto priv=GetPrivateContext();if(priv->viewports.empty() || priv->scenes.empty()){std::fill(out,out+16,0);return;}const auto& sceneIt=*priv->scenes.begin();if(sceneIt.second->viewports.empty()){std::fill(out,out+16,0);return;}auto vpIt=*sceneIt.second->viewports.begin();glm::mat4 m=Projection(vpIt.second);std::memcpy(out,&m,sizeof(m));}
void VK_GetViewMatrix(float* out){if(!out)return;auto priv=GetPrivateContext();if(priv->scenes.empty()||priv->scenes.begin()->second->viewports.empty()){std::fill(out,out+16,0);return;}glm::mat4 m=View(priv->scenes.begin()->second->viewports.begin()->second);std::memcpy(out,&m,sizeof(m));}
void VK_GetModelMatrix(HRL_Mesh* mesh,float* out){if(!out||!mesh)return;glm::mat4 m=ModelMatrix(mesh);std::memcpy(out,&m,sizeof(m));}
void VK_CreatePostProcess(HRL_id,int){}void VK_DeletePostProcess(HRL_id){}
int VK_GISupported(int method){return method==HRL_GI_NONE?HRL_TRUE:HRL_FALSE;}uint32_t VK_GISupportedMethods(){return 1u<<HRL_GI_NONE;}
void VK_FogChanged(HRL_id,hrl_fog_t*){}
void VK_DrawDebug(const DebugRenderer& r,float line){(void)r;(void)line;}
int VK_IsValidTexture(HRL_id id){return g.textures.find(id)!=g.textures.end()?HRL_TRUE:HRL_FALSE;}int VK_IsValidShader(HRL_id id){return g.shaders.find(id)!=g.shaders.end()?HRL_TRUE:HRL_FALSE;}
void VK_BindViewport(HRL_Viewport*){}void VK_ComputeFrameMatrices(){}void VK_BindMaterial(HRL_Material*){}void VK_BindScene(HRL_id){}void VK_ClearScene(){}
}

void VulkanSetSurfaceCallback(HRL_VulkanCreateSurfaceCallback callback, void* user_data)
{
    if (g.initialized || g.instance)
    {
        SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
                     "Vulkan: surface callback must be configured before HRL_InitContext");
        return;
    }
    g.create_surface_callback = callback;
    g.surface_user_data = user_data;
}

void VulkanSetInstanceExtensions(const char* const* extensions, HRL_uint extension_count)
{
    if (g.initialized || g.instance)
    {
        SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
                     "Vulkan: instance extensions must be configured before HRL_InitContext");
        return;
    }
    g.instance_extensions.clear();
    if (!extensions) return;
    g.instance_extensions.reserve(extension_count);
    for (HRL_uint i = 0; i < extension_count; ++i)
        if (extensions[i] && *extensions[i]) g.instance_extensions.emplace_back(extensions[i]);
}

VkInstance VulkanGetInstance() { return g.instance; }
VkPhysicalDevice VulkanGetPhysicalDevice() { return g.physical; }
VkDevice VulkanGetDevice() { return g.device; }
VkQueue VulkanGetGraphicsQueue() { return g.graphics_queue; }
VkSurfaceKHR VulkanGetSurface() { return g.surface; }

HRL_vtable GetVulkanBackend()
{
    HRL_vtable v{};
    v.RHI_Init = VK_Init;
    v.RHI_InitContext = VK_InitContext;
    v.RHI_Shutdown = VK_Shutdown;
    v.RHI_WindowResizeCallback = VK_WindowResizeCallback;
    v.RHI_TakeScreenshot = VK_TakeScreenshot;
    v.RHI_BeginFrame = VK_BeginFrame;
    v.RHI_RenderScene = VK_RenderScene;
    v.RHI_ResetFramebuffer = VK_ResetFramebuffer;
    v.RHI_BindViewport = VK_BindViewport;
    v.RHI_ComputeFrameMatrices = VK_ComputeFrameMatrices;
    v.RHI_BindMaterial = VK_BindMaterial;
    v.RHI_DrawMesh = nullptr;
    v.RHI_CreateMesh = VK_CreateMesh;
    v.RHI_CreateSkeletalMesh = VK_CreateSkeletalMesh;
    v.RHI_CreateMeshLOD = VK_CreateMeshLOD;
    v.RHI_DeleteMeshLODs = VK_DeleteMeshLODs;
    v.RHI_CreateSpriteMesh = VK_CreateSpriteMesh;
    v.RHI_DeleteMesh = VK_DeleteMesh;
    v.RHI_UpdateLights = VK_UpdateLights;
    v.RHI_DeleteLight = VK_DeleteLight;
    v.RHI_CreateTexture = VK_CreateTexture;
    v.RHI_CreateTextureFromBitmap = VK_CreateTextureFromBitmap;
    v.RHI_CreateTextureFromBitmapWithId = VK_CreateTextureFromBitmapWithId;
    v.RHI_DeleteTexture = VK_DeleteTexture;
    v.RHI_GetTextureSize = VK_GetTextureSize;
    v.RHI_SetTextureMinFilter = VK_SetTextureMinFilter;
    v.RHI_SetTextureMaxFilter = VK_SetTextureMaxFilter;
    v.RHI_CreateScene = VK_CreateScene;
    v.RHI_DeleteScene = VK_DeleteScene;
    v.RHI_BindScene = VK_BindScene;
    v.RHI_ClearScene = VK_ClearScene;
    v.RHI_ResizeSceneTexture = VK_ResizeSceneTexture;
    v.RHI_EnableColorPickingBuffer = VK_EnableColorPickingBuffer;
    v.RHI_SetAntialiasingMode = VK_SetAntialiasingMode;
    v.RHI_CreateShader = VK_CreateShader;
    v.RHI_CreateShaderWithId = VK_CreateShaderWithId;
    v.RHI_DeleteShader = VK_DeleteShader;
    v.RHI_GetProjectionMatrix = VK_GetProjectionMatrix;
    v.RHI_GetViewMatrix = VK_GetViewMatrix;
    v.RHI_GetModelMatrix = VK_GetModelMatrix;
    v.RHI_CreatePostProcess = VK_CreatePostProcess;
    v.RHI_DeletePostProcess = VK_DeletePostProcess;
    v.RHI_IsGlobalIlluminationMethodSupported = VK_GISupported;
    v.RHI_GetGlobalIlluminationSupportedMethods = VK_GISupportedMethods;
    v.RHI_FogPropertyChanged = VK_FogChanged;
    v.RHI_DrawDebug = VK_DrawDebug;
    v.RHI_IsValidTexture = VK_IsValidTexture;
    v.RHI_IsValidShader = VK_IsValidShader;
    return v;
}
