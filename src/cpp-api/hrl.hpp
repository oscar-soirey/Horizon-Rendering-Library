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
    static Mesh Borrowed(id value);
    virtual ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    id GetID() const noexcept;
    bool IsValid() const;
    void Release();

    /** Common operations shared by every concrete mesh type. */
    void SetPivotPoint(float x, float y, float z);
    void SetMaterial(const Material& material);
    void SetLocation(float x, float y, float z);
    void SetRotation(float pitch, float yaw, float roll);
    void SetScale(float x, float y, float z);

protected:
    Mesh() = default;
    explicit Mesh(id value, bool owned);

    id id_ = INVALID_ID;
    bool owned_ = false;
};

/** Common 3D mesh functionality, primarily level-of-detail support. */
class HRL_CPP_API Mesh3D : public Mesh {
protected:
    explicit Mesh3D(id value, bool owned) : Mesh(value, owned) {}

public:
    static Mesh3D Borrowed(id value);

    float GetCameraDistance() const;
    void SetLODAutomatic(bool enabled);
    void SetLODMode(ELODMode mode);
    void SetLODLevels(uint levels);
    void SetLODDistance(float baseDistance);
    void SetLODScale(float distanceScale);
    void SetLODMinDistance(float distance);
    void SetLODMaxDistance(float distance);
    void SetLODScreenThreshold(float threshold);
    void SetLODScreenScale(float scale);
    void SetLODHysteresis(float hysteresis);
    void SetLODOverride(int level);
    void ForceLODRebuild();
    uint GetLODCount() const;
    std::size_t GetLODVertexCount(uint level) const;
    std::size_t GetLODTriangleCount(uint level) const;
    int GetLODLevel() const;
};

/** A conventional non-skinned 3D mesh created inside a Scene. */
class HRL_CPP_API StaticMesh : public Mesh3D {
public:
    explicit StaticMesh(Scene& scene,
                         const Vertex3D* vertices,
                         std::size_t vertexCount,
                         const uint* indices = nullptr,
                         std::size_t indexCount = 0);

    static StaticMesh Borrowed(id value);

protected:
    explicit StaticMesh(id value, bool owned) : Mesh3D(value, owned) {}
};

/** A skinned 3D mesh created inside a Scene. */
class HRL_CPP_API SkeletalMesh : public Mesh3D {
public:
    explicit SkeletalMesh(Scene& scene, const SkeletalMeshData& data);

    static SkeletalMesh Borrowed(id value);

protected:
    explicit SkeletalMesh(id value, bool owned) : Mesh3D(value, owned) {}

    uint GetSkeletalBoneCount() const;
    const SkeletalBone* GetSkeletalBone(uint index) const;
    uint FindSkeletalBone(const char* name) const;

    uint GetSkeletalAnimationCount() const;
    const SkeletalAnimation* GetSkeletalAnimation(uint index) const;
    uint FindSkeletalAnimation(const char* name) const;

    void PlaySkeletalAnimation(uint animation);
    void StopSkeletalAnimation();
    void SetSkeletalAnimationTime(float time);
    void SetSkeletalAnimationSpeed(float speed);
    void SetSkeletalAnimationLoop(bool loop);
    int GetCurrentSkeletalAnimation() const;
    float GetSkeletalAnimationTime() const;
    bool IsSkeletalAnimationPlaying() const;
};

/** A camera-facing 2D sprite mesh created inside a Scene. */
class HRL_CPP_API Sprite : public Mesh {
public:
    explicit Sprite(Scene& scene);

    static Sprite Borrowed(id value);

protected:
    explicit Sprite(id value, bool owned) : Mesh(value, owned) {}

    void SetSpriteRegion(float minU, float minV, float maxU, float maxV);
    void SetDrawOrder(float drawOrder);
    void SetSpriteDrawOrder(float drawOrder);
};

class HRL_CPP_API Light {
public:
    explicit Light(Scene& scene, ELightType type);
    ~Light();

    Light(const Light&) = delete;
    Light& operator=(const Light&) = delete;
    Light(Light&& other) noexcept;
    Light& operator=(Light&& other) noexcept;

    id GetID() const noexcept;
    bool IsValid() const;
    void Release();

