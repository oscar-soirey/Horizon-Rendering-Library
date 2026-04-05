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


std::string OpenFile(const char* _path, size_t* _size)
{
  FILE* f = fopen(_path, "rb");
  if (!f)
  {
    std::cout << "Erreur de lecture du fichier" << std::endl;
    return "";
  }

#ifdef WIN32
  //windows
  _fseeki64(f, 0, SEEK_END);
  __int64 size = _ftelli64(f);
#else
  //compaptibilité
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
#endif

  if (size <= 0 || size > INT_MAX)
  {
    fclose(f);
    std::cout << "Erreur de taille (<= 0 ou superieur a INT_MAX)" << std::endl;
    return "";
  }
#ifdef WIN32
  _fseeki64(f, 0, SEEK_SET);
#else
  fseek(f, 0, SEEK_SET);
#endif

  std::string content((size_t)size, '\0');   // alloue buffer
  if (fread(&content[0], 1, (size_t)size, f) != (size_t)size)
  {
    fclose(f);
    std::cout << "Erreur de lecture complète" << std::endl;
    return "";
  }

  fclose(f);
  //passer la size
  if (_size)
  {
    *_size = (size_t)size;
  }
  return content;
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

  yaw   += offsetX;
  pitch += offsetY;

  // clamp pitch pour éviter de regarder trop haut/bas
  if (pitch > 89.0f) pitch = 89.0f;
  if (pitch < -89.0f) pitch = -89.0f;
}


void DrawDebugExamples(HRL_id sceneId)
{
  // cercle creux
  HRL_DrawDebugCircle(sceneId, HRL_DebugHollow,
    0.f, 0.f, 0.f,
    1.f, 16,
    1.f, 0.f, 0.f); // rouge

  // cercle plein
  HRL_DrawDebugCircle(sceneId, HRL_DebugSolid,
    3.f, 0.f, 0.f,
    0.5f, 16,
    0.f, 1.f, 0.f); // vert

  // carré creux
  {
    float vx[] = { -0.5f,  0.5f, 0.5f, -0.5f };
    float vy[] = { -0.5f, -0.5f, 0.5f,  0.5f };
    float vz[] = {  0.f,   0.f,  0.f,   0.f  };
    HRL_DrawDebugPolygon(sceneId, HRL_DebugHollow, vx, vy, vz, 4, 0.f, 0.f, 1.f); // bleu
  }

  // carré plein
  {
    float vx[] = {  2.f,  3.f, 3.f,  2.f };
    float vy[] = { -0.5f, -0.5f, 0.5f, 0.5f };
    float vz[] = {  0.f,  0.f,  0.f,  0.f };
    HRL_DrawDebugPolygon(sceneId, HRL_DebugSolid, vx, vy, vz, 4, 1.f, 1.f, 0.f); // jaune
  }

  // capsule
  HRL_DrawDebugCapsule(sceneId, HRL_DebugHollow,
    -2.f, -1.f, 0.f,   // A
    -2.f,  1.f, 0.f,   // B
    0.4f, 16,
    1.f, 0.5f, 0.f);   // orange

  // segment
  HRL_DrawDebugSegment(sceneId,
    -3.f, -1.f, 0.f,
     3.f,  1.f, 0.f,
    1.f, 1.f, 1.f);    // blanc

  // points
  HRL_DrawDebugPoint(sceneId,  0.f,  2.f, 0.f, 0.1f, 1.f, 0.f, 1.f); // magenta
  HRL_DrawDebugPoint(sceneId,  1.f,  2.f, 0.f, 0.1f, 0.f, 1.f, 1.f); // cyan
  HRL_DrawDebugPoint(sceneId, -1.f,  2.f, 0.f, 0.1f, 1.f, 1.f, 0.f); // jaune
}


