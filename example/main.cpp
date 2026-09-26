// HRL single header
#include <hrl/hrl.h>
#include <hrl/hrl_gl.h>

// Window
#include <iosfwd>
#include <glfw/glfw3.h>
#include <fstream>

#include <iostream>
#include <string>

#include "src/example.h"

#include <glm/glm.hpp>


typedef struct {
  float x, y, z;
} vec3;


// frame time
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


// GLFW window resize callback
void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
  (void)window;
  HRL_WindowResizeCallback(width, height);
  printf("%d/%d\n", width, height);
}


// variables de déplacement de la camera
float cameraSpeed = 20.0f;   // unités par seconde
float mouseSensitivity = 0.1f;

float yaw = -90.0f;   // rotation autour de Y — regarde vers -Z au démarrage
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

  yaw += offsetX;
  pitch += offsetY;

  // clamp pitch pour éviter de regarder trop haut/bas
  if (pitch > 89.0f) pitch = 89.0f;
  if (pitch < -89.0f) pitch = -89.0f;
}


void ErrorCallback(HRL_EError code, HRL_ESeverity severity, const char* detail)
{
  printf("Error of type : %s, Severity : %s, Details : %s\n",
         HRL_ErrorEnumToString(code),
         HRL_SeverityEnumToString(severity),
         detail);
  if (severity >= HRL_SEVERITY_FATAL)
  {
    exit(code);
  }
}


