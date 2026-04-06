#include "gl33_renderer.h"
#include "gl33_shader.h"
#include "gl33_texture.h"

#include "../../ressources/ressources.h"
#include "../../core/utils_functions.h"
#include "../../hrl.h"

//on va faire les implémentations des fonctions gl-api
#include "../../hrl_gl.h"

//GL et GLM
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

//Debug printf
#include <cstdio>

//Memory
#include <unordered_map>
#include <string>


#define Buffer_Num      4

#define Sprite_Buffer   0
#define Mesh2D_Buffer   1
#define Mesh3D_Buffer   2
#define Debug_Buffer   2


//ordre d'importance du batching :
// 1 - Type de mesh, ex : sprite, mesh2D, etc
// 2 - Texture
// 3 - Material
// 4 - Shader


//OpenGL objects//
static GLuint vao[Buffer_Num];
static GLuint vbo[Buffer_Num];
static GLuint ebo[Buffer_Num];


//Ubo : Uniform partagés entre plusieurs shaders
#define Ubo_Num         2

#define LIGHT_UBO       0
#define CAMERA_UBO      1

static GLuint ubo[Ubo_Num];


#define MAX_LIGHTS      32


//indices pour deux triangles
static unsigned int sprite_indices[] = {
    0, 1, 2,
    2, 3, 0
};


//light
/**
 * La norme std 140 de opengl a respecter pour les ubo demande d'alligner les objets
 * sur des multiples de 16, donc on ajoute des paddings pour correspondre
 */
typedef struct {
  //16 bytes {
  //4 bytes
  uint32_t type;
  //4 bytes
  float intensity;
  //4 bytes
  float attenuation;
  //4 bytes
  float padding1;
  // }

  //16 bytes {
  //12 bytes
  glm::vec3 position;
  //4 bytes
  float padding2;
  // }

  //16 bytes {
  //12 bytes
  glm::vec3 rotation;
  //4 bytes
  float padding3;
  // }

  //16 bytes {
  //12 bytes
  glm::vec3 color;
  //4 bytes
  float padding4;
  // }
}GL_Light_t;



typedef struct {
  GLuint texture_;
  GLuint framebuffer_;
  unsigned int width_, height_;
}GL_Scene_t;
static std::unordered_map<HRL_id, GL_Scene_t*> gpu_scenes_;




void check_errors(int line)
{
  GLenum err; \
  while ((err = glGetError()) != GL_NO_ERROR) \
    printf("OpenGL error: 0x%X after line %d\n", err, line);
}
#define GL33_CheckErrors() check_errors(__LINE__)




//shaders//
std::unordered_map<HRL_id, GL33_Shader*> shaders_;


//Textures//
static std::unordered_map<HRL_id, GL33_Texture*> textures_;

#define ALBEDO_INT (0)
#define NORMAL_INT (1)
#define SPECULAR_INT (2)
#define ROUGHNESS_INT (3)
#define METALIC_INT (4)
#define ALPHA_INT (5)
const char* tex_uniform_name[6]
{
  "T_Albedo", "T_Normal", "T_Specular", "T_Roughness", "T_Metalic", "T_Alpha"
};

static std::unordered_map<int, HRL_id> fallback_textures_;



//Debug//
const std::unordered_map<HRL_id, DebugRenderer>* internal_debug_renderer=nullptr;


/** Backend Implementation */

void GL33_Init() {}

