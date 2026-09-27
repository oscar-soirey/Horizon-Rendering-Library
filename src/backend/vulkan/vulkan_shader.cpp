#include "vulkan_shader.h"

#include "../../core/utils_functions.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <string_view>
#include <system_error>
#if defined(_WIN32)
#  include <windows.h>
#endif

namespace
{
struct ParsedUniform
{
    std::string type;
    std::string name;
    bool sampler = false;
    uint32_t array_count = 1;
};

static std::string ReadCommandOutput(const std::string& command)
{
    std::string output;
#if defined(_WIN32)
    const std::string cmd = command + " 2>&1";
#else
    const std::string cmd = command + " 2>&1";
#endif
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe)
        return output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe))
        output += buffer;
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
}

static bool CommandWorks(const std::string& command)
{
#if defined(_WIN32)
    return std::system((command + " --version > nul 2>&1").c_str()) == 0;
#else
    return std::system((command + " --version > /dev/null 2>&1").c_str()) == 0;
#endif
}

static std::string Quote(const std::string& value)
{
#if defined(_WIN32)
    std::string q = "\"";
    for (char c : value)
    {
        if (c == '"') q += "\\\"";
        else q += c;
    }
    q += "\"";
    return q;
#else
    std::string q = "'";
    for (char c : value)
    {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    q += "'";
    return q;
#endif
}

static std::string FindCompiler()
{
    if (const char* explicit_path = std::getenv("HRL_GLSLANG_VALIDATOR"))
    {
        if (*explicit_path)
            return explicit_path;
    }

    if (const char* sdk = std::getenv("VULKAN_SDK"))
    {
        std::filesystem::path base(sdk);
        std::filesystem::path candidate = base / "Bin" / "glslangValidator";
#if defined(_WIN32)
        candidate += ".exe";
#else
        if (!std::filesystem::exists(candidate)) candidate = base / "bin" / "glslangValidator";
#endif
        if (std::filesystem::exists(candidate))
            return candidate.string();
    }

#if defined(_WIN32)
    if (CommandWorks("glslangValidator")) return "glslangValidator";
    if (CommandWorks("glslc")) return "glslc";
#else
    if (CommandWorks("glslangValidator")) return "glslangValidator";
    if (CommandWorks("glslc")) return "glslc";
#endif
    return {};
}

static bool IsSamplerType(const std::string& type)
{
    return type == "sampler2D" || type == "samplerCube" ||
           type == "sampler2DShadow" || type == "samplerCubeShadow" ||
           type == "sampler3D" || type == "sampler2DArray" ||
           type == "sampler2DArrayShadow";
}

static bool IsNumericType(const std::string& type)
{
    return type == "float" || type == "int" || type == "uint" || type == "bool" ||
           type == "vec2" || type == "vec3" || type == "vec4" ||
           type == "mat3" || type == "mat4";
}

static std::vector<ParsedUniform> ParseUniforms(const std::string& source)
{
    std::vector<ParsedUniform> out;
    const std::regex rx(R"((?:layout\s*\([^)]*\)\s*)?uniform\s+(bool|int|uint|float|vec2|vec3|vec4|mat3|mat4|sampler2D|samplerCube|sampler2DShadow|samplerCubeShadow|sampler3D|sampler2DArray|sampler2DArrayShadow)\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*\[\s*(\d+)\s*\])?\s*;)");
    std::smatch match;
    std::string::const_iterator begin = source.begin();
    const std::string::const_iterator end = source.end();
    while (std::regex_search(begin, end, match, rx))
    {
        ParsedUniform u;
        u.type = match[1].str();
        u.name = match[2].str();
        u.sampler = IsSamplerType(u.type);
        if (match[3].matched)
            u.array_count = std::max(1u, static_cast<uint32_t>(std::strtoul(match[3].str().c_str(), nullptr, 10)));
        out.push_back(std::move(u));
        begin = match.suffix().first;
    }
    return out;
}

static std::string ReplaceUniformDeclarations(const std::string& source,
                                              const std::unordered_map<std::string, uint32_t>& sampler_bindings,
                                              const std::vector<ParsedUniform>& block_uniforms)
{
    std::string out;
    out.reserve(source.size() + 2048);

    const std::regex decl(R"((?:layout\s*\([^)]*\)\s*)?uniform\s+(bool|int|uint|float|vec2|vec3|vec4|mat3|mat4|sampler2D|samplerCube|sampler2DShadow|samplerCubeShadow|sampler3D|sampler2DArray|sampler2DArrayShadow)\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*\[\s*(\d+)\s*\])?\s*;)");
    std::smatch m;
    std::string::const_iterator cursor = source.begin();
    const std::string::const_iterator end = source.end();
    bool block_inserted = false;
    while (std::regex_search(cursor, end, m, decl))
    {
        out.append(cursor, m.prefix().second);

        const std::string type = m[1].str();
        const std::string name = m[2].str();
        const bool sampler = IsSamplerType(type);
        if (sampler)
        {
            auto it = sampler_bindings.find(name);
            if (it != sampler_bindings.end())
            {
                if (m[3].matched)
                {
                    out += "layout(set=0,binding=" + std::to_string(it->second) + ") uniform " + type + " " + name + "[" + m[3].str() + "];";
                }
                else
                {
                    out += "layout(set=0,binding=" + std::to_string(it->second) + ") uniform " + type + " " + name + ";";
                }
            }
            else
            {
                out.append(m[0].first, m[0].second);
            }
        }
        else if (IsNumericType(type))
        {
            if (!block_inserted)
            {
                out += "layout(set=0,binding=31,std140) uniform HRLUniformBlock {\n";
                for (const ParsedUniform& u : block_uniforms)
                {
                    if (!u.sampler && IsNumericType(u.type))
                    {
                        out += "  " + u.type + " " + u.name;
                        if (u.array_count > 1)
                            out += "[" + std::to_string(u.array_count) + "]";
                        out += ";\n";
                    }
                }
                out += "};\n";
                block_inserted = true;
            }
            // Members of an anonymous block are visible under their original names.
        }
        else
        {
            out.append(m[0].first, m[0].second);
        }
        cursor = m.suffix().first;
    }
    out.append(cursor, end);

    // If the shader had no numeric uniform declaration, it still receives the
    // engine's fixed descriptor layout; no block is necessary in GLSL.
    return out;
}

static size_t AlignUp(size_t value, size_t alignment)
{
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

static size_t Std140BaseAlignment(const std::string& type)
{
    if (type == "vec2") return 8;
    if (type == "vec3" || type == "vec4" || type == "mat3" || type == "mat4") return 16;
    return 4;
}

static size_t Std140ElementSize(const std::string& type)
{
    if (type == "float" || type == "int" || type == "uint" || type == "bool") return 4;
    if (type == "vec2") return 8;
    if (type == "vec3") return 16;
    if (type == "vec4") return 16;
    if (type == "mat3") return 48;
    if (type == "mat4") return 64;
    return 4;
}

static bool IsSpirv(const void* data, size_t size)
{
    if (!data || size < 4 || size % 4 != 0) return false;
    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    return magic == 0x07230203u;
}

static bool WriteBytes(const std::filesystem::path& path, const void* data, size_t size)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file.good();
}

static bool ReadBinary(const std::filesystem::path& path, std::vector<uint32_t>& words)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size <= 0 || size % 4 != 0) return false;
    file.seekg(0, std::ios::beg);
    words.resize(static_cast<size_t>(size) / 4u);
    file.read(reinterpret_cast<char*>(words.data()), size);
    return file.good();
}

