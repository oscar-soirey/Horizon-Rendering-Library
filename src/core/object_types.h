#ifndef HRL_OBJECT_TYPES
#define HRL_OBJECT_TYPES

#include "../hrl.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <algorithm>

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

  // User-owned opaque pointer associated with this mesh.
  // HRL never allocates, frees, or otherwise manages this pointer.
  void* user_handle_=nullptr;

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

// Heightmap-driven landscape. The renderer samples the red channel of the
// referenced OpenGL texture and generates a regular grid lazily on the GPU.
struct HRL_Landscape final {
  HRL_id id_ = HRL_INVALID_ID;
  HRL_id scene_ = HRL_INVALID_ID;
  HRL_id heightmap_ = HRL_INVALID_ID;
  HRL_id material_ = HRL_INVALID_ID;

  glm::vec3 position_{0.f};
  glm::vec3 rotation_{0.f};
  glm::vec3 scale_{1.f};

  // Width/depth in local world units. The grid is centered around the origin.
  glm::vec2 size_{100.f, 100.f};
  float height_scale_ = 20.f;

  // Number of cells. The generated vertex count is (x+1)*(z+1).
  HRL_uint resolution_x_ = 256;
  HRL_uint resolution_z_ = 256;
  glm::vec2 uv_scale_{1.f, 1.f};

  // Incremented whenever the CPU height samples or topology have to be rebuilt.
  uint64_t revision_ = 1;
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
  float shadow_strength_ = 1.0f; // 0 = no shadow darkening, 1 = fully dark shadows
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

  // User-owned opaque pointer associated with this material.
  // HRL never allocates, frees, or dereferences this pointer.
  void* user_handle_ = nullptr;
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

  // User-owned opaque pointer associated with this camera.
  // HRL never allocates, frees, or dereferences this pointer.
  void* user_handle_ = nullptr;
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

struct HRL_Decal final {
  HRL_id id_ = HRL_INVALID_ID;
  HRL_id scene_ = HRL_INVALID_ID;
  bool enabled = true;
  glm::vec3 position = glm::vec3(0.0f);
  glm::vec3 rotation = glm::vec3(0.0f);
  glm::vec3 size = glm::vec3(2.0f, 1.0f, 2.0f);
  HRL_id texture = HRL_INVALID_ID;
  glm::vec3 color = glm::vec3(1.0f);
  float opacity = 1.0f;
  float normal_fade_min = 0.15f;
  float normal_fade_max = 0.65f;
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


// VFX / particle system
struct HRL_VFXFloatKey {
  float time = 0.f;
  float value = 0.f;
};

struct HRL_VFXColorKey {
  float time = 0.f;
  glm::vec4 value = glm::vec4(1.f);
};

struct HRL_VFXCurve {
  HRL_id id_ = HRL_INVALID_ID;
  HRL_id emitter_ = HRL_INVALID_ID;
  bool color_ = false;
  std::vector<HRL_VFXFloatKey> float_keys_;
  std::vector<HRL_VFXColorKey> color_keys_;
};

struct HRL_VFXBurst {
  float time = 0.f;
  HRL_uint count = 0;
};

struct HRL_VFXParticle {
  glm::vec3 position{0.f};
  glm::vec3 velocity{0.f};
  glm::vec3 rotation{0.f};
  glm::vec3 base_rotation{0.f};
  glm::vec3 angular_velocity{0.f};
  glm::vec2 size{1.f};
  glm::vec4 color{1.f};
  float age = 0.f;
  float lifetime = 1.f;
  uint32_t seed = 1u;
};

struct HRL_VFXEmitter {
  HRL_id id_ = HRL_INVALID_ID;
  HRL_id system_ = HRL_INVALID_ID;
  bool enabled_ = true;

  glm::vec3 position_{0.f};
  glm::vec3 rotation_{0.f};

  HRL_uint max_particles_ = 4096;
  float spawn_rate_ = 20.f;
  float spawn_accumulator_ = 0.f;
  uint64_t spawned_total_ = 0;
  uint32_t random_seed_ = 0x13579BDFu;

  float min_lifetime_ = 1.f;
  float max_lifetime_ = 1.f;

