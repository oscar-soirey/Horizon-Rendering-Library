// HRL Vulkan backend - GLSL to SPIR-V compilation.
//
// The Vulkan SDK compiler (glslangValidator, or glslc) is run as an external
// process. Its location is taken from HRL_GLSLANG_VALIDATOR, then
// $VULKAN_SDK/Bin, then the PATH. Compiled SPIR-V is cached on disk
// (HRL_VULKAN_SHADER_CACHE, default: <temp>/hrl_vulkan_shader_cache), so
// the compiler only runs the first time a given shader is seen.
#include "vk_compile.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#if defined(_WIN32)
#  include <windows.h>
#endif

namespace
{

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

static std::mutex g_compilerMutex;
static std::string g_compiler;
static bool g_compilerSearched = false;
static std::atomic<uint64_t> g_tempCounter{0};

static std::string Compiler()
{
    std::lock_guard<std::mutex> lock(g_compilerMutex);
    if (!g_compilerSearched)
    {
        g_compiler = FindCompiler();
        g_compilerSearched = true;
    }
    return g_compiler;
}

static uint64_t Fnv1a(const std::string& s, uint64_t h = 1469598103934665603ull)
{
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

static std::filesystem::path CacheDirectory()
{
    std::error_code ec;
    std::filesystem::path dir;
    if (const char* env = std::getenv("HRL_VULKAN_SHADER_CACHE"))
    {
        if (!*env || std::strcmp(env, "0") == 0)
            return {};
        dir = env;
    }
    else
    {
        dir = std::filesystem::temp_directory_path(ec);
        if (ec) return {};
        dir /= "hrl_vulkan_shader_cache";
    }
    std::filesystem::create_directories(dir, ec);
    return ec ? std::filesystem::path() : dir;
}

static bool RunStage(const std::string& compiler, const std::string& source, const char* stage,
    std::vector<uint32_t>& spirv, std::string& error)
{
    std::error_code tempEc;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(tempEc);
    if (tempEc || dir.empty())
    {
        error = "Vulkan shader compiler: unable to determine the system temporary directory";
        return false;
    }
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << "hrl_vulkan_" << stamp << "_" << std::hash<std::thread::id>{}(std::this_thread::get_id()) << "_" << g_tempCounter++;
    const std::filesystem::path base = dir / name.str();
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
        command = quotedCompiler + " --target-env=vulkan1.1 -fshader-stage=" + stage + " -o " + Quote(out_path.string()) + " " + Quote(in_path.string());
    else
        command = quotedCompiler + " -V --target-env vulkan1.1 -S " + stage + " -o " + Quote(out_path.string()) + " " + Quote(in_path.string());

    std::string compilerOutput;
#if defined(_WIN32)
    // CreateProcess instead of cmd.exe : no command-line parsing issue with
    // spaces or parentheses in SDK installation paths.
    const std::string arguments = command.substr(quotedCompiler.size() + 1);
    RunCompilerWindows(compiler, arguments, compilerOutput);
#else
    compilerOutput = ReadCommandOutput(command);
#endif
    const bool ok = std::filesystem::exists(out_path) && ReadBinary(out_path, spirv);
    std::error_code ec;
    std::filesystem::remove(in_path, ec);
    std::filesystem::remove(out_path, ec);
    if (!ok)
    {
        error = std::string("Vulkan shader compilation failed (") + stage + ")";
        if (!compilerOutput.empty()) error += ": " + compilerOutput;
        else error += ". Command: " + command;
        return false;
    }
    return true;
}
} // namespace

namespace hrlvk
{
bool CompileGLSLToSpirv(const std::string& source, bool vertexStage, std::vector<uint32_t>& spirv, std::string& error)
{
    const char* stage = vertexStage ? "vert" : "frag";
    const uint64_t key = Fnv1a(source, Fnv1a(std::string("hrl-spirv-v1-") + stage));
    const std::filesystem::path cacheDir = CacheDirectory();
    std::filesystem::path cacheFile;
    if (!cacheDir.empty())
    {
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%016llx.spv", static_cast<unsigned long long>(key));
        cacheFile = cacheDir / hex;
        if (ReadBinary(cacheFile, spirv) && !spirv.empty() && spirv[0] == 0x07230203u)
            return true;
        spirv.clear();
    }

    const std::string compiler = Compiler();
    if (compiler.empty())
    {
        error = "Vulkan shader: glslangValidator/glslc was not found. Install the Vulkan SDK or set HRL_GLSLANG_VALIDATOR.";
        return false;
    }
    if (!RunStage(compiler, source, stage, spirv, error))
        return false;
    if (!cacheFile.empty())
    {
        // Write to a temporary name then rename : concurrent processes never read a partial file.
        std::filesystem::path tmp = cacheFile;
        tmp += "." + std::to_string(g_tempCounter++) + ".tmp";
        if (WriteBytes(tmp, spirv.data(), spirv.size() * sizeof(uint32_t)))
        {
            std::error_code ec;
            std::filesystem::rename(tmp, cacheFile, ec);
            if (ec) std::filesystem::remove(tmp, ec);
        }
    }
    return true;
}
} // namespace hrlvk