void GL33_InitContext(HRL_uint _width, HRL_uint _height, void* loader)
{
  // ici _handle = contexte déja créé / rendu attaché
  // GLAD doit charger les fonctions : 
  // tu passes une fonction de platform loader adaptée
  // ex: wglGetProcAddress sur Windows, glXGetProcAddress sur Linux

  if (!gladLoadGLLoader((GLADloadproc)loader))
  {
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL, "Failed to init GLAD, (loader error)");
    return;
  }

  //activer la transparence des shaders
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  //activer l'anti-aliasing
  glEnable(GL_MULTISAMPLE);

  //on crée les vao
  glGenVertexArrays(Buffer_Num, vao);
  //on crée les vbo
  glGenBuffers(Buffer_Num, vbo);
  //on crée les ebo
  glGenBuffers(Buffer_Num, ebo);

  //on crée le ubo
  glGenBuffers(Ubo_Num, ubo);

  //ubo light
  glBindBuffer(GL_UNIFORM_BUFFER, ubo[LIGHT_UBO]);
  glBufferData(GL_UNIFORM_BUFFER, MAX_LIGHTS * sizeof(HRL_Light), nullptr, GL_DYNAMIC_DRAW);
  glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo[LIGHT_UBO]);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);

  //mettre ici le ubo camera
  //
  //

  //on va initialiser le vao et vbo Sprite
  glBindVertexArray(vao[Sprite_Buffer]);

  //on bind le vbo sprite
  glBindBuffer(GL_ARRAY_BUFFER, vbo[Sprite_Buffer]);

  //allouer la bonne taille mais sans valeur par défaut
  //16 car on passe 2 float et 2 coordonées UV par vertex (donc 16 en tout pour 4 vertex)
  glBufferData(GL_ARRAY_BUFFER, 16*sizeof(float), nullptr, GL_DYNAMIC_DRAW);

  //coordonées des points
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);

  //coordonées de texture (UV)
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
  glEnableVertexAttribArray(1);

  //on bind le ebo
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo[Sprite_Buffer]);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(sprite_indices), sprite_indices, GL_DYNAMIC_DRAW);

  //Sprite shader
  //ajouter une gestion des erreurs
  auto* spriteShader = new GL33_Shader();
  spriteShader->GL33_Create(
    (const char*)res_sprite_vert_glsl,
    res_sprite_vert_glsl_len,
    (const char*)res_sprite_frag_glsl,
    res_sprite_frag_glsl_len);

  shaders_.emplace(HRL_SpriteShader, spriteShader);

  //gerer les vao Mesh2D et Mesh3D ici
  //
  //

  //debug
  glBindVertexArray(vao[Debug_Buffer]);
  glBindBuffer(GL_ARRAY_BUFFER, vbo[Debug_Buffer]);

  //nullptr car taille inconnue à l'avance, réallouée au flush
  glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);

  //position (x, y, z) — 3 floats
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);

  //couleur (r, g, b) — 3 floats
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
  glEnableVertexAttribArray(1);
  //pas de glBindBuffer(GL_ELEMENT_ARRAY_BUFFER) — pas d'EBO

  auto* debugShader = new GL33_Shader();
  debugShader->GL33_Create(
    (const char*)res_debug_vert_glsl,
    res_debug_vert_glsl_len,
    (const char*)res_debug_frag_glsl,
    res_debug_frag_glsl_len);

  shaders_.emplace(HRL_DebugShader, debugShader);



  //Creer les texures de fallback
  fallback_textures_[ALBEDO_INT] = GL33_CreateTexture((const char*)res_default_albedo_png, res_default_albedo_png_len);
  fallback_textures_[NORMAL_INT] = GL33_CreateTexture((const char*)res_default_normal_png, res_default_normal_png_len);
  fallback_textures_[SPECULAR_INT] = GL33_CreateTexture((const char*)res_default_specular_png, res_default_specular_png_len);
  fallback_textures_[ROUGHNESS_INT] = GL33_CreateTexture((const char*)res_default_roughness_png, res_default_roughness_png_len);
  fallback_textures_[METALIC_INT] = GL33_CreateTexture((const char*)res_default_metalic_png, res_default_metalic_png_len);
  fallback_textures_[ALPHA_INT] = GL33_CreateTexture((const char*)res_default_alpha_png, res_default_alpha_png_len);

  assert(fallback_textures_[ALBEDO_INT] != HRL_InvalidID && "Failed to load fallback albedo");
  assert(fallback_textures_[NORMAL_INT] != HRL_InvalidID && "Failed to load fallback normal");
  assert(fallback_textures_[SPECULAR_INT] != HRL_InvalidID && "Failed to load fallback specular");
  assert(fallback_textures_[ROUGHNESS_INT] != HRL_InvalidID && "Failed to load fallback roughness");
  assert(fallback_textures_[METALIC_INT] != HRL_InvalidID && "Failed to load fallback metalic");
  assert(fallback_textures_[ALPHA_INT] != HRL_InvalidID && "Failed to load fallback alpha");

}