  HRL_EVFXSpawnShape spawn_shape_ = HRL_VFX_SHAPE_POINT;
  float shape_radius_ = 1.f;
  glm::vec3 shape_size_{1.f};
  float shape_angle_degrees_ = 25.f;

  glm::vec3 min_initial_velocity_{0.f};
  glm::vec3 max_initial_velocity_{0.f};
  float min_initial_speed_ = 0.f;
  float max_initial_speed_ = 0.f;
  glm::vec3 min_initial_rotation_{0.f};
  glm::vec3 max_initial_rotation_{0.f};
  glm::vec3 min_angular_velocity_{0.f};
  glm::vec3 max_angular_velocity_{0.f};

  glm::vec3 gravity_{0.f};
  float drag_ = 0.f;
  glm::vec3 force_{0.f};
  float noise_strength_ = 0.f;
  float noise_frequency_ = 0.5f;
  float noise_scroll_speed_ = 1.f;

  HRL_EVFXRenderMode render_mode_ = HRL_VFX_RENDER_BILLBOARD;
  HRL_EVFXBlendMode blend_mode_ = HRL_VFX_BLEND_ALPHA;
  HRL_EVFXSimulationSpace simulation_space_ = HRL_VFX_SIMULATION_LOCAL;
  HRL_id texture_ = HRL_INVALID_ID;
  HRL_id material_ = HRL_INVALID_ID;
  HRL_id mesh_ = HRL_INVALID_ID;
  glm::vec3 mesh_scale_{1.f};
  glm::vec3 mesh_rotation_offset_{0.f};
  glm::vec2 particle_size_{0.1f, 0.1f};
  float stretch_ = 0.f;

  HRL_id color_curve_ = HRL_INVALID_ID;
  HRL_id size_curve_ = HRL_INVALID_ID;
  HRL_id rotation_curve_ = HRL_INVALID_ID;

  bool collision_enabled_ = false;
  float collision_restitution_ = 0.2f;
  float collision_friction_ = 0.25f;
  HRL_id collision_scene_ = HRL_INVALID_ID;

  std::vector<HRL_VFXBurst> bursts_;
  size_t next_burst_ = 0;
  std::vector<HRL_VFXParticle> particles_;
};

struct HRL_VFXSystem {
  HRL_id id_ = HRL_INVALID_ID;
  HRL_id scene_ = HRL_INVALID_ID;
  bool enabled_ = true;
  bool playing_ = false;
  bool paused_ = false;
  bool looping_ = true;
  bool auto_update_ = true;
  float time_scale_ = 1.f;
  float duration_ = 0.f;
  float time_ = 0.f;
  glm::vec3 position_{0.f};
  glm::vec3 rotation_{0.f};
  glm::vec3 scale_{1.f};
  std::unordered_map<HRL_id, HRL_VFXEmitter*> emitters_;
};

//GIZMO
struct HRL_Gizmo {
  HRL_id id_ = HRL_INVALID_ID;
  HRL_id scene_ = HRL_INVALID_ID;
  HRL_id viewport_ = HRL_INVALID_ID;

  glm::vec3 position_{0.f};
  glm::vec3 rotation_{0.f};
  glm::vec3 scale_{1.f};

  HRL_EGizmoMode mode_ = HRL_GIZMO_MODE_TRANSLATE;
  HRL_EGizmoSpace space_ = HRL_GIZMO_SPACE_WORLD;

  bool visible_ = true;
  bool enabled_ = true;
  bool show_translate_ = true;
  bool show_rotate_ = true;
  bool show_scale_ = true;
  int translate_axes_ = HRL_GIZMO_AXIS_X | HRL_GIZMO_AXIS_Y | HRL_GIZMO_AXIS_Z;
  int rotate_axes_ = HRL_GIZMO_AXIS_X | HRL_GIZMO_AXIS_Y | HRL_GIZMO_AXIS_Z;
  int scale_axes_ = HRL_GIZMO_AXIS_X | HRL_GIZMO_AXIS_Y | HRL_GIZMO_AXIS_Z;

  float world_size_ = 1.f;
  float screen_size_pixels_ = 96.f;
  bool use_screen_size_ = true;

