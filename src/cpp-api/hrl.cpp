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

Mesh::Mesh()=default;
Mesh::Mesh(id v,bool owned):id_(v),owned_(owned){}
Mesh Mesh::Borrowed(id v){return Mesh(v,false);}
Mesh::~Mesh(){Release();}
id Mesh::GetID() const noexcept{return id_;}
bool Mesh::IsValid() const{return id_!=INVALID_ID && HRL_IsValidMesh(id_);}
void Mesh::Release(){if(id_!=INVALID_ID && owned_) HRL_DeleteMesh(id_); id_=INVALID_ID; owned_=false;}
Mesh::Mesh(Mesh&& o) noexcept:id_(o.id_),owned_(o.owned_){o.id_=INVALID_ID;o.owned_=false;}
Mesh& Mesh::operator=(Mesh&& o) noexcept{if(this!=&o){Release();id_=o.id_;owned_=o.owned_;o.id_=INVALID_ID;o.owned_=false;}return *this;}
void Mesh::SetPivotPoint(float x,float y,float z){HRL_SetMeshPivotPoint(id_,x,y,z);}
void Mesh::SetSpriteRegion(float a,float b,float c,float d){HRL_SetSpriteRegion(id_,a,b,c,d);}
void Mesh::SetMaterial(const Material& m){HRL_SetMeshMaterial(id_,m.GetID());}
void Mesh::SetLocation(float x,float y,float z){HRL_SetMeshLocation(id_,x,y,z);}
void Mesh::SetRotation(float x,float y,float z){HRL_SetMeshRotation(id_,x,y,z);}
void Mesh::SetScale(float x,float y,float z){HRL_SetMeshScale(id_,x,y,z);}
float Mesh::GetCameraDistance()const{return HRL_GetMeshCameraDistance(id_);}
void Mesh::SetLODAutomatic(bool v){HRL_SetMeshLODAutomatic(id_,v?HRL_TRUE:HRL_FALSE);}
void Mesh::SetLODMode(ELODMode v){HRL_SetMeshLODMode(id_,ToCEnum<HRL_ELODMode>(v));}
void Mesh::SetLODLevels(uint v){HRL_SetMeshLODLevels(id_,v);}
void Mesh::SetLODDistance(float v){HRL_SetMeshLODDistance(id_,v);}
void Mesh::SetLODScale(float v){HRL_SetMeshLODScale(id_,v);}
void Mesh::SetLODMinDistance(float v){HRL_SetMeshLODMinDistance(id_,v);}
void Mesh::SetLODMaxDistance(float v){HRL_SetMeshLODMaxDistance(id_,v);}
void Mesh::SetLODScreenThreshold(float v){HRL_SetMeshLODScreenThreshold(id_,v);}
void Mesh::SetLODScreenScale(float v){HRL_SetMeshLODScreenScale(id_,v);}
void Mesh::SetLODHysteresis(float v){HRL_SetMeshLODHysteresis(id_,v);}
void Mesh::SetLODOverride(int v){HRL_SetMeshLODOverride(id_,v);}
void Mesh::ForceLODRebuild(){HRL_ForceMeshLODRebuild(id_);}
uint Mesh::GetLODCount()const{return HRL_GetMeshLODCount(id_);}
std::size_t Mesh::GetLODVertexCount(uint l)const{return HRL_GetMeshLODVertexCount(id_,l);}
std::size_t Mesh::GetLODTriangleCount(uint l)const{return HRL_GetMeshLODTriangleCount(id_,l);}
int Mesh::GetLODLevel()const{return HRL_GetMeshLODLevel(id_);}
uint Mesh::GetSkeletalBoneCount()const{return HRL_GetSkeletalBoneCount(id_);}
const SkeletalBone* Mesh::GetSkeletalBone(uint i)const{return reinterpret_cast<const SkeletalBone*>(HRL_GetSkeletalBone(id_,i));}
uint Mesh::FindSkeletalBone(const char* n)const{return HRL_FindSkeletalBone(id_,n);}
uint Mesh::GetSkeletalAnimationCount()const{return HRL_GetSkeletalAnimationCount(id_);}
const SkeletalAnimation* Mesh::GetSkeletalAnimation(uint i)const{return reinterpret_cast<const SkeletalAnimation*>(HRL_GetSkeletalAnimation(id_,i));}
uint Mesh::FindSkeletalAnimation(const char* n)const{return HRL_FindSkeletalAnimation(id_,n);}
void Mesh::PlaySkeletalAnimation(uint i){HRL_PlaySkeletalAnimation(id_,i);}
void Mesh::StopSkeletalAnimation(){HRL_StopSkeletalAnimation(id_);}
void Mesh::SetSkeletalAnimationTime(float v){HRL_SetSkeletalAnimationTime(id_,v);}
void Mesh::SetSkeletalAnimationSpeed(float v){HRL_SetSkeletalAnimationSpeed(id_,v);}
void Mesh::SetSkeletalAnimationLoop(bool v){HRL_SetSkeletalAnimationLoop(id_,v?HRL_TRUE:HRL_FALSE);}
int Mesh::GetCurrentSkeletalAnimation()const{return HRL_GetCurrentSkeletalAnimation(id_);}
float Mesh::GetSkeletalAnimationTime()const{return HRL_GetSkeletalAnimationTime(id_);}
bool Mesh::IsSkeletalAnimationPlaying()const{return HRL_IsSkeletalAnimationPlaying(id_)!=0;}
void Mesh::SetSpriteDrawOrder(float v){HRL_SetSpriteDrawOrder(id_,v);}

