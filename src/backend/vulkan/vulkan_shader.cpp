#include "vulkan_shader.h"
#include "vk_compile.h"
#include "vk_core.h"

#include <algorithm>
#include <functional>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace hrlvk
{
namespace
{
// ===========================================================================
// Lexical helpers
// ===========================================================================

// Replaces comments by spaces (newlines are kept, so line numbers in compiler
// messages still match the original source).
std::string StripComments(const std::string& src)
{
	std::string out = src;
	size_t i = 0;
	while (i < out.size())
	{
		if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '/')
		{
			while (i < out.size() && out[i] != '\n') out[i++] = ' ';
		}
		else if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '*')
		{
			out[i] = out[i + 1] = ' ';
			i += 2;
			while (i < out.size() && !(out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/'))
			{
				if (out[i] != '\n') out[i] = ' ';
				++i;
			}
			if (i < out.size()) { out[i] = ' '; if (i + 1 < out.size()) out[i + 1] = ' '; i += 2; }
		}
		else ++i;
	}
	return out;
}

bool IsIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool IsIdent(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

std::vector<std::string> Tokenize(const std::string& s)
{
	std::vector<std::string> tokens;
	size_t i = 0;
	while (i < s.size())
	{
		const char c = s[i];
		if (std::isspace((unsigned char)c)) { ++i; continue; }
		if (IsIdentStart(c))
		{
			size_t j = i + 1;
			while (j < s.size() && IsIdent(s[j])) ++j;
			tokens.push_back(s.substr(i, j - i));
			i = j;
		}
		else if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])))
		{
			size_t j = i + 1;
			while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '.' ||
				((s[j] == '-' || s[j] == '+') && (s[j - 1] == 'e' || s[j - 1] == 'E')))) ++j;
			tokens.push_back(s.substr(i, j - i));
			i = j;
		}
		else
		{
			tokens.push_back(std::string(1, c));
			++i;
		}
	}
	return tokens;
}

std::string ReplaceWord(const std::string& src, const std::string& word, const std::string& by)
{
	std::string out;
	out.reserve(src.size());
	size_t i = 0;
	while (i < src.size())
	{
		if (src.compare(i, word.size(), word) == 0 &&
			(i == 0 || !IsIdent(src[i - 1])) &&
			(i + word.size() >= src.size() || !IsIdent(src[i + word.size()])))
		{
			out += by;
			i += word.size();
		}
		else out += src[i++];
	}
	return out;
}

// ===========================================================================
// Top level statements
// ===========================================================================
struct Statement
{
	enum class Kind { Directive, Statement, Function } kind = Kind::Statement;
	std::string text;
};

std::vector<Statement> SplitTopLevel(const std::string& src)
{
	std::vector<Statement> out;
	std::string cur;
	int depth = 0;
	bool lineStart = true;
	size_t i = 0;
	auto flush = [&](Statement::Kind kind) {
		bool blank = true;
		for (char c : cur) if (!std::isspace((unsigned char)c)) { blank = false; break; }
		if (!blank) out.push_back({kind, cur});
		else if (!cur.empty()) out.push_back({Statement::Kind::Directive, cur}); // keep whitespace
		cur.clear();
	};
	while (i < src.size())
	{
		const char c = src[i];
		if (depth == 0 && lineStart && c == '#')
		{
			// Preprocessor directive at top level (with line continuations).
			flush(Statement::Kind::Statement);
			size_t j = i;
			while (j < src.size())
			{
				if (src[j] == '\n' && (j == 0 || src[j - 1] != '\\')) break;
				++j;
			}
			out.push_back({Statement::Kind::Directive, src.substr(i, j - i) + "\n"});
			i = (j < src.size()) ? j + 1 : j;
			lineStart = true;
			continue;
		}
		if (!std::isspace((unsigned char)c)) lineStart = false;
		if (c == '\n') lineStart = true;
		cur += c;
		if (c == '{' || c == '(' || c == '[') ++depth;
		else if (c == ')' || c == ']') --depth;
		else if (c == '}')
		{
			--depth;
			if (depth == 0)
			{
				// Function bodies end at '}', blocks/structs at the following ';'.
				const size_t brace = cur.find('{');
				const std::string head = cur.substr(0, brace);
				const std::vector<std::string> t = Tokenize(head);
				bool declaration = false;
				// Only the qualifiers before the parameter list count : a function
				// may have "in"/"out" parameters.
				for (size_t k = 0; k < t.size(); ++k)
				{
					if (t[k] == "layout")
					{
						// skip the layout qualifier arguments
						while (k < t.size() && t[k] != ")") ++k;
						continue;
					}
					if (t[k] == "(") break;
					if (t[k] == "struct" || t[k] == "uniform" || t[k] == "in" || t[k] == "out" || t[k] == "buffer") declaration = true;
				}
				if (!declaration && head.find('(') != std::string::npos && head.find('=') == std::string::npos)
				{
					++i;
					flush(Statement::Kind::Function);
					continue;
				}
			}
		}
		else if (c == ';' && depth == 0)
		{
			++i;
			flush(Statement::Kind::Statement);
			continue;
		}
		++i;
	}
	flush(Statement::Kind::Statement);
	return out;
}

// ===========================================================================
// Declarations
// ===========================================================================
const std::set<std::string> kStorageQualifiers = {
	"uniform", "in", "out", "inout", "flat", "smooth", "noperspective", "centroid", "invariant",
	"const", "highp", "mediump", "lowp", "attribute", "varying", "precise", "sample", "patch"};

bool IsSamplerTypeName(const std::string& t)
{
	return t.find("sampler") != std::string::npos;
}

SamplerDim SamplerDimFromType(const std::string& t)
{
	if (t == "sampler2D" || t == "sampler2DShadow" || t == "isampler2D" || t == "usampler2D") return SamplerDim::Tex2D;
	if (t == "samplerCube" || t == "samplerCubeShadow" || t == "isamplerCube" || t == "usamplerCube") return SamplerDim::Cube;
	if (t == "sampler2DArray" || t == "sampler2DArrayShadow" || t == "isampler2DArray" || t == "usampler2DArray") return SamplerDim::Array2D;
	if (t == "sampler3D" || t == "isampler3D" || t == "usampler3D") return SamplerDim::Tex3D;
	if (t == "sampler2DMS" || t == "isampler2DMS" || t == "usampler2DMS") return SamplerDim::Tex2DMS;
	return SamplerDim::Unsupported;
}

