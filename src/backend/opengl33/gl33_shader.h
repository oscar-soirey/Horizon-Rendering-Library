#ifndef GL33_SHADER
#define GL33_SHADER

#include "../../core/backend_vtable.h"

#include <glm/glm.hpp>
#include <glad/glad.h>

#include <unordered_map>
#include <string>

class GL33_Shader final {
public:
  GL33_Shader()=default;

  //retourne 0 pour pas d'erreur et -1 pour erreur
  int GL33_Create(const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize);

  ~GL33_Shader();

  uint64_t GetId() const;

  void Use();

  void SetInt(const std::string &name, int value);
  void SetUint(const std::string &name, uint32_t value);
  void SetFloat(const std::string &name, float value);
  void SetVec2(const std::string &name, const glm::vec2 &value);
  void SetVec3(const std::string &name, const glm::vec3 &value);
  void SetVec4(const std::string &name, const glm::vec4 &value);
  void SetMat4(const std::string &name, const glm::mat4 &value);

private:
  //optimisation pour éviter de glGetUniformLocation chaque fois
  GLint FindUniformLocation(const std::string& name);
  std::unordered_map<std::string, GLint> uniform_locations_;

  //id backend opengl
  uint64_t id;
};

#endif