HRL_COMMON(Light,HRL_IsValidLight,HRL_DeleteLight)
HRL_MOVE(Light)
void Light::SetColor(float x,float y,float z){HRL_SetLightColor(id_,x,y,z);}
void Light::SetIntensity(float v){HRL_SetLightIntensity(id_,v);}
void Light::SetAttenuation(float v){HRL_SetLightAttenuation(id_,v);}
void Light::SetLocation(float x,float y,float z){HRL_SetLightLocation(id_,x,y,z);}
void Light::SetRotation(float x,float y,float z){HRL_SetLightRotation(id_,x,y,z);}
void Light::SetCastShadows(bool v){HRL_SetLightCastShadows(id_,v?1:0);}
void Light::SetShadowBias(float v){HRL_SetLightShadowBias(id_,v);}
void Light::SetShadowResolution(int v){HRL_SetLightShadowResolution(id_,v);}
void Light::SetSpotLightInnerCutoff(float v){HRL_SetSpotLightInnerCutoff(id_,v);}
void Light::SetSpotLightOuterCutoff(float v){HRL_SetSpotLightOuterCutoff(id_,v);}

HRL_COMMON(Texture,HRL_IsValidTexture,HRL_DeleteTexture)
HRL_MOVE(Texture)
Texture Texture::FromMemory(const void* d,std::size_t s){return Texture(HRL_CreateTexture(static_cast<const char*>(d),s));}
Texture Texture::FromText(const char* t,const Font& f,float fs,float ww,float r,float g,float b,float br,float bg,float bb,float ba){
 return Texture(HRL_CreateTextureFromText(t,f.GetID(),fs,ww,r,g,b,br,bg,bb,ba));
}
void Texture::Reload(const void* d,std::size_t s){HRL_ReloadTexture(id_,static_cast<const char*>(d),s);}
void Texture::GetSize(int& w,int& h)const{HRL_GetTextureSize(id_,&w,&h);}
void Texture::SetMinFilter(EFilterType f){HRL_SetTextureMinFilter(id_,ToCEnum<HRL_EFilterType>(f));}
void Texture::SetMagFilter(EFilterType f){HRL_SetTextureMagFilter(id_,ToCEnum<HRL_EFilterType>(f));}