// Number of interface locations used by a varying/attribute type.
uint32_t LocationSlots(const std::string& type, uint32_t arraySize)
{
	uint32_t slots = 1;
	if (type.size() >= 4 && type.compare(0, 3, "mat") == 0)
	{
		slots = (uint32_t)(type[3] - '0');
		if (slots < 2 || slots > 4) slots = 4;
	}
	if (type.size() >= 5 && type.compare(0, 4, "dmat") == 0) slots = 8;
	if (type.compare(0, 4, "dvec") == 0 && type.size() == 5 && type[4] > '2') slots = 2;
	return slots * std::max(1u, arraySize);
}

struct Declarator
{
	std::string name;
	std::string arrayExpr; // text between [], may be empty
	std::string init;      // initializer text (after '=')
};

struct Declaration
{
	bool valid = false;
	std::vector<std::string> layout;      // layout(...) contents
	std::set<std::string> qualifiers;
	std::vector<std::string> qualifierOrder;
	std::string type;
	bool isBlock = false;                 // "uniform Name { ... } inst;"
	std::string blockName;
	std::string blockBody;                // text between braces
	std::string blockInstance;            // instance declarator text
	std::vector<Declarator> declarators;
	std::string typeArray;                // "float[4] a;" style
};

Declaration ParseDeclaration(const std::string& text)
{
	Declaration d;
	size_t i = 0;
	auto skipWs = [&]() { while (i < text.size() && std::isspace((unsigned char)text[i])) ++i; };
	auto readIdent = [&]() -> std::string {
		skipWs();
		size_t j = i;
		if (j < text.size() && IsIdentStart(text[j]))
		{
			++j;
			while (j < text.size() && IsIdent(text[j])) ++j;
		}
		std::string r = text.substr(i, j - i);
		i = j;
		return r;
	};
	while (true)
	{
		skipWs();
		const size_t save = i;
		std::string w = readIdent();
		if (w.empty()) return d;
		if (w == "layout")
		{
			skipWs();
			if (i >= text.size() || text[i] != '(') return d;
			int depth = 0;
			size_t start = i + 1;
			for (; i < text.size(); ++i)
			{
				if (text[i] == '(') ++depth;
				else if (text[i] == ')') { if (--depth == 0) break; }
			}
			std::string inner = text.substr(start, i - start);
			++i;
			std::stringstream ss(inner);
			std::string item;
			while (std::getline(ss, item, ','))
			{
				item.erase(0, item.find_first_not_of(" \t\r\n"));
				item.erase(item.find_last_not_of(" \t\r\n") + 1);
				if (!item.empty()) d.layout.push_back(item);
			}
			continue;
		}
		if (kStorageQualifiers.count(w))
		{
			d.qualifiers.insert(w);
			d.qualifierOrder.push_back(w);
			continue;
		}
		// type or block name
		skipWs();
		if (i < text.size() && text[i] == '{')
		{
			d.isBlock = true;
			d.blockName = w;
			int depth = 0;
			size_t start = i + 1;
			for (; i < text.size(); ++i)
			{
				if (text[i] == '{') ++depth;
				else if (text[i] == '}') { if (--depth == 0) break; }
			}
			d.blockBody = text.substr(start, i - start);
			++i;
			std::string rest = text.substr(i);
			const size_t semi = rest.rfind(';');
			if (semi != std::string::npos) rest = rest.substr(0, semi);
			d.blockInstance = rest;
			d.valid = true;
			return d;
		}
		(void)save;
		d.type = w;
		break;
	}
	skipWs();
	// "float[4] name" form
	if (i < text.size() && text[i] == '[')
	{
		size_t j = text.find(']', i);
		if (j == std::string::npos) return d;
		d.typeArray = text.substr(i + 1, j - i - 1);
		i = j + 1;
	}
	// declarators until ';'
	std::string rest = text.substr(i);
	const size_t semi = rest.rfind(';');
	if (semi != std::string::npos) rest = rest.substr(0, semi);
	// split by commas at depth 0
	std::vector<std::string> parts;
	{
		int depth = 0;
		std::string cur;
		for (char c : rest)
		{
			if (c == '(' || c == '[' || c == '{') ++depth;
			else if (c == ')' || c == ']' || c == '}') --depth;
			if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
			cur += c;
		}
		parts.push_back(cur);
	}
	for (std::string p : parts)
	{
		Declarator decl;
		const size_t eq = p.find('=');
		if (eq != std::string::npos)
		{
			decl.init = p.substr(eq + 1);
			p = p.substr(0, eq);
		}
		size_t k = 0;
		while (k < p.size() && std::isspace((unsigned char)p[k])) ++k;
		size_t n = k;
		while (n < p.size() && IsIdent(p[n])) ++n;
		decl.name = p.substr(k, n - k);
		const size_t lb = p.find('[', n);
		if (lb != std::string::npos)
		{
			const size_t rb = p.find(']', lb);
			decl.arrayExpr = p.substr(lb + 1, rb == std::string::npos ? std::string::npos : rb - lb - 1);
		}
		if (decl.name.empty()) return d;
		d.declarators.push_back(decl);
	}
	if (!d.typeArray.empty())
		for (auto& decl : d.declarators) if (decl.arrayExpr.empty()) decl.arrayExpr = d.typeArray;
	d.valid = !d.type.empty() && !d.declarators.empty();
	return d;
}

int LayoutLocation(const Declaration& d)
{
	for (const auto& item : d.layout)
	{
		const size_t eq = item.find('=');
		if (eq == std::string::npos) continue;
		std::string key = item.substr(0, eq);
		key.erase(key.find_last_not_of(" \t") + 1);
		if (key == "location")
			return std::atoi(item.substr(eq + 1).c_str());
	}
	return -1;
}

std::string LayoutWithout(const Declaration& d, std::initializer_list<const char*> remove)
{
	std::string out;
	for (const auto& item : d.layout)
	{
		std::string key = item.substr(0, item.find('='));
		key.erase(key.find_last_not_of(" \t") + 1);
		bool skip = false;
		for (const char* r : remove) if (key == r) skip = true;
		if (skip) continue;
		if (!out.empty()) out += ", ";
		out += item;
	}
	return out;
}

std::string QualifierText(const Declaration& d, std::initializer_list<const char*> remove)
{
	std::string out;
	for (const auto& q : d.qualifierOrder)
	{
		bool skip = false;
		for (const char* r : remove) if (q == r) skip = true;
		if (!skip) out += q + " ";
	}
	return out;
}