void GL33_ResetFramebuffer()
{
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, GetWindowWidth(), GetWindowHeight());
}


void GL33_Shutdown()
{
  glDeleteVertexArrays(Buffer_Num, vao);
  glDeleteBuffers(Buffer_Num, vbo);
  glDeleteBuffers(Buffer_Num, ebo);
}

//passer plus tard a une structure RenderContext
static GL_Scene_t* currentScene;
static HRL_Viewport* currentViewport;
static HRL_Camera* currentCamera;
static GL33_Shader* currentShader;

static unsigned int currentWidth, currentHeight;

static glm::mat4 cached_proj_mat_;
static glm::mat4 cached_view_mat_;

//calculate matrices
glm::mat4 CalculateProjectionMatrix()
{
  //taille absolue du viewport width et height (on prend en compte la taille de la fenetre et la taille relative du viewport HRL)
  float viewportWidth  = (float)currentWidth * currentViewport->width_;
  float viewportHeight = (float)currentHeight * currentViewport->height_;

  //on evite la division par 0
  if (viewportHeight < 1e-3f)
  {
    viewportHeight = 1.f;
  }

  //calcul du ratio largeur/hauteur du viewport
  float aspect = viewportWidth / viewportHeight;

  glm::mat4 proj;
  if (currentCamera->type_ == HRL_Perspective)
  {
    proj = glm::perspective(glm::radians(currentCamera->value_), aspect, currentCamera->near_plane_, currentCamera->far_plane_);
  }
  else
  {
    //on calcule la taille en hauteur d'abord, puis on fait le calcul de la largeur en fonction de la hauteur et de l'aspect
    float halfHeight = currentCamera->value_ * 0.5f;
    float halfWidth  = halfHeight * aspect;
    proj = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight,
                      currentCamera->near_plane_, currentCamera->far_plane_);
  }
  return proj;
}
glm::mat4 CalculateViewMatrix()
{
  //position et vue de la camera
  glm::mat4 view = glm::lookAt(
    currentCamera->position_,
    currentCamera->position_ + GetForwardVector(currentCamera->rotation_),
    GetUpVector(currentCamera->rotation_)
  );
  return view;
}
//called by API pipeline
void GL33_ComputeFrameMatrices()
{
  cached_proj_mat_ = CalculateProjectionMatrix();
  cached_view_mat_ = CalculateViewMatrix();
}


//stocke le nombre de texures bindées avant de draw.
//permet de savoir jusqu'a ou unbind les slots gl
static int textureSlotsBinded;

void GL33_BindScene(HRL_id _sceneid)
{
  auto it = gpu_scenes_.find(_sceneid);
  if (it == gpu_scenes_.end())
  {
    SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "GL33 Bind Scene error : scene id not valid");
    return;
  }
  currentScene = it->second;
  //si on veut draw on screen, le framebuffer id sera 0, donc l'ecran
  glBindFramebuffer(GL_FRAMEBUFFER, it->second->framebuffer_);
}

void GL33_ClearScene()
{
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
}

