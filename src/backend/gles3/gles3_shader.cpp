#include "gles3_shader.h"

#include "../../core/utils_functions.h"

#include <unordered_map>
#include <string>
#include <limits>
#include <cstring>

#include "gles3_gl.h"

//pour glm::value_ptr
#include <glm/gtc/type_ptr.hpp>



// Desktop GLSL ("#version 330 core" and later) is rewritten to GLSL ES 3.00 so
// that simple user shaders written for the OpenGL 3.3 backend keep working.
// Sources that already start with "#version 300 es" are passed through as is.
static std::string GLES3_TranslateSource(const char* content, size_t size)
{
  std::string src(content, size);
  // Embedded resources may carry a trailing NUL.
  while (!src.empty() && src.back() == '\0')
    src.pop_back();

  static const char* kHeader =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n";

  const size_t versionPos = src.find("#version");
  if (versionPos == std::string::npos)
    return std::string(kHeader) + src;

  size_t lineEnd = src.find('\n', versionPos);
  if (lineEnd == std::string::npos)
    lineEnd = src.size();
  const std::string versionLine = src.substr(versionPos, lineEnd - versionPos);
  if (versionLine.find(" es") != std::string::npos)
    return src;

  // GLSL ES requires #version to be the very first token: drop whatever
  // (comments) preceded it in the desktop source.
  return std::string(kHeader) + src.substr(lineEnd < src.size() ? lineEnd + 1 : lineEnd);
}


int GLES3_Shader::GLES3_Create(const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize)
{
  if (!_vertContent || !_fragContent || _vertSize == 0 || _fragSize == 0 ||
      _vertSize > static_cast<size_t>(std::numeric_limits<GLint>::max()) ||
      _fragSize > static_cast<size_t>(std::numeric_limits<GLint>::max()))
  {
    SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "GLES3_CreateShader: invalid shader source buffer");
    return -1;
  }

  const std::string vertTranslated = GLES3_TranslateSource(_vertContent, _vertSize);
  const std::string fragTranslated = GLES3_TranslateSource(_fragContent, _fragSize);
  const GLchar* vertsrc = vertTranslated.c_str();
  const GLchar* fragsrc = fragTranslated.c_str();
  GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
  if (vertex == 0)
  {
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GLES3_CreateShader: glCreateShader(vertex) failed");
    return -1;
  }

  const GLint vertLength = static_cast<GLint>(vertTranslated.size());
  glShaderSource(vertex, 1, &vertsrc, &vertLength);
  glCompileShader(vertex);

  GLint success = GL_FALSE;
  char infoLog[1024] = {};
  glGetShaderiv(vertex, GL_COMPILE_STATUS, &success);
  if (!success)
  {
    glGetShaderInfoLog(vertex, 1024, nullptr, infoLog);
    SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_FATAL,
      "Vertex Compilation Failed: " + static_cast<std::string>(infoLog));
    glDeleteShader(vertex);
    return -1;
  }

  GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
  if (fragment == 0)
  {
    glDeleteShader(vertex);
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GLES3_CreateShader: glCreateShader(fragment) failed");
    return -1;
  }

  const GLint fragLength = static_cast<GLint>(fragTranslated.size());
  glShaderSource(fragment, 1, &fragsrc, &fragLength);
  glCompileShader(fragment);

  glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
  if (!success)
  {
    glGetShaderInfoLog(fragment, 1024, nullptr, infoLog);
    SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_FATAL,
      "Fragment Compilation Failed: " + static_cast<std::string>(infoLog));
    glDeleteShader(fragment);
    glDeleteShader(vertex);
    return -1;
  }

  id = glCreateProgram();
  if (id == 0)
  {
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GLES3_CreateShader: glCreateProgram failed");
    return -1;
  }

  glAttachShader(id, vertex);
  glAttachShader(id, fragment);
  glLinkProgram(id);

  glGetProgramiv(id, GL_LINK_STATUS, &success);
  if (!success)
  {
    glGetProgramInfoLog(id, 1024, nullptr, infoLog);
    SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_FATAL,
      "Program link Failed: " + static_cast<std::string>(infoLog));
    glDeleteProgram(id);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    id = 0;
    return -1;
  }

  glDeleteShader(vertex);
  glDeleteShader(fragment);
  glUseProgram(id);

  const GLuint uboIndex = glGetUniformBlockIndex(id, "LightBlock");
  if (uboIndex != GL_INVALID_INDEX)
    glUniformBlockBinding(id, uboIndex, 0);

  const GLuint boneBlockIndex = glGetUniformBlockIndex(id, "BoneBlock");
  if (boneBlockIndex != GL_INVALID_INDEX)
    glUniformBlockBinding(id, boneBlockIndex, 1);

  return 0;
}