// ===========================================================================
// Constant expressions (array sizes, uniform initializers)
// ===========================================================================
struct Defines
{
	std::map<std::string, std::string> values;
	void Parse(const std::string& src)
	{
		std::istringstream ss(src);
		std::string line;
		while (std::getline(ss, line))
		{
			const auto t = Tokenize(line);
			if (t.size() >= 3 && t[0] == "#" && t[1] == "define")
			{
				std::string value;
				for (size_t k = 3; k < t.size(); ++k) value += t[k] + " ";
				values[t[2]] = value;
			}
		}
	}
	long Eval(const std::string& expr, const std::map<std::string, long>& consts) const
	{
		std::string e = expr;
		for (int pass = 0; pass < 8; ++pass)
		{
			const auto t = Tokenize(e);
			std::string replaced;
			bool changed = false;
			for (const auto& tok : t)
			{
				auto it = values.find(tok);
				auto ct = consts.find(tok);
				if (it != values.end()) { replaced += "(" + it->second + ")"; changed = true; }
				else if (ct != consts.end()) { replaced += std::to_string(ct->second); changed = true; }
				else replaced += tok;
				replaced += " ";
			}
			e = replaced;
			if (!changed) break;
		}
		// Tiny integer expression evaluator: + - * / ( ) and integer literals.
		const auto t = Tokenize(e);
		size_t pos = 0;
		std::function<long()> expr_, term, factor;
		factor = [&]() -> long {
			if (pos >= t.size()) return 0;
			if (t[pos] == "(") { ++pos; long v = expr_(); if (pos < t.size() && t[pos] == ")") ++pos; return v; }
			if (t[pos] == "-") { ++pos; return -factor(); }
			if (t[pos] == "+") { ++pos; return factor(); }
			long v = std::strtol(t[pos].c_str(), nullptr, 0);
			++pos;
			return v;
		};
		term = [&]() -> long {
			long v = factor();
			while (pos < t.size() && (t[pos] == "*" || t[pos] == "/"))
			{
				const bool mul = t[pos] == "*";
				++pos;
				long r = factor();
				v = mul ? v * r : (r ? v / r : 0);
			}
			return v;
		};
		expr_ = [&]() -> long {
			long v = term();
			while (pos < t.size() && (t[pos] == "+" || t[pos] == "-"))
			{
				const bool add = t[pos] == "+";
				++pos;
				long r = term();
				v = add ? v + r : v - r;
			}
			return v;
		};
		return expr_();
	}
};

// Parses "1.0", "vec3(1.0)", "vec2(0.5, 1)", "true", "-2" ... into floats.
bool ParseInitializer(const std::string& text, std::vector<double>& out)
{
	const auto t = Tokenize(text);
	out.clear();
	if (t.empty()) return false;
	size_t pos = 0;
	if (IsIdentStart(t[0][0]) && t[0] != "true" && t[0] != "false")
	{
		// constructor
		if (t.size() < 3 || t[1] != "(") return false;
		pos = 2;
	}
	bool negative = false;
	for (; pos < t.size(); ++pos)
	{
		const std::string& tok = t[pos];
		if (tok == "," || tok == ")" ) { negative = false; continue; }
		if (tok == "-") { negative = !negative; continue; }
		if (tok == "+") continue;
		if (tok == "true") { out.push_back(1.0); continue; }
		if (tok == "false") { out.push_back(0.0); continue; }
		if (std::isdigit((unsigned char)tok[0]) || tok[0] == '.')
		{
			std::string num = tok;
			while (!num.empty() && (num.back() == 'f' || num.back() == 'F' || num.back() == 'u' || num.back() == 'U')) num.pop_back();
			const double v = std::strtod(num.c_str(), nullptr);
			out.push_back(negative ? -v : v);
			negative = false;
			continue;
		}
		return false;
	}
	return !out.empty();
}

// ===========================================================================
// Program interface collected from both stages
// ===========================================================================
struct UniformDecl
{
	std::string type;
	std::string name;
	std::string arrayExpr;
	long arraySize = 0; // 0 = not an array
	std::string init;
};

struct SamplerDecl
{
	std::string type;
	std::string name;
	uint32_t binding = 0;
};

struct StructDecl
{
	std::string name;
	std::string text;
};

struct ProgramInterface
{
	std::vector<UniformDecl> uniforms;          // default block, in declaration order
	std::vector<SamplerDecl> samplers;
	std::vector<StructDecl> structs;            // structs used by uniforms
	std::map<std::string, uint32_t> varyingLocations; // vertex outputs
	uint32_t uniformBlockMask = 0;
	std::string error;

	const UniformDecl* FindUniform(const std::string& n) const
	{
		for (const auto& u : uniforms) if (u.name == n) return &u;
		return nullptr;
	}
	const SamplerDecl* FindSampler(const std::string& n) const
	{
		for (const auto& s : samplers) if (s.name == n) return &s;
		return nullptr;
	}
};

uint32_t UniformBlockPoint(const std::string& blockName)
{
	if (blockName == "LightBlock") return 0;
	if (blockName == "BoneBlock") return 1;
	if (blockName == "DDGIBlock0") return 2;
	if (blockName == "DDGIBlock1") return 3;
	if (blockName == "DDGIBlock2") return 4;
	return 0; // OpenGL default uniform block binding
}

void CollectInterface(const std::vector<Statement>& stmts, const Defines& defines, ProgramInterface& pi)
{
	std::map<std::string, std::string> structTexts;
	for (const auto& s : stmts)
	{
		if (s.kind != Statement::Kind::Statement) continue;
		const auto tokens = Tokenize(s.text);
		if (tokens.size() >= 2 && tokens[0] == "struct")
		{
			structTexts[tokens[1]] = s.text;
			continue;
		}
		bool hasUniform = false;
		for (const auto& t : tokens) if (t == "uniform") { hasUniform = true; break; }
		if (!hasUniform) continue;
		Declaration d = ParseDeclaration(s.text);
		if (!d.valid) continue;
		if (d.isBlock)
		{
			pi.uniformBlockMask |= 1u << UniformBlockPoint(d.blockName);
			continue;
		}
		for (const auto& decl : d.declarators)
		{
			if (IsSamplerTypeName(d.type))
			{
				if (!decl.arrayExpr.empty())
				{
					pi.error = "arrays of samplers are not supported by the Vulkan backend (" + decl.name + ")";
					continue;
				}
				if (!pi.FindSampler(decl.name))
				{
					if (pi.samplers.size() >= kMaxSamplers)
					{
						pi.error = "more than " + std::to_string(kMaxSamplers) + " samplers in one program";
						continue;
					}
					pi.samplers.push_back({d.type, decl.name, kSamplerBindingBase + (uint32_t)pi.samplers.size()});
				}
				continue;
			}
			if (pi.FindUniform(decl.name)) continue;
			UniformDecl u;
			u.type = d.type;
			u.name = decl.name;
			u.arrayExpr = decl.arrayExpr;
			if (!decl.arrayExpr.empty())
			{
				u.arraySize = defines.Eval(decl.arrayExpr, {});
				if (u.arraySize <= 0) u.arraySize = 1;
			}
			u.init = decl.init;
			if (structTexts.count(d.type))
			{
				bool known = false;
				for (const auto& st : pi.structs) if (st.name == d.type) known = true;
				if (!known) pi.structs.push_back({d.type, structTexts[d.type]});
			}
			pi.uniforms.push_back(u);
		}
	}
}