void GL33_BindViewport(HRL_Viewport* viewport)
{
  //si on render sur l'ecran, on prend les dimensions de l'ecran, sinon, la taille de la scene
  if (currentScene->framebuffer_ == 0)
  {
    currentWidth = GetWindowWidth();
    currentHeight = GetWindowHeight();
  }
  else
  {
    currentWidth = currentScene->width_;
    currentHeight = currentScene->height_;
  }

  glViewport((GLint)(viewport->x_*(float)currentWidth),
    //pour que 0 soit le haut et 1 le bas
    (GLint)((1 - viewport->y_ - viewport->height_)*(float)currentHeight),
    (GLint)(viewport->width_ * (float)currentWidth),
    (GLint)(viewport->height_ * (float)currentHeight));
  currentViewport = viewport;
  currentCamera = viewport->camera_;
}

static void ApplyFallback(int index)
{
  //texture non trouvée, on passe la fallback texture
  glActiveTexture(GL_TEXTURE0 + index);
  HRL_id fallback_hrl_id = fallback_textures_[index];
  glBindTexture(GL_TEXTURE_2D, textures_[fallback_hrl_id]->GetGL_ID());
}


void GL33_BindMaterial(HRL_Material* mat)
{
  auto it = shaders_.find(mat->shader_);
  if (it == shaders_.end())
  {
    SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_FATAL, "Bind material error : shader doesn't exists");
    return;
  }
  auto* s = it->second;
  //on set CurrentShader pour spécifier que les prochains calls utiliseront ce shader
  currentShader = s;
  s->Use();

  s->SetMat4("projection", cached_proj_mat_);
  s->SetMat4("view", cached_view_mat_);

  //on passe tous les uniforms donnés par l'utilisateur
  for (const auto& [name, value] : mat->intParams_)
  {
    s->SetInt(name, value);
  }

  //utilisé pour unbind les textures apres avoir draw
  textureSlotsBinded = (int)mat->textureParams_.size();

  for (int i=0; i<6; i++)
  {
    //on passe toujours les memes uniforms
    s->SetInt(tex_uniform_name[i], i);

    //on recherche la texture
    auto itParam = mat->textureParams_.find(std::string(tex_uniform_name[i]));
    if (itParam == mat->textureParams_.end())
    {
      ApplyFallback(i);
      continue;
    }

    auto itTexture = textures_.find(itParam->second);
    if (itTexture == textures_.end())
    {
      ApplyFallback(i);
      continue;
    }

    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, itTexture->second->GetGL_ID());
  }

  for (auto [name, value] : mat->floatParams_)
  {
    s->SetFloat(name, value);
  }
  for (auto [name, value] : mat->vec2Params_)
  {
    s->SetVec2(name, value);
  }
  for (auto [name, value] : mat->vec3Params_)
  {
    s->SetVec3(name, value);
  }
  for (auto [name, value] : mat->vec4Params_)
  {
    s->SetVec4(name, value);
  }
}

void GL33_DrawMesh(HRL_Mesh* mesh)
{
  glm::mat4 model = glm::mat4(1.f);

  //aller à la position du mesh
  model = glm::translate(model, mesh->position_);

  //aller au pivot
  model = glm::translate(model, mesh->pivot_point_);

  //tourner
  model = glm::rotate(model, mesh->rotation_.x, glm::vec3(1.f, 0.f, 0.f));
  model = glm::rotate(model, mesh->rotation_.y, glm::vec3(0.f, 1.f, 0.f));
  model = glm::rotate(model, mesh->rotation_.z, glm::vec3(0.f, 0.f, 1.f));

  //revenir en arrière
  model = glm::translate(model, -mesh->pivot_point_);

  //scale
  model = glm::scale(model, mesh->scale_);

  currentShader->SetMat4("model", model);

  if (mesh->type_ == HRL_Sprite)
  {
    float uMin, vMin, uMax, vMax;
    auto* sprite = static_cast<HRL_MeshSprite*>(mesh);
    uMin = sprite->region_[0];
    vMin = sprite->region_[1];
    uMax = sprite->region_[2];
    vMax = sprite->region_[3];

    float vertices[] = {
      -0.5f, -0.5f,  uMin, vMin,
       0.5f, -0.5f,  uMax, vMin,
       0.5f,  0.5f,  uMax, vMax,
      -0.5f,  0.5f,  uMin, vMax,
    };

    //on bind le bon vao (pas besoin de bind de vbo ou ebo car ils sont configurés pour le vao)
    glBindVertexArray(vao[Sprite_Buffer]);

    //on bind le vbo et on lui envoie les données du mesh
    glBindBuffer(GL_ARRAY_BUFFER, vbo[Sprite_Buffer]);

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);

    //desactiver si 2D, activer si 3D
    glDisable(GL_DEPTH_TEST);


    //on draw sur tous le render target
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

    //reset les binds de textures (changer avec le nouveau systeme)
    for (int i = 0; i < textureSlotsBinded; i++)
    {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
  }
}