GLES3_Shader::~GLES3_Shader()
{
  if (id != 0)
    glDeleteProgram(static_cast<GLuint>(id));
}

void GLES3_Shader::Use()
{
  glUseProgram(id);
}

uint64_t GLES3_Shader::GetId() const
{
  return id;
}

uint32_t GLES3_Shader::s_value_generation_ = 1;

void GLES3_Shader::InvalidateAllValueCaches()
{
  ++s_value_generation_;
  if (s_value_generation_ == 0)
    s_value_generation_ = 1; // 0 est reserve a "aucune valeur"
}

GLES3_Shader::UniformSlot* GLES3_Shader::FindUniformSlot(const char* name)
{
  if (!name)
    return nullptr;
  lookup_key_.assign(name);
  auto it = uniform_indices_.find(lookup_key_);
  if (it != uniform_indices_.end())
    return &uniform_slots_[it->second];

  // Premier acces a ce nom : une seule requete OpenGL, ensuite tout vient du cache.
  UniformSlot slot;
  slot.location = glGetUniformLocation(static_cast<GLuint>(id), name);
  uniform_slots_.push_back(slot);
  const uint32_t index = static_cast<uint32_t>(uniform_slots_.size() - 1);
  uniform_indices_.emplace(lookup_key_, index);
  return &uniform_slots_[index];
}

bool GLES3_Shader::NeedsUpload(UniformSlot* slot, const void* value, size_t size)
{
  // Uniform absent du programme (optimise par le compilateur GLSL, ou nom
  // inconnu) : glUniform*(-1) ne fait rien, inutile de l'appeler.
  if (!slot || slot->location < 0)
    return false;
  if (slot->generation == s_value_generation_ && std::memcmp(slot->data, value, size) == 0)
    return false;
  std::memcpy(slot->data, value, size);
  slot->generation = s_value_generation_;
  return true;
}

void GLES3_Shader::SetInt(const char* name, int value)
{
  UniformSlot* slot = FindUniformSlot(name);
  if (NeedsUpload(slot, &value, sizeof(value)))
    glUniform1i(slot->location, value);
}

void GLES3_Shader::SetUint(const char* name, uint32_t value)
{
  UniformSlot* slot = FindUniformSlot(name);
  if (NeedsUpload(slot, &value, sizeof(value)))
    glUniform1ui(slot->location, value);
}

void GLES3_Shader::SetFloat(const char* name, float value)
{
  UniformSlot* slot = FindUniformSlot(name);
  if (NeedsUpload(slot, &value, sizeof(value)))
    glUniform1f(slot->location, value);
}

void GLES3_Shader::SetVec2(const char* name, const glm::vec2 &value)
{
  UniformSlot* slot = FindUniformSlot(name);
  if (NeedsUpload(slot, glm::value_ptr(value), sizeof(float) * 2))
    glUniform2f(slot->location, value.x, value.y);
}

void GLES3_Shader::SetVec3(const char* name, const glm::vec3 &value)
{
  UniformSlot* slot = FindUniformSlot(name);
  if (NeedsUpload(slot, glm::value_ptr(value), sizeof(float) * 3))
    glUniform3f(slot->location, value.x, value.y, value.z);
}

void GLES3_Shader::SetVec4(const char* name, const glm::vec4 &value)
{
  UniformSlot* slot = FindUniformSlot(name);
  if (NeedsUpload(slot, glm::value_ptr(value), sizeof(float) * 4))
    glUniform4f(slot->location, value.x, value.y, value.z, value.w);
}

void GLES3_Shader::SetMat3(const char* name, const glm::mat3 &value)
{
  UniformSlot* slot = FindUniformSlot(name);
  if (NeedsUpload(slot, glm::value_ptr(value), sizeof(float) * 9))
    glUniformMatrix3fv(slot->location, 1, GL_FALSE, glm::value_ptr(value));
}

void GLES3_Shader::SetMat4(const char* name, const glm::mat4 &value)
{
  UniformSlot* slot = FindUniformSlot(name);
  if (NeedsUpload(slot, glm::value_ptr(value), sizeof(float) * 16))
    glUniformMatrix4fv(slot->location, 1, GL_FALSE, glm::value_ptr(value));
}