// ===========================================================================
// Translation of one stage
// ===========================================================================
std::string DefaultBlockText(const ProgramInterface& pi)
{
	if (pi.uniforms.empty()) return {};
	std::string out = "layout(set = 0, binding = " + std::to_string(kDefaultBlockBinding) + ", std140) uniform HRL_DefaultUniformBlock\n{\n";
	// bool members are stored as uint : drivers disagree on how a boolean
	// stored in a uniform buffer is read back, a uint compare is portable.
	std::string defines;
	for (const auto& u : pi.uniforms)
	{
		static const char* kBools[4] = {"bool", "bvec2", "bvec3", "bvec4"};
		static const char* kUints[4] = {"uint", "uvec2", "uvec3", "uvec4"};
		int boolIndex = -1;
		for (int b = 0; b < 4; ++b) if (u.type == kBools[b]) boolIndex = b;
		if (boolIndex >= 0 && u.arraySize == 0)
		{
			out += std::string("\t") + kUints[boolIndex] + " hrl_bool_" + u.name + ";\n";
			defines += "#define " + u.name + " " + (boolIndex == 0 ? std::string("(hrl_bool_") + u.name + " != 0u)"
				: std::string("notEqual(hrl_bool_") + u.name + ", " + kUints[boolIndex] + "(0u))") + "\n";
			continue;
		}
		out += "\t" + u.type + " " + u.name;
		if (u.arraySize > 0) out += "[" + std::to_string(u.arraySize) + "]";
		out += ";\n";
	}
	out += "};\n" + defines;
	return out;
}

