/**
 * Horizon Rendering Library - Official C++ API
 *
 * Copyright (c) 2025-2026 Oscar Soirey
 * https://github.com/oscar-soirey/Horizon-Rendering-Library
 *
 * This project was developed by a single passionate developer.
 * I've tried to make everything work smoothly, but there may still be bugs.
 * If you encounter any issues or have suggestions, please feel free to contact me at:
 * oscarsoirey.contact@gmail.com
 * Thank you for your support and understanding.
 *
 * This code is the intellectual property of Oscar Soirey and is
 * licensed under the Apache License, Version 2.0. You may not use,
 * modify, or distribute this software except in compliance with the
 * License. A copy of the License can be obtained at:
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * By using or modifying this code, you agree to adhere to the terms
 * of the Apache 2.0 License.
 *
 * This header contains the complete public C++ interface and has no dependency
 * on the C API header. The implementation in hrl.cpp is the compatibility
 * layer that delegates to the C API.
 */
#ifndef HRL_HPP
#define HRL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#if defined(_WIN32)
    #if defined(HRL_BUILD_CPP_DLL)
        #define HRL_CPP_API __declspec(dllexport)
    #elif defined(HRL_NO_DLL)
        #define HRL_CPP_API
    #else
        #define HRL_CPP_API __declspec(dllimport)
    #endif
#else
    #define HRL_CPP_API
#endif

namespace hrl {

/**
 * @brief Public HRL object identifier type.
 */
using id = std::uint32_t;

/**
 * @brief Public HRL unsigned integer type.
 */
using uint = std::uint32_t;

/**
 * @brief HRL version exposed by the C API.
 */
inline constexpr const char* API_VERSION = "0.6";

/**
 * @brief Invalid object identifier.
 */
inline constexpr id INVALID_ID = static_cast<id>(-1);

/**
 * @brief Maximum number of bone influences stored per skeletal vertex.
 */
inline constexpr uint SKELETAL_MAX_INFLUENCES = 4;

/**
 * @brief Maximum number of bones supported by the OpenGL 3.3 skeletal renderer.
 */
inline constexpr uint MAX_SKELETAL_BONES = 128;

/**
 * @brief Maximum number of localized volumetric fog objects rendered per scene.
 */
inline constexpr uint MAX_VOLUMETRIC_FOGS = 64;

/**
 * @brief Reserved ID for the built-in sprite shader.
 */
inline constexpr id SPRITE_SHADER = static_cast<id>(0xFFFFFFFFu);

/**
 * @brief Reserved ID for the built-in 2D mesh shader.
 */
inline constexpr id MESH_2D_SHADER = static_cast<id>(0xFFFFFFFEu);

/**
 * @brief Reserved ID for the built-in 3D mesh shader.
 */
inline constexpr id MESH_3D_SHADER = static_cast<id>(0xFFFFFFFDu);

/**
 * @brief Reserved ID for the built-in debug shader.
 */
inline constexpr id DEBUG_SHADER = static_cast<id>(0xFFFFFFFCu);

/**
 * @brief Reserved ID for the built-in default post-process shader.
 */
inline constexpr id DEFAULT_POST_PROCESS_SHADER = static_cast<id>(0xFFFFFFFBu);

/**
 * @brief Reserved ID for the built-in skinned 3D mesh shader.
 */
inline constexpr id SKINNED_3D_MESH_SHADER = static_cast<id>(0xFFFFFFFAu);

/**
 * @brief Integer value used for false in the C-compatible API.
 */
inline constexpr int FALSE_VALUE = 0;

/**
 * @brief Integer value used for true in the C-compatible API.
 */
inline constexpr int TRUE_VALUE = 1;

/**
 * @brief Built-in albedo texture semantic name.
 */
inline constexpr const char* T_ALBEDO = "T_Albedo";

/**
 * @brief Built-in normal texture semantic name.
 */
inline constexpr const char* T_NORMAL = "T_Normal";

/**
 * @brief Built-in specular texture semantic name.
 */
inline constexpr const char* T_SPECULAR = "T_Specular";

/**
 * @brief Built-in roughness texture semantic name.
 */
inline constexpr const char* T_ROUGHNESS = "T_Roughness";

/**
 * @brief Built-in metallic texture semantic name.
 */
inline constexpr const char* T_METALLIC = "T_Metallic";

/**
 * @brief Built-in ambient-occlusion texture semantic name.
 */
inline constexpr const char* T_AMBIENT_OCCLUSION = "T_AO";

/**
 * @brief Built-in alpha texture semantic name.
 */
inline constexpr const char* T_ALPHA = "T_Alpha";

/**
 * @brief Built-in emissive texture semantic name.
 */
inline constexpr const char* T_EMISSIVE = "T_Emissive";

/**
 * @brief Built-in shadow-map texture semantic name.
 */
inline constexpr const char* T_SHADOW_MAP = "T_ShadowMap";

/**
 * @brief Built-in cube-map texture semantic name.
 */
inline constexpr const char* T_CUBE_MAP = "T_CubeMap";

/**
 * @brief Material parameter name controlling two-sided rendering.
 *
 * Back-face culling is enabled by default; set this parameter to true for
 * a two-sided material.
 */
inline constexpr const char* MATERIAL_PARAM_TWO_SIDED = "TwoSided";

/**
 * @brief Material texture semantic used by screen-space displacement mapping.
 */
inline constexpr const char* MATERIAL_TEXTURE_SS_DISPLACEMENT_MAPPING =
    "SS_DISPLACEMENT_MAPPING";

/**
 * @brief Material parameter enabling screen-space displacement mapping.
 *
 * The feature is opt-in and defaults to disabled.
 */
inline constexpr const char* MATERIAL_PARAM_SS_DISPLACEMENT_ENABLED =
    "ss_displacement_enabled";

/**
 * @brief Material parameter controlling screen-space displacement strength.
 */
inline constexpr const char* MATERIAL_PARAM_SS_DISPLACEMENT_STRENGTH =
    "SSDisplacementStrength";

/**
 * @brief Material parameter controlling screen-space displacement scale.
 */
inline constexpr const char* MATERIAL_PARAM_SS_DISPLACEMENT_SCALE =
    "SSDisplacementScale";

/**
 * @brief Material parameter controlling screen-space displacement opacity.
 */
inline constexpr const char* MATERIAL_PARAM_SS_DISPLACEMENT_OPACITY =
    "SSDisplacementOpacity";

/**
 * @brief Vertex format used by static 3D meshes.
 *
 * All vectors are expressed in object/local space. UV coordinates use the same
 * convention as the sprite region API. Tangent and bitangent are used with the
 * normal map for the built-in OpenGL 3.3 shader.
 */
struct Vertex3D {
    /** @brief Object/local-space position. */
    float position[3];

    /** @brief Object/local-space normal. */
    float normal[3];

    /** @brief Texture coordinates. */
    float uv[2];

    /** @brief Tangent vector used by normal mapping. */
    float tangent[3];

    /** @brief Bitangent vector used by normal mapping. */
    float bitangent[3];
};

/**
 * @brief Vertex format used by skeletal meshes.
 *
 * The first five attributes share the Vertex3D layout, followed by four bone
 * indices and four normalized weights.
 */
struct SkeletalVertex {
    /** @brief Static vertex attributes for the skeletal vertex. */
    Vertex3D vertex;

    /** @brief Bone indices affecting this vertex. */
    uint boneIndices[SKELETAL_MAX_INFLUENCES];

    /** @brief Normalized influence weights associated with boneIndices. */
    float boneWeights[SKELETAL_MAX_INFLUENCES];
};

/**
 * @brief Local transform sampled for one skeletal bone at one animation frame.
 */
struct SkeletalBoneTransform {
    /** @brief Local-space translation. */
    float translation[3];

    /** @brief Rotation quaternion in x, y, z, w order. */
    float rotation[4];

    /** @brief Local-space scale. */
    float scale[3];
};

/**
 * @brief Public skeletal bone metadata.
 */
struct SkeletalBone {
    /** @brief Bone name. */
    const char* name;

    /** @brief Index of the parent bone. */
    uint parentIndex;

    /** @brief Local bind-pose transform used to reconstruct the bone hierarchy. */
    SkeletalBoneTransform bindTransform;

    /** @brief Evaluated bind-pose node-to-world matrix. */
    float bindWorldMatrix[16];

    /** @brief Inverse bind matrix used for skinning. */
    float inverseBindMatrix[16];
};

/**
 * @brief Baked animation data owned by SkeletalMeshData until freed.
 */
struct SkeletalAnimation {
    /** @brief Animation name. */
    const char* name;

    /** @brief Animation duration. */
    float duration;

    /** @brief Animation sampling frame rate. */
    float frameRate;

    /** @brief Number of sampled animation frames. */
    std::size_t frameCount;

    /** @brief Local bone transforms stored as frameCount * boneCount entries. */
    SkeletalBoneTransform* frames;

    /** @brief Optional evaluated skin matrices stored as frameCount * boneCount * 16 floats. */
    float* poseMatrices;
};

/**
 * @brief CPU-side skeletal mesh data produced by an importer.
 */
struct SkeletalMeshData {
    /** @brief Vertex buffer. */
    SkeletalVertex* vertices;

    /** @brief Number of vertices. */
    std::size_t vertexCount;

    /** @brief Index buffer. */
    uint* indices;

    /** @brief Number of indices. */
    std::size_t indexCount;

    /** @brief Skeletal bone array. */
    SkeletalBone* bones;

    /** @brief Number of bones. */
    std::size_t boneCount;

    /** @brief Skeletal animation array. */
    SkeletalAnimation* animations;

    /** @brief Number of animations. */
    std::size_t animationCount;

    /**
     * @brief Geometry-to-world transform of the FBX mesh instance.
     *
     * New data should set this to identity unless imported from FBX.
     */
    float geometryToWorldMatrix[16];
};

/**
 * @brief Supported graphics backend API.
 */
enum E_APIs {
    /** @brief OpenGL 3.3 backend. */
    HRL_OPENGL_33 = 0x0001,

    /** @brief OpenGL 4.5 backend. */
    HRL_OPENGL_45,

    /** @brief Vulkan backend. */
    HRL_VULKAN,

    /** @brief Direct3D 11 backend. */
    HRL_D3D11,

    /** @brief Direct3D 12 backend. */
    HRL_D3D12,

    /** @brief Metal backend. */
    HRL_METAL,

    /** @brief Nintendo NVN backend. */
    HRL_NVN,

    /** @brief PlayStation GNM backend. */
    HRL_GNM
};

/**
 * @brief Types of light supported by HRL.
 */
enum ELightType {
    /** @brief Point light. */
    HRL_POINT_LIGHT = 0x0011,

    /** @brief Directional light. */
    HRL_DIRECTIONAL_LIGHT,

    /** @brief Spot light. */
    HRL_SPOT_LIGHT,

    /**
     * @brief Low-intensity ambient light coming uniformly from all directions.
     *
     * This light does not cast shadows.
     */
    HRL_SKY_LIGHT
};

/**
 * @brief Types of mesh objects supported by HRL.
 */
enum EMeshType {
    /** @brief Camera-facing sprite mesh. */
    HRL_SPRITE = 0x0021,

    /** @brief 2D mesh. */
    HRL_2D_MESH,

    /** @brief Conventional non-skinned 3D mesh. */
    HRL_3D_MESH,

    /** @brief Skinned 3D mesh. */
    HRL_3D_SKELETAL_MESH
};

/**
 * @brief Debug geometry rendering modes.
 */
enum EDebugRenderingType {
    /** @brief Draw debug geometry as a hollow outline. */
    HRL_DEBUG_HOLLOW = 0x0031,

    /** @brief Draw debug geometry as solid geometry. */
    HRL_DEBUG_SOLID
};

/**
 * @brief Camera projection types.
 */
enum ECameraType {
    /** @brief Orthographic projection. */
    HRL_ORTHO = 0x0041,

    /** @brief Perspective projection. */
    HRL_PERSPECTIVE
};

/**
 * @brief Texture filtering modes.
 */
enum EFilterType {
    /** @brief Nearest-neighbour filtering. */
    HRL_FILTER_NEAREST = 0x0050,

    /** @brief Linear filtering. */
    HRL_FILTER_LINEAR,

    /** @brief Bilinear filtering. */
    HRL_FILTER_BILINEAR,

    /** @brief Trilinear filtering. */
    HRL_FILTER_TRILINEAR,

