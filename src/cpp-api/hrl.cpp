#include "hrl.hpp"
#include "../hrl.h"

namespace {

template <typename CEnum, typename CPPEnum>
constexpr CEnum ToCEnum(CPPEnum value) noexcept {
    return static_cast<CEnum>(value);
}

hrl::ErrorCallback g_errorCallback = nullptr;

extern "C" void HRL_CPP_ErrorCallbackBridge(HRL_EError code, HRL_ESeverity severity, const char* detail) {
    if (g_errorCallback) {
        g_errorCallback(static_cast<hrl::EError>(code), static_cast<hrl::ESeverity>(severity), detail);
    }
}

struct ButtonCallbackState {
    hrl::ButtonPressedCallback callback = nullptr;
    void* userData = nullptr;
};

extern "C" void HRL_CPP_ButtonPressedBridge(HRL_id button, int clicked, int released, void* userData) {
    auto* state = static_cast<ButtonCallbackState*>(userData);
    if (state && state->callback) {
        state->callback(static_cast<hrl::id>(button), clicked, released, state->userData);
    }
}

} // namespace

namespace hrl {

static_assert(sizeof(Vertex3D) == sizeof(HRL_Vertex3D), "Vertex3D layout must match HRL_Vertex3D");
static_assert(sizeof(SkeletalVertex) == sizeof(HRL_SkeletalVertex), "SkeletalVertex layout must match HRL_SkeletalVertex");
static_assert(sizeof(SkeletalBoneTransform) == sizeof(HRL_SkeletalBoneTransform), "SkeletalBoneTransform layout must match HRL_SkeletalBoneTransform");
static_assert(sizeof(SkeletalBone) == sizeof(HRL_SkeletalBone), "SkeletalBone layout must match HRL_SkeletalBone");
static_assert(sizeof(SkeletalAnimation) == sizeof(HRL_SkeletalAnimation), "SkeletalAnimation layout must match HRL_SkeletalAnimation");
static_assert(sizeof(SkeletalMeshData) == sizeof(HRL_SkeletalMeshData), "SkeletalMeshData layout must match HRL_SkeletalMeshData");

Instance::Instance(E_APIs api,uint width,uint height,void* loader){ HRL_Init(ToCEnum<HRL_E_APIs>(api)); HRL_InitContext(width,height,loader); }
Instance::~Instance(){ HRL_Shutdown(); }
void Instance::BeginFrame(){ HRL_BeginFrame(); }
void Instance::EndFrame(){ HRL_EndFrame(); }
void Instance::WindowResizeCallback(int w,int h){ HRL_WindowResizeCallback(w,h); }
EError Instance::GetLastError(const char** d,ESeverity* s) const {
    HRL_ESeverity cSeverity;
    HRL_ESeverity* cSeverityPtr = s ? &cSeverity : nullptr;
    HRL_EError error = HRL_GetLastError(d,cSeverityPtr);
    if (s && cSeverityPtr) *s = static_cast<ESeverity>(cSeverity);
    return static_cast<EError>(error);
}
std::string Instance::ErrorEnumToString(EError e) const { const char* s=HRL_ErrorEnumToString(ToCEnum<HRL_EError>(e)); return s?s:""; }
std::string Instance::SeverityEnumToString(ESeverity s) const { const char* v=HRL_SeverityEnumToString(ToCEnum<HRL_ESeverity>(s)); return v?v:""; }
void Instance::RegisterErrorCallback(ErrorCallback cb){ g_errorCallback=cb; HRL_RegisterErrorCallback(cb ? HRL_CPP_ErrorCallbackBridge : nullptr); }

#define HRL_MOVE(T) \
T::T(T&& o) noexcept:id_(o.id_){o.id_=INVALID_ID;} \
T& T::operator=(T&& o) noexcept { if(this!=&o){ Release(); id_=o.id_; o.id_=INVALID_ID;} return *this; }

#define HRL_COMMON(T, VALID, DELETE) \
T::T()=default; T::T(id v):id_(v){} \
T::~T(){Release();} \
id T::GetID() const noexcept{return id_;} \
bool T::IsValid() const{return id_!=INVALID_ID && VALID(id_);} \
void T::Release(){if(id_!=INVALID_ID){DELETE(id_);id_=INVALID_ID;}}

Mesh::Mesh(id v, bool owned) : id_(v), owned_(owned) {}
Mesh Mesh::Borrowed(id v) { return Mesh(v, false); }
Mesh::~Mesh() { Release(); }
id Mesh::GetID() const noexcept { return id_; }
bool Mesh::IsValid() const { return id_ != INVALID_ID && HRL_IsValidMesh(id_); }
void Mesh::Release() {
    if (id_ != INVALID_ID && owned_) HRL_DeleteMesh(id_);
    id_ = INVALID_ID;
    owned_ = false;
}
Mesh::Mesh(Mesh&& o) noexcept : id_(o.id_), owned_(o.owned_) {
    o.id_ = INVALID_ID;
    o.owned_ = false;
}
Mesh& Mesh::operator=(Mesh&& o) noexcept {
    if (this != &o) {
        Release();
        id_ = o.id_;
        owned_ = o.owned_;
        o.id_ = INVALID_ID;
        o.owned_ = false;
    }
    return *this;
}
void Mesh::SetPivotPoint(float x, float y, float z) { HRL_SetMeshPivotPoint(id_, x, y, z); }
void Mesh::SetMaterial(const Material& m) { HRL_SetMeshMaterial(id_, m.GetID()); }
void Mesh::SetLocation(float x, float y, float z) { HRL_SetMeshLocation(id_, x, y, z); }
void Mesh::SetRotation(float x, float y, float z) { HRL_SetMeshRotation(id_, x, y, z); }
void Mesh::SetScale(float x, float y, float z) { HRL_SetMeshScale(id_, x, y, z); }

Mesh3D Mesh3D::Borrowed(id v) { return Mesh3D(v, false); }
float Mesh3D::GetCameraDistance() const { return HRL_GetMeshCameraDistance(id_); }
void Mesh3D::SetLODAutomatic(bool v) { HRL_SetMeshLODAutomatic(id_, v ? HRL_TRUE : HRL_FALSE); }
void Mesh3D::SetLODMode(ELODMode v) { HRL_SetMeshLODMode(id_, ToCEnum<HRL_ELODMode>(v)); }
void Mesh3D::SetLODLevels(uint v) { HRL_SetMeshLODLevels(id_, v); }
void Mesh3D::SetLODDistance(float v) { HRL_SetMeshLODDistance(id_, v); }
void Mesh3D::SetLODScale(float v) { HRL_SetMeshLODScale(id_, v); }
void Mesh3D::SetLODMinDistance(float v) { HRL_SetMeshLODMinDistance(id_, v); }
void Mesh3D::SetLODMaxDistance(float v) { HRL_SetMeshLODMaxDistance(id_, v); }
void Mesh3D::SetLODScreenThreshold(float v) { HRL_SetMeshLODScreenThreshold(id_, v); }
void Mesh3D::SetLODScreenScale(float v) { HRL_SetMeshLODScreenScale(id_, v); }
void Mesh3D::SetLODHysteresis(float v) { HRL_SetMeshLODHysteresis(id_, v); }
void Mesh3D::SetLODOverride(int v) { HRL_SetMeshLODOverride(id_, v); }
void Mesh3D::ForceLODRebuild() { HRL_ForceMeshLODRebuild(id_); }
uint Mesh3D::GetLODCount() const { return HRL_GetMeshLODCount(id_); }
std::size_t Mesh3D::GetLODVertexCount(uint l) const { return HRL_GetMeshLODVertexCount(id_, l); }
std::size_t Mesh3D::GetLODTriangleCount(uint l) const { return HRL_GetMeshLODTriangleCount(id_, l); }
int Mesh3D::GetLODLevel() const { return HRL_GetMeshLODLevel(id_); }

StaticMesh::StaticMesh(Scene& scene, const Vertex3D* vertices, std::size_t vertexCount,
                       const uint* indices, std::size_t indexCount)
    : Mesh3D(HRL_CreateMesh3D(scene.GetID(), reinterpret_cast<const HRL_Vertex3D*>(vertices),
                              vertexCount, reinterpret_cast<const HRL_uint*>(indices), indexCount), true) {}
StaticMesh StaticMesh::Borrowed(id v) { return StaticMesh(v, false); }

SkeletalMesh::SkeletalMesh(Scene& scene, const SkeletalMeshData& data)
    : Mesh3D(HRL_CreateSkeletalMesh(scene.GetID(), reinterpret_cast<const HRL_SkeletalMeshData*>(&data)), true) {}
SkeletalMesh SkeletalMesh::Borrowed(id v) { return SkeletalMesh(v, false); }
uint SkeletalMesh::GetSkeletalBoneCount() const { return HRL_GetSkeletalBoneCount(id_); }
const SkeletalBone* SkeletalMesh::GetSkeletalBone(uint i) const { return reinterpret_cast<const SkeletalBone*>(HRL_GetSkeletalBone(id_, i)); }
uint SkeletalMesh::FindSkeletalBone(const char* n) const { return HRL_FindSkeletalBone(id_, n); }
uint SkeletalMesh::GetSkeletalAnimationCount() const { return HRL_GetSkeletalAnimationCount(id_); }
const SkeletalAnimation* SkeletalMesh::GetSkeletalAnimation(uint i) const { return reinterpret_cast<const SkeletalAnimation*>(HRL_GetSkeletalAnimation(id_, i)); }
uint SkeletalMesh::FindSkeletalAnimation(const char* n) const { return HRL_FindSkeletalAnimation(id_, n); }
void SkeletalMesh::PlaySkeletalAnimation(uint i) { HRL_PlaySkeletalAnimation(id_, i); }
void SkeletalMesh::StopSkeletalAnimation() { HRL_StopSkeletalAnimation(id_); }
void SkeletalMesh::SetSkeletalAnimationTime(float v) { HRL_SetSkeletalAnimationTime(id_, v); }
void SkeletalMesh::SetSkeletalAnimationSpeed(float v) { HRL_SetSkeletalAnimationSpeed(id_, v); }
void SkeletalMesh::SetSkeletalAnimationLoop(bool v) { HRL_SetSkeletalAnimationLoop(id_, v ? HRL_TRUE : HRL_FALSE); }
int SkeletalMesh::GetCurrentSkeletalAnimation() const { return HRL_GetCurrentSkeletalAnimation(id_); }
float SkeletalMesh::GetSkeletalAnimationTime() const { return HRL_GetSkeletalAnimationTime(id_); }
bool SkeletalMesh::IsSkeletalAnimationPlaying() const { return HRL_IsSkeletalAnimationPlaying(id_) != 0; }

Sprite::Sprite(Scene& scene)
    : Mesh(HRL_CreateMeshSprite(scene.GetID()), true) {}
Sprite Sprite::Borrowed(id v) { return Sprite(v, false); }
void Sprite::SetSpriteRegion(float a, float b, float c, float d) { HRL_SetSpriteRegion(id_, a, b, c, d); }
void Sprite::SetDrawOrder(float v) { HRL_SetSpriteDrawOrder(id_, v); }
void Sprite::SetSpriteDrawOrder(float v) { SetDrawOrder(v); }

Light::Light(Scene& scene, ELightType type)
    : id_(HRL_CreateLight(scene.GetID(), ToCEnum<HRL_ELightType>(type))) {}
Light::~Light() { Release(); }
HRL_MOVE(Light)
id Light::GetID() const noexcept { return id_; }
bool Light::IsValid() const { return id_ != INVALID_ID && HRL_IsValidLight(id_); }
void Light::Release() { if (id_ != INVALID_ID) { HRL_DeleteLight(id_); id_ = INVALID_ID; } }
void Light::SetColor(float x, float y, float z) { HRL_SetLightColor(id_, x, y, z); }
void Light::SetIntensity(float v) { HRL_SetLightIntensity(id_, v); }
void Light::SetAttenuation(float v) { HRL_SetLightAttenuation(id_, v); }
void Light::SetLocation(float x, float y, float z) { HRL_SetLightLocation(id_, x, y, z); }
void Light::SetRotation(float x, float y, float z) { HRL_SetLightRotation(id_, x, y, z); }
void Light::SetCastShadows(bool v) { HRL_SetLightCastShadows(id_, v ? 1 : 0); }
void Light::SetShadowBias(float v) { HRL_SetLightShadowBias(id_, v); }
void Light::SetShadowResolution(int v) { HRL_SetLightShadowResolution(id_, v); }
void Light::SetSpotLightInnerCutoff(float v) { HRL_SetSpotLightInnerCutoff(id_, v); }
void Light::SetSpotLightOuterCutoff(float v) { HRL_SetSpotLightOuterCutoff(id_, v); }

Texture::Texture(const void* d, std::size_t s)
    : id_(HRL_CreateTexture(static_cast<const char*>(d), s)) {}
Texture::Texture(const char* t, const Font& f, float fs, float ww,
                 float r, float g, float b, float br, float bg, float bb, float ba)
    : id_(HRL_CreateTextureFromText(t, f.GetID(), fs, ww, r, g, b, br, bg, bb, ba)) {}
Texture::~Texture() { Release(); }
HRL_MOVE(Texture)
id Texture::GetID() const noexcept { return id_; }
bool Texture::IsValid() const { return id_ != INVALID_ID && HRL_IsValidTexture(id_); }
void Texture::Release() { if (id_ != INVALID_ID) { HRL_DeleteTexture(id_); id_ = INVALID_ID; } }
void Texture::Reload(const void* d, std::size_t s) { HRL_ReloadTexture(id_, static_cast<const char*>(d), s); }
void Texture::GetSize(int& w, int& h) const { HRL_GetTextureSize(id_, &w, &h); }
void Texture::SetMinFilter(EFilterType f) { HRL_SetTextureMinFilter(id_, ToCEnum<HRL_EFilterType>(f)); }
void Texture::SetMagFilter(EFilterType f) { HRL_SetTextureMagFilter(id_, ToCEnum<HRL_EFilterType>(f)); }

Scene::~Scene() { Release(); }
Scene::Scene(bool v) : id_(HRL_CreateScene(v ? HRL_TRUE : HRL_FALSE)) {}
id Scene::GetID() const noexcept { return id_; }
bool Scene::IsValid() const { return id_ != INVALID_ID && HRL_IsValidScene(id_); }
void Scene::Release() { if (id_ != INVALID_ID) { HRL_DeleteScene(id_); id_ = INVALID_ID; } }
HRL_MOVE(Scene)
void Scene::ResizeTexture(int w, int h) { HRL_ResizeSceneTexture(id_, w, h); }
void Scene::SetSkySphereEnabled(bool v) { HRL_SetSkySphereEnabled(id_, v ? 1 : 0); }
void Scene::SetSkySphereColors(float a, float b, float c, float d, float e, float f, float g, float h, float i) { HRL_SetSkySphereColors(id_, a, b, c, d, e, f, g, h, i); }
void Scene::SetSkySphereRotation(float a, float b, float c) { HRL_SetSkySphereRotation(id_, a, b, c); }
void Scene::SetSkySphereTexture(const Texture* t) { HRL_SetSkySphereTexture(id_, t ? t->GetID() : INVALID_ID); }
void Scene::SetEnvironmentMappingEnabled(bool v) { HRL_SetEnvironmentMappingEnabled(id_, v ? 1 : 0); }
void Scene::SetEnvironmentMap(const Texture* t) { HRL_SetEnvironmentMap(id_, t ? t->GetID() : INVALID_ID); }
void Scene::EnableColorPickingBuffer(bool v) { HRL_EnableColorPickingBuffer(id_, v ? 1 : 0); }
Mesh Scene::GetHoveredObject(int x, int y, EMeshType* t) {
    HRL_EMeshType cType;
    HRL_id meshId = HRL_GetHoveredObject(id_, x, y, t ? &cType : nullptr);
    if (t) *t = static_cast<EMeshType>(cType);
    return Mesh::Borrowed(meshId);
}
void Scene::SetFogEnabled(bool v) { HRL_SetFogEnabled(id_, v ? 1 : 0); }
void Scene::SetFogMode(EFogType m) { HRL_SetFogMode(id_, ToCEnum<HRL_EFogType>(m)); }
void Scene::SetFogColor(float r, float g, float b) { HRL_SetFogColor(id_, r, g, b); }
void Scene::SetFogDensity(float d) { HRL_SetFogDensity(id_, d); }
void Scene::SetFogLinearRange(float a, float b) { HRL_SetFogLinearRange(id_, a, b); }
void Scene::DrawAsDebugMode(EDebugView m) { HRL_DrawSceneAsDebugMode(id_, ToCEnum<HRL_EDebugView>(m)); }
void Scene::TakeScreenshot(const char* p) const { HRL_TakeScreenshot(id_, p); }

PostProcess::PostProcess(Viewport& viewport, const Material& material, int priority)
    : id_(HRL_CreatePostProcess(viewport.GetID(), material.GetID(), priority)) {}
PostProcess::~PostProcess() { Release(); }
HRL_MOVE(PostProcess)
id PostProcess::GetID() const noexcept { return id_; }
bool PostProcess::IsValid() const { return id_ != INVALID_ID && HRL_IsValidPostProcess(id_); }
void PostProcess::Release() { if (id_ != INVALID_ID) { HRL_DeletePostProcess(id_); id_ = INVALID_ID; } }

Shader::Shader(const void* v, std::size_t vs, const void* f, std::size_t fs)
    : id_(HRL_CreateShader(static_cast<const char*>(v), vs, static_cast<const char*>(f), fs)) {}
Shader::~Shader() { Release(); }
HRL_MOVE(Shader)
id Shader::GetID() const noexcept { return id_; }
bool Shader::IsValid() const { return id_ != INVALID_ID && HRL_IsValidShader(id_); }
void Shader::Release() { if (id_ != INVALID_ID) { HRL_DeleteShader(id_); id_ = INVALID_ID; } }

Material::Material(const Shader& s) : id_(HRL_CreateMaterial(s.GetID())) {}
Material::~Material() { Release(); }
HRL_MOVE(Material)
id Material::GetID() const noexcept { return id_; }
bool Material::IsValid() const { return id_ != INVALID_ID && HRL_IsValidMaterial(id_); }
void Material::Release() { if (id_ != INVALID_ID) { HRL_DeleteMaterial(id_); id_ = INVALID_ID; } }
void Material::SetInt(const char* n, int v) { HRL_MaterialSetInt(id_, n, v); }
void Material::SetTexture(const char* n, const Texture& t) { HRL_MaterialSetTexture(id_, n, t.GetID()); }
void Material::SetBool(const char* n, bool v) { HRL_MaterialSetBool(id_, n, v ? 1 : 0); }
void Material::SetFloat(const char* n, float v) { HRL_MaterialSetFloat(id_, n, v); }
void Material::SetVec2(const char* n, float x, float y) { HRL_MaterialSetVec2(id_, n, x, y); }
void Material::SetVec3(const char* n, float x, float y, float z) { HRL_MaterialSetVec3(id_, n, x, y, z); }
void Material::SetVec4(const char* n, float x, float y, float z, float w) { HRL_MaterialSetVec4(id_, n, x, y, z, w); }
void Material::SetEmissiveColor(float r, float g, float b, float a) { HRL_MaterialSetEmissiveColor(id_, r, g, b, a); }

Viewport::Viewport(Scene& scene, const Camera* camera, float x, float y, float w, float h)
    : id_(HRL_CreateViewport(scene.GetID(), camera ? camera->GetID() : INVALID_ID, x, y, w, h)) {}
Viewport::~Viewport() { Release(); }
HRL_MOVE(Viewport)
id Viewport::GetID() const noexcept { return id_; }
bool Viewport::IsValid() const { return id_ != INVALID_ID && HRL_IsValidViewport(id_); }
void Viewport::Release() { if (id_ != INVALID_ID) { HRL_DeleteViewport(id_); id_ = INVALID_ID; } }
void Viewport::SetCamera(const Camera* c) { HRL_SetViewportCamera(id_, c ? c->GetID() : INVALID_ID); }
void Viewport::SetRect(float x, float y, float w, float h) { HRL_SetViewportRect(id_, x, y, w, h); }

Camera::Camera(Scene& scene, ECameraType type)
    : id_(HRL_CreateCamera(scene.GetID(), ToCEnum<HRL_ECameraType>(type))) {}
Camera::~Camera() { Release(); }
HRL_MOVE(Camera)
id Camera::GetID() const noexcept { return id_; }
bool Camera::IsValid() const { return id_ != INVALID_ID && HRL_IsValidCamera(id_); }
void Camera::Release() { if (id_ != INVALID_ID) { HRL_DeleteCamera(id_); id_ = INVALID_ID; } }
void Camera::SetType(ECameraType t) { HRL_SetCameraType(id_, ToCEnum<HRL_ECameraType>(t)); }
void Camera::SetOrthoVertical(float v) { HRL_SetCameraOrthoVertical(id_, v); }
void Camera::SetPerspectiveFov(float v) { HRL_SetCameraPerspectiveFov(id_, v); }
void Camera::SetNearPlane(float v) { HRL_SetCameraNearPlane(id_, v); }
void Camera::SetFarPlane(float v) { HRL_SetCameraFarPlane(id_, v); }
void Camera::SetLocation(float x, float y, float z) { HRL_SetCameraLocation(id_, x, y, z); }
void Camera::SetRotation(float x, float y, float z) { HRL_SetCameraRotation(id_, x, y, z); }

Font::Font(const void* d, std::size_t s)
    : id_(HRL_CreateFont(static_cast<const char*>(d), s)) {}
Font::~Font() { Release(); }
HRL_MOVE(Font)
id Font::GetID() const noexcept { return id_; }
bool Font::IsValid() const { return id_ != INVALID_ID && HRL_IsValidFont(id_); }
void Font::Release() { if (id_ != INVALID_ID) { HRL_DeleteFont(id_); id_ = INVALID_ID; } }

Widget::Widget(Viewport& viewport, EWidgetType type)
    : id_(HRL_CreateWidget(viewport.GetID(), ToCEnum<HRL_EWidgetType>(type))) {}
Widget::~Widget() { Release(); }
id Widget::GetID() const noexcept { return id_; }
bool Widget::IsValid() const { return id_ != INVALID_ID; }
void Widget::Release() {
    if (id_ != INVALID_ID) {
        HRL_SetButtonPressedCallback(id_, nullptr, nullptr);
        HRL_DeleteWidget(id_);
        id_ = INVALID_ID;
    }
    delete static_cast<ButtonCallbackState*>(buttonCallbackState_);
    buttonCallbackState_ = nullptr;
}
Widget::Widget(Widget&& o) noexcept
    : id_(o.id_), buttonCallbackState_(o.buttonCallbackState_) {
    o.id_ = INVALID_ID;
    o.buttonCallbackState_ = nullptr;
}
Widget& Widget::operator=(Widget&& o) noexcept {
    if (this != &o) {
        Release();
        id_ = o.id_;
        buttonCallbackState_ = o.buttonCallbackState_;
        o.id_ = INVALID_ID;
        o.buttonCallbackState_ = nullptr;
    }
    return *this;
}
void Widget::SetPosition(float x, float y) { HRL_SetWidgetPosition(id_, x, y); }
void Widget::SetSize(float w, float h) { HRL_SetWidgetSize(id_, w, h); }
void Widget::SetAlpha(float a) { HRL_SetWidgetAlpha(id_, a); }
bool Widget::IsHovered() const { return HRL_IsWidgetHovered(id_) != 0; }
void Widget::SetAnchor(float x, float y) { HRL_SetWidgetAnchor(id_, x, y); }
void Widget::SetButtonClickable(bool v) { HRL_SetButtonClickable(id_, v ? 1 : 0); }
void Widget::SetButtonText(const char* t) { HRL_SetButtonText(id_, t); }
void Widget::SetButtonTextTintColor(EWidgetState s, float r, float g, float b, float a) { HRL_SetButtonTextTintColor(id_, ToCEnum<HRL_EWidgetState>(s), r, g, b, a); }
void Widget::SetButtonTextFont(const Font& f) { HRL_SetButtonTextFont(id_, f.GetID()); }
void Widget::SetButtonBackgroundTexture(EWidgetState s, const Texture* t) { HRL_SetButtonBackgroundTexture(id_, ToCEnum<HRL_EWidgetState>(s), t ? t->GetID() : 0); }
void Widget::SetButtonBackgroundTintColor(EWidgetState s, float r, float g, float b, float a) { HRL_SetButtonBackgroundTintColor(id_, ToCEnum<HRL_EWidgetState>(s), r, g, b, a); }
void Widget::SetButtonPressedCallback(ButtonPressedCallback cb, void* u) {
    auto* state = static_cast<ButtonCallbackState*>(buttonCallbackState_);
    delete state;
    buttonCallbackState_ = nullptr;
    if (!cb) {
        HRL_SetButtonPressedCallback(id_, nullptr, nullptr);
        return;
    }
    state = new ButtonCallbackState{cb, u};
    buttonCallbackState_ = state;
    HRL_SetButtonPressedCallback(id_, HRL_CPP_ButtonPressedBridge, state);
}
void Widget::SetLabelText(const char* t) { HRL_SetLabelText(id_, t); }
void Widget::SetLabelFont(const Font& f) { HRL_SetLabelFont(id_, f.GetID()); }
void Widget::SetLabelTintColor(float r, float g, float b, float a) { HRL_SetLabelTintColor(id_, r, g, b, a); }

void MouseMoved(float x,float y){HRL_MouseMovedCallback(x,y);}
void UpdateSkeletalAnimations(float d){HRL_UpdateSkeletalAnimations(d);}
void ClearScreen(){HRL_ClearScreen();}
void SetAntialiasingMode(EAntialiasingMode m){HRL_SetAntialiasingMode(static_cast<uint>(m));}
void SetAntialiasingMode(uint m){HRL_SetAntialiasingMode(m);}
void GetProjectionMatrix(float (&m)[16]){HRL_GetProjectionMatrix(m);}
void GetViewMatrix(float (&m)[16]){HRL_GetViewMatrix(m);}
void GetModelMatrix(const Mesh& mesh,float (&m)[16]){HRL_GetModelMatrix(mesh.GetID(),m);}
void SetDebugLineThickness(float v){HRL_SetDebugLineThickness(v);}
void DrawDebugSegment(const Scene& s,float ax,float ay,float az,float bx,float by,float bz,float r,float g,float b){HRL_DrawDebugSegment(s.GetID(),ax,ay,az,bx,by,bz,r,g,b);}
void DrawDebugPolygon(const Scene& s,EDebugRenderingType m,const float* x,const float* y,const float* z,int n,float r,float g,float b){HRL_DrawDebugPolygon(s.GetID(),ToCEnum<HRL_EDebugRenderingType>(m),x,y,z,n,r,g,b);}
void DrawDebugCircle(const Scene& s,EDebugRenderingType m,float x,float y,float z,float rad,int n,float r,float g,float b){HRL_DrawDebugCircle(s.GetID(),ToCEnum<HRL_EDebugRenderingType>(m),x,y,z,rad,n,r,g,b);}
void DrawDebugCapsule(const Scene& s,EDebugRenderingType m,float ax,float ay,float az,float bx,float by,float bz,float rad,int n,float r,float g,float b){HRL_DrawDebugCapsule(s.GetID(),ToCEnum<HRL_EDebugRenderingType>(m),ax,ay,az,bx,by,bz,rad,n,r,g,b);}
void DrawDebugPoint(const Scene& s,float x,float y,float z,float size,float r,float g,float b){HRL_DrawDebugPoint(s.GetID(),x,y,z,size,r,g,b);}

Vertex3D* GetVertex3DFromFBX(const void* d,std::size_t s,std::size_t& n){return reinterpret_cast<Vertex3D*>(HRL_GetVertex3DFromFBX(static_cast<const char*>(d),s,&n));}
SkeletalMeshData* GetSkeletalMeshFromFBX(const void* d,std::size_t s){return reinterpret_cast<SkeletalMeshData*>(HRL_GetSkeletalMeshFromFBX(static_cast<const char*>(d),s));}
void FreeSkeletalMeshData(SkeletalMeshData* d){HRL_FreeSkeletalMeshData(reinterpret_cast<HRL_SkeletalMeshData*>(d));}
void FreeVertex3DFromFBX(Vertex3D* v){HRL_FreeVertex3DFromFBX(reinterpret_cast<HRL_Vertex3D*>(v));}

} // namespace hrl