  glm::vec4 axis_colors_[3] = {
    glm::vec4(1.f, 0.2f, 0.2f, 1.f),
    glm::vec4(0.25f, 1.f, 0.25f, 1.f),
    glm::vec4(0.3f, 0.55f, 1.f, 1.f)
  };
  glm::vec4 center_color_{1.f, 0.85f, 0.2f, 1.f};
  glm::vec4 hover_color_{1.f, 0.95f, 0.35f, 1.f};
  float rotate_arc_degrees_ = 90.f;

  HRL_EGizmoPart hovered_part_ = HRL_GIZMO_PART_NONE;
  HRL_EGizmoPart active_part_ = HRL_GIZMO_PART_NONE;
  HRL_EGizmoOperation hovered_operation_ = HRL_GIZMO_OPERATION_NONE;
  HRL_EGizmoOperation active_operation_ = HRL_GIZMO_OPERATION_NONE;
  bool dragging_ = false;

  glm::vec3 drag_start_position_{0.f};
  glm::vec3 drag_start_rotation_{0.f};
  glm::vec3 drag_start_scale_{1.f};
  glm::vec3 drag_start_axis_{1.f, 0.f, 0.f};
  glm::vec3 drag_start_vector_{1.f, 0.f, 0.f};
  glm::vec3 drag_plane_normal_{0.f, 1.f, 0.f};
  float drag_start_axis_value_ = 0.f;
  float drag_start_angle_ = 0.f;
  float drag_start_radius_ = 0.f;
  HRL_id drag_viewport_ = HRL_INVALID_ID;

  HRL_CGizmoChanged changed_callback_ = nullptr;
  void* changed_user_data_ = nullptr;
};

//Widget
typedef struct {
  glm::vec2 position;
  glm::vec2 size;
}hrl_widget_t;


// Screen-space debug messages (Unreal-style stacked on-screen messages).
struct HRL_ScreenMessage
{
  HRL_id id = HRL_INVALID_ID;
  std::string text;
  float remaining_seconds = 0.0f;
  float size = 16.0f;
  glm::vec4 color = glm::vec4(1.0f);
  HRL_id font = HRL_INVALID_ID;
};

// Internal 2D voxel-world storage.
  //
  // Voxel data is sparse at chunk granularity: an absent chunk is implicitly
  // filled with type 0. This is important for large mostly-empty worlds:
  // dimensions no longer imply a width*height allocation.
struct HRL_VoxelWorld final {
  using VoxelChunk = std::vector<HRL_Voxel>;

  int width_ = 0;
  int height_ = 0;
  int chunk_size_ = 32;
  float voxel_size_ = 1.0f;

  std::unordered_map<uint64_t, VoxelChunk> voxel_chunks_;

  static uint64_t MakeVoxelChunkKey(int chunkX, int chunkY)
  {
    return (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32u) |
      static_cast<uint64_t>(static_cast<uint32_t>(chunkY));
  }

  const VoxelChunk* FindVoxelChunk(int chunkX, int chunkY) const
  {
    const auto it = voxel_chunks_.find(MakeVoxelChunkKey(chunkX, chunkY));
    return it != voxel_chunks_.end() ? &it->second : nullptr;
  }

  VoxelChunk* FindVoxelChunk(int chunkX, int chunkY)
  {
    auto it = voxel_chunks_.find(MakeVoxelChunkKey(chunkX, chunkY));
    return it != voxel_chunks_.end() ? &it->second : nullptr;
  }

  VoxelChunk& EnsureVoxelChunk(int chunkX, int chunkY)
  {
    const uint64_t key = MakeVoxelChunkKey(chunkX, chunkY);
    auto [it, inserted] = voxel_chunks_.try_emplace(key);
    if (inserted)
    {
      const int minX = chunkX * chunk_size_;
      const int minY = chunkY * chunk_size_;
      const int chunkW = std::min(chunk_size_, width_ - minX);
      const int chunkH = std::min(chunk_size_, height_ - minY);
      it->second.assign(
        static_cast<size_t>(std::max(0, chunkW)) * static_cast<size_t>(std::max(0, chunkH)),
        HRL_Voxel{0});
    }
    return it->second;
  }