    /** @brief Anisotropic filtering. */
    HRL_FILTER_ANISOTROPIC,

    /** @brief Supersampling filtering. Not available with OpenGL backends. */
    HRL_FILTER_SUPERSAMPLING
};

/**
 * @brief Global illumination methods.
 *
 * Backend support in this release:
 *   - OpenGL 3.3: DDGI-style world-space irradiance probes
 *   - OpenGL 4.5: not implemented in this repository
 *   - Vulkan: not implemented in this repository
 *   - D3D11: not implemented in this repository
 *   - D3D12: not implemented in this repository
 *   - Metal: not implemented in this repository
 *   - NVN: not implemented in this repository
 *   - GNM: not implemented in this repository
 *
 * VCT, LPV, DDGI, path tracing and ray tracing are exposed now so the public
 * API does not need to change when future backends implement them.
 */
enum EGlobalIlluminationMethod {
    /** @brief Disable global illumination. */
    HRL_GI_NONE = 0,

    /** @brief Screen-space global illumination. */
    HRL_GI_SSGI,

    /** @brief Voxel cone tracing. */
    HRL_GI_VCT,

    /** @brief Light propagation volumes. */
    HRL_GI_LPV,

    /** @brief Dynamic diffuse global illumination. */
    HRL_GI_DDGI,

    /** @brief Path-tracing global illumination. */
    HRL_GI_PATH_TRACING,

    /** @brief Ray-tracing global illumination. */
    HRL_GI_RAY_TRACING
};

/**
 * @brief Global multisample anti-aliasing modes.
 */
enum EAntialiasingMode {
    /** @brief Disable multisample anti-aliasing. */
    HRL_ANTIALIASING_OFF = 0,

    /** @brief Two-times multisample anti-aliasing. */
    HRL_ANTIALIASING_2X = 2,

    /** @brief Four-times multisample anti-aliasing. */
    HRL_ANTIALIASING_4X = 4,

    /** @brief Eight-times multisample anti-aliasing. */
    HRL_ANTIALIASING_8X = 8
};

/**
 * @brief Debug visualization modes.
 */
enum EDebugView {
    /** @brief Normal rendering without a debug override. */
    HRL_DEBUG_VIEW_NONE = 0x0060,

    /** @brief Unlit visualization. */
    HRL_DEBUG_VIEW_UNLIT,

    /** @brief Normal-vector visualization. */
    HRL_DEBUG_VIEW_NORMAL,

    /** @brief Light visualization. */
    HRL_DEBUG_VIEW_LIGHTS,

    /** @brief Alias for HRL_DEBUG_VIEW_LIGHTS. */
    HRL_DEBUG_VIEW_LIGHTING = HRL_DEBUG_VIEW_LIGHTS,

    /** @brief Wireframe visualization. */
    HRL_DEBUG_VIEW_WIREFRAME,

    /** @brief Level-of-detail visualization. */
    HRL_DEBUG_VIEW_LOD
};

/**
 * @brief LOD selection modes.
 */
enum ELODMode {
    /** @brief Select LOD levels from world-space distance. */
    HRL_LOD_DISTANCE = 0,

    /** @brief Select LOD levels from projected screen size. */
    HRL_LOD_SCREEN_SIZE
};

/**
 * @brief Error codes reported by HRL.
 */
enum EError {
    /** @brief No error has been recorded. */
    HRL_NO_ERROR = 0x0070,

    /** @brief An object ID was invalid. */
    HRL_ERROR_INVALID_ID,

    /** @brief An enum value was invalid. */
    HRL_INVALID_ENUM,

    /** @brief A value was invalid. */
    HRL_INVALID_VALUE,

    /** @brief The requested operation was invalid. */
    HRL_INVALID_OPERATION,

    /** @brief The requested operation is not supported by the active backend. */
    HRL_INVALID_BACKEND_OPERATION,

    /** @brief Shader compilation or linking failed. */
    HRL_SHADER_COMPILE_FAIL,

    /** @brief A memory allocation failed. */
    HRL_OUT_OF_MEMORY,

    /** @brief A supplied file or memory buffer has an invalid format. */
    HRL_INVALID_FILE_FORMAT
};

/**
 * @brief Error severity levels.
 */
enum ESeverity {
    /** @brief Weak warning. */
    HRL_SEVERITY_WEAK_WARNING = 0x0080,

    /** @brief Warning. */
    HRL_SEVERITY_WARNING,

    /** @brief Error. */
    HRL_SEVERITY_ERROR,

    /** @brief Fatal error. */
    HRL_SEVERITY_FATAL
};

/**
 * @brief Fog blending modes.
 */
enum EFogType {
    /** @brief Linear fog. */
    HRL_FOG_LINEAR = 0x0090,

    /** @brief Exponential fog. */
    HRL_FOG_EXPONENTIAL,

    /** @brief Exponential-squared fog. */
    HRL_FOG_EXP_SQUARED
};

/**
 * @brief Widget interaction state flags.
 */
enum EWidgetState {
    /** @brief Idle widget state. */
    HRL_WIDGET_STATE_IDLE = (1 << 0),

    /** @brief Hovered widget state. */
    HRL_WIDGET_STATE_HOVERED = (1 << 1),

    /** @brief Pressed widget state. */
    HRL_WIDGET_STATE_PRESSED = (1 << 2)
};

/**
 * @brief Types of UI widgets supported by HRL.
 */
enum EWidgetType {
    /** @brief Button widget. */
    HRL_WIDGET_BUTTON = 0x00A0,

    /** @brief Label widget. */
    HRL_WIDGET_LABEL,

    /** @brief Image widget. */
    HRL_WIDGET_IMAGE,

    /** @brief Slider widget. */
    HRL_WIDGET_SLIDER,

    /** @brief Checkbox widget. */
    HRL_WIDGET_CHECKBOX,

    /** @brief Progress bar widget. */
    HRL_WIDGET_PROGRESSBAR
};

/**
 * @brief Gizmo axis bit flags.
 */
enum EGizmoAxis {
    /** @brief No gizmo axis. */
    HRL_GIZMO_AXIS_NONE = 0,

    /** @brief X axis. */
    HRL_GIZMO_AXIS_X = 1 << 0,

    /** @brief Y axis. */
    HRL_GIZMO_AXIS_Y = 1 << 1,

    /** @brief Z axis. */
    HRL_GIZMO_AXIS_Z = 1 << 2
};

/**
 * @brief Gizmo transform mode flags.
 */
enum EGizmoMode {
    /** @brief Translation mode. */
    HRL_GIZMO_MODE_TRANSLATE = 1 << 0,

    /** @brief Rotation mode. */
    HRL_GIZMO_MODE_ROTATE = 1 << 1,

    /** @brief Scale mode. */
    HRL_GIZMO_MODE_SCALE = 1 << 2
};

/**
 * @brief Gizmo coordinate spaces.
 */
enum EGizmoSpace {
    /** @brief World-space gizmo transforms. */
    HRL_GIZMO_SPACE_WORLD = 0,

    /** @brief Local-space gizmo transforms. */
    HRL_GIZMO_SPACE_LOCAL
};

/**
 * @brief Parts of a gizmo that may be hovered or active.
 */
enum EGizmoPart {
    /** @brief No gizmo part. */
    HRL_GIZMO_PART_NONE = 0,

    /** @brief X-axis part. */
    HRL_GIZMO_PART_X,

    /** @brief Y-axis part. */
    HRL_GIZMO_PART_Y,

    /** @brief Z-axis part. */
    HRL_GIZMO_PART_Z,

    /** @brief Center part. */
    HRL_GIZMO_PART_CENTER
};

/**
 * @brief Active gizmo operation.
 */
enum EGizmoOperation {
    /** @brief No active gizmo operation. */
    HRL_GIZMO_OPERATION_NONE = 0,

    /** @brief Translation operation. */
    HRL_GIZMO_OPERATION_TRANSLATE = HRL_GIZMO_MODE_TRANSLATE,

    /** @brief Rotation operation. */
    HRL_GIZMO_OPERATION_ROTATE = HRL_GIZMO_MODE_ROTATE,

    /** @brief Scale operation. */
    HRL_GIZMO_OPERATION_SCALE = HRL_GIZMO_MODE_SCALE
};

/**
 * @brief VFX simulation spaces.
 */
enum EVFXSimulationSpace {
    /** @brief Simulate particles in emitter-local space. */
    HRL_VFX_SIMULATION_LOCAL = 0,

    /** @brief Simulate particles in world space. */
    HRL_VFX_SIMULATION_WORLD
};

/**
 * @brief VFX particle rendering modes.
 */
enum EVFXRenderMode {
    /** @brief Camera-facing billboard particles. */
    HRL_VFX_RENDER_BILLBOARD = 0,

    /** @brief Stretched billboard particles. */
    HRL_VFX_RENDER_STRETCHED_BILLBOARD,

    /** @brief Mesh particles. */
    HRL_VFX_RENDER_MESH
};

/**
 * @brief VFX particle blending modes.
 */
enum EVFXBlendMode {
    /** @brief Alpha blending. */
    HRL_VFX_BLEND_ALPHA = 0,

    /** @brief Additive blending. */
    HRL_VFX_BLEND_ADDITIVE,

    /** @brief Multiply blending. */
    HRL_VFX_BLEND_MULTIPLY
};

/**
 * @brief VFX particle spawn shapes.
 */
enum EVFXSpawnShape {
    /** @brief Spawn from a point. */
    HRL_VFX_SHAPE_POINT = 0,

    /** @brief Spawn inside a sphere. */
    HRL_VFX_SHAPE_SPHERE,

    /** @brief Spawn inside a box. */
    HRL_VFX_SHAPE_BOX,

    /** @brief Spawn inside a cylinder. */
    HRL_VFX_SHAPE_CYLINDER,

    /** @brief Spawn inside a cone. */
    HRL_VFX_SHAPE_CONE
};

/**
 * @brief Slider orientation.
 */
enum ESliderOrientation {
    /** @brief Horizontal slider. */
    HRL_SLIDER_HORIZONTAL = 0,

    /** @brief Vertical slider. */
    HRL_SLIDER_VERTICAL
};

/**
 * @brief Left mouse button constant used by MouseButtonCallback().
 */
inline constexpr int MOUSE_BUTTON_LEFT = 0;

/**
 * @brief Right mouse button constant used by MouseButtonCallback().
 */
inline constexpr int MOUSE_BUTTON_RIGHT = 1;

/**
 * @brief Middle mouse button constant used by MouseButtonCallback().
 */
inline constexpr int MOUSE_BUTTON_MIDDLE = 2;

/**
 * @brief Mouse release transition value.
 */
inline constexpr int MOUSE_RELEASE = 0;

/**
 * @brief Mouse press transition value.
 */
inline constexpr int MOUSE_PRESS = 1;

/**
 * @brief Resource texture kinds reported by an imported FBX scene.
 */
enum EFBXTextureType {
    /** @brief Texture backed by a file resource. */
    HRL_FBX_TEXTURE_FILE = 0,

    /** @brief Layered texture resource. */
    HRL_FBX_TEXTURE_LAYERED,

    /** @brief Procedural texture resource. */
    HRL_FBX_TEXTURE_PROCEDURAL,

    /** @brief Shader-generated texture resource. */
    HRL_FBX_TEXTURE_SHADER
};

/**
 * @brief Description of one texture resource in an imported FBX scene.
 *
 * The data pointer remains valid until the owning FBXResources object is released.
 */
struct FBXTextureInfo {
    /** @brief FBX texture name. */
    const char* name;

    /** @brief Associated filename when available. */
    const char* filename;

    /** @brief Embedded texture data, when available. */
    const unsigned char* data;

    /** @brief Size of the texture data buffer in bytes. */
    std::size_t size;

    /** @brief Resource texture kind. */
    EFBXTextureType type;

    /** @brief Non-zero when the texture bytes are embedded in the FBX resource. */
    int embedded;
};

/**
 * @brief Texture slot understood by HRL's built-in 3D material shader.
 */
enum EFBXMaterialTextureSlot {
    /** @brief Albedo/base-color texture slot. */
    HRL_FBX_MATERIAL_ALBEDO = 0,

    /** @brief Normal-map texture slot. */
    HRL_FBX_MATERIAL_NORMAL,

    /** @brief Specular texture slot. */
    HRL_FBX_MATERIAL_SPECULAR,

    /** @brief Roughness texture slot. */
    HRL_FBX_MATERIAL_ROUGHNESS,

