#ifndef HRL_VK_COMPILE_H
#define HRL_VK_COMPILE_H

#include <cstdint>
#include <string>
#include <vector>

namespace hrlvk
{
// Compiles Vulkan GLSL (#version 450) to SPIR-V, using the on-disk cache.
bool CompileGLSLToSpirv(const std::string& source, bool vertexStage, std::vector<uint32_t>& spirv, std::string& error);
}

#endif