    void SetColor(float x, float y, float z);
    void SetIntensity(float intensity);
    void SetAttenuation(float attenuation);
    void SetLocation(float x, float y, float z);
    void SetRotation(float pitch, float yaw, float roll);
    void SetCastShadows(bool enabled);
    void SetShadowBias(float bias);
    void SetShadowResolution(int resolution);
    void SetSpotLightInnerCutoff(float degrees);
    void SetSpotLightOuterCutoff(float degrees);

private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Texture {
public:
    Texture(const void* data, std::size_t size);
    Texture(const char* text, const Font& font, float fontSize, float wrapWidth,
            float r, float g, float b, float bgR, float bgG, float bgB, float bgA);
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    id GetID() const noexcept;
    bool IsValid() const;
    void Release();

    void Reload(const void* data, std::size_t size);
    void GetSize(int& width, int& height) const;
    void SetMinFilter(EFilterType filter);
    void SetMagFilter(EFilterType filter);

private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Scene {
public:
    explicit Scene(bool renderOnScreen);
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&& other) noexcept;
    Scene& operator=(Scene&& other) noexcept;

    id GetID() const noexcept;
    bool IsValid() const;
    void Release();

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
    explicit PostProcess(Viewport& viewport, const Material& material, int priority);
    ~PostProcess();

    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;
    PostProcess(PostProcess&& other) noexcept;
    PostProcess& operator=(PostProcess&& other) noexcept;

    id GetID() const noexcept;
    bool IsValid() const;
    void Release();

private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Shader {
public:
    Shader(const void* vertexData, std::size_t vertexSize,
           const void* fragmentData, std::size_t fragmentSize);
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    id GetID() const noexcept;
    bool IsValid() const;
    void Release();

private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Material {
public:
    explicit Material(const Shader& shader);
    ~Material();

    Material(const Material&) = delete;
    Material& operator=(const Material&) = delete;
    Material(Material&& other) noexcept;
    Material& operator=(Material&& other) noexcept;

    id GetID() const noexcept;
    bool IsValid() const;
    void Release();

    void SetInt(const char* name, int value);
    void SetTexture(const char* name, const Texture& texture);
    void SetBool(const char* name, bool value);
    void SetFloat(const char* name, float value);
    void SetVec2(const char* name, float x, float y);
    void SetVec3(const char* name, float x, float y, float z);
    void SetVec4(const char* name, float x, float y, float z, float w);
    void SetEmissiveColor(float r, float g, float b, float a);

private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Viewport {
public:
    Viewport(Scene& scene, const Camera* camera, float x, float y, float width, float height);
    ~Viewport();

    Viewport(const Viewport&) = delete;
    Viewport& operator=(const Viewport&) = delete;
    Viewport(Viewport&& other) noexcept;
    Viewport& operator=(Viewport&& other) noexcept;

    id GetID() const noexcept;
    bool IsValid() const;
    void Release();

    void SetCamera(const Camera* camera);
    void SetRect(float x, float y, float width, float height);

private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Camera {
public:
    explicit Camera(Scene& scene, ECameraType type);
    ~Camera();

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;
    Camera(Camera&& other) noexcept;
    Camera& operator=(Camera&& other) noexcept;

    id GetID() const noexcept;
    bool IsValid() const;
    void Release();

    void SetType(ECameraType type);
    void SetOrthoVertical(float height);
    void SetPerspectiveFov(float fov);
    void SetNearPlane(float nearPlane);
    void SetFarPlane(float farPlane);
    void SetLocation(float x, float y, float z);
    void SetRotation(float pitch, float yaw, float roll);

private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Font {
public:
    Font(const void* data, std::size_t size);
    ~Font();

    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    Font(Font&& other) noexcept;
    Font& operator=(Font&& other) noexcept;

    id GetID() const noexcept;
    bool IsValid() const;
    void Release();

private:
    id id_ = INVALID_ID;
};

class HRL_CPP_API Widget {
public:
    Widget(Viewport& viewport, EWidgetType type);
    ~Widget();

    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;
    Widget(Widget&& other) noexcept;
    Widget& operator=(Widget&& other) noexcept;

    id GetID() const noexcept;
    bool IsValid() const;
    void Release();

    void SetPosition(float x, float y);
    void SetSize(float width, float height);
    void SetAlpha(float alpha);
    bool IsHovered() const;
    void SetAnchor(float ax, float ay);
    void SetButtonClickable(bool clickable);
    void SetButtonText(const char* text);
    void SetButtonTextTintColor(EWidgetState state, float r, float g, float b, float a);
    void SetButtonTextFont(const Font& font);
    void SetButtonBackgroundTexture(EWidgetState state, const Texture* texture);
    void SetButtonBackgroundTintColor(EWidgetState state, float r, float g, float b, float a);
    void SetButtonPressedCallback(ButtonPressedCallback callback, void* userData);
    void SetLabelText(const char* text);
    void SetLabelFont(const Font& font);
    void SetLabelTintColor(float r, float g, float b, float a);

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