    /** @brief Metallic texture slot. */
    HRL_FBX_MATERIAL_METALLIC,

    /** @brief Alpha texture slot. */
    HRL_FBX_MATERIAL_ALPHA,

    /** @brief Number of material texture slots. */
    HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT
};

/**
 * @brief Material information extracted from the FBX PBR/legacy material model.
 *
 * textureIndices contains INVALID_ID where no matching texture exists.
 */
struct FBXMaterialInfo {
    /** @brief FBX material name. */
    const char* name;

    /** @brief Base color RGBA values. */
    float baseColor[4];

    /** @brief Roughness value. */
    float roughness;

    /** @brief Metallic value. */
    float metallic;

    /** @brief Specular value. */
    float specular;

    /** @brief Opacity value. */
    float opacity;

    /** @brief Texture indices for the built-in material slots. */
    uint textureIndices[HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT];
};

class Instance;
class Mesh;
class Mesh3D;
class StaticMesh;
class SkeletalMesh;
class Sprite;
class Light;
class Texture;
class Scene;
class PostProcess;
class Shader;
class Material;
class Viewport;
class Camera;
class Font;
class Widget;
class Gizmo;
class VFXSystem;
class VFXEmitter;
class VFXCurve;
class VolumetricFog;
class FBXResources;

/**
 * @brief Error callback invoked whenever an internal error is raised.
 */
using ErrorCallback = void(*)(EError code, ESeverity severity, const char* detail);

/**
 * @brief Button callback receiving click/release transitions.
 */
using ButtonPressedCallback = void(*)(id button, int clicked, int released, void* userData);

/**
 * @brief Slider callback receiving the new slider value.
 */
using SliderChangedCallback = void(*)(id slider, float value, void* userData);

/**
 * @brief Checkbox callback receiving the new checked state.
 */
using CheckboxChangedCallback = void(*)(id checkbox, int checked, void* userData);

/**
 * @brief Gizmo callback invoked when a gizmo interaction changes.
 */
using GizmoChangedCallback = void(*)(id gizmo, EGizmoPart part, void* userData);

/**
 * @class Instance
 * @brief Owns the HRL rendering context and global renderer state.
 */
/* ============================================================================
 *  INITIALIZATION & LIFECYCLE
 * ============================================================================ */

class HRL_CPP_API Instance {
public:
    /**
     * @brief Selects the graphics backend to use and initializes the rendering context.
     *
     * The selected backend must be configured before rendering begins.
     *
     * @param api Graphics backend to use.
     * @param width Initial framebuffer width in pixels.
     * @param height Initial framebuffer height in pixels.
     * @param loader Platform-specific function loader (for example, glfwGetProcAddress).
     */
    Instance(E_APIs api, uint width, uint height, void* loader);

    /**
     * @brief Releases the rendering context.
     */
    ~Instance();

    /**
     * @brief Copy construction is disabled for this resource wrapper.
     */
    Instance(const Instance&) = delete;

    /**
     * @brief Copy assignment is disabled for this resource wrapper.
     */
    Instance& operator=(const Instance&) = delete;

    /**
     * @brief Move construction is disabled for this resource wrapper.
     */
    Instance(Instance&&) = delete;

    /**
     * @brief Move assignment is disabled for this resource wrapper.
     */
    Instance& operator=(Instance&&) = delete;

    /**
     * @brief Prepares the renderer for a new frame.
     *
     * Call at the start of the render loop.
     */
    void BeginFrame();

    /**
     * @brief Finalizes and submits the current frame.
     *
     * Flushes draw commands and swaps buffers if applicable.
     */
    void EndFrame();

    /**
     * @brief Notifies HRL of a window or framebuffer resize.
     *
     * Call from the framebuffer-size callback.
     *
     * @param width New drawable framebuffer width in pixels.
     * @param height New drawable framebuffer height in pixels.
     */
    void WindowResizeCallback(int width, int height);

    /**
     * @brief Retrieves the last recorded error, if any.
     *
     * @param detail Optional output pointer receiving a human-readable description.
     * @param severity Optional output pointer receiving the error severity.
     * @return The error code. Returns HRL_NO_ERROR if no error occurred.
     */
    EError GetLastError(const char** detail = nullptr, ESeverity* severity = nullptr) const;

    /**
     * @brief Converts an error enum value to its string representation.
     *
     * @param err Error code to convert.
     * @return Null-terminated string representation.
     */
    std::string ErrorEnumToString(EError err) const;

    /**
     * @brief Converts an error severity enum value to its string representation.
     *
     * @param severity Severity value to convert.
     * @return Null-terminated string representation.
     */
    std::string SeverityEnumToString(ESeverity severity) const;

    /**
     * @brief Registers a callback invoked whenever an error is raised internally.
     *
     * Useful for integrating HRL errors into a custom logging or assertion system.
     *
     * @param callback Callback with signature void(HRL_Error, HRL_Severity, const char*).
     */
    void RegisterErrorCallback(ErrorCallback callback);
};

/**
 * @class Mesh
 * @brief Common base wrapper for HRL mesh objects.
 */
/* ============================================================================
 *  MESHES & SPRITES
 * ============================================================================ */

class HRL_CPP_API Mesh {
public:
    /**
     * @brief Creates a non-owning C++ wrapper around an existing mesh ID.
     *
     * @param value Existing HRL mesh ID.
     * @return Non-owning mesh wrapper.
     */
    static Mesh Borrowed(id value);

    /**
     * @brief Virtual destructor for the common mesh wrapper.
     */
    virtual ~Mesh();

    /**
     * @brief Copy construction is disabled for this resource wrapper.
     */
    Mesh(const Mesh&) = delete;

    /**
     * @brief Copy assignment is disabled for this resource wrapper.
     */
    Mesh& operator=(const Mesh&) = delete;

    /**
     * @brief Move-constructs a mesh wrapper.
     */
    Mesh(Mesh&& other) noexcept;

    /**
     * @brief Move-assigns a mesh wrapper.
     */
    Mesh& operator=(Mesh&& other) noexcept;

    /**
     * @brief Returns the underlying HRL mesh ID.
     *
     * @return Mesh identifier, or INVALID_ID for an empty wrapper.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live mesh object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the owned mesh and releases its associated GPU resources.
     *
     * Non-owning wrappers do not destroy the underlying object.
     */
    void Release();

    /**
     * @brief Sets the pivot (origin) point of a mesh or sprite.
     *
     * Coordinates are normalized: (0,0,0) is center and (-0.5,-0.5,0) is top-left.
     *
     * @param x Pivot X coordinate.
     * @param y Pivot Y coordinate.
     * @param z Pivot Z coordinate.
     */
    void SetPivotPoint(float x, float y, float z);

    /**
     * @brief Assigns a material to a mesh.
     *
     * @param material Material to apply.
     */
    void SetMaterial(const Material& material);

    /**
     * @brief Sets the world-space position of a mesh.
     */
    void SetLocation(float x, float y, float z);

    /**
     * @brief Sets the rotation of a mesh using Euler angles in degrees.
     *
     * @param pitch Rotation around the X axis.
     * @param yaw Rotation around the Y axis.
     * @param roll Rotation around the Z axis.
     */
    void SetRotation(float pitch, float yaw, float roll);

    /**
     * @brief Sets the scale of a mesh along each local axis.
     */
    void SetScale(float x, float y, float z);

protected:
    /**
     * @brief Constructs an empty mesh wrapper.
     */
    Mesh() = default;

    /**
     * @brief Constructs a mesh wrapper around an existing HRL ID.
     *
     * @param value Existing mesh ID.
     * @param owned Whether this wrapper owns the underlying resource.
     */
    explicit Mesh(id value, bool owned);

    /** @brief Underlying HRL mesh identifier. */
    id id_ = INVALID_ID;

    /** @brief Whether this wrapper owns the underlying mesh resource. */
    bool owned_ = false;

    /** @brief Allows generic mesh factory functions to construct owning wrappers. */
    friend Mesh CreateMesh(Scene&, EMeshType, const float*);

    /** @brief Allows generic mesh factory functions to construct owning wrappers. */
    friend Mesh CreateMeshFromFile(Scene&, EMeshType, const void*, std::size_t);
};

/**
 * @class Mesh3D
 * @brief Common 3D mesh functionality, primarily level-of-detail support.
 */
class HRL_CPP_API Mesh3D : public Mesh {
protected:
    /**
     * @brief Constructs a 3D mesh wrapper.
     */
    explicit Mesh3D(id value, bool owned) : Mesh(value, owned) {}

public:
    /**
     * @brief Creates a non-owning wrapper around an existing 3D mesh ID.
     *
     * @param value Existing 3D mesh ID.
     * @return Non-owning 3D mesh wrapper.
     */
    static Mesh3D Borrowed(id value);

    /**
     * @brief Returns the minimum world-space distance from the mesh LOD center to any viewport camera.
     */
    float GetCameraDistance() const;

    /**
     * @brief Enables or disables automatic LOD selection for a 3D mesh.
     */
    void SetLODAutomatic(bool enabled);

    /**
     * @brief Selects distance-based or projected screen-size LOD selection.
     */
    void SetLODMode(ELODMode mode);

    /**
     * @brief Sets the number of LOD levels, including LOD 0.
     *
     * @param levels LOD count. The C API documents a range of 1..8.
     */
    void SetLODLevels(uint levels);

    /**
     * @brief Sets the first transition distance in world units.
     */
    void SetLODDistance(float baseDistance);

    /**
     * @brief Multiplies the distance threshold for each next LOD.
     */
    void SetLODScale(float distanceScale);

    /**
     * @brief Sets the minimum distance used by distance-based LOD selection.
     */
    void SetLODMinDistance(float distance);

    /**
     * @brief Sets the maximum distance used by distance-based LOD selection.
     */
    void SetLODMaxDistance(float distance);

    /**
     * @brief Sets the LOD 0 screen-height threshold.
     *
     * @param threshold Screen-height threshold in [0,1].
     */
    void SetLODScreenThreshold(float threshold);

    /**
     * @brief Multiplies the screen-size threshold for each next LOD.
     */
    void SetLODScreenScale(float scale);

    /**
     * @brief Adds transition hysteresis to reduce LOD popping.
     *
     * @param hysteresis Hysteresis value in [0,0.49].
     */
    void SetLODHysteresis(float hysteresis);

    /**
     * @brief Forces a fixed LOD level.
     *
     * Pass -1 to return to automatic selection.
     */
    void SetLODOverride(int level);

    /**
     * @brief Regenerates the internal LOD geometry from LOD 0.
     */
    void ForceLODRebuild();

    /**
     * @brief Returns the number of available LOD levels.
     */
    uint GetLODCount() const;

    /**
     * @brief Returns the vertex count of a specific LOD level.
     */
    std::size_t GetLODVertexCount(uint level) const;

    /**
     * @brief Returns the triangle count of a specific LOD level.
     */
    std::size_t GetLODTriangleCount(uint level) const;

    /**
     * @brief Returns the LOD chosen by the most recently rendered viewport.
     */
    int GetLODLevel() const;
};

/**
 * @class StaticMesh
 * @brief A conventional non-skinned 3D mesh created inside a Scene.
 */
/* ============================================================================
 *  SKELETAL / STATIC MESH IMPLEMENTATIONS
 * ============================================================================ */

class HRL_CPP_API StaticMesh : public Mesh3D {
public:
    /**
     * @brief Creates a static 3D mesh from caller-owned vertex and index arrays.
     *
     * @param scene Target scene.
     * @param vertices Vertex array.
     * @param vertexCount Number of vertices.
     * @param indices Optional triangle index array.
     * @param indexCount Number of indices, or zero for non-indexed rendering.
     */
    explicit StaticMesh(
        Scene& scene,
        const Vertex3D* vertices,
        std::size_t vertexCount,
        const uint* indices = nullptr,
        std::size_t indexCount = 0);

    /**
     * @brief Creates a non-owning wrapper around an existing static mesh ID.
     */
    static StaticMesh Borrowed(id value);

protected:
    /**
     * @brief Constructs a static mesh wrapper around an existing ID.
     */
    explicit StaticMesh(id value, bool owned) : Mesh3D(value, owned) {}
};

/**
 * @class SkeletalMesh
 * @brief A skinned 3D mesh created inside a Scene.
 */
class HRL_CPP_API SkeletalMesh : public Mesh3D {
public:
    /**
     * @brief Creates a skeletal mesh from CPU-side skeletal data.
     *
     * HRL copies the source data and the caller may release it after construction.
     */
    explicit SkeletalMesh(Scene& scene, const SkeletalMeshData& data);