Scene::Scene(id v):id_(v){}
Scene::~Scene(){Release();}
Scene::Scene(bool v):id_(HRL_CreateScene(v?HRL_TRUE:HRL_FALSE)){}
id Scene::GetID() const noexcept{return id_;}
bool Scene::IsValid() const{return id_!=INVALID_ID && HRL_IsValidScene(id_);}
void Scene::Release(){if(id_!=INVALID_ID){HRL_DeleteScene(id_);id_=INVALID_ID;}}
HRL_MOVE(Scene)
Mesh Scene::CreateSprite(){return Mesh(HRL_CreateMeshSprite(id_));}
Mesh Scene::CreateMesh3D(const Vertex3D* v,std::size_t n,const uint* i,std::size_t ni){return Mesh(HRL_CreateMesh3D(id_,reinterpret_cast<const HRL_Vertex3D*>(v),n,reinterpret_cast<const HRL_uint*>(i),ni));}
Mesh Scene::CreateSkeletalMesh(const SkeletalMeshData& d){return Mesh(HRL_CreateSkeletalMesh(id_,reinterpret_cast<const HRL_SkeletalMeshData*>(&d)));}
Mesh Scene::CreateMeshFromFile(EMeshType t,const void* d,std::size_t s){return Mesh(HRL_CreateMeshFromFile(id_,ToCEnum<HRL_EMeshType>(t),static_cast<const char*>(d),s));}
Light Scene::CreateLight(ELightType t){return Light(HRL_CreateLight(id_,ToCEnum<HRL_ELightType>(t)));}
Camera Scene::CreateCamera(ECameraType t){return Camera(HRL_CreateCamera(id_,ToCEnum<HRL_ECameraType>(t)));}
Viewport Scene::CreateViewport(const Camera* c,float x,float y,float w,float h){return Viewport(HRL_CreateViewport(id_,c?c->GetID():INVALID_ID,x,y,w,h));}
PostProcess Scene::CreatePostProcess(const Material& m,int p){return PostProcess(HRL_CreatePostProcess(id_,m.GetID(),p));}
void Scene::ResizeTexture(int w,int h){HRL_ResizeSceneTexture(id_,w,h);}
void Scene::SetSkySphereEnabled(bool v){HRL_SetSkySphereEnabled(id_,v?1:0);}
void Scene::SetSkySphereColors(float a,float b,float c,float d,float e,float f,float g,float h,float i){HRL_SetSkySphereColors(id_,a,b,c,d,e,f,g,h,i);}
void Scene::SetSkySphereRotation(float a,float b,float c){HRL_SetSkySphereRotation(id_,a,b,c);}
void Scene::SetSkySphereTexture(const Texture* t){HRL_SetSkySphereTexture(id_,t?t->GetID():INVALID_ID);}
void Scene::SetEnvironmentMappingEnabled(bool v){HRL_SetEnvironmentMappingEnabled(id_,v?1:0);}
void Scene::SetEnvironmentMap(const Texture* t){HRL_SetEnvironmentMap(id_,t?t->GetID():INVALID_ID);}
void Scene::EnableColorPickingBuffer(bool v){HRL_EnableColorPickingBuffer(id_,v?1:0);}
Mesh Scene::GetHoveredObject(int x,int y,EMeshType* t){
    HRL_EMeshType cType;
    HRL_id meshId = HRL_GetHoveredObject(id_,x,y,t ? &cType : nullptr);
    if (t) *t = static_cast<EMeshType>(cType);
    return Mesh::Borrowed(meshId);
}
void Scene::SetFogEnabled(bool v){HRL_SetFogEnabled(id_,v?1:0);}
void Scene::SetFogMode(EFogType m){HRL_SetFogMode(id_,ToCEnum<HRL_EFogType>(m));}
void Scene::SetFogColor(float r,float g,float b){HRL_SetFogColor(id_,r,g,b);}
void Scene::SetFogDensity(float d){HRL_SetFogDensity(id_,d);}
void Scene::SetFogLinearRange(float a,float b){HRL_SetFogLinearRange(id_,a,b);}
void Scene::DrawAsDebugMode(EDebugView m){HRL_DrawSceneAsDebugMode(id_,ToCEnum<HRL_EDebugView>(m));}
void Scene::TakeScreenshot(const char* p)const{HRL_TakeScreenshot(id_,p);}

HRL_COMMON(PostProcess,HRL_IsValidPostProcess,HRL_DeletePostProcess)
HRL_MOVE(PostProcess)

HRL_COMMON(Shader,HRL_IsValidShader,HRL_DeleteShader)
HRL_MOVE(Shader)
Shader Shader::FromSource(const void* v,std::size_t vs,const void* f,std::size_t fs){
 return Shader(HRL_CreateShader(static_cast<const char*>(v),vs,static_cast<const char*>(f),fs));
}

HRL_COMMON(Material,HRL_IsValidMaterial,HRL_DeleteMaterial)
HRL_MOVE(Material)
Material Material::Create(const Shader& s){return Material(HRL_CreateMaterial(s.GetID()));}
void Material::SetInt(const char* n,int v){HRL_MaterialSetInt(id_,n,v);}
void Material::SetTexture(const char* n,const Texture& t){HRL_MaterialSetTexture(id_,n,t.GetID());}
void Material::SetBool(const char* n,bool v){HRL_MaterialSetBool(id_,n,v?1:0);}
void Material::SetFloat(const char* n,float v){HRL_MaterialSetFloat(id_,n,v);}
void Material::SetVec2(const char* n,float x,float y){HRL_MaterialSetVec2(id_,n,x,y);}
void Material::SetVec3(const char* n,float x,float y,float z){HRL_MaterialSetVec3(id_,n,x,y,z);}
void Material::SetVec4(const char* n,float x,float y,float z,float w){HRL_MaterialSetVec4(id_,n,x,y,z,w);}
void Material::SetEmissiveColor(float r,float g,float b,float a){HRL_MaterialSetEmissiveColor(id_,r,g,b,a);}

