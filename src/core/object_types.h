#ifndef HRL_OBJECT_TYPES
#define HRL_OBJECT_TYPES

#include "../hrl.h"
#include <unordered_map>
#include <glm/glm.hpp>
#include <vector>

typedef uint64_t HRL_BackendHandle;

//MESH
typedef struct {
  HRL_uint type_;

  HRL_id material_;

  float draw_order_;

  glm::vec3 position_;
  glm::vec3 rotation_;
  glm::vec3 scale_;
}HRL_Mesh;

//LIGHT
typedef struct {
  HRL_uint type_;
  float intensity_;
  float attenuation_;
  /**
   * On utilise des vec4 pour eviter de poser des problemes de paddings pour certains backends
   */
  glm::vec3 position_;
  glm::vec3 rotation_;

  glm::vec3 color_;
}HRL_Light;

//POST PROCESS
typedef struct {
  HRL_id material_;
}HRL_PostProcess;

//MATERIAL
typedef struct {
  HRL_id shader_;

  std::unordered_map<std::string, int> intParams_;
  std::unordered_map<std::string, HRL_id> textureParams_;
  std::unordered_map<std::string, float> floatParams_;
  std::unordered_map<std::string, glm::vec2> vec2Params_;
  std::unordered_map<std::string, glm::vec3> vec3Params_;
  std::unordered_map<std::string, glm::vec4> vec4Params_;
}HRL_Material;

//CAMERA
typedef struct {
  HRL_uint type_;

  glm::vec3 position_;
  glm::vec3 rotation_;

  float value_;
  float near_plane_;
  float far_plane_;
}HRL_Camera;

//VIEWPORT
typedef struct {
  HRL_Camera* camera_;

  float x_;
  float y_;
  float width_;
  float height_;
}HRL_Viewport;


//Debug//

typedef struct {
  float x, y, z;
  float r, g, b;
}DebugVertex;

struct DebugRenderer {
  //for example : lines = [ A, B, C, D, E, F ], it will draw lines : A->B   C->D   E->F
  //manage grouping at the draw moment (opengl is automatic for example)
  std::vector<DebugVertex> lines;
  //every vertex of triangles, each traingles grouped by 3 vertices
  std::vector<DebugVertex> triangles;
};

#endif