    /**
     * @brief Creates a non-owning wrapper around an existing skeletal mesh ID.
     */
    static SkeletalMesh Borrowed(id value);

    /**
     * @brief Returns the number of skeletal bones.
     */
    uint GetSkeletalBoneCount() const;

    /**
     * @brief Returns read-only skeletal bone metadata.
     *
     * @param index Bone index.
     * @return Pointer to bone metadata, or nullptr for an invalid index.
     */
    const SkeletalBone* GetSkeletalBone(uint index) const;

    /**
     * @brief Finds a skeletal bone by name.
     *
     * @return Bone index, or INVALID_ID when no matching bone exists.
     */
    uint FindSkeletalBone(const char* name) const;

    /**
     * @brief Returns the number of skeletal animations.
     */
    uint GetSkeletalAnimationCount() const;

    /**
     * @brief Returns read-only skeletal animation metadata.
     *
     * @param index Animation index.
     * @return Pointer to animation metadata, or nullptr for an invalid index.
     */
    const SkeletalAnimation* GetSkeletalAnimation(uint index) const;

    /**
     * @brief Finds a skeletal animation by name.
     *
     * @return Animation index, or INVALID_ID when no matching animation exists.
     */
    uint FindSkeletalAnimation(const char* name) const;

    /**
     * @brief Starts playback of a skeletal animation by index.
     */
    void PlaySkeletalAnimation(uint animation);

    /**
     * @brief Stops skeletal animation playback.
     */
    void StopSkeletalAnimation();

    /**
     * @brief Sets the current skeletal animation time.
     */
    void SetSkeletalAnimationTime(float time);

    /**
     * @brief Sets skeletal animation playback speed.
     */
    void SetSkeletalAnimationSpeed(float speed);

    /**
     * @brief Enables or disables skeletal animation looping.
     */
    void SetSkeletalAnimationLoop(bool loop);

    /**
     * @brief Returns the current skeletal animation index.
     */
    int GetCurrentSkeletalAnimation() const;

    /**
     * @brief Returns the current skeletal animation time.
     */
    float GetSkeletalAnimationTime() const;

    /**
     * @brief Returns whether skeletal animation is currently playing.
     */
    bool IsSkeletalAnimationPlaying() const;

protected:
    /**
     * @brief Constructs a skeletal mesh wrapper around an existing ID.
     */
    explicit SkeletalMesh(id value, bool owned) : Mesh3D(value, owned) {}
};

/**
 * @class Sprite
 * @brief A camera-facing 2D sprite mesh created inside a Scene.
 */
class HRL_CPP_API Sprite : public Mesh {
public:
    /**
     * @brief Creates a sprite mesh in a scene.
     */
    explicit Sprite(Scene& scene);

    /**
     * @brief Creates a non-owning wrapper around an existing sprite ID.
     */
    static Sprite Borrowed(id value);

    /**
     * @brief Defines the UV region of the texture displayed on the sprite.
     *
     * Useful for sprite atlases. (minU,minV) is the top-left corner and
     * (maxU,maxV) is the bottom-right corner in normalized [0,1] coordinates.
     */
    void SetSpriteRegion(float minU, float minV, float maxU, float maxV);

    /**
     * @brief Controls the rendering order of a sprite on the Z axis.
     *
     * Only relevant when two or more sprites share the same Z depth.
     * Higher values are drawn on top.
     */
    void SetDrawOrder(float drawOrder);

    /**
     * @brief Alias for SetDrawOrder().
     */
    void SetSpriteDrawOrder(float drawOrder);

protected:
    /**
     * @brief Constructs a sprite wrapper around an existing ID.
     */
    explicit Sprite(id value, bool owned) : Mesh(value, owned) {}
};

/**
 * @class Light
 * @brief Light source owned by a Scene.
 */
/* ============================================================================
 *  LIGHTS
 * ============================================================================ */

class HRL_CPP_API Light {
public:
    /**
     * @brief Creates a light source in a scene.
     */
    explicit Light(Scene& scene, ELightType type);

    /**
     * @brief Destroys the owned light.
     */
    ~Light();

    /**
     * @brief Copy construction is disabled for this resource wrapper.
     */
    Light(const Light&) = delete;

    /**
     * @brief Copy assignment is disabled for this resource wrapper.
     */
    Light& operator=(const Light&) = delete;

    /**
     * @brief Move-constructs a light wrapper.
     */
    Light(Light&& other) noexcept;

    /**
     * @brief Move-assigns a light wrapper.
     */
    Light& operator=(Light&& other) noexcept;

    /**
     * @brief Returns the underlying light ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live light object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the owned light.
     */
    void Release();

    /**
     * @brief Sets the RGB color emitted by a light.
     *
     * Values are typically in [0,1] but may exceed 1 for HDR workflows.
     */
    void SetColor(float x, float y, float z);

    /**
     * @brief Sets the intensity (brightness multiplier) of a light.
     *
     * The C API documents 1.0 as the default.
     */
    void SetIntensity(float intensity);

    /**
     * @brief Sets the attenuation (falloff) factor of a light.
     *
     * Controls how quickly the light fades with distance.
     */
    void SetAttenuation(float attenuation);

    /**
     * @brief Sets the world-space position of a light.
     *
     * Relevant for point lights and spot lights.
     */
    void SetLocation(float x, float y, float z);

    /**
     * @brief Sets the orientation of a light using Euler angles in degrees.
     *
     * Primarily relevant for directional and spot lights.
     */
    void SetRotation(float pitch, float yaw, float roll);

    /**
     * @brief Enables or disables shadow casting for a light.
     *
     * OpenGL 3.3 supports shadows for point, directional and spot lights.
     */
    void SetCastShadows(bool enabled);

    /**
     * @brief Sets the shadow bias used to reduce self-shadowing artifacts.
     */
    void SetShadowBias(float bias);

    /**
     * @brief Controls how dark a cast shadow is for this light.
     *
     * @param strength Value in [0,1]. 0 keeps the surface fully lit and 1 applies the full shadow term.
     */
    void SetShadowStrength(float strength);

    /**
     * @brief Sets the resolution of the private shadow map for a light.
     *
     * Supported values are positive powers of two; the OpenGL backend may clamp
     * the requested value to implementation limits.
     */
    void SetShadowResolution(int resolution);

    /**
     * @brief Sets the inner cutoff angle of a spot light.
     *
     * @param degrees Cutoff angle in degrees.
     */
    void SetSpotLightInnerCutoff(float degrees);

    /**
     * @brief Sets the outer cutoff angle of a spot light.
     *
     * @param degrees Cutoff angle in degrees.
     */
    void SetSpotLightOuterCutoff(float degrees);

private:
    /** @brief Underlying light identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class Texture
 * @brief GPU texture resource.
 */
/* ============================================================================
 *  TEXTURES
 * ============================================================================ */

class HRL_CPP_API Texture {
public:
    /**
     * @brief Creates a GPU texture from a raw file buffer.
     *
     * Supported formats: png, jpeg, jpg, bmp, tga, gif (first frame), hdr, psd (partial).
     *
     * @param data Pointer to the file contents.
     * @param size Size of the buffer in bytes.
     */
    Texture(const void* data, std::size_t size);

    /**
     * @brief Starts an asynchronous texture load from a raw file buffer.
     *
     * The texture becomes usable when IsReady() returns true or after Wait().
     */
    static Texture CreateAsync(const void* data, std::size_t size);

    /**
     * @brief Rasterizes a UTF-8 text string into a new texture using a font.
     *
     * @param text Null-terminated UTF-8 string to render.
     * @param font Font used for rasterization.
     * @param fontSize Glyph height in pixels.
     * @param wrapWidth Line-wrap threshold in pixels. Pass 0 to disable wrapping.
     * @param r,g,b Text foreground color in [0,1].
     * @param bgR,bgG,bgB,bgA Background color. Set bgA to 0 for a transparent background.
     */
    Texture(
        const char* text,
        const Font& font,
        float fontSize,
        float wrapWidth,
        float r, float g, float b,
        float bgR, float bgG, float bgB, float bgA);

    /**
     * @brief Destroys the texture.
     */
    ~Texture();

    /**
     * @brief Copy construction is disabled for this resource wrapper.
     */
    Texture(const Texture&) = delete;

    /**
     * @brief Copy assignment is disabled for this resource wrapper.
     */
    Texture& operator=(const Texture&) = delete;

    /**
     * @brief Move-constructs a texture wrapper.
     */
    Texture(Texture&& other) noexcept;

    /**
     * @brief Move-assigns a texture wrapper.
     */
    Texture& operator=(Texture&& other) noexcept;

    /**
     * @brief Returns the underlying texture ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live texture object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the owned texture and releases its GPU memory.
     */
    void Release();

    /**
     * @brief Starts or performs a texture data reload from a new file buffer.
     *
     * The texture ID remains valid; materials referencing it use the updated image.
     */
    void Reload(const void* data, std::size_t size);

    /**
     * @brief Returns the current texture dimensions in pixels.
     *
     * @param width Output width.
     * @param height Output height.
     */
    void GetSize(int& width, int& height) const;

    /**
     * @brief Sets the minification filter used when the texture appears smaller than its native size.
     */
    void SetMinFilter(EFilterType filter);

    /**
     * @brief Sets the magnification filter used when the texture appears larger than its native size.
     */
    void SetMagFilter(EFilterType filter);

    /**
     * @brief Returns whether an asynchronous texture has finished decoding and GPU upload.
     */
    bool IsReady() const;

    /**
     * @brief Blocks until an asynchronous texture is fully uploaded.
     */
    void Wait();

protected:
    /**
     * @brief Constructs an owning texture wrapper around an existing HRL texture ID.
     */
    explicit Texture(id value) : id_(value) {}

    /** @brief Allows FBXResources to adopt a newly-created GPU texture ID. */
    friend class FBXResources;

private:
    /** @brief Underlying texture identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class Scene
 * @brief Scene container for meshes, lights, cameras and rendering effects.
 */
/* ============================================================================
 *  SCENES
 * ============================================================================ */

class HRL_CPP_API Scene {
public:
    /**
     * @brief Creates a scene.
     *
     * @param renderOnScreen If true, render directly to the screen; otherwise render to an off-screen texture.
     */
    explicit Scene(bool renderOnScreen);

    /**
     * @brief Destroys the scene and its owned objects.
     */
    ~Scene();

    /**
     * @brief Copy construction is disabled for this resource wrapper.
     */
    Scene(const Scene&) = delete;

    /**
     * @brief Copy assignment is disabled for this resource wrapper.
     */
    Scene& operator=(const Scene&) = delete;

    /**
     * @brief Move-constructs a scene wrapper.
     */
    Scene(Scene&& other) noexcept;

    /**
     * @brief Move-assigns a scene wrapper.
     */
    Scene& operator=(Scene&& other) noexcept;

    /**
     * @brief Returns the underlying scene ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live scene object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the scene and releases its resources.
     */
    void Release();

    /**
     * @brief Enables or disables global illumination for the scene.
     *
     * Global illumination is disabled by default.
     */
    void SetGlobalIlluminationEnabled(bool enabled);

    /**
     * @brief Selects the global illumination method.
     *
     * HRL_GI_NONE disables GI for the scene.
     */
    void SetGlobalIlluminationMethod(EGlobalIlluminationMethod method);

    /**
     * @brief Returns the scene global illumination enabled state.
     */
    bool IsGlobalIlluminationEnabled() const;

    /**
     * @brief Returns the scene's selected global illumination method.
     */
    EGlobalIlluminationMethod GetGlobalIlluminationMethod() const;

    /**
     * @brief Resizes the off-screen render texture of the scene.
     *
     * Has no effect if the scene was created with renderOnScreen = true.
     */
    void ResizeTexture(int width, int height);

    /**
     * @brief Enables or disables the procedural sky sphere for the scene.
     *
     * The sky follows camera translation so it behaves as an infinitely distant environment
     * while remaining a true sphere in the OpenGL backend. Disabled by default for backward compatibility.
     */
    void SetSkySphereEnabled(bool enabled);

