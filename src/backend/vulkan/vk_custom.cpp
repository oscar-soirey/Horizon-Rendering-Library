// HRL native Vulkan backend - custom shaders (HRL_CreateShader).
//
// Two kinds of sources are accepted, told apart by their content:
//   * surface shaders: the fragment source defines
//         void Surface(HRL_SurfaceInput i, inout HRL_Surface s)
//     and the vertex source may define void Vertex(inout HRL_VertexData v)
//     (contract in src/shaders/vulkan/custom.glsl). They are compiled into the
//     built-in mesh shaders, so lighting, shadows, fog, ray tracing and picking
//     work as with the built-in material;
//   * post-process shaders: the OpenGL 3.30 fragment shaders written for
//     HRL_CreatePostProcess (in vec2 uv, uScene, uBrightScene, uScreenSize,
//     uTime and the material uniforms) are compiled as they are.
// `uniform` declarations of plain types and sampler2D become material
// parameters: they are removed from the source and read from a buffer filled
// from the HRL_Material (HRL_MaterialSet*), with their initializer as default.
#include "vk_renderer_internal.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

#ifdef HRL_VK_RUNTIME_SHADERS
#include "hrl_vk_glsl_sources.h"
#include <glslang/Public/ShaderLang.h>
#if __has_include(<glslang/SPIRV/GlslangToSpv.h>)
#include <glslang/SPIRV/GlslangToSpv.h>
#else
#include <SPIRV/GlslangToSpv.h>
#endif
#endif

