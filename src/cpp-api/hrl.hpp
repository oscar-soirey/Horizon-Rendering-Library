/**
 * Horizon Rendering Library - Official C++ API
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

using id = std::uint32_t;
using uint = std::uint32_t;

inline constexpr id INVALID_ID = static_cast<id>(-1);
inline constexpr uint SKELETAL_MAX_INFLUENCES = 4;
inline constexpr uint MAX_SKELETAL_BONES = 128;
inline constexpr id SPRITE_SHADER = static_cast<id>(0xFFFFFFFFu);
inline constexpr id MESH_2D_SHADER = static_cast<id>(0xFFFFFFFEu);
inline constexpr id MESH_3D_SHADER = static_cast<id>(0xFFFFFFFDu);
inline constexpr id DEBUG_SHADER = static_cast<id>(0xFFFFFFFCu);
inline constexpr id DEFAULT_POST_PROCESS_SHADER = static_cast<id>(0xFFFFFFFBu);
inline constexpr id SKINNED_3D_MESH_SHADER = static_cast<id>(0xFFFFFFFAu);
inline constexpr int FALSE_VALUE = 0;
inline constexpr int TRUE_VALUE = 1;
inline constexpr const char* T_ALBEDO = "T_Albedo";
inline constexpr const char* T_NORMAL = "T_Normal";
inline constexpr const char* T_SPECULAR = "T_Specular";
inline constexpr const char* T_ROUGHNESS = "T_Roughness";
inline constexpr const char* T_METALLIC = "T_Metallic";
inline constexpr const char* T_AMBIENT_OCCLUSION = "T_AO";
inline constexpr const char* T_ALPHA = "T_Alpha";
inline constexpr const char* T_EMISSIVE = "T_Emissive";
inline constexpr const char* T_SHADOW_MAP = "T_ShadowMap";
inline constexpr const char* T_CUBE_MAP = "T_CubeMap";

struct Vertex3D {
    float position[3];
    float normal[3];
    float uv[2];
    float tangent[3];
    float bitangent[3];
};

struct SkeletalVertex {
    Vertex3D vertex;
    uint boneIndices[SKELETAL_MAX_INFLUENCES];
    float boneWeights[SKELETAL_MAX_INFLUENCES];
};

struct SkeletalBoneTransform {
    float translation[3];
    float rotation[4];
    float scale[3];
};

struct SkeletalBone {
    const char* name;
    uint parentIndex;
    SkeletalBoneTransform bindTransform;
    float inverseBindMatrix[16];
};

struct SkeletalAnimation {
    const char* name;
    float duration;
    float frameRate;
    std::size_t frameCount;
    SkeletalBoneTransform* frames;
};

struct SkeletalMeshData {
    SkeletalVertex* vertices;
    std::size_t vertexCount;
    uint* indices;
    std::size_t indexCount;
    SkeletalBone* bones;
    std::size_t boneCount;
    SkeletalAnimation* animations;
    std::size_t animationCount;
};

enum E_APIs {
    HRL_OPENGL_33 = 0x0001,
    HRL_OPENGL_45,
    HRL_VULKAN,
    HRL_D3D11,
    HRL_D3D12,
    HRL_METAL,
    HRL_NVN,
    HRL_GNM
};

enum ELightType {
    HRL_POINT_LIGHT = 0x0011,
    HRL_DIRECTIONAL_LIGHT,
    HRL_SPOT_LIGHT
};

enum EMeshType {
    HRL_SPRITE = 0x0021,
    HRL_2D_MESH,
    HRL_3D_MESH,
    HRL_3D_SKELETAL_MESH
};

enum EDebugRenderingType {
    HRL_DEBUG_HOLLOW = 0x0031,
    HRL_DEBUG_SOLID
};

enum ECameraType {
    HRL_ORTHO = 0x0041,
    HRL_PERSPECTIVE
};

enum EFilterType {
    HRL_FILTER_NEAREST = 0x0050,
    HRL_FILTER_LINEAR,
    HRL_FILTER_BILINEAR,
    HRL_FILTER_TRILINEAR,
    HRL_FILTER_ANISOTROPIC,
    /** Not avalaible with OpenGL backends */
    HRL_FILTER_SUPERSAMPLING
};

enum EAntialiasingMode {
    HRL_ANTIALIASING_OFF = 0,
    HRL_ANTIALIASING_2X = 2,
    HRL_ANTIALIASING_4X = 4,
    HRL_ANTIALIASING_8X = 8
};

enum EDebugView {
    HRL_DEBUG_VIEW_NONE = 0x0060,
    HRL_DEBUG_VIEW_UNLIT,
    HRL_DEBUG_VIEW_NORMAL,
    HRL_DEBUG_VIEW_LIGHTS,
    HRL_DEBUG_VIEW_LIGHTING = HRL_DEBUG_VIEW_LIGHTS,
    HRL_DEBUG_VIEW_WIREFRAME,
    HRL_DEBUG_VIEW_LOD
};

enum ELODMode {
    HRL_LOD_DISTANCE = 0,
    HRL_LOD_SCREEN_SIZE
};

enum EError {
    HRL_NO_ERROR = 0x0070,
    HRL_ERROR_INVALID_ID,
    HRL_INVALID_ENUM,
    HRL_INVALID_VALUE,
    HRL_INVALID_OPERATION,
    HRL_INVALID_BACKEND_OPERATION,
    HRL_SHADER_COMPILE_FAIL,
    HRL_OUT_OF_MEMORY,
    HRL_INVALID_FILE_FORMAT
};

enum ESeverity {
    HRL_SEVERITY_WEAK_WARNING = 0x0080,
    HRL_SEVERITY_WARNING,
    HRL_SEVERITY_ERROR,
    HRL_SEVERITY_FATAL
};

enum EFogType {
    HRL_FOG_LINEAR = 0x0090,
    HRL_FOG_EXPONENTIAL,
    HRL_FOG_EXP_SQUARED
};

enum EWidgetState {
    HRL_WIDGET_STATE_IDLE = (1 << 0),
    HRL_WIDGET_STATE_HOVERED = (1 << 1),
    HRL_WIDGET_STATE_PRESSED = (1 << 2)
};