#if defined(_WIN32)
static std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size) <= 0)
        return {};
    return result;
}

static std::wstring QuoteWindowsArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : value)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (c == L'\"')
        {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(c);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

static bool RunCompilerWindows(const std::string& compiler,
                               const std::string& arguments,
                               std::string& output)
{
    const std::wstring compilerW = Utf8ToWide(compiler);
    const std::wstring argumentsW = Utf8ToWide(arguments);
    if (compilerW.empty())
    {
        output = "Vulkan shader compiler: invalid compiler path";
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        output = "Vulkan shader compiler: CreatePipe failed";
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring commandLine;
    const bool hasPath = compiler.find('\\') != std::string::npos ||
                         compiler.find('/') != std::string::npos ||
                         compiler.find(':') != std::string::npos;
    commandLine = hasPath ? QuoteWindowsArgument(compilerW) : compilerW;
    if (!argumentsW.empty())
    {
        commandLine.push_back(L' ');
        commandLine += argumentsW;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    const BOOL created = CreateProcessW(
        hasPath ? compilerW.c_str() : nullptr,
        mutableCommand.data(),
        nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(writePipe);
    if (!created)
    {
        CloseHandle(readPipe);
        output = "Vulkan shader compiler: CreateProcess failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    // Read the pipe while the compiler runs. This keeps the child process from
    // blocking if it writes a large diagnostic stream.
    char buffer[512];
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available > 0)
        {
            DWORD read = 0;
            if (!ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
            output.append(buffer, buffer + read);
            continue;
        }
        const DWORD wait = WaitForSingleObject(pi.hProcess, 10);
        if (wait == WAIT_OBJECT_0)
            break;
    }
    DWORD available = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &available, nullptr) && available > 0)
        output.append(buffer, buffer + available);
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}
#endif

static bool CompileStage(const std::string& compiler,
                         const std::string& source,
                         const char* stage,
                         std::vector<uint32_t>& spirv,
                         std::string& error)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::error_code tempEc;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(tempEc);
    if (tempEc || dir.empty())
    {
        error = "Vulkan shader compiler: unable to determine the system temporary directory";
        return false;
    }
    const std::filesystem::path base = dir / ("hrl_vulkan_" + std::to_string(stamp));
    const std::filesystem::path in_path = base.string() + (stage[0] == 'v' ? ".vert" : ".frag");
    const std::filesystem::path out_path = base.string() + ".spv";

    if (!WriteBytes(in_path, source.data(), source.size()))
    {
        error = "Vulkan shader compiler: failed to create temporary source file";
        return false;
    }

    const std::string quotedCompiler = Quote(compiler);
    std::string command;
    if (compiler.find("glslc") != std::string::npos)
    {
        command = quotedCompiler + " -fshader-stage=" + stage + " -fentry-point=main -o " + Quote(out_path.string()) + " " + Quote(in_path.string());
    }
    else
    {
        command = quotedCompiler + " -V --target-env vulkan1.1 --auto-map-locations --auto-map-bindings -S " + stage + " -e main -o " + Quote(out_path.string()) + " " + Quote(in_path.string());
    }

    std::string compilerOutput;
#if defined(_WIN32)
    // Use CreateProcess instead of cmd.exe here. This avoids Windows command-line
    // parsing issues with spaces, parentheses and SDK installation paths.
    const std::string arguments = command.substr(quotedCompiler.size() + 1);
    RunCompilerWindows(compiler, arguments, compilerOutput);
#else
    compilerOutput = ReadCommandOutput(command);
#endif
    if (!std::filesystem::exists(out_path) || !ReadBinary(out_path, spirv))
    {
        error = "Vulkan shader compilation failed";
        if (!compilerOutput.empty())
        {
            error += ": ";
            error += compilerOutput;
        }
        else
        {
            error += ". Command: ";
            error += command;
        }
        std::error_code ec;
        std::filesystem::remove(in_path, ec);
        std::filesystem::remove(out_path, ec);
        return false;
    }

    std::error_code ec;
    std::filesystem::remove(in_path, ec);
    std::filesystem::remove(out_path, ec);
    return true;
}

static std::string BuildVulkanSource(const std::string& src,
                                     const std::unordered_map<std::string, uint32_t>& sampler_bindings,
                                     const std::vector<ParsedUniform>& uniforms)
{
    std::string result = src;
    const auto versionPos = result.find("#version");
    if (versionPos == std::string::npos)
        result = "#version 450\n" + result;
    else
    {
        const size_t lineEnd = result.find('\n', versionPos);
        if (lineEnd != std::string::npos)
        {
            std::string version = result.substr(versionPos, lineEnd - versionPos);
            // Vulkan GLSL is easier to consume from user code when we normalize
            // legacy desktop GLSL versions to 450; interface declarations remain.
            if (version.find("330") != std::string::npos || version.find("400") != std::string::npos || version.find("410") != std::string::npos)
                result.replace(versionPos, lineEnd - versionPos, "#version 450");
        }
    }
    return ReplaceUniformDeclarations(result, sampler_bindings, uniforms);
}
}