namespace hrlvk
{
namespace
{
// ---------------------------------------------------------------------------
// Source analysis
// ---------------------------------------------------------------------------
// Replaces comments with spaces (newlines kept, so line numbers do not move).
std::string StripComments(const std::string& src)
{
	std::string out = src;
	for (size_t i = 0; i + 1 < out.size(); ++i)
	{
		if (out[i] == '/' && out[i + 1] == '/')
		{
			while (i < out.size() && out[i] != '\n') out[i++] = ' ';
		}
		else if (out[i] == '/' && out[i + 1] == '*')
		{
			out[i] = out[i + 1] = ' ';
			i += 2;
			while (i + 1 < out.size() && !(out[i] == '*' && out[i + 1] == '/'))
			{
				if (out[i] != '\n') out[i] = ' ';
				++i;
			}
			if (i + 1 < out.size()) out[i] = out[i + 1] = ' ';
		}
	}
	return out;
}

bool IsIdent(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Blanks [begin, end) of both strings, keeping newlines.
void Blank(std::string& a, std::string& b, size_t begin, size_t end)
{
	for (size_t i = begin; i < end && i < a.size(); ++i)
		if (a[i] != '\n') { a[i] = ' '; b[i] = ' '; }
}

std::vector<std::string> Tokens(const std::string& s)
{
	std::vector<std::string> out;
	std::string cur;
	for (char c : s)
	{
		if (IsIdent(c)) { cur += c; continue; }
		if (!cur.empty()) { out.push_back(cur); cur.clear(); }
		if (!std::isspace((unsigned char)c)) out.push_back(std::string(1, c));
	}
	if (!cur.empty()) out.push_back(cur);
	return out;
}

// Numbers of an initializer ("vec3(1.0, 0.5, 0.0)", "2.2", "true").
glm::vec4 ParseDefault(const std::string& text, uint32_t components)
{
	glm::vec4 v(0.f);
	std::vector<float> numbers;
	for (size_t i = 0; i < text.size();)
	{
		if (std::isdigit((unsigned char)text[i]) || ((text[i] == '-' || text[i] == '.' || text[i] == '+') &&
			i + 1 < text.size() && (std::isdigit((unsigned char)text[i + 1]) || text[i + 1] == '.')))
		{
			// Skip digits that belong to an identifier (vec3).
			if (i > 0 && IsIdent(text[i - 1])) { ++i; continue; }
			char* end = nullptr;
			numbers.push_back(std::strtof(text.c_str() + i, &end));
			i = (size_t)(end - text.c_str());
			while (i < text.size() && (text[i] == 'f' || text[i] == 'F' || text[i] == 'u' || text[i] == 'U')) ++i;
		}
		else if (text.compare(i, 4, "true") == 0 && (i == 0 || !IsIdent(text[i - 1]))) { numbers.push_back(1.f); i += 4; }
		else if (text.compare(i, 5, "false") == 0 && (i == 0 || !IsIdent(text[i - 1]))) { numbers.push_back(0.f); i += 5; }
		else ++i;
	}
	if (numbers.size() == 1)
		for (uint32_t c = 0; c < components; ++c) v[(int)c] = numbers[0];
	else
		for (uint32_t c = 0; c < components && c < numbers.size(); ++c) v[(int)c] = numbers[c];
	return v;
}

bool ParamType(const std::string& type, CustomParam& p)
{
	struct T { const char* name; CustomParam::Kind kind; uint32_t components; };
	static const T types[] = {
		{"float", CustomParam::Float, 1}, {"vec2", CustomParam::Float, 2}, {"vec3", CustomParam::Float, 3}, {"vec4", CustomParam::Float, 4},
		{"int", CustomParam::Int, 1}, {"ivec2", CustomParam::Int, 2}, {"ivec3", CustomParam::Int, 3}, {"ivec4", CustomParam::Int, 4},
		{"uint", CustomParam::Uint, 1}, {"bool", CustomParam::Bool, 1}, {"sampler2D", CustomParam::Sampler, 1},
		{"mat3", CustomParam::Mat3, 9}, {"mat4", CustomParam::Mat4, 16}};
	for (const T& t : types)
		if (type == t.name) { p.kind = t.kind; p.components = t.components; return true; }
	return false;
}

// Finds the loose `uniform` declarations of `src`, removes them from `code`
// (same layout as src) and adds them to `params` (names shared between stages).
// Value of `#define NAME number` in the source (array sizes).
uint32_t DefinedNumber(const std::string& src, const std::string& name)
{
	for (size_t pos = 0; (pos = src.find("#define", pos)) != std::string::npos; pos += 7)
	{
		std::vector<std::string> tokens = Tokens(src.substr(pos, src.find('\n', pos) - pos));
		if (tokens.size() >= 4 && tokens[2] == name) return (uint32_t)std::strtoul(tokens[3].c_str(), nullptr, 10);
	}
	return 0;
}

void ExtractUniforms(const std::string& src, std::string& code, std::vector<CustomParam>& params,
	const std::vector<std::string>& builtins, bool allowArrays = false)
{
	std::string clean = StripComments(src);
	code = clean;
	for (size_t pos = 0; (pos = clean.find("uniform", pos)) != std::string::npos;)
	{
		const bool startOk = pos == 0 || !IsIdent(clean[pos - 1]);
		const bool endOk = pos + 7 < clean.size() && !IsIdent(clean[pos + 7]);
		if (!startOk || !endOk) { pos += 7; continue; }
		const size_t semi = clean.find(';', pos);
		if (semi == std::string::npos) break;
		const std::string statement = clean.substr(pos, semi - pos);
		const bool array = statement.find('[') != std::string::npos;
		if (statement.find('{') != std::string::npos || (array && !allowArrays) ||
			statement.find("layout") != std::string::npos) { pos = semi; continue; } // blocks: left as they are
		std::vector<std::string> tokens = Tokens(statement);
		// uniform [precision] type name [[size]] [= value]
		size_t t = 1;
		if (t < tokens.size() && (tokens[t] == "lowp" || tokens[t] == "mediump" || tokens[t] == "highp")) ++t;
		if (t + 1 >= tokens.size()) { pos = semi; continue; }
		CustomParam p;
		p.name = tokens[t + 1];
		if (!ParamType(tokens[t], p)) { pos = semi; continue; }
		if (array)
		{
			if (t + 3 >= tokens.size() || tokens[t + 2] != "[") { pos = semi; continue; }
			const std::string& size = tokens[t + 3];
			p.arraySize = std::isdigit((unsigned char)size[0]) ? (uint32_t)std::strtoul(size.c_str(), nullptr, 10) : DefinedNumber(clean, size);
			if (p.arraySize == 0 || p.kind == CustomParam::Sampler) { pos = semi; continue; }
		}
		const uint32_t perElement = p.kind == CustomParam::Mat4 ? 4u : (p.kind == CustomParam::Mat3 ? 3u : 1u);
		p.slots = perElement * std::max(p.arraySize, 1u);
		const size_t eq = statement.find('=');
		if (eq != std::string::npos) p.value = ParseDefault(statement.substr(eq + 1), p.components);
		Blank(clean, code, pos, semi + 1);
		const bool builtin = std::find(builtins.begin(), builtins.end(), p.name) != builtins.end();
		auto known = std::find_if(params.begin(), params.end(), [&](const CustomParam& q) { return q.name == p.name; });
		if (!builtin && known == params.end()) params.push_back(p);
		pos = semi;
	}
	// #version lines go away (the generated header has its own).
	for (size_t pos = 0; (pos = code.find("#version", pos)) != std::string::npos;)
	{
		const size_t eol = code.find('\n', pos);
		for (size_t i = pos; i < (eol == std::string::npos ? code.size() : eol); ++i) code[i] = ' ';
	}
}

// `in` / `out` declarations of an OpenGL fragment shader get explicit locations.
std::string AddLocations(const std::string& code)
{
	std::string out;
	uint32_t inLocation = 0, outLocation = 0;
	size_t pos = 0;
	while (pos < code.size())
	{
		// Statement start: beginning or after ; { }
		size_t start = pos;
		while (start < code.size() && std::isspace((unsigned char)code[start])) ++start;
		const size_t semi = code.find_first_of(";{}", start);
		const size_t end = semi == std::string::npos ? code.size() : semi + 1;
		std::string statement = code.substr(pos, end - pos);
		const std::string head = code.substr(start, std::min<size_t>(4, code.size() - start));
		const bool isIn = head.size() >= 3 && head.compare(0, 3, "in ") == 0;
		const bool isOut = head.compare(0, 4, "out ") == 0;
		if ((isIn || isOut) && semi != std::string::npos && code[semi] == ';')
		{
			const std::string layout = "layout(location = " + std::to_string(isIn ? inLocation++ : outLocation++) + ") ";
			statement.insert(start - pos, layout);
		}
		out += statement;
		pos = end;
	}
	return out;
}

// Places the parameters in the table from slot `first`; returns the slots used.
uint32_t LayoutParams(std::vector<CustomParam>& params, uint32_t first)
{
	uint32_t next = first;
	for (CustomParam& p : params) { p.first = next; next += p.slots; }
	return next;
}

// Surface shaders: every parameter is a macro reading the table.
std::string ParamMacros(const std::vector<CustomParam>& params)
{
	std::ostringstream m;
	for (size_t i = 0; i < params.size(); ++i)
	{
		const CustomParam& p = params[i];
		const std::string v = "hrlParams[hrlParamBase + " + std::to_string(p.first) + "u]";
		if (p.kind == CustomParam::Mat4 || p.kind == CustomParam::Mat3)
		{
			const bool four = p.kind == CustomParam::Mat4;
			m << "#define " << p.name << " " << (four ? "mat4(" : "mat3(");
			for (uint32_t c = 0; c < (four ? 4u : 3u); ++c)
				m << (c ? ", " : "") << "hrlParams[hrlParamBase + " << (p.first + c) << "u]" << (four ? "" : ".xyz");
			m << ")\n";
			continue;
		}
		static const char* swizzles[] = {"", ".x", ".xy", ".xyz", ""};
		const char* sw = swizzles[p.components];
		m << "#define " << p.name << " ";
		switch (p.kind)
		{
		case CustomParam::Float: m << "(" << v << sw << ")"; break;
		case CustomParam::Int:
			m << (p.components == 1 ? "int(" : (p.components == 2 ? "ivec2(" : (p.components == 3 ? "ivec3(" : "ivec4("))) << v << sw << ")";
			break;
		case CustomParam::Uint: m << "uint(" << v << ".x)"; break;
		case CustomParam::Bool: m << "(" << v << ".x != 0.0)"; break;
		case CustomParam::Sampler: m << "uTextures[nonuniformEXT(floatBitsToUint(" << v << ".x) * 2u)]"; break;
		default: break;
		}
		m << "\n";
	}
	return m.str();
}

// OpenGL shaders: parameters are global variables loaded from the table at
// the start of main() (arrays and matrices included); samplers stay macros.
std::string ParamGlobals(const std::vector<CustomParam>& params)
{
	static const char* floatTypes[] = {"", "float", "vec2", "vec3", "vec4"};
	static const char* intTypes[] = {"", "int", "ivec2", "ivec3", "ivec4"};
	static const char* swizzles[] = {"", ".x", ".xy", ".xyz", ""};
	std::ostringstream decl, load;
	load << "void hrlLoadParameters()\n{\n";
	for (const CustomParam& p : params)
	{
		if (p.kind == CustomParam::Sampler)
		{
			decl << "#define " << p.name << " uTextures[nonuniformEXT(floatBitsToUint(hrlParams[" << p.first << "].x) * 2u)]\n";
			continue;
		}
		const char* type = "float";
		switch (p.kind)
		{
		case CustomParam::Float: type = floatTypes[p.components]; break;
		case CustomParam::Int: type = intTypes[p.components]; break;
		case CustomParam::Uint: type = "uint"; break;
		case CustomParam::Bool: type = "bool"; break;
		case CustomParam::Mat3: type = "mat3"; break;
		case CustomParam::Mat4: type = "mat4"; break;
		default: break;
		}
		decl << type << " " << p.name;
		if (p.arraySize) decl << "[" << p.arraySize << "]";
		decl << ";\n";
		const uint32_t count = std::max(p.arraySize, 1u);
		const uint32_t stride = p.slots / count;
		for (uint32_t e = 0; e < count; ++e)
		{
			const std::string target = p.name + (p.arraySize ? "[" + std::to_string(e) + "]" : "");
			const std::string v = "hrlParams[" + std::to_string(p.first + e * stride) + "]";
			load << "\t" << target << " = ";
			switch (p.kind)
			{
			case CustomParam::Float: load << v << swizzles[p.components]; break;
			case CustomParam::Int: load << type << "(" << v << swizzles[p.components] << ")"; break;
			case CustomParam::Uint: load << "uint(" << v << ".x)"; break;
			case CustomParam::Bool: load << v << ".x != 0.0"; break;
			case CustomParam::Mat3:
				load << "mat3(" << v << ".xyz, hrlParams[" << (p.first + e * stride + 1) << "].xyz, hrlParams[" << (p.first + e * stride + 2) << "].xyz)";
				break;
			case CustomParam::Mat4:
				load << "mat4(" << v << ", hrlParams[" << (p.first + e * stride + 1) << "], hrlParams[" << (p.first + e * stride + 2)
					<< "], hrlParams[" << (p.first + e * stride + 3) << "])";
				break;
			default: break;
			}
			load << ";\n";
		}
	}
	load << "}\n";
	return decl.str() + load.str();
}

#ifdef HRL_VK_RUNTIME_SHADERS
bool Compile(EShLanguage stage, const std::string& source, const std::vector<const char*>& defines,
	const std::string& user, std::vector<uint32_t>& spirv, std::string& log);
#endif

// Builds and compiles an OpenGL 3.30 post-process / effect fragment shader.
bool BuildPostShader(const std::string& frag, CustomShader& shader, std::string& log)
{
#ifndef HRL_VK_RUNTIME_SHADERS
	(void)frag; (void)shader;
	log = "no run time compiler";
	return false;
#else
	std::string code;
	ExtractUniforms(frag, code, shader.params, {"uScene", "uBrightScene", "uScreenSize", "uTime", "uDepth", "uNormal"}, true);
	// Slot 0: screen size and time.
	shader.paramSlots = LayoutParams(shader.params, 1);
	std::ostringstream src;
	src << "#version 460\n#extension GL_EXT_nonuniform_qualifier : require\n"
		<< "layout(set = 0, binding = 0) uniform sampler2D uScene;\n"
		<< "layout(set = 0, binding = 1) uniform sampler2D uBrightScene;\n"
		<< "layout(set = 0, binding = 2) uniform sampler2D uDepth;\n"
		<< "layout(set = 0, binding = 3, std430) readonly buffer HRL_PostParameters { vec4 hrlParams[]; };\n"
		<< "layout(set = 0, binding = 4) uniform sampler2D uNormal;\n"
		<< "layout(set = 1, binding = 0) uniform sampler2D uTextures[];\n"
		<< "#define uScreenSize (hrlParams[0].xy)\n#define uTime (hrlParams[0].z)\n"
		<< ParamGlobals(shader.params)
		<< "#define main hrlUserMain\n#line 1\n" << AddLocations(code) << "\n#undef main\n"
		<< "void main()\n{\n\thrlLoadParameters();\n\thrlUserMain();\n}\n";
	const std::string empty;
	return Compile(EShLangFragment, src.str(), {}, empty, shader.fragment, log);
#endif
}

#ifdef HRL_VK_RUNTIME_SHADERS
// ---------------------------------------------------------------------------
// glslang
// ---------------------------------------------------------------------------
TBuiltInResource DefaultResources()
{
	TBuiltInResource r{};
	r.maxLights = 32; r.maxClipPlanes = 6; r.maxTextureUnits = 32; r.maxTextureCoords = 32;
	r.maxVertexAttribs = 64; r.maxVertexUniformComponents = 4096; r.maxVaryingFloats = 64;
	r.maxVertexTextureImageUnits = 32; r.maxCombinedTextureImageUnits = 80; r.maxTextureImageUnits = 32;
	r.maxFragmentUniformComponents = 4096; r.maxDrawBuffers = 32; r.maxVertexUniformVectors = 128;
	r.maxVaryingVectors = 8; r.maxFragmentUniformVectors = 16; r.maxVertexOutputVectors = 16;
	r.maxFragmentInputVectors = 15; r.minProgramTexelOffset = -8; r.maxProgramTexelOffset = 7;
	r.maxClipDistances = 8; r.maxComputeWorkGroupCountX = r.maxComputeWorkGroupCountY = r.maxComputeWorkGroupCountZ = 65535;
	r.maxComputeWorkGroupSizeX = 1024; r.maxComputeWorkGroupSizeY = 1024; r.maxComputeWorkGroupSizeZ = 64;
	r.maxComputeUniformComponents = 1024; r.maxComputeTextureImageUnits = 16; r.maxComputeImageUniforms = 8;
	r.maxComputeAtomicCounters = 8; r.maxComputeAtomicCounterBuffers = 1; r.maxVaryingComponents = 60;
	r.maxVertexOutputComponents = 64; r.maxGeometryInputComponents = 64; r.maxGeometryOutputComponents = 128;
	r.maxFragmentInputComponents = 128; r.maxImageUnits = 8; r.maxCombinedImageUnitsAndFragmentOutputs = 8;
	r.maxCombinedShaderOutputResources = 8; r.maxImageSamples = 0; r.maxVertexImageUniforms = 0;
	r.maxFragmentImageUniforms = 8; r.maxCombinedImageUniforms = 8; r.maxViewports = 16;
	r.maxFragmentAtomicCounters = 8; r.maxCombinedAtomicCounters = 8; r.maxAtomicCounterBindings = 1;
	r.maxFragmentAtomicCounterBuffers = 1; r.maxCombinedAtomicCounterBuffers = 1; r.maxAtomicCounterBufferSize = 16384;
	r.maxTransformFeedbackBuffers = 4; r.maxTransformFeedbackInterleavedComponents = 64; r.maxCullDistances = 8;
	r.maxCombinedClipAndCullDistances = 8; r.maxSamples = 4;
	r.limits.nonInductiveForLoops = r.limits.whileLoops = r.limits.doWhileLoops = true;
	r.limits.generalUniformIndexing = r.limits.generalAttributeMatrixVectorIndexing = true;
	r.limits.generalVaryingIndexing = r.limits.generalSamplerIndexing = true;
	r.limits.generalVariableIndexing = r.limits.generalConstantMatrixVectorIndexing = true;
	return r;
}

// #include of the HRL shader sources (embedded) and of the generated user code.
class Includer : public glslang::TShader::Includer
{
public:
	explicit Includer(const std::string& user) : user_(user) {}
	IncludeResult* includeLocal(const char* name, const char*, size_t) override { return Find(name); }
	IncludeResult* includeSystem(const char* name, const char*, size_t) override { return Find(name); }
	void releaseInclude(IncludeResult* result) override { delete result; }

private:
	IncludeResult* Find(const char* name)
	{
		const std::string n = name;
		if (n == "hrl_user.glsl") return new IncludeResult(n, user_.data(), user_.size(), nullptr);
		for (size_t i = 0; i < hrl_vk_glsl_source_count; ++i)
			if (n == hrl_vk_glsl_sources[i].name)
				return new IncludeResult(n, hrl_vk_glsl_sources[i].text, hrl_vk_glsl_sources[i].size, nullptr);
		return nullptr;
	}
	const std::string& user_;
};

const char* EmbeddedSource(const char* name)
{
	for (size_t i = 0; i < hrl_vk_glsl_source_count; ++i)
		if (std::string(name) == hrl_vk_glsl_sources[i].name) return hrl_vk_glsl_sources[i].text;
	return "";
}

bool Compile(EShLanguage stage, const std::string& source, const std::vector<const char*>& defines,
	const std::string& user, std::vector<uint32_t>& spirv, std::string& log)
{
	static const bool initialized = glslang::InitializeProcess();
	(void)initialized;
	glslang::TShader shader(stage);
	const char* text = source.c_str();
	const char* name = "hrl";
	shader.setStringsWithLengthsAndNames(&text, nullptr, &name, 1);
	std::string preamble;
	for (const char* d : defines) preamble += std::string("#define ") + d + "\n";
	shader.setPreamble(preamble.c_str());
	shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
	const TBuiltInResource resources = DefaultResources();
	const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
	Includer includer(user);
	if (!shader.parse(&resources, 460, false, messages, includer))
	{
		log = shader.getInfoLog();
		return false;
	}
	glslang::TProgram program;
	program.addShader(&shader);
	if (!program.link(messages))
	{
		log = program.getInfoLog();
		return false;
	}
	spirv.clear();
	glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
	return !spirv.empty();
}
#endif

HRL_VkShaderBlob Blob(const std::vector<uint32_t>& code) { return {code.data(), code.size() * sizeof(uint32_t)}; }

bool ContainsFunction(const std::string& src, const char* name)
{
	const std::string clean = StripComments(src);
	for (size_t pos = 0; (pos = clean.find(name, pos)) != std::string::npos; pos += 1)
	{
		if (pos > 0 && IsIdent(clean[pos - 1])) continue;
		size_t after = pos + std::strlen(name);
		if (after < clean.size() && IsIdent(clean[after])) continue;
		while (after < clean.size() && std::isspace((unsigned char)clean[after])) ++after;
		if (after < clean.size() && clean[after] == '(') return true;
	}
	return false;
}

void ReportShaderError(HRL_id id, const std::string& what, const std::string& log)
{
	SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
		"Vulkan custom shader " + std::to_string((unsigned long long)id) + ": " + what + (log.empty() ? "" : "\n" + log));
}

void DestroyPipeline(VkPipeline& p)
{
	if (!p) return;
	auto it = std::find(R.ownedPipelines.begin(), R.ownedPipelines.end(), p);
	if (it != R.ownedPipelines.end()) R.ownedPipelines.erase(it);
	VkDevice d = Dev().device;
	VkPipeline handle = p;
	Dev().Defer([d, handle]() { vkDestroyPipeline(d, handle, nullptr); });
	p = VK_NULL_HANDLE;
}
} // namespace

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
bool RuntimeShadersAvailable()
{
#ifdef HRL_VK_RUNTIME_SHADERS
	return true;
#else
	return false;
#endif
}

bool CreateCustomShader(HRL_id id, const std::string& vert, const std::string& frag)
{
#ifndef HRL_VK_RUNTIME_SHADERS
	(void)id; (void)vert; (void)frag;
	SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
		"Vulkan: custom shaders need HRL built with glslang (Vulkan SDK), see HRL_VULKAN_RUNTIME_SHADERS");
	return false;
#else
	CustomShader shader;
	shader.surface = ContainsFunction(frag, "Surface");
	shader.hasVertex = shader.surface && ContainsFunction(vert, "Vertex");
	std::string log;
	if (shader.surface)
	{
		// Parameters of both stages share one table.
		std::string fragCode, vertCode;
		ExtractUniforms(frag, fragCode, shader.params, {});
		if (shader.hasVertex) ExtractUniforms(vert, vertCode, shader.params, {});
		shader.paramSlots = LayoutParams(shader.params, 0);
		const std::string macros = ParamMacros(shader.params);
		shader.fragmentUser = macros + "#line 1\n" + fragCode + "\n";
		shader.vertexUser = macros + "#line 1\n" + vertCode + "\n";
		// Checked now: the errors reach the application where it created the shader.
		if (!Compile(EShLangFragment, EmbeddedSource("mesh.frag"), {"HRL_CUSTOM_SURFACE"}, shader.fragmentUser, shader.fragment, log))
		{
			ReportShaderError(id, "the surface function does not compile", log);
			return false;
		}
		if (shader.hasVertex &&
			!Compile(EShLangVertex, EmbeddedSource("mesh.vert"), {"HRL_CUSTOM_VERTEX"}, shader.vertexUser, shader.vertex, log))
		{
			ReportShaderError(id, "the vertex function does not compile", log);
			return false;
		}
	}
	else if (!BuildPostShader(frag, shader, log))
	{
		ReportShaderError(id, "the post-process shader does not compile", log);
		return false;
	}
	DeleteCustomShader(id);
	R.customShaders[id] = std::move(shader);
	return true;
#endif
}

