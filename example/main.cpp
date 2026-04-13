//HRL single header
#include <hrl/hrl.h>
#include <hrl/hrl_gl.h>

//Window
#include <iosfwd>
#include <glfw/glfw3.h>
//Files
#include <fstream>

//Print
#include <iostream>

#include "src/example.h"


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

void ProcessCameraMovement(GLFWwindow* win)
{
  auto deltaTime = (float)dt;
  float velocity = cameraSpeed * deltaTime;

  if (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS)
    camZ -= velocity;
  if (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS)
    camZ += velocity;
  if (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS)
    camX -= velocity;
  if (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS)
    camX += velocity;
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

  yaw   += offsetX*0.5f;
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
  HRL_id scene = HRL_CreateScene(false);
  HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
  HRL_SetCameraPerspectiveFov(camera, 90.f);
  HRL_id viewport = HRL_CreateViewport(scene, camera, 0.f, 0.f, 0.5f, 1.f);
  HRL_id viewport2 = HRL_CreateViewport(scene, camera, 0.5f, 0.f, 0.5f, 1.f);

  //Create Light
  HRL_id light;
  {
    light = HRL_CreateLight(scene, HRL_POINT_LIGHT);
    HRL_SetLightAttenuation(light, 0.02f);
    HRL_SetLightLocation(light, 0.f, 0.f, 12.f);
    HRL_SetLightIntensity(light, 2.f);
    HRL_SetLightColor(light, 1.f, 1.f, 1.f);
    HRL_SetLightRotation(light, 0.f, 0.f, 0.f);
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
  {
    size_t font_size;
    std::string font_data = example::OpenFile("JetBrainsMono.ttf", &font_size);
    HRL_id jetbrains_font = HRL_CreateFont(font_data.c_str(), font_size);
    HRL_id text_texture = HRL_CreateTextureFromText("Hello world!", jetbrains_font, 55, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f);
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
    //HRL_id pp = HRL_CreatePostProcess(viewport, pp_material, 1);
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

  //Enable fog
  HRL_SetFogEnabled(scene, false);
  HRL_SetFogMode(scene, HRL_FOG_EXPONENTIAL);
  HRL_SetFogColor(scene, 0.1f, 0.1f, 0.1f);



  while (!glfwWindowShouldClose(win))
  {
    if (glfwGetKey(win, GLFW_KEY_F5) == GLFW_PRESS)
    {
      HRL_DrawDebugCircle(scene, HRL_DEBUG_SOLID, 0.f,0.f,0.f, 30.f, 16, 1.f, 0.f,1.f);
    }
    CalculateDeltaTime();

    HRL_EndFrame();

    //update classique glfw
    glfwSwapBuffers(win);
    glfwPollEvents();

    printf("FPS : %f\n", 1/dt);

    //movement de la camera
    ProcessCameraMovement(win);
    ProcessCameraRotation(win);
    HRL_SetCameraLocation(camera, camX, camY, camZ);
    HRL_SetCameraRotation(camera, pitch, yaw, 0.f);
    HRL_SetLightLocation(light, camX, camY, camZ);

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
  }

  //on libere les ressources HRL
  HRL_Shutdown();
}