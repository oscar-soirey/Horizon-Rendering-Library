#include "gl33_shader.h"

#include "../../core/utils_functions.h"

#include <unordered_map>
#include <string>

#include <glad/glad.h>

//pour glm::value_ptr
#include <glm/gtc/type_ptr.hpp>

#define DEBUG_MSG() printf("[DEBUG] %s:%d\n", __FILE__, __LINE__)


int GL33_Shader::GL33_Create(const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize)
{
  //id des shaders
  unsigned int vertex, fragment;

  //on passe un tableau de pointeurs
  const GLchar* vertsrc = _vertContent;
  const GLchar* fragsrc = _fragContent;

  vertex = glCreateShader(GL_VERTEX_SHADER);

  glShaderSource(vertex, 1, &vertsrc, nullptr);
  glCompileShader(vertex);

  GLint success;
  char infoLog[512];
  glGetShaderiv(vertex, GL_COMPILE_STATUS, &success);
  if (!success)
  {
    glGetShaderInfoLog(vertex, 512, nullptr, infoLog);
    SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_FATAL, "Vertex Compilation Failed: " + static_cast<std::string>(infoLog));
    glDeleteShader(vertex);
    return -1;
  }

  //creation et compilation du fragment shader
  fragment = glCreateShader(GL_FRAGMENT_SHADER);

  glShaderSource(fragment, 1, &fragsrc, nullptr);
  glCompileShader(fragment);

  glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
  if (!success)
  {
    glGetShaderInfoLog(fragment, 512, nullptr, infoLog);
    SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_FATAL, "Fragment Compilation Failed: " + static_cast<std::string>(infoLog));
    glDeleteShader(fragment);
    return -1;
  }

  //Lier les shaders dans un programme
  //(pour vérifier que les shaders peuvent coopérer (ex. : le vertex shader produit bien des sorties que le fragment shader sait utiliser).
  id = glCreateProgram();
  glAttachShader(id, vertex);
  glAttachShader(id, fragment);
  glLinkProgram(id);

  glGetProgramiv(id, GL_LINK_STATUS, &success);
  if (!success)
  {
    glGetProgramInfoLog(id, 512, nullptr, infoLog);
    SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_FATAL, "Program link Failed: " + static_cast<std::string>(infoLog));
    glDeleteProgram(id);
    return -1;
  }

  //nettoyage des shaders intermediaires (une fois liées dans un programme, elles ne sont plus nécéssaires individuellement)
  glDeleteShader(vertex);
  glDeleteShader(fragment);

  glUseProgram(id);


  //Faire les bindings des ubo (obligatoire coté CPU avant opengl 4.2+)
  GLuint uboIndex = glGetUniformBlockIndex(id, "LightBlock");
  if (uboIndex != GL_INVALID_INDEX)
  {
    //on choisit le binding point 0 pour le uniform LightBlock
    glUniformBlockBinding(id, uboIndex, 0);
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

void GL33_Shader::SetMat4(const std::string &name, const glm::mat4 &value)
{
  glUniformMatrix4fv(FindUniformLocation(name), 1, GL_FALSE, glm::value_ptr(value));
}

GLint GL33_Shader::FindUniformLocation(const std::string &name)
{
  auto [it, inserted] = uniform_locations_.try_emplace(name, glGetUniformLocation(id, name.c_str()));
  return it->second;
}