bool TranslateStage(const std::string& source, bool vertexStage, ProgramInterface& pi, std::string& out, std::string& error)
{
	const std::string clean = StripComments(source);
	auto stmts = SplitTopLevel(clean);
	Defines defines;
	defines.Parse(clean);

	std::set<std::string> localStructs;
	for (const auto& s : stmts)
	{
		if (s.kind != Statement::Kind::Statement) continue;
		const auto t = Tokenize(s.text);
		if (t.size() >= 2 && t[0] == "struct") localStructs.insert(t[1]);
	}

	std::string result;
	bool versionDone = false;
	bool blockEmitted = false;
	std::set<uint32_t> usedLocationsIn, usedLocationsOut;
	// Explicit locations first.
	for (const auto& s : stmts)
	{
		if (s.kind != Statement::Kind::Statement) continue;
		const auto tokens = Tokenize(s.text);
		bool isIn = false, isOut = false;
		for (const auto& t : tokens) { if (t == "in") isIn = true; if (t == "out") isOut = true; if (t == "uniform" || t == "(" ) break; }
		if (!isIn && !isOut) continue;
		Declaration d = ParseDeclaration(s.text);
		if (!d.valid) continue;
		const int loc = LayoutLocation(d);
		if (loc < 0) continue;
		uint32_t slots = 1;
		if (!d.isBlock && !d.declarators.empty())
			slots = LocationSlots(d.type, d.declarators[0].arrayExpr.empty() ? 1u : (uint32_t)std::max(1L, defines.Eval(d.declarators[0].arrayExpr, {})));
		for (uint32_t k = 0; k < slots; ++k) (isIn ? usedLocationsIn : usedLocationsOut).insert((uint32_t)loc + k);
	}
	auto allocate = [](std::set<uint32_t>& used, uint32_t slots) -> uint32_t {
		for (uint32_t base = 0;; ++base)
		{
			bool free = true;
			for (uint32_t k = 0; k < slots; ++k) if (used.count(base + k)) { free = false; break; }
			if (free)
			{
				for (uint32_t k = 0; k < slots; ++k) used.insert(base + k);
				return base;
			}
		}
	};
	// Fragment inputs reuse the vertex output locations (matched by name).
	if (!vertexStage)
		for (const auto& [name, loc] : pi.varyingLocations) (void)name, (void)loc;

	std::string structPrelude;
	for (const auto& st : pi.structs)
		if (!localStructs.count(st.name)) structPrelude += st.text + "\n";

	for (size_t si = 0; si < stmts.size(); ++si)
	{
		const Statement& s = stmts[si];
		if (s.kind == Statement::Kind::Directive)
		{
			const auto t = Tokenize(s.text);
			if (t.size() >= 2 && t[0] == "#" && t[1] == "version")
			{
				result += "#version 450\n";
				versionDone = true;
				continue;
			}
			result += s.text;
			continue;
		}
		if (s.kind == Statement::Kind::Function)
		{
			if (!blockEmitted)
			{
				result += structPrelude + DefaultBlockText(pi);
				blockEmitted = true;
			}
			result += s.text;
			continue;
		}
		const auto tokens = Tokenize(s.text);
		if (tokens.empty()) { result += s.text; continue; }
		if (tokens[0] == "precision") { result += s.text; continue; }
		if (tokens[0] == "struct")
		{
			result += s.text;
			continue;
		}
		bool hasUniform = false, hasIn = false, hasOut = false;
		for (const auto& t : tokens)
		{
			if (t == "(" && tokens[0] != "layout") break;
			if (t == "uniform") hasUniform = true;
			if (t == "in" || t == "attribute" || (t == "varying" && !vertexStage)) hasIn = true;
			if (t == "out" || (t == "varying" && vertexStage)) hasOut = true;
		}
		if (!hasUniform && !hasIn && !hasOut)
		{
			result += s.text;
			continue;
		}
		Declaration d = ParseDeclaration(s.text);
		if (!d.valid)
		{
			result += s.text;
			continue;
		}
		// Keep the line structure : count newlines of the original statement.
		const size_t newlines = (size_t)std::count(s.text.begin(), s.text.end(), '\n');
		std::string replacement;
		if (hasUniform)
		{
			if (d.isBlock)
			{
				const uint32_t binding = kUniformBlockPointBase + UniformBlockPoint(d.blockName);
				std::string layout = LayoutWithout(d, {"binding", "set", "shared", "packed", "std140", "std430"});
				replacement = "layout(" + std::string("std140, set = 0, binding = ") + std::to_string(binding) + (layout.empty() ? "" : ", " + layout) + ") " +
					QualifierText(d, {}) + d.blockName + "\n{" + d.blockBody + "}" + d.blockInstance + ";";
			}
			else if (IsSamplerTypeName(d.type))
			{
				for (const auto& decl : d.declarators)
				{
					const SamplerDecl* sd = pi.FindSampler(decl.name);
					if (!sd) { error = "internal: sampler " + decl.name + " not collected"; return false; }
					replacement += "layout(set = 0, binding = " + std::to_string(sd->binding) + ") uniform " + d.type + " " + decl.name + "; ";
				}
			}
			else
			{
				// Default-block uniforms are gathered in one std140 block.
				if (!blockEmitted)
				{
					replacement = structPrelude + DefaultBlockText(pi);
					blockEmitted = true;
				}
			}
		}
		else
		{
			// Interface variables : explicit locations.
			const bool isInput = hasIn;
			std::string layout = LayoutWithout(d, {"location"});
			const int explicitLoc = LayoutLocation(d);
			auto declText = [&](uint32_t location, const std::string& body) {
				return "layout(location = " + std::to_string(location) + (layout.empty() ? "" : ", " + layout) + ") " + body;
			};
			std::string quals = QualifierText(d, {"attribute", "varying"});
			if (std::find(d.qualifierOrder.begin(), d.qualifierOrder.end(), "attribute") != d.qualifierOrder.end() ||
				std::find(d.qualifierOrder.begin(), d.qualifierOrder.end(), "varying") != d.qualifierOrder.end())
				quals += isInput ? "in " : "out ";
			if (d.isBlock)
			{
				// Interface block : one location range, matched by block name.
				uint32_t slots = 0;
				for (const auto& member : Tokenize(d.blockBody)) (void)member;
				{
					std::stringstream body(d.blockBody);
					std::string memberDecl;
					while (std::getline(body, memberDecl, ';'))
					{
						Declaration md = ParseDeclaration(memberDecl + ";");
						if (!md.valid) continue;
						for (const auto& decl : md.declarators)
							slots += LocationSlots(md.type, decl.arrayExpr.empty() ? 1u : (uint32_t)std::max(1L, defines.Eval(decl.arrayExpr, {})));
					}
				}
				slots = std::max(1u, slots);
				uint32_t location = 0;
				const std::string key = "block:" + d.blockName;
				if (explicitLoc >= 0) location = (uint32_t)explicitLoc;
				else if (!vertexStage && isInput && pi.varyingLocations.count(key))
				{
					location = pi.varyingLocations[key];
					for (uint32_t k = 0; k < slots; ++k) usedLocationsIn.insert(location + k);
				}
				else location = allocate(isInput ? usedLocationsIn : usedLocationsOut, slots);
				if (vertexStage && !isInput) pi.varyingLocations[key] = location;
				replacement = declText(location, quals + d.blockName + "\n{" + d.blockBody + "}" + d.blockInstance + ";");
			}
			else
			{
				for (const auto& decl : d.declarators)
				{
					const uint32_t arraySize = decl.arrayExpr.empty() ? 1u : (uint32_t)std::max(1L, defines.Eval(decl.arrayExpr, {}));
					const uint32_t slots = LocationSlots(d.type, arraySize);
					uint32_t location = 0;
					if (explicitLoc >= 0 && d.declarators.size() == 1) location = (uint32_t)explicitLoc;
					else if (!vertexStage && isInput && pi.varyingLocations.count(decl.name))
					{
						location = pi.varyingLocations[decl.name];
						for (uint32_t k = 0; k < slots; ++k) usedLocationsIn.insert(location + k);
					}
					else location = allocate(isInput ? usedLocationsIn : usedLocationsOut, slots);
					if (vertexStage && !isInput) pi.varyingLocations[decl.name] = location;
					std::string body = quals + d.type + " " + decl.name;
					if (!decl.arrayExpr.empty()) body += "[" + decl.arrayExpr + "]";
					body += ";";
					replacement += declText(location, body) + " ";
				}
			}
		}
		result += replacement + std::string(newlines, '\n');
	}
	if (!blockEmitted)
		result += structPrelude + DefaultBlockText(pi);

	if (!versionDone)
		result = "#version 450\n" + result;

	// Built-in variables renamed in Vulkan GLSL.
	result = ReplaceWord(result, "gl_VertexID", "gl_VertexIndex");
	result = ReplaceWord(result, "gl_InstanceID", "gl_InstanceIndex");

	if (vertexStage)
	{
		// OpenGL clip space keeps z in [-w, w]; Vulkan expects [0, w].
		result = ReplaceWord(result, "main", "hrl_gl_main");
		result += "\nvoid main()\n{\n\thrl_gl_main();\n\tgl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n}\n";
	}
	out = result;
	return true;
}

// ===========================================================================
// SPIR-V reflection
// ===========================================================================
struct SpirvType
{
	uint32_t op = 0;
	std::vector<uint32_t> operands;
};

struct SpirvModule
{
	std::map<uint32_t, std::string> names;
	std::map<std::pair<uint32_t, uint32_t>, std::string> memberNames;
	std::map<uint32_t, std::map<uint32_t, uint32_t>> decorations;               // id -> decoration -> value
	std::map<std::pair<uint32_t, uint32_t>, std::map<uint32_t, uint32_t>> memberDecorations;
	std::map<uint32_t, SpirvType> types;
	std::map<uint32_t, uint32_t> constants;
	struct Var { uint32_t id, type, storage; };
	std::vector<Var> variables;

	bool Parse(const std::vector<uint32_t>& w)
	{
		if (w.size() < 5 || w[0] != 0x07230203u) return false;
		size_t i = 5;
		while (i < w.size())
		{
			const uint32_t word = w[i];
			const uint32_t op = word & 0xFFFFu;
			const uint32_t count = word >> 16;
			if (count == 0 || i + count > w.size()) return false;
			const uint32_t* a = &w[i + 1];
			const uint32_t n = count - 1;
			auto str = [&](uint32_t start) {
				return std::string(reinterpret_cast<const char*>(a + start));
			};
			switch (op)
			{
			case 5: if (n >= 2) names[a[0]] = str(1); break;                       // OpName
			case 6: if (n >= 3) memberNames[{a[0], a[1]}] = str(2); break;          // OpMemberName
			case 71: if (n >= 2) decorations[a[0]][a[1]] = n >= 3 ? a[2] : 1; break; // OpDecorate
			case 72: if (n >= 3) memberDecorations[{a[0], a[1]}][a[2]] = n >= 4 ? a[3] : 1; break; // OpMemberDecorate
			case 20: case 21: case 22: case 23: case 24: case 25: case 27: case 28: case 30: case 32:
				if (n >= 1) { SpirvType t; t.op = op; t.operands.assign(a + 1, a + n); types[a[0]] = t; }
				break;
			case 43: if (n >= 3) constants[a[1]] = a[2]; break; // OpConstant
			case 59: if (n >= 3) variables.push_back({a[1], a[0], a[2]}); break; // OpVariable
			default: break;
			}
			i += count;
		}
		return true;
	}

