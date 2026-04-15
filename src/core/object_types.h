#ifndef HRL_OBJECT_TYPES
#define HRL_OBJECT_TYPES

#include "../hrl.h"

#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <stb/stb_truetype.h>

typedef uint64_t HRL_BackendHandle;

class HRL_Widget;

//Errors
typedef struct {
  HRL_EError code;
  HRL_ESeverity severity;
  std::string detail;
}HRL_Internal_Error;

//MESH
struct HRL_Mesh {
  virtual ~HRL_Mesh()=default;

  HRL_id scene_;

  HRL_EMeshType type_;

  HRL_id material_=HRL_INVALID_ID;

  float draw_order_=0.f;

  glm::vec3 position_{0.f};
  glm::vec3 rotation_{0.f};
  glm::vec3 scale_{1.f};

  glm::vec3 pivot_point_{0.f};
};

struct HRL_MeshSprite : HRL_Mesh {
  //uMin, vMin, uMax, vMax
  float region_[4] = {0.f, 0.f, 1.f, 1.f};
};

//LIGHT
typedef struct {
  HRL_uint type_;
  float intensity_=2.f;
  float attenuation_=0.02f;

  //cos(angle intérieur)
  float innerCutoff=32.f;
  //cos(angle extérieur)
  float outerCutoff=40.f;

  /**
   * On utilise des vec4 pour eviter de poser des problemes de paddings pour certains backends
   */
  glm::vec3 position_{0.f};
  glm::vec3 rotation_{0.f};

  glm::vec3 color_{1.f};
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

  //ordered by priority
  std::map<int, HRL_PostProcess*> post_processes;

  std::unordered_map<HRL_id, HRL_Widget*> widgets;
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


//usefull for text rendering
struct BitmapResult {
  std::vector<unsigned char> pixels; // RGBA
  int width;
  int height;
};



//texte - structure backend API only, pas besoin d'y acceder avec le backend
typedef struct {
  stbtt_fontinfo             info;
  std::vector<unsigned char> ttf_buffer;
}HRL_Font;

//Fog
typedef struct {
  //mode
  bool enabled = false;
  HRL_uint mode = HRL_FOG_LINEAR;
  //color
  float r = 0.5f;
  float g = 0.5f;
  float b = 0.5f;
  //rendering
  float density = 0.1f;
  float range_start = 20.f;
  float range_end = 100.f;
}hrl_fog_t;


//Widget
typedef struct {
  glm::vec2 position;
  glm::vec2 size;
}hrl_widget_t;


//objects//
typedef struct {
  int draw_on_screen;

  //objects
  std::unordered_map<HRL_id, HRL_Mesh*> meshes;
  std::unordered_map<HRL_id, HRL_Light*> lights;
  std::unordered_map<HRL_id, HRL_Viewport*> viewports;
  std::unordered_map<HRL_id, HRL_Camera*> cameras;

  //Effects
  hrl_fog_t fog;
}hrl_scene_t;


typedef struct {
  //errors
  HRL_Internal_Error last_error;
  HRL_CErrorCallback error_callback;

  //window dimensions
  uint32_t window_width;
  uint32_t window_height;

  //scenes
  std::unordered_map<HRL_id, hrl_scene_t*> scenes;

  //ressources copié des scenes (pour favoriser l'acces)
  std::unordered_map<HRL_id, HRL_Mesh*> meshes;
  std::unordered_map<HRL_id, HRL_Light*> lights;
  std::unordered_map<HRL_id, HRL_Viewport*> viewports;
  std::unordered_map<HRL_id, HRL_Camera*> cameras;
  std::unordered_map<HRL_id, HRL_PostProcess*> post_processes;
  std::unordered_map<HRL_id, HRL_Widget*> widgets;

  //global ressources
  std::unordered_map<HRL_id, HRL_Material*> materials;
  std::unordered_map<HRL_id, HRL_Font*> fonts;

  //debug
  //sorted by scenes
  std::unordered_map<HRL_id, DebugRenderer> debug_renderers;
  float debug_line_thickness = 1.f;


  //HUD
  float mouseX;
  float mouseY;
}HRL_Context;


#endif