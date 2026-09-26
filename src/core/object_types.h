#ifndef HRL_OBJECT_TYPES
#define HRL_OBJECT_TYPES

#include "../hrl.h"

#include <optional>
#include <string>
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
struct HRL_MeshLODData {
  std::vector<HRL_Vertex3D> vertices;
  std::vector<HRL_uint> indices;
};

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

  // Number of triangles represented by the original mesh (LOD 0).
  size_t triangle_count_ = 0;

  // Automatic LOD configuration/state.
  std::vector<HRL_MeshLODData> lods_;
  bool lod_automatic_ = false;
  HRL_ELODMode lod_mode_ = HRL_LOD_SCREEN_SIZE;
  HRL_uint lod_levels_ = 4;
  float lod_base_distance_ = 10.f;
  float lod_distance_scale_ = 2.f;
  float lod_min_distance_ = 0.f;
  float lod_max_distance_ = 1000000.f;
  float lod_screen_threshold_ = 0.25f;
  float lod_screen_scale_ = 0.5f;
  float lod_hysteresis_ = 0.05f;
  int lod_override_ = -1;
  int last_lod_level_ = 0;

  // Conservative local-space bounding sphere used by the OpenGL CPU culler.
  glm::vec3 bounds_center_{0.f};
  float bounds_radius_ = 0.f;

  //Sprites are ordinary HRL_Mesh objects whose geometry is a 3D plane.
  //The region is only a texture-coordinate transform used by the sprite path.
  float region_[4] = {0.f, 0.f, 1.f, 1.f};
};

struct HRL_SkeletalBoneInternal {
  HRL_SkeletalBone public_;
  std::string name_storage_;
  HRL_SkeletalBoneTransform bind_local_{};
  glm::mat4 inverse_bind_{1.f};
  glm::mat4 bind_world_{1.f};
};

struct HRL_SkeletalAnimationInternal {
  HRL_SkeletalAnimation public_{};
  std::string name_storage_;
  std::vector<HRL_SkeletalBoneTransform> frames_;
  std::vector<glm::mat4> pose_frames_; // Final bone skin matrices sampled from ufbx evaluation.
  std::vector<float> pose_matrices_storage_; // Public poseMatrices backing storage.
};

struct HRL_SkeletalMesh final : HRL_Mesh {
  std::vector<HRL_SkeletalVertex> vertices_;
  std::vector<HRL_uint> indices_;
  std::vector<HRL_SkeletalBoneInternal> bones_;
  std::vector<HRL_SkeletalAnimationInternal> animations_;
  std::vector<glm::mat4> bone_matrices_;

  int current_animation_ = -1;
  float animation_time_ = 0.f;
  float animation_speed_ = 1.f;
  bool animation_loop_ = true;
  bool animation_playing_ = false;
  uint64_t pose_serial_ = 1;

  // Geometry transform from the imported FBX mesh node. Kept separate from
  // the user-controlled HRL object transform and applied in the renderer
  // model matrix so the skin matrices can remain mesh-local.
  glm::mat4 fbx_geometry_to_world_{1.f};

  void RefreshPublicPointers() {
    for (auto &bone : bones_) {
      bone.public_.name = bone.name_storage_.c_str();
    }
    for (auto &anim : animations_) {
      anim.public_.name = anim.name_storage_.c_str();
      anim.public_.frames = anim.frames_.empty() ? nullptr : anim.frames_.data();
      anim.public_.poseMatrices = anim.pose_matrices_storage_.empty() ? nullptr : anim.pose_matrices_storage_.data();
    }
  }
};

//LIGHT
typedef struct {
  HRL_id scene_ = HRL_INVALID_ID;
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

  // Shadow settings. GPU shadow resources remain private to the backend.
  HRL_id id_ = HRL_INVALID_ID;
  bool cast_shadows_ = false;
  float shadow_bias_ = 0.0015f;
  int shadow_resolution_ = 2048;
}HRL_Light;

//POST PROCESS
typedef struct {
  HRL_id id_ = HRL_INVALID_ID;
  HRL_id material_ = HRL_INVALID_ID;
  int priority_ = 0;
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

  // Textures created internally by HRL_CreateMaterialFromFBX*. Manual
  // HRL_MaterialSetTexture() bindings remain non-owning for compatibility.
  std::vector<HRL_id> owned_textures_;
}HRL_Material;

//CAMERA
typedef struct {
  HRL_uint type_;

  glm::vec3 position_;
  glm::vec3 rotation_;

  float value_;
  float near_plane_;
  float far_plane_;

  HRL_id scene_ = HRL_INVALID_ID;
}HRL_Camera;