	uint32_t Decoration(uint32_t id, uint32_t dec, uint32_t def = UINT32_MAX) const
	{
		auto it = decorations.find(id);
		if (it == decorations.end()) return def;
		auto jt = it->second.find(dec);
		return jt == it->second.end() ? def : jt->second;
	}
	uint32_t MemberDecoration(uint32_t type, uint32_t member, uint32_t dec, uint32_t def = UINT32_MAX) const
	{
		auto it = memberDecorations.find({type, member});
		if (it == memberDecorations.end()) return def;
		auto jt = it->second.find(dec);
		return jt == it->second.end() ? def : jt->second;
	}
	// Scalar base of a (vector/matrix/scalar) type.
	bool Scalar(uint32_t typeId, ShaderUniform::Base& base, uint32_t& comps, uint32_t& cols) const
	{
		auto it = types.find(typeId);
		if (it == types.end()) return false;
		const SpirvType& t = it->second;
		if (t.op == 20) { base = ShaderUniform::Base::Bool; comps = 1; cols = 1; return true; }
		if (t.op == 21) { base = (t.operands.size() >= 2 && t.operands[1]) ? ShaderUniform::Base::Int : ShaderUniform::Base::UInt; comps = 1; cols = 1; return true; }
		if (t.op == 22) { base = ShaderUniform::Base::Float; comps = 1; cols = 1; return true; }
		if (t.op == 23 && t.operands.size() >= 2)
		{
			uint32_t c = 1, k = 1;
			if (!Scalar(t.operands[0], base, c, k)) return false;
			comps = t.operands[1]; cols = 1; return true;
		}
		if (t.op == 24 && t.operands.size() >= 2)
		{
			uint32_t c = 1, k = 1;
			if (!Scalar(t.operands[0], base, c, k)) return false;
			comps = c; cols = t.operands[1]; return true;
		}
		return false;
	}

	void Flatten(uint32_t structType, const std::string& prefix, uint32_t baseOffset, std::vector<ShaderUniform>& out) const
	{
		auto it = types.find(structType);
		if (it == types.end() || it->second.op != 30) return;
		const auto& members = it->second.operands;
		for (uint32_t m = 0; m < members.size(); ++m)
		{
			auto nameIt = memberNames.find({structType, m});
			const std::string name = prefix + (nameIt != memberNames.end() ? nameIt->second : ("member" + std::to_string(m)));
			const uint32_t offset = baseOffset + MemberDecoration(structType, m, 35 /*Offset*/, 0);
			const uint32_t matrixStride = MemberDecoration(structType, m, 7 /*MatrixStride*/, 16);
			uint32_t typeId = members[m];
			uint32_t arraySize = 0, arrayStride = 0;
			auto tt = types.find(typeId);
			if (tt != types.end() && tt->second.op == 28 && tt->second.operands.size() >= 2)
			{
				auto c = constants.find(tt->second.operands[1]);
				arraySize = c != constants.end() ? c->second : 1;
				arrayStride = Decoration(typeId, 6 /*ArrayStride*/, 16);
				typeId = tt->second.operands[0];
				tt = types.find(typeId);
			}
			if (tt != types.end() && tt->second.op == 30)
			{
				if (arraySize == 0) Flatten(typeId, name + ".", offset, out);
				else
					for (uint32_t e = 0; e < arraySize; ++e)
						Flatten(typeId, name + "[" + std::to_string(e) + "].", offset + e * arrayStride, out);
				continue;
			}
			ShaderUniform u;
			u.name = name;
			bool storedBool = false;
			if (prefix.empty() && name.rfind("hrl_bool_", 0) == 0) { u.name = name.substr(9); storedBool = true; }
			u.offset = offset;
			u.matrixStride = matrixStride;
			u.arraySize = arraySize == 0 ? 1 : arraySize;
			u.arrayStride = arrayStride;
			if (!Scalar(typeId, u.base, u.components, u.columns)) continue;
			if (storedBool) u.base = ShaderUniform::Base::Bool;
			out.push_back(u);
		}
	}
};

std::atomic<uint64_t> g_programIds{0};
} // namespace

// ===========================================================================
// Public
// ===========================================================================
std::string TranslateGLSLForVulkan(const std::string& vert, const std::string& frag, bool vertexStage, std::string& error)
{
	ProgramInterface pi;
	Defines dv, df;
	const std::string cv = StripComments(vert), cf = StripComments(frag);
	dv.Parse(cv);
	df.Parse(cf);
	CollectInterface(SplitTopLevel(cv), dv, pi);
	CollectInterface(SplitTopLevel(cf), df, pi);
	std::string outV, outF;
	if (!TranslateStage(vert, true, pi, outV, error)) return {};
	if (!TranslateStage(frag, false, pi, outF, error)) return {};
	return vertexStage ? outV : outF;
}

VulkanProgram::~VulkanProgram()
{
	Destroy();
}

void VulkanProgram::Destroy()
{
	Core& core = GetCore();
	if (core.device)
	{
		VkShaderModule v = vertexModule, f = fragmentModule;
		VkDevice device = core.device;
		if (v || f)
			core.Defer([device, v, f]() {
				if (v) vkDestroyShaderModule(device, v, nullptr);
				if (f) vkDestroyShaderModule(device, f, nullptr);
			});
	}
	vertexModule = fragmentModule = VK_NULL_HANDLE;
	uniforms.clear();
	samplers.clear();
	vertexInputs.clear();
	data.clear();
	lookup_.clear();
	blockSize = 0;
}