int main()
{
  // on init HRL avec l'api cible
  HRL_Init(HRL_OPENGL_33);

  // GLFW WINDOW //

  // on init glfw
  glfwInit();

  // on crée la fenetre
  GLFWwindow* win = glfwCreateWindow(1280, 720, "HRL 3D Example", nullptr, nullptr);

  // important! : le contexte doit etre actif avant HRL_InitContext
  glfwMakeContextCurrent(win);
  glfwSetFramebufferSizeCallback(win, framebuffer_size_callback);

  // cacher le curseur
  // glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
  // verouiller la souris au centre
  // glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

  // désactiver la v-sync
  glfwSwapInterval(0);

  HRL_RegisterErrorCallback(ErrorCallback);

  // on appelle initcontext avec le loader glfw
  HRL_InitContext(1280, 720, (void*)glfwGetProcAddress);

  HRL_SetDebugLineThickness(3.f);

  size_t sky_size;
  std::string sky_data = example::OpenFile("skydome.jpg", &sky_size);
  HRL_id sky_tex = HRL_CreateTexture(sky_data.c_str(), sky_size);

  // Create scene, camera & viewport
  HRL_id scene = HRL_CreateScene(true);
  HRL_SetSkySphereEnabled(scene, HRL_TRUE);
  HRL_SetSkySphereColors(
    scene,
    0.05f, 0.15f, 0.50f,
    0.55f, 0.75f, 1.00f,
    0.03f, 0.04f, 0.08f
  );
  HRL_SetSkySphereTexture(scene, sky_tex);


  HRL_SetEnvironmentMap(scene, sky_tex);
  HRL_SetEnvironmentMappingEnabled(scene, HRL_TRUE);


  HRL_SetAntialiasingMode(HRL_ANTIALIASING_8X);


  HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
  HRL_SetCameraPerspectiveFov(camera, 90.f);
  HRL_id viewport = HRL_CreateViewport(scene, camera, 0.f, 0.f, 1.f, 1.f);
  //HRL_id viewport2 = HRL_CreateViewport(scene, camera, 0.5f, 0.f, 0.5f, 1.f);

  // Initialiser explicitement la caméra avant le premier rendu.
  HRL_SetCameraLocation(camera, camX, camY, camZ);
  HRL_SetCameraRotation(camera, pitch, yaw, 0.f);

  // ───────────────────────────────────────────────────────────────────────────
  //  Chargement FBX : conversion vers HRL_Vertex3D puis création d'un mesh HRL
  //  Placez "model.fbx" à côté de l'exécutable de l'exemple.
  // ───────────────────────────────────────────────────────────────────────────

  size_t vertexCount = 0;
  size_t model_size;
  std::string model_data = example::OpenFile("model.fbx", &model_size);
  HRL_Vertex3D* vertices = HRL_GetVertex3DFromFBX(model_data.c_str(), model_size, &vertexCount);

  if (!vertices)
  {
    printf("Impossible de charger model.fbx\n");
    HRL_Shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 1;
  }

  HRL_id modelMaterial = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
  //HRL_MaterialSetVec3(modelMaterial, "TintColor", 0.8f, 0.8f, 0.8f);

  HRL_id model = HRL_CreateMesh3D(
    scene,
    vertices,
    vertexCount,
    nullptr,
    0
  );

  HRL_FreeVertex3DFromFBX(vertices);

  if (model == HRL_INVALID_ID)
  {
    HRL_Shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 1;
  }

  HRL_SetMeshMaterial(model, modelMaterial);
  HRL_SetMeshLocation(model, 0.f, 0.f, 0.f);
  HRL_SetMeshScale(model, 5.f, 5.f, 5.f);

  HRL_MaterialSetFloat(
      modelMaterial,
      "EnvironmentStrength",
      1.f
  );


  HRL_SetMeshLODLevels(model, 5);
  HRL_SetMeshLODMode(model, HRL_LOD_DISTANCE);
  HRL_SetMeshLODScreenThreshold(model, 0.22f);
  HRL_SetMeshLODScreenScale(model, 0.5f);
  HRL_SetMeshLODHysteresis(model, 0.08f);
  HRL_SetMeshLODAutomatic(model, HRL_TRUE);
  HRL_SetMeshLODMaxDistance(model, 2.f);



  // Lumière ponctuelle pour rendre le relief / les normales visibles.
  HRL_id light = HRL_CreateLight(scene, HRL_POINT_LIGHT);
  HRL_SetLightAttenuation(light, 0.02f);
  HRL_SetLightIntensity(light, 8.f);
  HRL_SetLightColor(light, 1.f, 0.95f, 0.85f);
  HRL_SetLightLocation(light, 3.f, 3.f, 4.f);
  HRL_SetLightRotation(light, 0.f, 0.f, 0.f);
  HRL_SetLightCastShadows(light, HRL_TRUE);
  HRL_SetLightShadowResolution(light, 1024);
  HRL_SetLightShadowBias(light, 0.0015f);


  while (!glfwWindowShouldClose(win))
  {
    if (glfwGetKey(win, GLFW_KEY_F6) == GLFW_PRESS)
    {
      HRL_id cam = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
      HRL_SetViewportCamera(viewport, cam);
      HRL_SetCameraPerspectiveFov(cam, 60.f);
    }

    CalculateDeltaTime();

    // Rotation lente du mesh FBX pour vérifier les normales et le depth test.
    static float modelYaw = 0.f;
    modelYaw += 20.f * (float)dt;
    HRL_SetMeshRotation(model, 0.f, modelYaw, 0.f);

    HRL_EndFrame();

    // update classique glfw
    glfwSwapBuffers(win);
    glfwPollEvents();

    // FPS dans le titre de la fenêtre
    if (dt > 0.0)
      glfwSetWindowTitle(win, std::to_string(1.0 / dt).c_str());

    // movement de la camera
    ProcessCameraMovement(win);
    ProcessCameraRotation(win);
    HRL_SetCameraLocation(camera, camX, camY, camZ);
    HRL_SetCameraRotation(camera, pitch, yaw, 0.f);

    // debug views
    if (glfwGetKey(win, GLFW_KEY_F6) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(scene, HRL_DEBUG_VIEW_NONE);
    if (glfwGetKey(win, GLFW_KEY_F7) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(scene, HRL_DEBUG_VIEW_UNLIT);
    if (glfwGetKey(win, GLFW_KEY_F8) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(scene, HRL_DEBUG_VIEW_WIREFRAME);
    if (glfwGetKey(win, GLFW_KEY_F9) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(scene, HRL_DEBUG_VIEW_NORMAL);
    if (glfwGetKey(win, GLFW_KEY_F10) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(scene, HRL_DEBUG_VIEW_LIGHTING);
    if (glfwGetKey(win, GLFW_KEY_F11) == GLFW_PRESS)
      HRL_DrawSceneAsDebugMode(scene, HRL_DEBUG_VIEW_LOD);

    // debug keys
    if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS)
      glfwSetWindowShouldClose(win, true);

    if (glfwGetKey(win, GLFW_KEY_F1) == GLFW_PRESS)
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    if (glfwGetKey(win, GLFW_KEY_F2) == GLFW_PRESS)
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    if (glfwGetKey(win, GLFW_KEY_F3) == GLFW_PRESS)
      // activer l'anti-aliasing, 8x MSAA
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
      HRL_DrawDebugCircle(scene, HRL_DEBUG_SOLID, 0.f,20.f,0.f, 30.f, 16, 1.f, 0.f,1.f);
      HRL_DrawDebugSegment(
        scene,
        0.0f, 0.0f, 0.0f,
        10.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f
      );
    }


    std::cout << HRL_GetMeshLODLevel(model) << std::endl;
  }

  // on libere les ressources HRL
  HRL_Shutdown();
}