void GL33_UpdateLights(const std::vector<HRL_Light*>& _lights)
{
  //on commence par creer le tableau de GL_Lights_t
  GL_Light_t gpuLights[MAX_LIGHTS];
  size_t count = 0;

  //on rempli le tableau
  for (const auto& light: _lights)
  {
    if (count >= MAX_LIGHTS)
    {
      break;
    }

    gpuLights[count].type = light->type_;
    gpuLights[count].intensity = light->intensity_;
    gpuLights[count].attenuation = light->attenuation_;
    gpuLights[count].padding1 = 0.f;

    gpuLights[count].position = light->position_;
    gpuLights[count].padding2 = 0.f;

    gpuLights[count].rotation = light->rotation_;
    gpuLights[count].padding3 = 0.f;

    gpuLights[count].color = light->color_;
    gpuLights[count].padding4 = 0.f;

    ++count;
  }

  //on passe les données à opengl
  glBindBuffer(GL_UNIFORM_BUFFER, ubo[LIGHT_UBO]);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, count * sizeof(GL_Light_t), gpuLights);
  GL33_CheckErrors();
}


HRL_id GL33_CreateTexture(const char* _imageContent, size_t _imageSize)
{
  //on crée la texture et on récupere le code d'erreur
  auto* t = new GL33_Texture();
  int error = t->GL33_Create(_imageContent, _imageSize);

  //si il n'y a pas d'erreur, on génere un ID et on push la texture dans la liste des textures, sinon on retourne invalid
  //la classe texture s'occupe des codes d'erreurs HRL, pas besoin de le faire ici.
  if (error == 0)
  {
    HRL_id id = GenerateHRL_ID();
    textures_.emplace(id, t);
    return id;
  }
  return HRL_InvalidID;
}

HRL_id GL33_CreateTextureFromBitmap(BitmapResult bmp)
{
  //on crée la texture et on récupere le code d'erreur
  auto* t = new GL33_Texture();
  int error = t->GL33_CreateFromBitmap(&bmp);

  //si il n'y a pas d'erreur, on génere un ID et on push la texture dans la liste des textures, sinon on retourne invalid
  //la classe texture s'occupe des codes d'erreurs HRL, pas besoin de le faire ici.
  if (error == 0)
  {
    HRL_id id = GenerateHRL_ID();
    textures_.emplace(id, t);
    return id;
  }
  return HRL_InvalidID;
}


void GL33_DeleteTexture(HRL_id _id)
{
  auto it = textures_.find(_id);
  if (it == textures_.end())
  {
    SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "DeleteTexture error : Texture ID doesn't exists");
    return;
  }
  delete it->second;
  textures_.erase(it);
}

void GL33_GetTextureSize(HRL_id id, int *width, int *height)
{
  auto it = textures_.find(id);
  if (it == textures_.end())
  {
    SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_GetTextureSize error : Texture ID doesn't exists");
    return;
  }
  *width = (int)it->second->GetWidth();
  *height = (int)it->second->GetHeight();
}

void GL33_SetTextureMinFilter(HRL_id id, HRL_uint _filter)
{
  auto it = textures_.find(id);
  if (it == textures_.end())
  {
    SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_SetTextureMinFilter error : Texture ID doesn't exists");
    return;
  }
  it->second->SetMinFilter(_filter);
}