bool VulkanShader::Build(VkDevice device,
                         const char* vert_data, size_t vert_size,
                         const char* frag_data, size_t frag_size,
                         std::string& error)
{
    Destroy(device);
    if (!device || !vert_data || !frag_data || vert_size == 0 || frag_size == 0)
    {
        error = "Vulkan shader: invalid source buffer";
        return false;
    }

    std::string vert_source(vert_data, vert_data + vert_size);
    std::string frag_source(frag_data, frag_data + frag_size);

    const std::vector<ParsedUniform> vertexUniforms = ParseUniforms(vert_source);
    const std::vector<ParsedUniform> fragmentUniforms = ParseUniforms(frag_source);

    std::vector<ParsedUniform> merged;
    std::unordered_map<std::string, bool> seen;
    for (const auto& u : vertexUniforms)
    {
        if (!seen.emplace(u.name, true).second) continue;
        if (IsNumericType(u.type)) merged.push_back(u);
    }
    for (const auto& u : fragmentUniforms)
    {
        if (!seen.emplace(u.name, true).second) continue;
        if (IsNumericType(u.type)) merged.push_back(u);
    }

    std::unordered_map<std::string, uint32_t> bindings;
    uint32_t nextBinding = 0;
    const auto addSamplers = [&](const std::vector<ParsedUniform>& list) -> bool {
        for (const auto& u : list)
        {
            if (!u.sampler) continue;
            if (!bindings.count(u.name))
            {
                if (nextBinding >= 16)
                {
                    error = "Vulkan shader: more than 16 sampled-image bindings are not supported";
                    return false;
                }
                bindings.emplace(u.name, nextBinding++);
            }
        }
        return true;
    };
    if (!addSamplers(vertexUniforms) || !addSamplers(fragmentUniforms))
        return false;

    std::vector<uint32_t> vertSpirv;
    std::vector<uint32_t> fragSpirv;

    if (IsSpirv(vert_data, vert_size))
        vertSpirv.assign(reinterpret_cast<const uint32_t*>(vert_data), reinterpret_cast<const uint32_t*>(vert_data) + vert_size / 4u);
    if (IsSpirv(frag_data, frag_size))
        fragSpirv.assign(reinterpret_cast<const uint32_t*>(frag_data), reinterpret_cast<const uint32_t*>(frag_data) + frag_size / 4u);

    if (vertSpirv.empty() || fragSpirv.empty())
    {
        const std::string compiler = FindCompiler();
        if (compiler.empty())
        {
            error = "Vulkan shader: glslangValidator/glslc was not found. Set HRL_GLSLANG_VALIDATOR or VULKAN_SDK.";
            return false;
        }

        const std::string vsrc = BuildVulkanSource(vert_source, bindings, merged);
        const std::string fsrc = BuildVulkanSource(frag_source, bindings, merged);
        if (vertSpirv.empty() && !CompileStage(compiler, vsrc, "vert", vertSpirv, error))
            return false;
        if (fragSpirv.empty() && !CompileStage(compiler, fsrc, "frag", fragSpirv, error))
            return false;
    }

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = vertSpirv.size() * sizeof(uint32_t);
    ci.pCode = vertSpirv.data();
    VkResult result = vkCreateShaderModule(device, &ci, nullptr, &vertex);
    if (result != VK_SUCCESS)
    {
        error = "Vulkan shader: vkCreateShaderModule(vertex) failed";
        Destroy(device);
        return false;
    }

    ci.codeSize = fragSpirv.size() * sizeof(uint32_t);
    ci.pCode = fragSpirv.data();
    result = vkCreateShaderModule(device, &ci, nullptr, &fragment);
    if (result != VK_SUCCESS)
    {
        error = "Vulkan shader: vkCreateShaderModule(fragment) failed";
        Destroy(device);
        return false;
    }

    samplers = std::move(bindings);

    uniforms.clear();
    size_t offset = 0;
    for (const ParsedUniform& u : merged)
    {
        VulkanUniformField field;
        field.name = u.name;
        if (u.type == "float") field.type = VulkanUniformField::Type::Float;
        else if (u.type == "int") field.type = VulkanUniformField::Type::Int;
        else if (u.type == "uint") field.type = VulkanUniformField::Type::UInt;
        else if (u.type == "bool") field.type = VulkanUniformField::Type::Bool;
        else if (u.type == "vec2") field.type = VulkanUniformField::Type::Vec2;
        else if (u.type == "vec3") field.type = VulkanUniformField::Type::Vec3;
        else if (u.type == "vec4") field.type = VulkanUniformField::Type::Vec4;
        else if (u.type == "mat3") field.type = VulkanUniformField::Type::Mat3;
        else if (u.type == "mat4") field.type = VulkanUniformField::Type::Mat4;
        else continue;

        const size_t alignment = Std140BaseAlignment(u.type);
        offset = AlignUp(offset, alignment);
        field.offset = offset;
        field.array_count = std::max(1u, u.array_count);
        if (field.array_count > 1)
        {
            const size_t stride = AlignUp(Std140ElementSize(u.type), 16);
            field.type = (u.type == "mat4") ? VulkanUniformField::Type::Mat4Array : field.type;
            field.size = stride * field.array_count;
            offset += field.size;
        }
        else
        {
            field.size = Std140ElementSize(u.type);
            offset += field.size;
        }
        uniforms.push_back(std::move(field));
    }
    uniform_block_size = AlignUp(offset, 16);
    return true;
}

void VulkanShader::Destroy(VkDevice device)
{
    if (!device) return;
    if (vertex) vkDestroyShaderModule(device, vertex, nullptr);
    if (fragment) vkDestroyShaderModule(device, fragment, nullptr);
    vertex = VK_NULL_HANDLE;
    fragment = VK_NULL_HANDLE;
    uniforms.clear();
    samplers.clear();
    uniform_block_size = 0;
}