enum EWidgetType {
    HRL_WIDGET_BUTTON = 0x00A0,
    HRL_WIDGET_LABEL,
    HRL_WIDGET_IMAGE,
    HRL_WIDGET_SLIDER,
    HRL_WIDGET_CHECKBOX,
    HRL_WIDGET_PROGRESSBAR
};

using ErrorCallback = void(*)(EError code, ESeverity severity, const char* detail);
using ButtonPressedCallback = void(*)(id button, int clicked, int released, void* userData);

class Instance;
class Mesh;
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

class HRL_CPP_API Instance {
public:
/**
	 * @brief Selects the graphics backend to use. Must be called before HRL_InitContext.
	 * @param _api One of the HRL_* API constants (e.g. HRL_OPENGL_45, HRL_VULKAN).
	 */
/**
	 * @brief Initializes the rendering context for the given window dimensions.
	 * @param _width  Initial framebuffer width in pixels.
	 * @param _height Initial framebuffer height in pixels.
	 * @param _loader Platform-specific function loader (eg. glfwGetProcAddress).
	 */
    Instance(E_APIs api, uint width, uint height, void* loader);
/** Releases the rendering context. */
    ~Instance();

/** Copy construction is disabled for this resource wrapper. */
    Instance(const Instance&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Instance& operator=(const Instance&) = delete;
/** Move construction is disabled for this resource wrapper. */
    Instance(Instance&&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Instance& operator=(Instance&&) = delete;
/**
	 * @brief Prepares the renderer for a new frame. Call at the start of your render loop.
	 * Clears internal per-frame state and begins command recording.
	 */
    void BeginFrame();
/**
	 * @brief Finalizes and submits the current frame. Call at the end of your render loop.
	 * Flushes all draw commands and swaps buffers if applicable.
	 */
    void EndFrame();
/**
	 * @brief Notifies HRL of a window resize. Call from your window resize callback.
	 * @param _width  New framebuffer width in pixels.
	 * @param _height New framebuffer height in pixels.
	 */
    void WindowResizeCallback(int width, int height);
/**
	 * @brief Retrieves the last recorded error, if any.
	 * @param _detail   Output pointer to a human-readable description string.
	 * @param _severity Output severity level of the error.
	 * @return The HRL_Error code. Returns HRL_NO_ERROR if no error occurred.
	 */
    EError GetLastError(const char** detail = nullptr, ESeverity* severity = nullptr) const;
/**
	 * @brief Converts an HRL_Error enum value to its string representation.
	 * @param err The error code to convert.
	 * @return A null-terminated string literal (e.g. "HRL_INVALID_ID").
	 */
    std::string ErrorEnumToString(EError err) const;
/**
	 * @brief Converts an HRL_Severity enum value to its string representation.
	 * @param sev The severity level to convert.
	 * @return A null-terminated string literal (e.g. "HRL_SEVERITY_FATAL").
	 */
    std::string SeverityEnumToString(ESeverity severity) const;
/**
	 * @brief Registers a callback invoked whenever an error is raised internally.
	 * Useful for integrating HRL errors into a custom logging or assertion system.
	 * @param _callback Function pointer with signature: void(HRL_Error, HRL_Severity, const char*).
	 */
    void RegisterErrorCallback(ErrorCallback callback);
};

class HRL_CPP_API Mesh {
public:
/** Constructs a Mesh wrapper. */
    Mesh();
/** Constructs a Mesh wrapper. */
    explicit Mesh(id value, bool owned = true);
/** Creates a non-owning mesh wrapper for an existing HRL mesh ID. */
    static Mesh Borrowed(id value);
/** Releases the owned Mesh resource. */
    ~Mesh();

/** Constructs a Mesh wrapper. */
    Mesh(const Mesh&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Mesh& operator=(const Mesh&) = delete;
/** Constructs a Mesh wrapper. */
    Mesh(Mesh&& other) noexcept;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Mesh& operator=(Mesh&& other) noexcept;

/** Returns the underlying HRL resource identifier. */
    id GetID() const noexcept;
/**
	 * @brief Returns whether the given ID refers to a live mesh object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
    bool IsValid() const;
/**
	 * @brief Destroys a mesh and frees its associated GPU resources.
	 * @param _meshid ID of the mesh to delete.
	 */
    void Release();

/**
	 * @brief Sets the pivot (origin) point of a mesh or sprite.
	 * Coordinates are normalized: (0,0,0) is center, (-0.5,-0.5,0) is top-left.
	 * Affects how translation and rotation are applied to the object.
	 */
    void SetPivotPoint(float x, float y, float z);
/**
	 * @brief Defines the UV region of the texture displayed on a sprite.
	 * Useful for sprite atlases. (min_u, min_v) is the top-left corner,
	 * (max_u, max_v) is the bottom-right corner, in normalized [0..1] coordinates.
	 */
    void SetSpriteRegion(float minU, float minV, float maxU, float maxV);
/**
	 * @brief Assigns a material to a mesh, controlling how it is shaded.
	 * @param _meshid ID of the target mesh.
	 * @param _matid  ID of the material to apply.
	 */
    void SetMaterial(const Material& material);
/**
	 * @brief Sets the world-space position of a mesh.
	 */
    void SetLocation(float x, float y, float z);
/**
	 * @brief Sets the rotation of a mesh using Euler angles (in degrees).
	 * @param pitch Rotation around the X axis.
	 * @param yaw   Rotation around the Y axis.
	 * @param roll  Rotation around the Z axis.
	 */
    void SetRotation(float pitch, float yaw, float roll);
/**
	 * @brief Sets the scale of a mesh along each local axis.
	 */
    void SetScale(float x, float y, float z);
/** Returns the minimum world-space distance from the mesh LOD center to any viewport camera.
	 * Uses the same object-center and camera-position metric as distance-based LOD. */
    float GetCameraDistance() const;

/** Enables or disables automatic LOD selection for a 3D mesh. */
    void SetLODAutomatic(bool enabled);
/** Selects distance-based or projected screen-size LOD selection. */
    void SetLODMode(ELODMode mode);
/** Sets the number of LOD levels, including LOD 0. Range: 1..8. */
    void SetLODLevels(uint levels);
/** Sets the first transition distance in world units. */
    void SetLODDistance(float baseDistance);
/** Multiplies the distance threshold for each next LOD. */
    void SetLODScale(float distanceScale);
/** C++ convenience function for SetLODMinDistance. */
    void SetLODMinDistance(float distance);
/** C++ convenience function for SetLODMaxDistance. */
    void SetLODMaxDistance(float distance);
/** Sets the LOD 0 screen-height threshold in [0,1]. */
    void SetLODScreenThreshold(float threshold);
/** Multiplies the screen-size threshold for each next LOD. */
    void SetLODScreenScale(float scale);
/** Adds transition hysteresis in [0,0.49] to reduce LOD popping. */
    void SetLODHysteresis(float hysteresis);
/** Forces a fixed LOD level. Pass -1 to return to automatic selection. */
    void SetLODOverride(int level);
/** Regenerates the internal LOD geometry from LOD 0. */
    void ForceLODRebuild();
/** C++ convenience function for GetLODCount. */
    uint GetLODCount() const;
/** C++ convenience function for GetLODVertexCount. */
    std::size_t GetLODVertexCount(uint level) const;
/** C++ convenience function for GetLODTriangleCount. */
    std::size_t GetLODTriangleCount(uint level) const;
/** Returns the LOD chosen by the most recently rendered viewport. */
    int GetLODLevel() const;

/** C++ convenience function for GetSkeletalBoneCount. */
    uint GetSkeletalBoneCount() const;
/** C++ convenience function for GetSkeletalBone. */
    const SkeletalBone* GetSkeletalBone(uint index) const;
/** C++ convenience function for FindSkeletalBone. */
    uint FindSkeletalBone(const char* name) const;
/** C++ convenience function for GetSkeletalAnimationCount. */
    uint GetSkeletalAnimationCount() const;
/** C++ convenience function for GetSkeletalAnimation. */
    const SkeletalAnimation* GetSkeletalAnimation(uint index) const;
/** C++ convenience function for FindSkeletalAnimation. */
    uint FindSkeletalAnimation(const char* name) const;
/** C++ convenience function for PlaySkeletalAnimation. */
    void PlaySkeletalAnimation(uint animation);
/** C++ convenience function for StopSkeletalAnimation. */
    void StopSkeletalAnimation();
/** C++ convenience function for SetSkeletalAnimationTime. */
    void SetSkeletalAnimationTime(float time);
/** C++ convenience function for SetSkeletalAnimationSpeed. */
    void SetSkeletalAnimationSpeed(float speed);
/** C++ convenience function for SetSkeletalAnimationLoop. */
    void SetSkeletalAnimationLoop(bool loop);
/** C++ convenience function for GetCurrentSkeletalAnimation. */
    int GetCurrentSkeletalAnimation() const;
/** C++ convenience function for GetSkeletalAnimationTime. */
    float GetSkeletalAnimationTime() const;
/** C++ convenience function for IsSkeletalAnimationPlaying. */
    bool IsSkeletalAnimationPlaying() const;
/**
	 * @brief Controls the rendering order of a sprite on the Z axis.
	 * Only relevant when two or more sprites share the same Z depth.
	 * Higher values are drawn on top.
	 */
    void SetSpriteDrawOrder(float drawOrder);

private:
    id id_ = INVALID_ID;
    bool owned_ = true;
};

class HRL_CPP_API Light {
public:
/** Constructs a Light wrapper. */
    Light();
/** Wraps an existing HRL light identifier. */
    explicit Light(id value);
/** Releases the owned Light resource. */
    ~Light();
/** Constructs a Light wrapper. */
    Light(const Light&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Light& operator=(const Light&) = delete;
/** Constructs a Light wrapper. */
    Light(Light&& other) noexcept;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Light& operator=(Light&& other) noexcept;
/** Returns the underlying HRL resource identifier. */
    id GetID() const noexcept;
/**
	 * @brief Returns whether the given ID refers to a live light object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
    bool IsValid() const;
/** Releases the owned HRL resource and invalidates this wrapper. */
    void Release();

/**
	 * @brief Sets the RGB color emitted by a light.
	 * Values are typically in [0..1] but may exceed 1 for HDR workflows.
	 */
    void SetColor(float x, float y, float z);
/**
	 * @brief Sets the intensity (brightness multiplier) of a light.
	 * @param i Intensity value. 1.0 is the default, higher values produce brighter results.
	 */
    void SetIntensity(float intensity);
/**
	 * @brief Sets the attenuation (falloff) factor of a light.
	 * Controls how quickly the light fades with distance.
	 * @param a Attenuation coefficient.
	 */
    void SetAttenuation(float attenuation);
/**
	 * @brief Sets the world-space position of a light.
	 * Relevant for point lights and spot lights.
	 */
    void SetLocation(float x, float y, float z);
/**
	 * @brief Sets the orientation of a light using Euler angles (in degrees).
	 * Primarily relevant for directional and spot lights.
	 */
    void SetRotation(float pitch, float yaw, float roll);
/**
	 * @brief Enables or disables shadow casting for a light.
	 * OpenGL 3.3 supports shadows for point, directional and spot lights.
	 */
    void SetCastShadows(bool enabled);
/**
	 * @brief Sets the shadow bias used to reduce self-shadowing artifacts.
	 */
    void SetShadowBias(float bias);
/**
	 * @brief Sets the resolution of the private shadow map for a light.
	 * Supported values are positive powers of two; the OpenGL backend may clamp
	 * the requested value to the implementation limits.
	 */
    void SetShadowResolution(int resolution);
/**
	 * 
	 * @param _lightid
	 * @param inner_cutoff Degrees
	 */
    void SetSpotLightInnerCutoff(float degrees);
/** C++ convenience function for SetSpotLightOuterCutoff. */
    void SetSpotLightOuterCutoff(float degrees);
private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Texture {
public:
/** Constructs a Texture wrapper. */
    Texture();
/** Wraps an existing HRL texture identifier. */
    explicit Texture(id value);
/** Releases the owned Texture resource. */
    ~Texture();
/** Constructs a Texture wrapper. */
    Texture(const Texture&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Texture& operator=(const Texture&) = delete;
/** Constructs a Texture wrapper. */
    Texture(Texture&& other) noexcept;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Texture& operator=(Texture&& other) noexcept;
/** Returns the underlying HRL resource identifier. */
    id GetID() const noexcept;
/**
	 * @brief Returns whether the given ID refers to a live texture object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
    bool IsValid() const;
/** Releases the owned HRL resource and invalidates this wrapper. */
    void Release();

/**
	 * @brief Creates a GPU texture from a raw file buffer.
	 * Supported formats: png, jpeg, jpg, bmp, tga, gif (first frame), hdr, psd (partial).
	 * @param _data       Pointer to the file contents (opened in binary mode).
	 * @param _bufferSize Size of the buffer in bytes.
	 * @return HRL_id of the new texture, or HRL_INVALID_ID on failure.
	 */
    static Texture FromMemory(const void* data, std::size_t size);
/**
	 * @brief Rasterizes a UTF-8 text string into a new texture using the given font.
	 * @param _text        Null-terminated UTF-8 string to render.
	 * @param _fontid      ID of a font created with HRL_CreateFont.
	 * @param _font_size   Glyph height in pixels.
	 * @param _wrap_width  Line wrap threshold in pixels. Pass 0 to disable wrapping.
	 * @param r,g,b        Text foreground color in [0..1].
	 * @param bg_r,bg_g,bg_b,bg_a Background color. Set bg_a = 0 for a transparent background.
	 * @return HRL_id of the newly created texture, or HRL_INVALID_ID on failure.
	 */
    static Texture FromText(const char* text, const Font& font, float fontSize, float wrapWidth,
                             float r, float g, float b, float bgR, float bgG, float bgB, float bgA);

/**
	 * @brief Replaces the pixel data of an existing texture from a new file buffer.
	 * The texture ID remains valid; any material referencing it will use the updated image.
	 * @param _textureid  ID of the texture to update.
	 * @param _data       Pointer to the new file contents (opened in binary mode).
	 * @param _bufferSize Size of the buffer in bytes.
	 */
    void Reload(const void* data, std::size_t size);
/**
	 * @brief Retrieves the current dimensions of a texture in pixels.
	 * @param _width  Output width.
	 * @param _height Output height.
	 */
    void GetSize(int& width, int& height) const;
/**
	 * @brief Sets the minification filter used when the texture appears smaller than its native size.
	 * @param _filter One of HRL_Filter_Nearest, HRL_Filter_Linear, HRL_Filter_Trilinear, etc.
	 */
    void SetMinFilter(EFilterType filter);
/**
	 * @brief Sets the magnification filter used when the texture appears larger than its native size.
	 * @param _filter One of HRL_Filter_Nearest, HRL_Filter_Linear, etc.
	 */
    void SetMagFilter(EFilterType filter);
private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Scene {
public:
/**
	 * @brief Creates a new scene that acts as a container for meshes, lights and cameras.
	 * @param _renderOnScreen If HRL_True, the scene renders directly to the screen.
	 *                        If HRL_False, it renders into an off-screen texture buffer (default 480x480).
	 * @return HRL_id of the new scene, or HRL_INVALID_ID on failure.
	 */
    explicit Scene(bool renderOnScreen);
/** Wraps an existing HRL scene identifier without creating a new scene. */
    explicit Scene(id value);
/** Releases the owned Scene resource. */
    ~Scene();
/**
	 * @brief Creates a new scene that acts as a container for meshes, lights and cameras.
	 * @param _renderOnScreen If HRL_True, the scene renders directly to the screen.
	 *                        If HRL_False, it renders into an off-screen texture buffer (default 480x480).
	 * @return HRL_id of the new scene, or HRL_INVALID_ID on failure.
	 */
    Scene(const Scene&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Scene& operator=(const Scene&) = delete;
/**
	 * @brief Creates a new scene that acts as a container for meshes, lights and cameras.
	 * @param _renderOnScreen If HRL_True, the scene renders directly to the screen.
	 *                        If HRL_False, it renders into an off-screen texture buffer (default 480x480).
	 * @return HRL_id of the new scene, or HRL_INVALID_ID on failure.
	 */
    Scene(Scene&& other) noexcept;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Scene& operator=(Scene&& other) noexcept;

/** Returns the underlying HRL resource identifier. */
    id GetID() const noexcept;
/**
	 * @brief Returns whether the given ID refers to a live scene object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
    bool IsValid() const;
/**
	 * @brief Destroys a scene and all objects it owns.
	 * @param _sceneid ID of the scene to delete.
	 */
    void Release();

/**
	 * @brief Creates a sprite mesh in the given scene.
	 * A sprite is a textured quad that always faces the camera.
	 * @param _sceneid ID of the target scene.
	 * @return HRL_id of the new sprite, or HRL_InvalidID on failure.
	 */
    Mesh CreateSprite();
/**
	 * @brief Creates a static 3D mesh from caller-owned vertex/index arrays.
	 *
	 * The mesh data is copied by HRL and may be released by the caller as soon
	 * as the function returns. When _indexCount is zero, the vertices are drawn
	 * sequentially as triangles. Otherwise _indices must contain triangle indices
	 * and _indexCount must be a multiple of 3.
	 *
	 * No standard 3D file format importer is involved in this API.
	 *
	 * @param _sceneid      ID of the target scene.
	 * @param _vertices     Pointer to _vertexCount HRL_Vertex3D values.
	 * @param _vertexCount  Number of vertices.
	 * @param _indices      Optional pointer to _indexCount HRL_uint indices.
	 * @param _indexCount   Number of indices, or 0 for non-indexed rendering.
	 * @return HRL_id of the new mesh, or HRL_INVALID_ID on failure.
	 */
    Mesh CreateMesh3D(const Vertex3D* vertices, std::size_t vertexCount,
                      const uint* indices = nullptr, std::size_t indexCount = 0);
/** Creates a skeletal mesh from CPU-side data returned by the FBX importer.
	 * HRL copies the data and the caller may free the source structure afterwards. */
    Mesh CreateSkeletalMesh(const SkeletalMeshData& data);
/**
	 * @brief Reserved generic file-based mesh entry point.
	 *
	 * This entry point remains reserved. FBX conversion is intentionally exposed
	 * separately through HRL_GetVertex3DFromFBX.
	 */
    Mesh CreateMeshFromFile(EMeshType type, const void* data, std::size_t size);
/**
	 * @brief Creates a light source in the given scene.
	 * @param _type One of HRL_PointLight, HRL_DirectionalLight, HRL_SpotLight.
	 * @return HRL_id of the new light, or HRL_INVALID_ID on failure.
	 */
    Light CreateLight(ELightType type);
/**
	 * @brief Creates a camera in the given scene.
	 * @param _type One of HRL_Ortho or HRL_Perspective. Defaults to HRL_Ortho.
	 * @return HRL_id of the new camera, or HRL_INVALID_ID on failure.
	 */
    Camera CreateCamera(ECameraType type);
/**
	 * @brief Creates a viewport that renders a scene through a camera into a screen region.
	 * Useful for split-screen or picture-in-picture setups.
	 * All coordinates are normalized [0..1]: (0,0) is top-left, (1,1) is bottom-right.
	 * @param _cameraid Camera to use, or HRL_INVALID_ID to leave unassigned.
	 * @return HRL_id of the new viewport, or HRL_INVALID_ID on failure.
	 */
    Viewport CreateViewport(const Camera* camera, float x, float y, float width, float height);
/**
	 * @brief Attaches a post-process pass to a viewport using a custom material.
	 * Passes are applied in creation order after the scene is rendered.
	 * @param _matid Material containing the full-screen shader to apply.
	 * @return HRL_id of the new post-process object, or HRL_INVALID_ID on failure.
	 */
    PostProcess CreatePostProcess(const Material& material, int priority);

/**
	 * @brief Resizes the off-screen render texture of a scene.
	 * Has no effect if the scene was created with _renderOnScreen = HRL_True.
	 */
    void ResizeTexture(int width, int height);
/**
	 * @brief Enables or disables the procedural sky sphere for a scene.
	 * The sky follows the camera translation, so it behaves as an infinitely
	 * distant environment while remaining a true sphere in the OpenGL backend.
	 * Disabled by default for backward compatibility.
	 */
    void SetSkySphereEnabled(bool enabled);
/**
	 * @brief Sets the top, horizon and bottom colors of the procedural sky.
	 * Values are linear RGB and may exceed 1 for HDR rendering.
	 */
    void SetSkySphereColors(float topR,float topG,float topB,float horizonR,float horizonG,float horizonB,
                            float bottomR,float bottomG,float bottomB);
/**
	 * @brief Rotates the sky sphere using Euler angles in degrees.
	 */
    void SetSkySphereRotation(float pitch,float yaw,float roll);
/**
	 * @brief Uses an equirectangular 2D texture as the sky-sphere image.
	 * Pass HRL_INVALID_ID to return to the procedural color sky.
	 */
    void SetSkySphereTexture(const Texture* texture);
/**
	 * @brief Enables or disables equirectangular environment mapping for 3D materials.
	 * The environment texture is independent from the sky texture.
	 */
    void SetEnvironmentMappingEnabled(bool enabled);
/**
	 * @brief Sets an equirectangular texture used for environment reflections.
	 * Pass HRL_INVALID_ID to remove the environment map.
	 */
    void SetEnvironmentMap(const Texture* texture);
/**
	 * @brief Enables or disables the GPU color-picking buffer for a scene.
	 * When enabled, each rendered object is assigned a unique color ID,
	 * allowing CPU-side object picking by reading pixel values.
	 * @param _enable HRL_True to enable, HRL_False to disable.
	 */
    void EnableColorPickingBuffer(bool enabled);
/**
	 *
	 * @param _scene
	 * @param mesh_type Reference to the type of hovered mesh, can be nullptr
	 * @param mouseX Relative mouse cursor position to the window
	 * @param mouseY
	 * @return HRL id of the hovered mesh, if none, it will return HRL_INVALID_ID
	 */
    Mesh GetHoveredObject(int mouseX, int mouseY, EMeshType* meshType = nullptr);
/**
	 * @brief Enables or disables the fog effect for a scene.
	 * @param scene  ID of the target scene.
	 * @param enable HRL_TRUE to enable, HRL_FALSE to disable.
	 */
    void SetFogEnabled(bool enabled);
/**
	 * @brief Sets the fog blending mode for a scene.
	 * @param scene ID of the target scene.
	 * @param mode  One of HRL_FOG_LINEAR, HRL_FOG_EXPONENTIAL, or HRL_FOG_EXP_SQUARED.
	 */
    void SetFogMode(EFogType mode);
/**
	 * @brief Sets the fog color for a scene.
	 * @param scene   ID of the target scene.
	 * @param r,g,b   Fog color in [0..1].
	 */
    void SetFogColor(float r,float g,float b);
/**
	 * @brief Sets the fog density for exponential fog modes.
	 * Has no effect when fog mode is HRL_FOG_LINEAR.
	 * @param scene   ID of the target scene.
	 * @param density Density coefficient. Higher values produce thicker fog.
	 */
    void SetFogDensity(float density);
/**
	 * @brief Sets the start and end distances for linear fog.
	 * Objects beyond _end are fully fogged; objects before _start are unaffected.
	 * @param scene  ID of the target scene.
	 * @param start  Distance at which fog begins (world units).
	 * @param end    Distance at which fog reaches full opacity (world units).
	 */
    void SetFogLinearRange(float start,float end);

/**
	 * @brief Overrides the scene rendering with a diagnostic visualization mode.
	 * Useful for inspecting normals, lighting, or other render passes in isolation.
	 * @param mode One of HRL_DEBUG_VIEW_NONE, HRL_DEBUG_VIEW_UNLIT, HRL_DEBUG_VIEW_NORMAL, HRL_DEBUG_VIEW_LIGHTING, HRL_DEBUG_VIEW_WIREFRAME or HRL_DEBUG_VIEW_LOD.
	 */
    void DrawAsDebugMode(EDebugView mode);
/**
	 * @brief Captures the rendered output of a scene and saves it as a PNG file.
	 * The capture is performed at the end of the current frame.
	 * @param _target_path Absolute path where the PNG image will be written.
	 */
    void TakeScreenshot(const char* path) const;

private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API PostProcess {
public:
/** Constructs a PostProcess wrapper. */
    PostProcess();
/** Wraps an existing HRL postprocess identifier. */
    explicit PostProcess(id value);
/** Releases the owned PostProcess resource. */
    ~PostProcess();
/** Constructs a PostProcess wrapper. */
    PostProcess(const PostProcess&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    PostProcess& operator=(const PostProcess&) = delete;
/** Constructs a PostProcess wrapper. */
    PostProcess(PostProcess&& other) noexcept;
/** Move/copy assignment operator for this C++ resource wrapper. */
    PostProcess& operator=(PostProcess&& other) noexcept;
/** Returns the underlying HRL resource identifier. */
    id GetID() const noexcept;
/**
	 * @brief Returns whether the given ID refers to a live post-process object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
    bool IsValid() const;
/** Releases the owned HRL resource and invalidates this wrapper. */
    void Release();
private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Shader {
public:
/** Constructs a Shader wrapper. */
    Shader();
/** Wraps an existing HRL shader identifier. */
    explicit Shader(id value);
/** Releases the owned Shader resource. */
    ~Shader();
/** Constructs a Shader wrapper. */
    Shader(const Shader&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Shader& operator=(const Shader&) = delete;
/** Constructs a Shader wrapper. */
    Shader(Shader&& other) noexcept;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Shader& operator=(Shader&& other) noexcept;
/** Returns the underlying HRL resource identifier. */
    id GetID() const noexcept;
/**
	 * @brief Returns whether the given ID refers to a live shader object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
    bool IsValid() const;
/** Releases the owned HRL resource and invalidates this wrapper. */
    void Release();
/**
	 * @brief Compiles and links a shader program from GLSL vertex and fragment source.
	 * @param _vertData  Pointer to the vertex shader source buffer.
	 * @param _vertSize  Size of the vertex shader source in bytes.
	 * @param _fragData  Pointer to the fragment shader source buffer.
	 * @param _fragSize  Size of the fragment shader source in bytes.
	 * @return HRL_id of the compiled shader, or HRL_INVALID_ID on compilation failure.
	 */
    static Shader FromSource(const void* vertexData, std::size_t vertexSize,
                             const void* fragmentData, std::size_t fragmentSize);
private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Material {
public:
/** Constructs a Material wrapper. */
    Material();
/** Wraps an existing HRL material identifier. */
    explicit Material(id value);
/** Releases the owned Material resource. */
    ~Material();
/** Constructs a Material wrapper. */
    Material(const Material&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Material& operator=(const Material&) = delete;
/** Constructs a Material wrapper. */
    Material(Material&& other) noexcept;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Material& operator=(Material&& other) noexcept;
/** Returns the underlying HRL resource identifier. */
    id GetID() const noexcept;
/**
	 * @brief Returns whether the given ID refers to a live material object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
    bool IsValid() const;
/** Releases the owned HRL resource and invalidates this wrapper. */
    void Release();
/**
	 * @brief Creates a material instance backed by the given shader.
	 * A material stores the uniform values (textures, floats, etc.) passed to its shader.
	 * @param _shaderid ID of the shader this material uses.
	 * @return HRL_id of the new material, or HRL_INVALID_ID on failure.
	 */
    static Material Create(const Shader& shader);

/**
	 * @brief Sets an integer uniform on a material.
	 */
    void SetInt(const char* name, int value);
/**
	 * @brief Binds a texture to a named sampler uniform on a material.
	 */
    void SetTexture(const char* name, const Texture& texture);
/**
	 * @brief Sets a boolean uniform on a material (internally stored as int 0 or 1).
	 */
    void SetBool(const char* name, bool value);
/**
	 * @brief Sets a float uniform on a material.
	 */
    void SetFloat(const char* name, float value);
/**
	 * @brief Sets a vec2 uniform on a material.
	 */
    void SetVec2(const char* name, float x, float y);
/**
	 * @brief Sets a vec3 uniform on a material.
	 */
    void SetVec3(const char* name, float x, float y, float z);
/**
	 * @brief Sets a vec4 uniform on a material.
	 */
    void SetVec4(const char* name, float x, float y, float z, float w);
/**
	 * @brief Sets the emissive color tint of a material, additively blended with the emissive texture.
	 * @param matid    ID of the target material.
	 * @param r,g,b,a  Emissive color and alpha multiplier in [0..1].
	 */
    void SetEmissiveColor(float r,float g,float b,float a);
private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Viewport {
public:
/** Constructs a Viewport wrapper. */
    Viewport();
/** Wraps an existing HRL viewport identifier. */
    explicit Viewport(id value);
/** Releases the owned Viewport resource. */
    ~Viewport();
/** Constructs a Viewport wrapper. */
    Viewport(const Viewport&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Viewport& operator=(const Viewport&) = delete;
/** Constructs a Viewport wrapper. */
    Viewport(Viewport&& other) noexcept;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Viewport& operator=(Viewport&& other) noexcept;
/** Returns the underlying HRL resource identifier. */
    id GetID() const noexcept;
/**
	 * @brief Returns whether the given ID refers to a live viewport object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
    bool IsValid() const;
/** Releases the owned HRL resource and invalidates this wrapper. */
    void Release();
/**
	 * @brief Reassigns the camera used by a viewport.
	 * @param _camid New camera ID, or HRL_INVALID_ID to detach.
	 */
    void SetCamera(const Camera* camera);
/**
	 * @brief Updates the screen-space rectangle of a viewport.
	 * All values are normalized [0..1].
	 */
    void SetRect(float x,float y,float width,float height);
/** C++ convenience function for CreateWidget. */
    Widget CreateWidget(EWidgetType type);
private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Camera {
public:
/** Constructs a Camera wrapper. */
    Camera();
/** Wraps an existing HRL camera identifier. */
    explicit Camera(id value);
/** Releases the owned Camera resource. */
    ~Camera();
/** Constructs a Camera wrapper. */
    Camera(const Camera&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Camera& operator=(const Camera&) = delete;
/** Constructs a Camera wrapper. */
    Camera(Camera&& other) noexcept;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Camera& operator=(Camera&& other) noexcept;
/** Returns the underlying HRL resource identifier. */
    id GetID() const noexcept;
/**
	 * @brief Returns whether the given ID refers to a live camera object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
    bool IsValid() const;
/** Releases the owned HRL resource and invalidates this wrapper. */
    void Release();
/**
	 * @brief Changes the projection type of an existing camera at runtime.
	 * @param _type HRL_Ortho or HRL_Perspective.
	 */
    void SetType(ECameraType type);
/**
	 * @brief Sets the vertical extent of an orthographic camera's view volume.
	 * @param _height World-space height visible on screen.
	 */
    void SetOrthoVertical(float height);
/**
	 * @brief Sets the vertical field of view for a perspective camera.
	 * @param _fov Vertical FOV in degrees.
	 */
    void SetPerspectiveFov(float fov);
/**
	 * @brief Sets the near clipping plane distance.
	 * Objects closer than this value will not be rendered.
	 */
    void SetNearPlane(float nearPlane);
/**
	 * @brief Sets the far clipping plane distance.
	 * Objects farther than this value will not be rendered.
	 */
    void SetFarPlane(float farPlane);
/**
	 * @brief Sets the world-space position of a camera.
	 */
    void SetLocation(float x,float y,float z);
/**
	 * @brief Sets the orientation of a camera using Euler angles (in degrees).
	 * Axis mapping: Pitch = X, Yaw = Y, Roll = Z.
	 */
    void SetRotation(float pitch,float yaw,float roll);
private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Font {
public:
/** Constructs a Font wrapper. */
    Font();
/** Wraps an existing HRL font identifier. */
    explicit Font(id value);
/** Releases the owned Font resource. */
    ~Font();
/** Constructs a Font wrapper. */
    Font(const Font&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Font& operator=(const Font&) = delete;
/** Constructs a Font wrapper. */
    Font(Font&& other) noexcept;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Font& operator=(Font&& other) noexcept;
/** Returns the underlying HRL resource identifier. */
    id GetID() const noexcept;
/**
	 * @brief Returns whether the given ID refers to a live font object.
	 * @param _id ID to test.
	 * @return HRL_TRUE if valid, HRL_FALSE otherwise.
	 */
    bool IsValid() const;
/** Releases the owned HRL resource and invalidates this wrapper. */
    void Release();
/**
	 * @brief Loads a TrueType font from a memory buffer for use with HRL_CreateTextureFromText.
	 * @param data       Pointer to the raw .ttf file contents.
	 * @param _data_size Size of the buffer in bytes.
	 * @return HRL_id of the new font, or HRL_INVALID_ID on failure.
	 */
    static Font FromMemory(const void* data, std::size_t size);
private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Widget {
public:
/** Constructs a Widget wrapper. */
    Widget();
/** Wraps an existing HRL widget identifier. */
    explicit Widget(id value);
/** Releases the owned Widget resource. */
    ~Widget();
/** Constructs a Widget wrapper. */
    Widget(const Widget&) = delete;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Widget& operator=(const Widget&) = delete;
/** Constructs a Widget wrapper. */
    Widget(Widget&& other) noexcept;
/** Move/copy assignment operator for this C++ resource wrapper. */
    Widget& operator=(Widget&& other) noexcept;
/** Returns the underlying HRL resource identifier. */
    id GetID() const noexcept;
/** Returns whether this widget wrapper contains a valid widget identifier. */
    bool IsValid() const;
/** Releases the owned HRL resource and invalidates this wrapper. */
    void Release();

/**
	 * @param widget
	 * @param x Normalized position in viewport [0;1]
	 */
    void SetPosition(float x,float y);
/**
	 * @param widget
	 * @param width Normalized in [0;1]
	 */
    void SetSize(float width,float height);
/** C++ convenience function for SetAlpha. */
    void SetAlpha(float alpha);
/** C++ convenience function for IsHovered. */
    bool IsHovered() const;
/**
	 *
	 * @param widget
	 * @param ax [0;1], [0.5, 0.5] is centered
	 * @param ay
	 */
    void SetAnchor(float ax,float ay);
/** BUTTON CONTROL FUNCTIONS **/
    void SetButtonClickable(bool clickable);
/** C++ convenience function for SetButtonText. */
    void SetButtonText(const char* text);
/** C++ convenience function for SetButtonTextTintColor. */
    void SetButtonTextTintColor(EWidgetState state,float r,float g,float b,float a);
/** C++ convenience function for SetButtonTextFont. */
    void SetButtonTextFont(const Font& font);
/**
	 *
	 * @param widget
	 * @param state HRL_BUTTON_IDLE, HRL_BUTTON_HOVERED, HRL_BUTTON_PRESSED
	 * @param texture The id of the texture to be set on background, pass 0 to set the image to white
	 */
    void SetButtonBackgroundTexture(EWidgetState state,const Texture* texture);
/** C++ convenience function for SetButtonBackgroundTintColor. */
    void SetButtonBackgroundTintColor(EWidgetState state,float r,float g,float b,float a);
/**
	 * Called every frame the button is pressed
	 * @param widget
	 * @param callback
	 * @param user_data
	 */
    void SetButtonPressedCallback(ButtonPressedCallback callback, void* userData);
/** TEXT SPECIFIC CONTROL **/
    void SetLabelText(const char* text);
/** C++ convenience function for SetLabelFont. */
    void SetLabelFont(const Font& font);
/** C++ convenience function for SetLabelTintColor. */
    void SetLabelTintColor(float r,float g,float b,float a);
private:
    id id_ = INVALID_ID;
    void* buttonCallbackState_ = nullptr;
};

/**
	 * 
	 * @param x Coordinates in pixels relative to the screen
	 */

void MouseMoved(float x,float y);
/** Advances all playing skeletal mesh animations by _deltaSeconds. */

void UpdateSkeletalAnimations(float deltaSeconds);
/**
	 * @brief Clears the entire screen to its default clear color.
	 * Useful when no scene covers the full framebuffer.
	 */

void ClearScreen();
/**
	 * @brief Enables or changes the global multisample anti-aliasing mode.
	 * @param _mode One of HRL_ANTIALIASING_OFF, HRL_ANTIALIASING_2X,
	 *              HRL_ANTIALIASING_4X or HRL_ANTIALIASING_8X.
	 */

void SetAntialiasingMode(EAntialiasingMode mode);
/**
	 * @brief Enables or changes the global multisample anti-aliasing mode.
	 * @param _mode One of HRL_ANTIALIASING_OFF, HRL_ANTIALIASING_2X,
	 *              HRL_ANTIALIASING_4X or HRL_ANTIALIASING_8X.
	 */

void SetAntialiasingMode(uint mode);
/**
 * @brief Writes the current projection matrix into a caller-provided array.
 * @param aa Pointer to a float[16] array. Matrix is column-major, contiguous.
 */
void GetProjectionMatrix(float (&matrix)[16]);
/**
 * @brief Writes the current view matrix into a caller-provided array.
 * @param aa Pointer to a float[16] array. Matrix is column-major, contiguous.
 */
void GetViewMatrix(float (&matrix)[16]);
/**
 * @brief Writes the model matrix of a specific mesh into a caller-provided array.
 * @param _meshid ID of the target mesh.
 * @param aa      Pointer to a float[16] array. Matrix is column-major, contiguous.
 */
void GetModelMatrix(const Mesh& mesh, float (&matrix)[16]);
/**
	 * @brief Sets the line thickness used by all debug draw calls.
	 * The exact visual result depends on the backend's line rendering support.
	 */

void SetDebugLineThickness(float thickness);
/**
	 * @brief Draws a debug line segment for the current frame.
	 * Must be called every frame to persist the rendering.
	 * @param a_x,a_y,a_z World-space start point.
	 * @param b_x,b_y,b_z World-space end point.
	 * @param r,g,b        Line color in [0..1].
	 */

void DrawDebugSegment(const Scene& scene, float ax,float ay,float az,float bx,float by,float bz,
                      float r,float g,float b);
/**
	 * @brief Draws a debug polygon (filled or outlined) for the current frame.
	 * Vertices are specified as separate X, Y and Z arrays of the same length.
	 * @param _mode         HRL_DebugHollow for outline, HRL_DebugSolid for filled.
	 * @param vertices_count Number of vertices.
	 */

void DrawDebugPolygon(const Scene& scene, EDebugRenderingType mode,
                      const float* xs,const float* ys,const float* zs,int count,float r,float g,float b);
/**
	 * @brief Draws a debug circle for the current frame.
	 * @param _mode    HRL_DebugHollow for outline, HRL_DebugSolid for filled.
	 * @param segments Number of segments used to approximate the circle.
	 */

void DrawDebugCircle(const Scene& scene, EDebugRenderingType mode,
                     float cx,float cy,float cz,float radius,int segments,float r,float g,float b);
/**
	 * @brief Draws a debug capsule (cylinder with hemispherical caps) for the current frame.
	 * @param a_x,a_y,a_z  World-space start (bottom hemisphere center).
	 * @param b_x,b_y,b_z  World-space end (top hemisphere center).
	 * @param _mode         HRL_DebugHollow or HRL_DebugSolid.
	 * @param segments      Number of segments used to approximate the capsule.
	 */

void DrawDebugCapsule(const Scene& scene, EDebugRenderingType mode,
                      float ax,float ay,float az,float bx,float by,float bz,
                      float radius,int segments,float r,float g,float b);
/**
	 * @brief Draws a debug point (screen-space square) for the current frame.
	 * @param size Point size in pixels.
	 */

void DrawDebugPoint(const Scene& scene,float x,float y,float z,float size,float r,float g,float b);
/**
	 * @brief Converts an FBX mesh scene to a flat HRL_Vertex3D array.
	 *
	 * The FBX file is decoded internally by HRL and the resulting geometry is
	 * returned as non-indexed triangles. Node transforms are baked into the
	 * returned vertex positions and tangent space. The returned buffer must be
	 * released with HRL_FreeVertex3DFromFBX.
	 *
	 * HRL does not expose ufbx types through this API. The FBX implementation is
	 * an internal dependency of hrl.cpp.
	 *
	 * @param _data          Pointer to the complete FBX data in memory.
	 * @param _bufferSize    Size of the FBX data buffer in bytes.
	 * @param _vertexCount   Receives the number of returned vertices.
	 * @return Newly allocated HRL_Vertex3D array, or NULL on failure.
	 */

Vertex3D* GetVertex3DFromFBX(const void* data,std::size_t size,std::size_t& vertexCount);
/**
	 * @brief Converts the first skinned FBX mesh in an in-memory FBX buffer.
	 *
	 * ufbx is an internal dependency of hrl.cpp. Geometry, bones, weights and
	 * baked animation samples are copied into an HRL-owned CPU data structure.
	 * No file path is accepted by this API.
	 */

SkeletalMeshData* GetSkeletalMeshFromFBX(const void* data,std::size_t size);
/** Frees all nested allocations returned by HRL_GetSkeletalMeshFromFBX. */

void FreeSkeletalMeshData(SkeletalMeshData* data);
/**
	 * @brief Frees the buffer returned by HRL_GetVertex3DFromFBX.
	 */

void FreeVertex3DFromFBX(Vertex3D* vertices);


} // namespace hrl

#endif // HRL_HPP