void GL33_SetTextureMaxFilter(HRL_id id, HRL_uint _filter)
{
  auto it = textures_.find(id);
  if (it == textures_.end())
  {
    SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_SetTextureMaxFilter error : Texture ID doesn't exists");
    return;
  }
  it->second->SetMaxFilter(_filter);
}




HRL_id GL33_CreateShader(const char *_vertContent, size_t _vertSize, const char *_fragContent, size_t _fragSize)
{
  //on crée le shader et on recupere le code d'erreur
  auto* s = new GL33_Shader();
  int error = s->GL33_Create(_vertContent, _vertSize, _fragContent, _fragSize);

  //si il n'y a pas d'erreur, on génere un ID et on push le shader dans la liste des shaders, sinon on retourne invalid
  //la classe shader s'occupe des codes d'erreurs HRL, pas besoin de le faire ici.
  if (error == 0)
  {
    HRL_id id = GenerateHRL_ID();
    shaders_.emplace(id, s);
    return id;
  }
  return HRL_InvalidID;
}

void GL33_DeleteShader(HRL_id _id)
{
  auto it = shaders_.find(_id);
  if (it == shaders_.end())
  {
    SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "DeleteShader error : Shader ID doesn't exists");
    return;
  }
  else
  {
    delete it->second;
    shaders_.erase(it);
  }
}


void GL33_CreateScene(HRL_id _newSceneid, int _renderOnScreen)
{
  auto* scene = new GL_Scene_t();
  if (_renderOnScreen)
  {
    //le framebuffer est 0 (ecran)
    scene->framebuffer_ = 0;
  }
  else
  {
    glGenTextures(1, &scene->texture_);
    glBindTexture(GL_TEXTURE_2D, scene->texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 480, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    scene->width_ = 480;
    scene->height_ = 480;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &scene->framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, scene->framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene->texture_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    assert(status == GL_FRAMEBUFFER_COMPLETE && "Framebuffer incomplete!");
  }

  //les HRL_id sont partagés entre le backend et l'api
  gpu_scenes_.emplace(_newSceneid, scene);
}

void GL33_DeleteScene(HRL_id _sceneid)
{
  auto it = gpu_scenes_.find(_sceneid);
  if (it == gpu_scenes_.end())
  {
    SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_DeleteScene error : invalid scene id");
    return;
  }

  if (it->second->framebuffer_ != 0)
  {
    //scene is not rendered at screen
    glDeleteTextures(1, &it->second->texture_);
    glDeleteFramebuffers(1, &it->second->framebuffer_);
  }
  delete it->second;
  gpu_scenes_.erase(it);
}

void GL33_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height)
{
  auto it = gpu_scenes_.find(_sceneid);
  if (it == gpu_scenes_.end())
  {
    SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "GL33_ResizeSceneTexture error : invalid scene id");
    return;
  }
  if (it->second->framebuffer_ == 0)
  {
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING, "GL33_ResizeSceneTexture error : scene is render on the screen");
    return;
  }

  GLuint tex = it->second->texture_;
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _width, _height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

  it->second->width_ = _width;
  it->second->height_ = _height;
}


//Matrices
void GL33_GetProjectionMatrix(float *aa)
{
  glm::mat4 proj = cached_proj_mat_;
  memcpy(aa, glm::value_ptr(proj), sizeof(float) * 16);
}

void GL33_GetViewMatrix(float *aa)
{
  glm::mat4 proj = cached_view_mat_;
  memcpy(aa, glm::value_ptr(proj), sizeof(float) * 16);
}