    /**
     * @brief Sets the top, horizon and bottom colors of the procedural sky.
     *
     * Values are linear RGB and may exceed 1 for HDR rendering.
     */
    void SetSkySphereColors(
        float topR, float topG, float topB,
        float horizonR, float horizonG, float horizonB,
        float bottomR, float bottomG, float bottomB);

    /**
     * @brief Rotates the sky sphere using Euler angles in degrees.
     */
    void SetSkySphereRotation(float pitch, float yaw, float roll);

    /**
     * @brief Uses an equirectangular 2D texture as the sky-sphere image.
     *
     * Passing a null texture pointer returns the scene to the procedural color sky.
     */
    void SetSkySphereTexture(const Texture* texture);

    /**
     * @brief Enables or disables equirectangular environment mapping for 3D materials.
     *
     * The environment texture is independent from the sky texture.
     */
    void SetEnvironmentMappingEnabled(bool enabled);

    /**
     * @brief Sets an equirectangular texture used for environment reflections.
     *
     * Passing a null texture pointer removes the environment map.
     */
    void SetEnvironmentMap(const Texture* texture);

    /**
     * @brief Enables or disables the GPU color-picking buffer for the scene.
     *
     * When enabled, each rendered object is assigned a unique color ID,
     * allowing CPU-side object picking by reading pixel values.
     */
    void EnableColorPickingBuffer(bool enabled);

    /**
     * @brief Returns the mesh hovered at the given window-relative mouse coordinates.
     *
     * @param mouseX Relative mouse cursor X coordinate.
     * @param mouseY Relative mouse cursor Y coordinate.
     * @param meshType Optional output for the hovered mesh type.
     * @return A non-owning mesh wrapper for the hovered object, or INVALID_ID when none is hovered.
     */
    Mesh GetHoveredObject(int mouseX, int mouseY, EMeshType* meshType = nullptr);

    /**
     * @brief Enables or disables the fog effect for the scene.
     */
    void SetFogEnabled(bool enabled);

    /**
     * @brief Sets the fog blending mode for the scene.
     */
    void SetFogMode(EFogType mode);

    /**
     * @brief Sets the fog color.
     *
     * @param r,g,b Fog color in [0,1].
     */
    void SetFogColor(float r, float g, float b);

    /**
     * @brief Sets the fog density for exponential fog modes.
     *
     * Has no effect when fog mode is HRL_FOG_LINEAR.
     */
    void SetFogDensity(float density);

    /**
     * @brief Sets the start and end distances for linear fog.
     *
     * Objects beyond end are fully fogged; objects before start are unaffected.
     */
    void SetFogLinearRange(float start, float end);

    /**
     * @brief Creates a localized volumetric fog volume attached to the scene.
     *
     * The OpenGL 3.3 backend supports up to MAX_VOLUMETRIC_FOGS active volumes per scene.
     */
    VolumetricFog CreateVolumetricFog();

    /**
     * @brief Enables or disables scene-wide volumetric fog.
     */
    void SetGlobalVolumetricFogEnabled(bool enabled);

    /**
     * @brief Sets the density of the scene-wide volumetric fog.
     */
    void SetGlobalVolumetricFogDensity(float density);

    /**
     * @brief Sets the color of the scene-wide volumetric fog.
     */
    void SetGlobalVolumetricFogColor(float r, float g, float b);

    /**
     * @brief Sets the ray-march sample count for scene-wide volumetric fog.
     *
     * The C API documents a range of 4..64.
     */
    void SetGlobalVolumetricFogSteps(uint steps);

    /**
     * @brief Enables or disables screen-space god rays.
     */
    void SetGodRaysEnabled(bool enabled);

    /**
     * @brief Sets the world-space position from which god rays radiate.
     */
    void SetGodRaysPosition(float x, float y, float z);

    /**
     * @brief Sets the god-ray color.
     */
    void SetGodRaysColor(float r, float g, float b);

    /**
     * @brief Sets radial sampling density for god rays.
     */
    void SetGodRaysDensity(float density);

    /**
     * @brief Sets per-sample decay for god rays.
     */
    void SetGodRaysDecay(float decay);

    /**
     * @brief Sets the contribution weight for god rays.
     */
    void SetGodRaysWeight(float weight);

    /**
     * @brief Sets the radial sample count for god rays.
     *
     * The C API documents a range of 8..96.
     */
    void SetGodRaysSamples(uint samples);

    /**
     * @brief Enables or disables screen-space ambient occlusion.
     *
     * Disabled by default.
     */
    void SetAmbientOcclusionEnabled(bool enabled);

    /**
     * @brief Sets the ambient occlusion strength.
     */
    void SetAmbientOcclusionStrength(float strength);

    /**
     * @brief Sets the ambient occlusion radius.
     */
    void SetAmbientOcclusionRadius(float radius);

    /**
     * @brief Sets the ambient occlusion bias.
     */
    void SetAmbientOcclusionBias(float bias);

    /**
     * @brief Sets the ambient occlusion power.
     */
    void SetAmbientOcclusionPower(float power);

    /**
     * @brief Overrides scene rendering with a diagnostic visualization mode.
     *
     * Useful for inspecting normals, lighting, or other render passes in isolation.
     */
    void DrawAsDebugMode(EDebugView mode);

    /**
     * @brief Captures the rendered output of the scene and saves it as a PNG file.
     *
     * The request is queued and consumed at the end of the current frame.
     * On OpenGL 3.3 the capture includes the final on-screen post-process result.
     *
     * @param path Target path for the PNG image.
     */
    void TakeScreenshot(const char* path) const;

private:
    /** @brief Underlying scene identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class PostProcess
 * @brief Post-process pass attached to a viewport.
 */
/* ============================================================================
 *  POST PROCESSING
 * ============================================================================ */

class HRL_CPP_API PostProcess {
public:
    /**
     * @brief Creates and attaches a post-process pass to a viewport.
     *
     * Passes are applied in creation order after the scene is rendered.
     *
     * @param viewport Target viewport.
     * @param material Material containing the full-screen shader.
     * @param priority Post-process priority.
     */
    explicit PostProcess(Viewport& viewport, const Material& material, int priority);

    /**
     * @brief Destroys the post-process pass.
     */
    ~PostProcess();

