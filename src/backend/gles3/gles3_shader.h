#ifndef GLES3_SHADER
#define GLES3_SHADER

#include "../../core/backend_vtable.h"

#include <glm/glm.hpp>
#include "gles3_gl.h"

#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>

class GLES3_Shader final {
public:
  GLES3_Shader()=default;

  //retourne 0 pour pas d'erreur et -1 pour erreur
  int GLES3_Create(const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize);

  ~GLES3_Shader();

  uint64_t GetId() const;

  void Use();

  // Les surcharges const char* evitent la construction d'une std::string
  // temporaire (allocation au-dela de 15 caracteres) a chaque appel.
  void SetInt(const char* name, int value);
  void SetUint(const char* name, uint32_t value);
  void SetFloat(const char* name, float value);
  void SetVec2(const char* name, const glm::vec2 &value);
  void SetVec3(const char* name, const glm::vec3 &value);
  void SetVec4(const char* name, const glm::vec4 &value);
  void SetMat3(const char* name, const glm::mat3 &value);
  void SetMat4(const char* name, const glm::mat4 &value);

  void SetInt(const std::string &name, int value) { SetInt(name.c_str(), value); }
  void SetUint(const std::string &name, uint32_t value) { SetUint(name.c_str(), value); }
  void SetFloat(const std::string &name, float value) { SetFloat(name.c_str(), value); }
  void SetVec2(const std::string &name, const glm::vec2 &value) { SetVec2(name.c_str(), value); }
  void SetVec3(const std::string &name, const glm::vec3 &value) { SetVec3(name.c_str(), value); }
  void SetVec4(const std::string &name, const glm::vec4 &value) { SetVec4(name.c_str(), value); }
  void SetMat3(const std::string &name, const glm::mat3 &value) { SetMat3(name.c_str(), value); }
  void SetMat4(const std::string &name, const glm::mat4 &value) { SetMat4(name.c_str(), value); }

  /**
   * Invalide le cache des valeurs d'uniformes de tous les shaders.
   * Le renderer l'appelle au debut de chaque scene : si l'application modifie
   * un uniform directement via glUniform* (programme obtenu avec
   * HRL_GL_GetShaderGL_ID), la valeur HRL est renvoyee au plus tard a la
   * scene suivante.
   */
  static void InvalidateAllValueCaches();

private:
  struct UniformSlot {
    GLint location = -1;
    uint32_t generation = 0; // 0 = aucune valeur connue
    float data[16] = {};
  };

  //optimisation pour éviter de glGetUniformLocation chaque fois
  UniformSlot* FindUniformSlot(const char* name);
  // Retourne true si la valeur doit etre envoyee a OpenGL (et met le cache a jour).
  bool NeedsUpload(UniformSlot* slot, const void* value, size_t size);

  std::unordered_map<std::string, uint32_t> uniform_indices_;
  std::vector<UniformSlot> uniform_slots_;
  std::string lookup_key_; // tampon reutilise : pas d'allocation pour les recherches

  static uint32_t s_value_generation_;

  //id backend opengl
  uint64_t id = 0;
};

#endif
