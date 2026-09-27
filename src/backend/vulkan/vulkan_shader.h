#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct VulkanUniformField
{
    enum class Type
    {
        Float,
        Int,
        UInt,
        Bool,
        Vec2,
        Vec3,
        Vec4,
        Mat3,
        Mat4,
        Mat4Array
    } type = Type::Float;

    std::string name;
    size_t offset = 0;
    size_t size = 0;
    uint32_t array_count = 1;
};

class VulkanShader
{
public:
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;

    std::vector<VulkanUniformField> uniforms;
    std::unordered_map<std::string, uint32_t> samplers;
    size_t uniform_block_size = 0;

    ~VulkanShader() = default;

    bool Build(VkDevice device,
               const char* vert_data, size_t vert_size,
               const char* frag_data, size_t frag_size,
               std::string& error);
    void Destroy(VkDevice device);
};