int main()
{
  //on init HRL avec l'api cible
  HRL_Init(HRL_OpenGL33);

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


  //on appelle initcontext avec le loader glfw (qui renvoie l'adresse opaque de la fenetre en gros)
  HRL_InitContext(1280, 720, (void*)glfwGetProcAddress);

  HRL_SetDebugLineThickness(3.f);

  HRL_id scene = HRL_CreateScene(1);

  //on ouvre la texture
  size_t texSize;
  std::string texString = OpenFile("canada.jpg", &texSize);
  size_t normalSize;
  std::string normalString = OpenFile("normal.jpg", &normalSize);
  //crée la texture et le material
  HRL_id tex = HRL_CreateTexture(texString.c_str(), texSize);
  HRL_id normaltex = HRL_CreateTexture(normalString.c_str(), normalSize);
  //material
  HRL_id mat = HRL_CreateMaterial(HRL_SpriteShader);
  HRL_MaterialSetTexture(mat, HRL_T_Albedo, tex);
  HRL_MaterialSetTexture(mat, HRL_T_Normal, normaltex);
  HRL_MaterialSetFloat(mat, "NormalStrength", 1);

  //mesh 1 : canada flag
  HRL_id sprite = HRL_CreateMeshSprite(scene);
  HRL_SetMeshMaterial(sprite, mat);
  HRL_SetMeshScale(sprite, 10, 10, 10);


  //Mesh 2 : portugal flag
  float ptZRot = 0.f; //pour faire tourner le sprite

  size_t ptSize;
  std::string ptString = OpenFile("portugal.jpg", &ptSize);
  HRL_id ptTex = HRL_CreateTexture(ptString.c_str(), ptSize);
  HRL_id ptMat = HRL_CreateMaterial(HRL_SpriteShader);
  HRL_MaterialSetTexture(ptMat, HRL_T_Albedo, ptTex);
  HRL_id sprite2 = HRL_CreateMeshSprite(scene);
  HRL_SetMeshMaterial(sprite2, ptMat);
  HRL_SetMeshScale(sprite2, 80, 80, 80);


  //camera 0 (default)
  HRL_SetCameraType(0, HRL_Perspective);
  HRL_SetCameraPerspectiveFov(0, 40.f);
  HRL_SetCameraFarPlane(0, 10000.f);

  //other camera and viewport
  HRL_id cam1 = HRL_CreateCamera(scene, HRL_Perspective);
  HRL_SetCameraPerspectiveFov(cam1, 90.f);
  HRL_id viewport = HRL_CreateViewport(scene, HRL_InvalidID, 0.f, 0.f, 1.f, 1.f);
  HRL_SetViewportCamera(viewport, cam1);


  //Lights
  vec3 lightpos(0.f, 0.f, 0.f);
  HRL_id light0 = HRL_CreateLight(scene, HRL_PointLight);
  HRL_SetLightAttenuation(light0, 0.02f);
  HRL_SetLightIntensity(light0, 5.f);
  HRL_SetLightColor(light0, 1.f, 1.f, 1.f);
  HRL_SetLightRotation(light0, 0.f, 0.f, 0.f);


  size_t liAlbSize;
  std::string AlbString = OpenFile("point_light.png", &liAlbSize);
  HRL_id liAlb = HRL_CreateTexture(AlbString.c_str(), liAlbSize);

  HRL_id liMat = HRL_CreateMaterial(HRL_SpriteShader);
  HRL_MaterialSetTexture(liMat, HRL_T_Albedo, liAlb);

  HRL_id sprite_light = HRL_CreateMeshSprite(scene);
  HRL_SetMeshMaterial(sprite_light, liMat);
  HRL_SetMeshScale(sprite_light, 2, 2, 2);
  HRL_SetMeshLocation(sprite_light, 10, 10, 10);
  HRL_SetSpriteDrawOrder(sprite_light, 50.f);


  size_t fontSize;
  std::string font = OpenFile("simsunb.ttf", &fontSize);
  HRL_id font_id = HRL_CreateFont(font.c_str(), fontSize);
  HRL_id text_texture = HRL_CreateTextureFromText("Hello", font_id, 100.f, 0.f,
    1.f, 1.f, 1.f,
    0.f, 0.f, 0.f, 0.f
  );
  HRL_id text_mat = HRL_CreateMaterial(HRL_SpriteShader);
  HRL_MaterialSetTexture(text_mat, HRL_T_Albedo, text_texture);

  HRL_id text_sprite = HRL_CreateMeshSprite(scene);
  HRL_SetMeshMaterial(text_sprite, text_mat);
  HRL_SetMeshLocation(text_sprite, 1, 1, 1);


  int w;
  int h;
  HRL_GetTextureSize(text_texture, &w, &h);
  HRL_SetMeshScale(text_sprite, w/40, h/40, 1.f);



  //boucle principale
  while (!glfwWindowShouldClose(win))
  {
    //efface la frame précedente
    HRL_BeginFrame();
    //dessine les objets à l'ecran
    HRL_EndFrame();

    //update classique glfw
    glfwSwapBuffers(win);
    glfwPollEvents();

    //calculer le deltatime
    CalculateDeltaTime();

    //printf("FPS : %f\n", 1/dt);

    //movement de la camera
    ProcessCameraMovement(win);
    ProcessCameraRotation(win);

    HRL_SetCameraPosition(cam1, camX, camY, camZ);
    HRL_SetCameraRotation(cam1, pitch, yaw, 0.f);

    lightpos.z += (float)dt;
    HRL_SetLightLocation(light0, lightpos.x, lightpos.y, lightpos.z + 10);
    HRL_SetMeshLocation(sprite_light, lightpos.x, lightpos.y, lightpos.z);

    //ptZRot += 1.f * (float)dt;
    //HRL_SetMeshRotation(sprite2, 0.f, 0.f, ptZRot);

    //debug keys
    if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS)
      glfwSetWindowShouldClose(win, true);
    if (glfwGetKey(win, GLFW_KEY_F1) == GLFW_PRESS)
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    if (glfwGetKey(win, GLFW_KEY_F2) == GLFW_PRESS)
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  }

  //on libere les ressources HRL
  HRL_Shutdown();
}