void DeleteCustomShader(HRL_id id)
{
	auto it = R.customShaders.find(id);
	if (it == R.customShaders.end()) return;
	CustomShader& s = it->second;
	for (VkPipeline& p : s.forward) DestroyPipeline(p);
	DestroyPipeline(s.shadow);
	DestroyPipeline(s.shadowMasked);
	DestroyPipeline(s.post);
	R.customShaders.erase(it);
}

bool IsCustomShader(HRL_id id) { return R.customShaders.count(id) != 0; }

const CustomShader* FindCustomShader(HRL_id id)
{
	auto it = R.customShaders.find(id);
	return it == R.customShaders.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Pipelines (created on first use)
// ---------------------------------------------------------------------------
VkPipeline CustomForwardPipeline(HRL_id id, uint32_t bucket)
{
	auto it = R.customShaders.find(id);
	if (it == R.customShaders.end() || !it->second.surface || bucket >= BucketSprite) return VK_NULL_HANDLE;
	CustomShader& s = it->second;
	if (s.forward[bucket] || s.forwardTried[bucket]) return s.forward[bucket];
	s.forwardTried[bucket] = true;
#ifdef HRL_VK_RUNTIME_SHADERS
	std::vector<const char*> defines = {"HRL_CUSTOM_SURFACE"};
	if (IsMasked(bucket)) defines.push_back("HRL_ALPHA_TEST");
	if (Dev().caps.rayQuery) defines.push_back("HRL_RAY_QUERY");
	std::vector<uint32_t> fs;
	std::string log;
	if (!Compile(EShLangFragment, EmbeddedSource("mesh.frag"), defines, s.fragmentUser, fs, log))
	{
		ReportShaderError(id, "the surface function does not compile", log);
		return VK_NULL_HANDLE;
	}
	const HRL_VkShaderBlob fsBlob = Blob(fs), vsBlob = Blob(s.vertex);
	s.forward[bucket] = MakeMeshPipeline(s.hasVertex ? &vsBlob : nullptr, &fsBlob, bucket);
#endif
	return s.forward[bucket];
}

VkPipeline CustomShadowPipeline(HRL_id id, bool masked)
{
	auto it = R.customShaders.find(id);
	if (it == R.customShaders.end() || !it->second.surface) return VK_NULL_HANDLE;
	CustomShader& s = it->second;
	// Without a vertex function an opaque caster is an ordinary one.
	if (!masked && !s.hasVertex) return R.shadowPipeline;
	VkPipeline& p = masked ? s.shadowMasked : s.shadow;
	bool& tried = masked ? s.shadowMaskedTried : s.shadowTried;
	if (p || tried) return p ? p : (masked ? R.shadowMaskedPipeline : R.shadowPipeline);
	tried = true;
#ifdef HRL_VK_RUNTIME_SHADERS
	std::vector<const char*> defines;
	if (s.hasVertex) defines.push_back("HRL_CUSTOM_VERTEX");
	if (masked) defines.push_back("HRL_CUSTOM_SURFACE");
	std::vector<uint32_t> vs, fs;
	std::string log;
	// Both stages see the parameters of the two user sources (same table).
	const std::string& vsUser = s.hasVertex ? s.vertexUser : s.fragmentUser;
	if (!Compile(EShLangVertex, EmbeddedSource("shadow.vert"), defines, s.hasVertex ? vsUser : std::string(), vs, log) ||
		(masked && !Compile(EShLangFragment, EmbeddedSource("shadow.frag"), {"HRL_CUSTOM_SURFACE"}, s.fragmentUser, fs, log)))
	{
		ReportShaderError(id, "the shadow variant does not compile", log);
		return masked ? R.shadowMaskedPipeline : R.shadowPipeline;
	}
	const HRL_VkShaderBlob vsBlob = Blob(vs), fsBlob = Blob(fs);
	p = MakeCustomShadowPipeline(&vsBlob, masked ? &fsBlob : nullptr);
#endif
	return p ? p : (masked ? R.shadowMaskedPipeline : R.shadowPipeline);
}

VkPipeline CustomPostPipeline(HRL_id id)
{
	auto it = R.customShaders.find(id);
	if (it == R.customShaders.end() || it->second.surface) return VK_NULL_HANDLE;
	CustomShader& s = it->second;
	if (!s.post && !s.forwardTried[0])
	{
		s.forwardTried[0] = true;
		const HRL_VkShaderBlob fsBlob = Blob(s.fragment);
		s.post = MakePostPipeline(&fsBlob);
	}
	return s.post;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
void WriteCustomParams(const CustomShader& shader, const HRL_Material* m, glm::vec4* out)
{
	for (const CustomParam& p : shader.params)
	{
		for (uint32_t i = 0; i < p.slots; ++i) out[p.first + i] = i == 0 ? p.value : glm::vec4(0.f);
		glm::vec4 v = p.value;
		if (m)
			switch (p.kind)
			{
			case CustomParam::Float:
				if (p.components == 1) { if (auto f = m->floatParams_.find(p.name); f != m->floatParams_.end()) v.x = f->second; }
				else if (p.components == 2) { if (auto f = m->vec2Params_.find(p.name); f != m->vec2Params_.end()) v = glm::vec4(f->second, 0.f, 0.f); }
				else if (p.components == 3) { if (auto f = m->vec3Params_.find(p.name); f != m->vec3Params_.end()) v = glm::vec4(f->second, 0.f); }
				else if (auto f = m->vec4Params_.find(p.name); f != m->vec4Params_.end()) v = f->second;
				break;
			case CustomParam::Int:
			case CustomParam::Uint:
			case CustomParam::Bool:
				if (auto f = m->intParams_.find(p.name); f != m->intParams_.end()) v.x = (float)f->second;
				else if (auto g = m->floatParams_.find(p.name); g != m->floatParams_.end()) v.x = g->second;
				break;
			default:
				break; // samplers below; matrices and arrays keep their defaults
			}
		if (p.kind == CustomParam::Sampler)
		{
			const uint32_t slot = m ? TextureSlot(m, p.name.c_str(), R.white.slot) : R.white.slot;
			std::memcpy(&v.x, &slot, sizeof(slot));
		}
		if (!p.arraySize && p.kind != CustomParam::Mat3 && p.kind != CustomParam::Mat4) out[p.first] = v;
	}
}

void WriteEffectParams(const CustomShader& shader, const EffectParams& values, glm::vec4* out)
{
	for (const CustomParam& p : shader.params)
	{
		for (uint32_t i = 0; i < p.slots; ++i) out[p.first + i] = i == 0 ? p.value : glm::vec4(0.f);
		auto it = values.find(p.name);
		if (it == values.end()) continue;
		const std::vector<float>& f = it->second;
		const uint32_t count = std::max(p.arraySize, 1u);
		const uint32_t stride = p.slots / count;
		if (p.kind == CustomParam::Mat3 || p.kind == CustomParam::Mat4)
		{
			const uint32_t n = p.kind == CustomParam::Mat4 ? 4u : 3u;
			for (uint32_t e = 0; e < count; ++e)
				for (uint32_t c = 0; c < n; ++c)
					for (uint32_t r = 0; r < n; ++r)
					{
						const size_t k = (size_t)e * n * n + c * n + r;
						if (k < f.size()) out[p.first + e * stride + c][(int)r] = f[k];
					}
			continue;
		}
		for (uint32_t e = 0; e < count; ++e)
			for (uint32_t c = 0; c < p.components && c < 4; ++c)
			{
				const size_t k = (size_t)e * p.components + c;
				if (k < f.size()) out[p.first + e * stride][(int)c] = f[k];
			}
	}
}

const CustomShader* EffectShader(const char* name, const char* source)
{
	static std::unordered_map<std::string, bool> failed;
	const std::string key = std::string("effect:") + name;
	// Effect shaders live with the custom ones, under ids no application id reaches.
	const HRL_id id = (HRL_id)(0xFFF00000u + (std::hash<std::string>{}(key) & 0xFFFFu));
	if (const CustomShader* s = FindCustomShader(id)) return s;
	if (failed[key] || !RuntimeShadersAvailable()) return nullptr;
	CustomShader shader;
	std::string log;
	if (!BuildPostShader(source, shader, log))
	{
		failed[key] = true;
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, std::string("Vulkan: built-in effect ") + name + " does not compile\n" + log);
		return nullptr;
	}
	R.customShaders[id] = std::move(shader);
	return FindCustomShader(id);
}
} // namespace hrlvk
