// HRL Vulkan backend - shader programs.
//
// HRL shaders (built-in and user shaders) are written in OpenGL GLSL 3.30.
// The Vulkan backend accepts exactly the same sources: VulkanProgram
// translates them to Vulkan GLSL 4.50 (descriptor bindings, explicit varying
// locations, default-block uniforms gathered in a std140 UBO, depth range
// remap), compiles them to SPIR-V and reflects the result. Uniform values are
// then set by name with the same calls as GL33_Shader (SetInt, SetVec3, ...)
// and keep their value between draws, like OpenGL program uniforms.
#ifndef HRL_VULKAN_SHADER_H
#define HRL_VULKAN_SHADER_H

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hrlvk
{
// Fixed descriptor set layout used by every program.
constexpr uint32_t kDefaultBlockBinding = 0;     // default uniform block (dynamic UBO)
constexpr uint32_t kUniformBlockPointBase = 1;   // GL uniform buffer binding points 0..4 -> bindings 1..5
constexpr uint32_t kUniformBlockPoints = 5;
constexpr uint32_t kSamplerBindingBase = 8;      // samplers -> bindings 8..8+kMaxSamplers-1
constexpr uint32_t kMaxSamplers = 24;

enum class SamplerDim { Tex2D, Cube, Array2D, Tex3D, Tex2DMS, Unsupported };

struct ShaderUniform
{
	enum class Base { Float, Int, UInt, Bool };
	std::string name;       // flattened name ("Color", "lights[2].pos", ...)
	Base base = Base::Float;
	uint32_t components = 1; // rows of a matrix / components of a vector
	uint32_t columns = 1;    // > 1 for matrices
	uint32_t offset = 0;
	uint32_t arraySize = 1;  // 1 for non-arrays
	uint32_t arrayStride = 0;
	uint32_t matrixStride = 16;
};

struct ShaderSampler
{
	std::string name;
	uint32_t binding = 0;
	SamplerDim dim = SamplerDim::Tex2D;
	int unit = 0; // OpenGL texture unit (glUniform1i on the sampler)
};

struct ShaderVertexInput
{
	uint32_t location = 0;
	ShaderUniform::Base base = ShaderUniform::Base::Float;
	uint32_t components = 4;
};

class VulkanProgram
{
public:
	VulkanProgram() = default;
	~VulkanProgram();
	VulkanProgram(const VulkanProgram&) = delete;
	VulkanProgram& operator=(const VulkanProgram&) = delete;

	// Translates + compiles. Thread safe as long as each program is distinct.
	bool Create(const char* vert, size_t vertSize, const char* frag, size_t fragSize, std::string& error);
	void Destroy();

	// GL33_Shader-compatible uniform setters (type rules of glUniform*).
	// true if the program has an active uniform / sampler with this name
	bool Has(const char* name) { Location l; return Find(name, l); }
	void SetInt(const char* name, int value);
	void SetUint(const char* name, uint32_t value);
	void SetFloat(const char* name, float value);
	void SetVec2(const char* name, const glm::vec2& value);
	void SetVec3(const char* name, const glm::vec3& value);
	void SetVec4(const char* name, const glm::vec4& value);
	void SetMat3(const char* name, const glm::mat3& value);
	void SetMat4(const char* name, const glm::mat4& value);
	void SetInt(const std::string& name, int value) { SetInt(name.c_str(), value); }
	void SetUint(const std::string& name, uint32_t value) { SetUint(name.c_str(), value); }
	void SetFloat(const std::string& name, float value) { SetFloat(name.c_str(), value); }
	void SetVec2(const std::string& name, const glm::vec2& value) { SetVec2(name.c_str(), value); }
	void SetVec3(const std::string& name, const glm::vec3& value) { SetVec3(name.c_str(), value); }
	void SetVec4(const std::string& name, const glm::vec4& value) { SetVec4(name.c_str(), value); }
	void SetMat3(const std::string& name, const glm::mat3& value) { SetMat3(name.c_str(), value); }
	void SetMat4(const std::string& name, const glm::mat4& value) { SetMat4(name.c_str(), value); }

	VkShaderModule vertexModule = VK_NULL_HANDLE;
	VkShaderModule fragmentModule = VK_NULL_HANDLE;
	std::vector<ShaderUniform> uniforms;
	std::vector<ShaderSampler> samplers;
	std::vector<ShaderVertexInput> vertexInputs;
	std::vector<unsigned char> data;  // default block values (std140)
	uint32_t blockSize = 0;            // size of the default block (0 : none)
	uint32_t uniformBlockMask = 0;     // bit i : uses GL uniform buffer binding point i
	uint32_t fragmentOutputs = 1;      // highest fragment output location + 1
	uint64_t id = 0;                   // unique program id (pipeline cache key)
	uint64_t dataVersion = 1;          // incremented when a value changes

private:
	struct Location { int uniform = -1; int sampler = -1; uint32_t element = 0; };
	bool Find(const char* name, Location& loc);
	unsigned char* Slot(const ShaderUniform& u, uint32_t element);
	std::unordered_map<std::string, Location> lookup_;
};

// Global compile helpers.
// Translates one stage (exposed for tests / diagnostics).
std::string TranslateGLSLForVulkan(const std::string& vert, const std::string& frag, bool vertexStage, std::string& error);
} // namespace hrlvk

#endif
