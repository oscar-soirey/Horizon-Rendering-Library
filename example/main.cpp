//HRL single header
#include <hrl/hrl.h>
#include <hrl/hrl_gl.h>

//Window
#include <iosfwd>
#include <glfw/glfw3.h>
#include <fstream>

#include <iostream>

#include "src/example.h"

#include <glm/glm.hpp>


typedef struct {
  float x, y, z;
}vec3;



//frame time
double dt;
double currentTime;
double lastTime;
void CalculateDeltaTime()
{
  currentTime = glfwGetTime();

  // delta time en secondes
  dt = currentTime - lastTime;

  // mettre à jour lastTime pour la prochaine frame
  lastTime = currentTime;
}


//GLFW window resize callback
void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
  HRL_WindowResizeCallback(width, height);
}


//variables de déplacement de la camera
float cameraSpeed = 5.0f;   // unités par seconde
float mouseSensitivity = 0.1f;

float yaw = 0.0f;    // rotation autour de Y
float pitch = 0.0f;  // rotation autour de X

float camX = 0.0f;
float camY = 0.0f;
float camZ = 5.0f;

double lastMouseX = 0.0;
double lastMouseY = 0.0;
bool firstMouse = true;

// ─────────────────────────────────────────────────────────────────────────────
//  Vecteurs de direction dérivés des angles d'Euler (yaw / pitch)
//  Convention : Z- = forward par défaut, Y = up
// ─────────────────────────────────────────────────────────────────────────────

struct Vec3 { float x, y, z; };

Vec3 GetForwardVector(float pitchDeg, float yawDeg)
{
  float p = glm::radians(pitchDeg);
  float y = glm::radians(yawDeg);
  return {
    (float)(-cosf(y) * cosf(p)),
    (float)( sinf(p)),
    (float)( sinf(y) * cosf(p))
};
}

Vec3 GetRightVector(float yawDeg)
{
  float y = glm::radians(yawDeg);
  return {
    -sinf(y),
     0.f,
     cosf(y)
};
}

Vec3 GetUpVector(float pitchDeg, float yawDeg)
{
    // up = right × forward
    Vec3 f = GetForwardVector(pitchDeg, yawDeg);
    Vec3 r = GetRightVector(yawDeg);
    return {
        r.y * f.z - r.z * f.y,
        r.z * f.x - r.x * f.z,
        r.x * f.y - r.y * f.x
    };
}

// ─────────────────────────────────────────────────────────────────────────────
//  Déplacement caméra orienté
// ─────────────────────────────────────────────────────────────────────────────

void ProcessCameraMovement(GLFWwindow* win)
{
    float velocity = cameraSpeed * (float)dt;

    Vec3 forward = GetForwardVector(pitch, yaw);
    Vec3 right   = GetRightVector(yaw);

    // W / S — avant / arrière
    if (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS)
    { camX += forward.x * velocity; camY += forward.y * velocity; camZ += forward.z * velocity; }
    if (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS)
    { camX -= forward.x * velocity; camY -= forward.y * velocity; camZ -= forward.z * velocity; }

    // A / D — gauche / droite (strafe)
    if (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS)
    { camX -= right.x * velocity; camZ -= right.z * velocity; }
    if (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS)
    { camX += right.x * velocity; camZ += right.z * velocity; }

    // Q / E — monter / descendre (world up, indépendant du pitch)
    if (glfwGetKey(win, GLFW_KEY_Q) == GLFW_PRESS) camY -= velocity;
    if (glfwGetKey(win, GLFW_KEY_E) == GLFW_PRESS) camY += velocity;
}

void ProcessCameraRotation(GLFWwindow* win)
{
  double mouseX, mouseY;
  glfwGetCursorPos(win, &mouseX, &mouseY);

  if (firstMouse)
  {
    lastMouseX = mouseX;
    lastMouseY = mouseY;
    firstMouse = false;
  }

  float offsetX = (float)(mouseX - lastMouseX) * mouseSensitivity;
  float offsetY = (float)(lastMouseY - mouseY) * mouseSensitivity; // Y inversé

  lastMouseX = mouseX;
  lastMouseY = mouseY;

  yaw += offsetX;   // au lieu de offsetX * 0.5f
  pitch += offsetY;

  // clamp pitch pour éviter de regarder trop haut/bas
  if (pitch > 89.0f) pitch = 89.0f;
  if (pitch < -89.0f) pitch = -89.0f;
}