bool VulkanProgram::Create(const char* vert, size_t vertSize, const char* frag, size_t fragSize, std::string& error)
{
	Destroy();
	if (!vert || !frag || vertSize == 0 || fragSize == 0)
	{
		error = "Vulkan shader: invalid source buffer";
		return false;
	}
	// HRL embedded resources carry a trailing NUL byte.
	std::string vs(vert, vert + vertSize), fs(frag, frag + fragSize);
	while (!vs.empty() && vs.back() == '\0') vs.pop_back();
	while (!fs.empty() && fs.back() == '\0') fs.pop_back();

	ProgramInterface pi;
	Defines dv, df;
	const std::string cv = StripComments(vs), cf = StripComments(fs);
	dv.Parse(cv);
	df.Parse(cf);
	CollectInterface(SplitTopLevel(cv), dv, pi);
	CollectInterface(SplitTopLevel(cf), df, pi);
	if (!pi.error.empty())
	{
		error = "Vulkan shader: " + pi.error;
		return false;
	}
	std::string vk_vs, vk_fs;
	if (!TranslateStage(vs, true, pi, vk_vs, error) || !TranslateStage(fs, false, pi, vk_fs, error))
		return false;

	std::vector<uint32_t> spvV, spvF;
	if (!CompileGLSLToSpirv(vk_vs, true, spvV, error))
	{
		error += "\n--- translated vertex shader ---\n" + vk_vs;
		return false;
	}
	if (!CompileGLSLToSpirv(vk_fs, false, spvF, error))
	{
		error += "\n--- translated fragment shader ---\n" + vk_fs;
		return false;
	}

	SpirvModule mv, mf;
	if (!mv.Parse(spvV) || !mf.Parse(spvF))
	{
		error = "Vulkan shader: invalid SPIR-V";
		return false;
	}
	// Default block layout (identical in both stages; take the first that has it).
	for (SpirvModule* m : {&mv, &mf})
	{
		if (!uniforms.empty()) break;
		for (const auto& var : m->variables)
		{
			if (var.storage != 2 /*Uniform*/) continue;
			if (m->Decoration(var.id, 33 /*Binding*/) != kDefaultBlockBinding) continue;
			auto ptr = m->types.find(var.type);
			if (ptr == m->types.end() || ptr->second.operands.size() < 2) continue;
			const uint32_t structType = ptr->second.operands[1];
			m->Flatten(structType, "", 0, uniforms);
		}
	}
	uint32_t size = 0;
	for (const auto& u : uniforms)
	{
		const uint32_t elemSize = u.columns > 1 ? u.columns * u.matrixStride : u.components * 4;
		const uint32_t end = u.offset + (u.arraySize > 1 ? (u.arraySize - 1) * u.arrayStride : 0) + elemSize;
		size = std::max(size, end);
	}
	blockSize = (size + 15u) & ~15u;
	data.assign(blockSize, 0);

	for (const auto& s : pi.samplers)
	{
		ShaderSampler ss;
		ss.name = s.name;
		ss.binding = s.binding;
		ss.dim = SamplerDimFromType(s.type);
		if (ss.dim == SamplerDim::Unsupported)
		{
			error = "Vulkan shader: unsupported sampler type " + s.type;
			return false;
		}
		samplers.push_back(ss);
	}
	uniformBlockMask = pi.uniformBlockMask;

	// Vertex inputs
	for (const auto& var : mv.variables)
	{
		if (var.storage != 1 /*Input*/) continue;
		if (mv.Decoration(var.id, 11 /*BuiltIn*/) != UINT32_MAX) continue;
		const uint32_t loc = mv.Decoration(var.id, 30 /*Location*/);
		if (loc == UINT32_MAX) continue;
		auto ptr = mv.types.find(var.type);
		if (ptr == mv.types.end() || ptr->second.operands.size() < 2) continue;
		uint32_t typeId = ptr->second.operands[1];
		uint32_t count = 1;
		auto tt = mv.types.find(typeId);
		if (tt != mv.types.end() && tt->second.op == 28 && tt->second.operands.size() >= 2)
		{
			auto c = mv.constants.find(tt->second.operands[1]);
			count = c != mv.constants.end() ? c->second : 1;
			typeId = tt->second.operands[0];
		}
		ShaderUniform::Base base;
		uint32_t comps = 1, cols = 1;
		if (!mv.Scalar(typeId, base, comps, cols)) continue;
		for (uint32_t e = 0; e < count; ++e)
			for (uint32_t c = 0; c < cols; ++c)
				vertexInputs.push_back({loc + e * cols + c, base, comps});
	}
	fragmentOutputs = 1;
	for (const auto& var : mf.variables)
	{
		if (var.storage != 3 /*Output*/) continue;
		const uint32_t loc = mf.Decoration(var.id, 30);
		if (loc != UINT32_MAX) fragmentOutputs = std::max(fragmentOutputs, loc + 1);
	}

	Core& core = GetCore();
	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = spvV.size() * 4;
	ci.pCode = spvV.data();
	if (vkCreateShaderModule(core.device, &ci, nullptr, &vertexModule) != VK_SUCCESS)
	{
		error = "Vulkan shader: vkCreateShaderModule(vertex) failed";
		return false;
	}
	ci.codeSize = spvF.size() * 4;
	ci.pCode = spvF.data();
	if (vkCreateShaderModule(core.device, &ci, nullptr, &fragmentModule) != VK_SUCCESS)
	{
		error = "Vulkan shader: vkCreateShaderModule(fragment) failed";
		Destroy();
		return false;
	}
	id = ++g_programIds;

	// Uniform initializers ("uniform float gamma = 2.2;") become default values.
	for (const auto& u : pi.uniforms)
	{
		if (u.init.empty()) continue;
		std::vector<double> values;
		if (!ParseInitializer(u.init, values)) continue;
		Location loc;
		if (!Find(u.name.c_str(), loc) || loc.uniform < 0) continue;
		const ShaderUniform& su = uniforms[(size_t)loc.uniform];
		const uint32_t count = su.components * su.columns;
		for (uint32_t c = 0; c < su.columns; ++c)
			for (uint32_t r = 0; r < su.components; ++r)
			{
				const uint32_t idx = c * su.components + r;
				double v = values.size() == 1 ? (su.columns > 1 ? (r == c ? values[0] : 0.0) : values[0])
					: (idx < values.size() ? values[idx] : 0.0);
				unsigned char* dst = data.data() + su.offset + c * su.matrixStride + r * 4;
				if (su.base == ShaderUniform::Base::Float) { float f = (float)v; std::memcpy(dst, &f, 4); }
				else if (su.base == ShaderUniform::Base::Int) { int32_t x = (int32_t)v; std::memcpy(dst, &x, 4); }
				else { uint32_t x = (uint32_t)(v != 0.0 && su.base == ShaderUniform::Base::Bool ? 1 : (uint32_t)v); std::memcpy(dst, &x, 4); }
			}
		(void)count;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Uniform setters (glUniform* rules)
// ---------------------------------------------------------------------------
bool VulkanProgram::Find(const char* name, Location& loc)
{
	if (!name) return false;
	auto it = lookup_.find(name);
	if (it != lookup_.end()) { loc = it->second; return loc.uniform >= 0 || loc.sampler >= 0; }
	Location result;
	std::string n = name;
	for (size_t i = 0; i < samplers.size(); ++i)
		if (samplers[i].name == n) result.sampler = (int)i;
	if (result.sampler < 0)
	{
		// exact match, "name[0]" for the first element, or "name[i]".
		auto match = [&](const std::string& key, uint32_t element) {
			for (size_t i = 0; i < uniforms.size(); ++i)
				if (uniforms[i].name == key && element < uniforms[i].arraySize)
				{
					result.uniform = (int)i;
					result.element = element;
					return true;
				}
			return false;
		};
		if (!match(n, 0))
		{
			const size_t lb = n.rfind('[');
			if (lb != std::string::npos && n.back() == ']')
			{
				const uint32_t element = (uint32_t)std::strtoul(n.substr(lb + 1).c_str(), nullptr, 10);
				match(n.substr(0, lb), element);
			}
		}
	}
	lookup_.emplace(n, result);
	loc = result;
	return result.uniform >= 0 || result.sampler >= 0;
}

unsigned char* VulkanProgram::Slot(const ShaderUniform& u, uint32_t element)
{
	return data.data() + u.offset + element * u.arrayStride;
}

void VulkanProgram::SetInt(const char* name, int value)
{
	Location loc;
	if (!Find(name, loc)) return;
	if (loc.sampler >= 0)
	{
		if (samplers[(size_t)loc.sampler].unit != value) { samplers[(size_t)loc.sampler].unit = value; ++dataVersion; }
		return;
	}
	const ShaderUniform& u = uniforms[(size_t)loc.uniform];
	if (u.columns != 1 || u.components != 1) return;
	if (u.base != ShaderUniform::Base::Int && u.base != ShaderUniform::Base::Bool) return;
	const int32_t v = u.base == ShaderUniform::Base::Bool ? (value != 0 ? 1 : 0) : value;
	unsigned char* dst = Slot(u, loc.element);
	if (std::memcmp(dst, &v, 4) != 0) { std::memcpy(dst, &v, 4); ++dataVersion; }
}

void VulkanProgram::SetUint(const char* name, uint32_t value)
{
	Location loc;
	if (!Find(name, loc) || loc.uniform < 0) return;
	const ShaderUniform& u = uniforms[(size_t)loc.uniform];
	if (u.columns != 1 || u.components != 1) return;
	if (u.base != ShaderUniform::Base::UInt && u.base != ShaderUniform::Base::Bool) return;
	const uint32_t v = u.base == ShaderUniform::Base::Bool ? (value != 0 ? 1u : 0u) : value;
	unsigned char* dst = Slot(u, loc.element);
	if (std::memcmp(dst, &v, 4) != 0) { std::memcpy(dst, &v, 4); ++dataVersion; }
}

void VulkanProgram::SetFloat(const char* name, float value)
{
	Location loc;
	if (!Find(name, loc) || loc.uniform < 0) return;
	const ShaderUniform& u = uniforms[(size_t)loc.uniform];
	if (u.columns != 1 || u.components != 1) return;
	unsigned char* dst = Slot(u, loc.element);
	if (u.base == ShaderUniform::Base::Float)
	{
		if (std::memcmp(dst, &value, 4) != 0) { std::memcpy(dst, &value, 4); ++dataVersion; }
	}
	else if (u.base == ShaderUniform::Base::Bool)
	{
		const uint32_t v = value != 0.f ? 1u : 0u;
		if (std::memcmp(dst, &v, 4) != 0) { std::memcpy(dst, &v, 4); ++dataVersion; }
	}
}

#define HRL_VK_SET_VECTOR(N)                                                                   \
	Location loc;                                                                              \
	if (!Find(name, loc) || loc.uniform < 0) return;                                           \
	const ShaderUniform& u = uniforms[(size_t)loc.uniform];                                     \
	if (u.columns != 1 || u.components != N) return;                                           \
	unsigned char* dst = Slot(u, loc.element);                                                  \
	if (u.base == ShaderUniform::Base::Float)                                                  \
	{                                                                                          \
		if (std::memcmp(dst, &value[0], 4 * N) != 0) { std::memcpy(dst, &value[0], 4 * N); ++dataVersion; } \
	}                                                                                          \
	else if (u.base == ShaderUniform::Base::Bool)                                              \
	{                                                                                          \
		uint32_t b[4] = {0, 0, 0, 0};                                                          \
		for (int k = 0; k < N; ++k) b[k] = value[k] != 0.f ? 1u : 0u;                          \
		if (std::memcmp(dst, b, 4 * N) != 0) { std::memcpy(dst, b, 4 * N); ++dataVersion; }    \
	}

void VulkanProgram::SetVec2(const char* name, const glm::vec2& value) { HRL_VK_SET_VECTOR(2) }
void VulkanProgram::SetVec3(const char* name, const glm::vec3& value) { HRL_VK_SET_VECTOR(3) }
void VulkanProgram::SetVec4(const char* name, const glm::vec4& value) { HRL_VK_SET_VECTOR(4) }
#undef HRL_VK_SET_VECTOR

void VulkanProgram::SetMat3(const char* name, const glm::mat3& value)
{
	Location loc;
	if (!Find(name, loc) || loc.uniform < 0) return;
	const ShaderUniform& u = uniforms[(size_t)loc.uniform];
	if (u.columns != 3 || u.components != 3 || u.base != ShaderUniform::Base::Float) return;
	unsigned char* dst = Slot(u, loc.element);
	for (int c = 0; c < 3; ++c)
	{
		unsigned char* col = dst + c * u.matrixStride;
		if (std::memcmp(col, &value[c][0], 12) != 0) { std::memcpy(col, &value[c][0], 12); ++dataVersion; }
	}
}

void VulkanProgram::SetMat4(const char* name, const glm::mat4& value)
{
	Location loc;
	if (!Find(name, loc) || loc.uniform < 0) return;
	const ShaderUniform& u = uniforms[(size_t)loc.uniform];
	if (u.columns != 4 || u.components != 4 || u.base != ShaderUniform::Base::Float) return;
	unsigned char* dst = Slot(u, loc.element);
	for (int c = 0; c < 4; ++c)
	{
		unsigned char* col = dst + c * u.matrixStride;
		if (std::memcmp(col, &value[c][0], 16) != 0) { std::memcpy(col, &value[c][0], 16); ++dataVersion; }
	}
}
} // namespace hrlvk
