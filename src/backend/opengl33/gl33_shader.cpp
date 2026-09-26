#include "gl33_shader.h"

#include "../../core/utils_functions.h"

#include <unordered_map>
#include <string>
#include <limits>

#include <glad/glad.h>

//pour glm::value_ptr
#include <glm/gtc/type_ptr.hpp>

#define DEBUG_MSG() printf("[DEBUG] %s:%d\n", __FILE__, __LINE__)


int GL33_Shader::GL33_Create(const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize)
{
  if (!_vertContent || !_fragContent || _vertSize == 0 || _fragSize == 0 ||
      _vertSize > static_cast<size_t>(std::numeric_limits<GLint>::max()) ||
      _fragSize > static_cast<size_t>(std::numeric_limits<GLint>::max()))
  {
    SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "GL33_CreateShader: invalid shader source buffer");
    return -1;
  }

  const GLchar* vertsrc = _vertContent;
  const GLchar* fragsrc = _fragContent;
  GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
  if (vertex == 0)
  {
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GL33_CreateShader: glCreateShader(vertex) failed");
    return -1;
  }

  const GLint vertLength = static_cast<GLint>(_vertSize);
  glShaderSource(vertex, 1, &vertsrc, &vertLength);
  glCompileShader(vertex);

  GLint success = GL_FALSE;
  char infoLog[512] = {};
  glGetShaderiv(vertex, GL_COMPILE_STATUS, &success);
  if (!success)
  {
    glGetShaderInfoLog(vertex, 512, nullptr, infoLog);
    SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_FATAL,
      "Vertex Compilation Failed: " + static_cast<std::string>(infoLog));
    glDeleteShader(vertex);
    return -1;
  }

  GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
  if (fragment == 0)
  {
    glDeleteShader(vertex);
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GL33_CreateShader: glCreateShader(fragment) failed");
    return -1;
  }

  const GLint fragLength = static_cast<GLint>(_fragSize);
  glShaderSource(fragment, 1, &fragsrc, &fragLength);
  glCompileShader(fragment);

  glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
  if (!success)
  {
    glGetShaderInfoLog(fragment, 512, nullptr, infoLog);
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
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GL33_CreateShader: glCreateProgram failed");
    return -1;
  }

  glAttachShader(id, vertex);
  glAttachShader(id, fragment);
  glLinkProgram(id);

  glGetProgramiv(id, GL_LINK_STATUS, &success);
  if (!success)
  {
    glGetProgramInfoLog(id, 512, nullptr, infoLog);
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

  const char* ddgiBlocks[] = { "DDGIBlock0", "DDGIBlock1", "DDGIBlock2" };
  for (GLuint binding = 0; binding < 3; ++binding)
  {
    const GLuint ddgiBlockIndex = glGetUniformBlockIndex(id, ddgiBlocks[binding]);
    if (ddgiBlockIndex != GL_INVALID_INDEX)
      glUniformBlockBinding(id, ddgiBlockIndex, 2 + binding);
  }

  return 0;
}


GL33_Shader::~GL33_Shader()
{
  glDeleteProgram(id);
}

void GL33_Shader::Use()
{
  glUseProgram(id);
}

uint64_t GL33_Shader::GetId() const
{
  return id;
}

void GL33_Shader::SetInt(const std::string &name, int value)
{
  glUniform1i(FindUniformLocation(name), value);
}

void GL33_Shader::SetUint(const std::string &name, uint32_t value)
{
  glUniform1ui(FindUniformLocation(name), value);
}

void GL33_Shader::SetFloat(const std::string &name, float value)
{
  glUniform1f(FindUniformLocation(name), value);
}

void GL33_Shader::SetVec2(const std::string &name, const glm::vec2 &value)
{
  glUniform2f(FindUniformLocation(name), value.x, value.y);
}

void GL33_Shader::SetVec3(const std::string &name, const glm::vec3 &value)
{
  glUniform3f(FindUniformLocation(name), value.x, value.y, value.z);
}

void GL33_Shader::SetVec4(const std::string &name, const glm::vec4 &value)
{
  glUniform4f(FindUniformLocation(name), value.x, value.y, value.z, value.w);
}

void GL33_Shader::SetMat3(const std::string &name, const glm::mat3 &value)
{
  glUniformMatrix3fv(FindUniformLocation(name), 1, GL_FALSE, glm::value_ptr(value));
}

void GL33_Shader::SetMat4(const std::string &name, const glm::mat4 &value)
{
  glUniformMatrix4fv(FindUniformLocation(name), 1, GL_FALSE, glm::value_ptr(value));
}

GLint GL33_Shader::FindUniformLocation(const std::string &name)
{
  auto [it, inserted] = uniform_locations_.try_emplace(name, glGetUniformLocation(id, name.c_str()));
  return it->second;
}