    /**
     * @brief Copy construction is disabled.
     */
    PostProcess(const PostProcess&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    PostProcess& operator=(const PostProcess&) = delete;

    /**
     * @brief Move-constructs a post-process wrapper.
     */
    PostProcess(PostProcess&& other) noexcept;

    /**
     * @brief Move-assigns a post-process wrapper.
     */
    PostProcess& operator=(PostProcess&& other) noexcept;

    /**
     * @brief Returns the underlying post-process ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live post-process object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the owned post-process pass.
     */
    void Release();

private:
    /** @brief Underlying post-process identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class Shader
 * @brief Compiled shader program.
 */
/* ============================================================================
 *  SHADERS
 * ============================================================================ */

class HRL_CPP_API Shader {
public:
    /**
     * @brief Compiles and links a shader from vertex and fragment source buffers.
     *
     * @param vertexData Vertex shader source buffer.
     * @param vertexSize Vertex shader source size in bytes.
     * @param fragmentData Fragment shader source buffer.
     * @param fragmentSize Fragment shader source size in bytes.
     */
    Shader(
        const void* vertexData, std::size_t vertexSize,
        const void* fragmentData, std::size_t fragmentSize);

    /**
     * @brief Starts asynchronous shader source loading.
     *
     * Compilation and linking are performed automatically on the HRL context thread.
     */
    static Shader CreateAsync(
        const void* vertexData, std::size_t vertexSize,
        const void* fragmentData, std::size_t fragmentSize);

    /**
     * @brief Destroys the shader program and frees its GPU resources.
     */
    ~Shader();

    /**
     * @brief Copy construction is disabled.
     */
    Shader(const Shader&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    Shader& operator=(const Shader&) = delete;

    /**
     * @brief Move-constructs a shader wrapper.
     */
    Shader(Shader&& other) noexcept;

    /**
     * @brief Move-assigns a shader wrapper.
     */
    Shader& operator=(Shader&& other) noexcept;

    /**
     * @brief Returns the underlying shader ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live shader object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the owned shader and frees its GPU resources.
     */
    void Release();

    /**
     * @brief Returns whether an asynchronous shader has finished compilation and linking.
     */
    bool IsReady() const;

    /**
     * @brief Blocks until an asynchronous shader is compiled and linked.
     */
    void Wait();

protected:
    /**
     * @brief Constructs an owning shader wrapper around an existing HRL shader ID.
     */
    explicit Shader(id value) : id_(value) {}

private:
    /** @brief Underlying shader identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class Material
 * @brief Material instance backed by a shader.
 */
/* ============================================================================
 *  MATERIALS
 * ============================================================================ */

class HRL_CPP_API Material {
public:
    /**
     * @brief Creates a material instance backed by a shader.
     */
    explicit Material(const Shader& shader);

    /**
     * @brief Destroys the material and its stored uniform data.
     */
    ~Material();

    /**
     * @brief Copy construction is disabled.
     */
    Material(const Material&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    Material& operator=(const Material&) = delete;

    /**
     * @brief Move-constructs a material wrapper.
     */
    Material(Material&& other) noexcept;

    /**
     * @brief Move-assigns a material wrapper.
     */
    Material& operator=(Material&& other) noexcept;

    /**
     * @brief Returns the underlying material ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live material object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the material and releases its resources.
     */
    void Release();

    /**
     * @brief Sets an integer uniform on the material.
     */
    void SetInt(const char* name, int value);

    /**
     * @brief Binds a texture to a named sampler uniform on the material.
     */
    void SetTexture(const char* name, const Texture& texture);

    /**
     * @brief Sets a boolean uniform on the material.
     *
     * The C API stores the value internally as integer 0 or 1.
     */
    void SetBool(const char* name, bool value);

    /**
     * @brief Sets a float uniform on the material.
     */
    void SetFloat(const char* name, float value);

    /**
     * @brief Sets a vec2 uniform on the material.
     */
    void SetVec2(const char* name, float x, float y);

    /**
     * @brief Sets a vec3 uniform on the material.
     */
    void SetVec3(const char* name, float x, float y, float z);

    /**
     * @brief Sets a vec4 uniform on the material.
     */
    void SetVec4(const char* name, float x, float y, float z, float w);

    /**
     * @brief Sets the emissive color tint of the material.
     *
     * The color is additively blended with the emissive texture.
     */
    void SetEmissiveColor(float r, float g, float b, float a);

protected:
    /**
     * @brief Constructs an owning material wrapper around an existing HRL material ID.
     */
    explicit Material(id value) : id_(value) {}

    /** @brief Allows FBX convenience factories to adopt newly-created material IDs. */
    friend Material CreateMaterialFromFBX(const void*, std::size_t);

    /** @brief Allows FBX convenience factories to adopt newly-created material IDs. */
    friend Material CreateMaterialFromFBXWithShader(const void*, std::size_t, const Shader&);

    /** @brief Allows FBX convenience factories to adopt newly-created material IDs. */
    friend Material CreateMaterialFromFBXIndexed(const void*, std::size_t, std::size_t);

    /** @brief Allows FBX convenience factories to adopt newly-created material IDs. */
    friend Material CreateMaterialFromFBXIndexedWithShader(
        const void*, std::size_t, std::size_t, const Shader&);

private:
    /** @brief Underlying material identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class Viewport
 * @brief Screen-space rendering region for a Scene.
 */
/* ============================================================================
 *  VIEWPORTS
 * ============================================================================ */

class HRL_CPP_API Viewport {
public:
    /**
     * @brief Creates a viewport that renders a scene through a camera into a screen region.
     *
     * All coordinates are normalized [0,1]: (0,0) is top-left and (1,1) is bottom-right.
     */
    Viewport(Scene& scene, const Camera* camera, float x, float y, float width, float height);

    /**
     * @brief Destroys the viewport.
     */
    ~Viewport();

    /**
     * @brief Copy construction is disabled.
     */
    Viewport(const Viewport&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    Viewport& operator=(const Viewport&) = delete;

    /**
     * @brief Move-constructs a viewport wrapper.
     */
    Viewport(Viewport&& other) noexcept;

    /**
     * @brief Move-assigns a viewport wrapper.
     */
    Viewport& operator=(Viewport&& other) noexcept;

    /**
     * @brief Returns the underlying viewport ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live viewport object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the viewport.
     */
    void Release();

    /**
     * @brief Reassigns the camera used by the viewport.
     *
     * Passing nullptr detaches the camera.
     */
    void SetCamera(const Camera* camera);

    /**
     * @brief Updates the screen-space rectangle of the viewport.
     *
     * All values are normalized [0,1].
     */
    void SetRect(float x, float y, float width, float height);

private:
    /** @brief Underlying viewport identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class Camera
 * @brief Camera owned by a Scene.
 */
/* ============================================================================
 *  CAMERA
 * ============================================================================ */

class HRL_CPP_API Camera {
public:
    /**
     * @brief Creates a camera in a scene.
     */
    explicit Camera(Scene& scene, ECameraType type);

    /**
     * @brief Destroys the camera.
     */
    ~Camera();

    /**
     * @brief Copy construction is disabled.
     */
    Camera(const Camera&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    Camera& operator=(const Camera&) = delete;

    /**
     * @brief Move-constructs a camera wrapper.
     */
    Camera(Camera&& other) noexcept;

    /**
     * @brief Move-assigns a camera wrapper.
     */
    Camera& operator=(Camera&& other) noexcept;

    /**
     * @brief Returns the underlying camera ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live camera object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the camera.
     */
    void Release();

    /**
     * @brief Changes the projection type of an existing camera at runtime.
     */
    void SetType(ECameraType type);

    /**
     * @brief Sets the vertical extent of an orthographic camera's view volume.
     *
     * @param height World-space height visible on screen.
     */
    void SetOrthoVertical(float height);

    /**
     * @brief Sets the vertical field of view for a perspective camera.
     *
     * @param fov Vertical field of view in degrees.
     */
    void SetPerspectiveFov(float fov);

    /**
     * @brief Sets the near clipping plane distance.
     *
     * Objects closer than this value will not be rendered.
     */
    void SetNearPlane(float nearPlane);

    /**
     * @brief Sets the far clipping plane distance.
     *
     * Objects farther than this value will not be rendered.
     */
    void SetFarPlane(float farPlane);

    /**
     * @brief Sets the world-space position of the camera.
     */
    void SetLocation(float x, float y, float z);

    /**
     * @brief Sets the orientation of the camera using Euler angles in degrees.
     *
     * Axis mapping: Pitch = X, Yaw = Y, Roll = Z.
     */
    void SetRotation(float pitch, float yaw, float roll);

private:
    /** @brief Underlying camera identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class Font
 * @brief Loaded TrueType font resource.
 */
/* ============================================================================
 *  FONTS
 * ============================================================================ */

class HRL_CPP_API Font {
public:
    /**
     * @brief Loads a TrueType font from a memory buffer.
     *
     * The font can be used with Texture's text-rasterization constructor.
     */
    Font(const void* data, std::size_t size);

    /**
     * @brief Destroys the font and releases its resources.
     */
    ~Font();

    /**
     * @brief Copy construction is disabled.
     */
    Font(const Font&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    Font& operator=(const Font&) = delete;

    /**
     * @brief Move-constructs a font wrapper.
     */
    Font(Font&& other) noexcept;

    /**
     * @brief Move-assigns a font wrapper.
     */
    Font& operator=(Font&& other) noexcept;

    /**
     * @brief Returns the underlying font ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live font object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the font.
     */
    void Release();

private:
    /** @brief Underlying font identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class Widget
 * @brief UI widget owned by a Viewport.
 */
/* ============================================================================
 *  UI
 * ============================================================================ */

class HRL_CPP_API Widget {
public:
    /**
     * @brief Creates a widget in a viewport.
     */
    Widget(Viewport& viewport, EWidgetType type);

    /**
     * @brief Destroys the widget.
     */
    ~Widget();

    /**
     * @brief Copy construction is disabled.
     */
    Widget(const Widget&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    Widget& operator=(const Widget&) = delete;

    /**
     * @brief Move-constructs a widget wrapper.
     */
    Widget(Widget&& other) noexcept;

    /**
     * @brief Move-assigns a widget wrapper.
     */
    Widget& operator=(Widget&& other) noexcept;

    /**
     * @brief Returns the underlying widget ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live widget object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the widget.
     */
    void Release();

    /**
     * @brief Sets the widget position.
     *
     * Widget positions are normalized to their owning viewport.
     */
    void SetPosition(float x, float y);

    /**
     * @brief Sets the widget size.
     *
     * The C API keeps the resulting pixel size across window resizes so aspect-ratio changes
     * do not stretch controls.
     */
    void SetSize(float width, float height);

    /**
     * @brief Sets the widget alpha value.
     */
    void SetAlpha(float alpha);

    /**
     * @brief Sets the widget anchor point.
     */
    void SetAnchor(float ax, float ay);

    /**
     * @brief Enables or disables widget visibility.
     */
    void SetVisible(bool visible);

    /**
     * @brief Enables or disables widget interaction.
     */
    void SetEnabled(bool enabled);

    /**
     * @brief Sets the widget Z index.
     */
    void SetZIndex(int zIndex);

    /**
     * @brief Returns whether the widget is currently hovered.
     */
    bool IsHovered() const;

    /**
     * @brief Enables or disables button clickability.
     */
    void SetButtonClickable(bool clickable);

    /**
     * @brief Sets the displayed button text.
     */
    void SetButtonText(const char* text);

    /**
     * @brief Sets the button text size.
     */
    void SetButtonTextSize(float size);

    /**
     * @brief Sets the button text tint color for a given interaction state.
     */
    void SetButtonTextTintColor(
        EWidgetState state, float r, float g, float b, float a);

    /**
     * @brief Sets the button text font.
     */
    void SetButtonTextFont(const Font& font);

    /**
     * @brief Sets the button background texture for a given interaction state.
     */
    void SetButtonBackgroundTexture(EWidgetState state, const Texture* texture);

    /**
     * @brief Sets the button background tint color for a given interaction state.
     */
    void SetButtonBackgroundTintColor(
        EWidgetState state, float r, float g, float b, float a);

    /**
     * @brief Registers the button pressed callback.
     *
     * @param callback Callback function, or nullptr to clear the callback.
     * @param userData User data passed back to the callback.
     */
    void SetButtonPressedCallback(ButtonPressedCallback callback, void* userData);

    /**
     * @brief Sets the displayed label text.
     */
    void SetLabelText(const char* text);

    /**
     * @brief Sets the label text size.
     */
    void SetLabelTextSize(float size);

    /**
     * @brief Sets the label font.
     */
    void SetLabelFont(const Font& font);

    /**
     * @brief Sets the label tint color.
     */
    void SetLabelTintColor(float r, float g, float b, float a);

    /**
     * @brief Sets the image texture.
     */
    void SetImageTexture(const Texture& texture);

    /**
     * @brief Sets the image tint color.
     */
    void SetImageTintColor(float r, float g, float b, float a);

    /**
     * @brief Sets the slider minimum and maximum range.
     */
    void SetSliderRange(float minimum, float maximum);

    /**
     * @brief Sets the slider value.
     */
    void SetSliderValue(float value);

    /**
     * @brief Returns the current slider value.
     */
    float GetSliderValue() const;

    /**
     * @brief Sets the slider orientation.
     */
    void SetSliderOrientation(ESliderOrientation orientation);

    /**
     * @brief Enables or disables slider clickability.
     */
    void SetSliderClickable(bool clickable);

    /**
     * @brief Sets the slider background color.
     */
    void SetSliderBackgroundColor(float r, float g, float b, float a);

    /**
     * @brief Sets the slider fill color.
     */
    void SetSliderFillColor(float r, float g, float b, float a);

    /**
     * @brief Sets the slider handle color.
     */
    void SetSliderHandleColor(float r, float g, float b, float a);

    /**
     * @brief Registers the slider value-changed callback.
     */
    void SetSliderChangedCallback(SliderChangedCallback callback, void* userData);

    /**
     * @brief Sets the checkbox checked state.
     */
    void SetCheckboxChecked(bool checked);

    /**
     * @brief Returns whether the checkbox is checked.
     */
    bool IsCheckboxChecked() const;

    /**
     * @brief Enables or disables checkbox clickability.
     */
    void SetCheckboxClickable(bool clickable);

    /**
     * @brief Sets the checkbox background color.
     */
    void SetCheckboxBackgroundColor(float r, float g, float b, float a);

    /**
     * @brief Sets the color used for the checked state.
     */
    void SetCheckboxCheckedColor(float r, float g, float b, float a);

    /**
     * @brief Registers the checkbox value-changed callback.
     */
    void SetCheckboxChangedCallback(CheckboxChangedCallback callback, void* userData);

    /**
     * @brief Sets the progress-bar value.
     */
    void SetProgressBarValue(float value);

    /**
     * @brief Returns the progress-bar value.
     */
    float GetProgressBarValue() const;

    /**
     * @brief Sets the progress-bar background color.
     */
    void SetProgressBarBackgroundColor(float r, float g, float b, float a);

    /**
     * @brief Sets the progress-bar fill color.
     */
    void SetProgressBarFillColor(float r, float g, float b, float a);

private:
    /** @brief Underlying widget identifier. */
    id id_ = INVALID_ID;

    /** @brief Internal button callback state retained by the wrapper. */
    void* buttonCallbackState_ = nullptr;
};

/**
 * @class Gizmo
 * @brief Editor gizmo attached to a viewport.
 *
 * Gizmos are independent of HRL scene objects and use the same world coordinate
 * system as meshes, lights and cameras.
 */
/* ============================================================================
 *  GIZMOS
 * ============================================================================ */

class HRL_CPP_API Gizmo {
public:
    /**
     * @brief Creates an editor gizmo attached to a viewport.
     */
    explicit Gizmo(Viewport& viewport);

    /**
     * @brief Destroys the gizmo.
     */
    ~Gizmo();

    /**
     * @brief Copy construction is disabled.
     */
    Gizmo(const Gizmo&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    Gizmo& operator=(const Gizmo&) = delete;

    /**
     * @brief Move-constructs a gizmo wrapper.
     */
    Gizmo(Gizmo&& other) noexcept;

    /**
     * @brief Move-assigns a gizmo wrapper.
     */
    Gizmo& operator=(Gizmo&& other) noexcept;

    /**
     * @brief Returns the underlying gizmo ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live gizmo.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the gizmo.
     */
    void Release();

    /**
     * @brief Sets the gizmo position in world space.
     */
    void SetPosition(float x, float y, float z);

    /**
     * @brief Writes the gizmo position into caller-provided values.
     */
    void GetPosition(float& x, float& y, float& z) const;

    /**
     * @brief Sets the gizmo rotation using Euler angles in degrees.
     */
    void SetRotation(float pitch, float yaw, float roll);

    /**
     * @brief Writes the gizmo rotation into caller-provided values.
     */
    void GetRotation(float& pitch, float& yaw, float& roll) const;

    /**
     * @brief Sets the gizmo scale.
     */
    void SetScale(float x, float y, float z);

    /**
     * @brief Writes the gizmo scale into caller-provided values.
     */
    void GetScale(float& x, float& y, float& z) const;

    /**
     * @brief Sets the gizmo transform mode flags.
     *
     * Multiple mode flags may be combined.
     */
    void SetMode(EGizmoMode mode);

    /**
     * @brief Convenience overload for combined gizmo mode flags.
     *
     * This overload accepts the integral result of bitwise OR expressions such as
     * HRL_GIZMO_MODE_TRANSLATE | HRL_GIZMO_MODE_ROTATE.
     */
    void SetMode(uint mode);

    /**
     * @brief Returns the current gizmo transform mode flags.
     */
    EGizmoMode GetMode() const;

    /**
     * @brief Sets the gizmo coordinate space.
     */
    void SetSpace(EGizmoSpace space);

    /**
     * @brief Returns the gizmo coordinate space.
     */
    EGizmoSpace GetSpace() const;

    /**
     * @brief Shows or hides the translation handles.
     */
    void SetTranslateVisible(bool visible);

    /**
     * @brief Shows or hides the rotation handles.
     */
    void SetRotateVisible(bool visible);

    /**
     * @brief Shows or hides the scale handles.
     */
    void SetScaleVisible(bool visible);

    /**
     * @brief Sets the visible translation axes using EGizmoAxis flags.
     */
    void SetTranslateAxes(int axisMask);

    /**
     * @brief Sets the visible rotation axes using EGizmoAxis flags.
     */
    void SetRotateAxes(int axisMask);

    /**
     * @brief Sets the visible scale axes using EGizmoAxis flags.
     */
    void SetScaleAxes(int axisMask);

    /**
     * @brief Sets the gizmo world-space size.
     */
    void SetSize(float worldSize);

    /**
     * @brief Sets the gizmo screen-space size in pixels.
     */
    void SetScreenSize(float pixels);

    /**
     * @brief Enables or disables screen-space sizing.
     */
    void SetUseScreenSize(bool useScreenSize);

    /**
     * @brief Shows or hides the gizmo.
     */
    void SetVisible(bool visible);

    /**
     * @brief Enables or disables gizmo interaction.
     */
    void SetEnabled(bool enabled);

    /**
     * @brief Sets the color of an axis.
     *
     * @param axis Gizmo axis selector.
     * @param r,g,b,a RGBA color.
     */
    void SetAxisColor(int axis, float r, float g, float b, float a);

    /**
     * @brief Sets the gizmo center color.
     */
    void SetCenterColor(float r, float g, float b, float a);

    /**
     * @brief Sets the gizmo hover color.
     */
    void SetHoverColor(float r, float g, float b, float a);

    /**
     * @brief Returns the currently hovered gizmo part.
     */
    EGizmoPart GetHoveredPart() const;

    /**
     * @brief Returns the currently active gizmo part.
     */
    EGizmoPart GetActivePart() const;

    /**
     * @brief Returns the currently hovered gizmo operation.
     */
    EGizmoOperation GetHoveredOperation() const;

    /**
     * @brief Returns the currently active gizmo operation.
     */
    EGizmoOperation GetActiveOperation() const;

    /**
     * @brief Sets the visible rotation arc size in degrees.
     */
    void SetRotateArcDegrees(float degrees);

    /**
     * @brief Registers a gizmo interaction callback.
     */
    void SetChangedCallback(GizmoChangedCallback callback, void* userData);

private:
    /** @brief Underlying gizmo identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class VFXSystem
 * @brief Particle/VFX system owned by a Scene.
 */
/* ============================================================================
 *  VFX / PARTICLE SYSTEMS
 * ============================================================================ */

class HRL_CPP_API VFXSystem {
public:
    /**
     * @brief Creates a VFX system in a scene.
     */
    explicit VFXSystem(Scene& scene);

    /**
     * @brief Destroys the VFX system.
     */
    ~VFXSystem();

    /**
     * @brief Copy construction is disabled.
     */
    VFXSystem(const VFXSystem&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    VFXSystem& operator=(const VFXSystem&) = delete;

    /**
     * @brief Move-constructs a VFX system wrapper.
     */
    VFXSystem(VFXSystem&& other) noexcept;

    /**
     * @brief Move-assigns a VFX system wrapper.
     */
    VFXSystem& operator=(VFXSystem&& other) noexcept;

    /**
     * @brief Returns the underlying VFX system ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live VFX system.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the VFX system.
     */
    void Release();

    /**
     * @brief Sets the VFX system world-space position.
     */
    void SetPosition(float x, float y, float z);

    /**
     * @brief Retrieves the VFX system world-space position.
     */
    void GetPosition(float& x, float& y, float& z) const;

    /**
     * @brief Sets the VFX system rotation using Euler angles in degrees.
     */
    void SetRotation(float pitch, float yaw, float roll);

    /**
     * @brief Retrieves the VFX system rotation.
     */
    void GetRotation(float& pitch, float& yaw, float& roll) const;

    /**
     * @brief Sets the VFX system scale.
     */
    void SetScale(float x, float y, float z);

    /**
     * @brief Retrieves the VFX system scale.
     */
    void GetScale(float& x, float& y, float& z) const;

    /**
     * @brief Starts VFX playback.
     */
    void Play();

    /**
     * @brief Stops VFX playback.
     */
    void Stop();

    /**
     * @brief Pauses VFX playback.
     */
    void Pause();

    /**
     * @brief Resets the VFX system.
     */
    void Reset();

    /**
     * @brief Enables or disables system looping.
     */
    void SetLooping(bool looping);

    /**
     * @brief Sets the VFX time scale.
     */
    void SetTimeScale(float scale);

    /**
     * @brief Enables or disables the VFX system.
     */
    void SetEnabled(bool enabled);

    /**
     * @brief Enables or disables automatic update for the VFX system.
     */
    void SetAutoUpdate(bool autoUpdate);

    /**
     * @brief Sets the system duration.
     */
    void SetDuration(float duration);

    /**
     * @brief Creates an emitter owned by this VFX system.
     */
    VFXEmitter CreateEmitter();

private:
    /** @brief Underlying VFX system identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class VFXEmitter
 * @brief Particle emitter belonging to a VFX system.
 */
class HRL_CPP_API VFXEmitter {
public:
    /**
     * @brief Creates an emitter owned by a VFX system.
     */
    explicit VFXEmitter(VFXSystem& system);

    /**
     * @brief Destroys the emitter.
     */
    ~VFXEmitter();

    /**
     * @brief Copy construction is disabled.
     */
    VFXEmitter(const VFXEmitter&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    VFXEmitter& operator=(const VFXEmitter&) = delete;

    /**
     * @brief Move-constructs an emitter wrapper.
     */
    VFXEmitter(VFXEmitter&& other) noexcept;

    /**
     * @brief Move-assigns an emitter wrapper.
     */
    VFXEmitter& operator=(VFXEmitter&& other) noexcept;

    /**
     * @brief Returns the underlying emitter ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live emitter.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the emitter.
     */
    void Release();

    /**
     * @brief Enables or disables the emitter.
     */
    void SetEnabled(bool enabled);

    /**
     * @brief Sets the emitter world-space position.
     */
    void SetPosition(float x, float y, float z);

    /**
     * @brief Sets the emitter rotation using Euler angles in degrees.
     */
    void SetRotation(float pitch, float yaw, float roll);

    /**
     * @brief Sets the maximum particle count.
     */
    void SetMaxParticles(uint maxParticles);

    /**
     * @brief Sets the particle spawn rate.
     */
    void SetSpawnRate(float particlesPerSecond);

    /**
     * @brief Sets the emitter's immediate burst count.
     */
    void SetBurst(uint count);

    /**
     * @brief Adds a burst event at a given system time.
     */
    void AddBurst(float time, uint count);

    /**
     * @brief Clears all scheduled burst events.
     */
    void ClearBursts();

    /**
     * @brief Sets the minimum and maximum particle lifetime.
     */
    void SetLifetime(float minLifetime, float maxLifetime);

    /**
     * @brief Sets the particle spawn shape.
     */
    void SetSpawnShape(EVFXSpawnShape shape);

    /**
     * @brief Sets the radius used by the spawn shape.
     */
    void SetShapeRadius(float radius);

    /**
     * @brief Sets the size used by the spawn shape.
     */
    void SetShapeSize(float x, float y, float z);

    /**
     * @brief Sets the angle used by the spawn shape.
     */
    void SetShapeAngle(float degrees);

    /**
     * @brief Sets the initial velocity range.
     */
    void SetInitialVelocity(
        float minX, float minY, float minZ,
        float maxX, float maxY, float maxZ);

    /**
     * @brief Sets the initial speed range.
     */
    void SetInitialSpeed(float minSpeed, float maxSpeed);

    /**
     * @brief Sets the initial rotation range.
     */
    void SetInitialRotation(
        float minX, float minY, float minZ,
        float maxX, float maxY, float maxZ);

    /**
     * @brief Sets the angular velocity range.
     */
    void SetAngularVelocity(
        float minX, float minY, float minZ,
        float maxX, float maxY, float maxZ);

    /**
     * @brief Sets the gravity vector.
     */
    void SetGravity(float x, float y, float z);

    /**
     * @brief Sets particle drag.
     */
    void SetDrag(float drag);

    /**
     * @brief Sets an external force vector.
     */
    void SetForce(float x, float y, float z);

    /**
     * @brief Sets the noise strength, frequency and scroll speed.
     */
    void SetNoise(float strength, float frequency, float scrollSpeed);

    /**
     * @brief Sets the particle render mode.
     */
    void SetRenderMode(EVFXRenderMode mode);

    /**
     * @brief Sets the texture used by the emitter.
     */
    void SetTexture(const Texture& texture);

    /**
     * @brief Sets the material used by the emitter.
     */
    void SetMaterial(const Material& material);

    /**
     * @brief Sets the mesh used by the emitter in mesh-rendering mode.
     */
    void SetMesh(const Mesh& mesh);

    /**
     * @brief Sets the particle mesh scale.
     */
    void SetMeshScale(float x, float y, float z);

    /**
     * @brief Sets the particle mesh rotation.
     */
    void SetMeshRotation(float pitch, float yaw, float roll);

    /**
     * @brief Sets the particle blending mode.
     */
    void SetBlendMode(EVFXBlendMode mode);

    /**
     * @brief Sets the particle billboard size.
     */
    void SetParticleSize(float x, float y);

    /**
     * @brief Sets the stretched-billboard amount.
     */
    void SetStretch(float amount);

    /**
     * @brief Sets the particle simulation space.
     */
    void SetSimulationSpace(EVFXSimulationSpace space);

    /**
     * @brief Creates a color curve owned by the emitter.
     */
    VFXCurve CreateColorCurve();

    /**
     * @brief Creates a float curve owned by the emitter.
     */
    VFXCurve CreateFloatCurve();

    /**
     * @brief Assigns a color curve to the emitter.
     */
    void SetColorCurve(const VFXCurve& curve);

    /**
     * @brief Assigns a size curve to the emitter.
     */
    void SetSizeCurve(const VFXCurve& curve);

    /**
     * @brief Assigns a rotation curve to the emitter.
     */
    void SetRotationCurve(const VFXCurve& curve);

    /**
     * @brief Enables or disables VFX collision.
     */
    void SetCollisionEnabled(bool enabled);

    /**
     * @brief Sets collision restitution.
     */
    void SetCollisionRestitution(float restitution);

    /**
     * @brief Sets collision friction.
     */
    void SetCollisionFriction(float friction);

    /**
     * @brief Enables or disables collision against a scene.
     */
    void SetCollisionScene(Scene& scene, bool enabled);

protected:
    /**
     * @brief Constructs an owning emitter wrapper around an existing HRL emitter ID.
     */
    explicit VFXEmitter(id value) : id_(value) {}

    /** @brief Allows VFXSystem to adopt newly-created emitter IDs. */
    friend class VFXSystem;

private:
    /** @brief Underlying VFX emitter identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class VFXCurve
 * @brief Curve resource used to animate VFX emitter properties.
 */
class HRL_CPP_API VFXCurve {
public:
    /**
     * @brief Creates a non-owning wrapper around an existing curve ID.
     */
    static VFXCurve Borrowed(id value);

    /**
     * @brief Creates a color curve owned by an emitter.
     */
    static VFXCurve CreateColor(VFXEmitter& emitter);

    /**
     * @brief Creates a float curve owned by an emitter.
     */
    static VFXCurve CreateFloat(VFXEmitter& emitter);

    /**
     * @brief Destroys the curve.
     */
    ~VFXCurve();

    /**
     * @brief Copy construction is disabled.
     */
    VFXCurve(const VFXCurve&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    VFXCurve& operator=(const VFXCurve&) = delete;

    /**
     * @brief Move-constructs a curve wrapper.
     */
    VFXCurve(VFXCurve&& other) noexcept;

    /**
     * @brief Move-assigns a curve wrapper.
     */
    VFXCurve& operator=(VFXCurve&& other) noexcept;

    /**
     * @brief Returns the underlying curve ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live curve.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the curve.
     */
    void Release();

    /**
     * @brief Adds a color key to the curve.
     */
    void AddColorKey(float time, float r, float g, float b, float a);

    /**
     * @brief Adds a float key to the curve.
     */
    void AddFloatKey(float time, float value);

    /**
     * @brief Removes all color keys from the curve.
     */
    void ClearColorKeys();

    /**
     * @brief Removes all float keys from the curve.
     */
    void ClearFloatKeys();

private:
    /**
     * @brief Constructs a curve wrapper around an existing ID.
     */
    explicit VFXCurve(id value, bool owned);

    /** @brief Underlying VFX curve identifier. */
    id id_ = INVALID_ID;

    /** @brief Whether this wrapper owns the curve resource. */
    bool owned_ = false;

    /** @brief Allows VFXEmitter to adopt newly-created curve IDs. */
    friend class VFXEmitter;
};

/**
 * @class VolumetricFog
 * @brief Localized volumetric fog volume attached to a Scene.
 */
/* ============================================================================
 *  EFFECTS / VOLUMETRIC FOG
 * ============================================================================ */

class HRL_CPP_API VolumetricFog {
public:
    /**
     * @brief Creates a localized volumetric fog volume in a scene.
     */
    explicit VolumetricFog(Scene& scene);

    /**
     * @brief Destroys the volumetric fog volume.
     */
    ~VolumetricFog();

    /**
     * @brief Copy construction is disabled.
     */
    VolumetricFog(const VolumetricFog&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    VolumetricFog& operator=(const VolumetricFog&) = delete;

    /**
     * @brief Move-constructs a volumetric fog wrapper.
     */
    VolumetricFog(VolumetricFog&& other) noexcept;

    /**
     * @brief Move-assigns a volumetric fog wrapper.
     */
    VolumetricFog& operator=(VolumetricFog&& other) noexcept;

    /**
     * @brief Returns the underlying volumetric fog ID.
     */
    id GetID() const noexcept;

    /**
     * @brief Returns whether the wrapped ID refers to a live volumetric fog object.
     */
    bool IsValid() const;

    /**
     * @brief Destroys the volumetric fog volume.
     */
    void Release();

    /**
     * @brief Enables or disables the volumetric fog volume.
     */
    void SetEnabled(bool enabled);

    /**
     * @brief Sets the center of the localized volumetric fog volume in world space.
     */
    void SetPosition(float x, float y, float z);

    /**
     * @brief Sets the radius of the localized volumetric fog volume.
     */
    void SetRadius(float radius);

    /**
     * @brief Sets the density of the localized volumetric fog volume.
     */
    void SetDensity(float density);

    /**
     * @brief Sets the color of the localized volumetric fog volume.
     */
    void SetColor(float r, float g, float b);

    /**
     * @brief Sets the ray-march sample count for the localized volumetric fog.
     *
     * The C API documents a range of 4..64.
     */
    void SetSteps(uint steps);

protected:
    /**
     * @brief Constructs an owning volumetric fog wrapper around an existing HRL ID.
     */
    explicit VolumetricFog(id value) : id_(value) {}

    /** @brief Allows Scene to adopt newly-created volumetric fog IDs. */
    friend class Scene;

private:
    /** @brief Underlying volumetric fog identifier. */
    id id_ = INVALID_ID;
};

/**
 * @class FBXResources
 * @brief Opaque decoded FBX material and texture resource collection.
 *
 * Geometry is not imported by this resource wrapper.
 */
class HRL_CPP_API FBXResources {
public:
    /**
     * @brief Loads FBX materials and textures without importing geometry.
     *
     * Embedded texture bytes are exposed through FBXTextureInfo.
     */
    FBXResources(const void* data, std::size_t size);

    /**
     * @brief Destroys the decoded FBX resource collection.
     */
    ~FBXResources();

    /**
     * @brief Copy construction is disabled.
     */
    FBXResources(const FBXResources&) = delete;

    /**
     * @brief Copy assignment is disabled.
     */
    FBXResources& operator=(const FBXResources&) = delete;

    /**
     * @brief Move-constructs an FBX resource wrapper.
     */
    FBXResources(FBXResources&& other) noexcept;

    /**
     * @brief Move-assigns an FBX resource wrapper.
     */
    FBXResources& operator=(FBXResources&& other) noexcept;

    /**
     * @brief Returns whether the FBX resource collection is valid.
     */
    bool IsValid() const;

    /**
     * @brief Releases the decoded FBX resource collection.
     */
    void Release();

    /**
     * @brief Returns the number of FBX materials.
     */
    std::size_t GetMaterialCount() const;

    /**
     * @brief Returns read-only material metadata.
     *
     * The returned pointer remains valid while this FBXResources object is alive.
     */
    const FBXMaterialInfo* GetMaterial(std::size_t index) const;

    /**
     * @brief Finds a material by FBX name.
     *
     * @return Material index, or INVALID_ID if none exists.
     */
    id FindMaterial(const char* name) const;

    /**
     * @brief Returns the number of FBX textures.
     */
    std::size_t GetTextureCount() const;

    /**
     * @brief Returns read-only texture metadata and embedded bytes.
     */
    const FBXTextureInfo* GetTexture(std::size_t index) const;

    /**
     * @brief Finds a texture by FBX name or filename.
     *
     * @return Texture index, or INVALID_ID if none exists.
     */
    id FindTexture(const char* name) const;

    /**
     * @brief Returns the texture associated with a material texture slot.
     */
    const FBXTextureInfo* GetMaterialTexture(
        std::size_t materialIndex,
        EFBXMaterialTextureSlot slot) const;

    /**
     * @brief Creates a GPU texture from an embedded FBX texture resource.
     *
     * @return Texture resource identifier, or INVALID_ID on failure.
     */
    Texture CreateTexture(std::size_t textureIndex) const;

private:
    /** @brief Opaque HRL FBX resource handle. */
    void* resources_ = nullptr;
};

/**
 * @brief Reserved generic mesh entry point.
 *
 * This prototype does not carry enough information to describe a bounded vertex buffer
 * and remains reserved. Use StaticMesh for static 3D geometry.
 */
HRL_CPP_API Mesh CreateMesh(Scene& scene, EMeshType type, const float* vertices);

/**
 * @brief Reserved generic file-based mesh entry point.
 *
 * This entry point remains reserved. FBX conversion is intentionally exposed separately
 * through GetVertex3DFromFBX().
 */
HRL_CPP_API Mesh CreateMeshFromFile(
    Scene& scene, EMeshType type, const void* data, std::size_t size);

/**
 * @brief Creates a material from the first FBX material.
 *
 * Automatically selects the built-in static or skinned 3D shader from the FBX contents.
 */
HRL_CPP_API Material CreateMaterialFromFBX(const void* data, std::size_t size);

/**
 * @brief Creates a material from the first FBX material using a caller-selected shader.
 */
HRL_CPP_API Material CreateMaterialFromFBXWithShader(
    const void* data, std::size_t size, const Shader& shader);

/**
 * @brief Creates one indexed FBX material and automatically selects the built-in static
 * or skinned 3D shader from the FBX contents.
 */
HRL_CPP_API Material CreateMaterialFromFBXIndexed(
    const void* data, std::size_t size, std::size_t materialIndex);

/**
 * @brief Creates one indexed FBX material using a caller-selected shader.
 */
HRL_CPP_API Material CreateMaterialFromFBXIndexedWithShader(
    const void* data, std::size_t size, std::size_t materialIndex, const Shader& shader);

/**
 * @brief Converts an FBX mesh scene to a flat Vertex3D array.
 *
 * The FBX file is decoded internally by HRL and the resulting geometry is returned as
 * non-indexed triangles. Node transforms are baked into the returned vertex positions
 * and tangent space.
 *
 * The returned buffer must be released with FreeVertex3DFromFBX().
 */
/* ============================================================================
 *  FBX CONVERSION UTILITIES
 * ============================================================================ */

HRL_CPP_API Vertex3D* GetVertex3DFromFBX(
    const void* data, std::size_t size, std::size_t& vertexCount);

/**
 * @brief Frees the buffer returned by GetVertex3DFromFBX().
 */
HRL_CPP_API void FreeVertex3DFromFBX(Vertex3D* vertices);

/**
 * @brief Converts the first skinned FBX mesh in an in-memory FBX buffer.
 *
 * Geometry, bones, weights and baked animation samples are copied into an HRL-owned
 * CPU data structure.
 */
HRL_CPP_API SkeletalMeshData* GetSkeletalMeshFromFBX(
    const void* data, std::size_t size);

/**
 * @brief Frees all nested allocations returned by GetSkeletalMeshFromFBX().
 */
HRL_CPP_API void FreeSkeletalMeshData(SkeletalMeshData* data);

/**
 * @brief Advances all playing skeletal mesh animations by deltaSeconds.
 */
/* ============================================================================
 *  UPDATE & INPUT
 * ============================================================================ */

HRL_CPP_API void UpdateSkeletalAnimations(float deltaSeconds);

/**
 * @brief Advances VFX systems that have AutoUpdate disabled.
 */
HRL_CPP_API void UpdateVFX(float deltaSeconds);

/**
 * @brief Waits for all queued asynchronous resources and uploads their completed GPU objects.
 */
HRL_CPP_API void WaitForAllAsyncResources();

/**
 * @brief Updates the renderer's mouse position.
 *
 * Coordinates are window-relative pixels.
 */
HRL_CPP_API void MouseMoved(float x, float y);

/**
 * @brief Notifies HRL of a mouse button transition.
 *
 * @param button One of MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT or MOUSE_BUTTON_MIDDLE.
 * @param pressed One of MOUSE_RELEASE or MOUSE_PRESS.
 */
HRL_CPP_API void MouseButtonCallback(int button, int pressed);

/* ============================================================================
 *  GLOBAL ILLUMINATION QUERIES
 * ============================================================================ */

/**
 * @brief Returns whether the active backend implements the requested GI method.
 */
HRL_CPP_API bool IsGlobalIlluminationMethodSupported(
    EGlobalIlluminationMethod method);

/**
 * @brief Returns the bitmask of global illumination methods supported by the active backend.
 *
 * The bit corresponding to a method is represented by (1u << method).
 */
HRL_CPP_API uint GetGlobalIlluminationSupportedMethods();

/**
 * @brief Clears the entire screen to its default clear color.
 *
 * Useful when no scene covers the full framebuffer.
 */
HRL_CPP_API void ClearScreen();

/**
 * @brief Enables or changes the global multisample anti-aliasing mode.
 */
HRL_CPP_API void SetAntialiasingMode(EAntialiasingMode mode);

/**
 * @brief Enables or changes the global multisample anti-aliasing mode.
 *
 * @param mode Integral mode value matching the HRL antialiasing constants.
 */
HRL_CPP_API void SetAntialiasingMode(uint mode);

/* ============================================================================
 *  MATRICES
 * ============================================================================ */

/**
 * @brief Writes the current projection matrix into a caller-provided array.
 *
 * The matrix is column-major and contiguous.
 */
HRL_CPP_API void GetProjectionMatrix(float (&matrix)[16]);

/**
 * @brief Writes the current view matrix into a caller-provided array.
 *
 * The matrix is column-major and contiguous.
 */
HRL_CPP_API void GetViewMatrix(float (&matrix)[16]);

/**
 * @brief Writes the model matrix of a specific mesh into a caller-provided array.
 *
 * The matrix is column-major and contiguous.
 */
HRL_CPP_API void GetModelMatrix(const Mesh& mesh, float (&matrix)[16]);

/* ============================================================================
 *  DEBUG GEOMETRY
 * ============================================================================ */

/**
 * @brief Sets the line thickness used by all debug draw calls.
 *
 * The exact visual result depends on backend line rendering support.
 */
HRL_CPP_API void SetDebugLineThickness(float thickness);

/**
 * @brief Draws a debug line segment for the current frame.
 *
 * Must be called every frame to persist the rendering.
 * The segment is expressed in world space and the color components are in [0,1].
 */
HRL_CPP_API void DrawDebugSegment(
    const Scene& scene,
    float ax, float ay, float az,
    float bx, float by, float bz,
    float r, float g, float b);

/**
 * @brief Draws a debug polygon for the current frame.
 *
 * Vertices are specified as separate X, Y and Z arrays of the same length.
 */
HRL_CPP_API void DrawDebugPolygon(
    const Scene& scene,
    EDebugRenderingType mode,
    const float* xs, const float* ys, const float* zs,
    int count,
    float r, float g, float b);

/**
 * @brief Draws a debug circle for the current frame.
 */
HRL_CPP_API void DrawDebugCircle(
    const Scene& scene,
    EDebugRenderingType mode,
    float cx, float cy, float cz,
    float radius, int segments,
    float r, float g, float b);

/**
 * @brief Draws a debug capsule for the current frame.
 *
 * The capsule is a cylinder with hemispherical caps.
 */
HRL_CPP_API void DrawDebugCapsule(
    const Scene& scene,
    EDebugRenderingType mode,
    float ax, float ay, float az,
    float bx, float by, float bz,
    float radius, int segments,
    float r, float g, float b);

/**
 * @brief Draws a debug point for the current frame.
 *
 * The point size is expressed in pixels.
 */
HRL_CPP_API void DrawDebugPoint(
    const Scene& scene,
    float x, float y, float z,
    float size,
    float r, float g, float b);

} // namespace hrl

#endif // HRL_HPP