  HRL_VoxelType GetVoxelType(int x, int y) const
  {
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
      return 0;

    const int chunkX = x / chunk_size_;
    const int chunkY = y / chunk_size_;
    const VoxelChunk* chunk = FindVoxelChunk(chunkX, chunkY);
    if (!chunk)
      return 0;

    const int localX = x - chunkX * chunk_size_;
    const int localY = y - chunkY * chunk_size_;
    const int chunkW = std::min(chunk_size_, width_ - chunkX * chunk_size_);
    const size_t index = static_cast<size_t>(localY) * static_cast<size_t>(chunkW) +
      static_cast<size_t>(localX);
    return (*chunk)[index].type;
  }

  HRL_VoxelType* GetVoxelTypeMutable(int x, int y)
  {
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
      return nullptr;

    const int chunkX = x / chunk_size_;
    const int chunkY = y / chunk_size_;
    VoxelChunk& chunk = EnsureVoxelChunk(chunkX, chunkY);
    const int localX = x - chunkX * chunk_size_;
    const int localY = y - chunkY * chunk_size_;
    const int chunkW = std::min(chunk_size_, width_ - chunkX * chunk_size_);
    return &chunk[static_cast<size_t>(localY) * static_cast<size_t>(chunkW) +
      static_cast<size_t>(localX)].type;
  }

  std::unordered_map<HRL_VoxelType, glm::vec4> type_colors_;

  std::unordered_map<HRL_VoxelType, uint32_t> type_collision_flags_;

  // Sparse type-level emissive colors. Most voxel types have no entry, so
  // ordinary voxels pay no per-voxel memory cost for emission.
  std::unordered_map<HRL_VoxelType, glm::vec3> type_emissive_colors_;

  // Chunks whose baked topology no longer matches the CPU data.
  std::unordered_set<uint64_t> dirty_chunks_;

  // Chunks whose GPU representation needs rebuilding. This is deliberately
  // separate from dirty_chunks_: the latter must remain dirty until the
  // changes have actually been persisted to disk.
  std::unordered_set<uint64_t> render_dirty_chunks_;

  // Serialized chunk records retained after a lazy world load.  The payload
  // stays compressed in serialized_source_ until a chunk becomes visible or
  // is explicitly accessed. This keeps large worlds cheap to load.
  struct SerializedChunkRecord
  {
    uint8_t encoding = 0;
    uint8_t bits = 0;
    std::vector<uint8_t> palette;
    size_t payload_offset = 0;
    size_t payload_size = 0;
    bool deleted = false;
  };
  std::vector<uint8_t> serialized_source_;
  std::unordered_map<uint64_t, SerializedChunkRecord> serialized_chunks_;

  // Incremented whenever all chunk geometry must be regenerated (world load or
  // a geometry-affecting configuration change).
  uint64_t geometry_revision_ = 1;

  // Incremented when voxel contents or voxel-space transforms change. Unlike
  // geometry_revision_, this revision is only used to invalidate derived
  // voxel-light data and therefore does not force every visible chunk to rebuild.
  uint64_t voxel_revision_ = 1;

  // Voxel edits can be grouped so a brush stroke invalidates derived lighting
  // once instead of once per modified voxel.
  uint32_t voxel_edit_depth_ = 0;
  bool voxel_edit_dirty_ = false;