HRL_COMMON(Viewport,HRL_IsValidViewport,HRL_DeleteViewport)
HRL_MOVE(Viewport)
void Viewport::SetCamera(const Camera* c){HRL_SetViewportCamera(id_,c?c->GetID():INVALID_ID);}
void Viewport::SetRect(float x,float y,float w,float h){HRL_SetViewportRect(id_,x,y,w,h);}
Widget Viewport::CreateWidget(EWidgetType t){return Widget(HRL_CreateWidget(id_,ToCEnum<HRL_EWidgetType>(t)));}

HRL_COMMON(Camera,HRL_IsValidCamera,HRL_DeleteCamera)
HRL_MOVE(Camera)
void Camera::SetType(ECameraType t){HRL_SetCameraType(id_,ToCEnum<HRL_ECameraType>(t));}
void Camera::SetOrthoVertical(float v){HRL_SetCameraOrthoVertical(id_,v);}
void Camera::SetPerspectiveFov(float v){HRL_SetCameraPerspectiveFov(id_,v);}
void Camera::SetNearPlane(float v){HRL_SetCameraNearPlane(id_,v);}
void Camera::SetFarPlane(float v){HRL_SetCameraFarPlane(id_,v);}
void Camera::SetLocation(float x,float y,float z){HRL_SetCameraLocation(id_,x,y,z);}
void Camera::SetRotation(float x,float y,float z){HRL_SetCameraRotation(id_,x,y,z);}

HRL_COMMON(Font,HRL_IsValidFont,HRL_DeleteFont)
HRL_MOVE(Font)
Font Font::FromMemory(const void* d,std::size_t s){return Font(HRL_CreateFont(static_cast<const char*>(d),s));}

Widget::Widget()=default;
Widget::Widget(id v):id_(v){}
Widget::~Widget(){Release();}
id Widget::GetID() const noexcept{return id_;}
bool Widget::IsValid() const{return id_!=INVALID_ID;}
void Widget::Release(){
    if(id_!=INVALID_ID){
        HRL_SetButtonPressedCallback(id_,nullptr,nullptr);
        HRL_DeleteWidget(id_);
        id_=INVALID_ID;
    }
    delete static_cast<ButtonCallbackState*>(buttonCallbackState_);
    buttonCallbackState_=nullptr;
}
HRL_MOVE(Widget)
void Widget::SetPosition(float x,float y){HRL_SetWidgetPosition(id_,x,y);}
void Widget::SetSize(float w,float h){HRL_SetWidgetSize(id_,w,h);}
void Widget::SetAlpha(float a){HRL_SetWidgetAlpha(id_,a);}
bool Widget::IsHovered()const{return HRL_IsWidgetHovered(id_)!=0;}
void Widget::SetAnchor(float x,float y){HRL_SetWidgetAnchor(id_,x,y);}
void Widget::SetButtonClickable(bool v){HRL_SetButtonClickable(id_,v?1:0);}
void Widget::SetButtonText(const char* t){HRL_SetButtonText(id_,t);}
void Widget::SetButtonTextTintColor(EWidgetState s,float r,float g,float b,float a){HRL_SetButtonTextTintColor(id_,ToCEnum<HRL_EWidgetState>(s),r,g,b,a);}
void Widget::SetButtonTextFont(const Font& f){HRL_SetButtonTextFont(id_,f.GetID());}
void Widget::SetButtonBackgroundTexture(EWidgetState s,const Texture* t){HRL_SetButtonBackgroundTexture(id_,ToCEnum<HRL_EWidgetState>(s),t?t->GetID():0);}
void Widget::SetButtonBackgroundTintColor(EWidgetState s,float r,float g,float b,float a){HRL_SetButtonBackgroundTintColor(id_,ToCEnum<HRL_EWidgetState>(s),r,g,b,a);}
void Widget::SetButtonPressedCallback(ButtonPressedCallback cb,void* u){
    auto* state = static_cast<ButtonCallbackState*>(buttonCallbackState_);
    delete state;
    buttonCallbackState_ = nullptr;
    if (!cb) {
        HRL_SetButtonPressedCallback(id_,nullptr,nullptr);
        return;
    }
    state = new ButtonCallbackState{cb,u};
    buttonCallbackState_ = state;
    HRL_SetButtonPressedCallback(id_,HRL_CPP_ButtonPressedBridge,state);
}
void Widget::SetLabelText(const char* t){HRL_SetLabelText(id_,t);}
void Widget::SetLabelFont(const Font& f){HRL_SetLabelFont(id_,f.GetID());}
void Widget::SetLabelTintColor(float r,float g,float b,float a){HRL_SetLabelTintColor(id_,r,g,b,a);}

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