void GL33_GetModelMatrix(HRL_Mesh* mesh, float *aa)
{
  glm::mat4 model = glm::mat4(1.f);

  //aller à la position du mesh
  model = glm::translate(model, mesh->position_);

  //aller au pivot
  model = glm::translate(model, mesh->pivot_point_);

  //tourner
  model = glm::rotate(model, mesh->rotation_.x, glm::vec3(1.f, 0.f, 0.f));
  model = glm::rotate(model, mesh->rotation_.y, glm::vec3(0.f, 1.f, 0.f));
  model = glm::rotate(model, mesh->rotation_.z, glm::vec3(0.f, 0.f, 1.f));

  //revenir en arrière
  model = glm::translate(model, -mesh->pivot_point_);

  //scale
  model = glm::scale(model, mesh->scale_);

  memcpy(aa, glm::value_ptr(model), sizeof(float) * 16);
}



//Debug//
struct GL33_DebugRenderer {
  size_t current_buffer_size=0;
};

//passer a une map pour gerer toutes les scenes
static GL33_DebugRenderer gl_debug_renderer{};

void GL33_DrawDebug(const DebugRenderer& _renderer, float line_thickness)
{
  auto it = shaders_.find(HRL_DebugShader);
  if (it == shaders_.end())
  {
    SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL, "GL33_DrawDebug error : debug shader doesn't exists");
    return;
  }
  auto* s = it->second;
  //on set CurrentShader pour spécifier que les prochains calls utiliseront ce shader
  currentShader = s;
  s->Use();

  s->SetMat4("projection", cached_proj_mat_);
  s->SetMat4("view", cached_view_mat_);

  //bind vao and initialize opengl evironement
  glBindVertexArray(vao[Debug_Buffer]);
  glBindBuffer(GL_ARRAY_BUFFER, vbo[Debug_Buffer]);

  glEnable(GL_DEPTH_TEST);

  //remplacer par une seule fonction dans la vtable pour eviter de le faire a chaque frames
  glLineWidth(line_thickness);

  // lignes
  if (!_renderer.lines.empty())
  {
    size_t size = _renderer.lines.size() * sizeof(DebugVertex);

    //réalloue si le buffer est trop petit
    if (size > gl_debug_renderer.current_buffer_size)
    {
      glBufferData(GL_ARRAY_BUFFER, (GLsizei)size, _renderer.lines.data(), GL_STREAM_DRAW);
      gl_debug_renderer.current_buffer_size = size;
    }
    else
    {
      glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizei)size, _renderer.lines.data());
    }

    glDrawArrays(GL_LINES, 0, (GLsizei)_renderer.lines.size());
  }

  // triangles
  if (!_renderer.triangles.empty())
  {
    size_t size = _renderer.triangles.size() * sizeof(DebugVertex);

    if (size > gl_debug_renderer.current_buffer_size)
    {
      glBufferData(GL_ARRAY_BUFFER, (GLsizei)size, _renderer.triangles.data(), GL_STREAM_DRAW);
      gl_debug_renderer.current_buffer_size = size;
    }
    else
    {
      glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizei)size, _renderer.triangles.data());
    }

    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)_renderer.triangles.size());
  }


}



///////// HRL_GL (hrl_gl.h) /////////

unsigned int HRL_GL_GetTextureGL_ID(HRL_id _textureid)
{
  auto it = textures_.find(_textureid);
  if (it == textures_.end())
  {
    SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetTextureGL_ID error : Texture ID doesn't exists");
    return GL_INVALID_VALUE;
  }

  return it->second->GetGL_ID();
}


unsigned int HRL_GL_GetShaderGL_ID(HRL_id _shaderid)
{
  auto it = shaders_.find(_shaderid);
  if (it == shaders_.end())
  {
    SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetShaderGL_ID error : Shader ID doesn't exists");
    return GL_INVALID_VALUE;
  }

  return it->second->GetId();
}

unsigned int HRL_GL_GetSceneTextureGL_ID(HRL_id _sceneid)
{
  auto it = gpu_scenes_.find(_sceneid);
  if (it == gpu_scenes_.end())
  {
    SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetSceneTextureGL_ID : scene ID is not valid");
    return GL_INVALID_VALUE;
  }

  return it->second->texture_;
}