//VIEWPORT
typedef struct {
  HRL_Camera* camera_;

  float x_;
  float y_;
  float width_;
  float height_;
  HRL_id scene_ = HRL_INVALID_ID;

  //Indexed by stable post-process ID. Priority is stored inside HRL_PostProcess.
  std::unordered_map<HRL_id, HRL_PostProcess*> post_processes;

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

// Localized volumetric fog is a regular scene object with its own stable HRL ID.
struct HRL_VolumetricFog final {
  HRL_id id_ = HRL_INVALID_ID;
  HRL_id scene_ = HRL_INVALID_ID;

  bool enabled = false;
  glm::vec3 position = glm::vec3(0.0f);
  float radius = 10.0f;
  glm::vec3 color = glm::vec3(0.65f, 0.72f, 0.80f);
  float density = 0.5f;
  HRL_uint steps = 16;
};

// Scene-wide volumetric fog. This is intentionally not an object because it
// has no position/radius and affects the entire scene uniformly.
typedef struct {
  bool enabled = false;
  glm::vec3 color = glm::vec3(0.65f, 0.72f, 0.80f);
  float density = 0.05f;
  HRL_uint steps = 16;
}hrl_global_volumetric_fog_t;

// Screen-space radial light scattering / god rays. The position is the
// world-space location of the light source used for the radial origin.
typedef struct {
  bool enabled = false;
  glm::vec3 position = glm::vec3(0.0f, 5.0f, 0.0f);
  glm::vec3 color = glm::vec3(1.0f);
  float density = 0.8f;
  float decay = 0.95f;
  float weight = 0.2f;
  HRL_uint samples = 48;
}hrl_god_rays_t;


//Widget
typedef struct {
  glm::vec2 position;
  glm::vec2 size;
}hrl_widget_t;


//objects//
typedef struct {
  int draw_on_screen;

  //color picking
  bool using_color_picking = true;

  //objects
  std::unordered_map<HRL_id, HRL_Mesh*> meshes;
  std::unordered_map<HRL_id, HRL_Light*> lights;
  std::unordered_map<HRL_id, HRL_VolumetricFog*> volumetric_fogs;

  //Per-scene diagnostic rendering state.
  HRL_EDebugView debug_view = HRL_DEBUG_VIEW_NONE;

  // Procedural sky sphere. Disabled by default to preserve existing scenes.
  bool sky_sphere_enabled = false;
  glm::vec3 sky_top_color = glm::vec3(0.08f, 0.24f, 0.65f);
  glm::vec3 sky_horizon_color = glm::vec3(0.55f, 0.72f, 0.95f);
  glm::vec3 sky_bottom_color = glm::vec3(0.08f, 0.10f, 0.16f);
  glm::vec3 sky_rotation = glm::vec3(0.f);
  HRL_id sky_texture = HRL_INVALID_ID;

  // Equirectangular environment used by reflective 3D materials.
  bool environment_mapping_enabled = false;
  HRL_id environment_texture = HRL_INVALID_ID;

  std::unordered_map<HRL_id, HRL_Viewport*> viewports;
  std::unordered_map<HRL_id, HRL_Camera*> cameras;

  //Effects
  hrl_fog_t fog;
  hrl_global_volumetric_fog_t global_volumetric_fog;
  hrl_god_rays_t god_rays;

  // Global illumination. Opt-in only; default keeps the existing renderer untouched.
  bool global_illumination_enabled = false;
  HRL_EGlobalIlluminationMethod global_illumination_method = HRL_GI_DDGI;

  // Shadow maps are static until geometry or a shadow-relevant light changes.
  bool shadows_dirty = true;

  // Explicit DDGI invalidation revisions. This avoids hashing/scanning the full
  // scene every frame just to detect static changes.
  uint64_t gi_geometry_revision = 1;
  uint64_t gi_lighting_revision = 1;
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
  std::unordered_map<HRL_id, HRL_VolumetricFog*> volumetric_fogs;
  std::unordered_map<HRL_id, HRL_Viewport*> viewports;
  std::unordered_map<HRL_id, HRL_Camera*> cameras;
  std::unordered_map<HRL_id, HRL_PostProcess*> post_processes;
  std::unordered_map<HRL_id, HRL_Widget*> widgets;

  //global ressources
  std::unordered_map<HRL_id, HRL_Material*> materials;
  std::unordered_map<HRL_id, HRL_Font*> fonts;

  // Screenshot requests are consumed after the corresponding scene finishes
  // rendering in HRL_EndFrame().
  std::unordered_map<HRL_id, std::string> pending_screenshots;

  //debug
  //sorted by scenes
  std::unordered_map<HRL_id, DebugRenderer> debug_renderers;
  float debug_line_thickness = 1.f;


  //HUD / widget input
  float mouseX = 0.0f;
  float mouseY = 0.0f;
  bool mouseLeftDown = false;
  bool mouseLeftPressed = false;
  bool mouseLeftReleased = false;
  HRL_id mouseCaptureWidget = HRL_INVALID_ID;
}HRL_Context;


#endif