void ErrorCallback(HRL_EError code, HRL_ESeverity severity, const char* detail)
{
  printf("Error of type : %s, Severity : %s, Details : %s\n", HRL_ErrorEnumToString(code), HRL_SeverityEnumToString(severity), detail);
  if (severity >= HRL_SEVERITY_FATAL)
  {
    exit(code);
  }
}


int main()
{
  //on init HRL avec l'api cible
  HRL_Init(HRL_OPENGL_33);

  //GLFW WINDOW//

  //(la gestion de glfw est mauvaise : il faudrait ajouter des logs en cas de crash, mais la ca ne nous interesse pas)
  //on init glfw
  glfwInit();

  //on crée la fenetre
  GLFWwindow* win = glfwCreateWindow(1280, 720, "HRL Example", nullptr, nullptr);

  //important! : le contexte doit etre actif avant HRL_InitContext
  glfwMakeContextCurrent(win);
  glfwSetFramebufferSizeCallback(win, framebuffer_size_callback);

  //cacher le curseur
  glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
  //verouiller la souris au centre
  glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

  //désactiver la v-sync
  glfwSwapInterval(0);


  HRL_RegisterErrorCallback(ErrorCallback);


  //on appelle initcontext avec le loader glfw (qui renvoie l'adresse opaque de la fenetre en gros)
  HRL_InitContext(1280, 720, (void*)glfwGetProcAddress);

  HRL_SetDebugLineThickness(3.f);


  //Create scene, camera & viewport
  HRL_id scene = HRL_CreateScene(true);
  HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
  HRL_SetCameraPerspectiveFov(camera, 90.f);
  HRL_id viewport = HRL_CreateViewport(scene, camera, 0.f, 0.f, 0.5f, 1.f);
  HRL_id viewport2 = HRL_CreateViewport(scene, camera, 0.5f, 0.f, 0.5f, 1.f);

  //Create Light
  HRL_id light;
  {
    light = HRL_CreateLight(scene, HRL_POINT_LIGHT);
    HRL_SetLightAttenuation(light, 0.02f);
    HRL_SetLightIntensity(light, 2.f);
    HRL_SetLightColor(light, 1.f, 1.f, 1.f);
    HRL_SetLightLocation(light, 0.f, 0.f, 12.f);
    HRL_SetLightRotation(light, 0,0,0);
    HRL_SetSpotLightOuterCutoff(light, 33.f);
  }

  //Create scene objects

  //Atlas sprite
  {
    size_t atlas_size;
    std::string atlas_data = example::OpenFile("atlas.png", &atlas_size);
    HRL_id atlas_texture = HRL_CreateTexture(atlas_data.c_str(), atlas_size);
    HRL_id atlas_material = HRL_CreateMaterial(HRL_SPRITE_SHADER);
    HRL_MaterialSetTexture(atlas_material, HRL_T_ALBEDO, atlas_texture);
    HRL_id atlas_mesh = HRL_CreateMeshSprite(scene);
    HRL_SetMeshMaterial(atlas_mesh, atlas_material);
  }

  //Text
  HRL_id text_texture;
  HRL_id jetbrains_font;
  {
    size_t font_size;
    std::string font_data = example::OpenFile("JetBrainsMono.ttf", &font_size);
    jetbrains_font = HRL_CreateFont(font_data.c_str(), font_size);
    text_texture = HRL_CreateTextureFromText("Hello world!", jetbrains_font, 55, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f);
    HRL_id text_material = HRL_CreateMaterial(HRL_SPRITE_SHADER);
    HRL_MaterialSetTexture(text_material, HRL_T_ALBEDO, text_texture);
    HRL_id text_sprite = HRL_CreateMeshSprite(scene);
    HRL_SetMeshMaterial(text_sprite, text_material);
    int w;
    int h;
    HRL_GetTextureSize(text_texture, &w, &h);
    HRL_SetMeshScale(text_sprite, w/20, h/20, 1.f);
  }

  //Post Process
  {
    HRL_id pp_material = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
    HRL_MaterialSetFloat(pp_material, "saturation", 1.5f);
    HRL_MaterialSetFloat(pp_material, "bloomStrength", 1.f);
    HRL_id pp = HRL_CreatePostProcess(viewport, pp_material, 1);
  }

  //Photorealistic texture
  {
    size_t al_size;
    std::string al_data = example::OpenFile("rock/albedo.jpg", &al_size);
    HRL_id rock_texture = HRL_CreateTexture(al_data.c_str(), al_size);
    HRL_SetTextureMinFilter(rock_texture, HRL_FILTER_TRILINEAR);
    HRL_SetTextureMagFilter(rock_texture, HRL_FILTER_TRILINEAR);

    size_t normal_size;
    std::string normal_data = example::OpenFile("rock/normal.jpg", &normal_size);
    HRL_id normal_texture = HRL_CreateTexture(normal_data.c_str(), normal_size);
    HRL_SetTextureMinFilter(normal_texture, HRL_FILTER_TRILINEAR);
    HRL_SetTextureMagFilter(normal_texture, HRL_FILTER_TRILINEAR);

    size_t roughness_size;
    std::string roughness_data = example::OpenFile("rock/roughness.jpg", &roughness_size);
    HRL_id roughness_texture = HRL_CreateTexture(roughness_data.c_str(), roughness_size);
    HRL_SetTextureMinFilter(roughness_texture, HRL_FILTER_TRILINEAR);
    HRL_SetTextureMagFilter(roughness_texture, HRL_FILTER_TRILINEAR);

    HRL_id rock_material = HRL_CreateMaterial(HRL_SPRITE_SHADER);
    HRL_MaterialSetTexture(rock_material, HRL_T_ALBEDO, rock_texture);
    HRL_MaterialSetTexture(rock_material, HRL_T_NORMAL, normal_texture);
    HRL_MaterialSetTexture(rock_material, HRL_T_ROUGHNESS, roughness_texture);
    HRL_MaterialSetTexture(rock_material, HRL_T_SPECULAR, roughness_texture);


    HRL_id rock_mesh = HRL_CreateMeshSprite(scene);
    HRL_SetMeshMaterial(rock_mesh, rock_material);
    HRL_SetMeshScale(rock_mesh, 20.f, 10.f, 1.f);
    HRL_SetMeshLocation(rock_mesh, 0.f, 0.f, 1.f);
  }

  //tree texture
  {/**
    size_t al_size;
    std::string al_data = example::OpenFile("tree/albedo.jpg", &al_size);
    HRL_id al_texture = HRL_CreateTexture(al_data.c_str(), al_size);
    HRL_SetTextureMinFilter(al_texture, HRL_FILTER_TRILINEAR);
    HRL_SetTextureMagFilter(al_texture, HRL_FILTER_TRILINEAR);

    size_t normal_size;
    std::string normal_data = example::OpenFile("tree/normal.jpg", &normal_size);
    HRL_id normal_texture = HRL_CreateTexture(normal_data.c_str(), normal_size);
    HRL_SetTextureMinFilter(normal_texture, HRL_FILTER_TRILINEAR);
    HRL_SetTextureMagFilter(normal_texture, HRL_FILTER_TRILINEAR);

    size_t roughness_size;
    std::string roughness_data = example::OpenFile("tree/roughness.jpg", &roughness_size);
    HRL_id roughness_texture = HRL_CreateTexture(roughness_data.c_str(), roughness_size);
    HRL_SetTextureMinFilter(roughness_texture, HRL_FILTER_TRILINEAR);
    HRL_SetTextureMagFilter(roughness_texture, HRL_FILTER_TRILINEAR);

    size_t metalic_size;
    std::string metalic_data = example::OpenFile("tree/metalic.jpg", &metalic_size);
    HRL_id metalic_texture = HRL_CreateTexture(metalic_data.c_str(), metalic_size);
    HRL_SetTextureMinFilter(metalic_texture, HRL_FILTER_TRILINEAR);
    HRL_SetTextureMagFilter(metalic_texture, HRL_FILTER_TRILINEAR);

    HRL_id tree_material = HRL_CreateMaterial(HRL_SPRITE_SHADER);
    HRL_MaterialSetTexture(tree_material, HRL_T_ALBEDO, al_texture);
    HRL_MaterialSetTexture(tree_material, HRL_T_NORMAL, normal_texture);
    HRL_MaterialSetTexture(tree_material, HRL_T_ROUGHNESS, roughness_texture);
    //HRL_MaterialSetTexture(tree_material, HRL_T_Metallic, metalic_texture);


    HRL_id tree_mesh = HRL_CreateMeshSprite(scene);
    HRL_SetMeshMaterial(tree_mesh, tree_material);
    HRL_SetMeshScale(tree_mesh, 20.f, 10.f, 1.f);
    HRL_SetMeshLocation(tree_mesh, 10.f, 0.f, 0.f);
  */}

  //Widgets
  HRL_id btn;
  {
    btn = HRL_CreateWidget(viewport, HRL_WIDGET_BUTTON);
    HRL_SetWidgetPosition(btn, 0.5f, 0.5f);
    HRL_SetWidgetSize(btn, 0.2f, 0.2f);
    size_t btn_size;
    std::string btn_data = example::OpenFile("button.png", &btn_size);
    HRL_id btn_texture = HRL_CreateTexture(btn_data.c_str(), btn_size);
    HRL_SetButtonBackgroundTexture(btn, HRL_WIDGET_STATE_IDLE, btn_texture);
    int w, h;
    HRL_GetTextureSize(btn_texture, &w, &h);
    HRL_SetWidgetSize(btn, (float)w/800.f, (float)h/800.f);
    HRL_SetButtonBackgroundTintColor(btn, HRL_WIDGET_STATE_IDLE, 1.f, 1.f, 1.f, 1.f);
    HRL_SetButtonTextFont(btn, jetbrains_font);
    HRL_SetButtonText(btn, "bonjour!");
    HRL_SetButtonTextTintColor(btn, HRL_WIDGET_STATE_IDLE, 0.1f, 0.1f, 0.1f, 1.f);
  }

  //Enable fog
  HRL_SetFogEnabled(scene, true);
  HRL_SetFogMode(scene, HRL_FOG_EXP_SQUARED);
  HRL_SetFogColor(scene, 0.02f, 0.02f, 0.02f);



  while (!glfwWindowShouldClose(win))
  {
    if (glfwGetKey(win, GLFW_KEY_F6) == GLFW_PRESS)
    {
    HRL_SetButtonText(btn, "comment va tu?*ù^$&é(-è)²~[|{@^");
    }
    CalculateDeltaTime();

    HRL_EndFrame();

    //update classique glfw
    glfwSwapBuffers(win);
    glfwPollEvents();

    glfwSetWindowTitle(win, std::to_string(1/dt).c_str());

    //printf("FPS : %f\n", 1/dt);

    //movement de la camera
    ProcessCameraMovement(win);
    ProcessCameraRotation(win);
    HRL_SetCameraLocation(camera, camX, camY, camZ);
    HRL_SetCameraRotation(camera, pitch, yaw, 0.f);
    HRL_SetLightLocation(light, camX, camY, camZ);
    HRL_SetLightRotation(light, pitch, yaw, 0.f);

    //debug keys
    if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS)
      glfwSetWindowShouldClose(win, true);

    if (glfwGetKey(win, GLFW_KEY_F1) == GLFW_PRESS)
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    if (glfwGetKey(win, GLFW_KEY_F2) == GLFW_PRESS)
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    if (glfwGetKey(win, GLFW_KEY_F3) == GLFW_PRESS)
      //activer l'anti-aliasing, 8x MSAA
      glfwWindowHint(GLFW_SAMPLES, 8);

    if (glfwGetKey(win, GLFW_KEY_F4) == GLFW_PRESS)
    {
      float proj[16];
      HRL_GetProjectionMatrix(proj);
      for (int col = 0; col < 4; col++)
        printf("%f %f %f %f\n", proj[col*4+0], proj[col*4+1], proj[col*4+2], proj[col*4+3]);
    }

    if (glfwGetKey(win, GLFW_KEY_F5) == GLFW_PRESS)
    {
      HRL_DrawDebugCircle(scene, HRL_DEBUG_SOLID, 0.f,0.f,0.f, 30.f, 16, 1.f, 0.f,1.f);
      HRL_DrawDebugSegment(
      scene,
        0.0f, 0.0f, 0.0f,
        10.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f
      );
    }
  }

  //on libere les ressources HRL
  HRL_Shutdown();
}