  // Incremented when a voxel type color changes. The OpenGL backend compares
  // this with each loaded chunk because colors are baked into chunk vertices.
  uint64_t color_revision_ = 1;
};

// Loads a serialized chunk on demand. Implemented in hrl.cpp and used by
// rendering backends so disk-loaded worlds stay lazy on the CPU side.
bool HRL_EnsureVoxelChunkLoaded(HRL_VoxelWorld* world, int chunkX, int chunkY);

//objects//
typedef struct {
  int draw_on_screen;

  //color picking
  bool using_color_picking = true;

  //objects
  std::unordered_map<HRL_id, HRL_Mesh*> meshes;

  // Optional 2D voxel world owned by this scene. The public API exposes only
  // HRL_Voxel; chunking and GPU resources remain backend/internal details.
  HRL_VoxelWorld* voxel_world = nullptr;
  std::unordered_map<HRL_id, HRL_Landscape*> landscapes;
  std::unordered_map<HRL_id, HRL_Light*> lights;
  std::unordered_map<HRL_id, HRL_VolumetricFog*> volumetric_fogs;
  std::unordered_map<HRL_id, HRL_Decal*> decals;
  std::unordered_map<HRL_id, HRL_Gizmo*> gizmos;
  std::unordered_map<HRL_id, HRL_VFXSystem*> vfx_systems;

  //Per-scene diagnostic rendering state.
  HRL_EDebugView debug_view = HRL_DEBUG_VIEW_NONE;

  // SDF mesh-info debug overlay settings.
  HRL_id debug_mesh_info_font = HRL_INVALID_ID;
  float debug_mesh_info_text_size = 14.0f;
  glm::vec4 debug_mesh_info_text_color = glm::vec4(1.0f, 0.95f, 0.25f, 1.0f);

  // Unreal-style on-screen debug messages. Settings affect only messages
  // created after the setting is changed.
  float screen_message_text_size = 16.0f;
  glm::vec4 screen_message_text_color = glm::vec4(1.0f);
  HRL_id screen_message_font = HRL_INVALID_ID;
  HRL_id next_screen_message_id = 1;
  std::vector<HRL_ScreenMessage> screen_messages;

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

  bool ambient_occlusion_enabled = false;
  float ambient_occlusion_strength = 0.65f;
  float ambient_occlusion_radius = 1.0f;
  float ambient_occlusion_bias = 0.03f;
  float ambient_occlusion_power = 1.4f;

  // Screen-space reflections. Disabled by default.
  bool screen_space_reflections_enabled = false;
  float screen_space_reflections_strength = 0.65f;
  float screen_space_reflections_max_distance = 80.0f;
  float screen_space_reflections_thickness = 0.25f;
  float screen_space_reflections_fade_start = 0.55f;
  float screen_space_reflections_fade_end = 1.0f;
  HRL_uint screen_space_reflections_steps = 48;

  // Volumetric cloud layer. OpenGL-only renderer feature; disabled by default.
  bool volumetric_cloud_enabled = false;
  float volumetric_cloud_coverage = 0.58f;
  float volumetric_cloud_density = 1.15f;
  float volumetric_cloud_height_min = 80.0f;
  float volumetric_cloud_height_max = 180.0f;
  float volumetric_cloud_scale = 0.0040f;
  float volumetric_cloud_detail = 0.65f;
  glm::vec2 volumetric_cloud_wind = glm::vec2(0.35f, 0.18f);
  float volumetric_cloud_wind_speed = 8.0f;
  glm::vec3 volumetric_cloud_color = glm::vec3(0.93f, 0.95f, 1.0f);
  glm::vec3 volumetric_cloud_light_color = glm::vec3(1.0f, 0.96f, 0.90f);
  float volumetric_cloud_light_absorption = 1.25f;
  float volumetric_cloud_light_intensity = 1.15f;
  HRL_uint volumetric_cloud_steps = 32;
  float volumetric_cloud_max_distance = 4000.0f;

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
  std::unordered_map<HRL_id, HRL_Landscape*> landscapes;
  std::unordered_map<HRL_id, HRL_Light*> lights;
  std::unordered_map<HRL_id, HRL_VolumetricFog*> volumetric_fogs;
  std::unordered_map<HRL_id, HRL_Decal*> decals;
  std::unordered_map<HRL_id, HRL_Gizmo*> gizmos;
  std::unordered_map<HRL_id, HRL_VFXSystem*> vfx_systems;
  std::unordered_map<HRL_id, HRL_VFXEmitter*> vfx_emitters;
  std::unordered_map<HRL_id, HRL_VFXCurve*> vfx_curves;
  std::unordered_map<HRL_id, HRL_Viewport*> viewports;
  std::unordered_map<HRL_id, HRL_Camera*> cameras;
  std::unordered_map<HRL_id, HRL_PostProcess*> post_processes;
  std::unordered_map<HRL_id, HRL_Widget*> widgets;

  //global ressources
  std::unordered_map<HRL_id, HRL_Material*> materials;
  std::unordered_map<HRL_id, HRL_Font*> fonts;

  // 0=pending, 1=ready, 2=failed, 3=cancelled.
  std::unordered_map<HRL_id, int> async_resource_states;

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
  HRL_id mouseCaptureGizmo = HRL_INVALID_ID;
  HRL_EGizmoPart mouseCaptureGizmoPart = HRL_GIZMO_PART_NONE;

  double vfx_last_frame_time = 0.0;
  bool vfx_has_frame_time = false;
}HRL_Context;


#endif